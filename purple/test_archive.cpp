#include "base/weak_ptr.h"
#include "purple/purple_archive_attention.h"
#include "purple/purple_archive_capture.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_archive_journal.h"
#include "purple/purple_archive_media_lifecycle.h"
#include "purple/purple_archive_sql_maintenance.h"
#include "purple/purple_desktop_archive_store.h"
#include "purple/purple_sync_json.h"
#include "tests/archive_test_support.h"
#include <QtCore/QBuffer>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <array>
#include <atomic>
#include <cstring>
#include <set>
#include <thread>

using namespace Purple;
using namespace ArchiveTest;

namespace {

DesktopArchiveOptions Options(const QString &root, int64 account = 123) {
	const auto profile = root + u"/profile"_q;
	CHECK(DesktopArchiveDirectory(profile));
	auto options = DesktopArchiveOptions{
		root + u"/archives"_q, profile, {0, account}, u"0"_q, QByteArray(256, 'k'), true};
	options.loginAuthority = QByteArray(32, 'a');
	options.loginAuthKeyId = QByteArray(16, '1');
	options.loginAuthKeyIds = options.loginAuthKeyId;
	return options;
}

void KeySetMigrationAndRebind() {
	Section = "all-DC anchor migration and atomic rebind";
	auto temporary = QTemporaryDir();
	auto options = Options(temporary.path());
	const auto k1 = QByteArray(16, '1');
	const auto k2 = QByteArray(16, '2');
	options.loginAuthKeyIds = k1 + ',' + k2;
	const auto selected = DesktopArchiveSelectLoginAuthority(
		options.loginAuthority, options.loginAuthKeyIds, options.loginAuthKeyIds);
	CHECK(selected && selected->nonce == options.loginAuthority);
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	CHECK(store.Capture(Event(Empty(), u"migration-secret"_q, 1, u"keep across DCs"_q)).retained);
	const auto epoch = store.epoch();
	const auto root = store.directory();
	CHECK(store.Close());
	const auto profile = QFileInfo(QFileInfo(QFileInfo(root).absolutePath()).absolutePath()).absolutePath();
	const auto catalogue = profile + u"/catalogue.json"_q;
	const auto before = DesktopArchiveRead(catalogue, 1024 * 1024);
	const auto wrap = DesktopArchiveRead(root + u"/wrapped-key"_q, 8192);
	CHECK(before && wrap);
	options.loginAuthKeyId = k2;
	CHECK(DesktopArchiveDirectory(profile + u"/catalogue.lock"_q));
	CHECK(store.Open(options) == DesktopArchiveStatus::IoError);
	CHECK(store.failure() == u"login-rebind-held"_q);
	CHECK(DesktopArchiveRead(catalogue, 1024 * 1024) == before);
	CHECK(DesktopArchiveRead(root + u"/wrapped-key"_q, 8192) == wrap);
	CHECK(!QDir(root).entryList({u"wrapped-key-*"_q}, QDir::Files).empty());
	CHECK(store.Close());
	CHECK(QDir().rmdir(profile + u"/catalogue.lock"_q));
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(store.epoch() == epoch);
	CHECK(store.Versions(Key()).versions.size() == 1);
	CHECK(store.Close());
	const auto after = DesktopArchiveRead(catalogue, 1024 * 1024);
	CHECK(after && after != before);
	if (after) {
		const auto record = QJsonDocument::fromJson(*after).object().value(u"accounts"_q)
			.toObject().value(u"0-123"_q).toObject();
		CHECK(record.value(u"loginAuthKeyId"_q).toString().toLatin1() == k2);
		CHECK(record.value(u"loginAuthKeys"_q).toString().toLatin1() == k1 + ',' + k2);
		CHECK(record.value(u"wrappedKey"_q) != QJsonValue(u"initial"_q));
	}
	const auto keep = DesktopArchiveSelectLoginAuthority(
		options.loginAuthority, options.loginAuthKeyIds, k2);
	CHECK(keep && keep->nonce == options.loginAuthority);
	options.loginAuthKeyIds = k2;
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(store.epoch() == epoch);
	CHECK(store.Versions(Key()).versions.size() == 1);
	CHECK(store.Close());
	options.loginAuthKeyIds = k1 + ',' + k2;
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(store.epoch() == epoch);
	CHECK(store.Close());
	options.loginAuthKeyIds = k2;
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(store.Versions(Key()).versions.size() == 1);
	CHECK(store.Close());
	const auto fresh = DesktopArchiveSelectLoginAuthority(options.loginAuthority,
		options.loginAuthKeyIds, QByteArray(16, '3'));
	CHECK(fresh && fresh->nonce != options.loginAuthority);
	options.loginAuthority = fresh->nonce;
	options.loginAuthKeyId = QByteArray(16, '3');
	options.loginAuthKeyIds = fresh->authKeys;
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(store.epoch() != epoch);
	CHECK(store.Versions(Key()).versions.empty());
	CHECK(!QFileInfo::exists(root));
	CHECK(store.Remove());
	CHECK(!DesktopArchiveLoginKeysValid(k2 + ',' + k1));
	CHECK(!DesktopArchiveLoginKeysValid(k1 + ',' + k1));
}

void ExplicitDiscardRecovery() {
	Section = "explicit fenced discard of refused account only";
	auto temporary = QTemporaryDir();
	auto options = Options(temporary.path());
	auto other = Options(temporary.path(), 456);
	other.slot = u"1"_q;
	auto store = DesktopArchiveStore();
	auto second = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(store.Capture(Event(Empty(), u"discard-me"_q, 1, u"first secret"_q)).retained);
	const auto oldRoot = store.directory();
	CHECK(store.Close());
	CHECK(second.Open(other) == DesktopArchiveStatus::Ready);
	auto event = Event(Empty(other.account), u"keep-other"_q, 1,
		u"other secret"_q, ArchiveOrigin::Baseline, Key(other.account));
	CHECK(second.Capture(event).retained);
	const auto otherRoot = second.directory();
	const auto otherEpoch = second.epoch();
	CHECK(second.Close());
	const auto profile = QFileInfo(QFileInfo(QFileInfo(oldRoot).absolutePath()).absolutePath()).absolutePath();
	const auto catalogue = profile + u"/catalogue.json"_q;
	const auto bytes = DesktopArchiveRead(catalogue, 1024 * 1024);
	CHECK(bytes);
	auto doc = QJsonDocument::fromJson(*bytes).object();
	auto accounts = doc.value(u"accounts"_q).toObject();
	auto record = accounts.value(u"0-123"_q).toObject();
	record.remove(u"loginAuthority"_q);
	accounts.insert(u"0-123"_q, record);
	doc.insert(u"accounts"_q, accounts);
	CHECK(DesktopArchiveWrite(catalogue, QJsonDocument(doc).toJson(QJsonDocument::Compact)));
	CHECK(store.Open(options) == DesktopArchiveStatus::Unreadable);
	CHECK(QFileInfo::exists(oldRoot));
	CHECK(store.Close());
	const auto token = DesktopArchiveStore::PrepareDiscard(options);
	CHECK(token);
	if (!token) {
		return;
	}
	CHECK(!DesktopArchiveStore::DiscardAccount(other, *token));
	auto stale = options;
	stale.loginAuthority = QByteArray(32, 'x');
	CHECK(!DesktopArchiveStore::DiscardAccount(stale, *token));
	stale = options;
	stale.active = [] { return false; };
	CHECK(!DesktopArchiveStore::DiscardAccount(stale, *token));
	stale = options;
	stale.maintenanceAllowed = [] { return false; };
	CHECK(!DesktopArchiveStore::DiscardAccount(stale, *token));
	CHECK(!DesktopArchiveStore::DiscardAccount(options, *token, 1));
	CHECK(QFileInfo::exists(oldRoot));
	CHECK(!DesktopArchiveStore::DiscardAccount(options, *token));
	const auto continuation = DesktopArchiveStore::PrepareDiscard(options);
	CHECK(continuation);
	CHECK(continuation && DesktopArchiveStore::DiscardAccount(options, *continuation));
	CHECK(!QFileInfo::exists(oldRoot));
	options.loginAuthority = QByteArray(32, 'b');
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(store.Versions(Key()).versions.empty());
	const auto newRoot = store.directory();
	CHECK(store.Close());
	CHECK(!DesktopArchiveStore::DiscardAccount(options, *token));
	auto oldContext = options;
	oldContext.loginAuthority = QByteArray(32, 'a');
	CHECK(!DesktopArchiveStore::DiscardAccount(oldContext, *token));
	CHECK(QFileInfo::exists(newRoot));
	CHECK(second.Open(other) == DesktopArchiveStatus::Ready);
	CHECK(second.directory() == otherRoot);
	CHECK(second.epoch() == otherEpoch);
	auto key = Key();
	key.account = other.account;
	CHECK(second.Versions(key).versions.size() == 1);
	CHECK(second.Remove());
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(store.Remove());
	auto virgin = QTemporaryDir();
	const auto empty = Options(virgin.path());
	const auto emptyToken = DesktopArchiveStore::PrepareDiscard(empty);
	CHECK(emptyToken);
	CHECK(emptyToken && DesktopArchiveStore::DiscardAccount(empty, *emptyToken));
	CHECK(store.Open(empty) == DesktopArchiveStatus::Ready);
	CHECK(store.Remove());
}

void AdmissionAuthentication() {
	Section = "mandatory authenticated admission refuses corruption without deletion";
	auto temporary = QTemporaryDir();
	const auto options = Options(temporary.path());
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	CHECK(store.Capture(Event(Empty(), u"admission-secret"_q, 1, u"keep secret"_q)).retained);
	const auto root = store.directory();
	CHECK(store.Close());
	const auto profile = QFileInfo(QFileInfo(QFileInfo(root).absolutePath())
		.absolutePath()).absolutePath();
	const auto catalogue = profile + u"/catalogue.json"_q;
	const auto original = DesktopArchiveRead(catalogue, 1024 * 1024);
	const auto database = DesktopArchiveRead(root + u"/archive.sqlite"_q, 4 * 1024 * 1024);
	const auto wrapped = DesktopArchiveRead(root + u"/wrapped-key"_q, 8192);
	CHECK(original && database && wrapped);
	if (!original || !database || !wrapped) {
		return;
	}
	const auto document = QJsonDocument::fromJson(*original).object();
	const auto accounts = document.value(u"accounts"_q).toObject();
	const auto account = *AccountKeyText(options.account.environment, options.account.userId);
	const auto record = accounts.value(account).toObject();
	const auto sign = [&](QJsonObject value) {
		value.remove(u"loginAdmissionMac"_q);
		auto plan = ArchiveKeyDerivationFor(options.account,
			QFileInfo(profile).fileName(), QFileInfo(profile).fileName(),
			ArchiveKeyPurpose::DesktopWrapping);
		CHECK(plan);
		plan->info += QByteArray("/login-admission-v1");
		const auto key = DesktopArchiveCrypto::Derive(options.localKey, *plan);
		const auto canonical = CanonicalizeSyncJson(QJsonDocument(value).toJson(QJsonDocument::Compact));
		CHECK(key && canonical);
		if (key && canonical) {
			const auto mac = DesktopArchiveCrypto::Hmac(*key, canonical.canonical);
			CHECK(mac);
			if (mac) {
				value.insert(u"loginAdmissionMac"_q, QString::fromLatin1(mac->toHex()));
			}
		}
		return value;
	};
	const auto refuses = [&](const QJsonObject &value) {
		auto changed = document;
		auto changedAccounts = accounts;
		changedAccounts.insert(account, value);
		changed.insert(u"accounts"_q, changedAccounts);
		CHECK(DesktopArchiveWrite(catalogue, QJsonDocument(changed).toJson(QJsonDocument::Compact)));
		auto nextLogin = options;
		nextLogin.loginAuthority = QByteArray(32, 'b');
		nextLogin.loginAuthKeyId = QByteArray(16, '2');
		nextLogin.loginAuthKeyIds = nextLogin.loginAuthKeyId;
		CHECK(store.Open(nextLogin) != DesktopArchiveStatus::Ready);
		CHECK(!store.Current());
		CHECK(QFileInfo::exists(root));
		CHECK(DesktopArchiveRead(root + u"/wrapped-key"_q, 8192) == wrapped);
		CHECK(DesktopArchiveRead(root + u"/archive.sqlite"_q, 4 * 1024 * 1024) == database);
		CHECK(store.Close());
		CHECK(DesktopArchiveWrite(catalogue, *original));
	};
	CHECK(sign(record) == record);
	for (auto i = record.begin(); i != record.end(); ++i) {
		auto missing = record;
		missing.remove(i.key());
		refuses(missing);
		if (i.key() != u"loginAdmissionMac"_q) {
			refuses(sign(missing));
		}
		auto wrongType = record;
		wrongType.insert(i.key(), QJsonArray());
		refuses(wrongType);
		if (i.key() != u"loginAdmissionMac"_q) {
			refuses(sign(wrongType));
		}
	}
	auto extra = record;
	extra.insert(u"futureAdmission"_q, true);
	refuses(extra);
	refuses(sign(extra));
	auto version = record;
	version.insert(u"loginAuthorityVersion"_q, 4);
	refuses(sign(version));
	auto badMac = record;
	badMac.insert(u"loginAdmissionMac"_q, QString(64, '0'));
	refuses(badMac);
	auto lostNonce = options;
	lostNonce.loginAuthority = QByteArray(32, 'b');
	CHECK(store.Open(lostNonce) == DesktopArchiveStatus::Unreadable);
	CHECK(store.failure() == u"login-authority-mismatch"_q);
	CHECK(QFileInfo::exists(root));
	CHECK(DesktopArchiveRead(root + u"/wrapped-key"_q, 8192) == wrapped);
	CHECK(DesktopArchiveRead(root + u"/archive.sqlite"_q, 4 * 1024 * 1024) == database);
	CHECK(store.Close());
	lostNonce.loginAuthority.clear();
	CHECK(store.Open(lostNonce) == DesktopArchiveStatus::Unreadable);
	CHECK(QFileInfo::exists(root));
	CHECK(store.Close());
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(store.directory() == root);
	CHECK(store.Versions(Key()).versions.size() == 1);
	CHECK(store.Remove());
}

void MtpLoginBinding() {
	Section = "new MTP identity rejects stale persisted nonce and old wrapping";
	auto temporary = QTemporaryDir();
	auto options = Options(temporary.path());
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	CHECK(store.Capture(Event(Empty(), u"old-login-secret"_q, 1, u"old secret"_q)).retained);
	const auto oldEpoch = store.epoch();
	const auto oldKeyId = store.security().keyId;
	const auto oldRoot = store.directory();
	const auto wrapped = DesktopArchiveRead(oldRoot + u"/wrapped-key"_q, 8192);
	CHECK(store.Close());
	const auto profilePath = QFileInfo(QFileInfo(QFileInfo(oldRoot).absolutePath())
		.absolutePath()).absolutePath();
	const auto profile = QFileInfo(profilePath).fileName();
	auto plan = ArchiveKeyDerivationFor(options.account, oldEpoch.storeId,
		profile, ArchiveKeyPurpose::DesktopWrapping);
	CHECK(plan && wrapped);
	if (plan && wrapped) {
		plan->info += QByteArray("/login-authority/") + options.loginAuthority
			+ QByteArray("/mtp-auth-keys/") + options.loginAuthKeyIds
			+ QByteArray("/mtp-main-key/") + options.loginAuthKeyId;
		const auto oldKey = DesktopArchiveCrypto::Derive(options.localKey, *plan);
		auto crypto = DesktopArchiveCrypto();
		CHECK(oldKey && crypto.AddKey(u"desktop-wrap-v1"_q, *oldKey));
		CHECK(UnwrapArchiveDataKey(options.account, oldEpoch.storeId, oldKeyId,
			u"desktop-wrap-v1"_q, *wrapped, crypto.callbacks()));
		options.loginAuthKeyId = QByteArray(16, '2');
		options.loginAuthKeyIds = options.loginAuthKeyId;
		plan->info.chop(16 + 14 + 16);
		plan->info += options.loginAuthKeyIds + QByteArray("/mtp-main-key/") + options.loginAuthKeyId;
		const auto key = DesktopArchiveCrypto::Derive(options.localKey, *plan);
		auto nextCrypto = DesktopArchiveCrypto();
		CHECK(key && nextCrypto.AddKey(u"desktop-wrap-v1"_q, *key));
		CHECK(!UnwrapArchiveDataKey(options.account, oldEpoch.storeId, oldKeyId,
			u"desktop-wrap-v1"_q, *wrapped, nextCrypto.callbacks()));
	}
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(store.epoch() != oldEpoch);
	CHECK(store.Versions(Key()).versions.empty());
	CHECK(!QFileInfo::exists(oldRoot));
	CHECK(store.Remove());
}

void LoginAuthorityRevocation() {
	Section = "logout authority survives total archive IO failure";
	auto temporary = QTemporaryDir();
	auto options = Options(temporary.path());
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	CHECK(store.Capture(Event(Empty(), u"before-logout"_q, 1, u"old secret"_q)).retained);
	const auto oldEpoch = store.epoch();
	const auto oldRoot = store.directory();
	const auto oldSecurity = store.security();
	const auto wrapped = DesktopArchiveRead(oldRoot + u"/wrapped-key"_q, 8192);
	CHECK(wrapped);
	CHECK(store.Close());
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(store.epoch() == oldEpoch);
	CHECK(store.Versions(Key()).versions.size() == 1);
	CHECK(store.Close());
	const auto intents = QFileInfo(QFileInfo(QFileInfo(oldRoot).absolutePath())
		.absolutePath()).absolutePath() + u"/intents"_q;
#ifndef Q_OS_WIN
	CHECK(QFile::setPermissions(intents, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
	CHECK(!DesktopArchiveStore::RemoveAccount(options, 64));
	CHECK(QDir(intents).isEmpty());
#endif
	options.loginAuthority = QByteArray(32, 'b');
	options.loginAuthKeyId = QByteArray(16, '2');
		options.loginAuthKeyIds = options.loginAuthKeyId;
#ifndef Q_OS_WIN
	auto denied = DesktopArchiveStore();
	CHECK(denied.Open(options) != DesktopArchiveStatus::Ready);
	CHECK(!denied.Current());
	CHECK(denied.Versions(Key()).versions.empty());
	CHECK(denied.Close());
	CHECK(QFile::setPermissions(intents,
		QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
#endif
	CHECK(DesktopArchiveStore::Recover(options));
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(store.epoch().storeId != oldEpoch.storeId);
	CHECK(store.Versions(Key()).versions.empty());
	CHECK(!QFileInfo::exists(oldRoot));
	if (wrapped) {
		CHECK(!UnwrapArchiveDataKey(options.account, oldEpoch.storeId, oldSecurity.keyId,
			u"desktop-wrap-v1"_q, *wrapped, oldSecurity.crypto));
		CHECK(!UnwrapArchiveDataKey(options.account, oldEpoch.storeId, oldSecurity.keyId,
			u"desktop-wrap-v1"_q, *wrapped, store.security().crypto));
	}
	CHECK(store.Remove());
}

void StaleLogoutCleanup() {
	Section = "stale logout cannot remove the new login";
	auto temporary = QTemporaryDir();
	auto old = Options(temporary.path());
	auto store = DesktopArchiveStore();
	CHECK(store.Open(old) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	old.removalStore = store.epoch().storeId;
	old.removalAuthority = QString::fromLatin1(QCryptographicHash::hash(
		old.loginAuthority, QCryptographicHash::Sha256).toHex());
	CHECK(store.Close());
	auto current = old;
	current.removalStore.clear();
	current.removalAuthority.clear();
	current.loginAuthority = QByteArray(32, 'b');
	current.loginAuthKeyId = QByteArray(16, '2');
	current.loginAuthKeyIds = current.loginAuthKeyId;
	CHECK(store.Open(current) == DesktopArchiveStatus::Ready);
	CHECK(store.Capture(Event(Empty(), u"new-login"_q, 1, u"new secret"_q)).retained);
	const auto directory = store.directory();
	CHECK(store.Close());
	CHECK(DesktopArchiveStore::RemoveAccount(old));
	old.removalStore.clear();
	CHECK(DesktopArchiveStore::RemoveAccount(old));
	CHECK(store.Open(current) == DesktopArchiveStatus::Ready);
	CHECK(store.directory() == directory);
	CHECK(store.Versions(Key()).versions.size() == 1);
	CHECK(store.Remove());
}

void HistoricalObservationRevision() {
	Section = "authenticated historical observation revision";
	auto temporary = QTemporaryDir();
	const auto options = Options(temporary.path());
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	auto journal = DesktopArchiveJournal(*store.JournalAccess());
	CHECK(journal.Open());
	auto original = Event(Empty(), u"original"_q, 0, u"A"_q);
	AddMedia(original);
	auto refresh = original;
	refresh.id = u"quality-refresh"_q;
	auto edit = Event(Empty(), u"authored-edit"_q, 0, u"B"_q, ArchiveOrigin::AuthoritativeEdit);
	AddMedia(edit);
	CHECK(journal.Append(original));
	CHECK(journal.Append(refresh));
	CHECK(journal.Append(edit));
	CHECK(journal.Drain(store));
	const auto hash = ArchiveContentHash(*original.content);
	CHECK(store.ObservedMediaVersion(Key(), refresh.id, hash, u"document:555"_q)
		== std::optional(original.id));
	CHECK(store.ObservedMediaVersion(Key(), original.id, hash, u"document:555"_q)
		== std::optional(original.id));
	CHECK(!store.ObservedMediaVersion(Key(), refresh.id,
		ArchiveContentHash(*edit.content), u"document:555"_q));
	CHECK(!store.ObservedMediaVersion(Key(), refresh.id, hash, u"document:556"_q));
	CHECK(!store.ObservedMediaVersion(Key({0, 456}), refresh.id, hash, u"document:555"_q));
	CHECK(!store.ObservedMediaVersion(Key(), u"unknown"_q, hash, u"document:555"_q));
	auto bytes = QByteArray("historical revision bytes");
	auto source = QBuffer(&bytes);
	CHECK(source.open(QIODevice::ReadOnly));
	const auto version = store.ObservedMediaVersion(Key(), refresh.id, hash, u"document:555"_q);
	CHECK(version && store.CopyMedia(Key(), *version, u"document:555"_q, source,
		{QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()), uint64_t(bytes.size())}));
	CHECK(store.ReadMedia(Key(), original.id, u"document:555"_q,
		[&](const QByteArray &chunk) { return chunk == bytes; }));
	CHECK(store.Remove());
}

void ShortSendAcknowledgement() {
	Section = "genuine short send acknowledgement";
	const auto ack = MTP_updateShortSentMessage(
		MTP_flags(MTPDupdateShortSentMessage::Flag::f_out), MTP_int(67), MTP_int(300),
		MTP_int(1), MTP_int(1700000000), MTP_messageMediaEmpty(),
		MTP_vector<MTPMessageEntity>(), MTP_int(0));
	auto observed = Event(Empty(), u"short-ack"_q, 0, u"acknowledged content"_q);
	const auto event = DesktopArchiveShortSentSnapshot(observed, ack.c_updateShortSentMessage());
	CHECK(event);
	if (!event) {
		return;
	}
	CHECK(event->origin == ArchiveOrigin::Baseline);
	CHECK(event->completeness == ArchiveCompleteness::Partial);
	CHECK(!event->missing.content.empty());
	CHECK(event->observation.serverTime == 1700000000);
	CHECK(event->observation.context.value(u"acceptedPts"_q).toInt() == 300);
	CHECK(event->observation.context.value(u"desktopSendMetadata"_q).toBool());
	CHECK(!event->observation.payload.isEmpty());
	const auto &payload = event->observation.payload;
	CHECK(payload.size() % sizeof(mtpPrime) == 0);
	auto wire = mtpBuffer(payload.size() / sizeof(mtpPrime));
	std::memcpy(wire.data(), payload.constData(), payload.size());
	auto position = wire.constData();
	auto decoded = MTPUpdates();
	const auto parsed = decoded.read(position, position + wire.size());
	CHECK(parsed);
	CHECK(parsed && decoded.type() == mtpc_updateShortSentMessage);
	if (parsed && decoded.type() == mtpc_updateShortSentMessage) {
		CHECK(decoded.c_updateShortSentMessage().vid().v == 67);
	}
	const auto unsupported = MTP_updateShortSentMessage(
		MTP_flags(MTPDupdateShortSentMessage::Flag::f_out
			| MTPDupdateShortSentMessage::Flag::f_media), MTP_int(67), MTP_int(301),
		MTP_int(1), MTP_int(1700000001), MTP_messageMediaUnsupported(),
		MTP_vector<MTPMessageEntity>(), MTP_int(0));
	const auto partial = DesktopArchiveShortSentSnapshot(observed,
		unsupported.c_updateShortSentMessage());
	CHECK(partial && partial->completeness == ArchiveCompleteness::Partial);

	observed.key.messageId = -1;
	CHECK(!DesktopArchiveShortSentSnapshot(observed, ack.c_updateShortSentMessage()));
	observed.key.messageId = 68;
	CHECK(!DesktopArchiveShortSentSnapshot(observed, ack.c_updateShortSentMessage()));
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	auto journal = DesktopArchiveJournal(*store.JournalAccess());
	CHECK(journal.Open());
	CHECK(journal.Append(*event));
	CHECK(journal.Drain(store));
	auto page = store.Versions(Key());
	CHECK(page.versions.size() == 1);
	CHECK(page.versions.front().version.content.text == u"acknowledged content"_q);
	auto repeated = *event;
	repeated.id = u"short-ack-repeat"_q;
	repeated.content->text = u"transport acknowledgement is not an edit"_q;
	CHECK(journal.Append(repeated));
	CHECK(journal.Drain(store));
	page = store.Versions(Key());
	CHECK(page.versions.size() == 1);
	CHECK(page.versions.front().version.content.text == u"acknowledged content"_q);
	CHECK(store.Remove());
}

void Store() {
	Section = "store";
	auto temporary = QTemporaryDir();
	CHECK(temporary.isValid());
	const auto options = Options(temporary.path());
	auto store = DesktopArchiveStore();
	const auto opened = store.Open(options);
	std::printf("open=%d %s\n", int(opened), store.failure().toUtf8().constData());
	CHECK(opened == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	const auto directory = store.directory();
	const auto epoch = store.epoch();
	const auto key = Key();
	const auto baseline = Event(Empty(), u"first"_q, 1);
	CHECK(store.Capture(baseline).retained);
	auto page = store.Versions(key, 1);
	CHECK(page.status == DesktopArchiveStatus::Ready);
	CHECK(page.versions.size() == 1);
	if (page.versions.empty()) {
		return;
	}
	CHECK(page.versions.front().version.content.text == u"A"_q);
	CHECK(store.Capture(baseline).retained);
	CHECK(store
			.Capture(
				Event(Empty(), u"second"_q, 2, u"B"_q, ArchiveOrigin::AuthoritativeEdit))
			.retained);
	CHECK(store
			.Capture(
				Event(Empty(), u"third"_q, 3, u"A"_q, ArchiveOrigin::AuthoritativeEdit))
			.retained);
	CHECK(store.Versions(key).versions.size() == 3);
	CHECK(store.Versions(key, 1, page.beforeOrder, page.revision).status
		  == DesktopArchiveStatus::Stale);
	for (const auto &phase :
		{u"prepare"_q, u"bind"_q, u"step"_q, u"finalize"_q, u"commit"_q}) {
		store.database().FailForTest(phase);
		CHECK(!store
				.Capture(Event(Empty(), u"failure-"_q + phase, 4, phase,
					ArchiveOrigin::AuthoritativeEdit))
				.retained);
		const auto failurePage = store.Versions(key);
		std::printf("fault=%s current=%d rows=%zu status=%d failure=%s\n",
			phase.toUtf8().constData(), store.Current(), failurePage.versions.size(),
			int(failurePage.status), store.failure().toUtf8().constData());
		CHECK(failurePage.versions.size() == 3);
	}
	const auto large = QString(40000, 'x');
	CHECK(store
			.Capture(
				Event(Empty(), u"chunked"_q, 4, large, ArchiveOrigin::AuthoritativeEdit))
			.retained);
	page = store.Versions(key, 1);
	CHECK(page.versions.size() == 1);
	CHECK(!page.versions.empty() && page.versions.front().version.content.text == large);
	const auto excessive = Event(Empty(), u"excessive"_q, 5,
		QString(4 * 1024 * 1024 + 1, 'z'), ArchiveOrigin::AuthoritativeEdit);
	CHECK(store.Capture(excessive).status == DesktopArchiveStatus::RetentionGap);
	CHECK(store.Versions(key).versions.size() == 4);
	const auto usage = store.Usage();
	CHECK(usage && TotalArchiveUsage(*usage).value_or(0) > 0);
	CHECK(store.Close());
	auto wrong = options;
	wrong.localKey[0] = 'X';
	CHECK(store.Open(wrong) == DesktopArchiveStatus::Unreadable);
	CHECK(store.Close());
	const auto reopened = store.Open(options);
	std::printf(
		"reopen=%d failure=%s\n", int(reopened), store.failure().toUtf8().constData());
	CHECK(reopened == DesktopArchiveStatus::Ready);
	CHECK(store.epoch() == epoch);
	CHECK(store.Versions(key).versions.size() == 4);
	auto other = DesktopArchiveStore();
	auto otherOptions = Options(temporary.path(), 456);
	CHECK(other.Open(otherOptions) == DesktopArchiveStatus::Ready);
	CHECK(other.Versions(key).status != DesktopArchiveStatus::Ready);
	const auto otherDirectory = other.directory();
	auto alternate = DesktopArchiveStore();
	auto alternateOptions = options;
	alternateOptions.profileNamespace = u"alternate-data-name"_q;
	alternateOptions.localKey[0] = 'Q';
	CHECK(alternate.Open(alternateOptions) == DesktopArchiveStatus::Ready);
	CHECK(alternate.directory() != directory);
	CHECK(alternate.Current());
	CHECK(alternate.Remove());

	CHECK(!store.Remove(2));
	CHECK(!store.Current());
	CHECK(store.Close());
	CHECK(DesktopArchiveStore::Recover(options));
	CHECK(!QFileInfo::exists(directory));
	CHECK(other.Current());
	CHECK(QFileInfo::exists(otherDirectory));
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(store.epoch().storeId != epoch.storeId);
	CHECK(store.epoch().generation > epoch.generation);
	CHECK(store.Versions(key).versions.empty());
	CHECK(store.Remove());
	CHECK(other.Remove());
	for (auto cut = 1; cut != 14; ++cut) {
		CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
		const auto removedPath = store.directory();
		CHECK(store.Capture(baseline).retained);
		const auto removed = store.Remove(cut);
		CHECK(store.Close());
		CHECK(DesktopArchiveStore::Recover(options));
		CHECK(!QFileInfo::exists(removedPath));
		static_cast<void>(removed);
	}
}

void ConcurrentAccounts() {
	Section = "parallel account bootstrap and catalogue updates";
	auto temporary = QTemporaryDir();
	const auto options = Options(temporary.path());
	auto start = std::atomic<bool>(false);
	auto entered = std::atomic<int>(0);
	auto opened = std::atomic<int>(0);
	struct Result {
		DesktopArchiveStatus status;
		bool retained = false;
		bool removed = false;
	};
	auto results = std::array<Result, 4>();
	auto workers = std::vector<std::thread>();
	for (auto index = 0; index != int(results.size()); ++index) {
		workers.emplace_back([&, index] {
			auto target = options;
			target.account.userId += index;
			target.slot = QString::number(index);
			entered.fetch_add(1);
			while (!start.load()) {
				std::this_thread::yield();
			}
			auto store = DesktopArchiveStore();
			results[index].status = store.Open(target);
			if (store.Current()) {
				results[index].retained
					= store
						  .Capture(Event(Empty(target.account), u"parallel"_q, 1,
							  u"body"_q, ArchiveOrigin::Baseline, Key(target.account)))
						  .retained;
			}
			opened.fetch_add(1);
			while (opened.load() != int(results.size())) {
				std::this_thread::yield();
			}
			results[index].removed = store.Remove();
		});
	}
	while (entered.load() != int(results.size())) {
		std::this_thread::yield();
	}
	start = true;
	for (auto &worker : workers) {
		worker.join();
	}
	for (const auto &result : results) {
		CHECK(result.status == DesktopArchiveStatus::Ready);
		CHECK(result.retained);
		CHECK(result.removed);
	}
	CHECK(DesktopArchiveStore::Recover(options));
}

void Refusals() {
	Section = "read-only refusal and keyless removal";
	for (const auto version : {int64(3), int64(2147483648LL)}) {
		auto temporary = QTemporaryDir();
		const auto options = Options(temporary.path());
		auto store = DesktopArchiveStore();
		CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
		if (!store.Current()) {
			continue;
		}
		const auto path = store.directory();
		CHECK(store.Capture(Event(Empty(), u"first"_q, 1)).retained);
		CHECK(store.database()
				.Read({u"UPDATE archive_meta SET schema_version=?"_q, {version},
					ArchiveSqlExpectation::OneChange})
				.ok);
		CHECK(store.Close());
		const auto original
			= DesktopArchiveRead(path + u"/archive.sqlite"_q, 4 * 1024 * 1024);
		CHECK(original);
		CHECK(store.Open(options) == DesktopArchiveStatus::ReadOnly);
		CHECK(store.Close());
		CHECK(
			DesktopArchiveRead(path + u"/archive.sqlite"_q, 4 * 1024 * 1024) == original);
		auto keyless = options;
		keyless.localKey.clear();
		CHECK(DesktopArchiveStore::RemoveAccount(keyless));
		CHECK(!QFileInfo::exists(path));
	}
	{
		auto temporary = QTemporaryDir();
		const auto options = Options(temporary.path());
		auto store = DesktopArchiveStore();
		CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
		if (!store.Current()) {
			return;
		}
		const auto path = store.directory();
		CHECK(store.Close());
		CHECK(DesktopArchiveRemoveFile(path + u"/wrapped-key"_q));
		CHECK(store.Open(options) == DesktopArchiveStatus::Unreadable);
		CHECK(store.Close());
		CHECK(!QFileInfo::exists(path + u"/wrapped-key"_q));
		CHECK(DesktopArchiveStore::RemoveAccount(options));
	}
}

void SchemaAdmission() {
	Section = "format2 exact shape admission";
	const auto mutations = std::vector<QString>{
		u"UPDATE archive_meta SET schema_version=1"_q,
		u"UPDATE archive_meta SET schema_fingerprint='changed'"_q,
		u"CREATE INDEX unexpected_archive_index ON messages(message_id)"_q,
		u"ALTER TABLE messages ADD COLUMN unexpected TEXT"_q,
		u"ALTER TABLE archive_meta DROP COLUMN schema_fingerprint"_q,
		u"DROP TABLE archive_meta"_q,
	};
	for (const auto &sql : mutations) {
		auto temporary = QTemporaryDir();
		const auto options = Options(temporary.path());
		auto store = DesktopArchiveStore();
		CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
		if (!store.Current()) {
			continue;
		}
		CHECK(store.Capture(Event(Empty(), u"kept"_q, 1)).retained);
		const auto path = store.directory();
		CHECK(store.database()
				.Read({sql, {},
					sql.startsWith(u"UPDATE "_q) ? ArchiveSqlExpectation::OneChange
												 : ArchiveSqlExpectation::Done})
				.ok);
		if (sql == mutations.front()) {
			for (const auto &legacy : std::vector<QString>{u"DROP INDEX deleted_topic"_q,
					 u"ALTER TABLE messages DROP COLUMN topic_token"_q,
					 u"ALTER TABLE messages DROP COLUMN collection"_q,
					 u"ALTER TABLE messages DROP COLUMN representation_token"_q,
					 u"ALTER TABLE archive_meta DROP COLUMN schema_fingerprint"_q}) {
				CHECK(store.database().Read({legacy}).ok);
			}
		}
		CHECK(store.Close());
		const auto before
			= DesktopArchiveRead(path + u"/archive.sqlite"_q, 4 * 1024 * 1024);
		CHECK(before);
		const auto status = store.Open(options);
		CHECK(status
			  == (sql == mutations.back() ? DesktopArchiveStatus::Unreadable
										  : DesktopArchiveStatus::SchemaMismatch));
		CHECK(!store.Current());
		CHECK(!store.Capture(Event(Empty(), u"refused"_q, 2)).retained);
		CHECK(store.Close());
		CHECK(DesktopArchiveRead(path + u"/archive.sqlite"_q, 4 * 1024 * 1024) == before);
		CHECK(DesktopArchiveStore::RemoveAccount(options));
		CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
		CHECK(store.Versions(Key()).versions.empty());
		CHECK(store.Remove());
	}
}

class InterleavedMedia final : public QBuffer {
  public:
	std::function<void()> interleave;

  protected:
	qint64 readData(char* data, qint64 size) override {
		if (interleave) {
			auto run = std::exchange(interleave, {});
			run();
		}
		return QBuffer::readData(data, size);
	}

};

void HistoricalMedia() {
	Section = "exact historical media and job codec";
	for (const auto revise : {false, true}) {
		auto temporary = QTemporaryDir();
		const auto options = Options(temporary.path());
		auto store = DesktopArchiveStore();
		CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
		if (!store.Current()) {
			continue;
		}
		auto first = Event(Empty(), u"v1"_q, 1);
		first.content->media.push_back({u"document:555"_q, u"document"_q});
		CHECK(store.Capture(first).retained);
		const auto firstHash = ArchiveContentHash(*first.content);
		CHECK(store.ObservedMediaVersion(Key(), first.id, firstHash, u"document:555"_q)
			  == first.id);
		auto next = first;
		next.id = u"v2"_q;
		next.order = 2;
		next.origin = ArchiveOrigin::AuthoritativeEdit;
		if (!revise) {
			next.content->text = u"new body"_q;
		}
		const auto bytes = QByteArray("already fetched bytes");
		const auto hash = QString::fromLatin1(
			QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
		auto source = InterleavedMedia();
		source.setData(bytes);
		CHECK(source.open(QIODevice::ReadOnly));
		source.interleave = [&] {
			auto slice = store.ReadSlice(Key(), {}, u"v1"_q);
			CHECK(slice);
			if (!slice) {
				return;
			}
			const auto staged
				= std::find_if(slice->state.jobs.begin(), slice->state.jobs.end(),
					[](const auto &job) { return job.version == u"v1"_q; });
			CHECK(staged != slice->state.jobs.end());
			if (staged != slice->state.jobs.end()) {
				CHECK(staged->revision == u"v1"_q && staged->contentHash == firstHash);
				const auto row = EncodeArchiveRow({*staged});
				CHECK(row);
				if (row) {
					const auto decoded = DecodeArchiveRow(*row);
					CHECK(decoded.row);
					if (decoded.row) {
						CHECK(std::get<ArchiveMediaJob>(decoded.row->value) == *staged);
					}
					for (const auto &binding : {u"revision"_q, u"contentHash"_q}) {
						auto old = QJsonDocument::fromJson(*row).object();
						auto value = old.value(u"value"_q).toObject();
						value.remove(binding);
						old.insert(u"value"_q, value);
						CHECK(!DecodeArchiveRow(
							QJsonDocument(old).toJson(QJsonDocument::Compact))
								.row);
					}
				}
			}
			CHECK(store.Capture(next).retained);
			slice = store.ReadSlice(Key(), {}, u"v1"_q);
			CHECK(slice);
			if (!slice) {
				return;
			}
			if (!revise) {
				auto job = Job(slice->state, u"independent-v2"_q, u"v2"_q);
				job.expectedSize = bytes.size();
				job.expectedHash = hash;
				const auto stage = PlanArchiveMediaStage(slice->state, job);
				CHECK(stage);
				CHECK(
					store.Execute(TranslateArchiveSql(stage, *slice, store.security())));
			}
		};
		CHECK(store.CopyMedia(Key(), u"v1"_q, u"document:555"_q, source,
				  {hash, uint64_t(bytes.size())})
			  == !revise);
		const auto slice = store.ReadSlice(Key(), {}, u"v1"_q);
		CHECK(slice);
		if (slice) {
			const auto firstVersion = FindArchiveVersion(slice->state, Key(), u"v1"_q);
			CHECK(firstVersion);
			if (firstVersion) {
				CHECK((firstVersion->media.front().state == ArchiveMediaState::Complete)
					  == !revise);
			}
			if (!revise) {
				const auto head = FindArchiveVersion(slice->state, Key(), u"v2"_q);
				CHECK(head);
				if (head) {
					CHECK(head->media.front().state != ArchiveMediaState::Complete);
				}
				const auto independent
					= std::find_if(slice->state.jobs.begin(), slice->state.jobs.end(),
						[](const auto &job) { return job.id == u"independent-v2"_q; });
				CHECK(independent != slice->state.jobs.end()
					  && independent->state == ArchiveJobState::Staged);
				CHECK(store.ObservedMediaVersion(
						  Key(), first.id, firstHash, u"document:555"_q)
					  == first.id);
				CHECK(!store.ObservedMediaVersion(Key(), first.id,
					ArchiveContentHash(*next.content), u"document:555"_q));
			} else {
				CHECK(store.failure() == u"media-job-missing"_q
					  || store.failure() == u"media-revision-stale"_q
					  || store.failure() == u"media-job-cancelled"_q);
				CHECK(!store.ObservedMediaVersion(
					Key(), u"unrelated"_q, firstHash, u"document:555"_q));
			}
		}
		CHECK(store.Remove());
	}
}

void LifecycleEpoch() {
	Section = "real SQL lifecycle epoch and durable journal";
	auto temporary = QTemporaryDir();
	const auto options = Options(temporary.path());
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	auto first = Event(Empty(), u"v1"_q, 1);
	first.content->media.push_back({u"document:555"_q, u"document"_q});
	CHECK(store.Capture(first).retained);
	const auto slice = store.ReadSlice(Key());
	CHECK(slice);
	if (!slice) {
		return;
	}
	const auto source = ArchiveMediaSource{store.epoch(), Key(), u"v1"_q, u"v1"_q,
		ArchiveContentHash(*first.content), u"document:555"_q,
		ArchiveGenerationFor(slice->state, Key().chat)};
	const auto fact = ArchiveMediaLifecycleFact{source, ArchiveMediaLifetime::Ttl,
		ArchiveMediaTransition::Expiry, ArchiveAvailableBytes::Unavailable, true,
		u"expiry"_q, 2, 102};
	const auto planned = PlanArchiveMediaLifecycle(slice->state, store.epoch(), fact);
	CHECK(planned.event && planned.captureBeforeDestruction);
	if (!planned.event) {
		return;
	}
	CHECK(!PlanArchiveSqlCapture(*slice, *planned.event, store.security()).program);
	auto wrong = *planned.event;
	wrong.lifecycleEpoch->generation++;
	CHECK(!store.Capture(wrong).retained);
	CHECK(!store.ReadSlice(Key())->state.messages.front().deleted);
	const auto access = store.JournalAccess();
	CHECK(access);
	if (!access) {
		return;
	}
	auto journal = DesktopArchiveJournal(*access);
	CHECK(journal.Open());
	CHECK(!journal.Append(wrong));
	CHECK(journal.Append(*planned.event));
	CHECK(store.Close());
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	const auto reopened = store.JournalAccess();
	CHECK(reopened);
	if (!reopened) {
		return;
	}
	auto replay = DesktopArchiveJournal(*reopened);
	CHECK(replay.Open());
	CHECK(replay.Drain(store));
	const auto expired = store.ReadSlice(Key());
	CHECK(expired);
	if (expired) {
		CHECK(expired->state.messages.front().deleted);
		CHECK(
			expired->state.messages.front().deletionOrigin == ArchiveOrigin::MediaExpiry);
	}
	CHECK(store.Remove());
}

void Media() {
	Section = "owned media";
	auto temporary = QTemporaryDir();
	const auto options = Options(temporary.path());
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	for (const auto size : {0, 1, 1024 * 1024, 1024 * 1024 + 1, 3 * 1024 * 1024 + 7}) {
		auto event = Event(Empty(), u"media-"_q + QString::number(size), size + 1,
			QString::number(size),
			size ? ArchiveOrigin::AuthoritativeEdit : ArchiveOrigin::Baseline);
		const auto identity = u"attachment-"_q + QString::number(size);
		event.content->media.push_back({identity, u"document"_q});
		CHECK(store.Capture(event).retained);
		const auto bytes = QByteArray(size, 'm');
		const auto hash = QString::fromLatin1(
			QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
		auto source = QBuffer();
		source.setData(bytes);
		CHECK(source.open(QIODevice::ReadOnly));
		CHECK(store.CopyMedia(Key(), event.id, identity, source, {hash, uint64_t(size)}));
		auto result = QByteArray();
		CHECK(store.ReadMedia(Key(), event.id, identity, [&](const QByteArray &chunk) {
			result += chunk;
			return true;
		}));
		CHECK(result == bytes);
		CHECK(store.ReadSlice(Key())->state.leases.empty());
		CHECK(source.seek(0));
		CHECK(store.CopyMedia(Key(), event.id, identity, source, {hash, uint64_t(size)}));
		const auto token = ArchiveSqlToken(
			options.account, ArchiveIndexKind::Blob, hash.toUtf8(), store.security());
		const auto chunkPath
			= store.directory() + u"/blobs/"_q + token + u"/00000000000000000000.chunk"_q;
		const auto chunk = DesktopArchiveRead(chunkPath, 2 * 1024 * 1024);
		CHECK(chunk);
		if (chunk) {
			auto corrupted = *chunk;
			corrupted[corrupted.size() / 2] ^= 1;
			CHECK(DesktopArchiveWrite(chunkPath, corrupted));
			CHECK(!store.ReadMedia(
				Key(), event.id, identity, [](const auto &) { return true; }));
			CHECK(DesktopArchiveWrite(chunkPath, *chunk));
		}
	}
	auto original = QByteArray();
	CHECK(store.ReadMedia(
		Key(), u"media-1"_q, u"attachment-1"_q, [&](const QByteArray &chunk) {
			original += chunk;
			return true;
		}));
	CHECK(original == QByteArray(1, 'm'));
	const auto released
		= store.database().Read(ArchiveSqlReadPage(ArchiveRecordKind::Lease, 128));
	CHECK(released.ok && released.rows.empty());
	CHECK(store.Remove());
}

void Journal() {
	Section = "durable capture journal";
	auto temporary = QTemporaryDir();
	const auto options = Options(temporary.path());
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	const auto access = store.JournalAccess();
	CHECK(access);
	if (!access) {
		return;
	}
	auto journal = DesktopArchiveJournal(*access);
	CHECK(journal.Open());
	auto a = Event(Empty(), u"accepted-pts-1"_q, 0, u"A"_q);
	auto b = Event(
		Empty(), u"accepted-pts-2"_q, 0, u"B"_q, ArchiveOrigin::AuthoritativeEdit);
	auto c = Event(
		Empty(), u"accepted-pts-3"_q, 0, u"A"_q, ArchiveOrigin::AuthoritativeEdit);
	CHECK(journal.Append(a));
	CHECK(journal.Append(b));
	CHECK(journal.Append(c));
	CHECK(store.Versions(Key()).versions.empty());
	CHECK(journal.Drain(store, 1));
	CHECK(store.Versions(Key()).versions.size() == 1);
	store.database().FailForTest(u"commit"_q);
	CHECK(!journal.Drain(store));
	CHECK(!journal.failure().isEmpty());
	CHECK(!store.failure().isEmpty());
	CHECK(store.Versions(Key()).hasGap);
	CHECK(store.Versions(Key()).versions.size() == 1);
	CHECK(journal.Drain(store));
	CHECK(store.Versions(Key()).versions.size() == 3);
	CHECK(journal.Append(b));
	CHECK(journal.Drain(store));
	CHECK(store.Versions(Key()).versions.size() == 3);
	CHECK(journal.Append(Event(Empty(), u"late-refresh"_q, 0, u"STALE"_q)));
	CHECK(journal.Drain(store));
	CHECK(store.Versions(Key()).versions.size() == 3);
	const auto hydratedKey = Key(options.account, 45, 68);
	auto partial
		= Event(Empty(), u"partial"_q, 0, {}, ArchiveOrigin::Baseline, hydratedKey);
	partial.completeness = ArchiveCompleteness::Partial;
	partial.missing.content.push_back({{u"text"_q}});
	CHECK(journal.Append(partial));
	CHECK(journal.Drain(store));
	auto hydration = Event(Empty(), u"hydrate"_q, 0, u"captured later"_q,
		ArchiveOrigin::Hydration, hydratedKey);
	hydration.targetVersion = partial.id;
	hydration.targetRevision = partial.id;
	hydration.expectedContentHash = ArchiveContentHash(*partial.content);
	CHECK(journal.Append(hydration));
	CHECK(store.Close());
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	const auto reopened = store.JournalAccess();
	CHECK(reopened);
	if (reopened) {
		auto replay = DesktopArchiveJournal(*reopened);
		CHECK(replay.Open());
		CHECK(replay.Drain(store));
		const auto page = store.Versions(hydratedKey);
		CHECK(page.versions.size() == 1);
		if (!page.versions.empty()) {
			CHECK(page.versions.front().version.id == partial.id);
			CHECK(page.versions.front().version.content.text == u"captured later"_q);
			CHECK(page.versions.front().version.completeness
				  == ArchiveCompleteness::Complete);
		}
	}
	CHECK(store.Remove());
	CHECK(!journal.Append(a));
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(!journal.Append(a));
	CHECK(store.Remove());
}

void Paging() {
	Section = "bounded history paging and lifetime";
	auto temporary = QTemporaryDir();
	const auto options = Options(temporary.path());
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	for (auto i = 1; i != 141; ++i) {
		CHECK(store
				.Capture(Event(Empty(), u"page-"_q + QString::number(i), i,
					QString::number(i), ArchiveOrigin::AuthoritativeEdit))
				.retained);
	}
	auto before = QString();
	auto revision = std::optional<uint64_t>();
	auto count = 0;
	auto previous = uint64_t(141);
	for (;;) {
		const auto page = store.Versions(Key(), 31, before, revision);
		CHECK(page.status == DesktopArchiveStatus::Ready);
		CHECK(page.versions.size() <= 31);
		if (page.status != DesktopArchiveStatus::Ready) {
			break;
		}
		revision = page.revision;
		before = page.beforeOrder;
		for (const auto &entry : page.versions) {
			CHECK(entry.version.order < previous);
			previous = entry.version.order;
			++count;
		}
		if (!page.more) {
			break;
		}
	}
	CHECK(count == 140);
	const auto access = store.JournalAccess();
	CHECK(access && access->current());
	CHECK(store.Close());
	CHECK(access && !access->current());
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(access && !access->current());
	CHECK(store.Remove());
}

void PrivatePurge() {
	Section = "private SQLite bounded purge";
	auto temporary = QTemporaryDir();
	const auto options = Options(temporary.path());
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	for (auto i = 1; i != 131; ++i) {
		CHECK(store
				.Capture(Event(Empty(), u"purge-"_q + QString::number(i), i,
					QString::number(i), ArchiveOrigin::AuthoritativeEdit))
				.retained);
	}
	const auto slice = store.ReadSlice(Key());
	CHECK(slice);
	if (!slice) {
		return;
	}
	auto header = slice->state;
	header.messages.clear();
	header.blobs.clear();
	header.jobs.clear();
	header.leases.clear();
	header.chatGenerations.clear();
	const auto &security = store.security();
	auto plan = BeginArchiveSqlPurge(header,
		{ArchivePurgeKind::EditHistory, {options.account, Key().chat}, header.revision},
		{{Key().chat, 0}}, u"desktop-purge"_q, security);
	CHECK(plan.program);
	CHECK(store.Execute(plan.program));
	CHECK(store.Versions(Key()).status != DesktopArchiveStatus::Ready);
	auto complete = false;
	for (auto iteration = 0; iteration != 64; ++iteration) {
		const auto purge = plan.next;
		auto rows = std::vector<ArchiveSqlPurgeRow>();
		if (purge.phase != ArchivePurgePhase::Finish) {
			const auto page
				= store.database().Read(ArchiveSqlPurgePage(purge, 31, security));
			CHECK(page.ok);
			if (!page.ok) {
				break;
			}
			CHECK(page.rows.size() <= 31);
			const auto kind
				= purge.phase == ArchivePurgePhase::Messages ? ArchiveRecordKind::Message
				  : purge.phase == ArchivePurgePhase::Versions
					  ? ArchiveRecordKind::Version
				  : purge.phase == ArchivePurgePhase::Jobs ? ArchiveRecordKind::Job
														   : ArchiveRecordKind::Lease;
			for (const auto &values : page.rows) {
				const auto id = std::get<QString>(values[0]);
				const auto frame = std::get<QByteArray>(values[1]);
				const auto opened = OpenArchiveEnvelope(
					{options.account, security.storeId, kind, id.toUtf8()},
					security.keyId, frame, security.crypto);
				CHECK(opened);
				if (!opened) {
					return;
				}
				const auto decoded = DecodeArchiveRow(opened.bytes);
				CHECK(decoded.row);
				if (!decoded.row) {
					return;
				}
				auto row = ArchiveSqlPurgeRow{{id, 0, *decoded.row}, frame};
				if (kind == ArchiveRecordKind::Message) {
					const auto key = std::get<ArchiveMessage>(decoded.row->value).key;
					const auto hint = store.database().Read(
						{u"SELECT head_version FROM messages WHERE message_key=?"_q, {id},
							ArchiveSqlExpectation::OneRow});
					const auto head = store.database().Read(
						ArchiveSqlPurgeHead(purge, key, security));
					CHECK(hint.ok && head.ok && head.rows.size() == 1);
					if (!hint.ok || !head.ok || head.rows.empty()) {
						return;
					}
					row.headFrame = std::get<QByteArray>(head.rows[0][0]);
					const auto plain = OpenArchiveEnvelope(
						{options.account, security.storeId, ArchiveRecordKind::Version,
							(id + '/' + std::get<QString>(hint.rows[0][0])).toUtf8()},
						security.keyId, row.headFrame, security.crypto);
					CHECK(plain);
					const auto version = plain ? DecodeArchiveRow(plain.bytes)
											   : ArchiveRowDecodeResult();
					CHECK(version.row);
					if (!version.row) {
						return;
					}
					row.head = std::get<ArchiveVersionSnapshot>(version.row->value);
				}
				rows.push_back(std::move(row));
			}
		}
		plan = ContinueArchiveSqlPurge(purge, rows, security);
		CHECK(plan.program);
		if (!plan.program) {
			break;
		}
		CHECK(store.Execute(plan.program));
		if (plan.complete) {
			complete = true;
			break;
		}
	}
	CHECK(complete);
	CHECK(store.Versions(Key()).versions.size() == 1);
	CHECK(store.Remove());
}

MTPMessage WireMessage(QString body, int views = 0, QByteArray reference = {},
	bool document = false, bool context = false, int64 stickerHash = 0) {
	auto wire = mtpBuffer();
	MTP_int(mtpc_message).write(wire);
	MTP_int((1 << 10) | (document ? (1 << 9) : 0)
			| (context ? (1 << 8) | (1 << 3) | (1 << 17) : 0))
		.write(wire);
	MTP_int(0).write(wire);
	MTP_int(67).write(wire);
	if (context) {
		MTPPeer(MTP_peerUser(MTP_long(123456789123LL))).write(wire);
	}
	MTPPeer(MTP_peerChannel(MTP_long(45))).write(wire);
	if (context) {
		MTP_int(mtpc_messageReplyHeader).write(wire);
		MTP_int((1 << 3) | (1 << 4) | (1 << 1)).write(wire);
		MTP_int(5).write(wire);
		MTP_int(8).write(wire);
	}
	MTP_int(90).write(wire);
	MTP_string(body).write(wire);
	if (document) {
		auto attributes = QVector<MTPDocumentAttribute>();
		if (stickerHash) {
			attributes.push_back(
				MTP_documentAttributeSticker(MTP_flags(0), MTP_string("x"),
					MTP_inputStickerSetID(MTP_long(2), MTP_long(stickerHash)),
					MTPMaskCoords()));
		}
		const auto doc = MTP_document(MTP_flags(0), MTP_long(10), MTP_long(999),
			MTP_bytes(reference), MTP_int(80), MTP_string("text/plain"), MTP_long(7),
			MTPVector<MTPPhotoSize>(), MTPVector<MTPVideoSize>(), MTP_int(2),
			MTP_vector<MTPDocumentAttribute>(attributes));
		MTPMessageMedia(MTP_messageMediaDocument(
							MTP_flags(MTPDmessageMediaDocument::Flag::f_document), doc,
							MTPVector<MTPDocument>(), MTPPhoto(), MTP_int(0), MTP_int(0)))
			.write(wire);
	}
	MTP_int(views).write(wire);
	MTP_int(0).write(wire);
	if (context) {
		MTP_long(9223372036854775806LL).write(wire);
	}
	auto message = MTPMessage();
	auto from = wire.constData();
	const auto parsed = message.read(from, from + wire.size());
	CHECK(parsed);
	if (!parsed) {
		return MTP_messageEmpty(MTP_flags(0), MTP_int(67), MTPPeer());
	}
	CHECK(from == wire.constData() + wire.size());
	return message;
}

void CaptureBoundary() {
	Section = "production Telegram capture snapshot";
	const auto normal = WireMessage(u"ordinary"_q);
	const auto contextual
		= DesktopArchiveSnapshot({0, 123}, WireMessage(u"topic"_q, 0, {}, false, true),
			ArchiveOrigin::Baseline, u"context"_q, 100);
	CHECK(contextual);
	if (contextual) {
		CHECK(contextual->observation.topicId == 8);
		CHECK(contextual->observation.context.value(u"sender"_q)
				  .toObject()
				  .value(u"id"_q)
				  .toString()
			  == u"123456789123"_q);
		CHECK(contextual->observation.context.value(u"album"_q).toString()
			  == u"9223372036854775806"_q);
		CHECK(contextual->observation.context.value(u"reply"_q)
				  .toObject()
				  .value(u"parent"_q)
				  .toString()
			  == u"45"_q);
		auto temporary = QTemporaryDir();
		auto store = DesktopArchiveStore();
		CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
		CHECK(store.Capture(*contextual).retained);
		const auto projection = store.database().Read(
			{u"SELECT topic_token,collection,representation_token FROM messages"_q, {},
				ArchiveSqlExpectation::OneRow});
		CHECK(projection.ok && projection.rows.size() == 1);
		if (projection.ok && projection.rows.size() == 1) {
			CHECK(std::get<QString>(projection.rows[0][0])
				  == ArchiveSqlTopicId(Key().chat, Key().account, 8, store.security()));
			CHECK(std::get<int64>(projection.rows[0][1])
				  == int64(ArchiveCollection::Received));
			CHECK(std::get<QString>(projection.rows[0][2])
				  == ArchiveSqlToken(Key().account, ArchiveIndexKind::Representation,
					  "original", store.security()));
		}
		const auto page = store.Versions(Key());
		CHECK(page.versions.size() == 1);
		if (!page.versions.empty()) {
			CHECK(page.versions.front().version.observation.topicId == 8);
		}
		CHECK(store.Remove());
	}
	CHECK(DesktopArchiveReceivedId(MsgId(67), {}, normal));
	CHECK(!DesktopArchiveReceivedId(ServerMaxMsgId + 68, {}, normal));
	CHECK(!DesktopArchiveReceivedId(MsgId(67), MessageFlag::AdminLogEntry, normal));
	const auto first = DesktopArchiveSnapshot({0, 123},
		WireMessage(u"A"_q, 1, "one", true), ArchiveOrigin::Baseline, u"a"_q, 100);
	const auto quality = DesktopArchiveSnapshot({0, 123},
		WireMessage(u"A"_q, 999, "changed-file-reference", true), ArchiveOrigin::Baseline,
		u"quality"_q, 101);
	const auto edited = DesktopArchiveSnapshot({0, 123},
		WireMessage(u"B"_q, 999, "changed-file-reference", true),
		ArchiveOrigin::AuthoritativeEdit, u"edit"_q, 102);
	const auto sticker = DesktopArchiveSnapshot({0, 123},
		WireMessage(u"sticker"_q, 0, {}, true, false, 3), ArchiveOrigin::Baseline,
		u"sticker"_q, 100);
	const auto stickerRefresh = DesktopArchiveSnapshot({0, 123},
		WireMessage(u"sticker"_q, 0, {}, true, false, 4), ArchiveOrigin::Baseline,
		u"sticker-refresh"_q, 101);
	CHECK(sticker && stickerRefresh);
	if (sticker && stickerRefresh) {
		CHECK(SameArchiveContent(*sticker->content, *stickerRefresh->content));
		CHECK(sticker->observation.payload != stickerRefresh->observation.payload);
	}
	CHECK(first && quality && edited);
	if (!first || !quality || !edited) {
		return;
	}
	CHECK(SameArchiveContent(*first->content, *quality->content));
	CHECK(!SameArchiveContent(*first->content, *edited->content));
	CHECK(first->observation.payload != quality->observation.payload);
	CHECK(IsArchiveToken(first->observation.payloadFormat));
	CHECK(first->content->media.size() == 1
		  && first->content->media[0].identity == u"document:10"_q);
	CHECK(first->key == Key());
	auto temporary = QTemporaryDir();
	const auto options = Options(temporary.path());
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	const auto access = store.JournalAccess();
	CHECK(access);
	if (!access) {
		return;
	}
	auto journal = DesktopArchiveJournal(*access);
	CHECK(journal.Open());
	CHECK(journal.Append(*first));
	CHECK(journal.Append(*quality));
	CHECK(journal.Append(*edited));
	for (auto step = 0; step != 3; ++step) {
		const auto drained = journal.Drain(store, 1);
		std::printf("capture step=%d drained=%d journal=%s store=%s rows=%zu\n", step,
			drained, journal.failure().toUtf8().constData(),
			store.failure().toUtf8().constData(), store.Versions(Key()).versions.size());
		CHECK(drained);
		if (step == 1) {
			auto all = std::vector<ArchiveStoredRow>();
			for (const auto kind :
				{ArchiveRecordKind::Account, ArchiveRecordKind::Message,
					ArchiveRecordKind::Version, ArchiveRecordKind::Event}) {
				const auto rows = store.database().Read(ArchiveSqlReadPage(kind, 128));
				CHECK(rows.ok);
				if (!rows.ok) {
					continue;
				}
				for (const auto &values : rows.rows) {
					const auto id = std::get<QString>(values[0]);
					const auto frame = std::get<QByteArray>(values[2]);
					const auto plain = OpenArchiveEnvelope(
						{options.account, store.security().storeId, kind, id.toUtf8()},
						store.security().keyId, frame, store.security().crypto);
					const auto row = plain ? DecodeArchiveRow(plain.bytes)
										   : ArchiveRowDecodeResult();
					CHECK(row.row);
					if (!row.row) {
						continue;
					}
					all.push_back({id, std::get<int64>(values[1]), *row.row});
				}
			}
			const auto restored = RestoreArchiveSqlSlice(all, store.security());
			std::printf("full restored=%d rows=%zu\n", bool(restored), all.size());
			CHECK(restored);
			std::erase_if(all, [](const auto &row) {
				return std::holds_alternative<ArchiveEventReceipt>(row.row.value)
					   && std::get<ArchiveEventReceipt>(row.row.value).id == u"a"_q;
			});
			const auto reduced = RestoreArchiveSqlSlice(all, store.security());
			std::printf("reduced restored=%d rows=%zu\n", bool(reduced), all.size());
			CHECK(!reduced);
		}
	}
	CHECK(store.Versions(Key()).versions.size() == 2);
	CHECK(store.Remove());
}

void CheckedFailures() {
	Section = "private statement and disk-full failures";
	auto temporary = QTemporaryDir();
	const auto options = Options(temporary.path());
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	CHECK(store.Capture(Event(Empty(), u"base"_q, 1)).retained);
	const auto slice = store.ReadSlice(Key());
	CHECK(slice);
	if (!slice) {
		return;
	}
	const auto plan = PlanArchiveSqlCapture(*slice,
		Event(Empty(), u"next"_q, 2, u"B"_q, ArchiveOrigin::AuthoritativeEdit),
		store.security());
	CHECK(plan.program);
	for (const auto &phase :
		{u"prepare"_q, u"bind"_q, u"step"_q, u"finalize"_q, u"commit"_q}) {
		store.database().FailForTest(phase,
			phase == u"prepare"_q || phase == u"step"_q || phase == u"finalize"_q ? 2
																				  : 1);
		const auto failed = store.database().Execute(plan.program);
		CHECK(!failed.ok && !failed.committed);
		CHECK(store.Versions(Key()).versions.size() == 1);
	}
	auto wrong = plan.program;
	wrong.statements[1].rows = {0, 0};
	CHECK(!store.database().Execute(wrong).committed);
	CHECK(store.Versions(Key()).versions.size() == 1);
	const auto pages = store.database().Read(
		{u"PRAGMA page_count"_q, {}, ArchiveSqlExpectation::OneRow});
	CHECK(pages.ok);
	if (pages.ok) {
		const auto count = std::get<int64>(pages.rows.front().front());
		CHECK(store.database()
				.Read({u"PRAGMA max_page_count="_q + QString::number(count), {},
					ArchiveSqlExpectation::OneRow})
				.ok);
		const auto huge = PlanArchiveSqlCapture(*slice,
			Event(Empty(), u"full"_q, 2, QString(200000, 'x'),
				ArchiveOrigin::AuthoritativeEdit),
			store.security());
		CHECK(huge.program);
		CHECK(!store.database().Execute(huge.program).committed);
		CHECK(store.Versions(Key()).versions.size() == 1);
	}
	store.database().FailForTest(u"close"_q);
	CHECK(!store.Close());
	CHECK(store.Close());
	CHECK(DesktopArchiveStore::RemoveAccount(options));
}

void DeleteAfterEviction() {
	Section = "unloaded non-channel deletion and ambiguity";
	auto temporary = QTemporaryDir();
	const auto options = Options(temporary.path());
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	const auto key = Key(options.account, 45, 67, ArchiveChatKind::User);
	CHECK(store
			.Capture(
				Event(Empty(), u"base"_q, 1, u"seen"_q, ArchiveOrigin::Baseline, key))
			.retained);
	const auto access = store.JournalAccess();
	CHECK(access);
	if (!access) {
		return;
	}
	auto journal = DesktopArchiveJournal(*access);
	CHECK(journal.Open());
	auto deleted
		= Event(Empty(), u"server-delete"_q, 0, {}, ArchiveOrigin::AuthoritativeDelete,
			Key(options.account, 1, 67, ArchiveChatKind::User));
	deleted.observation.context.insert(u"desktopNonChannelDelete"_q, true);
	CHECK(journal.Append(deleted));
	CHECK(journal.Drain(store));
	const auto after = store.ReadSlice(key);
	CHECK(after && after->state.messages.front().deleted);
	deleted.key.messageId = 68;
	deleted.id = u"unknown-delete"_q;
	CHECK(journal.Append(deleted));
	CHECK(journal.Drain(store));
	const auto missing = store.NonChannelMessages(68);
	CHECK(missing && missing->empty());
	CHECK(store.Remove());
}

void SourceFiles() {
	Section = "registered available file identity";
	auto temporary = QTemporaryDir();
	const auto path = temporary.path() + u"/download"_q;
	CHECK(DesktopArchiveWrite(path, "original"));
	const auto proof = DesktopArchiveInspectSource(path, 8);
	CHECK(proof);
	if (!proof) {
		return;
	}
	auto opened = QFile();
	CHECK(DesktopArchiveOpenSource(path, *proof, opened));
	CHECK(opened.readAll() == "original");
	CHECK(DesktopArchiveSourceMatches(opened, *proof));
	opened.close();
	CHECK(DesktopArchiveWrite(path, "replaced"));
	auto replaced = QFile();
	CHECK(!DesktopArchiveOpenSource(path, *proof, replaced));
	const auto fresh = DesktopArchiveInspectSource(path, 8);
	CHECK(fresh);
	if (!fresh) {
		return;
	}
	CHECK(DesktopArchiveOpenSource(path, *fresh, replaced));
	auto writer = QFile(path);
	CHECK(writer.open(QIODevice::WriteOnly));
	CHECK(writer.write("mutated!") == 8);
	CHECK(writer.flush());
	CHECK(writer.setFileTime(QDateTime::fromMSecsSinceEpoch(fresh->modified + 2000),
		QFileDevice::FileModificationTime));
	writer.close();
	CHECK(!DesktopArchiveSourceMatches(replaced, *fresh));
	replaced.close();
	const auto link = temporary.path() + u"/alias"_q;
	CHECK(QFile::link(path, link));
	CHECK(!DesktopArchiveInspectSource(link, 8));
}

namespace Delivery {
struct Session : base::has_weak_ptr {};
std::set<Session*> Retired;
void Invoke(base::weak_ptr<Session> weak, DesktopArchiveHistoryPage page,
	std::function<void(DesktopArchiveHistoryPage)> done,
	std::shared_ptr<std::atomic<uint64_t>> fence, uint64_t ticket) {
#include "archive-page-callback.inc"
}
}

void PageDelivery() {
	Section = "production asynchronous page delivery";
	auto temporary = QTemporaryDir();
	const auto options = Options(temporary.path());
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	CHECK(store.Capture(Event(Empty(), u"observed"_q, 1)).retained);
	const auto page = store.Versions(Key());
	CHECK(page.status == DesktopArchiveStatus::Ready && page.versions.size() == 1);
	auto session = std::make_unique<Delivery::Session>();
	const auto weak = base::make_weak(session.get());
	const auto fence = std::make_shared<std::atomic<uint64_t>>(1);
	auto delivered = DesktopArchiveHistoryPage();
	auto calls = 0;
	const auto done = [&](DesktopArchiveHistoryPage value) {
		++calls;
		delivered = std::move(value);
	};
	Delivery::Invoke(weak, page, done, fence, 1);
	CHECK(calls == 1 && delivered.versions.size() == 1);
	*fence = 2;
	Delivery::Invoke(weak, page, done, fence, 1);
	CHECK(calls == 2 && delivered.status == DesktopArchiveStatus::Stale
		  && delivered.versions.empty());
	CHECK(fence->load() == 2);
	Delivery::Invoke(weak, page, done, fence, 2);
	CHECK(calls == 3 && delivered.versions.size() == 1);
	Delivery::Retired.insert(session.get());
	Delivery::Invoke(weak, page, done, fence, 2);
	CHECK(calls == 3);
	CHECK(fence->load() == 2);
	Delivery::Retired.clear();
	session.reset();
	Delivery::Invoke(weak, page, done, fence, 2);
	CHECK(calls == 3);
	CHECK(store.Remove());
}

void AttentionRecovery() {
	Section = "encrypted attention recovery";
	auto temporary = QTemporaryDir();
	const auto options = Options(temporary.path());
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	const auto path = store.directory();
	const auto access = store.JournalAccess();
	CHECK(access);
	if (!access) {
		return;
	}
	auto journal = DesktopArchiveJournal(*access);
	CHECK(journal.Open());
	CHECK(journal.Attention(u"queue-full"_q, 100));
	const auto frame = DesktopArchiveRead(path + u"/retention-gap"_q, 8192);
	CHECK(frame && !frame->contains("queue-full"));
	CHECK(store.Close());
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(!QFileInfo::exists(path + u"/retention-gap"_q));
	CHECK(store.Versions(Key()).hasGap);
	const auto gaps = store.database().Read(ArchiveSqlReadCaptureGaps(1));
	CHECK(gaps.ok && gaps.rows.size() == 1);
	CHECK(store.Remove());
}

void Crypto() {
	Section = "crypto";
	auto crypto = DesktopArchiveCrypto();
	CHECK(crypto.AddKey(u"key"_q, QByteArray(32, char(0))));
	const auto api = crypto.callbacks();
	const auto sealed = api.sealAes256Gcm(
		u"key"_q, QByteArray(12, char(0)), QByteArray(16, char(0)), {});
	CHECK(sealed);
	if (!sealed) {
		return;
	}
	CHECK(sealed->ciphertext.toHex() == "cea7403d4d606b6e074ec5d3baf39d18");
	CHECK(sealed->tag.toHex() == "d0d1c8a799996bf0265b98b5d48ab919");
	CHECK(api.openAes256Gcm(u"key"_q, QByteArray(12, char(0)), *sealed, {})
		  == QByteArray(16, char(0)));
	auto bad = *sealed;
	bad.tag[0] ^= 1;
	CHECK(!api.openAes256Gcm(u"key"_q, QByteArray(12, char(0)), bad, {}));
	CHECK(!api.openAes256Gcm(u"wrong"_q, QByteArray(12, char(0)), *sealed, {}));
	CHECK(!api.openAes256Gcm(u"key"_q, QByteArray(12, char(0)), *sealed, "wrong"));
}

}

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	const auto application = QCoreApplication(argc, argv);
	if (argc > 1 && QByteArray(argv[1]) == "capture-only") {
		CaptureBoundary();
		return Failures ? 1 : 0;
	}
	KeySetMigrationAndRebind();
	ExplicitDiscardRecovery();
	AdmissionAuthentication();
	MtpLoginBinding();
	LoginAuthorityRevocation();
	StaleLogoutCleanup();
	HistoricalObservationRevision();
	ShortSendAcknowledgement();
	AttentionRecovery();
	PageDelivery();
	SourceFiles();
	Crypto();
	Store();
	ConcurrentAccounts();
	Refusals();
	SchemaAdmission();
	HistoricalMedia();
	LifecycleEpoch();
	Media();
	Journal();
	Paging();
	PrivatePurge();
	CaptureBoundary();
	CheckedFailures();
	DeleteAfterEviction();
	std::printf("Desktop archive: %d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
