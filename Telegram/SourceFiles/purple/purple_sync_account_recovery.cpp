/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_account_recovery.h"

#include "core/application.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "purple/purple_sync_account_binding.h"
#include "purple/purple_sync_config_post.h"
#include "purple/purple_sync_config_review.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLockFile>
#include <QtCore/qscopeguard.h>

#include <algorithm>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef Q_OS_MAC
#include <stdio.h>
#elif defined __linux__
#include <sys/syscall.h>
#endif
#endif

namespace Purple {
namespace {

constexpr auto kJournalBytes = 32 * 1024 * 1024;

QString JournalPath(const AccountFiles &files) {
	return files.syncDirectory + u"/recovery.json"_q;
}

AccountFiles StoreFiles(const QString &directory) {
	auto result = AccountFiles();
	result.syncDirectory = directory;
	result.syncStore = directory + u"/state.json"_q;
	return result;
}

bool SameStore(const SyncRecoveryStore &a, const SyncRecoveryStore &b) {
	return a.directoryAccount == b.directoryAccount && a.statePresent == b.statePresent
		&& a.state == b.state && a.pendingPresent == b.pendingPresent
		&& a.pendingComplete == b.pendingComplete && a.pendingFiles == b.pendingFiles;
}

bool SameRecovery(const DesktopSyncRecovery &a, const DesktopSyncRecovery &b) {
	if (a.input.account != b.input.account || a.input.startupOwner != b.input.startupOwner
		|| a.input.preferenceToken != b.input.preferenceToken || a.identities != b.identities
		|| a.input.stores.size() != b.input.stores.size()
		|| a.input.otherPreferences.size() != b.input.otherPreferences.size()
		|| a.journal != b.journal || a.accountDirectories != b.accountDirectories || a.journalPresent != b.journalPresent || a.plan.status != b.plan.status) { return false; }
	for (auto i = size_t(0); i != a.input.stores.size(); ++i) {
		if (!SameStore(a.input.stores[i], b.input.stores[i])) { return false; }
	}
	for (auto i = size_t(0); i != a.input.otherPreferences.size(); ++i) {
		if (a.input.otherPreferences[i].account != b.input.otherPreferences[i].account
			|| a.input.otherPreferences[i].token != b.input.otherPreferences[i].token) { return false; }
	}
	return true;
}

QByteArray SerializeArchive(const SyncStartAgainPlan &plan) {
	auto pending = QJsonObject();
	for (const auto &[name, bytes] : plan.expected.pendingFiles) {
		pending.insert(name, QString::fromLatin1(bytes.toBase64()));
	}
	const auto key = AccountKeyText(plan.account.environment, plan.account.userId);
	return QJsonDocument(QJsonObject{
		{ u"version"_q, 1 }, { u"account"_q, key.value_or(QString()) },
		{ u"archive"_q, plan.archiveDirectory }, { u"history"_q, plan.history },
		{ u"token"_q, QString::fromLatin1(plan.preferenceToken.toBase64()) },
		{ u"state_present"_q, plan.expected.statePresent },
		{ u"state"_q, QString::fromLatin1(plan.expected.state.toBase64()) },
		{ u"pending_present"_q, plan.expected.pendingPresent }, { u"pending"_q, pending },
	}).toJson(QJsonDocument::Compact);
}

bool ValidArchivePlan(const AccountFiles &files, const SyncStartAgainPlan &plan) {
	const auto prefix = files.syncDirectory + u"/orphaned-"_q;
	const auto date = plan.archiveDirectory.mid(prefix.size());
	const auto format = u"yyyyMMdd-HHmmss"_q;
	if (plan.status != SyncStartAgainStatus::Ready || !plan.preservesHistory
		|| !plan.archiveDirectory.startsWith(prefix) || plan.history != files.history
		|| QDateTime::fromString(date, format).toString(format) != date || date.size() != 15
		|| (!plan.expected.statePresent && !plan.expected.state.isEmpty())
		|| (!plan.expected.pendingPresent && !plan.expected.pendingFiles.empty())
		|| !plan.expected.pendingComplete || plan.moves.empty()) { return false; }
	const auto expected = std::vector<SyncOrphanMove>{
		{ SyncOrphanMoveKind::PendingDirectory, files.syncDirectory + u"/pending"_q, plan.archiveDirectory + u"/pending"_q },
		{ SyncOrphanMoveKind::StateFile, files.syncStore, plan.archiveDirectory + u"/state.json"_q },
	};
	auto index = size_t(0);
	for (auto i = 0; i != 2; ++i) {
		if (!(i ? plan.expected.statePresent : plan.expected.pendingPresent)) { continue; }
		if (index >= plan.moves.size()) { return false; }
		const auto &move = plan.moves[index++];
		if (move.kind != expected[i].kind || move.source != expected[i].source
			|| move.destination != expected[i].destination) { return false; }
	}
	for (const auto &[name, bytes] : plan.expected.pendingFiles) {
		if (name.isEmpty() || name == u"."_q || name == u".."_q || name.contains('/')
			|| name.contains('\\') || std::any_of(name.begin(), name.end(), [](QChar ch) {
			return ch.unicode() < 0x20 || ch.unicode() == 0x7f;
		})) { return false; }
	}
	return index == plan.moves.size();
}

bool FlushSyncDirectory(const QString &path) {
#ifdef Q_OS_UNIX
	const auto fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) { return false; }
	const auto ok = ::fsync(fd) == 0;
	::close(fd);
	return ok;
#else
	return true;
#endif
}

bool MoveExclusive(const QString &source, const QString &destination) {
#ifdef Q_OS_WIN
	const auto openDirectory = [](const QString &path) {
		return CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	};
	const auto fromPath = QFileInfo(source).absolutePath();
	const auto toPath = QFileInfo(destination).absolutePath();
	const auto fromIdentity = SyncPathIdentity(fromPath);
	const auto toIdentity = SyncPathIdentity(toPath);
	const auto from = openDirectory(fromPath);
	const auto to = openDirectory(toPath);
	const auto close = qScopeGuard([&] {
		if (from != INVALID_HANDLE_VALUE) { CloseHandle(from); }
		if (to != INVALID_HANDLE_VALUE) { CloseHandle(to); }
	});
	const auto identity = [](HANDLE handle) {
		auto value = BY_HANDLE_FILE_INFORMATION();
		return handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &value)
			&& (value.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(value.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
			? QByteArray::number(value.dwVolumeSerialNumber) + ':' + QByteArray::number(value.nFileIndexHigh)
				+ ':' + QByteArray::number(value.nFileIndexLow) : QByteArray();
	};
	return !fromIdentity.isEmpty() && !toIdentity.isEmpty() && identity(from) == fromIdentity && identity(to) == toIdentity
		&& SyncPathIdentity(fromPath) == fromIdentity && SyncPathIdentity(toPath) == toIdentity
		&& MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()),
			reinterpret_cast<LPCWSTR>(destination.utf16()), MOVEFILE_WRITE_THROUGH);
#else
	const auto fromParent = QFileInfo(source).absolutePath();
	const auto toParent = QFileInfo(destination).absolutePath();
	const auto fromIdentity = SyncPathIdentity(fromParent);
	const auto toIdentity = SyncPathIdentity(toParent);
	const auto from = ::open(QFile::encodeName(fromParent).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	const auto to = ::open(QFile::encodeName(toParent).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	const auto close = qScopeGuard([&] { if (from >= 0) { ::close(from); } if (to >= 0) { ::close(to); } });
	const auto descriptorIdentity = [](int fd) {
		struct stat value = {};
		return ::fstat(fd, &value) == 0 && S_ISDIR(value.st_mode)
			? QByteArray::number(qulonglong(value.st_dev)) + ':' + QByteArray::number(qulonglong(value.st_ino)) : QByteArray();
	};
	if (from < 0 || to < 0 || descriptorIdentity(from) != fromIdentity || descriptorIdentity(to) != toIdentity
		|| SyncPathIdentity(fromParent) != fromIdentity
		|| SyncPathIdentity(toParent) != toIdentity) { return false; }
	const auto sourceName = QFile::encodeName(QFileInfo(source).fileName());
	const auto destinationName = QFile::encodeName(QFileInfo(destination).fileName());
#ifdef Q_OS_MAC
	const auto moved = ::renameatx_np(from, sourceName.constData(), to, destinationName.constData(), RENAME_EXCL) == 0;
#elif defined __linux__
	const auto moved = ::syscall(SYS_renameat2, from, sourceName.constData(), to, destinationName.constData(), 1) == 0;
#else
	const auto moved = false;
#endif
	return moved && ::fsync(from) == 0 && ::fsync(to) == 0
		&& SyncPathIdentity(fromParent) == fromIdentity && SyncPathIdentity(toParent) == toIdentity;
#endif
}

}

std::optional<DesktopSyncStoreDiscovery> DiscoverDesktopSyncStores(const QString &root) {
	auto result = DesktopSyncStoreDiscovery();
	result.identities[root] = SyncPathIdentity(root);
	if (!QDir::isAbsolutePath(root) || !QFileInfo(root).isDir()) { return std::nullopt; }
	const auto captureStore = [&](const QString &directory, std::optional<AccountKey> account) {
		const auto snapshot = ReadSyncRecoveryStore(StoreFiles(directory), account);
		if (snapshot.status != SyncStoreStatus::Ready) { return false; }
		result.stores.push_back(snapshot.store);
		result.identities.insert(snapshot.identities.begin(), snapshot.identities.end());
		return true;
	};
	const auto accounts = root + u"/accounts"_q;
	result.identities[accounts] = SyncPathIdentity(accounts);
	if (!QFileInfo(accounts).isReadable() || !QFileInfo(accounts).isDir()) { return std::nullopt; }
	const auto filters = QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot;
	result.accountDirectories = QDir(accounts).entryList(filters, QDir::Name);
	if (!captureStore(root + u"/sync"_q, std::nullopt)) { return std::nullopt; }
	for (const auto &name : result.accountDirectories) {
		const auto path = QDir(accounts).filePath(name);
		const auto info = QFileInfo(path);
		if (name == u"root.toml"_q) { continue; }
		const auto account = ParseAccountKey(name);
		if (!account || info.isSymLink() || !info.isDir()) { return std::nullopt; }
		const auto identity = SyncPathIdentity(path);
		if (identity.isEmpty() || identity == "missing") { return std::nullopt; }
		result.identities[path] = identity;
		if (!captureStore(path + u"/sync"_q, account)) { return std::nullopt; }
	}
	for (const auto &[path, identity] : result.identities) {
		if (identity.isEmpty() || SyncPathIdentity(path) != identity) { return std::nullopt; }
	}
	if (QDir(accounts).entryList(filters, QDir::Name) != result.accountDirectories) { return std::nullopt; }
	return result;
}

std::optional<DesktopSyncRecovery> InspectSyncAccountRecovery(
		Main::Session &session,
		const SyncAccountInventoryResult *inventory) {
	const auto opening = AccountSettingsOpenTargetFor(&session, SettingsOpenRole::Own);
	if (!opening || !AccountSyncOperationsAllowed(&session)) { return std::nullopt; }
	auto result = DesktopSyncRecovery();
	result.opening = *opening;
	auto &input = result.input;
	input.account = SessionAccountKey(&session);
	input.startupOwner = SettingsStartupOwner();
	input.signedIn = !session.account().loggingOut() && session.account().maybeSession() == &session;
	input.preferenceToken = AccountSyncBindingToken(session.account());
	const auto target = AccountSyncInspectionTarget(&session);
	if (!target || !input.startupOwner) { return std::nullopt; }
	input.local = ReadSyncSettingsFile(target->files);
	if (input.local.status == SyncSettingsFileStatus::Present
		&& !input.local.usingLastGood && !CanApplyAccountSettingsDocument(u"settings.toml"_q, input.local.text)) {
		input.local.status = SyncSettingsFileStatus::Invalid;
	}
	const auto discovered = DiscoverDesktopSyncStores(opening->root);
	if (!discovered) { return std::nullopt; }
	input.stores = discovered->stores;
	result.identities = discovered->identities;
	result.accountDirectories = discovered->accountDirectories;
	for (const auto &entry : Core::App().domain().accounts()) {
		const auto live = entry.account->maybeSession();
		if (live == &session) { continue; }
		const auto token = AccountSyncBindingToken(*entry.account);
		if (!live) { if (!token.isEmpty()) { return std::nullopt; } continue; }
		input.otherPreferences.push_back({ SessionAccountKey(live), token });
	}
	input.discoveryComplete = true;
	if (inventory) {
		const auto complete = std::all_of(input.stores.begin(), input.stores.end(), [](const auto &store) {
			return !store.statePresent || bool(ParseSyncLocalState(store.state));
		});
		input.cloud = SyncRecoveryCloud{ input.account, complete, *inventory };
	}
	const auto journal = JournalPath(target->files);
	result.journalPresent = QFileInfo::exists(journal) || QFileInfo(journal).isSymLink();
	result.identities[journal] = SyncPathIdentity(journal);
	if (result.journalPresent && ReadSyncPrivateFile(journal, kJournalBytes, result.journal) != SyncStoreStatus::Ready) { return std::nullopt; }
	for (const auto &[path, identity] : result.identities) {
		if (identity.isEmpty() || SyncPathIdentity(path) != identity) { return std::nullopt; }
	}
	if (!ValidateAccountSettingsOpenTarget(&session, *opening)) { return std::nullopt; }
	result.plan = PlanSyncAccountRecovery(input);
	return result;
}

bool RelinkSyncAccount(Main::Session &session, const DesktopSyncRecovery &review, bool confirmed) {
	if (review.journalPresent || (review.plan.status != SyncAccountRecoveryStatus::AutoRelink
		&& (review.plan.status != SyncAccountRecoveryStatus::RootRelink || !confirmed))) { return false; }
	const auto target = AccountSyncInspectionTarget(&session);
	if (!target) { return false; }
	auto lock = QLockFile(target->files.syncDirectory + u"/lock"_q);
	lock.setStaleLockTime(0);
	if (!lock.tryLock(0)) { return false; }
	const auto current = InspectSyncAccountRecovery(session,
		review.input.cloud ? &review.input.cloud->inventory : nullptr);
	if (!current || !SameRecovery(review, *current) || !review.plan.store
		|| !ValidateAccountSettingsOpenTarget(&session, review.opening)) { return false; }
	const auto state = ParseSyncLocalState(review.input.stores[*review.plan.store].state);
	return state && state.state.bindingToken == review.plan.restoreToken
		&& RestoreAccountSyncBinding(state.state, session.account(), review.input.preferenceToken);
}

std::optional<SyncStartAgainPlan> ReadSyncArchiveJournal(
		const QString &root,
		const AccountFiles &files,
		const AccountKey &account) {
	auto bytes = QByteArray();
	if (ReadSyncPrivateFile(JournalPath(files), kJournalBytes, bytes) != SyncStoreStatus::Ready) { return std::nullopt; }
	const auto object = QJsonDocument::fromJson(bytes).object();
	const auto key = ParseAccountKey(object.value(u"account"_q).toString());
	if (object.value(u"version"_q).toInt() != 1 || !key || *key != account) { return std::nullopt; }
	auto plan = SyncStartAgainPlan();
	plan.status = SyncStartAgainStatus::Ready;
	plan.account = account;
	plan.archiveDirectory = object.value(u"archive"_q).toString();
	plan.history = object.value(u"history"_q).toString();
	plan.preferenceToken = QByteArray::fromBase64(object.value(u"token"_q).toString().toLatin1());
	plan.expected.statePresent = object.value(u"state_present"_q).toBool();
	plan.expected.state = QByteArray::fromBase64(object.value(u"state"_q).toString().toLatin1());
	plan.expected.pendingPresent = object.value(u"pending_present"_q).toBool();
	plan.expected.pendingComplete = true;
	const auto pending = object.value(u"pending"_q).toObject();
	for (auto i = pending.begin(); i != pending.end(); ++i) {
		plan.expected.pendingFiles[i.key()] = QByteArray::fromBase64(i.value().toString().toLatin1());
	}
	const auto relative = QDir(root).relativeFilePath(files.syncDirectory);
	if (relative.startsWith(u"../"_q) || QDir::isAbsolutePath(relative)) { return std::nullopt; }
	const auto relativeFiles = [&] {
		auto result = files;
		result.syncDirectory = relative;
		result.syncStore = QDir(root).relativeFilePath(files.syncStore);
		result.history = QDir(root).relativeFilePath(files.history);
		return result;
	}();
	if (plan.expected.pendingPresent) {
		plan.moves.push_back({ SyncOrphanMoveKind::PendingDirectory,
			relative + u"/pending"_q, plan.archiveDirectory + u"/pending"_q });
	}
	if (plan.expected.statePresent) {
		plan.moves.push_back({ SyncOrphanMoveKind::StateFile,
			relativeFiles.syncStore, plan.archiveDirectory + u"/state.json"_q });
	}
	if (!ValidArchivePlan(relativeFiles, plan) || SerializeArchive(plan) != bytes) { return std::nullopt; }
	return plan;
}

SyncArchiveResult ArchiveSyncAccountState(
		const QString &root,
		const AccountFiles &files,
		const SyncStartAgainPlan &plan,
		const std::function<bool()> &current) {
	auto result = SyncArchiveResult();
	const auto absolute = [&](const QString &path) { return QDir(root).filePath(path); };
	auto relativeFiles = files;
	relativeFiles.syncDirectory = QDir(root).relativeFilePath(files.syncDirectory);
	relativeFiles.syncStore = QDir(root).relativeFilePath(files.syncStore);
	relativeFiles.history = QDir(root).relativeFilePath(files.history);
	if (!ValidArchivePlan(relativeFiles, plan) || !current || !current()
		|| EnsureSyncPrivateDirectory(files.syncDirectory) != SyncStoreStatus::Ready) { return result; }
	const auto rootIdentity = SyncPathIdentity(root);
	const auto storeIdentity = SyncPathIdentity(files.syncDirectory);
	if (rootIdentity.isEmpty() || storeIdentity.isEmpty()) { return result; }
	const auto active = [&] {
		return current() && SyncPathIdentity(root) == rootIdentity
			&& SyncPathIdentity(files.syncDirectory) == storeIdentity;
	};
	const auto archive = absolute(plan.archiveDirectory);
	const auto journal = JournalPath(files);
	const auto journalBytes = SerializeArchive(plan);
	const auto journalInfo = QFileInfo(journal);
	const auto resume = journalInfo.exists() || journalInfo.isSymLink();
	if (resume) {
		auto saved = QByteArray();
		if (ReadSyncPrivateFile(journal, kJournalBytes, saved) != SyncStoreStatus::Ready
			|| saved != journalBytes) { return result; }
	} else {
		const auto original = ReadSyncRecoveryStore(files, plan.expected.directoryAccount);
		if (original.status != SyncStoreStatus::Ready || !SameStore(original.store, plan.expected)
			|| SyncPathIdentity(archive) != "missing" || !active()) { return result; }
		auto file = QFile(journal);
		if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { return result; }
		if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
			|| file.write(journalBytes) != journalBytes.size() || !file.flush()) { return result; }
#ifdef Q_OS_WIN
		if (!FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle())))) { return result; }
