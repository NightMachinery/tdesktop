#include "purple/purple_playlists_resolver.h"

#include "purple/purple_config.h"
#include "purple/purple_playlists_policy.h"
#include "purple/purple_sync_request_sender.h"
#include "purple/purple_sync_chat_requests.h"
#include "purple/purple_user_control.h"
#include "crl/crl_on_main.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_account.h"
#include "main/main_session.h"

namespace Purple {
namespace {

PeerId SourcePeer(const PlaylistSourceRef &source, uint64_t selfUserId) {
	switch (source.peer.kind) {
	case PeerTokenKind::Self: return peerFromUser(UserId(selfUserId));
	case PeerTokenKind::User: return peerFromUser(UserId(source.peer.id));
	case PeerTokenKind::Chat: return peerFromChat(ChatId(source.peer.id));
	case PeerTokenKind::Channel: return peerFromChannel(ChannelId(source.peer.id));
	}
	return PeerId();
}

PlaylistSourceRead ReadItem(not_null<HistoryItem*> item) {
	if (!item->computeUnavailableReason().isEmpty()) {
		return { PlaylistSongStatus::Restricted };
	} else if (item->isEmpty()) {
		return { PlaylistSongStatus::Deleted };
	}
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document || item->id.bare <= 0 || item->id.bare > kMaximumPlaylistMessageId) {
		return { PlaylistSongStatus::Changed };
	}
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
	return QualifySong(candidate, SaveRestrictionsDisabled())
		? PlaylistSourceRead{ PlaylistSongStatus::Ready, document->id }
		: PlaylistSourceRead{ PlaylistSongStatus::Restricted };
}

} // namespace

class DesktopPlaylistResolver::Impl final : public base::has_weak_ptr {
public:
	Impl(not_null<Main::Session*> session,
		std::vector<PlaylistResolveSong> songs,
		Fn<void(std::vector<PlaylistSongResolution>)> done)
	: _session(base::make_weak(session.get()))
	, _account(SessionAccountKey(session))
	, _api(&session->mtp(), [=] { return current(); })
	, _queue(std::move(songs), _account.userId, {
		.cached = [=](const PlaylistSourceRef &source) { return cached(source); },
		.read = [=](auto refs, auto done) { read(std::move(refs), std::move(done)); },
		.defer = [](auto continuation) { crl::on_main(std::move(continuation)); },
	}, [=, done = std::move(done)](auto results) {
		if (current()) {
			done(std::move(results));
		}
	}) {
	}

	void start() { _queue.start(); }

private:
	bool current() const {
		const auto session = _session.get();
		return session && !session->account().loggingOut()
			&& session->account().maybeSession() == session
			&& SessionAccountKey(session) == _account;
	}

	std::optional<PlaylistSourceRead> cached(const PlaylistSourceRef &source) const {
		if (!current()) {
			return PlaylistSourceRead();
		}
		const auto item = _session.get()->data().message(
			SourcePeer(source, _account.userId), MsgId(source.msg));
		return item ? std::optional(ReadItem(item)) : std::nullopt;
	}

	ChannelData *channel(const PlaylistSourceRef &source) const {
		return source.peer.kind == PeerTokenKind::Channel
			? _session.get()->data().channelLoaded(ChannelId(source.peer.id)) : nullptr;
	}

	void read(std::vector<PlaylistSourceRef> refs, PlaylistResolverIo::Done done) {
		_refs = std::move(refs);
		_reads.assign(_refs.size(), std::nullopt);
		_done = std::move(done);
		next();
	}

