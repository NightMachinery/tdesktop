#include "purple/purple_desktop_archive_store.h"
#include "purple/purple_archive_stream.h"
#include "purple/purple_archive_attention.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_archive_media_lifecycle.h"
#include "purple/purple_sync_json.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFileInfo>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <set>

namespace Purple {
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

template <typename T>
const T* Value(const std::vector<ArchiveSqlValue> &row, size_t index) {
	return index < row.size() ? std::get_if<T>(&row[index]) : nullptr;
}

struct Layout {
	QString profile;
	QString profileDirectory;
	QString catalogue;
	QString account;
	QString accountDirectory;
	QString root;
	QString store;
	QString key;
	QString intent;
	QString fence;
	QString lock;
	uint64_t generation = 0;
	QJsonObject record;
	std::optional<QJsonValue> discardRecord;
};

QString Marker(const DesktopArchiveOptions &options) {
	const auto suffix
		= options.profileNamespace.isEmpty()
			  ? QString()
			  : '-'
					+ QString::fromLatin1(QCryptographicHash::hash(
						options.profileNamespace.toUtf8(), QCryptographicHash::Sha256)
							.toHex());
	return options.profileRoot + u"/purple-archive-profile"_q + suffix + u".json"_q;
}

QByteArray BindingIdentity(const DesktopArchiveOptions &options) {
	const auto native = DesktopArchiveIdentity(options.profileRoot);
	return native.isEmpty() || options.profileNamespace.isEmpty()
			   ? native
			   : native + ':'
					 + QCryptographicHash::hash(
						 options.profileNamespace.toUtf8(), QCryptographicHash::Sha256)
						   .toHex();
}

bool NoProfileBinding(const DesktopArchiveOptions &options) {
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

std::optional<Layout> Profile(const DesktopArchiveOptions &options, bool create) {
	const auto serial = std::lock_guard(CatalogueMutex);
	const auto account
		= AccountKeyText(options.account.environment, options.account.userId);
	const auto native = BindingIdentity(options);
	if (!account || native.isEmpty() || !QFileInfo(options.profileRoot).isDir()) {
		return {};
	}
	if (!Exists(options.archiveRoot) && !create) {
		return {};
	}
	if (!DesktopArchiveDirectory(options.archiveRoot)) {
		return {};
	}
	auto global = QLockFile(options.archiveRoot + u"/profile.lock"_q);
	global.setStaleLockTime(0);
	if (!global.tryLock(0)) {
		return {};
	}
	const auto links = options.archiveRoot + u"/profile-links"_q;
	if (!DesktopArchiveDirectory(links)) {
		return {};
	}
	const auto marker = Marker(options);
	const auto link
		= links + '/'
		  + QString::fromLatin1(
			  QCryptographicHash::hash(native, QCryptographicHash::Sha256).toHex())
		  + u".json"_q;
	auto identity = Exists(marker) ? ReadObject(marker)
					: Exists(link) ? ReadObject(link)
								   : std::optional<QJsonObject>();
	if (!identity) {
		if (!create || Exists(marker) || Exists(link)) {
			return {};
		}
		const auto token = NewToken();
		if (token.isEmpty()) {
			return {};
		}
		identity = QJsonObject{{u"version"_q, 1}, {u"profile"_q, token},
			{u"identity"_q, QString::fromLatin1(native)}};
	}
	if (identity->value(u"version"_q) != QJsonValue(1)
		|| !Token(identity->value(u"profile"_q).toString())
		|| identity->value(u"identity"_q).toString().toLatin1() != native) {
		return {};
	}
	if (Exists(link)) {
		const auto old = ReadObject(link);
		if (!old || *old != *identity) {
			return {};
		}
	} else if (!WriteObject(link, *identity)) {
		return {};
	}
	if (Exists(marker)) {
		const auto old = ReadObject(marker);
		if (!old || *old != *identity) {
			return {};
		}
	} else if (!WriteObject(marker, *identity)) {
		return {};
	}
	auto result = Layout();
	result.profile = identity->value(u"profile"_q).toString();
	result.profileDirectory
		= QFileInfo(options.archiveRoot).canonicalFilePath() + '/' + result.profile;
	if (!Exists(result.profileDirectory) && !create) {
		return {};
	}
	if (!DesktopArchiveDirectory(result.profileDirectory)) {
		return {};
	}
	for (const auto &name : {u"accounts"_q, u"intents"_q, u"fences"_q, u"locks"_q}) {
		if (!DesktopArchiveDirectory(result.profileDirectory + '/' + name)) {
			return {};
		}
	}
	result.catalogue = result.profileDirectory + u"/catalogue.json"_q;
	result.account = *account;
	result.accountDirectory = result.profileDirectory + u"/accounts/"_q + *account;
	result.intent = result.profileDirectory + u"/intents/"_q + *account + u".json"_q;
	result.fence = result.profileDirectory + u"/fences/"_q + *account + u".json"_q;
	result.lock = result.profileDirectory + u"/locks/"_q + *account + u".lock"_q;
	return result;
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

bool AdmissionShape(const QJsonObject &record) {
	const auto fields = {u"store"_q, u"key"_q, u"generation"_q, u"phase"_q,
		u"slot"_q, u"loginAuthority"_q, u"loginAuthorityVersion"_q,
		u"loginAdmissionMac"_q, u"loginAuthKeyId"_q,
		u"loginAuthKeys"_q, u"wrappedKey"_q};
	if (record.size() != fields.size()) {
		return false;
	}
	for (const auto &field : fields) {
		if (!record.contains(field)) {
			return false;
		}
	}
	const auto keyId = record.value(u"loginAuthKeyId"_q).toString().toLatin1();
	const auto generation = Number(record.value(u"generation"_q));
	return Token(record.value(u"store"_q).toString())
		&& Token(record.value(u"key"_q).toString())
		&& generation && *generation
		&& (record.value(u"phase"_q) == QJsonValue(u"creating"_q)
			|| record.value(u"phase"_q) == QJsonValue(u"ready"_q))
		&& record.value(u"slot"_q).isString()
		&& Number(record.value(u"slot"_q)).has_value()
		&& IsArchiveHash(record.value(u"loginAuthority"_q).toString())
		&& record.value(u"loginAuthorityVersion"_q) == QJsonValue(3)
		&& IsArchiveHash(record.value(u"loginAdmissionMac"_q).toString())
		&& DesktopArchiveLoginKeysValid(record.value(u"loginAuthKeys"_q).toString().toLatin1())
		&& record.value(u"loginAuthKeys"_q).toString().toLatin1().split(',').contains(keyId)
		&& (record.value(u"wrappedKey"_q) == QJsonValue(u"initial"_q)
			|| Token(record.value(u"wrappedKey"_q).toString()))
		&& keyId.size() == 16 && keyId != QByteArray(16, '0')
		&& std::all_of(keyId.begin(), keyId.end(), [](char ch) {
			return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
		});
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
	layout.record = record;
	layout.store = store;
	layout.key = key;
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

bool UpdateRecord(const Layout &layout, const std::optional<QJsonObject> &record) {
	const auto serial = std::lock_guard(CatalogueMutex);
	auto lock = QLockFile(layout.profileDirectory + u"/catalogue.lock"_q);
	lock.setStaleLockTime(0);
	if (!lock.tryLock(0)) {
		return false;
	}
	auto catalogue = Catalogue(layout, true);
	if (!catalogue) {
		return false;
	}
	auto accounts = catalogue->value(u"accounts"_q).toObject();
	const auto old = accounts.value(layout.account);
	if (layout.discardRecord && (record || old != *layout.discardRecord)) {
		return false;
	}
	if (!layout.discardRecord && !old.isUndefined()
		&& (!old.isObject()
			|| old.toObject().value(u"store"_q) != QJsonValue(layout.store))) {
		return false;
	}
	if (record) {
		accounts.insert(layout.account, *record);
	} else {
		accounts.remove(layout.account);
	}
	catalogue->insert(u"accounts"_q, accounts);
	return WriteObject(layout.catalogue, *catalogue);
}

bool PrivateDatabaseFiles(const Layout &layout) {
	for (const auto &suffix : {QString(), u"-wal"_q, u"-shm"_q, u"-journal"_q}) {
		const auto path = layout.root + u"/archive.sqlite"_q + suffix;
		if (Exists(path)
			&& (!DesktopArchivePrivate(path, false)
				|| !DesktopArchiveExcludeBackup(path))) {
			return false;
		}
	}
	return DesktopArchiveSyncDirectory(layout.root);
}

bool RunRemoval(Layout layout, ArchiveRemoval removal, DesktopArchiveSqlite* database,
	int maximumActions, uint64_t readers = 0,
	const std::function<bool()> &allowed = {}) {
	if (removal.epoch.account != *ParseAccountKey(layout.account)
		|| removal.epoch.storeId != layout.store || maximumActions < 1
		|| maximumActions > 1000000) {
		return false;
	}
	for (auto count = 0;
		count != maximumActions && removal.action != ArchiveRemovalAction::Done;
		++count) {
		if (allowed && !allowed()) {
			return false;
		}
		auto ok = false;
		switch (removal.action) {
		case ArchiveRemovalAction::WriteTombstone: {
			const auto bytes = EncodeArchiveRemovalTombstone(removal);
			ok = bytes && DesktopArchiveWrite(layout.intent, *bytes);
		} break;
		case ArchiveRemovalAction::FenceWork: {
			const auto old = Exists(layout.fence) ? Fence(layout)
												  : std::optional<ArchiveStoreEpoch>();
			ok = (!Exists(layout.fence) || old.has_value())
				 && (!old
					 || (old->account == removal.epoch.account
						 && old->storeId == removal.epoch.storeId
						 && old->generation <= removal.epoch.generation))
				 && WriteFence(layout, removal.epoch);
		} break;
		case ArchiveRemovalAction::DrainReadersAndClose:
			ok = !readers && (!database || database->Close());
			break;
		case ArchiveRemovalAction::DeleteDatabase:
			ok = DesktopArchiveRemoveFile(layout.root + u"/archive.sqlite"_q);
			break;
		case ArchiveRemovalAction::DeleteWal:
			ok = DesktopArchiveRemoveFile(layout.root + u"/archive.sqlite-wal"_q);
			break;
		case ArchiveRemovalAction::DeleteShm:
			ok = DesktopArchiveRemoveFile(layout.root + u"/archive.sqlite-shm"_q);
			break;
		case ArchiveRemovalAction::DeleteRollbackJournal:
			ok = DesktopArchiveRemoveFile(layout.root + u"/archive.sqlite-journal"_q);
			break;
		case ArchiveRemovalAction::DeleteBlobDirectory:
		case ArchiveRemovalAction::DeleteStagingDirectory: {
			auto complete = false;
			ok = DesktopArchiveRemoveTreePage(
				layout.root
					+ (removal.action == ArchiveRemovalAction::DeleteBlobDirectory
							? u"/blobs"_q
							: u"/staging"_q),
				128, complete);
			if (ok && !complete) {
				continue;
			}
		} break;
		case ArchiveRemovalAction::DeleteWrappedKey:
			ok = DesktopArchiveRemoveFile(layout.root + u"/wrapped-key"_q);
			break;
		case ArchiveRemovalAction::DeleteOwnedDirectoryAndBinding: {
			auto complete = false;
			ok = DesktopArchiveRemoveTreePage(layout.root, 128, complete);
			if (ok && !complete) {
				continue;
			}
			ok = ok && UpdateRecord(layout, std::nullopt);
		} break;
		case ArchiveRemovalAction::ClearTombstone:
			ok = DesktopArchiveRemoveFile(layout.intent);
			break;
		case ArchiveRemovalAction::Done:
			ok = true;
			break;
		}
		const auto next = AdvanceArchiveRemoval(
			removal, {removal.epoch.account, removal.epoch.storeId, removal.operationId,
						 removal.epoch.generation, removal.action, ok, ok, readers, 0,
						 !database || !database->usable()});
		if (next.status != ArchiveRemovalStatus::Advanced) {
			return false;
		}
		removal = next.state;
	}
	return removal.action == ArchiveRemovalAction::Done;
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

std::optional<DesktopArchiveLoginAuthority> DesktopArchiveSelectLoginAuthority(
	const QByteArray &nonce, const QByteArray &storedKeys, const QByteArray &liveKeys) {
	if (!DesktopArchiveLoginKeysValid(liveKeys)) {
		return {};
	}
	if (nonce.size() == 32 && DesktopArchiveLoginKeysOverlap(storedKeys, liveKeys)) {
		return DesktopArchiveLoginAuthority{nonce, liveKeys};
	}
	const auto fresh = DesktopArchiveCrypto::Random(32);
	return fresh ? std::optional(DesktopArchiveLoginAuthority{*fresh, liveKeys}) : std::nullopt;
}

class DesktopArchiveStore::Impl {
  public:
	DesktopArchiveOptions options;
	Layout layout;
	std::unique_ptr<QLockFile> lock;
	DesktopArchiveSqlite database;
	DesktopArchiveCrypto crypto;
	ArchiveSqlSecurity security;
	ArchiveStoreEpoch epoch;
	DesktopArchiveStatus status = DesktopArchiveStatus::Disabled;
	QString failure;
	QByteArray native;
	QByteArray profileNative;
	QByteArray databaseNative;
	QString loginAuthority;
	std::unique_ptr<QDirIterator> usageIterator;
	ArchiveUsage usage;
	bool usageComplete = false;
	uint64_t readers = 0;
	std::map<QString, DesktopArchiveMediaHandle> mediaHandles;
	struct HistoryCapability {
		ArchiveMessageKey key;
		ArchiveStoreEpoch epoch;
		ArchiveGeneration generation;
		std::shared_ptr<std::atomic<bool>> active;
		std::function<void()> revoked;
	};
	std::map<QString, HistoryCapability> histories;
	std::map<QString, std::unique_ptr<DesktopArchiveDeletedQuery>> deletedViews;
	bool refreshingMediaReaders = false;
	struct StreamEntry {
		std::shared_ptr<DesktopArchiveStream> stream;
		std::shared_ptr<std::atomic<bool>> active;
	};
	std::map<QString, StreamEntry> streams;
	std::shared_ptr<std::atomic<bool>> journalAdmission;
	uint64_t journalGeneration = 0;
	QJsonObject journalDeleteGenerations;
	struct JournalRenewal {
		ArchiveStoreEpoch epoch;
		uint64_t accountGeneration = 0;
		QJsonObject deleteGenerations;
		int attempts = 0;
		bool held = false;
		bool candidate = false;
	};
	std::optional<JournalRenewal> journalRenewal;
	QString admissionFailure;
	void RevokeJournalAdmission() {
		if (journalAdmission) {
			journalAdmission->store(false);
			journalAdmission.reset();
		}
	}
	struct StagingCounters {
		std::atomic<uint64_t> fenceReads = 0;
		std::atomic<uint64_t> proofQueries = 0;
		std::atomic<uint64_t> catalogueOpens = 0;
	};
	std::shared_ptr<StagingCounters> stagingCounters = std::make_shared<StagingCounters>();
	bool MemoryCurrent() const {
		return alive->load() && (!options.active || options.active())
			&& status == DesktopArchiveStatus::Ready && lock && lock->isLocked()
			&& database.usable();
	}
	bool recoveringMediaLeases = false;
	struct MediaRetirement {
		ArchiveMediaSource source;
		QString blob;
		uint64_t size = 0;
		std::weak_ptr<std::atomic<bool>> active;
	};
	std::map<QString, MediaRetirement> retiredMedia;
	std::set<QString> purgeReaders;
	bool maintenancePending = false;
	bool garbagePending = true;
	bool garbageHeld = false;
	QString cancelledJobCursor;
	bool cancelledJobsComplete = false;
	std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(false);
	bool Authorized() const {
		++stagingCounters->fenceReads;
		const auto fence = Fence(layout);
		return alive->load() && (!options.active || options.active())
			   && lock && lock->isLocked()
			   && database.usable() && DesktopArchiveIdentity(layout.root) == native
			   && DesktopArchiveIdentity(options.profileRoot) == profileNative
			   && DesktopArchiveIdentity(layout.root + u"/archive.sqlite"_q)
					  == databaseNative
			   && fence && CanRunArchiveWork(epoch, *fence) && !Exists(layout.intent);
	}
	bool Current() const { return status == DesktopArchiveStatus::Ready && Authorized(); }
	bool Fail(DesktopArchiveStatus reason, const QString &text) {
		status = reason;
		failure = text;
		return false;
	}
	std::optional<ArchiveStoredRow> Decode(ArchiveRecordKind kind, const QString &id,
		const QByteArray &frame, int64 position = 0) {
		return DesktopArchiveDecodeRow(database, options.account, security, kind, id, frame, position);
	}
	std::optional<ArchiveStoredRow> ChatEpoch(const ArchiveMessageKey &key, bool &valid) {
		valid = false;
		if (key.account != options.account) {
			return {};
		}
		const auto read = database.Read(ArchiveSqlReadChatEpoch(key.account, key.chat, security));
		if (!read.ok || read.rows.size() > 1) {
			return {};
		}
		if (read.rows.empty()) {
			valid = true;
			return {};
		}
		const auto &values = read.rows.front();
		const auto projection = Value<QString>(values, 0);
		const auto frame = Value<QByteArray>(values, 1);
		if (values.size() != 2 || !frame || !projection) {
			return {};
		}
		const auto row = Decode(ArchiveRecordKind::Generation,
			ArchiveSqlChatId(key.chat, key.account, security), *frame);
		if (!row) {
			return {};
		}
		const auto &generation = std::get<ArchiveChatGeneration>(row->row.value);
		valid = generation.chat == key.chat && QString::number(generation.generation) == *projection;
		return valid ? row : std::nullopt;
	}
	ArchiveSqlStatus Admit() {
		if (!database.BeginRead()) {
			return ArchiveSqlStatus::Invalid;
		}
		const auto status = DesktopArchiveAdmitSnapshot(database);
		return database.EndRead() ? status : ArchiveSqlStatus::Invalid;
	}
	std::optional<QString> AdmissionMac(QJsonObject record) const {
		record.remove(u"loginAdmissionMac"_q);
		const auto canonical = CanonicalizeSyncJson(
			QJsonDocument(record).toJson(QJsonDocument::Compact));
		const auto mac = canonical
			? crypto.indexHmac(u"admission-v1"_q)(canonical.canonical) : std::nullopt;
		return mac ? std::optional(QString::fromLatin1(mac->toHex())) : std::nullopt;
	}
	bool SealAdmission(QJsonObject &record) const {
		const auto mac = AdmissionMac(record);
		if (!mac) {
			return false;
		}
		record.insert(u"loginAdmissionMac"_q, *mac);
		return AdmissionShape(record);
	}
	bool RequireAdmission() {
		const auto admission = Admit();
		if (admission == ArchiveSqlStatus::Ready) {
			return true;
		}
		return Fail(admission == ArchiveSqlStatus::ReadOnly
						? DesktopArchiveStatus::ReadOnly
					: admission == ArchiveSqlStatus::SchemaMismatch
						? DesktopArchiveStatus::SchemaMismatch
						: DesktopArchiveStatus::Unreadable,
			admission == ArchiveSqlStatus::ReadOnly			? u"schema-read-only"_q
			: admission == ArchiveSqlStatus::SchemaMismatch ? u"schema-mismatch"_q
															: u"schema-unreadable"_q);
	}
	std::optional<ArchiveStoredRow> Header(bool *readFailure = nullptr) {
		return DesktopArchiveReadHeader(database, options.account, security, {}, readFailure);
	}

};

DesktopArchiveStore::DesktopArchiveStore() : _impl(std::make_unique<Impl>()) {}
DesktopArchiveStore::~DesktopArchiveStore() {
	_impl->RevokeJournalAdmission();
	_impl->streams.clear();
	ExpireHistory(true);
	const auto readersClosed = ExpireDeletedViews(true);
	_impl->crypto.Revoke();
	if (_impl->database.CloseFinal() && readersClosed) {
		ReleaseAccountLock(_impl->lock, _impl->layout.lock);
	}
}

DesktopArchiveStatus DesktopArchiveStore::Open(DesktopArchiveOptions options) {
	auto &self = *_impl;
	const auto clearKey = std::unique_ptr<QByteArray, void (*)(QByteArray*)>(
		&options.localKey, [](QByteArray* key) { DesktopArchiveCrypto::Clear(*key); });
	const auto clearAuthority = std::unique_ptr<QByteArray, void (*)(QByteArray*)>(
		&options.loginAuthority, [](QByteArray* key) { DesktopArchiveCrypto::Clear(*key); });
	if (options.revocableSecrets) {
		self.crypto = *options.revocableSecrets;
	}
	const auto authority = QString::fromLatin1(QCryptographicHash::hash(
		options.loginAuthority, QCryptographicHash::Sha256).toHex());
	if (self.lock || (options.active && !options.active())
		|| options.localKey.isEmpty() || options.loginAuthority.size() != 32
		|| !DesktopArchiveLoginKeysValid(options.loginAuthKeyIds)
		|| !options.loginAuthKeyIds.split(',').contains(options.loginAuthKeyId)
		|| options.loginAuthKeyId.size() != 16
		|| options.loginAuthKeyId == QByteArray(16, '0')
		|| !std::all_of(options.loginAuthKeyId.begin(), options.loginAuthKeyId.end(), [](char ch) {
			return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
		})
		|| !AccountKeyText(options.account.environment, options.account.userId)) {
		self.Fail(DesktopArchiveStatus::Unreadable, u"key-or-account-unavailable"_q);
		return self.status;
	}
	self.failure.clear();
	self.admissionFailure.clear();
	self.journalRenewal.reset();
	self.maintenancePending = false;
	self.garbagePending = true;
	self.garbageHeld = false;
	self.cancelledJobCursor.clear();
	self.cancelledJobsComplete = false;
	self.loginAuthority = authority;
	self.database.SetActive(options.active);
	self.alive = std::make_shared<std::atomic<bool>>(true);
	self.options = options;
	self.profileNative = DesktopArchiveIdentity(options.profileRoot);
	DesktopArchiveCrypto::Clear(self.options.localKey);
	DesktopArchiveCrypto::Clear(self.options.loginAuthority);
	self.options.authorityReady = {};
	const auto profile = Profile(options, options.create);
	if (!profile) {
		self.Fail(DesktopArchiveStatus::Invalid, u"profile-binding"_q);
		return self.status;
	}
	if (options.active && !options.active()) {
		self.Fail(DesktopArchiveStatus::Stale, u"account-revoked"_q);
		return self.status;
	}
	self.layout = *profile;
	self.lock = std::make_unique<QLockFile>(self.layout.lock);
	self.lock->setStaleLockTime(0);
	if (!RecoverReleasedLock(self.layout.lock) || !self.lock->tryLock(0)) {
		self.lock.reset();
		self.Fail(DesktopArchiveStatus::Busy, u"account-lock"_q);
		return self.status;
	}
	if (Exists(self.layout.intent)) {
		self.Fail(DesktopArchiveStatus::RemovalPending, u"removal-pending"_q);
		return self.status;
	}
	auto admissionPlan = ArchiveKeyDerivationFor(options.account,
		self.layout.profile, self.layout.profile, ArchiveKeyPurpose::DesktopWrapping);
	if (admissionPlan) {
		admissionPlan->info += QByteArray("/login-admission-v1");
	}
	auto admissionKey = admissionPlan
		? DesktopArchiveCrypto::Derive(options.localKey, *admissionPlan) : std::nullopt;
	const auto admissionAdded = admissionKey
		&& self.crypto.AddKey(u"admission-v1"_q, *admissionKey);
	if (admissionKey) {
		DesktopArchiveCrypto::Clear(*admissionKey);
	}
	if (!admissionAdded || (options.active && !options.active())) {
		self.Fail(DesktopArchiveStatus::Unreadable, u"admission-key-unavailable"_q);
		return self.status;
	}
	const auto catalogue = Catalogue(self.layout, options.create);
	if (!catalogue) {
		self.Fail(DesktopArchiveStatus::Invalid, u"catalogue"_q);
		return self.status;
	}
	auto record = catalogue->value(u"accounts"_q).toObject().value(self.layout.account);
	if (!record.isUndefined()) {
		if (!record.isObject() || !AdmissionShape(record.toObject())) {
			self.Fail(DesktopArchiveStatus::Unreadable, u"admission-record-shape"_q);
			return self.status;
		}
		const auto mac = self.AdmissionMac(record.toObject());
		if (!mac || record.toObject().value(u"loginAdmissionMac"_q) != QJsonValue(*mac)) {
			self.Fail(DesktopArchiveStatus::Unreadable, u"admission-record-authentication"_q);
			return self.status;
		}
	}
	const auto changedAuthKey = record.isObject()
		&& !DesktopArchiveLoginKeysOverlap(
			record.toObject().value(u"loginAuthKeys"_q).toString().toLatin1(),
			options.loginAuthKeyIds);
	if (record.isObject() && !changedAuthKey
		&& record.toObject().value(u"loginAuthority"_q) != QJsonValue(authority)) {
		self.Fail(DesktopArchiveStatus::Unreadable, u"login-authority-mismatch"_q);
		return self.status;
	}
	if (changedAuthKey) {
		if (!ApplyRecord(self.layout, record.toObject())) {
			self.Fail(DesktopArchiveStatus::Unreadable, u"orphaned-authority-record"_q);
			return self.status;
		}
		const auto durable = Exists(self.layout.intent)
			? DesktopArchiveRead(self.layout.intent, 4096) : std::optional<QByteArray>();
		const auto removal = durable ? ResumeArchiveRemoval(*durable)
			: BeginArchiveRemoval({options.account, self.layout.store, self.layout.generation, false}, NewToken());
		if (!removal || !RunRemoval(self.layout, *removal, nullptr, 64, 0, options.active)) {
			self.Fail(DesktopArchiveStatus::RemovalPending, u"orphan-removal-pending"_q);
			return self.status;
		}
		record = QJsonValue(QJsonValue::Undefined);
	}
	if (record.isUndefined()) {
		if (!options.create) {
			self.Fail(DesktopArchiveStatus::Disabled, u"not-created"_q);
			return self.status;
		}
		if (Exists(self.layout.accountDirectory)
			&& !QDir(self.layout.accountDirectory).isEmpty()) {
			self.Fail(DesktopArchiveStatus::Unreadable, u"orphaned-account-store"_q);
			return self.status;
		}
		const auto oldFence = Exists(self.layout.fence)
								  ? Fence(self.layout)
								  : std::optional<ArchiveStoreEpoch>();
		if ((Exists(self.layout.fence) && !oldFence)
			|| (oldFence
				&& (!oldFence->removalPending
					|| oldFence->generation == std::numeric_limits<uint64_t>::max()))) {
			self.Fail(DesktopArchiveStatus::Invalid, u"unresolved-store-fence"_q);
			return self.status;
		}
		record = QJsonObject{{u"store"_q, NewToken()}, {u"key"_q, NewToken()},
			{u"generation"_q, QString::number(oldFence ? oldFence->generation + 1 : 1)},
			{u"phase"_q, u"creating"_q}, {u"slot"_q, options.slot},
			{u"loginAuthority"_q, authority}, {u"loginAuthorityVersion"_q, 3},
			{u"loginAuthKeys"_q, QString::fromLatin1(options.loginAuthKeyIds)},
			{u"wrappedKey"_q, u"initial"_q},
			{u"loginAuthKeyId"_q, QString::fromLatin1(options.loginAuthKeyId)}};
		auto authenticated = record.toObject();
		if ((options.active && !options.active()) || !self.SealAdmission(authenticated)
			|| !ApplyRecord(self.layout, authenticated)
			|| !UpdateRecord(self.layout, authenticated)) {
			self.Fail(DesktopArchiveStatus::IoError, u"catalogue-create"_q);
			return self.status;
		}
	} else if (!record.isObject() || !ApplyRecord(self.layout, record.toObject())) {
		self.Fail(DesktopArchiveStatus::Invalid, u"catalogue-entry"_q);
		return self.status;
	}
	self.epoch = {options.account, self.layout.store, self.layout.generation, false};
	const auto creating
		= self.layout.record.value(u"phase"_q) == QJsonValue(u"creating"_q);
	const auto oldFence = Exists(self.layout.fence) ? Fence(self.layout)
													: std::optional<ArchiveStoreEpoch>();
	if (!oldFence || !CanRunArchiveWork(self.epoch, *oldFence)) {
		if (!creating
			|| (oldFence
				&& (!oldFence->removalPending
					|| oldFence->generation >= self.epoch.generation))
			|| !WriteFence(self.layout, self.epoch)) {
			self.Fail(DesktopArchiveStatus::Invalid, u"work-fence"_q);
			return self.status;
		}
	}
	if ((!Exists(self.layout.accountDirectory) && !creating)
		|| !DesktopArchiveDirectory(self.layout.accountDirectory)
		|| (!Exists(self.layout.root) && !creating)
		|| !DesktopArchiveDirectory(self.layout.root)) {
		self.Fail(DesktopArchiveStatus::IoError, u"owned-directory"_q);
		return self.status;
	}
	self.native = DesktopArchiveIdentity(self.layout.root);
	auto wrapping = ArchiveKeyDerivationFor(options.account, self.layout.store,
		self.layout.profile, ArchiveKeyPurpose::DesktopWrapping);
	if (wrapping) {
		wrapping->info += QByteArray("/login-authority/") + options.loginAuthority
			+ QByteArray("/mtp-auth-keys/")
			+ self.layout.record.value(u"loginAuthKeys"_q).toString().toLatin1()
			+ QByteArray("/mtp-main-key/")
			+ self.layout.record.value(u"loginAuthKeyId"_q).toString().toLatin1();
	}
	auto wrappingKey = wrapping
						   ? DesktopArchiveCrypto::Derive(options.localKey, *wrapping)
						   : std::nullopt;
	auto wrappingCrypto = self.crypto;
	const auto wrappingAdded = wrappingKey
		&& wrappingCrypto.AddKey(u"desktop-wrap-v1"_q, *wrappingKey);
	if (wrappingKey) {
		DesktopArchiveCrypto::Clear(*wrappingKey);
	}
	if (wrapping) {
		DesktopArchiveCrypto::Clear(wrapping->info);
	}
	if (!wrappingAdded) {
		self.Fail(DesktopArchiveStatus::Unreadable, u"wrapping-key"_q);
		return self.status;
	}
	const auto wrappedName = self.layout.record.value(u"wrappedKey"_q).toString();
	const auto wrappedPath = self.layout.root + u"/wrapped-key"_q
		+ (wrappedName == u"initial"_q ? QString() : '-' + wrappedName);
	const auto databasePath = self.layout.root + u"/archive.sqlite"_q;
	auto dataKey = std::optional<QByteArray>();
	const auto clearDataKey = std::unique_ptr<std::optional<QByteArray>, void (*)(std::optional<QByteArray>*)>(
		&dataKey, [](std::optional<QByteArray>* key) {
			if (*key) {
				DesktopArchiveCrypto::Clear(**key);
			}
		});
	if (Exists(wrappedPath)) {
		const auto frame = DesktopArchiveRead(wrappedPath, 8192);
		auto opened = frame
								? UnwrapArchiveDataKey(options.account, self.layout.store,
									  self.layout.key, u"desktop-wrap-v1"_q, *frame,
									  wrappingCrypto.callbacks())
								: ArchiveEnvelopeResult();
		if (opened) {
			dataKey = std::move(opened.bytes);
		}
	} else if (creating && !Exists(databasePath) && QDir(self.layout.root).isEmpty()) {
		dataKey = DesktopArchiveCrypto::Random(32);
		const auto wrapped = dataKey
								 ? WrapArchiveDataKey(options.account, self.layout.store,
									   self.layout.key, u"desktop-wrap-v1"_q, *dataKey,
									   wrappingCrypto.callbacks())
								 : ArchiveEnvelopeResult();
		if (!wrapped || !DesktopArchiveWrite(wrappedPath, wrapped.bytes)) {
			dataKey.reset();
		}
	}
	if (!dataKey || dataKey->size() != 32
		|| !self.crypto.AddKey(self.layout.key, *dataKey)) {
		self.Fail(DesktopArchiveStatus::Unreadable, u"data-key-unavailable"_q);
		return self.status;
	}
	const auto index = ArchiveKeyDerivationFor(options.account, self.layout.store,
		self.layout.profile, ArchiveKeyPurpose::IndexTokens);
	auto indexKey = index ? DesktopArchiveCrypto::Derive(*dataKey, *index) : std::nullopt;
	const auto indexAdded = indexKey && self.crypto.AddKey(u"index-v1"_q, *indexKey);
	if (indexKey) {
		DesktopArchiveCrypto::Clear(*indexKey);
	}
	if (!indexAdded) {
		self.Fail(DesktopArchiveStatus::Unreadable, u"index-key"_q);
		return self.status;
	}
	auto rebound = std::optional<QJsonObject>();
	auto rebindBytes = QByteArray();
	if (self.layout.record.value(u"loginAuthKeys"_q)
			!= QJsonValue(QString::fromLatin1(options.loginAuthKeyIds))
		|| self.layout.record.value(u"loginAuthKeyId"_q)
			!= QJsonValue(QString::fromLatin1(options.loginAuthKeyId))) {
		auto record = self.layout.record;
		const auto token = NewToken();
		auto plan = ArchiveKeyDerivationFor(options.account, self.layout.store,
			self.layout.profile, ArchiveKeyPurpose::DesktopWrapping);
		if (!plan || token.isEmpty()) {
			self.Fail(DesktopArchiveStatus::Unreadable, u"rebind-key"_q);
			return self.status;
		}
		plan->info += QByteArray("/login-authority/") + options.loginAuthority
			+ QByteArray("/mtp-auth-keys/") + options.loginAuthKeyIds
			+ QByteArray("/mtp-main-key/") + options.loginAuthKeyId;
		auto key = DesktopArchiveCrypto::Derive(options.localKey, *plan);
		auto crypto = DesktopArchiveCrypto();
		const auto added = key && crypto.AddKey(u"desktop-wrap-v1"_q, *key);
		if (key) {
			DesktopArchiveCrypto::Clear(*key);
		}
		DesktopArchiveCrypto::Clear(plan->info);
		auto wrapped = added ? WrapArchiveDataKey(options.account,
			self.layout.store, self.layout.key, u"desktop-wrap-v1"_q,
			*dataKey, crypto.callbacks()) : ArchiveEnvelopeResult();
		record.insert(u"loginAuthKeys"_q, QString::fromLatin1(options.loginAuthKeyIds));
		record.insert(u"loginAuthKeyId"_q, QString::fromLatin1(options.loginAuthKeyId));
		record.insert(u"wrappedKey"_q, token);
		crypto.Revoke();
		if (!wrapped || !self.SealAdmission(record)) {
			self.Fail(DesktopArchiveStatus::Unreadable, u"rebind-key"_q);
			return self.status;
		}
		rebound = std::move(record);
		rebindBytes = std::move(wrapped.bytes);
	}
	DesktopArchiveCrypto::Clear(*dataKey);
	DesktopArchiveCrypto::Clear(options.localKey);
	DesktopArchiveCrypto::Clear(options.loginAuthority);
	self.security = {self.layout.store, self.layout.key, self.crypto.callbacks(),
		self.crypto.indexHmac(u"index-v1"_q)};
	auto initialize = !Exists(databasePath);
	if (initialize) {
		if (!creating || !DesktopArchiveWrite(databasePath, {})) {
			self.Fail(DesktopArchiveStatus::Unreadable, u"database-missing"_q);
			return self.status;
		}
	} else if (!DesktopArchivePrivate(databasePath, false)) {
		self.Fail(DesktopArchiveStatus::Invalid, u"database-file"_q);
		return self.status;
	}
	self.databaseNative = DesktopArchiveIdentity(databasePath);
	if (!self.database.Open(databasePath, false, true).ok) {
		self.Fail(DesktopArchiveStatus::IoError, u"database-probe-open"_q);
		return self.status;
	}
	const auto tables = self.database.Read(
		{u"SELECT count(*) FROM sqlite_schema WHERE sql IS NOT NULL AND name NOT GLOB 'sqlite_*'"_q,
			{}, ArchiveSqlExpectation::OneRow});
	const auto count = tables.ok && tables.rows.size() == 1
						   ? Value<int64>(tables.rows.front(), 0)
						   : nullptr;
	initialize = count && !*count && creating;
	if (!count || (!*count && !initialize)) {
		self.Fail(DesktopArchiveStatus::Invalid, u"database-schema"_q);
		return self.status;
	}
	if (!initialize && !self.RequireAdmission()) {
		return self.status;
	}
	if (!self.database.Close() || !self.database.Open(databasePath, false).ok) {
		self.Fail(DesktopArchiveStatus::IoError, u"database-reopen"_q);
		return self.status;
	}
	if (!initialize && !self.RequireAdmission()) {
		return self.status;
	}
	if (!self.database.Configure().ok) {
		self.Fail(DesktopArchiveStatus::IoError, u"database-configure"_q);
		return self.status;
	}
	if (initialize) {
		auto empty = ArchiveState();
		empty.account = options.account;
		if (!self.database.Execute(InitializeArchiveSql(empty, self.security))
				.committed) {
			self.Fail(DesktopArchiveStatus::IoError, u"database-initialize"_q);
			return self.status;
		}
	}
	if (!self.RequireAdmission()) {
		return self.status;
	}
	const auto maintenance = InspectDesktopArchiveMaintenance(self.database, self.epoch, self.security);
	if (!maintenance.valid || !PrivateDatabaseFiles(self.layout)) {
		self.Fail(DesktopArchiveStatus::Unreadable, u"header-or-files"_q);
		return self.status;
	}
	for (const auto &name : {u"blobs"_q, u"staging"_q, u"outbox"_q}) {
		if (!DesktopArchiveDirectory(self.layout.root + '/' + name)) {
			self.Fail(DesktopArchiveStatus::IoError, u"owned-subdirectory"_q);
			return self.status;
		}
	}
	if (rebound) {
		if ((options.active && !options.active())
			|| !DesktopArchiveWrite(self.layout.root + u"/wrapped-key-"_q
				+ rebound->value(u"wrappedKey"_q).toString(), rebindBytes)
			|| !UpdateRecord(self.layout, *rebound)) {
			self.Fail(DesktopArchiveStatus::IoError, u"login-rebind-held"_q);
			return self.status;
		}
		self.layout.record = std::move(*rebound);
	}
	self.layout.record.insert(u"phase"_q, u"ready"_q);
	self.layout.record.insert(u"slot"_q, options.slot);
	if (!self.SealAdmission(self.layout.record)
		|| !UpdateRecord(self.layout, self.layout.record)) {
		self.Fail(DesktopArchiveStatus::IoError, u"catalogue-ready"_q);
		return self.status;
	}
	self.maintenancePending = maintenance.purge.has_value();
	self.status = self.maintenancePending ? DesktopArchiveStatus::Maintenance : DesktopArchiveStatus::Ready;
	if (self.maintenancePending) {
		return self.status;
	}
	if (!RecoverMediaLeases()) {
		return self.status;
	}
	if (!RecoverCaptureAttention()) {
		return self.status;
	}
	const auto gaps = self.database.Read(ArchiveSqlReadCaptureGaps(1));
	if (!gaps.ok) {
		self.Fail(DesktopArchiveStatus::Unreadable, u"gap-index"_q);
		return self.status;
	}
	if (!gaps.rows.empty()) {
		self.failure = u"capture-gap"_q;
	}
	return self.status;
}

bool DesktopArchiveStore::Close() {
	_impl->journalRenewal.reset();
	_impl->RevokeJournalAdmission();
	_impl->streams.clear();
	ExpireHistory(true);
	if (!ExpireDeletedViews(true)) {
		return false;
	}
	*_impl->alive = false;
	_impl->crypto.Revoke();
	for (const auto &[id, handle] : _impl->mediaHandles) {
		handle.active->store(false);
	}
	_impl->mediaHandles.clear();
	_impl->retiredMedia.clear();
	_impl->purgeReaders.clear();
	if (_impl->readers || !_impl->database.Close()) {
		_impl->Fail(DesktopArchiveStatus::IoError, u"database-close"_q);
		return false;
	}
	ReleaseAccountLock(_impl->lock, _impl->layout.lock);
	_impl->security = {};
	_impl->crypto = DesktopArchiveCrypto();
	_impl->status = DesktopArchiveStatus::Disabled;
	return true;
}

bool DesktopArchiveStore::Current() const { return _impl->Current(); }
bool DesktopArchiveStore::MemoryCurrent() const { return _impl->MemoryCurrent(); }
DesktopArchiveStagingCounters DesktopArchiveStore::StagingCountersForTest() const {
	return {_impl->stagingCounters->fenceReads.load(),
		_impl->stagingCounters->proofQueries.load(),
		_impl->stagingCounters->catalogueOpens.load(), _impl->streams.size()};
}
void DesktopArchiveStore::CountStagingProof() { ++_impl->stagingCounters->proofQueries; }
void DesktopArchiveStore::CountStagingOpen() { ++_impl->stagingCounters->catalogueOpens; }
DesktopArchiveStatus DesktopArchiveStore::status() const { return _impl->status; }
QString DesktopArchiveStore::failure() const {
	return _impl->failure.isEmpty() ? _impl->admissionFailure : _impl->failure;
}
const ArchiveStoreEpoch &DesktopArchiveStore::epoch() const { return _impl->epoch; }
const ArchiveSqlSecurity &DesktopArchiveStore::security() const {
	return _impl->security;
}
QString DesktopArchiveStore::directory() const { return _impl->layout.root; }
DesktopArchiveSqlite &DesktopArchiveStore::database() { return _impl->database; }

bool DesktopArchiveStore::Execute(const ArchiveSqlProgram &program) {
	return Execute(program, false);
}

bool DesktopArchiveStore::Execute(const ArchiveSqlProgram &program, std::function<bool()> active) {
	return Execute(program, false, std::move(active));
}

bool DesktopArchiveStore::Execute(const ArchiveSqlProgram &program, bool maintenance,
		std::function<bool()> active) {
	const auto current = [&] {
		if (active && !active()) {
			return false;
		}
		if (!maintenance) {
			return Current();
		}
		return _impl->Authorized() && _impl->maintenancePending
			&& _impl->status == DesktopArchiveStatus::Maintenance;
	};
	if (!current()) {
		return false;
	}
	auto keepAdmission = true;
	auto admissionReadFailure = false;
	const auto result = _impl->database.Execute(program, [&] {
		if (_impl->journalAdmission) {
			const auto header = _impl->Header(&admissionReadFailure);
			keepAdmission = header && std::get<ArchiveState>(header->row.value).accountGeneration
				== _impl->journalGeneration;
		}
		return current();
	});
	if (result.committed) {
		if (!keepAdmission) {
			if (admissionReadFailure) {
				if (!_impl->journalRenewal) {
					_impl->journalRenewal = Impl::JournalRenewal{epoch(),
						_impl->journalGeneration, _impl->journalDeleteGenerations};
				}
				_impl->journalRenewal->candidate = false;
				_impl->admissionFailure = u"capture-admission-header-unreadable"_q;
			}
			_impl->RevokeJournalAdmission();
		}
		_impl->streams.clear();
	}
	const auto ok = result.ok && (program.statements.empty() || result.committed)
		&& PrivateDatabaseFiles(_impl->layout) && current();
	const auto viewsCurrent = _impl->deletedViews.empty() || ExpireDeletedViews();
	ExpireHistory();
	if (!_impl->mediaHandles.empty()) {
		RefreshMediaReaders();
	}
	return ok && viewsCurrent;
}

bool DesktopArchiveStore::Gap(
	const QString &id, const QString &reason, int64 observedAt) {
	_impl->failure = reason;
	if (!Current()) {
		return _impl->Authorized() && _impl->maintenancePending
			&& WriteDesktopArchiveAttention(directory(), epoch(), security(), reason, observedAt);
	}
	return Execute(PlanArchiveCaptureGap(
		{_impl->options.account, _impl->layout.store, id, observedAt, reason, {}},
		_impl->security));
}

bool DesktopArchiveStore::Remove(int maximumActions) {
	_impl->journalRenewal.reset();
	_impl->RevokeJournalAdmission();
	ExpireHistory(true);
	if (!ExpireDeletedViews(true)) {
		return false;
	}
	auto &self = *_impl;
	self.streams.clear();
	if (!self.lock || !self.lock->isLocked() || self.layout.store.isEmpty()) {
		return false;
	}
	const auto durable = Exists(self.layout.intent)
							 ? DesktopArchiveRead(self.layout.intent, 4096)
							 : std::optional<QByteArray>();
	auto removal = durable ? ResumeArchiveRemoval(*durable)
						   : BeginArchiveRemoval(self.epoch, NewToken());
	if (!removal) {
		return false;
	}
	*self.alive = false;
	self.crypto.Revoke();
	self.status = DesktopArchiveStatus::RemovalPending;
	const auto result
		= RunRemoval(self.layout, *removal, &self.database, maximumActions, self.readers);
	if (result) {
		self.lock.reset();
		self.security = {};
		self.crypto = DesktopArchiveCrypto();
	}
	return result;
}

std::optional<DesktopArchiveDiscardToken> DesktopArchiveStore::PrepareDiscard(
	const DesktopArchiveOptions &options) {
	if (options.loginAuthority.size() != 32
		|| !DesktopArchiveLoginKeysValid(options.loginAuthKeyIds)
		|| (options.active && !options.active())) {
		return {};
	}
	if (NoProfileBinding(options)) {
		return DesktopArchiveDiscardToken{options.account, {},
			DesktopArchiveIdentity(options.profileRoot), {}, {}, {},
			QString::fromLatin1(QCryptographicHash::hash(options.loginAuthority, QCryptographicHash::Sha256).toHex()),
			options.loginAuthKeyIds};
	}
	const auto profile = Profile(options, false);
	if (!profile) {
		return {};
	}
	auto lock = std::make_unique<QLockFile>(profile->lock);
	const auto release = gsl::finally([&] { ReleaseAccountLock(lock, profile->lock); });
	lock->setStaleLockTime(0);
	if (!RecoverReleasedLock(profile->lock) || !lock->tryLock(0)) {
		return {};
	}
	const auto catalogue = Catalogue(*profile, false);
	const auto fence = Exists(profile->fence)
		? DesktopArchiveRead(profile->fence, 4096) : std::optional(QByteArray("absent"));
	if (!catalogue || !fence) {
		return {};
	}
	const auto entry = catalogue->value(u"accounts"_q).toObject().value(profile->account);
	const auto identity = DesktopArchiveIdentity(profile->accountDirectory);
	if (Exists(profile->accountDirectory) && identity.isEmpty()) {
		return {};
	}
	return DesktopArchiveDiscardToken{options.account, profile->profile,
		DesktopArchiveIdentity(options.profileRoot), identity,
		QCryptographicHash::hash(QJsonDocument(QJsonArray{entry}).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256),
		QCryptographicHash::hash(*fence, QCryptographicHash::Sha256),
		QString::fromLatin1(QCryptographicHash::hash(options.loginAuthority, QCryptographicHash::Sha256).toHex()),
		options.loginAuthKeyIds};
}

bool DesktopArchiveStore::DiscardAccount(DesktopArchiveOptions options,
	const DesktopArchiveDiscardToken &token, int maximumActions) {
	DesktopArchiveCrypto::Clear(options.localKey);
	if (options.account != token.account || options.loginAuthority.size() != 32
		|| options.loginAuthKeyIds != token.keys
		|| QString::fromLatin1(QCryptographicHash::hash(options.loginAuthority, QCryptographicHash::Sha256).toHex()) != token.authority
		|| maximumActions < 1 || maximumActions > 64
		|| (options.maintenanceAllowed && !options.maintenanceAllowed())
		|| (options.active && !options.active())) {
		return false;
	}
	if (token.profile.isEmpty()) {
		return NoProfileBinding(options)
			&& DesktopArchiveIdentity(options.profileRoot) == token.profileIdentity;
	}
	const auto profile = Profile(options, false);
	if (!profile || profile->profile != token.profile
		|| DesktopArchiveIdentity(options.profileRoot) != token.profileIdentity) {
		return false;
	}
	auto layout = *profile;
	auto lock = std::make_unique<QLockFile>(layout.lock);
	const auto release = gsl::finally([&] { ReleaseAccountLock(lock, layout.lock); });
	lock->setStaleLockTime(0);
	if (!RecoverReleasedLock(layout.lock) || !lock->tryLock(0)) {
		return false;
	}
	const auto catalogue = Catalogue(layout, false);
	const auto fenceBytes = Exists(layout.fence)
		? DesktopArchiveRead(layout.fence, 4096) : std::optional(QByteArray("absent"));
	if (!catalogue || !fenceBytes) {
		return false;
	}
	const auto entry = catalogue->value(u"accounts"_q).toObject().value(layout.account);
	if (DesktopArchiveIdentity(layout.accountDirectory) != token.directoryIdentity
		|| QCryptographicHash::hash(QJsonDocument(QJsonArray{entry}).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256) != token.recordDigest
		|| QCryptographicHash::hash(*fenceBytes, QCryptographicHash::Sha256) != token.fenceDigest
		|| (options.active && !options.active())) {
		return false;
	}
	const auto oldFence = Fence(layout);
	const auto durable = Exists(layout.intent)
		? DesktopArchiveRead(layout.intent, 4096) : std::optional<QByteArray>();
	auto removal = durable ? ResumeArchiveRemoval(*durable)
		: BeginArchiveRemoval({options.account,
			oldFence ? oldFence->storeId : NewToken(),
			oldFence ? oldFence->generation : 1, false}, NewToken());
	if (!removal || removal->epoch.account != options.account) {
		return false;
	}
	layout.store = removal->epoch.storeId;
	layout.generation = removal->epoch.generation;
	layout.root = layout.accountDirectory;
	layout.discardRecord = entry;
	auto fence = removal->epoch;
	fence.removalPending = true;
	if ((options.active && !options.active())
		|| (options.maintenanceAllowed && !options.maintenanceAllowed())
		|| !WriteFence(layout, fence)) {
		return false;
	}
	return RunRemoval(layout, *removal, nullptr, maximumActions, 0,
		[&] {
			return (!options.active || options.active())
				&& (!options.maintenanceAllowed || options.maintenanceAllowed());
		});
}

bool DesktopArchiveStore::RemoveAccount(
	DesktopArchiveOptions options, int maximumActions) {
	DesktopArchiveCrypto::Clear(options.localKey);
	if (NoProfileBinding(options)) {
		return true;
	}
	const auto profile = Profile(options, false);
	if (!profile) {
		return !Exists(options.archiveRoot);
	}
	auto layout = *profile;
	auto lock = QLockFile(layout.lock);
	lock.setStaleLockTime(0);
	if (!RecoverReleasedLock(layout.lock) || !lock.tryLock(0)) {
		return false;
	}
	const auto catalogue = Catalogue(layout, false);
	const auto entry
		= catalogue ? catalogue->value(u"accounts"_q).toObject().value(layout.account)
					: QJsonValue();
	if (!entry.isUndefined() && !options.removalStore.isEmpty()
		&& entry.toObject().value(u"store"_q) != QJsonValue(options.removalStore)) {
		return true;
	}
	if (!entry.isUndefined() && !options.removalAuthority.isEmpty()
		&& entry.toObject().value(u"loginAuthority"_q) != QJsonValue(options.removalAuthority)) {
		return options.removalAuthority != u"unavailable"_q
			&& IsArchiveHash(entry.toObject().value(u"loginAuthority"_q).toString());
	}
	auto removal = std::optional<ArchiveRemoval>();
	if (Exists(layout.intent)) {
		const auto bytes = DesktopArchiveRead(layout.intent, 4096);
		removal = bytes ? ResumeArchiveRemoval(*bytes) : std::nullopt;
		if (!removal || removal->epoch.account != options.account
			|| !Token(removal->epoch.storeId)) {
			return false;
		}
		if (!options.removalStore.isEmpty() && options.removalStore != removal->epoch.storeId) {
			return true;
		}
		layout.store = removal->epoch.storeId;
		layout.root = layout.accountDirectory + '/' + layout.store;
	} else {
		if (entry.isUndefined()) {
			return catalogue.has_value();
		}
		if (!entry.isObject() || !ApplyRecord(layout, entry.toObject())) {
			return false;
		}
		removal = BeginArchiveRemoval(
			{options.account, layout.store, layout.generation, false}, NewToken());
	}

	return removal && RunRemoval(layout, *removal, nullptr, maximumActions, 0, options.maintenanceAllowed);
}

bool DesktopArchiveStore::Recover(const DesktopArchiveOptions &options) {
	if (NoProfileBinding(options)) {
		return true;
	}
	const auto profile = Profile(options, false);
	if (!profile) {
		return !Exists(options.archiveRoot);
	}
	auto iterator = QDirIterator(profile->profileDirectory + u"/intents"_q,
		QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
	auto count = 0;
	while (iterator.hasNext()) {
		if (++count > 1024) {
			return false;
		}
		const auto path = iterator.next();
		const auto bytes = DesktopArchiveRead(path, 4096);
		const auto removal = bytes ? ResumeArchiveRemoval(*bytes) : std::nullopt;
		if (!removal) {
			return false;
		}
		const auto key = AccountKeyText(
			removal->epoch.account.environment, removal->epoch.account.userId);
		if (!key || iterator.fileName() != *key + u".json"_q) {
			return false;
		}
		auto target = options;
		target.account = removal->epoch.account;
		target.removalStore.clear();
		target.removalAuthority.clear();
		if (!RemoveAccount(std::move(target))) {
			return false;
		}
	}
	return true;
}

std::optional<AccountKey> DesktopArchiveStore::AccountForSlot(
	const DesktopArchiveOptions &options, const QString &slot, bool* valid) {
	if (valid) {
		*valid = false;
	}
	if (slot.isEmpty()) {
		return {};
	}
	if (NoProfileBinding(options)) {
		if (valid) {
			*valid = true;
		}
		return {};
	}
	const auto profile = Profile(options, false);
	if (!profile) {
		return {};
	}
	const auto catalogue = Catalogue(*profile, false);
	if (!catalogue) {
		return {};
	}
	auto result = std::optional<AccountKey>();
	const auto accounts = catalogue->value(u"accounts"_q).toObject();
	for (auto i = accounts.begin(); i != accounts.end(); ++i) {
		auto layout = *profile;
		const auto key = ParseAccountKey(i.key());
		if (!key || !i.value().isObject() || !ApplyRecord(layout, i.value().toObject())
			|| !i.value().toObject().value(u"slot"_q).isString()) {
			return {};
		}
		if (i.value().toObject().value(u"slot"_q) == QJsonValue(slot)) {
			if (result) {
				return {};
			}
			result = key;
		}
	}
	if (valid) {
		*valid = true;
	}
	return result;
}

std::optional<ArchiveSqlSlice> DesktopArchiveStore::ReadSlice(
	const ArchiveMessageKey &key, const QString &event, const QString &target,
	const QString &lease, const QString &blob) {
	auto &self = *_impl;
	if (!Current() || key.account != self.options.account || !IsArchiveMessageKey(key)
		|| !self.database.BeginRead()) {
		return {};
	}
	auto phase = u"header"_q;
	const auto result = [&]() -> std::optional<ArchiveSqlSlice> {
		auto rows = std::vector<ArchiveStoredRow>();
		const auto header = self.Header();
		if (!header) {
			return {};
		}
		rows.push_back(*header);
		const auto messageId = ArchiveSqlMessageId(key, self.security);
		if (messageId.isEmpty()) {
			return {};
		}
		phase = u"message"_q;
		const auto message
			= self.database.Read(ArchiveSqlReadMessage(key, self.security));
		if (!message.ok) {
			return {};
		}
		auto versions = std::set<QString>();
		auto receipts = std::set<QString>();
		auto blobs = std::set<QString>();
		auto jobs = std::set<QString>();
		if (!event.isEmpty()) {
			receipts.insert(event);
		}
		if (!target.isEmpty()) {
			versions.insert(target);
		}
		if (!message.rows.empty()) {
			const auto &values = message.rows.front();
			const auto id = Value<QString>(values, 0);
			const auto head = Value<QString>(values, 1);
			const auto bytes = Value<QByteArray>(values, 2);
			if (values.size() != 3 || !id || *id != messageId || !head || !bytes) {
				return {};
			}
			const auto row
				= self.Decode(ArchiveRecordKind::Message, messageId, *bytes, 1);
			if (!row || std::get<ArchiveMessage>(row->row.value).key != key) {
				return {};
			}
			rows.push_back(*row);
			if (!head->isEmpty()) {
				phase = u"head"_q;
				const auto read
					= self.database.Read(ArchiveSqlReadHead(key, self.security));
				if (!read.ok || read.rows.size() != 1) {
					return {};
				}
				const auto frame = Value<QByteArray>(read.rows.front(), 0);
				if (!frame) {
					return {};
				}
				const auto version = self.Decode(
					ArchiveRecordKind::Version, messageId + '/' + *head, *frame);
				if (!version) {
					return {};
				}
				const auto &snapshot
					= std::get<ArchiveVersionSnapshot>(version->row.value);
				if (snapshot.key != key
					|| ArchiveSqlChildId(key, ArchiveIndexKind::Version,
						   snapshot.version.id, self.security)
						   != version->id
					|| snapshot.version.order
						   > std::get<ArchiveMessage>(row->row.value).order) {
					return {};
				}
				versions.insert(snapshot.version.id);
			}
		}
		if (!lease.isEmpty()) {
			const auto read = self.database.Read(ArchiveSqlReadRecord(
				key.account, ArchiveRecordKind::Lease, lease, self.security));
			if (!read.ok || read.rows.size() != 1) {
				return {};
			}
			const auto token = Value<QString>(read.rows.front(), 0);
			const auto frame = Value<QByteArray>(read.rows.front(), 1);
			if (!token || !frame) {
				return {};
			}
			const auto row = self.Decode(ArchiveRecordKind::Lease, *token, *frame);
			if (!row) {
				return {};
			}
			const auto &value = std::get<ArchiveReaderLease>(row->row.value);
			if (value.key != key || value.id != lease) {
				return {};
			}
			if (!value.revoked) {
				versions.insert(value.version);
			}
			blobs.insert(value.blobId);
			rows.push_back(*row);
		}
		phase = u"versions"_q;
		for (const auto &id : versions) {
			const auto read
				= self.database.Read(ArchiveSqlReadVersion(key, id, self.security));
			if (!read.ok || read.rows.size() != 1) {
				return {};
			}
			const auto frame = Value<QByteArray>(read.rows.front(), 0);
			if (!frame) {
				return {};
			}
			const auto rowId
				= ArchiveSqlChildId(key, ArchiveIndexKind::Version, id, self.security);
			const auto row = self.Decode(ArchiveRecordKind::Version, rowId, *frame);
			if (!row) {
				return {};
			}
			const auto &snapshot = std::get<ArchiveVersionSnapshot>(row->row.value);
			if (snapshot.key != key || snapshot.version.id != id) {
				return {};
			}
			receipts.insert(snapshot.version.id);
			receipts.insert(snapshot.version.revision);
			for (const auto &ref : snapshot.version.media) {
				if (!ref.blobId.isEmpty()) {
					blobs.insert(ref.blobId);
				}
			}
			const auto pending = self.database.Read(
				ArchiveSqlVersionJobs(key, id, kArchiveSqlPageLimit, self.security));
			if (!pending.ok) {
				return {};
			}
			for (const auto &values : pending.rows) {
				const auto token = Value<QString>(values, 0);
				const auto bytes = Value<QByteArray>(values, 1);
				if (!token || !bytes) {
					return {};
				}
				const auto job = self.Decode(ArchiveRecordKind::Job, *token, *bytes);
				if (!job) {
					return {};
				}
				const auto &value = std::get<ArchiveMediaJob>(job->row.value);
				if (value.key != key || value.version != id
					|| ArchiveSqlToken(key.account, ArchiveIndexKind::Job,
						   value.id.toUtf8(), self.security)
						   != *token) {
					return {};
				}
				if (jobs.insert(value.id).second) {
					rows.push_back(*job);
				}
			}
			rows.push_back(*row);
		}
		phase = u"receipts"_q;
		for (const auto &id : receipts) {
			const auto read
				= self.database.Read(ArchiveSqlReadReceipt(key, id, self.security));
			if (!read.ok) {
				return {};
			}
			if (read.rows.empty()) {
				continue;
			}
			const auto frame = Value<QByteArray>(read.rows.front(), 0);
			if (!frame) {
				return {};
			}
			const auto row = self.Decode(ArchiveRecordKind::Event,
				ArchiveSqlChildId(key, ArchiveIndexKind::Event, id, self.security),
				*frame);
			if (!row || std::get<ArchiveEventReceipt>(row->row.value).id != id) {
				return {};
			}
			rows.push_back(*row);
		}
		phase = u"epoch"_q;
		auto validEpoch = false;
		const auto chatEpoch = self.ChatEpoch(key, validEpoch);
		if (!validEpoch) {
			return {};
		}
		if (chatEpoch) {
			rows.push_back(*chatEpoch);
		}
		if (!blob.isEmpty()) {
			blobs.insert(blob);
		}
		for (const auto &id : blobs) {
			const auto read = self.database.Read(ArchiveSqlReadRecord(
				key.account, ArchiveRecordKind::Blob, id, self.security));
			if (read.ok && read.rows.empty() && id == blob) {
				continue;
			}
			if (!read.ok || read.rows.size() != 1) {
				return {};
			}
			const auto token = Value<QString>(read.rows.front(), 0);
			const auto frame = Value<QByteArray>(read.rows.front(), 1);
			if (!token || !frame) {
				return {};
			}
			const auto row = self.Decode(ArchiveRecordKind::Blob, *token, *frame);
			if (!row || std::get<ArchiveBlob>(row->row.value).id != id) {
				return {};
			}
			rows.push_back(*row);
		}
		phase = u"restore"_q;
		return rows.size() <= kArchiveSqlSliceRowLimit
				   ? RestoreArchiveSqlSlice(std::move(rows), self.security)
				   : std::nullopt;
	}();
	if (!result) {
		self.failure = u"read-"_q + phase;
	}
	const auto ended = self.database.EndRead();
	return ended && Current() ? result : std::nullopt;
}

DesktopArchiveCaptureResult DesktopArchiveStore::Capture(ArchiveEvent event) {
	auto result = DesktopArchiveCaptureResult();
	const auto slice = ReadSlice(event.key, event.id, event.targetVersion);
	if (!slice) {
		result.status
			= Current() ? DesktopArchiveStatus::Invalid : DesktopArchiveStatus::Stale;
		result.reason = u"capture-slice"_q;
		_impl->failure = result.reason;
		static_cast<void>(Gap(event.id, result.reason, event.observation.observedAt));
		return result;
	}
	const auto plan
		= PlanArchiveSqlCapture(*slice, event, _impl->security, &_impl->epoch);
	result.model = plan.transaction.status;
	if (plan.program.status == ArchiveSqlStatus::RetentionGap) {
		result.status = DesktopArchiveStatus::RetentionGap;
		result.reason = plan.program.retentionGapReason;
	} else if (plan.transaction.status == ArchivePlanStatus::NoChange && plan.program) {
		result.status = DesktopArchiveStatus::Ready;
		result.retained = true;
		return result;
	} else if (!plan.transaction || !plan.program) {
		result.status = DesktopArchiveStatus::Invalid;
		result.reason = u"capture-refused"_q;
	} else if (!Execute(plan.program)) {
		result.status = DesktopArchiveStatus::IoError;
		result.reason = u"capture-commit"_q;
	} else {
		const auto completion
			= CompleteArchiveTransaction(slice->state, plan.transaction, true);
		result.retained = completion.status == ArchiveCommitStatus::Applied
						  || completion.status == ArchiveCommitStatus::AlreadyApplied;
		result.status = result.retained ? DesktopArchiveStatus::Ready
										: DesktopArchiveStatus::Invalid;
		return result;
	}
	_impl->failure = result.reason;
	const auto id = QString::fromLatin1(
		QCryptographicHash::hash(event.id.toUtf8(), QCryptographicHash::Sha256).toHex());
	static_cast<void>(Gap(id, result.reason, event.observation.observedAt));
	return result;
}

DesktopArchiveHistoryPage DesktopArchiveStore::Versions(const ArchiveMessageKey &key,
	int limit, const QString &beforeOrder, std::optional<uint64_t> revision) {
	auto result = DesktopArchiveHistoryPage();
	auto &self = *_impl;
	result.epoch = self.epoch;
	result.hasGap = !self.failure.isEmpty();
	if (!Current() || key.account != self.options.account || limit < 1
		|| limit > kArchiveSqlPageLimit || !self.database.BeginRead()) {
		return result;
	}
	const auto loaded = [&] {
		const auto header = self.Header();
		if (!header) {
			return false;
		}
		const auto admission = self.database.Read({
			u"SELECT 1 FROM archive_meta WHERE singleton=1 AND maintenance_id=''"_q,
			{}, ArchiveSqlExpectation::OneRow});
		if (!admission.ok) {
			return false;
		}
		auto validEpoch = false;
		const auto chatEpoch = self.ChatEpoch(key, validEpoch);
		if (!validEpoch) {
			return false;
		}
		const auto &state = std::get<ArchiveState>(header->row.value);
		result.generation = {state.accountGeneration,
			chatEpoch ? std::get<ArchiveChatGeneration>(chatEpoch->row.value).generation : 0};
		result.revision = state.revision;
		if (revision && *revision != result.revision) {
			result.status = DesktopArchiveStatus::Stale;
			return false;
		}
		const auto read = self.database.Read(
			ArchiveSqlReadVersions(key, limit, self.security, beforeOrder));
		if (!read.ok) {
			return false;
		}
		auto previous = beforeOrder;
		for (const auto &values : read.rows) {
			const auto id = Value<QString>(values, 0);
			const auto order = Value<QString>(values, 1);
			const auto bytes = Value<QByteArray>(values, 2);
			if (!id || !order || !bytes || (!previous.isEmpty() && *order >= previous)) {
				return false;
			}
			const auto rowId = ArchiveSqlMessageId(key, self.security) + '/' + *id;
			const auto row = self.Decode(ArchiveRecordKind::Version, rowId, *bytes);
			if (!row) {
				return false;
			}
			const auto snapshot = std::get<ArchiveVersionSnapshot>(row->row.value);
			if (snapshot.key != key
				|| rowId
					   != ArchiveSqlChildId(key, ArchiveIndexKind::Version,
						   snapshot.version.id, self.security)
				|| ArchiveSqlOrder(snapshot.version.order) != *order) {
				return false;
			}
			previous = *order;
			result.versions.push_back(snapshot);
		}
		result.beforeOrder = previous;
		result.more = read.rows.size() == size_t(limit);
		return true;
	}();
	const auto ended = self.database.EndRead();
	if (loaded && ended && Current()) {
		result.status = DesktopArchiveStatus::Ready;
	} else {
		result.versions.clear();
	}
	return result;
}

std::optional<std::vector<ArchiveMessageKey>> DesktopArchiveStore::NonChannelMessages(
	int64 messageId) {
	auto &self = *_impl;
	if (!Current() || messageId <= 0 || !self.database.BeginRead()) {
		return {};
	}
	const auto result = [&]() -> std::optional<std::vector<ArchiveMessageKey>> {
		if (!self.Header()) {
			return {};
		}
		auto statement = ArchiveSqlStatement{
			u"SELECT message_key,envelope FROM messages WHERE message_id=? AND chat_kind IN (0,1) AND EXISTS(SELECT 1 FROM archive_meta WHERE maintenance_id='') LIMIT 3"_q,
			{messageId}, ArchiveSqlExpectation::Rows};
		statement.rows.maximum = 2;
		const auto rows = self.database.Read(statement);
		if (!rows.ok) {
			return {};
		}
		auto keys = std::vector<ArchiveMessageKey>();
		for (const auto &values : rows.rows) {
			const auto id = Value<QString>(values, 0);
			const auto bytes = Value<QByteArray>(values, 1);
			if (!id || !bytes) {
				return {};
			}
			const auto row = self.Decode(ArchiveRecordKind::Message, *id, *bytes);
			if (!row) {
				return {};
			}
			const auto key = std::get<ArchiveMessage>(row->row.value).key;
			if (key.account != self.options.account || key.messageId != messageId
				|| ArchiveSqlMessageId(key, self.security) != *id) {
				return {};
			}
			if (key.collection != ArchiveCollection::Received
				|| key.representation != u"original"_q) {
				continue;
			}
			if (key.chat.kind != ArchiveChatKind::User
				&& key.chat.kind != ArchiveChatKind::Group) {
				return {};
			}
			keys.push_back(key);
		}
		return keys;
	}();
	const auto ended = self.database.EndRead();
	return ended && Current() ? result : std::nullopt;
}

std::optional<QString> DesktopArchiveStore::ObservedMediaVersion(
	const ArchiveMessageKey &key, const QString &observationId,
	const QString &contentHash, const QString &identity) {
	if (!IsArchiveToken(observationId) || !IsArchiveHash(contentHash)
		|| !IsArchiveToken(identity)) {
		return {};
	}
	auto slice = ReadSlice(key);
	if (!slice) {
		return {};
	}
	const auto message = FindArchiveMessage(slice->state, key);
	if (!message || message->versions.empty()) {
		return {};
	}
	auto selected = message->versions.back();
	if (selected.id != observationId && selected.revision != observationId) {
		auto before = QString();
		auto revision = std::optional<uint64_t>();
		auto versionId = QString();
		for (;;) {
			const auto page = Versions(key, kArchiveSqlPageLimit, before, revision);
			if (page.status != DesktopArchiveStatus::Ready) {
				return {};
			}
			revision = page.revision;
			for (const auto &snapshot : page.versions) {
				const auto &version = snapshot.version;
				if (version.id == observationId || version.revision == observationId) {
					if (!versionId.isEmpty() && versionId != version.id) {
						return {};
					}
					versionId = version.id;
				}
			}
			if (!page.more) {
				break;
			}
			before = page.beforeOrder;
		}
		if (versionId.isEmpty()) {
			return {};
		}
		slice = ReadSlice(key, {}, versionId);
		if (!slice) {
			return {};
		}
		const auto version = FindArchiveVersion(slice->state, key, versionId);
		if (!version) {
			return {};
		}
		selected = *version;
	}
	return (selected.id == observationId || selected.revision == observationId)
				   && ArchiveContentHash(selected.content) == contentHash
				   && std::any_of(selected.media.begin(), selected.media.end(),
					   [&](const auto &entry) { return entry.identity == identity; })
				   && Current()
			   ? std::optional(selected.id)
			   : std::nullopt;
}

bool DesktopArchiveStore::CaptureLifecycle(const ArchiveEvent &preimage, const ArchiveStoreEpoch &captured) {
	const auto context = preimage.observation.context;
	const auto transition = context.value(u"desktopLifecycle"_q).toInt(-1);
	if (!preimage.content || transition < 0 || transition > int(ArchiveMediaTransition::CacheEviction)
		|| !CanRunArchiveWork(captured, epoch())) {
		return false;
	}
	auto slice = ReadSlice(preimage.key);
	const auto message = slice ? FindArchiveMessage(slice->state, preimage.key) : nullptr;
	if (!message || message->versions.empty()) {
		return false;
	}
	if (std::any_of(message->receipts.begin(), message->receipts.end(),
		[&](const auto &receipt) { return receipt.id == preimage.id + u":lifecycle"_q; })) {
		const auto missing = std::any_of(message->versions.back().media.begin(), message->versions.back().media.end(),
			[](const auto &ref) { return ref.state != ArchiveMediaState::Complete; });
		return !missing || Gap(preimage.id + u":media-unavailable"_q,
			u"lifecycle-media-not-owned"_q, preimage.observation.observedAt);
	}
	const auto &head = message->versions.back();
	const auto original = context.value(u"desktopLifecycleObservation"_q).toString();
	const auto hash = context.value(u"desktopLifecycleHash"_q).toString();
	if ((!original.isEmpty() && head.id != original && head.revision != original)
		|| (!hash.isEmpty() && ArchiveContentHash(head.content) != hash)
		|| (!context.value(u"desktopLifecycleFallback"_q).toBool() && original.isEmpty() && (head.content.text != preimage.content->text
			|| head.content.media.size() != preimage.content->media.size()))) {
		return false;
	}
	auto event = ArchiveEvent();
	auto mediaUnavailable = false;
	const auto fallback = context.value(u"desktopLifecycleFallback"_q).toBool();
	if (preimage.content->media.empty() && !fallback) {
		event.key = preimage.key;
		event.id = preimage.id + u":lifecycle"_q;
		event.order = preimage.order + 1;
		event.origin = ArchiveOrigin::MediaExpiry;
		event.generation = preimage.generation;
		event.targetVersion = head.id;
		event.targetRevision = head.revision;
		event.expectedContentHash = ArchiveContentHash(head.content);
		event.observation = preimage.observation;
		if (!context.value(u"desktopWholeMessageTtl"_q).toBool()
			|| preimage.generation != ArchiveGenerationFor(slice->state, preimage.key.chat)) {
			return false;
		}
	} else {
		if (fallback && head.media.empty()) {
			return false;
		}
		const auto identity = fallback ? head.media.front().identity : preimage.content->media.front().identity;
		const auto plan = PlanArchiveMediaLifecycle(slice->state, epoch(), {
			{captured, preimage.key, head.id, head.revision, ArchiveContentHash(head.content), identity,
				preimage.generation},
			context.value(u"desktopViewOnce"_q).toBool() ? ArchiveMediaLifetime::ViewOnce : ArchiveMediaLifetime::Ttl,
			ArchiveMediaTransition(transition), ArchiveAvailableBytes::Unavailable, true,
			preimage.id + u":lifecycle"_q, preimage.order + 1, preimage.observation.observedAt});
		if (plan.transaction.status == ArchivePlanStatus::NoChange) {
			return true;
		}
		if (!plan.event) {
			return false;
		}
		mediaUnavailable = plan.retention != ArchiveMediaRetention::Owned;
		event = *plan.event;
	}
	const auto result = Capture(std::move(event));
	return (result.retained || result.model == ArchivePlanStatus::NoChange)
		&& (!mediaUnavailable || Gap(preimage.id + u":media-unavailable"_q,
			u"lifecycle-media-not-owned"_q, preimage.observation.observedAt));
}

std::optional<ArchiveStagingBinding> DesktopArchiveStore::StageStream(const ArchiveMessageKey &key,
	const QString &version, const QString &identity, uint64_t size) {
	auto slice = ReadSlice(key, {}, version);
	const auto selected = slice ? FindArchiveVersion(slice->state, key, version) : nullptr;
	if (!selected) {
		return {};
	}
	const auto reference = std::find_if(selected->media.begin(), selected->media.end(),
		[&](const auto &ref) { return ref.identity == identity; });
	if (reference == selected->media.end() || reference->state == ArchiveMediaState::Complete) {
		return {};
	}
	auto job = ArchiveMediaJob{NewToken(), key, version, identity,
		ArchiveGenerationFor(slice->state, key.chat), size, {}};
	job.revision = selected->revision;
	job.contentHash = ArchiveContentHash(selected->content);
	const auto pending = std::find_if(slice->state.jobs.begin(), slice->state.jobs.end(),
		[&](const auto &old) { return old.key == key && old.version == version
			&& old.mediaIdentity == identity && old.state == ArchiveJobState::Staged; });
	if (pending != slice->state.jobs.end()) {
		job = *pending;
		if (job.expectedSize != std::optional<uint64_t>(size)) {
			return {};
		}
	} else {
		const auto plan = PlanArchiveMediaStage(slice->state, job);
		if (!plan || !Execute(TranslateArchiveSql(plan, *slice, security()))) {
			return {};
		}
	}
	return ArchiveStagingBinding{{epoch(), key, version, job.revision, job.contentHash,
		identity, job.generation}, job.id, size, job.expectedHash.isEmpty() ? std::nullopt : std::optional(job.expectedHash)};
}

bool DesktopArchiveStore::ResumeStreams(QString &cursor, bool &more) {
	more = false;
	if (!Current()) {
		return false;
	}
	const auto page = database().Read(ArchiveSqlReadPage(ArchiveRecordKind::Job, 16, cursor));
	if (!page.ok) {
		return false;
	}
	for (const auto &values : page.rows) {
		const auto token = Value<QString>(values, 0);
		const auto frame = Value<QByteArray>(values, 2);
		if (values.size() != 3 || !token || !frame || *token <= cursor) {
			return false;
		}
		const auto row = _impl->Decode(ArchiveRecordKind::Job, *token, *frame);
		if (!row) {
			return false;
		}
		const auto &job = std::get<ArchiveMediaJob>(row->row.value);
		if (job.key.account != epoch().account || ArchiveSqlToken(job.key.account,
			ArchiveIndexKind::Job, job.id.toUtf8(), security()) != *token) {
			return false;
		}
		cursor = *token;
		if (job.state != ArchiveJobState::Staged || !job.expectedSize) {
			continue;
		}
		const auto binding = ArchiveStagingBinding{{epoch(), job.key, job.version, job.revision,
			job.contentHash, job.mediaIdentity, job.generation}, job.id, *job.expectedSize,
			job.expectedHash.isEmpty() ? std::nullopt : std::optional(job.expectedHash)};
		const auto path = directory() + u"/staging/stream-"_q + ArchiveStagingBindingId(binding, security());
		if (!QFileInfo::exists(path)) {
			continue;
		}
		auto stream = DesktopArchiveStream(*this, binding);
		if (!stream.Open() || (stream.header().coveredBytes == binding.totalBytes && !stream.Complete())) {
			if (!Gap(job.id + u":resume"_q, u"stream-recovery-held"_q, QDateTime::currentSecsSinceEpoch())) {
				return false;
			}
		}
	}
	more = page.rows.size() == 16;
	return true;
}

void DesktopArchiveStore::PruneStreams() {
	if (!MemoryCurrent()) {
		_impl->streams.clear();
	} else {
		std::erase_if(_impl->streams, [](const auto &entry) {
			return entry.second.active && !entry.second.active->load();
		});
	}
}

bool DesktopArchiveStore::StreamPart(const ArchiveStagingBinding &binding, uint64_t offset,
	const QByteArray &bytes, std::shared_ptr<std::atomic<bool>> active) {
	PruneStreams();
	if ((active && !active->load()) || !Current() || !CanRunArchiveWork(binding.source.epoch, epoch())
		|| bytes.size() > kArchiveMediaChunkBytes || offset > binding.totalBytes
		|| uint64_t(bytes.size()) > binding.totalBytes - offset) {
		return false;
	}
	const auto slice = ReadSlice(binding.source.key, {}, binding.source.version);
	const auto saved = slice ? database().Read(ArchiveSqlReadRecord(binding.source.key.account,
		ArchiveRecordKind::Job, binding.jobId, security())) : DesktopArchiveSqlResult();
	const auto token = saved.ok && saved.rows.size() == 1 ? Value<QString>(saved.rows.front(), 0) : nullptr;
	const auto frame = saved.ok && saved.rows.size() == 1 ? Value<QByteArray>(saved.rows.front(), 1) : nullptr;
	const auto row = token && frame ? _impl->Decode(ArchiveRecordKind::Job, *token, *frame) : std::nullopt;
	const auto job = row ? std::get_if<ArchiveMediaJob>(&row->row.value) : nullptr;
	if (!slice || !saved.ok || !job || job->id != binding.jobId
		|| *token != ArchiveSqlToken(binding.source.key.account, ArchiveIndexKind::Job,
			binding.jobId.toUtf8(), security())) {
		return false;
	}
	if (job->state == ArchiveJobState::Complete) {
		const auto &source = binding.source;
		const auto version = FindArchiveVersion(slice->state, source.key, source.version);
		if (!version || job->key != source.key || job->version != source.version
			|| job->revision != source.revision || job->contentHash != source.contentHash
			|| job->mediaIdentity != source.mediaIdentity || job->generation != source.generation
			|| version->revision != source.revision || ArchiveContentHash(version->content) != source.contentHash
			|| ArchiveGenerationFor(slice->state, source.key.chat) != source.generation
			|| job->expectedSize != std::optional<uint64_t>(binding.totalBytes)
			|| (!job->expectedHash.isEmpty() && job->expectedHash != binding.expectedBlobHash.value_or(QString()))) {
			return false;
		}
		auto position = uint64_t(0);
		auto matched = uint64_t(0);
		return ReadMedia(source.key, source.version, source.mediaIdentity, [&](const QByteArray &part) {
			const auto from = std::max(position, offset);
			const auto till = std::min(position + uint64_t(part.size()), offset + uint64_t(bytes.size()));
			const auto same = from >= till || part.mid(from - position, till - from) == bytes.mid(from - offset, till - from);
			matched += from < till ? till - from : 0;
			position += part.size();
			return same;
		}) && matched == uint64_t(bytes.size());
	}
	const auto id = ArchiveStagingBindingId(binding, security());
	if (id.isEmpty()) {
		return false;
	}
	auto &streams = _impl->streams;
	auto found = streams.find(id);
	if (found != streams.end() && found->second.active != active) {
		streams.erase(found);
		found = streams.end();
	}
	if (found == streams.end()) {
		if (streams.size() >= 8) {
			streams.erase(streams.begin());
		}
		auto stream = std::make_shared<DesktopArchiveStream>(*this, binding, active);
		if (!stream->Open()) {
			return false;
		}
		found = streams.emplace(id, Impl::StreamEntry{std::move(stream), active}).first;
	}
	const auto stream = found->second.stream;
	const auto written = bytes.isEmpty() && !binding.totalBytes ? ArchiveStagingStatus::NoChange
		: stream->Write(offset, bytes);
	if (written != ArchiveStagingStatus::Ready && written != ArchiveStagingStatus::NoChange) {
		streams.erase(id);
		return false;
	}
	if (stream->header().coveredBytes != binding.totalBytes) {
		return true;
	}
	const auto completed = stream->Complete();
	streams.erase(id);
	return completed;
}

bool DesktopArchiveStore::CopyMedia(const ArchiveMessageKey &key, const QString &version,
	const QString &identity, QIODevice &source, const DesktopArchiveMediaCopy &copy) {
	auto slice = ReadSlice(key, {}, version, {}, copy.hash);
	if (!slice) {
		return false;
	}
	const auto capturedEpoch = epoch();
	const auto selected = FindArchiveVersion(slice->state, key, version);
	if (!selected) {
		return false;
	}
	const auto reference = std::find_if(selected->media.begin(), selected->media.end(),
		[&](const auto &entry) { return entry.identity == identity; });
	if (reference == selected->media.end()) {
		return false;
	}
	const auto verifyOwned = [&] {
		return ReadMedia(key, version, identity, [](const QByteArray &) { return true; });
	};
	if (reference->state == ArchiveMediaState::Complete) {
		return verifyOwned();
	}
	auto job = ArchiveMediaJob{NewToken(), key, version, identity,
		ArchiveGenerationFor(slice->state, key.chat), copy.size, copy.hash};
	job.revision = selected->revision;
	job.contentHash = ArchiveContentHash(selected->content);
	const auto stage = PlanArchiveMediaStage(slice->state, job);
	if (!stage || !Execute(TranslateArchiveSql(stage, *slice, security()))) {
		return false;
	}
	if (std::none_of(stage.next.jobs.begin(), stage.next.jobs.end(),
			[&](const auto &entry) { return entry.id == job.id; })) {
		return verifyOwned();
	}
	const auto copied = MediaPublicationAllowed(copy.hash) && DesktopArchivePublishMedia(
		directory(), key.account, security(), source, copy, [this] { return Current(); });
	slice = ReadSlice(key, {}, version, {}, copy.hash);
	if (!slice) {
		return false;
	}
	const auto result
		= copied ? ArchiveMediaResult{true, copy.hash, copy.hash, copy.size}
				 : ArchiveMediaResult{false, {}, {}, 0, u"owned-copy-failed"_q};
	const auto done = PlanArchiveScopedMediaCompletion(
		slice->state, capturedEpoch, epoch(), job.id, result);
	if (!done) {
		switch (done.mediaRefusal) {
		case ArchiveMediaRefusal::StoreEpochMismatch:
			_impl->failure = u"media-store-stale"_q;
			break;
		case ArchiveMediaRefusal::PurgeEpochMismatch:
			_impl->failure = u"media-purge-stale"_q;
			break;
		case ArchiveMediaRefusal::VersionMissing:
			_impl->failure = u"media-version-missing"_q;
			break;
		case ArchiveMediaRefusal::RevisionMismatch:
			_impl->failure = u"media-revision-stale"_q;
			break;
		case ArchiveMediaRefusal::ContentHashMismatch:
			_impl->failure = u"media-body-stale"_q;
			break;
		case ArchiveMediaRefusal::MediaMissing:
			_impl->failure = u"media-reference-missing"_q;
			break;
		case ArchiveMediaRefusal::JobCancelled:
			_impl->failure = u"media-job-cancelled"_q;
			break;
		case ArchiveMediaRefusal::None:
			_impl->failure = done.status == ArchivePlanStatus::NotFound
								 ? u"media-job-missing"_q
								 : u"media-completion-refused"_q;
			break;
		}
		return false;
	}
	return Execute(TranslateArchiveSql(done, *slice, security())) && copied;
}

bool DesktopArchiveStore::ReadMedia(const ArchiveMessageKey &key, const QString &version,
	const QString &identity, const std::function<bool(const QByteArray &)> &sink) {
	auto slice = ReadSlice(key, {}, version);
	if (!slice || !sink || _impl->readers + _impl->mediaHandles.size() + _impl->deletedViews.size() >= 16) {
		return false;
	}
	const auto selected = FindArchiveVersion(slice->state, key, version);
	if (!selected) {
		return false;
	}
	const auto reference = std::find_if(selected->media.begin(), selected->media.end(),
		[&](const auto &ref) { return ref.identity == identity; });
	if (reference == selected->media.end()
		|| reference->state != ArchiveMediaState::Complete) {
		return false;
	}
	const auto blob = std::find_if(slice->state.blobs.begin(), slice->state.blobs.end(),
		[&](const auto &entry) { return entry.id == reference->blobId; });
	if (blob == slice->state.blobs.end() || blob->state != ArchiveBlobState::Complete) {
		return false;
	}
	const auto copy = DesktopArchiveMediaCopy{blob->id, blob->size};
	const auto lease = ArchiveReaderLease{
		NewToken(), key, version, blob->id, ArchiveGenerationFor(slice->state, key.chat)};
	const auto acquire = PlanArchiveReaderAcquire(slice->state, lease);
	if (!acquire || !Execute(TranslateArchiveSql(acquire, *slice, security()))) {
		return false;
	}
	++_impl->readers;
	const auto result = DesktopArchiveReadMedia(
		directory(), key.account, security(), copy, sink, [&, this] {
			const auto live = ReadSlice(key, {}, version, lease.id);
			if (!live
				|| ArchiveGenerationFor(live->state, key.chat) != lease.generation) {
				return false;
			}
			const auto found
				= std::find_if(live->state.leases.begin(), live->state.leases.end(),
					[&](const auto &entry) { return entry.id == lease.id; });
			return found != live->state.leases.end() && !found->revoked;
		});
	--_impl->readers;
	slice = ReadSlice(key, {}, {}, lease.id);
	if (!slice) {
		return false;
	}
	const auto release = PlanArchiveReaderRelease(slice->state, lease.id);
	return release && Execute(TranslateArchiveSql(release, *slice, security())) && result;
}

std::optional<DesktopArchiveMediaHandle> DesktopArchiveStore::OpenMedia(
	const ArchiveMediaRequest &request, ArchiveLeaseRevisionChange *committed) {
	if (committed) {
		*committed = {};
	}
	std::erase_if(_impl->retiredMedia, [](const auto &entry) { return entry.second.active.expired(); });
	if (_impl->retiredMedia.size() + _impl->mediaHandles.size() >= 64) return {};
	const auto &source = request.source;
	const auto &key = source.key;
	const auto &version = source.version;
	const auto &identity = source.mediaIdentity;
	if (!CanRunArchiveWork(source.epoch, epoch()) || _impl->readers + _impl->mediaHandles.size() + _impl->deletedViews.size() >= 16) {
		return {};
	}
	const auto slice = ReadSlice(key, {}, version);
	const auto selected = slice ? FindArchiveVersion(slice->state, key, version) : nullptr;
	if (!selected || selected->revision != source.revision
		|| ArchiveContentHash(selected->content) != source.contentHash
		|| ArchiveGenerationFor(slice->state, key.chat) != source.generation) {
		return {};
	}
	const auto ref = std::find_if(selected->media.begin(), selected->media.end(),
		[&](const auto &value) { return value.identity == identity; });
	if (ref == selected->media.end() || ref->state != ArchiveMediaState::Complete || ref->blobId != request.blob) {
		return {};
	}
	const auto blob = std::find_if(slice->state.blobs.begin(), slice->state.blobs.end(),
		[&](const auto &value) { return value.id == ref->blobId; });
	if (blob == slice->state.blobs.end() || blob->state != ArchiveBlobState::Complete) {
		return {};
	}
	const auto generation = ArchiveGenerationFor(slice->state, key.chat);
	const auto lease = ArchiveReaderLease{NewToken(), key, version, blob->id, generation};
	const auto acquire = PlanArchiveReaderAcquire(slice->state, lease);
	if (!acquire || !Execute(TranslateArchiveSql(acquire, *slice, security()))) {
		return {};
	}
	auto handle = DesktopArchiveMediaHandle{source, lease.id, blob->id,
		blob->size, std::make_shared<std::atomic<bool>>(true)};
	_impl->mediaHandles.emplace(handle.lease, handle);
	if (!MediaCurrent(handle)) {
		static_cast<void>(ReleaseMedia(handle));
		return {};
	}
	if (committed) {
		*committed = DescribeArchiveLeaseCommit(acquire);
	}
	return handle;
}

bool DesktopArchiveStore::MediaCurrent(const DesktopArchiveMediaHandle &handle) {
	const auto found = _impl->mediaHandles.find(handle.lease);
	if (found == _impl->mediaHandles.end()) {
		return false;
	}
	const auto &bound = found->second;
	const auto &a = handle.source;
	const auto &b = bound.source;
	if (handle.active != bound.active || handle.blob != bound.blob || handle.size != bound.size
		|| a.epoch != b.epoch || a.key != b.key || a.version != b.version || a.revision != b.revision
		|| a.contentHash != b.contentHash || a.mediaIdentity != b.mediaIdentity || a.generation != b.generation) {
		return false;
	}
	if (!handle.active->load() || !Current() || !CanRunArchiveWork(a.epoch, epoch())) {
		handle.active->store(false);
		return false;
	}
	const auto slice = ReadSlice(a.key, {}, a.version, handle.lease);
	const auto version = slice ? FindArchiveVersion(slice->state, a.key, a.version) : nullptr;
	const auto lease = slice ? std::find_if(slice->state.leases.begin(), slice->state.leases.end(),
		[&](const auto &value) { return value.id == handle.lease; }) : std::vector<ArchiveReaderLease>::const_iterator();
	const auto valid = version && version->revision == a.revision
		&& ArchiveContentHash(version->content) == a.contentHash
		&& ArchiveGenerationFor(slice->state, a.key.chat) == a.generation
		&& lease != slice->state.leases.end() && !lease->revoked
		&& lease->key == a.key && lease->version == a.version && lease->blobId == handle.blob
		&& lease->generation == a.generation
		&& std::any_of(version->media.begin(), version->media.end(), [&](const auto &ref) {
			return ref.identity == a.mediaIdentity && ref.state == ArchiveMediaState::Complete
				&& ref.blobId == handle.blob;
		});
	if (!valid) {
		handle.active->store(false);
	}
	return valid;
}

void DesktopArchiveStore::RefreshMediaReaders() {
	if (_impl->refreshingMediaReaders || _impl->recoveringMediaLeases) {
		return;
	}
	if (!Current() || !_impl->Header()) {
		for (const auto &[id, handle] : _impl->mediaHandles) {
			handle.active->store(false);
		}
		return;
	}
	_impl->refreshingMediaReaders = true;
	const auto done = gsl::finally([&] { _impl->refreshingMediaReaders = false; });
	auto handles = std::vector<DesktopArchiveMediaHandle>();
	for (const auto &[id, handle] : _impl->mediaHandles) {
		handles.push_back(handle);
	}
	for (const auto &handle : handles) {
		if (handle.active->load()) {
			static_cast<void>(MediaCurrent(handle));
		} else {
			static_cast<void>(ReleaseMedia(handle));
		}
	}
}

std::optional<QByteArray> DesktopArchiveStore::MediaRange(const DesktopArchiveMediaHandle &handle,
	uint64_t offset, uint32_t length) {
	return MediaCurrent(handle) ? DesktopArchiveReadMediaRange(directory(), handle.source.key.account,
		security(), {handle.blob, handle.size}, offset, length, [&] { return MediaCurrent(handle); }) : std::nullopt;
}

bool DesktopArchiveStore::ExportMedia(const DesktopArchiveMediaHandle &handle, const QString &path) {
	const auto root = QFileInfo(_impl->options.archiveRoot).canonicalFilePath();
	const auto outside = [&] {
		const auto info = QFileInfo(path);
		const auto parent = QFileInfo(info.absolutePath()).canonicalFilePath();
		const auto destination = info.exists() ? info.canonicalFilePath() : parent + '/' + info.fileName();
		return !root.isEmpty() && !parent.isEmpty() && !destination.isEmpty()
			&& destination != root && !destination.startsWith(root + '/')
			&& parent != root && !parent.startsWith(root + '/');
	};
	return outside() && MediaCurrent(handle) && DesktopArchiveExportMedia(directory(), handle.source.key.account,
		security(), {handle.blob, handle.size}, path, [&] { return outside() && MediaCurrent(handle); });
}

bool DesktopArchiveStore::ReleaseMedia(const DesktopArchiveMediaHandle &handle,
		ArchiveLeaseRevisionChange *committed) {
	if (committed) {
		*committed = {};
	}
	const auto matches = [&](const DesktopArchiveMediaHandle &bound) {
		const auto &a = handle.source;
		const auto &b = bound.source;
		return handle.active && handle.active == bound.active && handle.lease == bound.lease
			&& handle.blob == bound.blob && handle.size == bound.size
			&& a.epoch == b.epoch && a.key == b.key && a.version == b.version
			&& a.revision == b.revision && a.contentHash == b.contentHash
			&& a.mediaIdentity == b.mediaIdentity && a.generation == b.generation;
	};
	const auto found = _impl->mediaHandles.find(handle.lease);
	if (found == _impl->mediaHandles.end()) {
		const auto old = _impl->retiredMedia.find(handle.lease);
		return old != _impl->retiredMedia.end() && _impl->Authorized()
			&& CanRunArchiveWork(handle.source.epoch, epoch())
			&& matches({old->second.source, handle.lease, old->second.blob,
				old->second.size, old->second.active.lock()});
	}
	if (!matches(found->second)) return false;
	const auto bound = found->second;
	bound.active->store(false);
	if (!Current()) {
		if (_impl->maintenancePending && _impl->purgeReaders.contains(bound.lease)
			&& _impl->Authorized() && CanRunArchiveWork(bound.source.epoch, epoch())) {
			return true;
		}
		_impl->failure = u"media-reader-release-held"_q;
		return false;
	}
	const auto acknowledge = [&] {
		if (_impl->purgeReaders.erase(bound.lease)) {
			_impl->retiredMedia[bound.lease] = {bound.source, bound.blob, bound.size, bound.active};
		}
	};
	const auto token = ArchiveSqlToken(bound.source.key.account, ArchiveIndexKind::Lease,
		bound.lease.toUtf8(), security());
	const auto absent = token.isEmpty() ? DesktopArchiveSqlResult() : _impl->database.Read({
		u"SELECT 1 FROM archive_meta WHERE singleton=1 AND maintenance_id='' "
		"AND NOT EXISTS(SELECT 1 FROM leases WHERE lease_id=?)"_q,
		{token}, ArchiveSqlExpectation::Rows});
	const auto proof = absent.ok && absent.rows.size() == 1 && absent.rows[0].size() == 1
		? Value<int64>(absent.rows[0], 0) : nullptr;
	if (proof && *proof == 1) {
		acknowledge();
		_impl->mediaHandles.erase(found);
		return true;
	}
	const auto slice = ReadSlice(bound.source.key, {}, {}, bound.lease);
	const auto release = slice ? PlanArchiveReaderRelease(slice->state, bound.lease) : ArchiveTransaction();
	if (!slice || !release) {
		_impl->failure = u"media-reader-release-held"_q;
		return false;
	}
	const auto program = TranslateArchiveSql(release, *slice, security());
	_impl->mediaHandles.erase(found);
	if (Execute(program)) {
		if (committed) {
			*committed = DescribeArchiveLeaseCommit(release);
		}
		acknowledge();
		return true;
	}
	_impl->mediaHandles.emplace(bound.lease, bound);
	_impl->failure = u"media-reader-release-held"_q;
	return false;
}

bool DesktopArchiveStore::JournalRenewalPending() const {
	return _impl->journalRenewal && !_impl->journalRenewal->held
		&& _impl->journalRenewal->attempts < 3;
}

bool DesktopArchiveStore::JournalRenewalHeld() const {
	return _impl->journalRenewal && _impl->journalRenewal->held;
}

std::optional<DesktopArchiveJournalAccess> DesktopArchiveStore::RenewJournalAccess() {
	auto &self = *_impl;
	if (!JournalRenewalPending()) return {};
	++self.journalRenewal->attempts;
	self.journalRenewal->candidate = false;
	const auto access = JournalAccess(true);
	if (access) self.journalRenewal->candidate = true;
	return access;
}

void DesktopArchiveStore::RefuseJournalRenewal(const QString &reason) {
	if (!JournalRenewalPending()) return;
	++_impl->journalRenewal->attempts;
	_impl->admissionFailure = reason;
	static_cast<void>(CompleteJournalRenewal(false));
}

bool DesktopArchiveStore::CompleteJournalRenewal(bool success) {
	auto &self = *_impl;
	if (!self.journalRenewal) return false;
	if (success && self.journalRenewal->candidate && !self.journalRenewal->held
		&& self.journalAdmission && self.journalAdmission->load() && Current()) {
		self.journalRenewal.reset();
		self.admissionFailure = u"capture-admission-renewed"_q;
		return true;
	}
	self.RevokeJournalAdmission();
	self.journalRenewal->candidate = false;
	if (!self.journalRenewal->held && self.journalRenewal->attempts >= 3) {
		self.journalRenewal->held = true;
		self.admissionFailure = u"capture-admission-renewal-exhausted"_q;
	}
	return false;
}

std::optional<DesktopArchiveJournalAccess> DesktopArchiveStore::JournalAccess() {
	return JournalAccess(false);
}

std::optional<DesktopArchiveJournalAccess> DesktopArchiveStore::JournalAccess(bool renewing) {
	auto &self = *_impl;
	if ((self.journalRenewal && !renewing) || !Current() || !self.database.BeginRead()) {
		return {};
	}
	const auto loaded = [&]() -> std::optional<DesktopArchiveJournalAccess> {
		auto readFailure = false;
		const auto header = self.Header(&readFailure);
		if (!header) {
			if (renewing && !readFailure) {
				self.journalRenewal->held = true;
				self.admissionFailure = u"capture-admission-scope-changed"_q;
			}
			return {};
		}
		const auto order
			= self.database.Read({u"SELECT COALESCE(MAX(causal_order),'0') FROM messages"_q,
				{}, ArchiveSqlExpectation::OneRow});
		const auto text = order.ok && order.rows.size() == 1
							  ? Value<QString>(order.rows.front(), 0)
							  : nullptr;
		auto ok = false;
		const auto minimum = text ? text->toULongLong(&ok) : 0;
		if (!ok || (*text != QString::number(minimum) && *text != ArchiveSqlOrder(minimum))) {
			return {};
		}
		auto generations = std::vector<ArchiveChatGeneration>();
		auto deleteGenerations = QJsonObject();
		auto after = QString();
		for (;;) {
			auto query = ArchiveSqlStatement{
				u"SELECT chat_token,generation,envelope FROM purge_epochs WHERE chat_token>? ORDER BY chat_token LIMIT 128"_q,
				{after}, ArchiveSqlExpectation::Rows};
			query.rows.maximum = 128;
			const auto read = self.database.Read(query);
			if (!read.ok) {
				return {};
			}
			for (const auto &values : read.rows) {
				const auto token = Value<QString>(values, 0);
				const auto projection = Value<QString>(values, 1);
				const auto frame = Value<QByteArray>(values, 2);
				const auto row = values.size() == 3 && token && *token > after && projection && frame
					? self.Decode(ArchiveRecordKind::Generation, *token, *frame) : std::nullopt;
				if (!row || generations.size() >= 4096) {
					self.failure = u"capture-generation-admission"_q;
					return {};
				}
				const auto &generation = std::get<ArchiveChatGeneration>(row->row.value);
				if (ArchiveSqlChatId(generation.chat, self.options.account, self.security) != *token
					|| QString::number(generation.generation) != *projection) {
					return {};
				}
				generations.push_back(generation);
				deleteGenerations.insert(QString::number(int(generation.chat.kind)) + '/'
					+ QString::number(generation.chat.id), QString::number(generation.generation));
				after = *token;
			}
			if (read.rows.size() < 128) {
				break;
			}
		}
		const auto layout = self.layout;
		const auto epoch = self.epoch;
		const auto native = self.native;
		const auto profile = self.options.profileRoot;
		const auto profileNative = self.profileNative;
		const auto active = self.options.active;
		const auto alive = self.alive;
		const auto counters = self.stagingCounters;
		const auto generation = std::get<ArchiveState>(header->row.value).accountGeneration;
		if (renewing && (!CanRunArchiveWork(self.journalRenewal->epoch, self.epoch)
			|| generation != self.journalRenewal->accountGeneration
			|| deleteGenerations != self.journalRenewal->deleteGenerations)) {
			self.journalRenewal->held = true;
			self.admissionFailure = u"capture-admission-scope-changed"_q;
			return {};
		}
		if (!self.journalAdmission || self.journalGeneration != generation) {
			self.RevokeJournalAdmission();
			self.journalAdmission = std::make_shared<std::atomic<bool>>(true);
			self.journalGeneration = generation;
			self.journalDeleteGenerations = deleteGenerations;
		}
		const auto admission = self.journalAdmission;
		return DesktopArchiveJournalAccess{layout.root, epoch, self.security,
			{std::get<ArchiveState>(header->row.value).accountGeneration, 0}, minimum,
			[layout, epoch, native, profile, profileNative, active, alive, counters] {
				++counters->fenceReads;
				const auto fence = Fence(layout);
				return alive->load() && (!active || active()) && fence
					   && CanRunArchiveWork(epoch, *fence) && !Exists(layout.intent)
					   && DesktopArchiveIdentity(layout.root) == native
					   && DesktopArchiveIdentity(profile) == profileNative;
			}, [active, alive, admission] {
				return admission->load() && alive->load() && (!active || active());
			},
			std::move(generations), std::move(deleteGenerations)};
	}();
	return self.database.EndRead() ? loaded : std::nullopt;
}

bool DesktopArchiveStore::UsageComplete() const { return _impl->usageComplete; }

std::optional<ArchiveUsage> DesktopArchiveStore::Usage() {
	auto &self = *_impl;
	if (!Current()) {
		return {};
	}
	if (!self.usageIterator) {
		self.usage = {};
		self.usageComplete = false;
		self.usageIterator = std::make_unique<QDirIterator>(self.layout.root,
			QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
			QDirIterator::Subdirectories);
	}
	for (auto i = 0; i != kArchiveSqlPageLimit && self.usageIterator->hasNext(); ++i) {
		const auto path = self.usageIterator->next();
		const auto info = self.usageIterator->fileInfo();
		if (info.isSymLink() || DesktopArchiveIdentity(path).isEmpty()
			|| info.size() < 0) {
			self.usageIterator.reset();
			return {};
		}
		if (info.isDir()) {
			continue;
		}
		auto target = &self.usage.stagingBytes;
		if (path == self.layout.root + u"/archive.sqlite"_q) {
			target = &self.usage.databaseBytes;
		} else if (path == self.layout.root + u"/archive.sqlite-wal"_q) {
			target = &self.usage.walBytes;
		} else if (path == self.layout.root + u"/archive.sqlite-shm"_q) {
			target = &self.usage.shmBytes;
		} else if (path.startsWith(self.layout.root + u"/blobs/"_q)) {
			target = &self.usage.blobBytes;
		}
		if (uint64_t(info.size()) > std::numeric_limits<uint64_t>::max() - *target) {
			self.usageIterator.reset();
			return {};
		}
		*target += uint64_t(info.size());
	}
	self.usageComplete = !self.usageIterator->hasNext();
	if (self.usageComplete) {
		self.usageIterator.reset();
	}
	return TotalArchiveUsage(self.usage) && Current() ? std::optional(self.usage)
													  : std::nullopt;
}

}

namespace Purple {

ArchiveDeletedPage DesktopArchiveStore::OpenDeletedView(const ArchiveViewScope &scope,
		ArchiveTranscriptOrder order, int limit) {
	auto &self = *_impl;
	auto result = ArchiveDeletedPage();
	result.epoch = self.epoch;
	if (!Current()) {
		result.status = self.status == DesktopArchiveStatus::ReadOnly ? ArchiveQueryStatus::ReadOnly
			: self.status == DesktopArchiveStatus::SchemaMismatch ? ArchiveQueryStatus::SchemaMismatch
			: self.status == DesktopArchiveStatus::Busy ? ArchiveQueryStatus::Busy
			: ArchiveQueryStatus::Unavailable;
		result.reason = self.failure;
		return result;
	}
	if (scope.messages.account != self.options.account || !ExpireDeletedViews()) {
		return result;
	}
	if (self.readers + self.mediaHandles.size() + self.deletedViews.size() >= 16) {
		result.status = ArchiveQueryStatus::Busy;
		return result;
	}
	const auto id = NewToken();
	if (!Token(id) || self.deletedViews.contains(id)) {
		return result;
	}
	auto query = std::make_unique<DesktopArchiveDeletedQuery>(id, self.epoch, self.security, scope, order);
	const auto active = self.alive;
	const auto external = self.options.active;
	result.status = query->Open(self.layout.root + u"/archive.sqlite"_q,
		[active, external] { return active->load() && (!external || external()); });
	if (result.status != ArchiveQueryStatus::Ready) {
		static_cast<void>(query->Close());
		return result;
	}
	self.deletedViews.emplace(id, std::move(query));
	return ReadDeletedView(id, self.epoch, limit);
}

ArchiveDeletedPage DesktopArchiveStore::ReadDeletedView(const QString &id,
		const ArchiveStoreEpoch &epoch, int limit) {
	auto &self = *_impl;
	auto result = ArchiveDeletedPage();
	result.status = ArchiveQueryStatus::Stale;
	result.epoch = self.epoch;
	const auto i = self.deletedViews.find(id);
	if (i == self.deletedViews.end() || i->second->epoch() != epoch) {
		return result;
	}
	if (!Current() || !CanRunArchiveWork(epoch, self.epoch)) {
		static_cast<void>(CloseDeletedView(id, epoch));
		return result;
	}
	if (i->second->Current(self.database)) {
		result = i->second->Read(limit);
		result.hasGap = !self.failure.isEmpty();
		if (result.status == ArchiveQueryStatus::Ready && (!Current() || !i->second->Current(self.database))) {
			result.entries.clear();
			result.status = ArchiveQueryStatus::Stale;
		}
	}
	if (result.status != ArchiveQueryStatus::Ready) {
		static_cast<void>(CloseDeletedView(id, epoch));
	}
	return result;
}

bool DesktopArchiveStore::CloseDeletedView(const QString &id, const ArchiveStoreEpoch &epoch) {
	auto &self = *_impl;
	const auto i = self.deletedViews.find(id);
	if (i == self.deletedViews.end()) {
		return true;
	}
	if (i->second->epoch() != epoch || !i->second->Close()) {
		return false;
	}
	self.deletedViews.erase(i);
	return true;
}

bool DesktopArchiveStore::ExpireDeletedViews(bool all) {
	auto &self = *_impl;
	auto closed = true;
	for (auto i = self.deletedViews.begin(); i != self.deletedViews.end();) {
		if (!all && self.Current() && !i->second->Expired() && i->second->Current(self.database)) {
			++i;
		} else if (i->second->Close()) {
			i = self.deletedViews.erase(i);
		} else {
			closed = false;
			++i;
		}
	}
	if (!closed) {
		for (const auto &[id, query] : self.deletedViews) {
			static_cast<void>(query->Close());
		}
		return self.Fail(DesktopArchiveStatus::IoError, u"deleted-reader-close"_q);
	}
	return true;
}

}

namespace Purple {

bool DesktopArchiveStore::RecoverMediaLeases() {
	auto &self = *_impl;
	if (!Current() || self.recoveringMediaLeases) {
		return false;
	}
	self.recoveringMediaLeases = true;
	const auto recovering = gsl::finally([&] { self.recoveringMediaLeases = false; });
	for (auto page = 0; page != 64; ++page) {
		const auto leases = self.database.Read(
			ArchiveSqlReadPage(ArchiveRecordKind::Lease, kArchiveSqlPageLimit));
		if (!leases.ok) {
			self.Fail(DesktopArchiveStatus::Unreadable, u"reader-recovery"_q);
			return false;
		}
		if (leases.rows.empty()) {
			break;
		}
		for (const auto &values : leases.rows) {
			const auto token = Value<QString>(values, 0);
			const auto frame = Value<QByteArray>(values, 2);
			const auto row = token && frame
								 ? self.Decode(ArchiveRecordKind::Lease, *token, *frame)
								 : std::nullopt;
			if (!row) {
				self.Fail(DesktopArchiveStatus::Unreadable, u"reader-evidence"_q);
				return false;
			}
			const auto lease = std::get<ArchiveReaderLease>(row->row.value);
			const auto slice = ReadSlice(
				lease.key, {}, lease.revoked ? QString() : lease.version, lease.id);
			const auto release = slice ? PlanArchiveReaderRelease(slice->state, lease.id)
									   : ArchiveTransaction();
			if (!slice || !release
				|| !Execute(TranslateArchiveSql(release, *slice, self.security))) {
				self.Fail(DesktopArchiveStatus::Unreadable, u"reader-recovery-commit"_q);
				return false;
			}
		}
		if (page == 63) {
			self.Fail(DesktopArchiveStatus::Unreadable, u"reader-recovery-budget"_q);
			return false;
		}
	}
	self.recoveringMediaLeases = false;
	RefreshMediaReaders();
	return true;
}

DesktopArchivePurgePreview DesktopArchiveStore::PreparePurge(
		ArchivePurgeKind kind, const ArchivePurgeScope &scope) {
	if (!Current() || MaintenanceHeld()) {
		auto result = DesktopArchivePurgePreview();
		result.epoch = epoch();
		result.maintenance = MaintenancePending() || MaintenanceHeld();
		result.status = _impl->status == DesktopArchiveStatus::ReadOnly ? ArchiveQueryStatus::ReadOnly
			: _impl->status == DesktopArchiveStatus::SchemaMismatch ? ArchiveQueryStatus::SchemaMismatch
			: result.maintenance ? ArchiveQueryStatus::Busy : ArchiveQueryStatus::Unavailable;
		return result;
	}
	return PrepareDesktopArchivePurge(_impl->database, epoch(), kind, scope, security());
}

bool DesktopArchiveStore::BeginPurge(const ArchivePurgeConfirmation &confirmation) {
	auto &self = *_impl;
	if (!Current()) {
		return false;
	}
	if (!BeginDesktopArchivePurge(self.database, epoch(), confirmation, NewToken(), security(),
		[this](const ArchiveSqlProgram &program) { return Execute(program); })) {
		const auto outcome = InspectDesktopArchiveMaintenance(self.database, epoch(), security());
		if (!outcome.valid || outcome.purge || !self.Authorized()) {
			self.maintenancePending = true;
			if (outcome.valid && outcome.purge) {
				for (const auto &[id, handle] : self.mediaHandles) self.purgeReaders.insert(id);
			}
			static_cast<void>(self.Fail(DesktopArchiveStatus::MaintenanceHeld, u"purge-begin-outcome-held"_q));
		}
		return false;
	}
	self.maintenancePending = true;
	self.garbagePending = true;
	self.garbageHeld = false;
	self.cancelledJobCursor.clear();
	self.cancelledJobsComplete = false;
	self.status = DesktopArchiveStatus::Maintenance;
	for (const auto &[id, handle] : self.mediaHandles) self.purgeReaders.insert(id);
	if (!self.Authorized() || !PrivateDatabaseFiles(self.layout)) {
		return self.Fail(DesktopArchiveStatus::MaintenanceHeld, u"purge-reader-close-held"_q);
	}
	return true;
}

DesktopArchiveMaintenanceStep DesktopArchiveStore::ContinuePurge() {
	auto &self = *_impl;
	if (!self.Authorized() || !self.maintenancePending) {
		return {};
	}
	const auto step = ContinueDesktopArchivePurge(self.database, epoch(), security(),
		[this](const ArchiveSqlProgram &program) { return Execute(program, true); });
	if (!step.valid || !self.Authorized() || !PrivateDatabaseFiles(self.layout)) {
		static_cast<void>(self.Fail(DesktopArchiveStatus::MaintenanceHeld, u"purge-continuation-held"_q));
		return {};
	}
	if (step.finished) {
		self.maintenancePending = false;
		self.status = DesktopArchiveStatus::Ready;
		if (self.failure.startsWith(u"purge-"_q)) {
			self.failure.clear();
		}
		if (!RecoverMediaLeases() || !RecoverCaptureAttention()) {
			static_cast<void>(self.Fail(DesktopArchiveStatus::MaintenanceHeld, u"purge-lease-release-held"_q));
			return {};
		}
	}
	return step;
}

bool DesktopArchiveStore::MaintenancePending() const {
	return _impl->maintenancePending;
}

bool DesktopArchiveStore::MaintenanceHeld() const {
	return _impl->status == DesktopArchiveStatus::MaintenanceHeld || _impl->garbageHeld;
}

bool DesktopArchiveStore::RetryMaintenance() {
	auto &self = *_impl;
	if (!self.Authorized()) {
		return false;
	}
	const auto record = InspectDesktopArchiveMaintenance(self.database, epoch(), security());
	if (!record.valid) {
		return false;
	}
	self.maintenancePending = record.purge.has_value();
	self.status = self.maintenancePending ? DesktopArchiveStatus::Maintenance : DesktopArchiveStatus::Ready;
	self.garbagePending = true;
	self.garbageHeld = false;
	return ExpireDeletedViews(true) && (self.maintenancePending || (RecoverMediaLeases() && RecoverCaptureAttention()));
}

bool DesktopArchiveStore::ContinueGarbage(bool &more) {
	auto &self = *_impl;
	more = self.garbagePending;
	if (!Current() || !more) {
		return !more;
	}
	const auto result = ContinueDesktopArchiveGarbage(self.database, epoch(), security(), directory(),
		[this] { return Current(); }, self.cancelledJobCursor, self.cancelledJobsComplete,
		[this](const ArchiveSqlProgram &program) { return Execute(program); });
	if (!result.valid || !Current() || !PrivateDatabaseFiles(self.layout)) {
		self.garbageHeld = true;
		self.garbagePending = more = false;
		self.failure = u"purge-cleanup-held"_q;
		return false;
	}
	self.garbagePending = more = !result.complete;
	if (result.complete && self.failure.startsWith(u"purge-"_q)) {
		self.failure.clear();
	}
	return true;
}

}

namespace Purple {

bool DesktopArchiveStore::RecoverCaptureAttention() {
	auto &self = *_impl;
	if (!Current()) {
		return false;
	}
	const auto attention
		= ReadDesktopArchiveAttention(self.layout.root, self.epoch, self.security);
	if (!attention.valid) {
		self.Fail(DesktopArchiveStatus::Unreadable, u"gap-recovery"_q);
		return false;
	}
	if (attention.present) {
		if (Gap(attention.gap.id, attention.gap.reason, attention.gap.observedAt)) {
			static_cast<void>(
				DesktopArchiveRemoveFile(self.layout.root + u"/retention-gap"_q));
		}
	}
	return true;
}

}

namespace Purple {

bool DesktopArchiveStore::GarbagePending() const {
	return _impl->garbagePending && !_impl->garbageHeld;
}

}

namespace Purple {

bool DesktopArchiveStore::MediaPublicationAllowed(const QString &hash) {
	auto &self = *_impl;
	if (!IsArchiveHash(hash) || !Current() || !self.database.BeginRead()) {
		return false;
	}
	const auto allowed = [&] {
		if (!self.Header()) {
			return false;
		}
		const auto token = ArchiveSqlToken(epoch().account, ArchiveIndexKind::Blob, hash.toUtf8(), security());
		auto query = ArchiveSqlStatement{u"SELECT state,envelope FROM blobs WHERE blob_id=?"_q,
			{token}, ArchiveSqlExpectation::Rows};
		query.rows.maximum = 1;
		const auto read = self.database.Read(query);
		if (token.isEmpty() || !read.ok) {
			return false;
		}
		if (read.rows.empty()) {
			return self.database.Read({u"SELECT 1 FROM archive_meta WHERE singleton=1 AND maintenance_id='' "
				"AND NOT EXISTS(SELECT 1 FROM blobs WHERE blob_id=?)"_q,
				{token}, ArchiveSqlExpectation::OneRow}).ok;
		}
		const auto &values = read.rows[0];
		const auto state = Value<int64>(values, 0);
		const auto frame = Value<QByteArray>(values, 1);
		const auto row = values.size() == 2 && state && frame
			? self.Decode(ArchiveRecordKind::Blob, token, *frame) : std::nullopt;
		if (!row || row->row.body || !row->row.preserved.isEmpty()) {
			return false;
		}
		const auto &blob = std::get<ArchiveBlob>(row->row.value);
		return blob.id == hash && blob.state == ArchiveBlobState::Complete && *state == int64(blob.state);
	}();
	return self.database.EndRead() && allowed && Current();
}

}

namespace Purple {

DesktopArchiveHistoryPage DesktopArchiveStore::History(const ArchiveMessageKey &key,
		int limit, const QString &beforeOrder, std::optional<uint64_t> revision,
		const QString &id, std::function<void()> revoked) {
	auto &self = *_impl;
	ExpireHistory();
	auto found = self.histories.find(id);
	if (!id.isEmpty() && (found == self.histories.end() || found->second.key != key
		|| !CanRunArchiveWork(found->second.epoch, epoch()))) {
		auto result = DesktopArchiveHistoryPage();
		result.status = DesktopArchiveStatus::Stale;
		return result;
	}
	auto page = Versions(key, limit, beforeOrder, revision);
	if (id.isEmpty() && page.status == DesktopArchiveStatus::Ready) {
		if (self.histories.size() >= 64) {
			page.versions.clear();
			page.status = DesktopArchiveStatus::Busy;
			return page;
		}
		const auto token = NewToken();
		if (!Token(token) || self.histories.contains(token)) {
			page.versions.clear();
			page.status = DesktopArchiveStatus::Invalid;
			return page;
		}
		found = self.histories.emplace(token, Impl::HistoryCapability{key, page.epoch,
			page.generation, std::make_shared<std::atomic<bool>>(true), std::move(revoked)}).first;
		page.history = token;
	} else {
		page.history = id;
	}
	if (found != self.histories.end()) {
		page.active = found->second.active;
		ExpireHistory();
		if (!page.active->load()) {
			page.versions.clear();
			page.status = DesktopArchiveStatus::Stale;
		}
	}
	return page;
}

void DesktopArchiveStore::CloseHistory(const QString &id, const ArchiveStoreEpoch &epoch) {
	const auto i = _impl->histories.find(id);
	if (i != _impl->histories.end() && i->second.epoch == epoch) {
		i->second.active->store(false);
		_impl->histories.erase(i);
	}
}

void DesktopArchiveStore::ExpireHistory(bool all) {
	auto &self = *_impl;
	const auto reading = !all && !self.histories.empty() && Current() && self.database.BeginRead();
	const auto header = reading ? self.Header() : std::nullopt;
	const auto account = header ? std::get<ArchiveState>(header->row.value).accountGeneration : 0;
	for (auto i = self.histories.begin(); i != self.histories.end();) {
		auto &capability = i->second;
		const auto chat = header && capability.active->load()
			? DesktopArchiveReadChatGeneration(self.database, epoch().account,
				capability.key.chat, security()) : std::nullopt;
		if (chat && account == capability.generation.account && *chat == capability.generation.chat
			&& CanRunArchiveWork(capability.epoch, epoch())) {
			++i;
		} else {
			capability.active->store(false);
			if (capability.revoked) capability.revoked();
			i = self.histories.erase(i);
		}
	}
	if (reading && !self.database.EndRead()) {
		ExpireHistory(true);
	}
}

}
