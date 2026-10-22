/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_account_files.h"
#include "purple/purple_config_file.h"
#include "purple/purple_settings.h"
#include "purple/purple_splice.h"

#include <QtCore/QFileSystemWatcher>

#include <functional>
#include <map>
#include <memory>

namespace Purple {

[[nodiscard]] bool AccountSyncRecordSupported(const QByteArray &record);
[[nodiscard]] bool AccountFilePathSafe(const QString &path);
[[nodiscard]] bool CanReplaceLegacyRootSettings(const QByteArray &before, const QByteArray &after);

struct AccountFileTarget {
	AccountKey account;
	QString path;
	SettingsFileInput input;
	uint64_t generation = 0;
	QByteArray directoryIdentity;
};

enum class SettingsOpenRole { Own, AppWide };

struct AccountSettingsOpenTarget {
	AccountKey requester;
	SettingsOpenRole role = SettingsOpenRole::Own;
	AccountFileTarget file;
	AccountKey owner;
	AccountKey global;
	QString root;
	QByteArray rootIdentity;
	QByteArray accountsIdentity;
	QByteArray accountDirectoryIdentity;
	SettingsFileInput localInput;
	SettingsFileInput ownerInput;
};

enum class SettingsOpenStatus { Ready, Created, Changed, ReadError, CreateError };

struct SettingsOpenResult {
	SettingsOpenStatus status = SettingsOpenStatus::Changed;
	std::optional<AccountSettingsOpenTarget> target;
};

enum class LegacySettingsDocumentKind { Unrelated, Whole, Layered };

[[nodiscard]] QByteArray NewAccountSettingsText();
[[nodiscard]] QString SettingsDocumentFileName(int version);
[[nodiscard]] LegacySettingsDocumentKind ClassifySettingsDocumentName(
	const QString &name);
[[nodiscard]] bool CanApplyWholeSettingsDocument(
	const QString &name,
	const QByteArray &bytes);

enum class AccountSettingsRow { LastSeenReasons, LastSeenPeek, SendAfterSave };

struct AccountSettingsRowTarget {
	AccountSettingsOpenTarget destination;
	AccountSettingsRow row;
	QString globalPath;
	SettingsFileInput globalInput;
	QByteArray globalDirectoryIdentity;
	bool globalFallback = false;
	SettingsFileInput globalGoodInput;
};

struct AccountSettingsRowState {
	bool value = false;
	bool pinned = false;
	std::optional<AccountSettingsRowTarget> target;
};

struct AccountSettingsRowWriteResult {
	bool ok = false;
	bool changed = false;
	bool created = false;
	QString error;
	std::optional<AccountSettingsRowTarget> target;
};

struct AccountOperationTarget {
	AccountFileTarget file;
	AccountFiles files;
	QByteArray syncDirectoryIdentity;
	QByteArray historyDirectoryIdentity;
};

struct AccountSendFingerprints {
	QString sent;
	QString imported;
	SettingsFileInput input;
};

enum class ManualSettingsExportRole {
	SingleFile,
	AccountFile,
};

struct ManualSettingsExportTarget {
	ManualSettingsExportRole role;
	AccountFileTarget file;
};

[[nodiscard]] bool AccountFilteringEnabled(bool active, AccountWorkMode mode);

class DesktopAccountConfig final {
public:
	explicit DesktopAccountConfig(QString root);
	~DesktopAccountConfig();

