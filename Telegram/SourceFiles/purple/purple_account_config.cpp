/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_account_config.h"
#include "purple/purple_account_appwide_removal.h"
#include "purple/purple_config_diff.h"
#include "purple/purple_state.h"
#include "purple/purple_config_payload.h"
#include "purple/purple_engine.h"
#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#else
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif
#define TOML_EXCEPTIONS 0
#include <tomlplusplus/toml.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QFile>
#include <QtCore/QUuid>
#include <QtCore/qscopeguard.h>

#include <algorithm>

namespace Purple {
namespace {

QByteArray DirectoryIdentity(const QString &path) {
	const auto info = QFileInfo(path);
	if (info.isSymLink() || !info.isDir()) {
		return {};
	}
#ifdef Q_OS_WIN
	const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
		0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return {};
	}
	auto identity = BY_HANDLE_FILE_INFORMATION();
	const auto ok = GetFileInformationByHandle(handle, &identity);
	CloseHandle(handle);
	return ok ? QByteArray::number(identity.dwVolumeSerialNumber) + ':'
		+ QByteArray::number(identity.nFileIndexHigh) + ':'
		+ QByteArray::number(identity.nFileIndexLow) : QByteArray();
#else
	struct stat identity = {};
	return ::stat(QFile::encodeName(path).constData(), &identity) == 0
		? QByteArray::number(qulonglong(identity.st_dev)) + ':'
			+ QByteArray::number(qulonglong(identity.st_ino)) : QByteArray();
#endif
}

// Creation pins the existing owner/accounts directory before creating one child.
// POSIX writes through directory descriptors and publishes a complete temporary
// inode with linkat, which cannot replace an existing destination. Windows holds
// directory handles without delete sharing and moves without replace permission.
// Final path/session checks detect stale routing but are not a cross-process CAS.
bool CreateAccountSettingsFile(
		const QString &root,
		const QString &name,
		const QByteArray &bytes,
		const QByteArray &expectedAccounts,
		const QByteArray &expectedDirectory,
		const std::function<bool()> &current) {
	if (expectedAccounts.isEmpty() || name.isEmpty() || name.contains('/') || name.contains('\\')
		|| name == u"."_q || name == u".."_q || !current()) {
		return false;
	}
	const auto physicalRoot = QFileInfo(root).canonicalFilePath();
	if (physicalRoot.isEmpty() || DirectoryIdentity(physicalRoot) != DirectoryIdentity(root)) {
		return false;
	}
	const auto accountsPath = physicalRoot + u"/accounts"_q;
	const auto directory = accountsPath + u"/"_q + name;
#ifdef Q_OS_WIN
	const auto openDirectory = [](const QString &path) {
		return CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
			FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	};
	const auto validDirectory = [](HANDLE handle) {
		auto info = BY_HANDLE_FILE_INFORMATION();
		return handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &info)
			&& (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			&& !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
	};
	const auto rootHandle = openDirectory(physicalRoot);
	const auto closeRoot = qScopeGuard([&] { if (rootHandle != INVALID_HANDLE_VALUE) { CloseHandle(rootHandle); } });
	if (!validDirectory(rootHandle) || !current()) { return false; }
	const auto accountsHandle = openDirectory(accountsPath);
	const auto closeAccounts = qScopeGuard([&] { if (accountsHandle != INVALID_HANDLE_VALUE) { CloseHandle(accountsHandle); } });
	if (!validDirectory(accountsHandle) || DirectoryIdentity(accountsPath) != expectedAccounts
		|| !current()) { return false; }
	if (expectedDirectory.isEmpty()) {
		if (!CreateDirectoryW(reinterpret_cast<LPCWSTR>(directory.utf16()), nullptr)) { return false; }
	} else if (DirectoryIdentity(directory) != expectedDirectory) {
		return false;
	}
	const auto directoryHandle = openDirectory(directory);
	const auto closeDirectory = qScopeGuard([&] { if (directoryHandle != INVALID_HANDLE_VALUE) { CloseHandle(directoryHandle); } });
	if (!validDirectory(directoryHandle) || !current()
		|| DirectoryIdentity(accountsPath) != expectedAccounts
		|| (!expectedDirectory.isEmpty() && DirectoryIdentity(directory) != expectedDirectory)) { return false; }
	const auto temporary = directory + u"/.settings-"_q
		+ QUuid::createUuid().toString(QUuid::WithoutBraces);
	const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(temporary.utf16()),
		GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE) { return false; }
	const auto cleanTemporary = qScopeGuard([&] { DeleteFileW(reinterpret_cast<LPCWSTR>(temporary.utf16())); });
	auto written = DWORD();
	const auto complete = WriteFile(handle, bytes.constData(), DWORD(bytes.size()), &written, nullptr)
		&& written == DWORD(bytes.size()) && FlushFileBuffers(handle);
	CloseHandle(handle);
	const auto destination = directory + u"/settings.toml"_q;
	return complete && current() && MoveFileExW(
		reinterpret_cast<LPCWSTR>(temporary.utf16()),
		reinterpret_cast<LPCWSTR>(destination.utf16()), MOVEFILE_WRITE_THROUGH);
#else
	const auto rootFd = ::open(QFile::encodeName(physicalRoot).constData(),
		O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (rootFd < 0) { return false; }
	const auto closeRoot = qScopeGuard([&] { ::close(rootFd); });
	const auto identity = [](int fd) {
		struct stat info = {};
		return ::fstat(fd, &info) == 0 && S_ISDIR(info.st_mode)
			? QByteArray::number(qulonglong(info.st_dev)) + ':'
				+ QByteArray::number(qulonglong(info.st_ino)) : QByteArray();
	};
	if (identity(rootFd) != DirectoryIdentity(root) || !current()) { return false; }
	const auto accountsFd = ::openat(rootFd, "accounts", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (accountsFd < 0) { return false; }
	const auto closeAccounts = qScopeGuard([&] { ::close(accountsFd); });
	const auto nameBytes = QFile::encodeName(name);
	const auto parentsCurrent = [&] {
		return current() && identity(rootFd) == DirectoryIdentity(root)
			&& identity(accountsFd) == expectedAccounts
			&& identity(accountsFd) == DirectoryIdentity(accountsPath);
	};
	if (!parentsCurrent()) { return false; }
	if (expectedDirectory.isEmpty()) {
		if (::mkdirat(accountsFd, nameBytes.constData(), 0700) != 0) { return false; }
	} else if (DirectoryIdentity(directory) != expectedDirectory) {
		return false;
	}
	const auto directoryFd = ::openat(accountsFd, nameBytes.constData(),
		O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (directoryFd < 0) { return false; }
	const auto closeDirectory = qScopeGuard([&] { ::close(directoryFd); });
	const auto destinationCurrent = [&] {
		return parentsCurrent() && identity(directoryFd) == DirectoryIdentity(directory)
			&& (expectedDirectory.isEmpty() || identity(directoryFd) == expectedDirectory);
	};
	if (!destinationCurrent()) { return false; }
	const auto temporary = QByteArray(".settings-")
		+ QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
	const auto fd = ::openat(directoryFd, temporary.constData(),
		O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) { return false; }
	const auto closeFile = qScopeGuard([&] { ::close(fd); });
	const auto cleanTemporary = qScopeGuard([&] { ::unlinkat(directoryFd, temporary.constData(), 0); });
	auto offset = qsizetype(0);
	while (offset != bytes.size()) {
		const auto written = ::write(fd, bytes.constData() + offset, bytes.size() - offset);
		if (written < 0 && errno == EINTR) { continue; }
		if (written <= 0) { return false; }
		offset += written;
	}
	if (::fsync(fd) != 0 || !destinationCurrent()
		|| ::linkat(directoryFd, temporary.constData(), directoryFd, "settings.toml", 0) != 0) {
		return false;
	}
	return ::fsync(directoryFd) == 0;
#endif
}

bool RowDirectorySafe(const QString &path) {
	const auto info = QFileInfo(path);
	return !info.isSymLink() && (!info.exists() || info.isDir());
}

QString RowTable(AccountSettingsRow row) {
	return row == AccountSettingsRow::SendAfterSave ? u"sync"_q : u"last_seen"_q;
}

QString RowKey(AccountSettingsRow row) {
	switch (row) {
	case AccountSettingsRow::LastSeenReasons: return u"reasons_p"_q;
	case AccountSettingsRow::LastSeenPeek: return u"trade_p"_q;
	case AccountSettingsRow::SendAfterSave: return u"send_after_save_p"_q;
	}
	return {};
}

bool RowInputUsable(const SettingsFileInput &input, const QString &path) {
	if (input.state == SettingsFileState::Missing) { return true; }
	const auto text = QString::fromUtf8(input.bytes);
	const auto parsed = ParseSettings(text, path);
	return input.state == SettingsFileState::Readable && text.toUtf8() == input.bytes
		&& parsed.ok() && parsed.settings.version <= kHighestSettingsVersion;
}

bool HasOwnRowKey(const SettingsFileInput &input, AccountSettingsRow row) {
	if (input.state != SettingsFileState::Readable) { return false; }
	const auto parsed = toml::parse(std::string_view(input.bytes.constData(), input.bytes.size()));
	if (!parsed) { return false; }
	const auto table = RowTable(row).toStdString();
	const auto key = RowKey(row).toStdString();
	const auto values = parsed.table()[table].as_table();
	return values && values->contains(key);
}

QByteArray ChoiceFileIdentity(const QString &path) {
	const auto info = QFileInfo(path);
	if (!info.exists() && !info.isSymLink()) { return QByteArray("missing"); }
	if (!AccountFilePathSafe(path)) { return {}; }
#ifdef Q_OS_WIN
	const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
		0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (handle == INVALID_HANDLE_VALUE) { return {}; }
	auto value = BY_HANDLE_FILE_INFORMATION();
	const auto ok = GetFileInformationByHandle(handle, &value);
	CloseHandle(handle);
	return ok ? QByteArray::number(value.dwVolumeSerialNumber) + ':'
		+ QByteArray::number(value.nFileIndexHigh) + ':' + QByteArray::number(value.nFileIndexLow)
		+ ':' + QByteArray::number(value.ftLastWriteTime.dwHighDateTime)
		+ ':' + QByteArray::number(value.ftLastWriteTime.dwLowDateTime) : QByteArray();
#else
	struct stat value = {};
	if (::lstat(QFile::encodeName(path).constData(), &value) != 0) { return {}; }
#ifdef Q_OS_MAC
	const auto modified = value.st_mtimespec;
	const auto changed = value.st_ctimespec;
#else
	const auto modified = value.st_mtim;
	const auto changed = value.st_ctim;
#endif
	return QByteArray::number(qulonglong(value.st_dev)) + ':'
		+ QByteArray::number(qulonglong(value.st_ino)) + ':' + QByteArray::number(qlonglong(value.st_size))
		+ ':' + QByteArray::number(qlonglong(modified.tv_sec)) + ':' + QByteArray::number(qlonglong(modified.tv_nsec))
		+ ':' + QByteArray::number(qlonglong(changed.tv_sec)) + ':' + QByteArray::number(qlonglong(changed.tv_nsec));
#endif
}

std::optional<QString> ChoiceText(
		const QString &text, const QString &path, const AccountKey &account) {
	const auto key = AccountKeyText(account.environment, account.userId);
	const auto bytes = text.toUtf8();
	const auto parsed = toml::parse(std::string_view(bytes.constData(), bytes.size()), path.toStdString());
	if (!key || !parsed) { return std::nullopt; }
	const auto value = u"\"%1\""_q.arg(*key);
	const auto node = parsed.table().get("global_account");
	const auto owner = ParseLocalChoice(text, path).choice.rootFileAccount;
	if (!owner) { return std::nullopt; }
	const auto selectedPath = account == *owner
		? u"settings.toml"_q : u"accounts/%1/settings.toml"_q.arg(*key);
	const auto pathComment = u"# That account's file on this device: "_q + selectedPath;
	if (!node) { return u"global_account = %1\n%2\n"_q.arg(value, pathComment) + text; }
	const auto begin = node->source().begin;
	const auto end = node->source().end;
	if (node->is_table() || node->is_array() || begin.line != end.line) { return std::nullopt; }
	auto lines = text.split('\n');
	if (begin.line < 1 || begin.line > lines.size()) { return std::nullopt; }
	const auto points = lines[begin.line - 1].toUcs4();
	const auto from = int(begin.column) - 1;
	const auto till = int(end.column) - 1;
	if (from < 0 || till < from || till > points.size()) { return std::nullopt; }
	lines[begin.line - 1] = QString::fromUcs4(points.constData(), from) + value
		+ QString::fromUcs4(points.constData() + till, points.size() - till);
	if (begin.line < lines.size() && lines[begin.line].startsWith(u"# That account's file on this device: "_q)) {
		lines[begin.line] = pathComment;
	}
	return lines.join('\n');
}

SettingsFileInput OwnerInput(const QString &path) {
	const auto info = QFileInfo(path);
	if (info.isSymLink() || (info.exists() && !info.isFile())) {
		return { SettingsFileState::ReadError };
	}
	return ReadSettingsFileInput(path);
}

OwnerFileSnapshot Snapshot(const SettingsFileInput &input) {
	return { input.state != SettingsFileState::Missing,
		QString::fromUtf8(input.bytes) };
}

}

std::optional<AccountAppWideRemovalTarget> PreviewAccountAppWideRemoval(
		DesktopAccountConfig &config, const AccountKey &account) {
	const auto destination = config.openTarget(account, SettingsOpenRole::Own);
	if (!destination || destination->requester != account
		|| destination->file.account != account || destination->global == account
		|| destination->file.input.state != SettingsFileState::Readable) { return std::nullopt; }
	const auto &input = destination->file.input;
	const auto text = QString::fromUtf8(input.bytes);
	if (text.toUtf8() != input.bytes || !RowInputUsable(input, destination->file.path)) { return std::nullopt; }
	const auto splice = RemoveAppWideSettings(text, destination->file.path);
	if (!splice.ok()) { return std::nullopt; }
	const auto summary = SummarizeConfigChange(input.bytes, splice.text.toUtf8());
	if (!summary.parsed) { return std::nullopt; }
	auto result = AccountAppWideRemovalTarget();
	result.destination = *destination;
	result.successor = splice.text.toUtf8();
	for (const auto &entry : summary.entries) {
		for (const auto &key : entry.keys) {
			if (key.scope == SettingScope::AppWide && key.kind == ConfigChangeKind::Removed) {
				result.removed.push_back(key);
			}
		}
	}
	const auto paths = config.files(account);
	if (!paths || !RowDirectorySafe(paths->directory)
		|| destination->accountDirectoryIdentity.isEmpty()) { return std::nullopt; }
	result.backup = OwnerInput(paths->backup);
	result.writable = AccountFilePathSafe(paths->backup)
		&& result.backup.state != SettingsFileState::ReadError
		&& QFileInfo(destination->file.path).isWritable()
		&& QFileInfo(paths->directory).isWritable()
		&& (!QFileInfo::exists(paths->backup) || QFileInfo(paths->backup).isWritable());
	for (const auto &path : { QDir(destination->root).filePath(u"local.toml"_q),
			QDir(destination->root).filePath(u"accounts/root.toml"_q), destination->file.path, paths->backup }) {
		const auto identity = ChoiceFileIdentity(path);
		result.writable = result.writable && !identity.isEmpty();
		result.identities.emplace(path, identity);
	}
	result.revision = config.generation();
	return result;
}

bool ValidateAccountAppWideRemoval(
		DesktopAccountConfig &config, const AccountAppWideRemovalTarget &target) {
	if (!target.writable || config.generation() != target.revision
		|| !config.validateOpen(target.destination)) { return false; }
	const auto current = PreviewAccountAppWideRemoval(config, target.destination.requester);
	return current && current->writable && current->revision == target.revision
		&& current->destination.file.input == target.destination.file.input
		&& current->successor == target.successor && current->backup == target.backup
		&& current->identities == target.identities;
}

AccountAppWideRemovalResult CommitAccountAppWideRemoval(
		DesktopAccountConfig &config,
		const AccountAppWideRemovalTarget &target,
		const std::function<bool()> &originCurrent) {
	auto expected = target;
	const auto current = [&] {
		return originCurrent && originCurrent() && ValidateAccountAppWideRemoval(config, expected);
	};
	if (!current()) { return { .error = u"The account settings context changed or is unavailable."_q }; }
	if (target.removed.empty()) { return { .ok = true }; }
	const auto paths = config.files(target.destination.requester);
	if (!paths) { return { .error = u"The account settings destination is unavailable."_q }; }
	auto result = AccountAppWideRemovalResult();
	const auto before = target.destination.file.input.bytes;
	if (target.backup.state != SettingsFileState::Readable || target.backup.bytes != before) {
		if (WriteGuardedSettingsFile(paths->backup, target.backup, before, current, true)
			!= GuardedFileWrite::Written) {
			return { .error = u"The account settings backup could not be written safely."_q };
		}
		result.backupChanged = true;
	}
	expected.backup = { SettingsFileState::Readable, before };
	expected.identities[paths->backup] = ChoiceFileIdentity(paths->backup);
	if (!current() || WriteGuardedSettingsFile(target.destination.file.path,
			target.destination.file.input, target.successor, current) != GuardedFileWrite::Written) {
		result.error = u"No main-file replacement was confirmed; the backup may have been updated."_q;
		return result;
	}
	result.changed = true;
	result.committedIdentity = ChoiceFileIdentity(target.destination.file.path);
	result.backupIdentity = expected.identities[paths->backup];
	const auto committedCurrent = [&] {
		return originCurrent && originCurrent() && ValidateCommittedAppWideRemoval(config, target, result);
	};
	if (!committedCurrent()) {
		result.error = u"Settings were saved, but their context changed after publication."_q;
		return result;
	}
	config.forgetAccountSave(target.destination.requester);
	config.refreshSettings();
	if (!committedCurrent()) {
		result.error = u"Settings were saved, but their context changed while reloading."_q;
		return result;
	}
	result.ok = true;
	return result;
}

bool ValidateCommittedAppWideRemoval(
		DesktopAccountConfig &config,
		const AccountAppWideRemovalTarget &target,
		const AccountAppWideRemovalResult &result) {
	if (!result.changed || result.committedIdentity.isEmpty() || result.backupIdentity.isEmpty()) { return false; }
	auto committed = target.destination;
	committed.file.input = { SettingsFileState::Readable, target.successor };
	const auto paths = config.files(committed.requester);
	if (!paths || !config.validateOpen(committed)
		|| ChoiceFileIdentity(committed.file.path) != result.committedIdentity
		|| OwnerInput(paths->backup) != SettingsFileInput{ SettingsFileState::Readable, target.destination.file.input.bytes }
		|| ChoiceFileIdentity(paths->backup) != result.backupIdentity) { return false; }
	for (const auto &[path, identity] : target.identities) {
		if (path != paths->backup && path != committed.file.path
			&& ChoiceFileIdentity(path) != identity) { return false; }
	}
	return true;
}

bool AccountSyncRecordSupported(const QByteArray &record) {
	const auto parsed = ParseSyncEnvelope(record);
	const auto inspected = InspectConfigPayload(parsed);
	if (!parsed || inspected.status != ConfigPayloadStatus::Valid || inspected.schema > kSettingsVersion) {
		return false;
	}
	const auto canonical = SerializeSyncEnvelope(parsed.envelope);
	return canonical && canonical.canonical == record;
}

bool AccountFilePathSafe(const QString &path) {
	const auto info = QFileInfo(path);
	if (info.isSymLink() || (info.exists() && !info.isFile())) {
		return false;
	}
	if (!info.exists()) {
		return true;
	}
#ifdef Q_OS_WIN
	const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
		0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (handle == INVALID_HANDLE_VALUE) { return false; }
	auto identity = BY_HANDLE_FILE_INFORMATION();
	const auto ok = GetFileInformationByHandle(handle, &identity);
	CloseHandle(handle);
	return ok && identity.nNumberOfLinks == 1;
#else
	struct stat identity = {};
	return ::lstat(QFile::encodeName(path).constData(), &identity) == 0
		&& S_ISREG(identity.st_mode) && identity.st_nlink == 1;
#endif
}

bool CanReplaceLegacyRootSettings(const QByteArray &before, const QByteArray &after) {
	const auto parsed = ParseSettings(QString::fromUtf8(after), u"settings.toml"_q);
	if (!parsed.ok() || parsed.settings.version > kSettingsVersion) {
		return false;
	}
	const auto summary = SummarizeConfigChange(before, after);
	if (!summary.parsed) {
		return false;
	}
	for (const auto &entry : summary.entries) {
		for (const auto &key : entry.keys) {
			if (key.kind == ConfigChangeKind::Removed
				&& (key.scope == SettingScope::AppWide || key.scope == SettingScope::Unknown)) {
				return false;
			}
		}
	}
	return true;
}

QByteArray NewAccountSettingsText() {
	return QByteArray(
		"# Purple Telegram settings for one account.\n"
		"# This file holds only what differs for this account. Anything it leaves out\n"
		"# comes from the account that supplies app-wide settings on each device.\n"
		"# Work Mode, Local Premium and the other app-wide settings written here take\n"
		"# effect only on devices where this account supplies the app-wide settings.\n"
		"# See config.md, \"Settings per account\".\n"
		"version = 2\n");
}

QString SettingsDocumentFileName(int version) {
	return version >= 2 ? u"account-settings.toml"_q : u"settings.toml"_q;
}

LegacySettingsDocumentKind ClassifySettingsDocumentName(const QString &name) {
	if (name.compare(SettingsDocumentFileName(1), Qt::CaseInsensitive) == 0) {
		return LegacySettingsDocumentKind::Whole;
	} else if (name.compare(SettingsDocumentFileName(2), Qt::CaseInsensitive) == 0) {
		return LegacySettingsDocumentKind::Layered;
	}
	return LegacySettingsDocumentKind::Unrelated;
}

bool CanApplyWholeSettingsDocument(const QString &name, const QByteArray &bytes) {
	const auto parsed = ParseSettings(QString::fromUtf8(bytes), name);
	return ClassifySettingsDocumentName(name) == LegacySettingsDocumentKind::Whole
		&& parsed.ok() && parsed.settings.version <= kSettingsVersion;
}

bool AccountFilteringEnabled(bool active, AccountWorkMode mode) {
	return active && mode != AccountWorkMode::Stock;
}

DesktopAccountConfig::DesktopAccountConfig(QString root)
: _root(std::move(root)) {
	start({});
}

DesktopAccountConfig::~DesktopAccountConfig() = default;

QString DesktopAccountConfig::absolute(const QString &relative) const {
	return _root + u"/"_q + relative;
}

void DesktopAccountConfig::invalidate() {
	++_generation;
}

void DesktopAccountConfig::start(std::vector<StartupAccountCandidate> accounts,
		std::optional<AccountKey> choice) {
	if (!QDir::isAbsolutePath(_root)) {
		_plan = {};
		_plan.error = u"An absolute config directory is required."_q;
		_persistFailed = true;
		return;
	}
	if (_persistFailed) {
		return;
	}
	_accounts = std::move(accounts);
	if (_owner) {
		refreshLocalChoice();
		watch();
		return;
	}
	const auto localPath = absolute(u"local.toml"_q);
	const auto recordPath = absolute(u"accounts/root.toml"_q);
	const auto local = OwnerInput(localPath);
	const auto record = OwnerInput(recordPath);
	const auto accountsPresent = QFileInfo::exists(absolute(u"accounts"_q));
	_persistFailed = false;
	if (local.state == SettingsFileState::ReadError
		|| record.state == SettingsFileState::ReadError
		|| QFileInfo(absolute(u"accounts"_q)).isSymLink()
		|| (accountsPresent && !QFileInfo(absolute(u"accounts"_q)).isDir())) {
		_plan = {};
		_plan.error = u"Owner record could not be read. Account operations are held."_q;
		_persistFailed = true;
		invalidate();
		return;
	}
	auto input = AccountOwnerStartupInput{ Snapshot(local), Snapshot(record),
		accountsPresent, _accounts, choice };
	_plan = PlanAccountOwnerStartup(input);
	if (!_plan.writes.empty()) {
		if (!persistPlan(_plan, local, record, accountsPresent)) {
			_persistFailed = true;
			_plan.error = u"Owner records changed or could not be saved. Restart to recover."_q;
			invalidate();
			return;
		}
		input.localChoice = Snapshot(ReadSettingsFileInput(localPath));
		input.ownerRecord = Snapshot(ReadSettingsFileInput(recordPath));
		input.accountsDirectoryPresent = true;
		input.chosenAccount.reset();
		const auto notice = _plan.restoredHandEditedOwnerNotice;
		_plan = PlanAccountOwnerStartup(input);
		_plan.restoredHandEditedOwnerNotice = notice;
	}
	if (_plan.status == AccountOwnerStartupStatus::Owned && _plan.startupOwner) {
		_owner = _plan.startupOwner;
		_choice = _plan.localChoice;
	}
	++_routingGeneration;
	_localInput = ReadSettingsFileInput(localPath);
	invalidate();
	watch();
	if (_changed) {
		_changed();
	}
}

bool DesktopAccountConfig::persistPlan(const AccountOwnerStartupPlan &plan,
		SettingsFileInput local, SettingsFileInput record, bool accountsPresent) {
	const auto localPath = absolute(u"local.toml"_q);
	const auto recordPath = absolute(u"accounts/root.toml"_q);
	const auto accountPresence = [&] {
		for (const auto &account : _accounts) {
			const auto key = AccountKeyText(account.account.environment, account.account.userId);
			if (!key || QFileInfo::exists(absolute(u"accounts/%1"_q.arg(*key)))
				!= account.hasAccountDirectory) {
				return false;
			}
		}
		return true;
	};
	const auto current = [&] {
		return OwnerInput(localPath) == local
			&& OwnerInput(recordPath) == record
			&& !QFileInfo(absolute(u"accounts"_q)).isSymLink()
			&& QFileInfo::exists(absolute(u"accounts"_q)) == accountsPresent
			&& accountPresence();
	};
	for (const auto &write : plan.writes) {
		if (!current()) {
			return false;
		}
		if (write.target == AccountOwnerWriteTarget::OwnerRecordCopy2) {
			if (!QDir().mkpath(absolute(u"accounts"_q))) {
				return false;
			}
			accountsPresent = true;
		}
		const auto path = absolute(write.relativePath);
		const auto bytes = write.text.toUtf8();
		if (WriteGuardedSettingsFile(path,
			write.target == AccountOwnerWriteTarget::OwnerRecordCopy2 ? record : local,
			bytes, current, true) != GuardedFileWrite::Written) {
			return false;
		}
		const auto actual = ReadSettingsFileInput(path);
		if (actual.state != SettingsFileState::Readable || actual.bytes != bytes) {
			return false;
		}
		if (write.target == AccountOwnerWriteTarget::OwnerRecordCopy2) {
			record = actual;
		} else {
			local = actual;
		}
	}
	return current();
}

void DesktopAccountConfig::refreshLocalChoice() {
	if (!_owner) {
		return;
	}
	const auto input = OwnerInput(absolute(u"local.toml"_q));
	if (input == _localInput) {
		return;
	}
	_localInput = input;
	++_routingGeneration;
	if (input.state == SettingsFileState::Readable) {
		const auto parsed = ParseLocalChoice(QString::fromUtf8(input.bytes),
			absolute(u"local.toml"_q));
		if (parsed.ok()) {
			_choice = parsed.choice;
			_choice.rootFileAccount = _owner;
			_plan.error.clear();
		} else {
			_plan.error = u"local.toml does not parse; keeping the previous global choice."_q;
		}
	} else {
		_plan.error = u"local.toml is unavailable; keeping the previous global choice."_q;
	}
	invalidate();
	watch();
}

const AccountOwnerStartupPlan &DesktopAccountConfig::ownerPlan() const {
	return _plan;
}

std::optional<AccountKey> DesktopAccountConfig::owner() const {
	return _owner;
}

std::optional<AccountKey> DesktopAccountConfig::globalAccount() const {
	return _owner ? GlobalAccountFor(_choice, *_owner) : std::nullopt;
}

QString DesktopAccountConfig::globalSettingsPath() const {
	const auto global = globalAccount();
	const auto selected = global ? files(*global) : std::nullopt;
	return !QDir::isAbsolutePath(_root) ? QString()
		: global ? (selected ? selected->settings : QString())
		: absolute(u"settings.toml"_q);
}

std::optional<AccountFiles> DesktopAccountConfig::files(const AccountKey &key) const {
	if (!QDir::isAbsolutePath(_root)) {
		return std::nullopt;
	}
	auto result = ResolveAccountFiles(key, _owner, AccountFileLayout::Desktop);
	if (!result) {
		return std::nullopt;
	}
	if (key != *_owner && (QFileInfo(absolute(u"accounts"_q)).isSymLink()
		|| QFileInfo(absolute(result->directory)).isSymLink())) {
		return std::nullopt;
	}
	for (auto field : { &result->directory, &result->settings, &result->good,
		&result->backup, &result->importBackup, &result->syncDirectory,
		&result->syncStore, &result->history, &result->fingerprintFile }) {
		*field = field->isEmpty() ? _root : absolute(*field);
	}
	return result;
}

AccountWorkMode DesktopAccountConfig::workMode(const AccountKey &key) const {
	const auto global = globalAccount();
	return global ? AccountWorkModeFor(_choice, key, *global) : AccountWorkMode::Own;
}

void DesktopAccountConfig::acknowledgeOwnerNotice() {
	_plan.restoredHandEditedOwnerNotice = false;
}

void DesktopAccountConfig::refreshSettings() {
	refreshLocalChoice();
	invalidate();
}

const Settings &DesktopAccountConfig::globalSettings() {
	return settings(globalAccount().value_or(AccountKey{}));
}

const Settings &DesktopAccountConfig::settings(const AccountKey &key) {
	const auto name = AccountKeyText(key.environment, key.userId);
	const auto cacheKey = name.value_or(u"single-file"_q);
	auto &slot = _cache[cacheKey];
	if (!slot) {
		slot = std::make_unique<CachedSettings>();
	}
	if (slot->generation == _generation) {
		return slot->settings;
	}
	slot->generation = _generation;
	const auto globalPath = globalSettingsPath();
	const auto own = files(key);
	const auto ownPath = own ? own->settings : globalPath;
	auto globalReport = SettingsLayerFileReport();
	auto accountReport = SettingsLayerFileReport();
	const auto load = [&](const QString &path, SettingsLayerFileReport &report) {
		const auto current = path.isEmpty() || QFileInfo(path).isSymLink()
			? SettingsFileInput{ SettingsFileState::ReadError } : ReadSettingsFileInput(path);
		report.current.path = path;
		report.current.present = current.state != SettingsFileState::Missing;
		report.current.parsed = ParseSettings(QString::fromUtf8(current.bytes), path);
		if (current.state != SettingsFileState::Readable) {
			report.current.parsed.error = current.state == SettingsFileState::Missing
				? u"Settings file is absent."_q : u"Settings file could not be read."_q;
		}
		const auto goodPath = path + u".good"_q;
		const auto good = path.isEmpty() || QFileInfo(goodPath).isSymLink()
			? SettingsFileInput{ SettingsFileState::ReadError } : ReadSettingsFileInput(goodPath);
		if (report.current.parsed.ok()) {
			report.usable = true;
			if (good.state != SettingsFileState::ReadError && good.bytes != current.bytes) {
				const auto saved = WriteGuardedSettingsFile(goodPath, good, current.bytes, [&] {
					return ReadSettingsFileInput(path) == current;
				}, true);
				if (saved != GuardedFileWrite::Written) {
					report.current.parsed.warnings.push_back(u"Could not update the last-good settings copy."_q);
				}
			}
			return std::make_optional(SettingsLayerText{ QString::fromUtf8(current.bytes), path });
		}
		if (good.state == SettingsFileState::Readable) {
			report.good = SettingsLayerFileParse{ goodPath,
				ParseSettings(QString::fromUtf8(good.bytes), goodPath), true };
			if (report.good->parsed.ok()) {
				report.usable = true;
				report.usedGood = true;
				return std::make_optional(SettingsLayerText{ QString::fromUtf8(good.bytes), goodPath });
			}
		}
		return std::optional<SettingsLayerText>();
	};
	const auto global = load(globalPath, globalReport);
	const auto account = own && ownPath != globalPath
		? load(ownPath, accountReport) : std::optional<SettingsLayerText>();
	auto options = SettingsLayerOptions();
	options.globalPresent = global.has_value();
	options.accountPresent = account.has_value();
	options.followListMembers = workMode(key) == AccountWorkMode::Follow;
	if (!_owner || (own && ownPath == globalPath)) {
		slot->report = {};
		slot->report.effective = ParseSettings(global ? global->text : QString(),
			global ? global->path : globalPath);
	} else {
		slot->report = LayerSettings(global ? global->text : QString(),
		global ? global->path : globalPath,
		account ? account->text : QString(),
		account ? account->path : ownPath, options);
	}
	slot->report.global = std::move(globalReport);
	slot->report.account = std::move(accountReport);
	if (slot->report.effective.ok()) {
		slot->settings = slot->report.effective.settings;
	}
	return slot->settings;
}

const LayerSettingsResult *DesktopAccountConfig::report(const AccountKey &key) const {
	const auto name = AccountKeyText(key.environment, key.userId);
	const auto i = name ? _cache.find(*name) : _cache.end();
	return i == _cache.end() ? nullptr : &i->second->report;
}

std::optional<AccountFileTarget> DesktopAccountConfig::target(const AccountKey &key) {
	refreshLocalChoice();
	const auto selected = files(key);
	if (!permissionsReady() || !selected || !AccountFilePathSafe(selected->settings)) {
		return std::nullopt;
	}
	return AccountFileTarget{ key, selected->settings,
		ReadSettingsFileInput(selected->settings), _routingGeneration,
		DirectoryIdentity(_root) + '/' + DirectoryIdentity(QFileInfo(selected->directory).absolutePath())
			+ '/' + DirectoryIdentity(selected->directory) };
}

bool DesktopAccountConfig::validate(const AccountFileTarget &target) {
	const auto current = this->target(target.account);
	return current && current->generation == target.generation
		&& current->path == target.path && current->input == target.input
		&& current->directoryIdentity == target.directoryIdentity;
}

std::optional<AccountSettingsOpenTarget> DesktopAccountConfig::openTarget(
		const AccountKey &requester,
		SettingsOpenRole role) {
	refreshLocalChoice();
	const auto global = globalAccount();
	if (!permissionsReady() || !_owner || !global
		|| (role != SettingsOpenRole::Own && role != SettingsOpenRole::AppWide)
		|| !AccountKeyText(requester.environment, requester.userId)
		|| OwnerInput(absolute(u"accounts/root.toml"_q)).state != SettingsFileState::Readable) {
		return std::nullopt;
	}
	const auto local = OwnerInput(absolute(u"local.toml"_q));
	const auto record = OwnerInput(absolute(u"accounts/root.toml"_q));
	const auto parsedLocal = ParseLocalChoice(QString::fromUtf8(local.bytes), absolute(u"local.toml"_q));
	const auto parsedRecord = ParseOwnerRecord(QString::fromUtf8(record.bytes), absolute(u"accounts/root.toml"_q));
	if (local.state != SettingsFileState::Readable || local != _localInput || !parsedLocal.ok()
		|| !parsedRecord.ok() || parsedLocal.choice.rootFileAccount != _owner
		|| parsedRecord.account != _owner) {
		return std::nullopt;
	}
	const auto selected = target(role == SettingsOpenRole::AppWide ? *global : requester);
	const auto rootIdentity = DirectoryIdentity(_root);
	const auto selectedFiles = selected ? files(selected->account) : std::nullopt;
	const auto accountsIdentity = DirectoryIdentity(absolute(u"accounts"_q));
	return selected && selectedFiles && !rootIdentity.isEmpty() && !accountsIdentity.isEmpty()
		&& globalAccount() == global && OwnerInput(absolute(u"local.toml"_q)) == local
		&& OwnerInput(absolute(u"accounts/root.toml"_q)) == record
		? std::make_optional(AccountSettingsOpenTarget{
			requester, role, *selected, *_owner, *global, _root, rootIdentity,
			accountsIdentity, DirectoryIdentity(selectedFiles->directory), local, record })
		: std::nullopt;
}

bool DesktopAccountConfig::validateOpen(const AccountSettingsOpenTarget &target) {
	const auto current = openTarget(target.requester, target.role);
	return current && current->owner == target.owner && current->global == target.global
		&& current->root == target.root && current->rootIdentity == target.rootIdentity
		&& current->accountsIdentity == target.accountsIdentity
		&& current->accountDirectoryIdentity == target.accountDirectoryIdentity
		&& current->localInput == target.localInput && current->ownerInput == target.ownerInput
		&& current->file.account == target.file.account
		&& validate(target.file);
}

SettingsOpenResult DesktopAccountConfig::prepareOpen(
		const AccountSettingsOpenTarget &target,
		const std::function<bool()> &originCurrent) {
	return prepareInput(target, NewAccountSettingsText(), originCurrent);
}

SettingsOpenResult DesktopAccountConfig::prepareInput(
		const AccountSettingsOpenTarget &target,
		const QByteArray &bytes,
		const std::function<bool()> &originCurrent,
		bool *published) {
	if (!originCurrent || !originCurrent() || !validateOpen(target)) {
		return {};
	}
	if (target.file.input.state == SettingsFileState::ReadError) {
		return { SettingsOpenStatus::ReadError };
	} else if (target.file.input.state == SettingsFileState::Readable) {
		return { SettingsOpenStatus::Ready, target };
	}
	const auto key = AccountKeyText(target.file.account.environment, target.file.account.userId);
	if (!key || target.file.account == target.owner
		|| target.file.path != absolute(u"accounts/%1/settings.toml"_q.arg(*key))) {
		return { SettingsOpenStatus::CreateError };
	}
	const auto logicalCurrent = [&] {
		if (!originCurrent()) { return false; }
		const auto current = openTarget(target.requester, target.role);
		return current && current->owner == target.owner && current->global == target.global
			&& current->root == target.root && current->rootIdentity == target.rootIdentity
			&& current->accountsIdentity == target.accountsIdentity
			&& current->localInput == target.localInput && current->ownerInput == target.ownerInput
			&& current->file.account == target.file.account
			&& current->file.path == target.file.path
			&& current->file.generation == target.file.generation
			&& current->file.input == target.file.input;
	};
	if (!CreateAccountSettingsFile(_root, *key, bytes,
			target.accountsIdentity, target.accountDirectoryIdentity, logicalCurrent)) {
		return { logicalCurrent() ? SettingsOpenStatus::CreateError : SettingsOpenStatus::Changed };
	}
	if (published) { *published = true; }
	forgetAccountSave(target.file.account);
	(void)observeAccountSave(target.file.account);
	invalidate();
	watch();
	if (_changed) { _changed(); }
	if (!originCurrent()) { return {}; }
	const auto current = openTarget(target.requester, target.role);
	if (!current || current->owner != target.owner || current->global != target.global
		|| current->root != target.root || current->rootIdentity != target.rootIdentity
		|| current->accountsIdentity != target.accountsIdentity
		|| current->localInput != target.localInput || current->ownerInput != target.ownerInput
		|| current->file.account != target.file.account || current->file.path != target.file.path
		|| current->file.generation != target.file.generation
		|| current->file.input != SettingsFileInput{ SettingsFileState::Readable, bytes }
		|| !validateOpen(*current)) {
		return {};
	}
	return { SettingsOpenStatus::Created, current };
}

AccountSettingsRowState DesktopAccountConfig::rowState(
		const AccountKey &key, AccountSettingsRow row) {
	auto result = AccountSettingsRowState();
	result.target = rowTarget(key, row);
	const auto read = [&](const Settings &effective) {
		switch (row) {
		case AccountSettingsRow::LastSeenReasons: result.value = effective.lastSeen.reasons; break;
		case AccountSettingsRow::LastSeenPeek: result.value = effective.lastSeen.trade; break;
		case AccountSettingsRow::SendAfterSave: result.value = effective.sync.sendAfterSave; break;
		}
	};
	const auto pinned = [&](const std::vector<SettingsLayerKey> &applied) {
		return row != AccountSettingsRow::SendAfterSave && globalAccount() != key
			&& std::any_of(applied.begin(), applied.end(), [&](const SettingsLayerKey &entry) {
				return entry.table == std::vector<QString>{ RowTable(row) } && entry.key == RowKey(row);
			});
	};
	if (result.target) {
		const auto &target = *result.target;
		const auto &own = target.destination.file;
		if (key == target.destination.global) {
			read(ParseSettings(QString::fromUtf8(own.input.bytes), own.path).settings);
		} else {
			auto global = SettingsLayerText{ QString(), target.globalPath };
			auto usable = false;
			if (!target.globalFallback) {
				global.text = QString::fromUtf8(target.globalInput.bytes);
				usable = true;
			} else if (target.globalGoodInput.state == SettingsFileState::Readable) {
				const auto text = QString::fromUtf8(target.globalGoodInput.bytes);
				const auto path = target.globalPath + u".good"_q;
				if (ParseSettings(text, path).ok()) {
					global = { text, path };
					usable = true;
				}
			}
			auto options = SettingsLayerOptions();
			options.globalPresent = usable;
			options.accountPresent = own.input.state == SettingsFileState::Readable;
			options.followListMembers = workMode(key) == AccountWorkMode::Follow;
			const auto layered = LayerSettings(global.text,
				global.path, QString::fromUtf8(own.input.bytes), own.path, options);
			read(layered.effective.settings);
			result.pinned = pinned(layered.applied);
		}
	} else {
		const auto own = files(key);
		const auto globalPath = globalSettingsPath();
		if ((own && (!RowDirectorySafe(own->directory) || !AccountFilePathSafe(own->settings)))
			|| !RowDirectorySafe(QFileInfo(globalPath).absolutePath())
			|| !AccountFilePathSafe(globalPath)) {
			return result;
		}
		refreshSettings();
		read(settings(key));
		const auto source = report(key);
		result.pinned = source && pinned(source->applied);
	}
	return result;
}

std::optional<AccountSettingsRowTarget> DesktopAccountConfig::rowTarget(
		const AccountKey &key, AccountSettingsRow row) {
	const auto table = RowTable(row);
	const auto name = RowKey(row);
	const auto scope = SettingsScope(table, name);
	if (name.isEmpty() || (scope != SettingScope::PerAccount
		&& !(row == AccountSettingsRow::SendAfterSave && scope == SettingScope::FileLocal))) {
		return std::nullopt;
	}
	refreshLocalChoice();
	const auto paths = files(key);
	const auto globalPath = globalSettingsPath();
	if (!paths || !RowDirectorySafe(paths->directory)
		|| !RowDirectorySafe(QFileInfo(globalPath).absolutePath())
		|| !AccountFilePathSafe(paths->settings) || !AccountFilePathSafe(globalPath)) {
		return std::nullopt;
	}
	const auto destination = openTarget(key, SettingsOpenRole::Own);
	const auto global = OwnerInput(globalPath);
	if (!destination || !RowInputUsable(destination->file.input, destination->file.path)
		|| (destination->file.input.state == SettingsFileState::Missing
			&& (destination->file.account == destination->owner
				|| OwnerInput(destination->file.path + u".good"_q).state != SettingsFileState::Missing))
		|| !AccountFilePathSafe(globalPath)) { return std::nullopt; }
	const auto fallback = global.state != SettingsFileState::Readable
		|| !ParseSettings(QString::fromUtf8(global.bytes), globalPath).ok();
	return AccountSettingsRowTarget{ *destination, row, globalPath, global,
		DirectoryIdentity(QFileInfo(globalPath).absolutePath()), fallback,
		fallback ? OwnerInput(globalPath + u".good"_q) : SettingsFileInput() };
}

bool DesktopAccountConfig::validateRow(const AccountSettingsRowTarget &target) {
	const auto current = rowTarget(target.destination.requester, target.row);
	return current && target.destination.role == SettingsOpenRole::Own
		&& target.destination.file.account == target.destination.requester
		&& current->globalPath == target.globalPath
		&& current->globalInput == target.globalInput
		&& current->globalDirectoryIdentity == target.globalDirectoryIdentity
		&& current->globalFallback == target.globalFallback
		&& current->globalGoodInput == target.globalGoodInput
		&& validateOpen(target.destination);
}

AccountSettingsRowWriteResult DesktopAccountConfig::writeRow(
		const AccountSettingsRowTarget &target,
		std::optional<bool> value,
		const std::function<bool()> &originCurrent) {
	const auto current = [&] { return originCurrent && originCurrent() && validateRow(target); };
	if (!current()) { return { .error = u"Settings changed. Try again."_q }; }
	const auto &destination = target.destination;
	const auto &file = destination.file;
	const auto reset = !value.has_value();
	if (reset && (target.row == AccountSettingsRow::SendAfterSave
		|| file.account == destination.global)) {
		return { .error = u"This row cannot inherit from another file."_q };
	}
	if (reset && (file.input.state == SettingsFileState::Missing
		|| !HasOwnRowKey(file.input, target.row))) {
		return { .ok = true, .target = target };
	}
	const auto text = file.input.state == SettingsFileState::Missing
		? QString::fromUtf8(NewAccountSettingsText()) : QString::fromUtf8(file.input.bytes);
	const auto splice = reset
		? RemoveTableKey(text, file.path, RowTable(target.row), RowKey(target.row))
		: SetTableBool(text, file.path, RowTable(target.row), RowKey(target.row), *value);
	if (!splice.ok()) { return { .error = splice.error }; }
	if (!splice.changed) { return { .ok = true, .target = target }; }
	const auto bytes = splice.text.toUtf8();
	const auto paths = files(file.account);
	if (!paths || !current() || !AccountFilePathSafe(paths->backup)
		|| !AccountFilePathSafe(paths->good)) {
		return { .error = u"The settings destination is unavailable."_q };
	}
	auto expected = target;
	const auto created = file.input.state == SettingsFileState::Missing;
	if (created) {
		if (target.globalPath == file.path
			&& target.globalGoodInput.state != SettingsFileState::Missing) {
			return { .error = u"Repair the app-wide settings file before changing this row."_q };
		}
		const auto creationOrigin = [&] {
			return originCurrent && originCurrent()
				&& (!target.globalFallback
					|| OwnerInput(target.globalPath + u".good"_q) == target.globalGoodInput)
				&& (target.globalPath == file.path
					|| (OwnerInput(target.globalPath) == target.globalInput
						&& DirectoryIdentity(QFileInfo(target.globalPath).absolutePath())
							== target.globalDirectoryIdentity));
		};
		auto published = false;
		const auto ready = prepareInput(destination, bytes, creationOrigin, &published);
		if (!ready.target || ready.status != SettingsOpenStatus::Created) {
			return { .changed = published, .created = published,
				.error = u"The account settings file could not be created safely."_q };
		}
		expected.destination = *ready.target;
	} else {
		const auto backup = OwnerInput(paths->backup);
		const auto good = OwnerInput(paths->good);
		if (backup.state == SettingsFileState::ReadError || good.state == SettingsFileState::ReadError
			|| WriteGuardedSettingsFile(paths->backup, backup, file.input.bytes, [&] {
				return current() && AccountFilePathSafe(paths->backup);
			}, true) != GuardedFileWrite::Written) {
			return { .error = u"The account settings backup could not be written."_q };
		}
		if (WriteGuardedSettingsFile(file.path, file.input, bytes, [&] {
			return current() && AccountFilePathSafe(paths->backup) && AccountFilePathSafe(paths->good);
		}) != GuardedFileWrite::Written) {
			return { .error = u"Settings changed or could not be written."_q };
		}
		expected.destination.file.input = { SettingsFileState::Readable, bytes };
		invalidate();
		watch();
	}
	if (expected.globalPath == file.path) {
		expected.globalInput = { SettingsFileState::Readable, bytes };
		expected.globalFallback = false;
		expected.globalGoodInput = {};
		expected.globalDirectoryIdentity = DirectoryIdentity(QFileInfo(file.path).absolutePath());
	}
	const auto committedCurrent = [&] {
		return originCurrent && originCurrent() && validateRow(expected);
	};
	if (!committedCurrent()) {
		return { .changed = true, .created = created, .error = u"Settings context changed after writing."_q };
	}
	const auto good = OwnerInput(paths->good);
	if (!AccountFilePathSafe(paths->good)
		|| WriteGuardedSettingsFile(paths->good, good, bytes, committedCurrent, true)
			!= GuardedFileWrite::Written) {
		return { .changed = true, .created = created, .error = u"Settings saved, but the last-good copy could not be updated."_q };
	}
	if (_changed) { _changed(); }
	if (!committedCurrent()) {
		return { .changed = true, .created = created, .error = u"Settings context changed after writing."_q };
	}
	return { .ok = true, .changed = true, .created = created, .target = expected };
}

bool ShouldShowGlobalAccountChoice(int signedIn, bool globalSignedIn) {
	return signedIn >= 2 || !globalSignedIn;
}

std::optional<GlobalAccountChoicePreview> DesktopAccountConfig::previewGlobalChoice(
		const AccountKey &requester,
		const AccountKey &selected,
		const QString &runningPreset) {
	refreshLocalChoice();
	const auto eligible = [&](const AccountKey &key) {
		return std::any_of(_accounts.begin(), _accounts.end(), [&](const StartupAccountCandidate &entry) {
			return entry.account == key;
		});
	};
	if (!eligible(requester) || (!eligible(selected) && globalAccount() != selected)) { return std::nullopt; }
	const auto paths = files(selected);
	const auto globalPath = globalSettingsPath();
	if (!paths || !RowDirectorySafe(paths->directory)
		|| !RowDirectorySafe(QFileInfo(globalPath).absolutePath())) { return std::nullopt; }
	const auto current = openTarget(requester, SettingsOpenRole::AppWide);
	const auto destination = openTarget(selected, SettingsOpenRole::Own);
	if (!current || !destination) { return std::nullopt; }
	auto result = GlobalAccountChoicePreview();
	result.current = *current;
	result.selected = *destination;
	result.currentGood = OwnerInput(current->file.path + u".good"_q);
	result.selectedGood = OwnerInput(destination->file.path + u".good"_q);
	result.currentGoodRelevant = current->file.input.state != SettingsFileState::Readable
		|| !RowInputUsable(current->file.input, current->file.path);
	result.selectedGoodRelevant = destination->file.input.state != SettingsFileState::Readable
		|| !RowInputUsable(destination->file.input, destination->file.path);
	const auto load = [&](const AccountFileTarget &file, const SettingsFileInput &good,
			Settings &settings, bool *usesGood = nullptr) -> std::optional<QByteArray> {
		const auto usable = [&](const SettingsFileInput &input, const QString &path) {
			return input.state == SettingsFileState::Readable && RowInputUsable(input, path);
		};
		if (usable(file.input, file.path)) {
			settings = ParseSettings(QString::fromUtf8(file.input.bytes), file.path).settings;
			return file.input.bytes;
		} else if (usable(good, file.path + u".good"_q)) {
			settings = ParseSettings(QString::fromUtf8(good.bytes), file.path + u".good"_q).settings;
			if (usesGood) { *usesGood = true; }
			return good.bytes;
		} else if (file.input.state == SettingsFileState::Missing && good.state == SettingsFileState::Missing) {
			settings = ParseSettings(QString(), file.path).settings;
			return QByteArray();
		}
		return std::nullopt;
	};
	const auto previousText = load(current->file, result.currentGood, result.before);
	const auto selectedText = load(destination->file, result.selectedGood, result.after, &result.selectedUsesGood);
	if (!previousText || !selectedText) { return std::nullopt; }
	auto identityPaths = std::vector<QString>{ absolute(u"local.toml"_q),
		absolute(u"accounts/root.toml"_q), current->file.path, destination->file.path };
	if (result.currentGoodRelevant) { identityPaths.push_back(current->file.path + u".good"_q); }
	if (result.selectedGoodRelevant) { identityPaths.push_back(destination->file.path + u".good"_q); }
	for (const auto &path : identityPaths) {
		const auto identity = ChoiceFileIdentity(path);
		if (identity.isEmpty()) { return std::nullopt; }
		result.fileIdentities[path] = identity;
	}
	result.runningPreset = runningPreset.isEmpty() ? NormalPreset() : runningPreset;
	result.enterNormal = !Resolve(result.after, result.runningPreset).has_value();
	const auto summary = SummarizeConfigChange(QByteArray("version=2\n"), *selectedText);
	if (!summary.parsed) { return std::nullopt; }
	result.noAppWideSettings = true;
	for (const auto &entry : summary.entries) {
		for (const auto &key : entry.keys) {
			if (key.scope == SettingScope::AppWide) { result.noAppWideSettings = false; }
		}
	}
	return result;
}

bool DesktopAccountConfig::validateGlobalChoice(const GlobalAccountChoicePreview &preview) {
	const auto current = previewGlobalChoice(preview.current.requester,
		preview.selected.file.account, preview.runningPreset);
	return current && preview.current.role == SettingsOpenRole::AppWide
		&& preview.selected.role == SettingsOpenRole::Own
		&& preview.selected.requester == preview.selected.file.account
		&& validateOpen(preview.current) && validateOpen(preview.selected)
		&& current->enterNormal == preview.enterNormal
		&& current->noAppWideSettings == preview.noAppWideSettings
		&& current->selectedUsesGood == preview.selectedUsesGood
		&& current->currentGoodRelevant == preview.currentGoodRelevant
		&& current->selectedGoodRelevant == preview.selectedGoodRelevant
		&& current->fileIdentities == preview.fileIdentities
		&& (!preview.currentGoodRelevant || current->currentGood == preview.currentGood)
		&& (!preview.selectedGoodRelevant || current->selectedGood == preview.selectedGood);
}

bool DesktopAccountConfig::validateGlobalChoiceCommit(
		const GlobalAccountChoicePreview &preview,
		const GlobalAccountChoiceResult &commit) const {
	if (!commit.changed || !_owner || *_owner != preview.current.owner
		|| _routingGeneration != commit.generation || _persistFailed
		|| globalAccount() != (commit.activated ? preview.selected.file.account : preview.current.global)
		|| DirectoryIdentity(_root) != preview.current.rootIdentity
		|| DirectoryIdentity(absolute(u"accounts"_q)) != preview.current.accountsIdentity
		|| DirectoryIdentity(QFileInfo(preview.current.file.path).absolutePath()) != preview.current.accountDirectoryIdentity
		|| DirectoryIdentity(QFileInfo(preview.selected.file.path).absolutePath()) != preview.selected.accountDirectoryIdentity
		|| OwnerInput(absolute(u"local.toml"_q)) != commit.local
		|| ChoiceFileIdentity(absolute(u"local.toml"_q)) != commit.localIdentity
		|| OwnerInput(absolute(u"accounts/root.toml"_q)) != preview.current.ownerInput
		|| OwnerInput(preview.current.file.path) != preview.current.file.input
		|| OwnerInput(preview.selected.file.path) != preview.selected.file.input
		|| (preview.currentGoodRelevant && OwnerInput(preview.current.file.path + u".good"_q) != preview.currentGood)
		|| (preview.selectedGoodRelevant && OwnerInput(preview.selected.file.path + u".good"_q) != preview.selectedGood)) { return false; }
	for (const auto &[path, identity] : preview.fileIdentities) {
		if (path != absolute(u"local.toml"_q) && ChoiceFileIdentity(path) != identity) { return false; }
	}
	return true;
}

GlobalAccountChoiceResult DesktopAccountConfig::commitGlobalChoice(
		const GlobalAccountChoicePreview &preview,
		const std::function<bool()> &originCurrent) {
	const auto current = [&] { return originCurrent && originCurrent() && validateGlobalChoice(preview); };
	if (!current()) { return { .error = u"The app-wide settings choice changed. Preview it again."_q }; }
	const auto path = absolute(u"local.toml"_q);
	const auto text = QString::fromUtf8(preview.current.localInput.bytes);
	if (text.toUtf8() != preview.current.localInput.bytes) { return { .error = u"Local choices are not valid UTF-8."_q }; }
	const auto next = ChoiceText(text, path, preview.selected.file.account);
	if (!next) { return { .error = u"The local global-account key cannot be changed safely."_q }; }
	const auto parsed = ParseLocalChoice(*next, path);
	const auto previous = ParseLocalChoice(text, path);
	if (!parsed.ok() || parsed.choice.rootFileAccount != _owner
		|| parsed.choice.globalAccount != preview.selected.file.account
		|| parsed.choice.accountWorkMode != previous.choice.accountWorkMode) {
		return { .error = u"Local choices could not be preserved."_q };
	}
	const auto bytes = next->toUtf8();
	if (bytes == preview.current.localInput.bytes) { return { .ok = true }; }
	if (WriteGuardedSettingsFile(path, preview.current.localInput, bytes, current)
		!= GuardedFileWrite::Written) { return { .error = u"Local choices changed or could not be saved."_q }; }
	auto result = GlobalAccountChoiceResult{ .changed = true,
		.local = { SettingsFileState::Readable, bytes }, .localIdentity = ChoiceFileIdentity(path),
		.generation = _routingGeneration };
	result.ok = originCurrent() && validateGlobalChoiceCommit(preview, result);
	if (!result.ok) { result.error = u"The choice was saved, but its context changed."_q; }
	return result;
}

bool DesktopAccountConfig::activateGlobalChoice(
		const GlobalAccountChoicePreview &preview,
		GlobalAccountChoiceResult &commit) {
	if (!commit.ok || !validateGlobalChoiceCommit(preview, commit)) { return false; }
	refreshLocalChoice();
	commit.activated = true;
	commit.generation = _routingGeneration;
	return validateGlobalChoiceCommit(preview, commit);
}

std::optional<ManualSettingsExportTarget> DesktopAccountConfig::manualExportTarget(
		const AccountKey &key) {
	refreshLocalChoice();
	if (!AccountKeyText(key.environment, key.userId) || _persistFailed
		|| !QDir::isAbsolutePath(_root)) {
		return std::nullopt;
	}
	if (_owner) {
		const auto selected = target(key);
		return selected ? std::make_optional(ManualSettingsExportTarget{
			ManualSettingsExportRole::AccountFile, *selected }) : std::nullopt;
	}
	const auto path = absolute(u"settings.toml"_q);
	return ManualSettingsExportTarget{ ManualSettingsExportRole::SingleFile,
		{ key, path, ReadSettingsFileInput(path), _routingGeneration, DirectoryIdentity(_root) } };
}

bool DesktopAccountConfig::validateManualExport(
		const ManualSettingsExportTarget &target) {
	const auto current = manualExportTarget(target.file.account);
	return current && current->role == target.role
		&& current->file.generation == target.file.generation
		&& current->file.path == target.file.path
		&& current->file.input == target.file.input
		&& current->file.directoryIdentity == target.file.directoryIdentity;
}

std::optional<AccountFileTarget> DesktopAccountConfig::editTarget(
		const AccountKey &key) {
	const auto selected = target(key);
	return selected && accountTrafficReady(key) && globalAccount() == _owner
		? selected : std::nullopt;
}

std::optional<AccountOperationTarget> DesktopAccountConfig::operationTarget(
		const AccountKey &key) {
	const auto selected = target(key);
	const auto paths = files(key);
	if (!selected || !paths || DirectoryIdentity(_root).isEmpty()
		|| DirectoryIdentity(paths->directory).isEmpty()
		|| selected->input.state != SettingsFileState::Readable) {
		return std::nullopt;
	}
	for (const auto &path : { paths->settings, paths->good, paths->backup,
		paths->importBackup, paths->fingerprintFile, paths->syncStore }) {
		if (!AccountFilePathSafe(path)) {
			return std::nullopt;
		}
	}
	for (const auto &path : { paths->syncDirectory, paths->history }) {
		const auto info = QFileInfo(path);
		if (info.isSymLink() || (info.exists() && !info.isDir())) {
			return std::nullopt;
		}
	}
	const auto parsed = ParseSettings(QString::fromUtf8(selected->input.bytes), selected->path);
	if (!parsed.ok() || parsed.settings.version > kSettingsVersion) {
		return std::nullopt;
	}
	return AccountOperationTarget{ *selected, *paths, DirectoryIdentity(paths->syncDirectory),
		DirectoryIdentity(paths->history) };
}

bool DesktopAccountConfig::validateOperation(
		const AccountOperationTarget &target, bool sameInput) {
	const auto current = operationTarget(target.file.account);
	return current && current->file.generation == target.file.generation
		&& current->file.path == target.file.path
		&& current->file.directoryIdentity == target.file.directoryIdentity
		&& current->files.directory == target.files.directory
		&& current->files.settings == target.files.settings
		&& current->files.good == target.files.good
		&& current->files.backup == target.files.backup
		&& current->files.importBackup == target.files.importBackup
		&& current->files.syncStore == target.files.syncStore
		&& current->files.fingerprintStorage == target.files.fingerprintStorage
		&& current->files.syncDirectory == target.files.syncDirectory
		&& current->files.history == target.files.history
		&& current->files.fingerprintFile == target.files.fingerprintFile
		&& (target.syncDirectoryIdentity.isEmpty() || current->syncDirectoryIdentity == target.syncDirectoryIdentity)
		&& (target.historyDirectoryIdentity.isEmpty() || current->historyDirectoryIdentity == target.historyDirectoryIdentity)
		&& (!sameInput || current->file.input == target.file.input);
}

bool DesktopAccountConfig::validateStore(const AccountOperationTarget &target,
		const SettingsFileInput &expected, bool sameInput) {
	return validateOperation(target, sameInput)
		&& expected.state == SettingsFileState::Readable
		&& ReadSettingsFileInput(target.files.syncStore) == expected;
}

std::optional<AccountSendFingerprints> DesktopAccountConfig::fingerprints(
		const AccountOperationTarget &target) {
	if (!validateOperation(target)) {
		return std::nullopt;
	}
	const auto input = ReadSettingsFileInput(target.files.fingerprintFile);
	if (input.state == SettingsFileState::ReadError) {
		return std::nullopt;
	}
	const auto parsed = toml::parse(input.bytes.toStdString());
	if (!parsed) {
		return std::nullopt;
	}
	if (target.files.fingerprintStorage == AccountFingerprintStorage::AccountSendState) {
		for (const auto &[key, node] : parsed.table()) {
			if ((key.str() != "last_sent_fingerprint" && key.str() != "last_imported_fingerprint")
				|| !node.is_string()) {
				return std::nullopt;
			}
		}
	}
	const auto read = [&](const char *key) {
		const auto value = parsed.table()[key].value<std::string>();
		return value ? QString::fromStdString(*value) : QString();
	};
	const auto sent = read("last_sent_fingerprint");
	const auto imported = read("last_imported_fingerprint");
	static const auto syntax = QRegularExpression(u"^(?:[0-9]+:[0-9a-f]{64})?$"_q);
	if (!syntax.match(sent).hasMatch() || !syntax.match(imported).hasMatch()) {
		return std::nullopt;
	}
	return AccountSendFingerprints{ sent, imported, input };
}

void DesktopAccountConfig::suppressImportedEcho(const AccountOperationTarget &target) {
	if (validateOperation(target)) {
		_importedEcho[target.file.path] = SettingsFingerprint(target.file.input.bytes);
	}
}

bool DesktopAccountConfig::noteFingerprint(
		const AccountOperationTarget &target, bool imported) {
	if (imported) { suppressImportedEcho(target); }
	const auto current = fingerprints(target);
	if (!current || target.files.fingerprintStorage != AccountFingerprintStorage::AccountSendState) {
		return false;
	}
	const auto input = current->input;
	const auto hash = SettingsFingerprint(target.file.input.bytes);
	const auto sent = imported ? current->sent : hash;
	const auto received = imported ? hash : current->imported;
	const auto bytes = u"last_sent_fingerprint = \"%1\"\nlast_imported_fingerprint = \"%2\"\n"_q
		.arg(sent, received).toUtf8();
	return WriteGuardedSettingsFile(target.files.fingerprintFile, input, bytes,
		[&] { return validateOperation(target); }, true) == GuardedFileWrite::Written;
}

std::optional<AccountOperationTarget> DesktopAccountConfig::observeAccountSave(
		const AccountKey &key) {
	const auto name = AccountKeyText(key.environment, key.userId);
	const auto file = target(key);
	if (!name || !file) {
		if (name) { _saveObservations.erase(*name); }
		return std::nullopt;
	}
	const auto previous = _saveObservations.find(*name);
	const auto changed = previous != _saveObservations.end()
		&& previous->second.state == SettingsFileState::Readable
		&& file->input.state == SettingsFileState::Readable
		&& previous->second != file->input;
	_saveObservations[*name] = file->input;
	const auto operation = changed ? operationTarget(key) : std::nullopt;
	return operation && shouldAutoSend(*operation) ? operation : std::nullopt;
}

void DesktopAccountConfig::forgetAccountSave(const AccountKey &key) {
	if (const auto name = AccountKeyText(key.environment, key.userId)) {
		_saveObservations.erase(*name);
	}
}

bool DesktopAccountConfig::shouldAutoSend(const AccountOperationTarget &target) {
	const auto imported = _importedEcho.find(target.file.path);
	if (imported != _importedEcho.end() && imported->second == SettingsFingerprint(target.file.input.bytes)) {
		return false;
	}
	const auto state = fingerprints(target);
	const auto parsed = ParseSettings(QString::fromUtf8(target.file.input.bytes), target.file.path);
	return state && parsed.ok() && ShouldAutoSend(parsed.settings, state->sent,
		state->imported, target.file.input.bytes, false);
}

GuardedFileWrite DesktopAccountConfig::replace(
		const AccountOperationTarget &target, const QByteArray &bytes) {
	const auto parsed = ParseSettings(QString::fromUtf8(bytes), target.file.path);
	if (!validateOperation(target) || !parsed.ok() || parsed.settings.version > kSettingsVersion
		|| !CanReplaceLegacyRootSettings(target.file.input.bytes, bytes)) {
		return GuardedFileWrite::InputChanged;
	}
	if (bytes != target.file.input.bytes) {
		const auto backup = ReadSettingsFileInput(target.files.backup);
		if (WriteGuardedSettingsFile(target.files.backup, backup, target.file.input.bytes,
			[&] { return validateOperation(target); }, true) != GuardedFileWrite::Written) {
			return GuardedFileWrite::WriteError;
		}
	}
	const auto result = WriteGuardedSettingsFile(target.file.path, target.file.input, bytes,
		[&] { return validateOperation(target); });
	if (result == GuardedFileWrite::Written) {
		invalidate();
	}
	return result;
}

bool DesktopAccountConfig::permissionsReady() const {
	return _owner.has_value() && !_persistFailed;
}

bool DesktopAccountConfig::accountTrafficReady(const AccountKey &key) const {
	return permissionsReady() && _owner == key;
}

uint64_t DesktopAccountConfig::generation() const {
	return _generation;
}

void DesktopAccountConfig::setChanged(std::function<void()> changed) {
	_changed = std::move(changed);
}

void DesktopAccountConfig::watch() {
	if (!QCoreApplication::instance() || !QDir::isAbsolutePath(_root)) {
		return;
	}
	if (!_watching) {
		_watching = true;
		const auto changed = [this](const QString &) {
			invalidate();
			refreshLocalChoice();
			watch();
			if (_changed) {
				_changed();
			}
		};
		QObject::connect(&_watcher, &QFileSystemWatcher::directoryChanged, &_watcher, changed);
		QObject::connect(&_watcher, &QFileSystemWatcher::fileChanged, &_watcher, changed);
	}
	auto wanted = QStringList{ _root, QFileInfo(_root).absolutePath(), absolute(u"accounts"_q) };
	for (const auto &account : _accounts) {
		const auto selected = files(account.account);
		if (selected) {
			wanted.push_back(selected->directory);
		}
	}
	wanted.push_back(QFileInfo(globalSettingsPath()).absolutePath());
	for (const auto &path : wanted) {
		if (QFileInfo(path).isDir() && !_watcher.directories().contains(path)) {
			_watcher.addPath(path);
		}
	}
	for (const auto &path : { absolute(u"local.toml"_q), globalSettingsPath() }) {
		if (QFileInfo(path).isFile() && !_watcher.files().contains(path)) {
			_watcher.addPath(path);
		}
	}
}

}
