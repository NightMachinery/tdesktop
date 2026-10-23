/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_settings.h"
#include "purple/purple_account_config.h"
#include "purple/purple_sync_config_history.h"
#include "purple/purple_splice.h"
#include "purple/purple_state.h"
#include "purple/purple_config_file.h"

namespace Main { class Session; }
namespace Ui { class GenericBox; }
namespace Window { class SessionController; }

namespace Purple {

// Fork-local configuration, kept as hand-editable TOML outside Telegram's own
// encrypted settings blob. See docs/purple/config.md.
[[nodiscard]] QString ConfigDirectory();
[[nodiscard]] QString SettingsFilePath();
[[nodiscard]] QString LastGoodSettingsFilePath();
[[nodiscard]] QString StateFilePath();
[[nodiscard]] AccountKey SessionAccountKey(not_null<Main::Session*> session);
void StartDesktopAccountRouting();
void RefreshDesktopAccounts();
[[nodiscard]] const Settings &AccountSettings(not_null<Main::Session*> session);
[[nodiscard]] AccountWorkMode AccountWorkModeForSession(not_null<Main::Session*> session);
[[nodiscard]] std::optional<AccountFileTarget> AccountSettingsTarget(not_null<Main::Session*> session);
[[nodiscard]] bool ValidateAccountSettingsTarget(not_null<Main::Session*> session, const AccountFileTarget &target);
[[nodiscard]] bool AccountAutomaticOperationsAllowed(not_null<Main::Session*> session);
[[nodiscard]] bool LegacyRootSettingsOperationsAllowed();
[[nodiscard]] std::optional<AccountOperationTarget> AccountSyncTarget(not_null<Main::Session*> session);
[[nodiscard]] bool ValidateAccountSyncTarget(not_null<Main::Session*> session, const AccountOperationTarget &target, bool sameInput = true);
[[nodiscard]] SyncHistoryAccess AccountSyncHistoryAccess(not_null<Main::Session*> session, const AccountOperationTarget &target);
[[nodiscard]] bool ValidateAccountSyncStoreTarget(not_null<Main::Session*> session, const AccountOperationTarget &target, const SettingsFileInput &expected, bool sameInput = true);
[[nodiscard]] bool AccountSyncOperationsAllowed(not_null<Main::Session*> session);
[[nodiscard]] GuardedFileWrite ReplaceAccountSyncSettings(not_null<Main::Session*> session, const AccountOperationTarget &target, const QByteArray &bytes);
[[nodiscard]] std::optional<ManualSettingsExportTarget> AccountManualExportTarget(not_null<Main::Session*> session);
[[nodiscard]] bool ValidateAccountManualExportTarget(not_null<Main::Session*> session, const ManualSettingsExportTarget &target);
[[nodiscard]] std::optional<AccountFileTarget> AccountSettingsEditTarget(not_null<Main::Session*> session);
[[nodiscard]] std::optional<AccountKey> AppWideSettingsAccount(
	not_null<Main::Session*> session);
[[nodiscard]] std::optional<AccountSettingsOpenTarget> AccountSettingsOpenTargetFor(
	not_null<Main::Session*> session,
	SettingsOpenRole role);
[[nodiscard]] bool ValidateAccountSettingsOpenTarget(
	not_null<Main::Session*> session,
	const AccountSettingsOpenTarget &target);
[[nodiscard]] SettingsOpenResult PrepareAccountSettingsOpen(
	not_null<Main::Session*> session,
	const AccountSettingsOpenTarget &target,
	const std::function<bool()> &windowCurrent);
[[nodiscard]] AccountSettingsRowState AccountSettingsRowFor(
	not_null<Main::Session*> session, AccountSettingsRow row);
[[nodiscard]] AccountSettingsRowWriteResult WriteAccountSettingsRow(
	not_null<Main::Session*> session,
	const AccountSettingsRowTarget &target,
	std::optional<bool> value,
	const std::function<bool()> &windowCurrent);
[[nodiscard]] QString AccountSettingsScopeCaption(
	not_null<Main::Session*> session,
	std::optional<AccountSettingsRow> row = std::nullopt);
[[nodiscard]] QString AccountSettingsResetCaption(not_null<Main::Session*> session);
[[nodiscard]] bool AccountSettingsResetVisible(
	not_null<Main::Session*> session, AccountSettingsRow row);
[[nodiscard]] QString GlobalAccountChoiceTitle(not_null<Main::Session*> session);
[[nodiscard]] bool GlobalAccountChoiceVisible(not_null<Main::Session*> session);
void ChooseGlobalAccountBox(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller);

[[nodiscard]] QString SettingsOwnerStatus();
void ChooseSettingsOwnerBox(not_null<Ui::GenericBox*> box);


// The write every file in that directory goes through: QSaveFile puts a
// temporary alongside the target and renames over it, so a crash mid-write
// cannot leave a truncated config behind. Exposed because importing a
// settings.toml from elsewhere is a write to the same file with the same
// requirement, and a second implementation of it would be one to keep in step.
bool WriteConfigFile(const QString &path, const QString &text);

// The Premium features Telegram Desktop gates on the client alone. Features
// the server enforces are deliberately not covered - see docs/purple/premium.md.
[[nodiscard]] bool LocalPremium();
[[nodiscard]] rpl::producer<bool> LocalPremiumValue();
void SetLocalPremium(bool value);

// What settings.toml said, last time it said anything we could use. A file that
// stops parsing leaves this at the last good value rather than reverting to
// defaults, so a typo mid-edit does not reshuffle the user's chat list.
[[nodiscard]] const Settings &ActiveSettings();
[[nodiscard]] bool UsingLastGoodSettings();
[[nodiscard]] std::optional<AccountSettingsWriteTarget> SettingsInputForOperation();
[[nodiscard]] bool ValidateSettingsInput(const AccountSettingsWriteTarget &expected);

struct Problems {
	QString error;
	std::vector<QString> warnings;