#else
		if (::fsync(file.handle()) != 0) { return result; }
#endif
		file.close();
		if (!FlushSyncDirectory(files.syncDirectory)) { return result; }
		auto saved = QByteArray();
		if (ReadSyncPrivateFile(journal, kJournalBytes, saved) != SyncStoreStatus::Ready
			|| saved != journalBytes) { return result; }
	}
	result.partial = true;
	result.archive = archive;
	const auto journalIdentity = SyncPathIdentity(journal);
	const auto journalCurrent = [&] {
		auto bytes = QByteArray();
		return active() && SyncPathIdentity(journal) == journalIdentity
			&& ReadSyncPrivateFile(journal, kJournalBytes, bytes) == SyncStoreStatus::Ready
			&& bytes == journalBytes;
	};
	if (!journalCurrent()) { return result; }
	if (SyncPathIdentity(archive) == "missing") {
		if (!QDir().mkdir(archive, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) { return result; }
	} else if (!resume) { return result; }
	if (EnsureSyncPrivateDirectory(archive) != SyncStoreStatus::Ready) { return result; }
	if (!FlushSyncDirectory(files.syncDirectory)) { return result; }
	const auto archiveIdentity = SyncPathIdentity(archive);
	const auto archiveShape = [&] {
		const auto names = QDir(archive).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
		return std::all_of(names.begin(), names.end(), [&](const auto &name) {
			return (name == u"pending"_q && plan.expected.pendingPresent)
				|| (name == u"state.json"_q && plan.expected.statePresent);
		});
	};
	if (!archiveShape()) { return result; }
	for (const auto &move : plan.moves) {
		if (!journalCurrent() || SyncPathIdentity(archive) != archiveIdentity) { return result; }
		const auto original = ReadSyncRecoveryStore(files, plan.expected.directoryAccount);
		const auto archived = ReadSyncRecoveryStore(StoreFiles(archive), plan.expected.directoryAccount);
		if (original.status != SyncStoreStatus::Ready || archived.status != SyncStoreStatus::Ready) { return result; }
		const auto pending = move.kind == SyncOrphanMoveKind::PendingDirectory;
		const auto sourcePresent = pending ? original.store.pendingPresent : original.store.statePresent;
		const auto destinationPresent = pending ? archived.store.pendingPresent : archived.store.statePresent;
		const auto matches = [&](const SyncRecoveryStore &store) {
			return pending ? store.pendingComplete && store.pendingFiles == plan.expected.pendingFiles
				: store.state == plan.expected.state;
		};
		if (sourcePresent) {
			if (destinationPresent || !matches(original.store)
				|| !MoveExclusive(absolute(move.source), absolute(move.destination))) { return result; }
		} else if (!destinationPresent || !matches(archived.store)) { return result; }
		const auto verified = ReadSyncRecoveryStore(StoreFiles(archive), plan.expected.directoryAccount);
		if (verified.status != SyncStoreStatus::Ready || !matches(verified.store)) { return result; }
	}
	const auto original = ReadSyncRecoveryStore(files, plan.expected.directoryAccount);
	const auto archived = ReadSyncRecoveryStore(StoreFiles(archive), plan.expected.directoryAccount);
	if (original.status != SyncStoreStatus::Ready || archived.status != SyncStoreStatus::Ready
		|| !journalCurrent() || !archiveShape() || SyncPathIdentity(archive) != archiveIdentity) { return result; }
	auto observation = SyncStartAgainObservation();
	observation.account = plan.account;
	observation.complete = true;
	observation.originalStatePresent = original.store.statePresent;
	observation.originalPendingPresent = original.store.pendingPresent;
	if (archived.store.statePresent) { observation.archivedState = archived.store.state; }
	if (archived.store.pendingPresent) { observation.archivedPending = archived.store.pendingFiles; }
	observation.preferenceToken = plan.preferenceToken;
	const auto completion = CompleteSyncStartAgain(plan, observation);
	if (completion.status != SyncStartAgainCompletionStatus::SetupReady) { return result; }
	if (!journalCurrent() || !QFile::remove(journal)) { return result; }
	if (!FlushSyncDirectory(files.syncDirectory)) { return result; }
	result.ready = true;
	result.partial = false;
	return result;
}

