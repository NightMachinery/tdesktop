/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_account_files.h"
#include "purple/purple_config_diff.h"
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
[[nodiscard]] bool SettingsExportPathAllowed(const QString &root, const QString &path);
[[nodiscard]] bool ConfigImpactFindingUsedHere(const ConfigImpactFinding &finding);
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

[[nodiscard]] bool CanApplyAccountSettingsDocument(
	const QString &name, const QByteArray &bytes);

struct AccountTransferReview {
	AccountSettingsOpenTarget destination;
	ConfigImpactInput context;
	ConfigChangeImpact impact;
	std::map<QString, QByteArray> directories;
	std::map<QString, SettingsFileInput> inputs;
	std::map<QString, QByteArray> identities;
	bool usingGood = false;
};

struct AccountTransferResult {
	bool ok = false;
	bool changed = false;
	QString error;
};

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

struct AccountSettingsWriteTarget {
	AccountSettingsOpenTarget destination;
	AccountSettingsOpenTarget globalSource;
	SettingsFileInput globalGoodInput;
	std::map<QString, QByteArray> identities;
};

struct AccountSettingsEditResult {
	bool ok = false;
	bool changed = false;
	bool created = false;
	QString error;
	std::optional<AccountSettingsWriteTarget> target;
};

struct AccountListEditTarget {
	AccountSettingsWriteTarget edit;
	QString list;
	std::vector<PeerIdValue> members;
	std::map<PeerIdValue, QString> comments;
};

struct OwnListMembers {
	std::vector<PeerIdValue> members;
	bool defined = false;
	bool deletable = false;
	bool hasMembers = false;
};

[[nodiscard]] std::map<QString, OwnListMembers> ReadOwnListMembers(
	const QByteArray &bytes,
	const QString &path);

struct AccountListDeleteTarget {
	AccountSettingsWriteTarget edit;
	QString list;
	int members = 0;
};

struct AccountWorkModeTarget {
	AccountSettingsOpenTarget own;
	AccountSettingsOpenTarget global;
	std::map<QString, QByteArray> identities;
};

struct AccountWorkModeResult {
	bool ok = false;
	bool changed = false;
	QString error;
	std::optional<AccountWorkModeTarget> target;
};

struct AccountListReloadChange {
	AccountKey account;
	int inactiveChanges = 0;
	std::optional<ConfigChangeImpact> impact;
	std::map<QString, int> unusedMembers;
	std::map<QString, int> emptiedLists;
};

[[nodiscard]] int InactiveFollowListMemberChanges(
	const QByteArray &before,
	const QByteArray &after,
	AccountWorkMode openingMode,
	bool appWideAccount);

[[nodiscard]] AccountListReloadChange DescribeAccountListReload(
	const QByteArray &beforeGlobal,
	const QByteArray &afterGlobal,
	const QByteArray &beforeOwn,
	const QByteArray &afterOwn);

[[nodiscard]] bool ShouldShowGlobalAccountChoice(int signedIn, bool globalSignedIn);

struct GlobalAccountChoicePreview {
	AccountSettingsOpenTarget current;
	AccountSettingsOpenTarget selected;
	SettingsFileInput currentGood;
	SettingsFileInput selectedGood;
	std::map<QString, QByteArray> fileIdentities;
	Settings before;
	Settings after;
	QString runningPreset;
	QString activationState;
	bool enterNormal = false;
	bool noAppWideSettings = false;
	bool selectedUsesGood = false;
	bool currentGoodRelevant = false;
	bool selectedGoodRelevant = false;
};

struct GlobalAccountChoiceResult {
	bool ok = false;
	bool changed = false;
	bool activated = false;
	SettingsFileInput local;
	QByteArray localIdentity;
	uint64_t generation = 0;
	QString error;
};

