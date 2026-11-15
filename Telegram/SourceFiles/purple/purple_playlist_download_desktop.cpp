#include "purple/purple_playlist_download_desktop.h"

#include "purple/purple_config.h"
#include "purple/purple_playlist_download_files.h"
#include "purple/purple_playlist_download_registry.h"
#include "purple/purple_playlist_owned_transfer.h"
#include "purple/purple_playlist_managed_cache.h"
#include "purple/purple_playlist_streaming_source.h"
#include "purple/purple_playlists_resolver.h"
#include "purple/purple_playlists_service.h"
#include "purple/purple_playlists_policy.h"
#include "purple/purple_user_control.h"
#include "core/file_location.h"
#include "crl/crl_on_main.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QThread>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace Purple {
namespace {

PeerId SourcePeer(const PlaylistSourceRef &source, uint64_t self) {
	switch (source.peer.kind) {
	case PeerTokenKind::Self: return peerFromUser(UserId(self));
	case PeerTokenKind::User: return peerFromUser(UserId(source.peer.id));
	case PeerTokenKind::Chat: return peerFromChat(ChatId(source.peer.id));
	case PeerTokenKind::Channel: return peerFromChannel(ChannelId(source.peer.id));
	}
	return PeerId();
}

DocumentData *Revalidate(
		not_null<Main::Session*> session,
		const PlaylistSongResolution &resolution,
		HistoryItem *&item) {
	const auto id = FullMsgId(SourcePeer(resolution.source,
		SessionAccountKey(session).userId), MsgId(resolution.source.msg));
	item = session->data().message(id);
	const auto media = item ? item->media() : nullptr;
	const auto document = media ? media->document() : nullptr;
	if (!item || item->isEmpty() || item->fullId() != id || !document
		|| document->id != resolution.documentId || document->size <= 0
		|| item->id.bare <= 0 || item->id.bare > kMaximumPlaylistMessageId
		|| &item->history()->session() != session.get()
		|| &document->session() != session.get()
		|| !item->computeUnavailableReason().isEmpty()) return nullptr;
	const auto candidate = PlaylistSongCandidate{
		.documentId = document->id,
		.messageId = int32_t(item->id.bare),
		.hasAudioAttribute = document->isSong(),
		.isVoiceMessage = document->isVoiceMessage(),
		.isRegularServerMessage = item->isRegular(),
		.isMainMessageMedia = dynamic_cast<Data::MediaFile*>(media) != nullptr,
		.isScheduled = item->isScheduled(),
		.isLocalUnsent = item->isLocal() || item->isSending(),
		.isSponsored = item->isSponsored(),
		.hasSelfDestructTimer = bool(media->ttlSeconds()) || bool(item->ttlDestroyAt()),
		.isProtectedContent = !item->allowsForward(),
	};
	return QualifySong(candidate, SaveRestrictionsDisabled()) ? document : nullptr;
}

} // namespace

