#include "purple/purple_sync_chat_create_store.h"

#include "purple/purple_sync_local_store.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QThread>

#include <algorithm>

namespace Purple {
namespace {

bool Worker() {
	const auto app = QCoreApplication::instance();
	return !app || QThread::currentThread() != app->thread();
}

}

DesktopSyncChatCreateStore::DesktopSyncChatCreateStore(QString directory,
		QString authority, AccountKey account, Fn<bool()> current)
: _directory(std::move(directory))
, _path(QDir(_directory).filePath(u"sync-chat-setup.json"_q))
, _authority(std::move(authority))
, _account(account)
, _current(std::move(current)) {
}

bool DesktopSyncChatCreateStore::Open() {
	const auto authority = _authority.size() == 64
		&& std::all_of(_authority.begin(), _authority.end(), [](QChar ch) {
			return (ch >= u'0' && ch <= u'9') || (ch >= u'a' && ch <= u'f');
		});
	if (_lock || !Worker() || !authority
		|| !AccountKeyText(_account.environment, _account.userId)
		|| !_current || !_current()
		|| EnsureSyncPrivateDirectory(_directory) != SyncStoreStatus::Ready) {
		_error = u"setup-worker-or-directory-held"_q;
		return false;
	}
	_lock = std::make_unique<QLockFile>(QDir(_directory).filePath(u"lock"_q));
	_lock->setStaleLockTime(0);
	if (!_lock->tryLock(0)) {
		_lock.reset(); _error = u"account-lock-busy"_q; return false;
	}
	_identity = SyncPathIdentity(_path);
	if (_identity.isEmpty()) {
		_error = u"setup-path-held"_q;
	} else if (_identity != "missing") {
		if (ReadSyncPrivateFile(_path, 512 * 1024, _bytes) != SyncStoreStatus::Ready) {
			_error = u"setup-read-held"_q;
		} else {
			_journal = ParseDesktopSyncChatCreateJournal(_bytes, _authority, _account);
			if (!_journal) _error = u"setup-journal-or-login-held"_q;
		}
	}
	if (!_current() || _identity != SyncPathIdentity(_path)) _error = u"setup-opening-changed"_q;
	if (!_error.isEmpty()) { _lock.reset(); return false; }
	return true;
}

bool DesktopSyncChatCreateStore::Save(const DesktopSyncChatCreateJournal &journal) {
	const auto fail = [&] { _error = u"setup-write-held"_q; return false; };
	if (!_lock || !Worker() || !_error.isEmpty() || !_current || !_current()
		|| journal.authority != _authority || journal.account != _account
		|| (_journal && _journal->operation != journal.operation)
		|| _identity != SyncPathIdentity(_path)) return fail();
	const auto bytes = SerializeDesktopSyncChatCreateJournal(journal);
	if (!bytes) return fail();
	if (_identity != "missing") {
		auto before = QByteArray();
		if (ReadSyncPrivateFile(_path, 512 * 1024, before) != SyncStoreStatus::Ready
			|| before != _bytes) return fail();
	}
	if (!_current() || WriteSyncPrivateFile(_path, *bytes) != SyncStoreStatus::Ready) return fail();
	_bytes = *bytes;
	_identity = SyncPathIdentity(_path);
	_journal = journal;
	return _current() && !_identity.isEmpty() && _identity != "missing";
}

const std::optional<DesktopSyncChatCreateJournal> &DesktopSyncChatCreateStore::journal() const {
	return _journal;
}
QString DesktopSyncChatCreateStore::error() const { return _error; }

}
