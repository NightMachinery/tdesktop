#include "purple/purple_music_index_service.h"

#include "purple/hooks/music_index.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "crl/crl_object_on_thread.h"
#include "data/data_chat_filters.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_config.h"
#include "purple/purple_playlists_menu.h"
#include "purple/purple_playlists_policy.h"
#include "purple/purple_user_control.h"
#include "storage/storage_account.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>

namespace Purple {
namespace {

constexpr auto kMusicFolder = "purple.music.source-filter";
rpl::event_stream<Main::Session*> MusicArrivals;

class IndexWorker final {
public:
	IndexWorker(QString root, QString key, std::shared_ptr<std::atomic<bool>> live)
	: _root(std::move(root))
	, _key(std::move(key))
	, _live(std::move(live)) {
	}

	MusicIndex *index(QString *errorOut = nullptr) {
		const auto fail = [&](QString reason) -> MusicIndex* {
			_lastError = std::move(reason);
			if (errorOut) *errorOut = _lastError;
			return nullptr;
		};
		if (!_live->load()) return fail(u"music index worker stopped"_q);
		if (_index) {
			return current()
				? _index.get()
				: fail(u"music index directory permissions or identity changed"_q);
		}
		if (!QFileInfo::exists(_root) && !QDir().mkpath(_root)) {
			return fail(u"could not create Purple config directory"_q);
		}
		_canonical = QFileInfo(_root).canonicalFilePath();
		if (_canonical.isEmpty()) {
			return fail(u"Purple config directory path is unavailable"_q);
		}
		_library = _canonical + u"/music-index"_q;
		_directory = _library + u'/' + _key;
		if (!DesktopArchiveDirectory(_library)) {
			return fail(u"music-index directory failed privacy or backup-exclusion check"_q);
		}
		if (!DesktopArchiveDirectory(_directory)) {
			return fail(u"account music-index directory failed privacy or backup-exclusion check"_q);
		}
		_path = _directory + u"/library.sqlite"_q;
		const auto existing = QFileInfo(_path);
		if (!existing.exists() && !existing.isSymLink()
			&& !DesktopArchiveWrite(_path, {})) {
			return fail(u"could not create library.sqlite privately"_q);
		}
		_libraryIdentity = DesktopArchiveIdentity(_library);
		_directoryIdentity = DesktopArchiveIdentity(_directory);
		_fileIdentity = DesktopArchiveIdentity(_path);
		if (!current()) {
			return fail(u"library.sqlite failed ownership or 0600/0700 privacy check"_q);
		}
		auto index = std::make_unique<MusicIndex>([=] { return current(); });
		if (!index->open(_path)) {
			return fail(index->lastError().isEmpty()
				? u"could not open library.sqlite"_q
				: index->lastError());
		}
		_lastError.clear();
		_index = std::move(index);
		return _index.get();
	}

	bool current() const {
		return _live->load()
			&& QFileInfo(_root).canonicalFilePath() == _canonical
			&& !_libraryIdentity.isEmpty()
			&& DesktopArchiveIdentity(_library) == _libraryIdentity
			&& DesktopArchivePrivate(_library, true)
			&& !_directoryIdentity.isEmpty()
			&& DesktopArchiveIdentity(_directory) == _directoryIdentity
			&& DesktopArchivePrivate(_directory, true)
			&& !_fileIdentity.isEmpty()
			&& DesktopArchiveIdentity(_path) == _fileIdentity
			&& DesktopArchivePrivate(_path, false);
	}

private:
	QString _root;
	QString _key;
	std::shared_ptr<std::atomic<bool>> _live;
	QString _canonical;
	QString _library;
	QString _directory;
	QString _path;
	QByteArray _libraryIdentity;
	QByteArray _directoryIdentity;
	QByteArray _fileIdentity;
	QString _lastError;
	std::unique_ptr<MusicIndex> _index;

};

struct AccountIndex {
	AccountKey account;
	QString root;
	std::shared_ptr<std::atomic<bool>> live =
		std::make_shared<std::atomic<bool>>(true);
	crl::object_on_thread<IndexWorker> worker;

	AccountIndex(AccountKey account, QString root, QString key)
	: account(account)
	, root(std::move(root))
	, worker(QString(this->root), std::move(key),
		std::shared_ptr<std::atomic<bool>>(live)) {
	}

