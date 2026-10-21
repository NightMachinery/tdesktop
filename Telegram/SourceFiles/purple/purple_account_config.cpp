/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_account_config.h"
#include "purple/purple_config_diff.h"
#include "purple/purple_state.h"
#include "purple/purple_config_payload.h"
#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#else
#include <sys/stat.h>
#endif
#define TOML_EXCEPTIONS 0
#include <tomlplusplus/toml.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>

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
