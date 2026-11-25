#include "purple/purple_archive_previous.h"

#include "purple/purple_archive_crypto.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_archive_lifecycle.h"
#include "purple/purple_sync_json.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDirIterator>
#include <QtCore/QFileInfo>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <algorithm>
#include <limits>
#include <map>
#include <mutex>

namespace Purple {

bool DesktopArchiveLoginKeysValid(const QByteArray &keys);

namespace {

auto &CatalogueMutex = *new std::mutex();
auto &ReleasedLocksMutex = *new std::mutex();
auto &ReleasedLocks = *new std::map<QString, QByteArray>();

bool RecoverReleasedLock(const QString &path) {
	const auto guard = std::lock_guard(ReleasedLocksMutex);
	const auto i = ReleasedLocks.find(path);
	if (i == ReleasedLocks.end()) {
		return true;
	}
	const auto identity = DesktopArchiveIdentity(path);
	if (identity.isEmpty() || identity != i->second) {
		ReleasedLocks.erase(i);
		return true;
	}
	if (!DesktopArchivePrivate(QFileInfo(path).absolutePath(), true)
		|| !QFile::remove(path)
		|| !DesktopArchiveSyncDirectory(QFileInfo(path).absolutePath())) {
		return false;
	}
	ReleasedLocks.erase(i);
	return true;
}

void ReleaseAccountLock(std::unique_ptr<QLockFile> &lock, const QString &path) {
	if (!lock || !lock->isLocked()) {
		lock.reset();
		return;
	}
	const auto identity = DesktopArchiveIdentity(path);
	lock.reset();
	if (!identity.isEmpty() && DesktopArchiveIdentity(path) == identity) {
		const auto guard = std::lock_guard(ReleasedLocksMutex);
		ReleasedLocks[path] = identity;
	}
}

std::optional<QJsonObject> ReadObject(const QString &path) {
	const auto bytes = DesktopArchiveRead(path, 1024 * 1024);
	const auto canonical = bytes ? CanonicalizeSyncJson(*bytes)
								 : SyncJsonResult{{}, SyncJsonErrorKind::Syntax};
	if (!canonical) {
		return {};
	}
	const auto document = QJsonDocument::fromJson(canonical.canonical);
	return document.isObject() ? std::optional(document.object()) : std::nullopt;
}

bool WriteObject(const QString &path, const QJsonObject &object) {
	const auto canonical
		= CanonicalizeSyncJson(QJsonDocument(object).toJson(QJsonDocument::Compact));
	return canonical && DesktopArchiveWrite(path, canonical.canonical);
}

bool Exists(const QString &path) {
	const auto info = QFileInfo(path);
	return info.exists() || info.isSymLink();
}
QString NewToken() {
	const auto random = DesktopArchiveCrypto::Random(16);
	return random ? QString::fromLatin1(random->toHex()) : QString();
}
bool Token(const QString &value) {
	if (value.size() != 32) {
		return false;
	}
	return std::all_of(value.begin(), value.end(), [](QChar ch) {
		return (ch >= u'0' && ch <= u'9') || (ch >= u'a' && ch <= u'f');
	});
}
std::optional<uint64_t> Number(const QJsonValue &value) {
	if (!value.isString()) {
		return {};
	}
	auto ok = false;
	const auto n = value.toString().toULongLong(&ok);
	return ok && QString::number(n) == value.toString() ? std::optional<uint64_t>(n)
														: std::nullopt;
}

struct Layout {
	QString profile;
	QString profileDirectory;
	QString catalogue;
	QString account;
	QString accountDirectory;
	QString root;
	QString store;
	QString intent;
	QString fence;
	QString lock;
	uint64_t generation = 0;
};

QString Marker(const PreviousArchiveOptions &options) {
	const auto suffix
		= options.profileNamespace.isEmpty()
			  ? QString()
			  : '-'
					+ QString::fromLatin1(QCryptographicHash::hash(
						options.profileNamespace.toUtf8(), QCryptographicHash::Sha256)
							.toHex());
	return options.profileRoot + u"/purple-archive-profile"_q + suffix + u".json"_q;
}

QByteArray BindingIdentity(const PreviousArchiveOptions &options) {
	const auto native = DesktopArchivePersistentIdentity(options.profileRoot);
	return native.isEmpty() || options.profileNamespace.isEmpty()
			   ? native
			   : native + ':'
					 + QCryptographicHash::hash(
						 options.profileNamespace.toUtf8(), QCryptographicHash::Sha256)
						   .toHex();
}

bool NoProfileBinding(const PreviousArchiveOptions &options) {
	const auto native = BindingIdentity(options);
	if (native.isEmpty() || !QFileInfo(options.profileRoot).isDir()) {
		return false;
	}
	const auto marker = Marker(options);
	const auto link
		= options.archiveRoot + u"/profile-links/"_q
		  + QString::fromLatin1(
			  QCryptographicHash::hash(native, QCryptographicHash::Sha256).toHex())
		  + u".json"_q;
	return !Exists(marker) && !Exists(link);
}

void ScopeAccount(Layout &layout, const QString &account) {
	layout.account = account;
	layout.accountDirectory = layout.profileDirectory + u"/accounts/"_q + account;
	layout.intent = layout.profileDirectory + u"/intents/"_q + account + u".json"_q;
	layout.fence = layout.profileDirectory + u"/fences/"_q + account + u".json"_q;
	layout.lock = layout.profileDirectory + u"/locks/"_q + account + u".lock"_q;
}

std::optional<Layout> PreviousProfile(const PreviousArchiveOptions &options) {
	const auto native = BindingIdentity(options);
	if (native.isEmpty() || !QFileInfo(options.profileRoot).isDir()
		|| QFileInfo(options.profileRoot).canonicalFilePath() != options.profileRoot
		|| QFileInfo(options.archiveRoot).canonicalFilePath() != options.archiveRoot
		|| !DesktopArchivePrivate(options.archiveRoot, true)) return {};
	const auto links = options.archiveRoot + u"/profile-links"_q;
	if (!DesktopArchivePrivate(links, true)) return {};
	const auto marker = Marker(options);
	const auto link = links + '/' + QString::fromLatin1(
		QCryptographicHash::hash(native, QCryptographicHash::Sha256).toHex()) + u".json"_q;
	const auto object = ReadObject(marker);
	if (!object || object->size() != 3 || object->value(u"version"_q) != QJsonValue(1)
		|| object->value(u"identity"_q).toString().toLatin1() != native
		|| ReadObject(link) != object) return {};
	auto layout = Layout();
	layout.profile = object->value(u"profile"_q).toString();
	if (!Token(layout.profile)) return {};
	layout.profileDirectory = options.archiveRoot + '/' + layout.profile;
	if (!DesktopArchivePrivate(layout.profileDirectory, true)) return {};
	for (const auto &name : {u"accounts"_q, u"intents"_q, u"fences"_q, u"locks"_q}) {
		if (!DesktopArchivePrivate(layout.profileDirectory + '/' + name, true)) return {};
	}
	layout.catalogue = layout.profileDirectory + u"/catalogue.json"_q;
	return BindingIdentity(options) == native ? std::optional(layout) : std::nullopt;
}

std::optional<Layout> PreviousAccount(const PreviousArchiveOptions &options) {
	const auto account = AccountKeyText(options.account.environment, options.account.userId);
	auto layout = PreviousProfile(options);
	if (!account || !layout) return {};
	ScopeAccount(*layout, *account);
	return layout;
}

std::optional<QJsonObject> Catalogue(const Layout &layout, bool create) {
	if (!Exists(layout.catalogue)) {
		return create ? std::optional(QJsonObject{
							{u"version"_q, 1}, {u"accounts"_q, QJsonObject()}})
					  : std::nullopt;
	}
	const auto object = ReadObject(layout.catalogue);
	return object && object->size() == 2
				   && object->value(u"version"_q) == QJsonValue(1)
				   && object->value(u"accounts"_q).isObject()
				   && object->value(u"accounts"_q).toObject().size() <= 1024
			   ? object
			   : std::nullopt;
}

QString DetachedAccountDirectory(const Layout &layout) {
	return layout.accountDirectory + u".discard"_q;
}

QString DiscardIntent(const Layout &layout) {
	return layout.accountDirectory + u".discard.json"_q;
}

bool DiscardPending(const Layout &layout) {
	return Exists(DiscardIntent(layout)) || Exists(DetachedAccountDirectory(layout));
}

QByteArray DiscardStamp(const QString &path) {
	if (!Exists(path)) {
		return QByteArray("absent");
	}
	const auto identity = DesktopArchivePersistentIdentity(path);
	if (identity.isEmpty() || !DesktopArchivePrivate(path, false)) {
		return {};
	}
	const auto info = QFileInfo(path);
	const auto bytes = DesktopArchiveRead(path, 4096);
	return identity + ':' + QByteArray::number(info.size()) + ':'
		+ QByteArray::number(info.lastModified().toMSecsSinceEpoch()) + ':'
		+ (bytes ? QCryptographicHash::hash(*bytes, QCryptographicHash::Sha256) : "opaque");
}

QByteArray DiscardRecordDigest(const QJsonValue &entry) {
	return QCryptographicHash::hash(
		QJsonDocument(QJsonArray{entry}).toJson(QJsonDocument::Compact),
		QCryptographicHash::Sha256);
}

QString DiscardAuthority(const PreviousArchiveOptions &options) {
	return QString::fromLatin1(QCryptographicHash::hash(
		options.loginAuthority, QCryptographicHash::Sha256).toHex());
}

bool ApplyRecord(Layout &layout, const QJsonObject &record) {
	const auto store = record.value(u"store"_q).toString();
	const auto key = record.value(u"key"_q).toString();
	const auto generation = Number(record.value(u"generation"_q));
	if (!Token(store) || !Token(key) || !generation || !*generation
		|| (record.value(u"phase"_q) != QJsonValue(u"creating"_q)
			&& record.value(u"phase"_q) != QJsonValue(u"ready"_q))) {
		return false;
	}
	layout.store = store;
	layout.generation = *generation;
	layout.root = layout.accountDirectory + '/' + store;
	return true;
}

std::optional<ArchiveStoreEpoch> Fence(const Layout &layout) {
	const auto object = ReadObject(layout.fence);
	if (!object || object->value(u"version"_q) != QJsonValue(1)
		|| object->value(u"account"_q) != QJsonValue(layout.account)
		|| !Token(object->value(u"store"_q).toString())
		|| !object->value(u"removing"_q).isBool()) {
		return {};
	}
	const auto generation = Number(object->value(u"generation"_q));
	const auto account = ParseAccountKey(layout.account);
	if (!generation || !*generation || !account) {
		return {};
	}
	return ArchiveStoreEpoch{*account, object->value(u"store"_q).toString(), *generation,
		object->value(u"removing"_q).toBool()};
}

bool WriteFence(const Layout &layout, const ArchiveStoreEpoch &epoch) {
	return WriteObject(layout.fence,
		{{u"version"_q, 1}, {u"account"_q, layout.account}, {u"store"_q, epoch.storeId},
			{u"generation"_q, QString::number(epoch.generation)},
			{u"removing"_q, epoch.removalPending}});
}

struct PendingDiscard {
	QByteArray directory;
	QByteArray record;
	QByteArray fence;
	QByteArray logoutIntent;
	QString authority;
	QByteArray keys;
	ArchiveStoreEpoch replacement;
};

QJsonObject EncodeDiscard(const Layout &layout, const PendingDiscard &job) {
	return {
		{u"version"_q, 1}, {u"profile"_q, layout.profile}, {u"account"_q, layout.account},
		{u"directory"_q, QString::fromLatin1(job.directory)},
		{u"record"_q, QString::fromLatin1(job.record.toHex())},
		{u"fence"_q, QString::fromLatin1(job.fence.toHex())},
		{u"logoutIntent"_q, QString::fromLatin1(job.logoutIntent.toHex())},
		{u"authority"_q, job.authority}, {u"keys"_q, QString::fromLatin1(job.keys)},
		{u"store"_q, job.replacement.storeId},
		{u"generation"_q, QString::number(job.replacement.generation)},
	};
}

std::optional<PendingDiscard> ReadDiscard(const Layout &layout) {
	const auto object = ReadObject(DiscardIntent(layout));
	if (!object || object->size() != 11 || object->value(u"version"_q) != QJsonValue(1)
		|| object->value(u"profile"_q) != QJsonValue(layout.profile)
		|| object->value(u"account"_q) != QJsonValue(layout.account)) {
		return {};
	}
	const auto text = [&](const QString &key) { return object->value(key).toString(); };
	const auto account = ParseAccountKey(layout.account);
	const auto generation = Number(object->value(u"generation"_q));
	auto result = PendingDiscard{
		text(u"directory"_q).toLatin1(), QByteArray::fromHex(text(u"record"_q).toLatin1()),
		QByteArray::fromHex(text(u"fence"_q).toLatin1()),
		QByteArray::fromHex(text(u"logoutIntent"_q).toLatin1()),
		text(u"authority"_q), text(u"keys"_q).toLatin1(),
		{account.value_or(AccountKey()), text(u"store"_q), generation.value_or(0), true},
	};
	return account && generation && *generation && Token(result.replacement.storeId)
		&& result.record.size() == 32 && !result.fence.isEmpty() && !result.logoutIntent.isEmpty()
		&& IsArchiveHash(result.authority) && DesktopArchiveLoginKeysValid(result.keys)
		&& EncodeDiscard(layout, result) == *object ? std::optional(result) : std::nullopt;
}

// The caller owns the account lock. Catalogues are shared across accounts,
// so release the catalogue lock before removing the detached data.
PreviousArchiveResult FinishDiscard(const Layout &layout,
	const PreviousArchiveOptions &options, const PreviousArchiveReview::Token *token,
	bool confirmed = false) {
	const auto detached = DetachedAccountDirectory(layout);
	const auto intent = DiscardIntent(layout);
	{
		const auto serial = std::lock_guard(CatalogueMutex);
		auto catalogueLock = QLockFile(layout.profileDirectory + u"/catalogue.lock"_q);
		catalogueLock.setStaleLockTime(0);
		if (!catalogueLock.tryLock(0)) {
			return {u"discard-catalogue-lock-unavailable"_q, true};
		}
		auto catalogue = Catalogue(layout, true);
		if (!catalogue) {
			return {u"discard-catalogue-unreadable"_q};
		}
		auto accounts = catalogue->value(u"accounts"_q).toObject();
		const auto entry = accounts.value(layout.account);
		const auto digest = DiscardRecordDigest(entry);
		const auto fence = Fence(layout);
		const auto fenceStamp = DiscardStamp(layout.fence);
		const auto logoutStamp = DiscardStamp(layout.intent);
		const auto live = Exists(layout.accountDirectory);
		const auto moved = Exists(detached);
		if ((live && !DesktopArchivePrivate(layout.accountDirectory, true))
			|| (moved && !DesktopArchivePrivate(detached, true))
			|| (live && moved) || fenceStamp.isEmpty() || logoutStamp.isEmpty()) {
			return {u"discard-account-ownership-unverified"_q};
		}
		const auto directory = DesktopArchivePersistentIdentity(live ? layout.accountDirectory : detached);
		if ((live || moved) && directory.isEmpty()) {
			return {u"discard-directory-identity-unavailable"_q};
		}
		auto job = std::optional<PendingDiscard>();
		if (Exists(intent)) {
			job = ReadDiscard(layout);
			if (!job) {
				return {u"discard-intent-unreadable"_q};
			}
		} else if (token) {
			if (moved || DesktopArchiveIdentity(layout.accountDirectory) != token->directoryIdentity
				|| digest != token->recordDigest || fenceStamp != token->fenceStamp
				|| logoutStamp != token->intentStamp) {
				return {u"discard-archive-changed"_q};
			}
			if (!live && !fence && !Exists(layout.fence)
				&& logoutStamp == "absent" && entry.isUndefined()) {
				return {};
			}
			const auto generation = std::max(fence ? fence->generation : uint64_t(0),
				Number(entry.toObject().value(u"generation"_q)).value_or(0));
			const auto store = NewToken();
			if (store.isEmpty()) {
				return {u"discard-fence-token-unavailable"_q};
			}
			job = PendingDiscard{directory, digest, fenceStamp, logoutStamp,
				token->authority, token->keys, {options.account, store,
					generation == std::numeric_limits<uint64_t>::max() ? uint64_t(1) : generation + 1, true}};
			if (options.active && !options.active()) {
				return {u"discard-account-changed"_q};
			}
			if (!WriteObject(intent, EncodeDiscard(layout, *job))) {
				return {u"discard-intent-write-failed"_q};
			}
		} else {
			return {u"discard-intent-missing"_q};
		}

		const auto fenced = fence && *fence == job->replacement;
		if ((live || moved) && directory != job->directory) {
			return {u"discard-directory-changed"_q};
		}
		if (!live && !moved && !job->directory.isEmpty()
			&& !(fenced && entry.isUndefined() && logoutStamp == "absent")) {
			return {u"discard-directory-missing"_q};
		}
		if ((digest != job->record && !(fenced && entry.isUndefined()))
			|| (fenceStamp != job->fence && !fenced)
			|| (logoutStamp != job->logoutIntent && !(fenced && logoutStamp == "absent"))
			|| (live && fenced)) {
			return {u"discard-metadata-changed"_q};
		}
		if (live) {
			if (!confirmed && ((options.active && !options.active()) || DiscardAuthority(options) != job->authority
				|| options.loginAuthKeyIds != job->keys)) {
				return {u"discard-login-changed-before-detach"_q};
			}
			if (!QDir().rename(layout.accountDirectory, detached)) {
				return {u"discard-detach-failed"_q};
			}
		}
		if (!DesktopArchiveSyncDirectory(QFileInfo(detached).absolutePath())) {
			return {u"discard-detach-sync-failed"_q};
		}
		if (!confirmed && options.active && !options.active()) {
			return {u"cleanup_pending"_q, true};
		}
		if (!fenced && !WriteFence(layout, job->replacement)) {
			return {u"discard-fence-write-failed"_q};
		}
		if (!DesktopArchiveRemoveFile(layout.intent)) {
			return {u"discard-intent-remove-failed"_q};
		}
		if (!entry.isUndefined()) {
			accounts.remove(layout.account);
			catalogue->insert(u"accounts"_q, accounts);
			if (!WriteObject(layout.catalogue, *catalogue)) {
				return {u"discard-catalogue-write-failed"_q};
			}
		}
	}
	auto complete = false;
	while (!complete) {
		if (!DesktopArchiveRemoveTreePage(detached, 128, complete)) {
			return {u"discard-detached-cleanup-failed"_q, true};
		}
	}
	return DesktopArchiveRemoveFile(intent)
		? PreviousArchiveResult{} : PreviousArchiveResult{u"discard-marker-remove-failed"_q, true};
}

bool RecoverConfirmedDiscards(const Layout &profile, const PreviousArchiveOptions &options) {
	const auto suffix = u".discard.json"_q;
	auto entries = QDirIterator(profile.profileDirectory + u"/accounts"_q,
		QStringList{u"*"_q + suffix}, QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks);
	auto retry = false;
	while (entries.hasNext() && (!options.active || options.active())) {
		entries.next();
		const auto name = entries.fileName().chopped(suffix.size());
		const auto account = ParseAccountKey(name);
		if (!account || AccountKeyText(account->environment, account->userId) != name) {
			continue;
		}
		auto layout = profile;
		ScopeAccount(layout, name);
		auto lock = std::make_unique<QLockFile>(layout.lock);
		const auto release = gsl::finally([&] { ReleaseAccountLock(lock, layout.lock); });
		lock->setStaleLockTime(0);
		if (!RecoverReleasedLock(layout.lock) || !lock->tryLock(0)) {
			retry = true;
			continue;
		}
		if (Exists(DiscardIntent(layout))) {
			// A recorded confirmation authorizes only the exact identities
			// in that job. It remains valid after the account logs out.
			auto target = PreviousArchiveOptions();
			target.account = *account;
			retry = FinishDiscard(layout, target, nullptr, true).retryable || retry;
		}
	}
	return retry;
}

}

bool DesktopArchiveLoginKeysValid(const QByteArray &keys) {
	const auto parts = keys.split(',');
	if (parts.empty() || parts.size() > 128) {
		return false;
	}
	auto previous = QByteArray();
	for (const auto &part : parts) {
		if (part.size() != 16 || part == QByteArray(16, '0')
			|| (!previous.isEmpty() && previous >= part)
			|| !std::all_of(part.begin(), part.end(), [](char ch) {
				return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
			})) {
			return false;
		}
		previous = part;
	}
	return true;
}

bool DesktopArchiveLoginKeysOverlap(const QByteArray &a, const QByteArray &b) {
	if (!DesktopArchiveLoginKeysValid(a) || !DesktopArchiveLoginKeysValid(b)) {
		return false;
	}
	const auto other = b.split(',');
	for (const auto &key : a.split(',')) {
		if (other.contains(key)) {
			return true;
		}
	}
	return false;
}

PreviousArchivePreparation PreviousArchive::ReviewPreviousArchive(const PreviousArchiveOptions &options) {
	if (options.active && !options.active()) return {{}, u"discard-account-changed"_q};
	if (!Exists(options.archiveRoot) && NoProfileBinding(options)) return {};
	auto layout = PreviousAccount(options);
	if (!layout) return {{}, u"discard-profile-binding-unavailable"_q};
	if (DiscardPending(*layout)) return {{}, u"cleanup_pending"_q};
	if (!Exists(layout->accountDirectory)) return {};
	if (options.loginAuthority.size() != 32 || !DesktopArchiveLoginKeysValid(options.loginAuthKeyIds)) {
		return {{}, u"discard-login-authority-unavailable"_q};
	}
	const auto directory = DesktopArchiveIdentity(layout->accountDirectory);
	const auto profile = DesktopArchiveIdentity(options.profileRoot);
	const auto catalogue = Catalogue(*layout, false);
	const auto entry = catalogue
		? catalogue->value(u"accounts"_q).toObject().value(layout->account) : QJsonValue();
	const auto bound = Fence(*layout);
	if (!entry.isObject() || !ApplyRecord(*layout, entry.toObject()) || !bound
		|| bound->storeId != layout->store || bound->generation != layout->generation
		|| entry.toObject().value(u"loginAuthority"_q).toString() != DiscardAuthority(options)
		|| !DesktopArchiveLoginKeysOverlap(options.loginAuthKeyIds,
			entry.toObject().value(u"loginAuthKeys"_q).toString().toLatin1())
		|| !DesktopArchivePrivate(layout->root, true)
		|| QDir(layout->accountDirectory).entryList(
			QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System) != QStringList{layout->store}) {
		return {{}, u"discard-account-ownership-unverified"_q};
	}
	const auto fence = DiscardStamp(layout->fence);
	const auto logout = DiscardStamp(layout->intent);
	const auto size = DesktopArchiveMeasureTree(layout->accountDirectory);
	if (directory.isEmpty() || profile.isEmpty() || !catalogue || fence.isEmpty() || logout.isEmpty()
		|| !size || DesktopArchiveIdentity(layout->accountDirectory) != directory
		|| DesktopArchiveIdentity(options.profileRoot) != profile) {
		return {{}, u"discard-account-ownership-unverified"_q};
	}
	auto result = PreviousArchiveReview();
	result._bytes = *size;
	result._token = { options.account, layout->profile, profile, directory,
		DiscardRecordDigest(entry),
		fence, logout, DiscardAuthority(options), options.loginAuthKeyIds };
	return { std::move(result), {} };
}

PreviousArchiveResult PreviousArchive::DeletePreviousArchive(
		const PreviousArchiveOptions &options, const PreviousArchiveReview &review) {
	const auto &token = review._token;
	if (options.account != token.account || options.loginAuthority.size() != 32
		|| options.loginAuthKeyIds != token.keys || DiscardAuthority(options) != token.authority
		|| (options.active && !options.active())) return { u"discard-account-changed"_q };
	const auto layout = PreviousAccount(options);
	if (!layout || layout->profile != token.profile
		|| DesktopArchiveIdentity(options.profileRoot) != token.profileIdentity) return { u"discard-profile-changed"_q };
	auto profileLock = QLockFile(options.archiveRoot + u"/profile.lock"_q);
	profileLock.setStaleLockTime(0);
	if (!profileLock.tryLock(0)) return { u"discard-profile-lock-unavailable"_q, true };
	auto lock = std::make_unique<QLockFile>(layout->lock);
	const auto release = gsl::finally([&] { ReleaseAccountLock(lock, layout->lock); });
	lock->setStaleLockTime(0);
	if (!RecoverReleasedLock(layout->lock) || !lock->tryLock(0)) return { u"discard-account-lock-unavailable"_q, true };
	const auto verified = PreviousAccount(options);
	if (!verified || verified->profile != layout->profile
		|| DesktopArchiveIdentity(options.profileRoot) != token.profileIdentity) return { u"discard-profile-changed"_q };
	profileLock.unlock();
	if (DiscardPending(*layout) || DesktopArchiveIdentity(layout->accountDirectory) != token.directoryIdentity
		|| DesktopArchiveMeasureTree(layout->accountDirectory) != review._bytes) return { u"discard-archive-changed"_q };
	return FinishDiscard(*layout, options, &token);
}

bool PreviousArchive::RetryPreviousArchiveDeletions(const PreviousArchiveOptions &profile) {
	// Only a completed profile binding and a previously confirmed on-disk
	// intent authorize recovery. Startup never prepares a deletion.
	const auto layout = PreviousProfile(profile);
	return layout && RecoverConfirmedDiscards(*layout, profile);
}

}