	~AccountIndex() {
		live->store(false);
	}
};

std::map<Main::Session*, std::unique_ptr<AccountIndex>> Indexes;

bool Current(not_null<Main::Session*> session) {
	return !Core::App().passcodeLocked()
		&& !session->account().loggingOut()
		&& session->account().maybeSession() == session;
}

QString CurrentUnavailableReason(not_null<Main::Session*> session) {
	if (Core::App().passcodeLocked()) return u"app is passcode locked"_q;
	if (session->account().loggingOut()) return u"account is logging out"_q;
	if (session->account().maybeSession() != session) return u"session changed"_q;
	const auto account = SessionAccountKey(session);
	if (!AccountKeyText(account.environment, account.userId)) {
		return u"account identity is unavailable"_q;
	}
	if (ConfigDirectory().isEmpty()) {
		return u"Purple config directory is unset"_q;
	}
	return u"music index unavailable"_q;
}

AccountIndex *For(not_null<Main::Session*> session) {
	if (!Current(session)) return nullptr;
	const auto account = SessionAccountKey(session);
	const auto key = AccountKeyText(account.environment, account.userId);
	const auto root = ConfigDirectory();
	if (!key || root.isEmpty()) return nullptr;
	auto it = Indexes.find(session.get());
	if (it != Indexes.end()
		&& (it->second->account != account || it->second->root != root)) {
		Indexes.erase(it);
		it = Indexes.end();
	}
	if (it == Indexes.end()) {
		it = Indexes.emplace(
			session.get(),
			std::make_unique<AccountIndex>(account, root, *key)).first;
		session->lifetime().add([ptr = session.get()] { Indexes.erase(ptr); });
	}
	return it->second.get();
}

template <typename Operation, typename MakeError, typename Result>
void Request(
		not_null<Main::Session*> session,
		Operation operation,
		MakeError makeError,
		Fn<void(Result)> done) {
	const auto service = For(session);
	if (!service) {
		done(makeError(CurrentUnavailableReason(session)));
		return;
	}
	const auto weak = base::make_weak(session.get());
	const auto account = service->account;
	const auto root = service->root;
	const auto live = service->live;
	service->worker.with([
		weak, account, root, live,
		operation = std::move(operation),
		makeError = std::move(makeError),
		done = std::move(done)
	](IndexWorker &worker) mutable {
		auto openError = QString();
		const auto index = worker.index(&openError);
		auto result = index
			? operation(*index)
			: makeError(openError.isEmpty() ? u"music index unavailable"_q : openError);
		if (index && !worker.current()) {
			result = makeError(u"music index revoked during operation"_q);
		}
		crl::on_main([
			weak, account, root, live,
			result = std::move(result),
			makeError = std::move(makeError),
			done = std::move(done)
		]() mutable {
			const auto session = weak.get();
			if (!session) return;
			if (!live->load() || !Current(session)
				|| SessionAccountKey(session) != account
				|| ConfigDirectory() != root) {
				result = makeError(u"session or config changed during index request"_q);
			}
			done(std::move(result));
		});
	});
}

} // namespace

FilterId MusicSourceFolder(not_null<Main::Session*> session) {
	return session->local().readPref<QString>(kMusicFolder).toInt();
}

void SetMusicSourceFolder(not_null<Main::Session*> session, FilterId folder) {
	if (MusicSourceFolder(session) == folder) return;
	Indexes.erase(session.get());
	session->local().writePref<QString>(kMusicFolder, QString::number(folder));
}

MusicIndexEntryInspection InspectMusicIndexEntry(not_null<HistoryItem*> item) {
	const auto history = item->history();
	if (!item->computeUnavailableReason().isEmpty()) {
		return { std::nullopt, MusicSkipReason::Unavailable };
	}
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document || !document->isSong() || item->date() <= 0) {
		return { std::nullopt, MusicSkipReason::NotSong };
	}
	if (item->id.bare <= 0 || item->id.bare > kMaximumPlaylistMessageId) {
		return { std::nullopt, MusicSkipReason::Other };
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
	const auto qualified = QualifySong(candidate, SaveRestrictionsDisabled());
	if (!qualified) {
		if (qualified.reason == SongQualificationReason::ProtectedContentRestricted) {
			return { std::nullopt, MusicSkipReason::ProtectedContent };
		}
		if (qualified.reason == SongQualificationReason::MissingAudioAttribute
			|| qualified.reason == SongQualificationReason::VoiceMessage
			|| qualified.reason == SongQualificationReason::NotMainMessageMedia) {
			return { std::nullopt, MusicSkipReason::NotSong };
		}
		return { std::nullopt, MusicSkipReason::Other };
	}
	const auto songData = document->song();
	const auto meta = PrepareSongMeta({
		.title = songData ? songData->title : QString(),
		.performer = songData ? songData->performer : QString(),
		.fileName = document->filename(),
		.durationSeconds = document->duration() / 1000,
	});
	if (!meta) {
		return { std::nullopt, MusicSkipReason::InvalidMetadata };
	}
	return {
		MusicIndexEntry{
			.peer = history->peer->id.value,
			.message = item->id.bare,
			.document = document->id,
			.postedAt = item->date(),
			.title = meta.meta.title,
			.performer = meta.meta.performer,
			.duration = meta.meta.duration,
			.searchText = meta.meta.title + u' ' + meta.meta.performer
				+ u' ' + document->filename(),
		},
		MusicSkipReason::None,
	};
}

std::optional<MusicIndexEntry> CaptureMusicIndexEntry(not_null<HistoryItem*> item) {
	return InspectMusicIndexEntry(item).entry;
}

rpl::producer<> MusicIndexArrivals(not_null<Main::Session*> session) {
	return MusicArrivals.events() | rpl::filter([=](Main::Session *owner) {
		return owner == session;
	}) | rpl::to_empty;
}

void ObserveMusicIndexArrivals(Main::Session &owner) {
	const auto session = not_null(&owner);
	session->data().newItemAdded() | rpl::on_next([=](not_null<HistoryItem*> item) {
		const auto media = item->media();
		const auto document = media ? media->document() : nullptr;
		if (!document || !document->isSong() || !Current(session)) return;
		const auto folder = MusicSourceFolder(session);
		const auto &filters = session->data().chatsFilters();
		if (folder <= 0 || Data::IsPurpleView(folder) || !filters.loaded()) return;
		const auto &list = filters.list();
		const auto filter = std::ranges::find(list, folder, &Data::ChatFilter::id);
		if (filter == list.end() || !filter->contains(item->history(), true, true)) return;
		if (const auto row = CaptureMusicIndexEntry(item)) {
			Request(session,
				[folder, row = *row](MusicIndex &index) {
					return index.saveArrival(folder, row);
				},
				[](QString) { return false; },
				Fn<void(bool)>([=](bool saved) {
					if (saved && Current(session) && MusicSourceFolder(session) == folder) {
						MusicArrivals.fire_copy(session.get());
					}
				}));
		}
	}, session->lifetime());
}

void StopMusicIndex(Main::Account &account) {
	if (const auto session = account.maybeSession()) Indexes.erase(session);
}

void ConfigureMusicIndex(
		not_null<Main::Session*> session,
		qint64 folder,
		std::vector<quint64> peers,
		Fn<void(MusicIndexConfigureResult)> done) {
	Request(session,
		[folder, peers = std::move(peers)](MusicIndex &index) -> MusicIndexConfigureResult {
			const auto scope = index.configureSources(folder, peers);
			if (!scope) {
				return { std::nullopt, index.lastError().isEmpty()
					? u"configureSources failed"_q : index.lastError() };
			}
			if (auto sources = index.sources()) {
				return { MusicIndexRoster{ *scope, std::move(*sources) }, QString() };
			}
			return { std::nullopt, index.lastError().isEmpty()
				? u"sources read failed"_q : index.lastError() };
		},
		[](QString error) -> MusicIndexConfigureResult {
			return { std::nullopt, std::move(error) };
		},
		std::move(done));
}

void SaveMusicIndexPage(
		not_null<Main::Session*> session,
		MusicIndexScope scope,
		MusicSourceCursor expected,
		MusicSourceCursor next,
		std::vector<MusicIndexEntry> rows,
		Fn<void(MusicIndexSaveResult)> done) {
	Request(session,
		[scope, expected, next, rows = std::move(rows)](MusicIndex &index) -> MusicIndexSaveResult {
			const auto ok = index.savePage(scope, expected, next, rows);
			return { ok, ok ? QString() : index.lastError() };
		},
		[](QString error) -> MusicIndexSaveResult {
			return { false, std::move(error) };
		},
		std::move(done));
}

void ReadMusicIndexPage(
		not_null<Main::Session*> session,
		MusicIndexScope scope,
		QString query,
		std::vector<quint64> visiblePeers,
		std::optional<MusicIndexCursor> cursor,
		bool tail,
		Fn<void(MusicIndexPageResult)> done) {
	Request(session,
		[scope, query = std::move(query),
			visiblePeers = std::move(visiblePeers), cursor, tail](MusicIndex &index)
		-> MusicIndexPageResult {
			auto rows = index.page(scope, query, visiblePeers, cursor, tail);
			if (!rows) {
				return { false, {}, 0, index.lastError().isEmpty()
					? u"page read failed"_q : index.lastError() };
			}
			const auto total = index.count(scope, visiblePeers);
			return {
				true,
				std::move(*rows),
				total.value_or(qint64(0)),
				QString(),
			};
		},
		[](QString error) -> MusicIndexPageResult {
			return { false, {}, 0, std::move(error) };
		},
		std::move(done));
}

} // namespace Purple