SyncArchiveResult StartAgainSyncAccount(
		Main::Session &session,
		const DesktopSyncRecovery &review,
		bool confirmed,
		bool operationInFlight) {
	if (!confirmed || operationInFlight || SyncConfigSendQueueOf(&session) != SyncConfigSendQueue::Empty
		|| !ValidateAccountSettingsOpenTarget(&session, review.opening)) { return {}; }
	const auto target = AccountSyncInspectionTarget(&session);
	if (!target) { return {}; }
	auto lock = QLockFile(target->files.syncDirectory + u"/lock"_q);
	lock.setStaleLockTime(0);
	if (!lock.tryLock(0)) { return {}; }
	const auto fresh = InspectSyncAccountRecovery(session, review.input.cloud ? &review.input.cloud->inventory : nullptr);
	if (!fresh || !SameRecovery(review, *fresh)
		|| fresh->plan.status == SyncAccountRecoveryStatus::Ambiguous
		|| fresh->plan.status == SyncAccountRecoveryStatus::InvalidInput
		|| fresh->plan.status == SyncAccountRecoveryStatus::WaitingForSignIn) { return {}; }
	auto plan = std::optional<SyncStartAgainPlan>();
	if (review.journalPresent) {
		plan = ReadSyncArchiveJournal(review.opening.root, target->files, review.input.account);
	} else {
		const auto date = QDateTime::currentDateTimeUtc().toString(u"yyyyMMdd-HHmmss"_q);
		const auto archive = target->files.syncDirectory + u"/orphaned-"_q + date;
		plan = PlanSyncStartAgain({ review.input, date, SyncPathIdentity(archive) == "missing",
			true, operationInFlight, SyncConfigSendQueueOf(&session) });
	}
	if (!plan || plan->status != SyncStartAgainStatus::Ready
		|| plan->preferenceToken != AccountSyncBindingToken(session.account())) { return {}; }
	const auto current = [&] {
		return ValidateAccountSettingsOpenTarget(&session, review.opening)
			&& AccountSyncBindingToken(session.account()) == plan->preferenceToken
			&& SyncConfigSendQueueOf(&session) == SyncConfigSendQueue::Empty;
	};
	return ArchiveSyncAccountState(review.opening.root, target->files, *plan, current);
}

}