	[[nodiscard]] bool empty() const {
		return error.isEmpty() && warnings.empty();
	}
};

// Everything the last load could not make sense of, for the UI banner.
[[nodiscard]] const Problems &SettingsProblems();

// Fires when either of the two above changes, including on hot reload.
[[nodiscard]] rpl::producer<> SettingsChanges();

// What a write to settings.toml did, for a screen that has to say so. A
// refusal is not the app failing: every splice op checks the file before and
// after its edit and says no in words - "there is already a list called 'os'",
// "the rule at that index is not the one you read" - and those words are the
// only useful thing to put in front of someone whose edit did not land.
//
// `ok' with an empty `error' also covers "the file already said that", which is
// a write that changed nothing and is not worth telling anybody about.
struct WriteResult {
	bool ok = false;
	QString error;
	std::optional<AccountSettingsWriteTarget> target;
};

struct AppWideSettingsEditorState {
	std::optional<AccountSettingsWriteTarget> target;
	std::function<bool()> originCurrent;
};

using AppWideSettingsEditor = std::shared_ptr<AppWideSettingsEditorState>;

[[nodiscard]] AppWideSettingsEditor CaptureAppWideSettingsEditor(
	not_null<QObject*> context);
WriteResult WriteSettingsFromEditor(
	const AppWideSettingsEditor &editor,
	Fn<SpliceResult(const QString &text)> splice,
	const QString &what,
	const std::function<bool()> &originCurrent);

WriteResult WriteSettings(
	Fn<SpliceResult(const QString &text)> splice,
	const QString &what);

WriteResult WriteSettingsGuarded(
	const AccountSettingsWriteTarget &expected,
	Fn<SpliceResult(const QString &text)> splice,
	const QString &what,
	const std::function<bool()> &originCurrent = {});

using AccountListPreviewNotes = Fn<QString(const QByteArray&, const QByteArray&)>;
[[nodiscard]] AccountListPreviewNotes CaptureOwnListPreviewNotes(
	not_null<Main::Session*> session);
[[nodiscard]] bool AllAccountsWorkModeStock();
[[nodiscard]] QString AccountListsSummary(not_null<Main::Session*> session);
[[nodiscard]] QString AccountWorkModeTitle(not_null<Main::Session*> session);
[[nodiscard]] bool AccountWorkModeChoiceVisible(not_null<Main::Session*> session);
void ChooseAccountWorkModeBox(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller);
[[nodiscard]] std::vector<AccountListDeleteTarget> AccountListDeletes(
	not_null<Main::Session*> session);
void DeleteAccountListMembersBox(
	not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session,
	AccountListDeleteTarget target);
[[nodiscard]] QString AccountListEditNotice(
	not_null<Main::Session*> session,
	const AccountListEditTarget &target,
	bool member);
[[nodiscard]] QString AccountListFileWarnings(not_null<Main::Session*> session);

[[nodiscard]] std::optional<AccountListEditTarget> AccountListEditFor(
	not_null<Main::Session*> session,
	const QString &list);
[[nodiscard]] bool SetAccountListMember(
	not_null<Main::Session*> session,
	const AccountListEditTarget &target,
	PeerIdValue id,
	bool member,
	const MemberTitle &title);

// The only writes to settings.toml besides the Premium toggle. Both return
// false if the file could not be edited, having changed nothing.
bool AddToList(
	not_null<Main::Session*> session,
	const QString &list,
	PeerIdValue id,
	const MemberTitle &title);
bool RemoveFromList(
	not_null<Main::Session*> session,
	const QString &list,
	PeerIdValue id,
	const MemberTitle &title);

// The pinned order of one of a preset's extra views, which the app owns the
// same way it owns list membership: the file is where it lives, and dragging a
// row is an edit to the file. See SetViewPinned().
bool SetViewPins(
	const QString &preset,
	const QString &view,
	const std::vector<PeerIdValue> &ids,
	const MemberTitle &title);

// The same for the preset's own main view. Writing a non-empty order is what
// takes the main view off the account's pins and onto its own.
bool SetPresetPins(
	const QString &preset,
	const std::vector<PeerIdValue> &ids,
	const MemberTitle &title);

// Writes a new empty [lists.x] table. The list exists and does nothing until a
// preset names it, which is deliberate - there is no splice that edits a
// list_order, and guessing which preset wanted it would be worse than saying so.
bool CreateList(const QString &name, const QString &title);

// Remembers bytes after Telegram confirms their post to Saved Messages.
// Manual and automatic sends share this fingerprint so neither repeats bytes
// that the other posted.
void NoteSettingsSent(not_null<Main::Session*> session, const QByteArray &bytes);

// The same for a file that arrived from somewhere else and has just been
// written over the local one. This is the half of the rule that stops a
// ping-pong: without it the machine that imported a file would offer it
// straight back to the machine that sent it. See ShouldAutoSend().
void NoteSettingsImported(not_null<Main::Session*> session, const QByteArray &bytes);

[[nodiscard]] const State &CurrentState();
[[nodiscard]] rpl::producer<> StateChanges();

// Mutates state.toml through a callback so the write and the notification
// cannot be forgotten. Writes nothing if the callback changes nothing.
void UpdateState(Fn<void(State&)> apply);

} // namespace Purple