	void next() {
		if (!current()) {
			return;
		}
		const auto pending = std::find(_reads.begin(), _reads.end(), std::nullopt);
		if (pending == _reads.end()) {
			auto result = std::vector<PlaylistSourceRead>();
			for (const auto &read : _reads) result.push_back(*read);
			auto done = std::move(_done);
			done(std::move(result));
			return;
		}
		const auto first = std::size_t(pending - _reads.begin());
		const auto ref = _refs[first];
		const auto isChannel = ref.peer.kind == PeerTokenKind::Channel;
		const auto peer = channel(ref);
		if (isChannel && (!peer || !peer->accessHash())) {
			if (ref.username.isEmpty()) {
				_reads[first] = PlaylistSourceRead{ PlaylistSongStatus::NoAccess };
				crl::on_main([weak = base::make_weak(this)] {
					if (const auto self = weak.get()) self->next();
				});
			} else {
				resolve(first);
			}
			return;
		}
		auto indices = std::vector<std::size_t>();
		auto ids = QVector<MTPInputMessage>();
		for (auto i = first; i != _refs.size(); ++i) {
			const auto &source = _refs[i];
			if (_reads[i] || (source.peer.kind == PeerTokenKind::Channel) != isChannel
				|| (isChannel && source.peer.id != ref.peer.id)) continue;
			indices.push_back(i);
			ids.push_back(MTP_inputMessageID(MTP_int(source.msg)));
		}
		const auto weak = base::make_weak(this);
		const auto send = [&](auto request) {
			_api.request(std::move(request)).done([weak, indices, peer, ref](const MTPmessages_Messages &response) {
				const auto self = weak.get();
				if (!self || !self->current()) return;
				if (!RecognizedDesktopSyncChatRead(response, ref.peer)) {
					self->finish(indices, PlaylistSongStatus::Failed);
					return;
				}
				self->_session.get()->data().processExistingMessages(peer, response);
				for (const auto i : indices) {
					self->_reads[i] = self->cached(self->_refs[i]).value_or(
						PlaylistSourceRead{ PlaylistSongStatus::Deleted });
				}
				self->next();
			}).fail([weak, indices](const MTP::Error &error) {
				if (const auto self = weak.get(); self && self->current()) {
					self->finish(indices, error.type() == u"CHANNEL_PRIVATE"_q
						|| error.type() == u"CHANNEL_INVALID"_q
						? PlaylistSongStatus::NoAccess : PlaylistSongStatus::Failed);
				}
			}).send();
		};
		if (isChannel) send(MTPchannels_GetMessages(peer->inputChannel(), MTP_vector<MTPInputMessage>(ids)));
		else send(MTPmessages_GetMessages(MTP_vector<MTPInputMessage>(ids)));
	}

	void finish(const std::vector<std::size_t> &indices, PlaylistSongStatus status) {
		for (const auto i : indices) _reads[i] = PlaylistSourceRead{ status };
		next();
	}

	void resolve(std::size_t index) {
		const auto ref = _refs[index];
		const auto weak = base::make_weak(this);
		_api.request(MTPcontacts_ResolveUsername(MTP_flags(0), MTP_string(ref.username), MTPstring()))
		.done([weak, index, ref](const MTPcontacts_ResolvedPeer &response) {
			const auto self = weak.get();
			if (!self || !self->current()) return;
			const auto &data = response.c_contacts_resolvedPeer();
			self->_session.get()->data().processUsers(data.vusers());
			self->_session.get()->data().processChats(data.vchats());
			const auto peer = self->channel(ref);
			const auto resolved = peerFromMTP(data.vpeer());
			if (!PlaylistResolvedChannelMatches(ref,
				{ peerIsChannel(resolved) ? PeerTokenKind::Channel : PeerTokenKind::User,
					peerIsChannel(resolved) ? peerToChannel(resolved).bare : 0 },
				peer ? peer->accessHash() : 0)) {
				self->_reads[index] = PlaylistSourceRead{ PlaylistSongStatus::NoAccess };
			}
			self->next();
		}).fail([weak, index](const MTP::Error &error) {
			if (const auto self = weak.get(); self && self->current()) {
				self->finish({ index }, error.type() == u"USERNAME_NOT_OCCUPIED"_q
					|| error.type() == u"USERNAME_INVALID"_q
					? PlaylistSongStatus::NoAccess : PlaylistSongStatus::Failed);
			}
		}).send();
	}

	base::weak_ptr<Main::Session> _session;
	AccountKey _account;
	SyncRequestSender _api;
	PlaylistResolveQueue _queue;
	std::vector<PlaylistSourceRef> _refs;
	std::vector<std::optional<PlaylistSourceRead>> _reads;
	PlaylistResolverIo::Done _done;

};

DesktopPlaylistResolver::DesktopPlaylistResolver(
		not_null<Main::Session*> session,
		std::vector<PlaylistResolveSong> songs,
		Fn<void(std::vector<PlaylistSongResolution>)> done)
: _impl(std::make_unique<Impl>(session, std::move(songs), std::move(done))) {
}

DesktopPlaylistResolver::~DesktopPlaylistResolver() = default;

void DesktopPlaylistResolver::start() {
	_impl->start();
}

} // namespace Purple
