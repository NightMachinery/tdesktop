#include "purple/purple_music_index_service.h"

#include "purple/hooks/music_index.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "crl/crl_object_on_thread.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_config.h"

#include <QtCore/QFileInfo>

namespace Purple {
namespace {

class IndexWorker final {
public:
	IndexWorker(QString root, QString key, std::shared_ptr<std::atomic<bool>> live)
	: _root(std::move(root))
	, _key(std::move(key))
	, _live(std::move(live)) {
	}

	MusicIndex *index() {
		if (!_live->load()) return nullptr;
		if (_index) return current() ? _index.get() : nullptr;
		_canonical = QFileInfo(_root).canonicalFilePath();
		if (_canonical.isEmpty()) return nullptr;
		_library = _canonical + u"/music-index"_q;
		_directory = _library + u'/' + _key;
		if (!DesktopArchiveDirectory(_library)
			|| !DesktopArchiveDirectory(_directory)) {
			return nullptr;
		}
		_path = _directory + u"/library.sqlite"_q;
		const auto existing = QFileInfo(_path);
		if (!existing.exists() && !existing.isSymLink()
			&& !DesktopArchiveWrite(_path, {})) {
			return nullptr;
		}
		_libraryIdentity = DesktopArchiveIdentity(_library);
		_directoryIdentity = DesktopArchiveIdentity(_directory);
		_fileIdentity = DesktopArchiveIdentity(_path);
		if (!current()) return nullptr;
		auto index = std::make_unique<MusicIndex>([=] { return current(); });
		if (!index->open(_path)) return nullptr;
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

template <typename Operation, typename Result>
void Request(
		not_null<Main::Session*> session,
		Operation operation,
		Fn<void(Result)> done) {
	const auto service = For(session);
	if (!service) {
		done(Result());
		return;
	}
	const auto weak = base::make_weak(session.get());
	const auto account = service->account;
	const auto root = service->root;
	const auto live = service->live;
	service->worker.with([
		weak, account, root, live,
		operation = std::move(operation), done = std::move(done)
	](IndexWorker &worker) mutable {
		const auto index = worker.index();
		auto result = index ? operation(*index) : Result();
		if (!worker.current()) result = Result();
		crl::on_main([
			weak, account, root, live,
			result = std::move(result), done = std::move(done)
		]() mutable {
			const auto session = weak.get();
			if (!session) return;
			if (!live->load() || !Current(session)
				|| SessionAccountKey(session) != account
				|| ConfigDirectory() != root) {
				result = Result();
			}
			done(std::move(result));
		});
	});
}

} // namespace

void StopMusicIndex(Main::Account &account) {
	if (const auto session = account.maybeSession()) Indexes.erase(session);
}

void ConfigureMusicIndex(
		not_null<Main::Session*> session,
		qint64 folder,
		std::vector<quint64> peers,
		Fn<void(std::optional<MusicIndexRoster>)> done) {
	Request(session, [folder, peers = std::move(peers)](MusicIndex &index)
	-> std::optional<MusicIndexRoster> {
		const auto scope = index.configureSources(folder, peers);
		if (scope) {
			if (auto sources = index.sources()) {
				return MusicIndexRoster{ *scope, std::move(*sources) };
			}
		}
		return std::nullopt;
	}, std::move(done));
}

void SaveMusicIndexPage(
		not_null<Main::Session*> session,
		MusicIndexScope scope,
		MusicSourceCursor expected,
		MusicSourceCursor next,
		std::vector<MusicIndexEntry> rows,
		Fn<void(bool)> done) {
	Request(session, [scope, expected, next, rows = std::move(rows)](MusicIndex &index) {
		return index.savePage(scope, expected, next, rows);
	}, std::move(done));
}

void ReadMusicIndexPage(
		not_null<Main::Session*> session,
		MusicIndexScope scope,
		QString query,
		std::vector<quint64> visiblePeers,
		std::optional<MusicIndexCursor> cursor,
		Fn<void(std::optional<std::vector<MusicIndexEntry>>)> done) {
	Request(session, [scope, query = std::move(query),
		visiblePeers = std::move(visiblePeers), cursor](MusicIndex &index) {
		return index.page(scope, query, visiblePeers, cursor);
	}, std::move(done));
}

} // namespace Purple
