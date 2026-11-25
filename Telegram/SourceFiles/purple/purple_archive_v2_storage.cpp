#include "purple/purple_archive_v2_store.h"

#include "purple/purple_archive_files.h"
#include "purple/purple_local_choice.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <algorithm>
#include <limits>

namespace Purple::ArchiveV2 {
namespace {

constexpr auto kPointerLimit = 16 * 1024;
constexpr auto kMagic = "AV2P";

bool Exists(const QString &path) {
	const auto info = QFileInfo(path);
	return info.exists() || info.isSymLink();
}

QString Hex(const QByteArray &bytes) {
	return QString::fromLatin1(bytes.toHex());
}

QByteArray Bytes(const QJsonValue &value) {
	const auto text = value.toString().toLatin1();
	const auto bytes = QByteArray::fromHex(text);
	return bytes.toHex() == text ? bytes : QByteArray();
}

}

void Lease::revoke() {
	live.store(false);
}

Store::Store(StoreOptions options) : _options(std::move(options)) {
}

Store::~Store() {
	if (_options.lease) {
		_options.lease->revoke();
		_options.lease->crypto.Revoke();
	}
	static_cast<void>(_db.CloseFinal());
	DesktopArchiveCrypto::Clear(_manifestKey);
	DesktopArchiveCrypto::Clear(_options.profileKey);
	DesktopArchiveCrypto::Clear(_options.loginSecret);
}

bool Store::step(Phase phase) const {
	return !_options.checkpoint || _options.checkpoint(phase);
}

QString Store::bind(bool create) {
	const auto account = AccountKeyText(_options.account.environment, _options.account.userId);
	if (!account || !_options.lease || _options.profileId.size() != 16 || _options.accountSlot.size() != 16
		|| _options.profileKey.size() != 32 || !QDir::isAbsolutePath(_options.root)
		|| QDir::cleanPath(_options.root) != _options.root || _options.root == u"/"_q) {
		return u"key_unavailable"_q;
	}
	_scope = "purple-archive-v2/" + _options.profileId.toHex() + '/' + _options.accountSlot.toHex()
		+ '/' + account->toLatin1() + '/';
	const auto profile = _options.root + '/' + Hex(_options.profileId);
	_root = profile + '/' + Hex(_options.accountSlot);
	for (const auto &path : { _options.root, profile, _root }) {
		const auto parent = QFileInfo(path).absolutePath();
		if (QFileInfo(parent).canonicalFilePath() != parent) return u"store_unreadable"_q;
		if (create ? !DesktopArchiveDirectory(path) : !DesktopArchivePrivate(path, true)) {
			return u"store_unreadable"_q;
		}
		if (QFileInfo(path).canonicalFilePath() != path) return u"store_unreadable"_q;
	}
	_rootIdentity = DesktopArchivePersistentIdentity(_root);
	_lock = std::make_unique<QLockFile>(_root + u"/live.lock"_q);
	_lock->setStaleLockTime(0);
	if (_rootIdentity.isEmpty() || !_lock->tryLock(0)) return u"store_busy"_q;
	auto manifest = DesktopArchiveCrypto::Derive(_options.profileKey,
		{ _options.profileId, _scope + "pointer" });
	if (!manifest) return u"key_unavailable"_q;
	_manifestKey = std::move(*manifest);
	if (_options.loginSecret.size() == 32) {
		auto wrapping = DesktopArchiveCrypto::Derive(_options.profileKey,
			{ _options.loginSecret, _scope + "wrap" });
		const auto added = wrapping && _options.lease->crypto.AddKey(u"wrap"_q, *wrapping);
		if (wrapping) DesktopArchiveCrypto::Clear(*wrapping);
		if (!added) return u"key_unavailable"_q;
	}
	DesktopArchiveCrypto::Clear(_options.profileKey);
	DesktopArchiveCrypto::Clear(_options.loginSecret);
	const auto failure = readPointer();
	_bound = failure.isEmpty();
	return failure;
}

QString Store::pointerPath() const {
	return _root + u"/active.v2"_q;
}

QString Store::archivePath(const QByteArray &id, bool detached) const {
	return _root + (detached ? u"/retired-"_q : u"/store-"_q) + Hex(id);
}

QString Store::directory() const {
	return archivePath(_pointer.id);
}

QString Store::readPointer() {
	const auto path = pointerPath();
	if (!Exists(path)) {
		const auto files = QDir(_root).entryList({ u"store-*"_q, u"retired-*"_q },
			QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
		return files.isEmpty() ? QString() : u"store_unreadable"_q;
	}
	const auto bytes = DesktopArchiveRead(path, kPointerLimit);
	if (!bytes || bytes->size() < 38 || !bytes->startsWith(kMagic)) return u"store_unreadable"_q;
	const auto json = bytes->mid(36);
	const auto mac = DesktopArchiveCrypto::Hmac(_manifestKey, json);
	if (!mac || *mac != bytes->mid(4, 32)) return u"pointer_authentication_failed"_q;
	const auto object = QJsonDocument::fromJson(json).object();
	const auto generation = object.value(u"generation"_q).toString();
	const auto initialized = object.value(u"initialized"_q);
	auto next = Pointer{ Bytes(object.value(u"id"_q)), Bytes(object.value(u"wrapped"_q)),
		Bytes(object.value(u"directory"_q)), generation.toLongLong(), initialized.toBool(), {} };
	const auto list = object.value(u"retired"_q);
	if (object.size() != 6 || next.generation <= 0 || generation != QString::number(next.generation)
		|| !initialized.isBool() || !list.isArray() || (next.id.isEmpty() ? !next.wrapped.isEmpty() || !next.directory.isEmpty()
			: next.id.size() != 16 || next.wrapped.size() != 60) || (next.initialized && next.directory.isEmpty())) {
		return u"store_unreadable"_q;
	}
	for (const auto &value : list.toArray()) {
		const auto entry = value.toObject();
		const auto id = Bytes(entry.value(u"id"_q));
		const auto identity = Bytes(entry.value(u"directory"_q));
		if (entry.size() != 2 || id.size() != 16 || identity.isEmpty() || id == next.id
			|| std::any_of(next.retired.begin(), next.retired.end(), [&](const Retired &old) { return old.id == id; })) {
			return u"store_unreadable"_q;
		}
		next.retired.push_back({ id, identity });
	}
	if (!DesktopArchiveSyncDirectory(_root)) return u"store_unreadable"_q;
	_published = *bytes;
	_pointer = std::move(next);
	return {};
}

bool Store::ownsPointer() const {
	const auto path = pointerPath();
	return !_rootIdentity.isEmpty() && DesktopArchivePersistentIdentity(_root) == _rootIdentity
		&& (_published.isEmpty() ? !Exists(path) : DesktopArchiveRead(path, kPointerLimit) == _published);
}

bool Store::publish(const Pointer &next) {
	const auto path = pointerPath();
	if (!step(Phase::BeforePublish) || !ownsPointer()) return false;
	auto retired = QJsonArray();
	for (const auto &entry : next.retired) {
		retired.push_back(QJsonObject{ { u"id"_q, Hex(entry.id) }, { u"directory"_q, Hex(entry.directory) } });
	}
	const auto json = QJsonDocument(QJsonObject{
		{ u"id"_q, Hex(next.id) }, { u"wrapped"_q, Hex(next.wrapped) },
		{ u"directory"_q, Hex(next.directory) },
		{ u"generation"_q, QString::number(next.generation) }, { u"initialized"_q, next.initialized },
		{ u"retired"_q, retired },
	}).toJson(QJsonDocument::Compact);
	const auto mac = DesktopArchiveCrypto::Hmac(_manifestKey, json);
	if (!mac) return false;
	const auto bytes = kMagic + *mac + json;
	if (bytes.size() > kPointerLimit || !DesktopArchiveWrite(path, bytes)) return false;
	_published = bytes;
	_pointer = next;
	return true;
}

bool Store::makeActive(Pointer &next) {
	const auto id = DesktopArchiveCrypto::Random(16);
	auto key = DesktopArchiveCrypto::Random(32);
	if (!key) return false;
	const auto clear = gsl::finally([&] { DesktopArchiveCrypto::Clear(*key); });
	if (!id || Exists(archivePath(*id)) || Exists(archivePath(*id, true))) return false;
	const auto wrapped = seal(u"wrap", _scope + "wrap/" + id->toHex(), *key);
	if (!wrapped) return false;
	next.id = *id;
	next.wrapped = *wrapped;
	return true;
}

StoreEffect Store::open() {
	if (_lock || !_options.lease || !_options.lease->live.load()) return { u"key_unavailable"_q };
	const auto hasLogin = _options.loginSecret.size() == 32;
	_bindFailure = bind(hasLogin);
	if (!_bindFailure.isEmpty()) return { _bindFailure };
	if (!hasLogin) return { u"key_unavailable"_q };
	if (const auto pending = cleanup(); !pending.isEmpty()) return { pending };
	if (_pointer.id.isEmpty()) {
		if (_pointer.generation == std::numeric_limits<int64>::max()) return { u"store_unreadable"_q };
		auto next = _pointer;
		++next.generation;
		if (!makeActive(next)) return { u"key_unavailable"_q };
		if (!publish(next)) return { u"store_unreadable"_q };
	}
	auto key = unseal(u"wrap", _scope + "wrap/" + _pointer.id.toHex(), _pointer.wrapped);
	if (!key) return { u"key_unavailable"_q };
	const auto clear = gsl::finally([&] { DesktopArchiveCrypto::Clear(*key); });
	if (const auto failure = openDatabase(*key); !failure.isEmpty()) return { failure };
	_ready = true;
	return {};
}

std::optional<int64> Store::pendingBytes() const {
	if (!_bound || !ownsPointer()) return std::nullopt;
	auto pending = int64(0);
	for (const auto &old : _pointer.retired) {
		const auto live = archivePath(old.id);
		const auto detached = archivePath(old.id, true);
		const auto path = Exists(live) ? live : detached;
		if (!Exists(path)) continue;
		const auto size = DesktopArchivePersistentIdentity(path) == old.directory
			? DesktopArchiveMeasureTree(path) : std::nullopt;
		if ((Exists(live) && Exists(detached)) || !size || *size > std::numeric_limits<int64>::max() - pending) {
			return std::nullopt;
		}
		pending += *size;
	}
	return ownsPointer() ? std::optional(pending) : std::nullopt;
}

std::optional<StorageSummary> Store::storageSummary() {
	const auto active = usage();
	if (!active) return std::nullopt;
	const auto ordinary = int64(_options.policy.totalMiB) * 1024 * 1024;
	const auto timed = int64(_options.policy.timedMiB) * 1024 * 1024;
	const auto overhead = kWalHeadroom + kMetadataHeadroom;
	return StorageSummary{ *active, pendingBytes(), ordinary, timed,
		int64(_options.policy.ordinaryFileMiB) * 1024 * 1024, overhead,
		active->ordinary > ordinary || active->timed > timed || active->physical() > ordinary + timed + overhead };
}

QString Store::cleanup() {
	while (!_pointer.retired.empty()) {
		if (!ownsPointer()) return u"cleanup_blocked"_q;
		const auto old = _pointer.retired.front();
		const auto live = archivePath(old.id);
		const auto detached = archivePath(old.id, true);
		if (Exists(live)) {
			if (Exists(detached) || DesktopArchivePersistentIdentity(live) != old.directory) {
				return u"cleanup_blocked"_q;
			}
			if (!step(Phase::BeforeDetach) || !ownsPointer() || !QDir().rename(live, detached)
				|| !DesktopArchiveSyncDirectory(_root) || !step(Phase::AfterDetach)) {
				return u"cleanup_pending"_q;
			}
		}
		if (Exists(detached) && DesktopArchivePersistentIdentity(detached) != old.directory) {
			return u"cleanup_blocked"_q;
		}
		auto complete = false;
		while (!complete) {
			if (!step(Phase::BeforeUnlink) || !ownsPointer() || !DesktopArchiveRemoveTreePage(detached, 128, complete)
				|| !step(Phase::AfterUnlink)) return u"cleanup_pending"_q;
		}
		auto next = _pointer;
		next.retired.erase(next.retired.begin());
		if (!publish(next)) return u"cleanup_pending"_q;
	}
	return {};
}

StoreEffect Store::recover() {
	if (!_lock) {
		if (const auto failure = bind(false); !failure.isEmpty()) return { failure };
	}
	if (!_lock->isLocked() || !_bound) return { u"store_busy"_q };
	if (!_ready && !_db.CloseFinal()) return { u"cleanup_pending"_q };
	return { cleanup() };
}

Store::ResetCheck Store::reviewReset(const QByteArray &expectedId) const {
	if (!_bound) return _bindFailure.isEmpty() ? u"store_unreadable"_q : _bindFailure;
	if (!ownsPointer()) return u"pointer_changed"_q;
	if (expectedId.size() != 16 || expectedId != _pointer.id) return u"scope_retired"_q;
	const auto path = directory();
	if (!Exists(path)) return ResetReview(expectedId, {}, 0);
	const auto identity = DesktopArchivePersistentIdentity(path);
	const auto bytes = DesktopArchiveMeasureTree(path);
	if (identity.isEmpty() || identity != _pointer.directory || !bytes || DesktopArchivePersistentIdentity(path) != identity
		|| !ownsPointer()) return u"cleanup_blocked"_q;
	return ResetReview(expectedId, identity, *bytes);
}

StoreEffect Store::retire(const QByteArray &expectedId, const ResetReview *review) {
	if (!_bound) return { _bindFailure.isEmpty() ? u"store_unreadable"_q : _bindFailure };
	if (!ownsPointer()) return { u"pointer_changed"_q };
	if ((review && !_options.lease->live.load()) || expectedId.size() != 16 || expectedId != _pointer.id
		|| _pointer.generation == std::numeric_limits<int64>::max()) return { u"scope_retired"_q };
	auto next = _pointer;
	const auto path = directory();
	const auto exists = Exists(path);
	const auto identity = exists ? DesktopArchivePersistentIdentity(path) : QByteArray();
	if (exists && (identity.isEmpty() || identity != _pointer.directory || !DesktopArchivePrivate(path, true))) {
		return { u"cleanup_blocked"_q };
	}
	if (review) {
		const auto bytes = exists ? DesktopArchiveMeasureTree(path) : std::optional<int64>(0);
		if (!bytes || *bytes != review->_bytes || identity != review->_directory
			|| (exists && DesktopArchivePersistentIdentity(path) != identity)) return { u"reset_review_stale"_q };
	}
	if (exists) next.retired.push_back({ _pointer.id, identity });
	++next.generation;
	next.id.clear();
	next.wrapped.clear();
	next.directory.clear();
	next.initialized = false;
	if (review && !makeActive(next)) return { u"key_unavailable"_q };
	if (!review) _options.lease->revoke();
	if (!publish(next)) {
		if (!ownsPointer()) {
			_ready = false;
			_options.lease->revoke();
			_options.lease->crypto.Revoke();
		}
		return { u"store_unreadable"_q };
	}
	_ready = false;
	_options.lease->revoke();
	_options.lease->crypto.Revoke();
	if (!step(Phase::AfterPublish) || !_db.CloseFinal()) return { u"cleanup_pending"_q, true };
	return { cleanup(), true };
}

StoreEffect Store::reset(const ResetReview &review) {
	return retire(review._id, &review);
}

StoreEffect Store::logout(const QByteArray &expectedId) {
	if (!_lock && !_bound) _bindFailure = bind(false);
	return retire(expectedId, nullptr);
}

QByteArray Store::id() const {
	return _pointer.id;
}

bool Store::current() const {
	return _ready && _options.lease->live.load();
}

}