	void start(std::vector<StartupAccountCandidate> accounts,
		std::optional<AccountKey> choice = std::nullopt);
	void refreshLocalChoice();
	void refreshSettings();
	void acknowledgeOwnerNotice();
	[[nodiscard]] const Settings &globalSettings();
	void setChanged(std::function<void()> changed);
	void watch();
	[[nodiscard]] const AccountOwnerStartupPlan &ownerPlan() const;
	[[nodiscard]] std::optional<AccountKey> owner() const;
	[[nodiscard]] std::optional<AccountKey> globalAccount() const;
	[[nodiscard]] QString globalSettingsPath() const;
	[[nodiscard]] std::optional<AccountFiles> files(const AccountKey &key) const;
	[[nodiscard]] const Settings &settings(const AccountKey &key);
	[[nodiscard]] const LayerSettingsResult *report(const AccountKey &key) const;
	[[nodiscard]] std::optional<AccountFileTarget> target(const AccountKey &key);
	[[nodiscard]] bool validate(const AccountFileTarget &target);
	[[nodiscard]] std::optional<AccountSettingsOpenTarget> openTarget(
		const AccountKey &requester,
		SettingsOpenRole role);
	[[nodiscard]] bool validateOpen(const AccountSettingsOpenTarget &target);
	[[nodiscard]] SettingsOpenResult prepareOpen(
		const AccountSettingsOpenTarget &target,
		const std::function<bool()> &originCurrent);
	[[nodiscard]] AccountSettingsRowState rowState(
		const AccountKey &key, AccountSettingsRow row);
	[[nodiscard]] std::optional<AccountSettingsRowTarget> rowTarget(
		const AccountKey &key, AccountSettingsRow row);
	[[nodiscard]] bool validateRow(const AccountSettingsRowTarget &target);
	[[nodiscard]] AccountSettingsRowWriteResult writeRow(
		const AccountSettingsRowTarget &target,
		std::optional<bool> value,
		const std::function<bool()> &originCurrent);
	[[nodiscard]] std::optional<ManualSettingsExportTarget> manualExportTarget(const AccountKey &key);
	[[nodiscard]] bool validateManualExport(const ManualSettingsExportTarget &target);
	[[nodiscard]] std::optional<AccountFileTarget> editTarget(const AccountKey &key);
	[[nodiscard]] std::optional<AccountOperationTarget> operationTarget(const AccountKey &key);
	[[nodiscard]] bool validateOperation(const AccountOperationTarget &target, bool sameInput = true);
	[[nodiscard]] bool validateStore(const AccountOperationTarget &target, const SettingsFileInput &expected, bool sameInput = true);
	[[nodiscard]] std::optional<AccountSendFingerprints> fingerprints(const AccountOperationTarget &target);
	void suppressImportedEcho(const AccountOperationTarget &target);
	[[nodiscard]] bool noteFingerprint(const AccountOperationTarget &target, bool imported);
	[[nodiscard]] std::optional<AccountOperationTarget> observeAccountSave(const AccountKey &key);
	void forgetAccountSave(const AccountKey &key);
	[[nodiscard]] bool shouldAutoSend(const AccountOperationTarget &target);
	[[nodiscard]] GuardedFileWrite replace(const AccountOperationTarget &target, const QByteArray &bytes);
	[[nodiscard]] bool permissionsReady() const;
	[[nodiscard]] bool accountTrafficReady(const AccountKey &key) const;
	[[nodiscard]] uint64_t generation() const;
	[[nodiscard]] AccountWorkMode workMode(const AccountKey &key) const;

private:
	struct CachedSettings {
		Settings settings;
		uint64_t generation = 0;
		LayerSettingsResult report;
	};
	void invalidate();
	[[nodiscard]] SettingsOpenResult prepareInput(
		const AccountSettingsOpenTarget &target,
		const QByteArray &bytes,
		const std::function<bool()> &originCurrent,
		bool *published = nullptr);
	[[nodiscard]] bool persistPlan(const AccountOwnerStartupPlan &plan,
		SettingsFileInput local, SettingsFileInput record, bool accountsPresent);
	[[nodiscard]] QString absolute(const QString &relative) const;

	QString _root;
	std::optional<AccountKey> _owner;
	AccountOwnerStartupPlan _plan;
	LocalChoice _choice;
	SettingsFileInput _localInput;
	std::vector<StartupAccountCandidate> _accounts;
	std::map<QString, std::unique_ptr<CachedSettings>> _cache;
	QFileSystemWatcher _watcher;
	std::function<void()> _changed;
	uint64_t _generation = 1;
	uint64_t _routingGeneration = 1;
	std::map<QString, QString> _importedEcho;
	std::map<QString, SettingsFileInput> _saveObservations;
	bool _persistFailed = false;
	bool _watching = false;

};

}