struct AccountOperationTarget {
	AccountFileTarget file;
	AccountFiles files;
	QByteArray syncDirectoryIdentity;
	QByteArray historyDirectoryIdentity;
	bool legacySend = false;
	bool inspection = false;
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
	[[nodiscard]] std::optional<AccountSettingsWriteTarget> settingsEditTarget(
		const AccountKey &requester,
		SettingsOpenRole role,
		QString *error = nullptr);
	[[nodiscard]] bool validateSettingsEdit(const AccountSettingsWriteTarget &target);
	[[nodiscard]] AccountSettingsEditResult writeSettingsEdit(
		const AccountSettingsWriteTarget &target,
		const std::function<SpliceResult(const QString&)> &splice,
		const std::function<bool()> &originCurrent);
	[[nodiscard]] std::optional<AccountListEditTarget> listEditTarget(
		const AccountKey &requester, const QString &list);
	[[nodiscard]] AccountSettingsEditResult writeListMember(
		const AccountListEditTarget &target,
		PeerIdValue id,
		bool member,
		const MemberTitle &title,
		const std::function<bool()> &originCurrent);
	[[nodiscard]] std::optional<AccountWorkModeTarget> workModeTarget(
		const AccountKey &key,
		QString *error = nullptr);
	[[nodiscard]] bool validateWorkModeTarget(
		const AccountWorkModeTarget &target);
	[[nodiscard]] AccountWorkModeResult writeWorkMode(
		const AccountWorkModeTarget &target,
		AccountWorkMode mode,
		const std::function<bool()> &originCurrent);
	[[nodiscard]] std::optional<AccountListDeleteTarget> listDeleteTarget(
		const AccountKey &key, const QString &list);
	[[nodiscard]] AccountSettingsEditResult deleteListMembers(
		const AccountListDeleteTarget &target,
		const std::function<bool()> &originCurrent);
	[[nodiscard]] std::vector<AccountListReloadChange> observeListReloads(
		const std::vector<AccountKey> &accounts);
	[[nodiscard]] std::optional<GlobalAccountChoicePreview> previewGlobalChoice(
		const AccountKey &requester,
		const AccountKey &selected,
		const QString &runningPreset);
	[[nodiscard]] bool validateGlobalChoice(const GlobalAccountChoicePreview &preview);
	[[nodiscard]] GlobalAccountChoiceResult commitGlobalChoice(
		const GlobalAccountChoicePreview &preview,
		const std::function<bool()> &originCurrent);
	[[nodiscard]] bool validateGlobalChoiceCommit(
		const GlobalAccountChoicePreview &preview,
		const GlobalAccountChoiceResult &commit) const;
	[[nodiscard]] bool activateGlobalChoice(
		const GlobalAccountChoicePreview &preview,
		GlobalAccountChoiceResult &commit);
	[[nodiscard]] std::optional<ManualSettingsExportTarget> manualExportTarget(const AccountKey &key);
	[[nodiscard]] bool validateManualExport(const ManualSettingsExportTarget &target);
	[[nodiscard]] std::optional<AccountFileTarget> editTarget(const AccountKey &key);
	[[nodiscard]] std::optional<AccountOperationTarget> operationTarget(const AccountKey &key, bool legacySend = false, bool inspection = false);
	[[nodiscard]] std::optional<AccountTransferReview> reviewTransfer(
		const AccountKey &key, const QByteArray &bytes);
	[[nodiscard]] bool validateTransfer(const AccountTransferReview &review);
	[[nodiscard]] std::optional<AccountTransferReview> rebaseTransferAfterSetup(const AccountTransferReview &review);
	[[nodiscard]] AccountTransferResult replaceReviewed(
		const AccountTransferReview &review, const std::function<bool()> &originCurrent);
	[[nodiscard]] AccountTransferResult importReviewed(
		const AccountTransferReview &review, const std::function<bool()> &originCurrent);
	[[nodiscard]] bool validateOperation(const AccountOperationTarget &target, bool sameInput = true);
	[[nodiscard]] bool validateStore(const AccountOperationTarget &target, const SettingsFileInput &expected, bool sameInput = true);
	[[nodiscard]] std::optional<AccountSendFingerprints> fingerprints(const AccountOperationTarget &target);
	void suppressImportedEcho(const AccountOperationTarget &target);
	[[nodiscard]] bool noteFingerprint(const AccountOperationTarget &target, bool imported);
	[[nodiscard]] std::optional<AccountOperationTarget> observeAccountSave(const AccountKey &key, bool legacy = true);
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
	[[nodiscard]] AccountTransferResult writeReviewed(
		const AccountTransferReview &review, const std::function<bool()> &originCurrent, bool importBackups);
	[[nodiscard]] SettingsOpenResult prepareInput(
		const AccountSettingsOpenTarget &target,
		const QByteArray &bytes,
		const std::function<bool()> &originCurrent,
		bool *published = nullptr,
		bool allowRoot = false);
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
	std::map<QString, SettingsFileInput> _modernSaveObservations;
	std::optional<AccountKey> _listObservationGlobal;
	QByteArray _listObservationGlobalBytes;
	std::map<QString, QByteArray> _listObservations;
	bool _persistFailed = false;
	bool _watching = false;

};

}