class DesktopPlaylistDownloadJob::State final
	: public std::enable_shared_from_this<DesktopPlaylistDownloadJob::State> {
public:
	State(not_null<Main::Session*> session, QString id,
		Fn<void(PlaylistDownloadProgress)> changed)
	: session(base::make_weak(session.get()))
	, account(SessionAccountKey(session))
	, route(CaptureAccountSyncRouting(session))
	, playlistId(std::move(id))
	, changed(std::move(changed)) {
		if (route) {
			const auto files = ResolveAccountFiles(account, route->owner, AccountFileLayout::Desktop);
			if (files) directory = route->root + u'/' + files->playlists;
		}
	}

	bool current() const {
		Expects(QThread::currentThread() == QCoreApplication::instance()->thread());
		const auto value = session.get();
		return !cancelled && value && route && !directory.isEmpty()
			&& !value->account().loggingOut() && value->account().maybeSession() == value
			&& SessionAccountKey(value) == account && AccountSyncRoutingCurrent(value, *route);
	}

	void start() {
		if (started) return;
		started = true;
		if (!current()) {
			cancel(true, PlaylistDownloadProblem::Changed);
			return;
		}
		const auto weak = weak_from_this();
		RequestAccountPlaylistDownloads(session.get(), {}, [weak](PlaylistAccountResult result) {
			const auto self = weak.lock();
			if (!self) return;
			if (!self->current()) {
				self->cancel(true, PlaylistDownloadProblem::Changed);
				return;
			}
			if (!result || !result.downloads || !*result.downloads) {
				self->cancel(true, PlaylistDownloadProblem::Storage);
				return;
			}
			const auto &library = *result.snapshot.library;
			const auto i = library.playlists.find(self->playlistId);
			if (i == end(library.playlists) || i->second.isPartial()
				|| i->second.deleted->value || library.purged.contains(self->playlistId)) {
				self->cancel(true, PlaylistDownloadProblem::MissingPlaylist);
				return;
			}
			for (const auto id : PlaylistVisibleSongIds(i->second)) {
				self->songs.push_back({ id, *i->second.songs->at(id).value.source });
			}
			self->policy = PlaylistDownloadJobPolicy(self->songs.size());
			self->pump();
		});
	}

	void notify(bool retiring = false) {
		if ((!retiring && !current()) || !changed) return;
		auto value = policy.progress();
		value.problem = problem;
		if (retiring) {
			value.terminal = true;
			value.cancelled = problem == PlaylistDownloadProblem::Cancelled
				|| problem == PlaylistDownloadProblem::Changed;
		}
		changed(value);
	}

	void cancel(bool publish = true,
			PlaylistDownloadProblem reason = PlaylistDownloadProblem::Cancelled) {
		Expects(QThread::currentThread() == QCoreApplication::instance()->thread());
		if (cancelled) return;
		cancelled = true;
		problem = reason;
		authority->store(false, std::memory_order_release);
		for (auto &[index, token] : transferAuthority) token->store(false, std::memory_order_release);
		for (const auto &[index, transfer] : transfers) transfer->cancel();
		for (const auto &[index, transfer] : cacheTransfers) transfer->cancel();
		transfers.clear();
		cacheTransfers.clear();
		policy.cancel();
		resolvers.clear();
		if (publish) notify(true);
	}

	void retry() {
		if (current() && started && policy.retry()) {
			problem = PlaylistDownloadProblem::None;
			pump();
		}
	}

	void finish(PlaylistDownloadTicket ticket, PlaylistDownloadPhase phase,
			PlaylistDownloadProblem reason = PlaylistDownloadProblem::None) {
		if (!current() || !policy.finish(ticket, phase)) return;
		if (reason != PlaylistDownloadProblem::None) problem = reason;
		else if (phase == PlaylistDownloadPhase::Failed) problem = PlaylistDownloadProblem::Storage;
		else if (phase == PlaylistDownloadPhase::Busy) problem = PlaylistDownloadProblem::Busy;
		else if (phase == PlaylistDownloadPhase::Unavailable) problem = PlaylistDownloadProblem::Unavailable;
		resolvers.erase(ticket.index);
		const auto i = transferAuthority.find(ticket.index);
		if (i != end(transferAuthority)) i->second->store(false, std::memory_order_release);
		transfers.erase(ticket.index);
		cacheTransfers.erase(ticket.index);
		transferAuthority.erase(ticket.index);
		pump();
	}

	void pump() {
		if (!current()) {
			cancel(true, PlaylistDownloadProblem::Changed);
			return;
		}
		const auto tickets = policy.admit();
		const auto weak = weak_from_this();
		for (const auto ticket : tickets) {
			if (!current() || !policy.current(ticket)) break;
			auto resolver = std::make_shared<DesktopPlaylistResolver>(session.get(),
				std::vector<PlaylistResolveSong>{ songs[ticket.index] },
				[weak, ticket](std::vector<PlaylistSongResolution> values) mutable {
					crl::on_main([weak, ticket, values = std::move(values)]() mutable {
						const auto self = weak.lock();
						if (self && self->current() && self->policy.current(ticket)) {
							self->resolved(ticket, std::move(values));
						}
					});
				});
			resolvers[ticket.index] = resolver;
			resolver->start();
		}
		notify();
	}

	void resolved(PlaylistDownloadTicket ticket, std::vector<PlaylistSongResolution> values) {
		if (values.size() != 1 || values.front().documentId != songs[ticket.index].documentId) {
			finish(ticket, PlaylistDownloadPhase::Failed);
			return;
		}
		const auto resolution = values.front();
		if (resolution.status != PlaylistSongStatus::Ready
			&& resolution.status != PlaylistSongStatus::EarlierCopy) {
			finish(ticket, resolution.status == PlaylistSongStatus::Failed
				|| resolution.status == PlaylistSongStatus::Resolving
				? PlaylistDownloadPhase::Failed : PlaylistDownloadPhase::Unavailable);
			return;
		}
		const auto weak = weak_from_this();
		RequestAccountPlaylistDownloads(session.get(), {}, [weak, ticket, resolution](auto result) {
			const auto self = weak.lock();
			if (!self) return;
			if (!self->current()) {
				self->cancel(true, PlaylistDownloadProblem::Changed);
				return;
			}
			if (!self->policy.current(ticket)) return;
			if (!result || !result.downloads || !*result.downloads) {
				self->finish(ticket, PlaylistDownloadPhase::Failed);
				return;
			}
			const auto &library = *result.snapshot.library;
			const auto i = library.playlists.find(self->playlistId);
			const auto &song = self->songs[ticket.index];
			if (i == end(library.playlists) || i->second.isPartial() || i->second.deleted->value
				|| library.purged.contains(self->playlistId)
				|| !i->second.songs->contains(song.documentId)
				|| i->second.songs->at(song.documentId).value.source != std::optional(song.source)) {
				self->finish(ticket, PlaylistDownloadPhase::Unavailable);
				return;
			}
			self->save(ticket, resolution, result.downloads->state);
		});
	}

	void install(PlaylistDownloadTicket ticket, const PlaylistSongResolution &resolution,
			const PlaylistDownloadState &state) {
		auto item = static_cast<HistoryItem*>(nullptr);
		const auto document = Revalidate(session.get(), resolution, item);
		const auto i = state.files.find(resolution.documentId);
		if (!document || i == end(state.files) || document->loading() || document->uploading()) {
			finish(ticket, PlaylistDownloadPhase::Busy);
			return;
		}
		const auto path = PlaylistKeptFilePath(directory, resolution.documentId, i->second.extension);
		const auto location = Core::FileLocation(path);
		if (!location.check() || location.size != document->size
			|| i->second.size != uint64_t(document->size) || SyncPathIdentity(path) != i->second.identity) {
			finish(ticket, PlaylistDownloadPhase::Failed);
			return;
		}
		document->setLocation(location);
		session.get()->local().writeFileLocation(document->mediaKey(), location);
		finish(ticket, PlaylistDownloadPhase::Downloaded);
	}

	void commitTransfer(PlaylistDownloadTicket ticket, const PlaylistSongResolution &resolution,
			const PlaylistDownloadPrepared &prepared) {
		if (!current() || !policy.current(ticket)) return;
		auto item = static_cast<HistoryItem*>(nullptr);
		const auto document = Revalidate(session.get(), resolution, item);
		if (!document || uint64_t(document->size) != prepared.intent.size) {
			finish(ticket, PlaylistDownloadPhase::Unavailable);
			return;
		}
		const auto weak = weak_from_this();
		RequestAccountPlaylistDownloads(session.get(), {
			.kind = PlaylistDownloadRequestKind::CommitTransfer,
			.playlistId = playlistId,
			.transferToken = prepared.token,
			.documentId = resolution.documentId,
			.expectedSource = songs[ticket.index].source,
			.file = { prepared.intent.extension, prepared.intent.size, {}, {}, {} },
			.jobAuthority = transferAuthority.at(ticket.index),
		}, [weak, ticket, resolution](auto result) {
			const auto self = weak.lock();
			if (!self) return;
			if (!self->current()) {
				self->cancel(true, PlaylistDownloadProblem::Changed);
				return;
			}
			if (!self->policy.current(ticket)) return;
			if (!result || !result.downloads || !*result.downloads) {
				self->finish(ticket, PlaylistDownloadPhase::Failed);
				return;
			}
			self->install(ticket, resolution, result.downloads->state);
		});
	}

	void cache(PlaylistDownloadTicket ticket, const PlaylistSongResolution &resolution,
			not_null<DocumentData*> document, not_null<HistoryItem*> item) {
		const auto token = std::make_shared<std::atomic<bool>>(true);
		transferAuthority[ticket.index] = token;
		const auto key = document->bigFileBaseCacheKey();
		const auto size = document->size;
		const auto weak = weak_from_this();
		auto transfer = std::make_shared<PlaylistManagedCacheTransfer>(document,
			Data::FileOrigin(item->fullId()), token,
			[weak, ticket, resolution, key, size](PlaylistDownloadStatus status) {
				const auto self = weak.lock();
				if (!self) return;
				if (!self->current()) {
					self->cancel(true, PlaylistDownloadProblem::Changed);
					return;
				}
				if (!self->policy.current(ticket)) return;
				if (status != PlaylistDownloadStatus::Ready) {
					self->finish(ticket,
						status == PlaylistDownloadStatus::Busy ? PlaylistDownloadPhase::Busy
						: status == PlaylistDownloadStatus::Unsupported ? PlaylistDownloadPhase::Unavailable
						: PlaylistDownloadPhase::Failed,
						status == PlaylistDownloadStatus::Unsupported ? PlaylistDownloadProblem::Unsupported
						: PlaylistDownloadProblem::None);
					return;
				}
				RequestAccountPlaylistDownloads(self->session.get(), {},
					[weak, ticket, resolution, key, size](auto result) {
						const auto owner = weak.lock();
						if (!owner) return;
						if (!owner->current()) {
							owner->cancel(true, PlaylistDownloadProblem::Changed);
							return;
						}
						if (!owner->policy.current(ticket)) return;
						if (!result || !result.downloads) {
							owner->finish(ticket, PlaylistDownloadPhase::Failed);
							return;
						}
						const auto &library = *result.snapshot.library;
						const auto playlist = library.playlists.find(owner->playlistId);
						const auto &song = owner->songs[ticket.index];
						auto item = static_cast<HistoryItem*>(nullptr);
						const auto document = Revalidate(owner->session.get(), resolution, item);
						if (!document || document->size != size || document->bigFileBaseCacheKey() != key
							|| playlist == end(library.playlists) || playlist->second.isPartial()
							|| playlist->second.deleted->value || library.purged.contains(owner->playlistId)
							|| !playlist->second.songs->contains(song.documentId)
							|| playlist->second.songs->at(song.documentId).value.source != std::optional(song.source)) {
							owner->finish(ticket, PlaylistDownloadPhase::Unavailable);
							return;
						}
						owner->finish(ticket, PlaylistDownloadPhase::Downloaded);
					});
			});
		cacheTransfers[ticket.index] = transfer;
		transfer->start();
	}

	void save(PlaylistDownloadTicket ticket, const PlaylistSongResolution &resolution,
			const PlaylistDownloadState &state) {
		auto item = static_cast<HistoryItem*>(nullptr);
		const auto document = Revalidate(session.get(), resolution, item);
		if (!document) {
			finish(ticket, PlaylistDownloadPhase::Unavailable);
			return;
		}
		if (!PlaylistStreamingSizeSupported(document->size)) {
			finish(ticket, PlaylistDownloadPhase::Unavailable, PlaylistDownloadProblem::Unsupported);
			return;
		}
		if (!PlaylistKeepsDownloads(state, playlistId, ActiveSettings().playlists.keepDownloaded)) {
			cache(ticket, resolution, document, item);
			return;
		}
		const auto old = state.files.find(document->id);
		auto extension = old == end(state.files)
			? QFileInfo(document->filename()).suffix().toLower() : old->second.extension;
		if (!PlaylistDownloadExtensionSafe(extension)) extension = u"bin"_q;
		const auto token = std::make_shared<std::atomic<bool>>(true);
		transferAuthority[ticket.index] = token;
		const auto weak = weak_from_this();
		RequestAccountPlaylistDownloads(session.get(), {
			.kind = PlaylistDownloadRequestKind::PrepareTransfer,
			.playlistId = playlistId,
			.documentId = document->id,
			.expectedSource = songs[ticket.index].source,
			.file = { extension, uint64_t(document->size), {}, {}, {} },
			.jobAuthority = token,
		}, [weak, ticket, resolution](auto result) {
			const auto self = weak.lock();
			if (!self) return;
			if (!self->current()) {
				self->cancel(true, PlaylistDownloadProblem::Changed);
				return;
			}
			if (!self->policy.current(ticket)) return;
			if (!result || !result.downloads || !*result.downloads || !result.downloads->prepared) {
				self->finish(ticket, result.downloads
					&& result.downloads->status == PlaylistDownloadStatus::Busy
					? PlaylistDownloadPhase::Busy : PlaylistDownloadPhase::Failed);
				return;
			}
			const auto prepared = *result.downloads->prepared;
			if (prepared.alreadyKept) {
				self->install(ticket, resolution, result.downloads->state);
				return;
			} else if (prepared.complete) {
				self->commitTransfer(ticket, resolution, prepared);
				return;
			}
			auto item = static_cast<HistoryItem*>(nullptr);
			const auto document = Revalidate(self->session.get(), resolution, item);
			if (!document || uint64_t(document->size) != prepared.intent.size) {
				self->finish(ticket, PlaylistDownloadPhase::Unavailable);
				return;
			}
			const auto path = PlaylistStagingFilePath(self->directory, prepared.token);
			auto transfer = std::make_shared<PlaylistOwnedTransfer>(document,
				Data::FileOrigin(item->fullId()), path, prepared.intent.identity,
				self->transferAuthority.at(ticket.index),
				[weak, ticket, resolution, prepared](PlaylistDownloadStatus status) {
					const auto owner = weak.lock();
					if (!owner || !owner->current() || !owner->policy.current(ticket)) return;
					if (status != PlaylistDownloadStatus::Ready) {
						owner->finish(ticket, status == PlaylistDownloadStatus::Busy
							? PlaylistDownloadPhase::Busy : status == PlaylistDownloadStatus::Unsupported
							? PlaylistDownloadPhase::Unavailable : PlaylistDownloadPhase::Failed,
							status == PlaylistDownloadStatus::Unsupported ? PlaylistDownloadProblem::Unsupported
							: PlaylistDownloadProblem::None);
						return;
					}
					owner->commitTransfer(ticket, resolution, prepared);
				});
			self->transfers[ticket.index] = transfer;
			transfer->start();
		});
	}

	base::weak_ptr<Main::Session> session;
	AccountKey account;
	std::optional<AccountRoutingSnapshot> route;
	QString playlistId;
	QString directory;
	Fn<void(PlaylistDownloadProgress)> changed;
	std::vector<PlaylistResolveSong> songs;
	PlaylistDownloadJobPolicy policy;
	std::map<std::size_t, std::shared_ptr<DesktopPlaylistResolver>> resolvers;
	std::map<std::size_t, std::shared_ptr<std::atomic<bool>>> transferAuthority;
	std::map<std::size_t, std::shared_ptr<PlaylistOwnedTransfer>> transfers;
	std::map<std::size_t, std::shared_ptr<PlaylistManagedCacheTransfer>> cacheTransfers;
	std::shared_ptr<std::atomic<bool>> authority =
		std::make_shared<std::atomic<bool>>(true);
	PlaylistDownloadProblem problem = PlaylistDownloadProblem::None;
	bool started = false;
	bool cancelled = false;

};

DesktopPlaylistDownloadJob::DesktopPlaylistDownloadJob(
		not_null<Main::Session*> session, QString playlistId,
		Fn<void(PlaylistDownloadProgress)> changed)
: _state(std::make_shared<State>(session, playlistId, std::move(changed))) {
	const auto weak = std::weak_ptr<State>(_state);
	RegisterPlaylistDownloadJob(session.get(), std::move(playlistId), _state, [weak] {
		if (const auto state = weak.lock()) state->cancel();
	});
}

DesktopPlaylistDownloadJob::~DesktopPlaylistDownloadJob() {
	_state->cancel(false);
}

void DesktopPlaylistDownloadJob::start() {
	const auto state = _state;
	state->start();
}

void DesktopPlaylistDownloadJob::retry() {
	const auto state = _state;
	state->retry();
}

void DesktopPlaylistDownloadJob::cancel() {
	const auto state = _state;
	state->cancel();
}

} // namespace Purple
