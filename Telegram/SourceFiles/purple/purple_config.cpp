/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_config.h"
#include "platform/platform_file_utilities.h"
#include "ui/toast/toast.h"
#include "ui/delayed_activation.h"
#include "ui/layers/show.h"
#include "purple/purple_account_appwide_removal.h"
#include "ui/widgets/buttons.h"
#include "purple/purple_account_send_queue.h"
#include "purple/purple_gate.h"
#include "purple/purple_global_choice_activation.h"
#include "ui/widgets/checkbox.h"
#include "ui/boxes/confirm_box.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include <QtCore/QPointer>
#include <QtCore/qscopeguard.h>

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "purple/purple_sync_account_binding.h"
#include "purple/purple_sync_local_state.h"
#include "purple/purple_sync_local_store.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include <utility>
#include <algorithm>
#include "lang/lang_keys.h"
#include "purple/purple_readme.h"
#include "purple/purple_sync.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>

namespace Purple {
namespace {

void ShowOfflineGlobalEditBulletin();
void ShowAccountListReloadNotices();

}

bool WriteConfigFile(const QString &path, const QString &text) {
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		LOG(("Purple Error: Could not write %1.").arg(path));
		return false;
	}
	const auto bytes = text.toUtf8();
	if (file.write(bytes) != bytes.size()) {
		file.cancelWriting();
		return false;
	}
	if (!file.commit()) {
		LOG(("Purple Error: Could not commit %1.").arg(path));
		return false;
	}
	return true;
}

namespace {

DesktopAccountConfig &AccountRouting() {
	static const auto result = new DesktopAccountConfig(ConfigDirectory());
	return *result;
}

// Editors write a file in several steps, and some replace it wholesale. Wait
// for the flurry to settle rather than reloading a half-written file - which
// would parse as broken and flash an error banner on every save.
constexpr auto kReloadDelay = crl::time(250);

// How long the automatic send waits after a write before posting the file. A
// run of checkbox taps is one document rather than six, and a preset switched
// twice in a row posts what it settled on rather than what it passed through.
// Long enough to cover a hand changing its mind, short enough that the file is
// on its way before anyone has moved on to something else.
constexpr auto kAutoSendDelay = crl::time(5 * 1000);

// Written once, on first run. Everything the user may want to change should be
// present and commented, because an empty file teaches nobody what is possible.
constexpr auto kStarterSettings = R"(# The layout this file is written to, so a newer app can tell. Leave it alone.
version = 1

[premium]
# Unlock the Telegram Premium features that Telegram Desktop gates on the
# client alone: no sponsored messages, exact "last seen" times, real-time chat
# translation, and up to six accounts instead of three.
#
# Features the server enforces - large uploads, faster downloads, voice-to-text,
# custom emoji, emoji status, message effects and the rest - are unaffected by
# this and stay locked. See docs/purple/premium.md.
enabled_p = true

# Everything below configures Work Mode: named presets that decide which chats
# are visible and which may interrupt you. Nothing is active until you pick a
# preset; until then the app behaves exactly like stock Telegram Desktop.
#
# This file is yours. The app only ever edits list members and the toggle
# above, one line at a time, and never touches your comments or layout.
# See docs/purple/config.md.
#
# Two ideas, and that is the whole model:
#
#   - a LIST says who is in it, and nothing else;
#   - a PRESET names the lists it wants, in order, and says what each one does.
#
# Order is priority: the first entry whose list holds a chat decides that chat,
# and nothing further down ever sees it. A chat no entry claims is hidden and
# silenced - a preset names what gets through.

# A list matches a chat by id, by kind, or by both. Add and remove members by
# right-clicking a chat: Work Mode > the list.
[lists.os]
title = "OS"
members = [
]

[lists.emergency]
title = "Emergency"
members = [
]

# Matching by kind rather than by member. "private", "groups", "channels" and
# "bots" are the four a chat can be.
[lists.private]
title = "Private chats"
kinds = ["private"]

[lists.groups]
title = "Groups"
kinds = ["groups"]

[lists.channels]
title = "Channels"
kinds = ["channels"]

[lists.bots]
title = "Bots"
kinds = ["bots"]

# A named sequence you can splice into any preset with "*name", the way Python
# spreads a list. This is what replaces preset inheritance: reuse without a
# chain to follow when you are trying to read what a preset actually does.
[list_sets.always]
list_order = [
  { list = "os",        show_mode = "always", notify_p = true },
  { list = "emergency", show_mode = "always", notify_p = true },
]

# No show_mode here, so each entry takes the default for whatever the chat
# turns out to be: channels and bots always, groups only when they name you,
# private chats when they have something unread. The five spellings are
# "always", "message", "message_or_reaction", "mention" and "never".
[list_sets.the_rest]
list_order = [
  { list = "private",  notify_p = true },
  { list = "groups",   notify_p = true },
  { list = "channels", notify_p = true },
  { list = "bots",     notify_p = true },
]

# An example preset. Uncomment and adjust, or write your own.
#
# A running preset does not empty your chat list. It replaces the "All chats"
# tab with a view of its own, named after the preset, holding what it does not
# hide - so a hidden chat is still pinned, still searchable and still there in
# the forward picker. Set hide_everywhere_p = true if you would rather it were
# gone from the whole app.
#
# [presets.work]
# # A key that turns this preset on, and off again if it is already on. Qt
# # portable text: Ctrl, Shift, Alt and Meta joined with "+". On macOS "Ctrl"
# # means Command and "Meta" means the physical Control key. Two presets may
# # not share a key, and none may take the peek key below - Qt fires neither
# # action when a sequence is ambiguous.
# hotkey = "Ctrl+Shift+W"
# list_order = [
#   "*always",
#   { list = "groups", show_mode = "mention", notify_p = true },
#   { list = "bots",   show_mode = "never",   notify_p = false },
# ]
#
# # Folders are named the same way. "*ALL" is every folder you have; leaving
# # this key out entirely means no folder tabs at all.
# folders = [
#   "*ALL",
#   { name = "Music", notify_p = false, badge_p = false, include_in_main_view = "pinned" },
# ]
#
# # The archive goes with everything else this preset hides: no row in the list,
# # and on Android no pull gesture either. Uncomment to keep it reachable.
# # hide_archive_p = false
#
# # The "add a story" button - your own row at the head of the stories strip -
# # goes too, for the same reason and on a key of its own rather than on
# # whether a list happens to name Saved Messages. Uncomment to keep it.
# # hide_add_story_p = false
#
# # An extra tab of its own, with its own unread badge and its own pins.
# [[presets.work.views]]
# name = "People"
# list_order = [ { list = "private" } ]

[schedule]
enabled_p = true

# Which preset the schedule wants whenever no window covers the moment. Five
# o'clock puts this back, not necessarily stock Telegram: if your default is
# something quieter, name it here and the working day becomes the exception.
outside = "normal"

# A window: these days, between these times, this preset. Disabled until you
# point it at a preset you have actually written.
[[schedule.rules]]
enabled_p = false
days      = ["mon", "tue", "wed", "thu", "fri"]
from      = "09:00"
to        = "17:00"
preset    = "work"

# One file, several machines. A ruleset says which devices its windows are for,
# so the same settings.toml can carry the laptop's schedule and the phone's
# without either one running the other's. The rules above belong to no ruleset
# and are therefore for every device.
#
#   device  = "any" | "desktop" | "mobile" | "android" | "ios" | "macos" |
#             "windows" | "linux" | a device id, as listed under [devices]
#   mode    = "disabled" | "enabled" | "always"
#   outside = this ruleset's own answer to the key above, when it wants one
#
# Among the enabled rulesets that name this device only the most specific one
# runs - a ruleset naming this machine's id REPLACES the one for desktops
# rather than piling on top of it - and every "always" ruleset runs as well,
# whatever won, for the windows that are true everywhere.
[[schedule.rulesets]]
name    = "laptop"
device  = "desktop"
mode    = "disabled"
outside = "normal"

[[schedule.rulesets.rules]]
enabled_p = false
days      = ["sat", "sun"]
from      = "10:00"
to        = "14:00"
preset    = "work"

# What to call the device ids a ruleset can name. Nothing depends on a device
# being listed - an unnamed one shows as its id - but an id is unreadable and
# this is where it stops being. Settings > Advanced > Purple prints the id this
# machine reports for itself, which is the one to paste in.
#
# [devices]
# "macos-3f9a2c1d" = "the laptop"

[sync]
# Whether saving settings.toml also posts it to your Saved Messages, so the
# other machines have something newer to find on their next start. Off by
# default: sending is a message in a real chat, and that should be something
# you asked for in words rather than something an upgrade started doing on
# your behalf. See docs/purple/sync.md.
send_after_save_p = false

[peek]
# Close a conversation only when the restored preset hides it.
exit_hidden_chats_p = true
# Optional global activation delay in whole seconds. Absence means zero on
# desktop and ten on mobile. An explicit value, including zero, travels to both.
# start_delay_seconds = 0
# Temporarily reveal what the active preset is hiding.
hotkey   = "Ctrl+Shift+E"
auto_off = "2m"

# How long each way of starting one lasts, when they want to differ: tap is
# what the checkbox and the chips in the Work Mode box start, hotkey_length
# what the key starts. Unset means "whatever auto_off says"; "off" is a peek
# with no clock on it. Pressing the key again stops a running Peek, or aborts
# a pending start. The next press begins a fresh activation.
# tap           = "5m"
# hotkey_length = "2m"
#
# What a tap is worth on the phone, which reads this file but has no way to
# edit it. The one length here that does not fall back: a phone without this
# key taps for five minutes rather than for tap or auto_off, since those were
# chosen at a keyboard and a phone should not inherit a decision nobody made
# for it. "off" works here too.
# tap_mobile    = "5m"

[recent]
# Reading a chat is what takes an unread-gated one out of the view, so without
# this it vanishes on the frame you click away from it. The clock starts when
# you stop looking at the chat, not when you open it. Same spellings as
# auto_off above; "off" to turn it off.
stay_visible_after_close = "2m"

# Which chats that covers:
#
#   "already_in_view"                - only a chat that was in the view when
#                                      you opened it. Nothing you open can pull
#                                      in a chat the preset was hiding.
#   "any_open_chat"                  - any chat you open, hidden or not.
#   "any_open_chat_except_in_folder" - the above, minus the chats that are
#                                      already one click away on an extra view
#                                      or in a folder whose tab is showing.
applies_to = "already_in_view"

[suggestions]
# The strips of chats the app offers unasked - the people and recent rows in
# the search panel, the quick-share popup, the frequent contacts a gift is sent
# to - leave out what the running preset hides. Typed search, the forward picker
# and the Ctrl+Tab switcher are never touched: this is about what the app
# suggests, not about what you can still reach.
hide_invisible_p = true
)";

[[nodiscard]] std::optional<QString> ReadFile(const QString &path) {
	auto file = QFile(path);
	if (!file.exists()) {
		return std::nullopt;
	} else if (!file.open(QIODevice::ReadOnly)) {
		LOG(("Purple Error: Could not read %1.").arg(path));
		return std::nullopt;
	}
	return QString::fromUtf8(file.readAll());
}

// Everything here runs on the main thread, in response to startup, a click in
// Settings or a file change notification, so no locking.
class Config final : public base::has_weak_ptr {
public:
	Config();

	[[nodiscard]] bool localPremium() const;
	[[nodiscard]] rpl::producer<bool> localPremiumValue() const;
	void setLocalPremium(bool value);

	[[nodiscard]] const Settings &settings() const;
	[[nodiscard]] bool usingLastGoodSettings() const;
	[[nodiscard]] const Problems &problems() const;
	[[nodiscard]] rpl::producer<> changes() const;

	bool setViewPins(
		const QString &preset,
		const QString &view,
		const std::vector<PeerIdValue> &ids,
		const MemberTitle &title);
	bool setPresetPins(
		const QString &preset,
		const std::vector<PeerIdValue> &ids,
		const MemberTitle &title);
	bool createList(const QString &name, const QString &title);
	WriteResult write(
		const Fn<SpliceResult(const QString&)> &op,
		const QString &what,
		const std::optional<AccountSettingsWriteTarget> &expected = std::nullopt,
		const std::function<bool()> &originCurrent = {});
	[[nodiscard]] std::optional<AccountSettingsWriteTarget> checkedInput();
	void observeAccountFiles() { autoSendLater(); }
	void refreshAccountRows() { _accountRefreshPending = true; reloadFromDisk(); }
	GlobalAccountChoiceResult switchGlobalAccount(
		const GlobalAccountChoicePreview &preview,
		const std::function<bool()> &originCurrent);
	void flushGlobalChoiceState();

	void noteSent(const QByteArray &bytes);
	void noteImported(const QByteArray &bytes);

	[[nodiscard]] const State &state() const;
	[[nodiscard]] rpl::producer<> stateChanges() const;
	void updateState(Fn<void(State&)> apply);

private:
	void loadSettings();
	void loadState();
	void applyInput(const SettingsFileInput &input);
	void applySettings(Settings settings, const QString &path);
	void startWatching();
	void reloadFromDisk();
	void reloadStateFromDisk();
	[[nodiscard]] bool splice(
		const Fn<SpliceResult(const QString&)> &op,
		const QString &what);
	void autoSendLater();
	void autoSendNow();

	SettingsFileInput _diskInput;
	QString _settingsPath;
	bool _accountRefreshPending = false;
	bool _globalChoiceApplying = false;
	bool _globalChoiceStatePending = false;
	QString _text;
	Settings _settings;
	Problems _problems;
	bool _usingLastGoodSettings = false;

	State _state;
	QString _stateText;
	std::map<QString, AccountKey> _observedAccountKeys;
	AccountSendQueue<base::weak_ptr<Main::Session>> _accountsPendingSend;

	rpl::variable<bool> _localPremium = true;
	rpl::event_stream<> _changes;
	rpl::event_stream<> _stateChanges;

	std::unique_ptr<QFileSystemWatcher> _watcher;
	base::Timer _reload;
	base::Timer _autoSend;

};

Config::Config()
: _reload([=] { reloadFromDisk(); })
, _autoSend([=] { autoSendNow(); }) {
	loadSettings();
	loadState();

	// Before the watch, so that writing it on a fresh install - or after this
	// document changes - cannot queue a reload of the file we have just read.
	RefreshReadme();

	startWatching();
	AccountRouting().setChanged([=] {
		_accountRefreshPending = true;
		_reload.callOnce(kReloadDelay);
	});
	AccountRouting().watch();
	ShowAccountListReloadNotices();
}

void Config::loadSettings() {
	const auto path = SettingsFilePath();
	const auto input = ReadSettingsFileInput(path);
	if (input.state != SettingsFileState::Missing
		|| QFileInfo::exists(LastGoodSettingsFilePath())) {
		applyInput(input);
		return;
	} else if (path != ConfigDirectory() + u"/settings.toml"_q
		|| !QDir().mkpath(ConfigDirectory())) {
		applyInput(input);
		return;
	}
	const auto starter = QString::fromUtf8(kStarterSettings);
	if (WriteConfigFile(path, starter)) {
		applyInput({ SettingsFileState::Readable, starter.toUtf8() });
	} else {
		applyInput(ReadSettingsFileInput(path));
	}
}

void Config::applyInput(const SettingsFileInput &input) {
	_accountRefreshPending = false;
	_diskInput = input;
	_settingsPath = SettingsFilePath();
	_text = input.state == SettingsFileState::Readable
		? QString::fromUtf8(input.bytes) : QString();
	AccountRouting().refreshSettings();
	applySettings(AccountRouting().globalSettings(), SettingsFilePath());
	const auto global = AccountRouting().globalAccount().value_or(AccountKey{});
	const auto report = AccountRouting().report(global);
	_usingLastGoodSettings = report && report->global.usedGood;
	_problems = {};
	if (report) {
		_problems.error = report->global.current.parsed.error;
		_problems.warnings = report->global.current.parsed.warnings;
	}
	_changes.fire({});
}

void Config::applySettings(Settings settings, const QString &path) {
	_settings = std::move(settings);

	// One line per load, so "the toggle looks off" can be answered from the log
	// instead of from a rebuild. The file is hand-edited and lives outside
	// tdata, so which one we actually read is worth stating too.
	LOG(("Purple: %1, %2 lists, %3 presets, read from %4."
		).arg(_settings.premium.enabled
			? u"local premium ON"_q
			: u"local premium OFF"_q
		).arg(_settings.lists.size()
		).arg(_settings.presets.size()
		).arg(path));

	_localPremium = _settings.premium.enabled;
}

bool Config::usingLastGoodSettings() const {
	return _usingLastGoodSettings;
}

void Config::autoSendLater() {
	if (!Core::App().domain().started()) {
		return;
	}
	auto liveKeys = QSet<QString>();
	for (const auto &entry : Core::App().domain().accounts()) {
		const auto session = entry.account->maybeSession();
		const auto target = session ? AccountSettingsTarget(session) : std::nullopt;
		if (!target) {
			continue;
		}
		const auto key = AccountKeyText(target->account.environment, target->account.userId);
		if (!key) {
			continue;
		}
		liveKeys.insert(*key);
		_observedAccountKeys[*key] = target->account;
		const auto account = base::make_weak(entry.account.get());
		const auto origin = base::make_weak(session);
		const auto identity = target->account;
		_accountsPendingSend.observe(AccountRouting(), identity, origin,
			[account, identity](const base::weak_ptr<Main::Session> &owner) {
				const auto live = owner.get();
				const auto liveAccount = account.get();
				const auto current = live ? AccountSettingsTarget(live) : std::nullopt;
				return live && liveAccount && liveAccount->maybeSession() == live
					&& current && current->account == identity;
			});
	}
	for (auto i = _observedAccountKeys.begin(); i != _observedAccountKeys.end();) {
		if (!liveKeys.contains(i->first)) {
			_accountsPendingSend.forget(AccountRouting(), i->second);
			i = _observedAccountKeys.erase(i);
		} else {
			++i;
		}
	}
	if (!_accountsPendingSend.empty()) {
		_autoSend.callOnce(kAutoSendDelay);
	}
}

void Config::autoSendNow() {
	for (const auto &operation : _accountsPendingSend.takeReady(AccountRouting())) {
		const auto session = operation.owner.get();
		const auto &target = operation.target;
		if (!session || !ValidateAccountLegacySendTarget(session, target)) {
			continue;
		}
		const auto receipt = _accountsPendingSend.beginSend(AccountRouting(), operation);
		if (!receipt) {
			continue;
		}
		const auto weak = base::make_weak(this);
		const auto parsed = ParseSettings(QString::fromUtf8(target.file.input.bytes), target.file.path);
		if (!Upload(session, target.file.input.bytes, parsed.settings.version,
				[weak, receipt = *receipt](std::optional<MsgId>) {
			if (const auto self = weak.get()) {
				self->_accountsPendingSend.finishSend(receipt);
			}
		})) {
			_accountsPendingSend.finishSend(*receipt);
		}
	}
}

void Config::noteSent(const QByteArray &bytes) {
	const auto input = ReadSettingsFileInput(ConfigDirectory() + u"/settings.toml"_q);
	if (input.state != SettingsFileState::Readable || input.bytes != bytes) {
		return;
	}
	updateState([&](State &state) { state.lastSentFingerprint = SettingsFingerprint(bytes); });
}

void Config::noteImported(const QByteArray &bytes) {
	const auto input = ReadSettingsFileInput(ConfigDirectory() + u"/settings.toml"_q);
	if (input.state != SettingsFileState::Readable || input.bytes != bytes) {
		return;
	}
	updateState([&](State &state) { state.lastImportedFingerprint = SettingsFingerprint(bytes); });
}

void Config::startWatching() {
	if (!QCoreApplication::instance()) {
		return;
	}
	_watcher = std::make_unique<QFileSystemWatcher>();

	// Watch the directory rather than only the file: editors that save by
	// writing a temporary and renaming it over the original leave a watch on
	// the file itself pointing at an inode nobody will ever write to again.
	_watcher->addPath(ConfigDirectory());
	if (QFileInfo::exists(SettingsFilePath())) {
		_watcher->addPath(SettingsFilePath());
	}
	const auto queue = [=] { _reload.callOnce(kReloadDelay); };
	QObject::connect(
		_watcher.get(),
		&QFileSystemWatcher::directoryChanged,
		_watcher.get(),
		queue);
	QObject::connect(
		_watcher.get(),
		&QFileSystemWatcher::fileChanged,
		_watcher.get(),
		queue);
}

void Config::reloadFromDisk() {
	if (_globalChoiceApplying) { _accountRefreshPending = true; return; }
	if (_watcher
		&& !_watcher->files().contains(SettingsFilePath())
		&& QFileInfo::exists(SettingsFilePath())) {
		// Re-arm after a rename-on-save replaced the file we were watching.
		_watcher->addPath(SettingsFilePath());
	}
	AccountRouting().refreshLocalChoice();
	reloadStateFromDisk();
	ShowAccountListReloadNotices();

	const auto input = ReadSettingsFileInput(SettingsFilePath());
	if (input == _diskInput && _settingsPath == SettingsFilePath()) {
		if (std::exchange(_accountRefreshPending, false)) {
			_changes.fire({});
		}
		autoSendLater();
		flushGlobalChoiceState();
		// Our own writes come back through the watcher too.
		return;
	}
	const auto editedPath = SettingsFilePath();
	const auto editedAccount = AccountRouting().globalAccount();
	const auto edited = _settingsPath == editedPath
		&& _diskInput.state == SettingsFileState::Readable
		&& input.state == SettingsFileState::Readable;
	applyInput(input);
	autoSendLater();
	flushGlobalChoiceState();
	if (edited && editedPath == SettingsFilePath()
		&& editedAccount == AccountRouting().globalAccount()
		&& _problems.error.isEmpty() && !_usingLastGoodSettings) {
		ShowOfflineGlobalEditBulletin();
	}
}

void Config::reloadStateFromDisk() {
	// state.toml is the app's file, so this is not the point of the watch. It
	// is here because picking a preset by hand is the only way to pick one
	// until the Work Mode UI lands, and requiring a restart for that would make
	// the whole engine untestable. Our own writes land here too and compare
	// equal, so they cost a read and nothing else.
	auto text = ReadFile(StateFilePath());
	if (!text || *text == _stateText) {
		return;
	}
	_stateText = std::move(*text);
	_state = ParseState(_stateText, StateFilePath());
	_stateChanges.fire({});
}

bool Config::localPremium() const {
	return _localPremium.current();
}

rpl::producer<bool> Config::localPremiumValue() const {
	return _localPremium.value();
}

void Config::setLocalPremium(bool value) {
	if (_localPremium.current() == value) {
		return;
	}
	const auto written = splice([&](const QString &text) {
		return SetTableBool(
			text,
			SettingsFilePath(),
			u"premium"_q,
			u"enabled_p"_q,
			value);
	}, u"the Premium toggle"_q);
	if (!written) {
		// Keep memory and disk agreeing. Changing the setting anyway would
		// apply now and silently revert on the next start.
		return;
	}
	_localPremium = value;
}

const Settings &Config::settings() const {
	return _settings;
}

const Problems &Config::problems() const {
	return _problems;
}

rpl::producer<> Config::changes() const {
	return _changes.events();
}

WriteResult Config::write(
		const Fn<SpliceResult(const QString&)> &op,
		const QString &what,
		const std::optional<AccountSettingsWriteTarget> &expected,
		const std::function<bool()> &originCurrent) {
	Expects(op != nullptr);
	const auto input = expected ? expected : checkedInput();
	if (!input || input->destination.role != SettingsOpenRole::AppWide
		|| !AccountRouting().validateSettingsEdit(*input)) {
		return { .error = u"Settings changed on disk. Try again."_q };
	}
	observeAccountFiles();
	const auto current = [&] {
		return ConfigDirectory() == input->destination.root && (!originCurrent || originCurrent());
	};
	const auto result = AccountRouting().writeSettingsEdit(*input,
		[&](const QString &text) { return op(text); }, current);
	if (!result.ok) {
		LOG(("Purple Error: Could not write %1 to %2: %3."
			).arg(what, input->destination.file.path, result.error));
		return { .error = result.error };
	}
	if (result.changed) {
		refreshAccountRows();
		observeAccountFiles();
		ShowOfflineGlobalEditBulletin();
	}
	if (!current() || !result.target || !AccountRouting().validateSettingsEdit(*result.target)) {
		return { .error = u"Settings context changed after writing."_q };
	}
	return { .ok = true, .target = result.target };
}

std::optional<AccountSettingsWriteTarget> Config::checkedInput() {
	reloadStateFromDisk();
	const auto global = AccountRouting().globalAccount();
	const auto target = global
		? AccountRouting().settingsEditTarget(*global, SettingsOpenRole::AppWide) : std::nullopt;
	if (!target) { return std::nullopt; }
	if (target->destination.file.input != _diskInput || _settingsPath != target->destination.file.path) {
		applyInput(target->destination.file.input);
		return std::nullopt;
	}
	return target;
}

bool Config::splice(
		const Fn<SpliceResult(const QString&)> &op,
		const QString &what) {
	return write(op, what).ok;
}

bool Config::setViewPins(
		const QString &preset,
		const QString &view,
		const std::vector<PeerIdValue> &ids,
		const MemberTitle &title) {
	return splice([&](const QString &text) {
		return SetViewPinned(text, SettingsFilePath(), preset, view, ids, title);
	}, u"the pins of view '%1'"_q.arg(view));
}

bool Config::setPresetPins(
		const QString &preset,
		const std::vector<PeerIdValue> &ids,
		const MemberTitle &title) {
	return splice([&](const QString &text) {
		return SetPresetPinned(text, SettingsFilePath(), preset, ids, title);
	}, u"the pins of preset '%1'"_q.arg(preset));
}

bool Config::createList(const QString &name, const QString &title) {
	return splice([&](const QString &text) {
		return AddList(text, SettingsFilePath(), name, title);
	}, u"the list '%1'"_q.arg(name));
}

void Config::loadState() {
	_stateText = ReadFile(StateFilePath()).value_or(QString());
	_state = ParseState(_stateText, StateFilePath());
}

const State &Config::state() const {
	return _state;
}

rpl::producer<> Config::stateChanges() const {
	return _stateChanges.events();
}

void Config::updateState(Fn<void(State&)> apply) {
	Expects(apply != nullptr);

	auto updated = _state;
	apply(updated);
	auto text = SerializeState(updated);
	if (text == _stateText) {
		return;
	}
	_state = std::move(updated);
	_stateText = std::move(text);

	// Unlike settings.toml, a failed write here is not worth refusing the
	// change over: the state is what the app is already doing, and losing it
	// costs the user a preset to re-pick after a restart, nothing more.
	if (!QDir().mkpath(ConfigDirectory())
		|| !WriteConfigFile(StateFilePath(), _stateText)) {
		LOG(("Purple Error: Could not save %1.").arg(StateFilePath()));
	}
	if (_globalChoiceApplying) { _globalChoiceStatePending = true; }
	else { _stateChanges.fire({}); }
}

void Config::flushGlobalChoiceState() {
	if (!_globalChoiceApplying && std::exchange(_globalChoiceStatePending, false)) {
		_stateChanges.fire({});
	}
}

GlobalAccountChoiceResult Config::switchGlobalAccount(
		const GlobalAccountChoicePreview &preview,
		const std::function<bool()> &originCurrent) {
	if (ConfigDirectory() != preview.current.root || _globalChoiceApplying || !GlobalAccountChoiceActivationReady()
		|| SerializeState(_state) != preview.activationState) {
		return { .error = u"The device state changed. Preview the choice again."_q };
	}
	const auto verified = AccountRouting().previewGlobalChoice(preview.current.requester,
		preview.selected.file.account, preview.runningPreset);
	if (!verified || !AccountRouting().validateGlobalChoice(preview)) {
		return { .error = u"The preview changed. Preview the choice again."_q };
	}
	_globalChoiceApplying = true;
	auto barrier = qScopeGuard([&] { _globalChoiceApplying = false; });
	const auto beforeCommit = [&] {
		return originCurrent && originCurrent() && ConfigDirectory() == preview.current.root
			&& SerializeState(_state) == preview.activationState;
	};
	auto result = AccountRouting().commitGlobalChoice(preview, beforeCommit);
	if (!result.changed) { return result; }
	_accountRefreshPending = true;
	_reload.callOnce(kReloadDelay);
	const auto committedCurrent = [&] {
		return originCurrent && originCurrent() && ConfigDirectory() == preview.current.root
			&& AccountRouting().validateGlobalChoiceCommit(preview, result);
	};
	const auto normal = [&] {
		updateState([](State &state) {
			state.activePreset = NormalPreset();
			state.activeSource = PresetSource::Manual;
		});
		return RefreshGlobalAccountChoiceNormal();
	};
	if (!RunGlobalAccountChoiceActivation(result, preview.enterNormal, committedCurrent, normal,
			[&] { return AccountRouting().activateGlobalChoice(preview, result); })) {
		result.error = u"The app-wide choice was saved, but activation did not finish safely. Preview it again."_q;
		return result;
	}
	_diskInput = preview.selected.file.input;
	_settingsPath = preview.selected.file.path;
	_text = QString::fromUtf8(_diskInput.bytes);
	AccountRouting().refreshSettings();
	applySettings(verified->after, _settingsPath);
	_usingLastGoodSettings = preview.selectedUsesGood;
	_problems = {};
	const auto current = ParseSettings(_text, _settingsPath);
	_problems.error = current.error;
	_problems.warnings = current.warnings;
	_accountRefreshPending = false;
	_changes.fire({});
	_globalChoiceApplying = false;
	barrier.dismiss();
	flushGlobalChoiceState();
	if (!originCurrent() || !AccountRouting().validateGlobalChoiceCommit(preview, result)) {
		result.ok = false;
		result.error = u"The app-wide choice changed during activation."_q;
	}
	return result;
}

// Deliberately never destroyed. It owns QObjects - a watcher and a timer - and
// a function-local static would tear them down at exit, after Qt has already
// gone, for no benefit whatsoever.
[[nodiscard]] Config &Instance() {
	static const auto result = new Config();
	return *result;
}

} // namespace

QString ConfigDirectory() {
	const auto xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
	return xdg.isEmpty()
		? (QDir::homePath() + u"/.purple-telegram"_q)
		: (xdg + u"/purple-telegram"_q);
}

QString SettingsFilePath() {
	return AccountRouting().globalSettingsPath();
}

QString LastGoodSettingsFilePath() {
	return SettingsFilePath() + u".good"_q;
}

QString StateFilePath() {
	return ConfigDirectory() + u"/state.toml"_q;
}

bool LocalPremium() {
	return Instance().localPremium();
}

rpl::producer<bool> LocalPremiumValue() {
	return Instance().localPremiumValue();
}

void SetLocalPremium(bool value) {
	Instance().setLocalPremium(value);
}

const Settings &ActiveSettings() {
	return Instance().settings();
}

bool UsingLastGoodSettings() {
	return Instance().usingLastGoodSettings();
}

const Problems &SettingsProblems() {
	return Instance().problems();
}

rpl::producer<> SettingsChanges() {
	return Instance().changes();
}

std::optional<AccountSettingsWriteTarget> SettingsInputForOperation() {
	return Instance().checkedInput();
}

bool ValidateSettingsInput(const AccountSettingsWriteTarget &expected) {
	const auto current = SettingsInputForOperation();
	return current && expected.destination.role == SettingsOpenRole::AppWide
		&& ConfigDirectory() == expected.destination.root
		&& AccountRouting().validateSettingsEdit(expected);
}

WriteResult WriteSettingsGuarded(
		const AccountSettingsWriteTarget &expected,
		Fn<SpliceResult(const QString &text)> splice,
		const QString &what,
		const std::function<bool()> &originCurrent) {
	return Instance().write(splice, what, expected, originCurrent);
}

AppWideSettingsEditor CaptureAppWideSettingsEditor(not_null<QObject*> context) {
	const auto weak = QPointer<QObject>(context.get());
	return std::make_shared<AppWideSettingsEditorState>(AppWideSettingsEditorState{
		SettingsInputForOperation(), [=] { return !weak.isNull(); } });
}

WriteResult WriteSettingsFromEditor(
		const AppWideSettingsEditor &editor,
		Fn<SpliceResult(const QString &text)> splice,
		const QString &what,
		const std::function<bool()> &originCurrent) {
	if (!editor || !editor->target || !editor->originCurrent()) {
		return { .error = u"Reopen the settings editor and try again."_q };
	}
	const auto current = [=] {
		return editor->originCurrent() && originCurrent && originCurrent();
	};
	const auto result = WriteSettingsGuarded(*editor->target, std::move(splice), what, current);
	if (result.ok) { editor->target = result.target; }
	return result;
}

std::optional<AccountListEditTarget> AccountListEditFor(
		not_null<Main::Session*> session,
		const QString &list) {
	return !session->account().loggingOut() && session->account().maybeSession() == session.get()
		? AccountRouting().listEditTarget(SessionAccountKey(session), list) : std::nullopt;
}

bool SetAccountListMember(
		not_null<Main::Session*> session,
		const AccountListEditTarget &target,
		PeerIdValue id,
		bool member,
		const MemberTitle &title) {
	const auto weak = base::make_weak(session.get());
	const auto originCurrent = [=] {
		const auto live = weak.get();
		return live && !live->account().loggingOut() && live->account().maybeSession() == live
			&& SessionAccountKey(live) == target.edit.destination.requester
			&& ConfigDirectory() == target.edit.destination.root;
	};
	if (!originCurrent() || !AccountRouting().validateSettingsEdit(target.edit)) { return false; }
	Instance().observeAccountFiles();
	const auto result = AccountRouting().writeListMember(target, id, member, title, originCurrent);
	if (!result.ok) { return false; }
	Instance().refreshAccountRows();
	Instance().observeAccountFiles();
	if (result.changed && target.edit.destination.file.account == target.edit.destination.global) {
		ShowOfflineGlobalEditBulletin();
	}
	return originCurrent() && result.target && AccountRouting().validateSettingsEdit(*result.target);
}

bool AddToList(
		not_null<Main::Session*> session,
		const QString &list,
		PeerIdValue id,
		const MemberTitle &title) {
	const auto target = AccountListEditFor(session, list);
	return target && SetAccountListMember(session, *target, id, true, title);
}

bool RemoveFromList(
		not_null<Main::Session*> session,
		const QString &list,
		PeerIdValue id,
		const MemberTitle &title) {
	const auto target = AccountListEditFor(session, list);
	return target && SetAccountListMember(session, *target, id, false, title);
}

bool SetViewPins(
		const QString &preset,
		const QString &view,
		const std::vector<PeerIdValue> &ids,
		const MemberTitle &title) {
	return Instance().setViewPins(preset, view, ids, title);
}

bool SetPresetPins(
		const QString &preset,
		const std::vector<PeerIdValue> &ids,
		const MemberTitle &title) {
	return Instance().setPresetPins(preset, ids, title);
}

bool CreateList(const QString &name, const QString &title) {
	return Instance().createList(name, title);
}

WriteResult WriteSettings(
		Fn<SpliceResult(const QString &text)> splice,
		const QString &what) {
	return Instance().write(splice, what);
}

void NoteSettingsSent(not_null<Main::Session*> session, const QByteArray &bytes) {
	const auto target = AccountLegacySendTarget(session);
	if (!target || target->file.input.bytes != bytes) {
		return;
	}
	if (target->files.fingerprintStorage == AccountFingerprintStorage::DeviceState) {
		Instance().noteSent(bytes);
	} else {
		(void)AccountRouting().noteFingerprint(*target, false);
	}
}

void NoteSettingsImported(not_null<Main::Session*> session, const QByteArray &bytes) {
	const auto target = AccountLegacySendTarget(session);
	if (!target || target->file.input.bytes != bytes) {
		return;
	}
	AccountRouting().suppressImportedEcho(*target);
	if (target->files.fingerprintStorage == AccountFingerprintStorage::DeviceState) {
		Instance().noteImported(bytes);
	} else {
		(void)AccountRouting().noteFingerprint(*target, true);
	}
}

const State &CurrentState() {
	return Instance().state();
}

rpl::producer<> StateChanges() {
	return Instance().stateChanges();
}

void UpdateState(Fn<void(State&)> apply) {
	Instance().updateState(std::move(apply));
}

AccountKey SessionAccountKey(not_null<Main::Session*> session) {
	return { session->isTestMode() ? 1 : 0, int64(session->userId().bare) };
}

void RefreshDesktopAccounts() {
	auto &domain = Core::App().domain();
	if (!domain.started()) {
		return;
	}
	auto accounts = std::vector<StartupAccountCandidate>();
	auto storeBytes = QByteArray();
	const auto storeRead = ReadSyncPrivateFile(ConfigDirectory() + u"/sync/state.json"_q,
		1024 * 1024, storeBytes);
	const auto store = storeRead == SyncStoreStatus::Ready
		? ParseSyncLocalState(storeBytes) : SyncLocalParseResult();
	for (const auto &entry : domain.accounts()) {
		const auto session = entry.account->maybeSession();
		if (!session || entry.account->loggingOut()) {
			continue;
		}
		const auto key = SessionAccountKey(session);
		const auto text = AccountKeyText(key.environment, key.userId);
		if (!text) {
			continue;
		}
		accounts.push_back({ key,
			QFileInfo::exists(ConfigDirectory() + u"/accounts/"_q + *text),
			store && CheckAccountSyncBinding(store.state, *entry.account)
				== SyncAccountBindingVerdict::Bound });
	}
	AccountRouting().start(std::move(accounts));
	Instance().observeAccountFiles();
	ShowAccountListReloadNotices();
}

void StartDesktopAccountRouting() {
	static auto started = false;
	if (std::exchange(started, true)) {
		return;
	}
	static auto lifetime = rpl::lifetime();
	static auto sessions = rpl::lifetime();
	auto &domain = Core::App().domain();
	const auto refresh = [&domain] {
		sessions.destroy();
		for (const auto &entry : domain.accounts()) {
			entry.account->sessionChanges() | rpl::on_next([] {
				RefreshDesktopAccounts();
			}, sessions);
		}
		RefreshDesktopAccounts();
	};
	domain.accountsChanges() | rpl::on_next(refresh, lifetime);
	domain.activeSessionChanges() | rpl::on_next([=] { refresh(); }, lifetime);
	refresh();
}

const Settings &AccountSettings(not_null<Main::Session*> session) {
	return AccountRouting().settings(SessionAccountKey(session));
}

AccountWorkMode AccountWorkModeForSession(not_null<Main::Session*> session) {
	return AccountRouting().workMode(SessionAccountKey(session));
}

std::optional<AccountFileTarget> AccountSettingsTarget(not_null<Main::Session*> session) {
	if (session->account().loggingOut() || session->account().maybeSession() != session.get()) {
		return std::nullopt;
	}
	return AccountRouting().target(SessionAccountKey(session));
}

bool ValidateAccountSettingsTarget(not_null<Main::Session*> session,
		const AccountFileTarget &target) {
	return !session->account().loggingOut()
		&& session->account().maybeSession() == session.get()
		&& SessionAccountKey(session) == target.account
		&& AccountRouting().validate(target);
}

std::optional<ManualSettingsExportTarget> AccountManualExportTarget(
		not_null<Main::Session*> session) {
	if (session->account().loggingOut() || session->account().maybeSession() != session.get()) {
		return std::nullopt;
	}
	return AccountRouting().manualExportTarget(SessionAccountKey(session));
}

bool ValidateAccountManualExportTarget(not_null<Main::Session*> session,
		const ManualSettingsExportTarget &target) {
	return !session->account().loggingOut()
		&& session->account().maybeSession() == session.get()
		&& SessionAccountKey(session) == target.file.account
		&& AccountRouting().validateManualExport(target);
}

std::optional<AccountFileTarget> AccountSettingsEditTarget(
		not_null<Main::Session*> session) {
	if (session->account().loggingOut() || session->account().maybeSession() != session.get()) {
		return std::nullopt;
	}
	return AccountRouting().editTarget(SessionAccountKey(session));
}

std::optional<AccountKey> AppWideSettingsAccount(not_null<Main::Session*> session) {
	if (session->account().loggingOut() || session->account().maybeSession() != session.get()) {
		return std::nullopt;
	}
	AccountRouting().refreshLocalChoice();
	return AccountRouting().globalAccount();
}

std::optional<AccountSettingsOpenTarget> AccountSettingsOpenTargetFor(
		not_null<Main::Session*> session,
		SettingsOpenRole role) {
	if (session->account().loggingOut() || session->account().maybeSession() != session.get()) {
		return std::nullopt;
	}
	const auto target = AccountRouting().openTarget(SessionAccountKey(session), role);
	return target && target->root == ConfigDirectory() ? target : std::nullopt;
}

bool ValidateAccountSettingsOpenTarget(
		not_null<Main::Session*> session,
		const AccountSettingsOpenTarget &target) {
	return !session->account().loggingOut()
		&& session->account().maybeSession() == session.get()
		&& SessionAccountKey(session) == target.requester
		&& ConfigDirectory() == target.root && AccountRouting().validateOpen(target);
}

SettingsOpenResult PrepareAccountSettingsOpen(
		not_null<Main::Session*> session,
		const AccountSettingsOpenTarget &target,
		const std::function<bool()> &windowCurrent) {
	const auto weak = base::make_weak(session.get());
	const auto originCurrent = [=] {
		const auto live = weak.get();
		return live && !live->account().loggingOut() && live->account().maybeSession() == live
			&& SessionAccountKey(live) == target.requester && ConfigDirectory() == target.root
			&& windowCurrent && windowCurrent();
	};
	if (!originCurrent() || !ValidateAccountSettingsOpenTarget(session, target)) { return {}; }
	const auto result = AccountRouting().prepareOpen(target, originCurrent);
	if (!originCurrent() || !result.target
		|| !ValidateAccountSettingsOpenTarget(weak.get(), *result.target)) {
		return { result.target ? SettingsOpenStatus::Changed : result.status };
	}
	return result;
}

std::optional<AccountOperationTarget> AccountSyncInspectionTarget(
		not_null<Main::Session*> session) {
	if (session->account().loggingOut() || session->account().maybeSession() != session.get()) { return std::nullopt; }
	return AccountRouting().operationTarget(SessionAccountKey(session), false, true);
}

std::optional<AccountTransferReview> RebaseAccountTransferAfterSetup(
		not_null<Main::Session*> session, const AccountTransferReview &review) {
	if (session->account().loggingOut() || session->account().maybeSession() != session.get()
		|| SessionAccountKey(session) != review.destination.requester) { return std::nullopt; }
	return AccountRouting().rebaseTransferAfterSetup(review);
}

AccountTransferResult ReplaceReviewedAccountSettings(
		not_null<Main::Session*> session, const AccountTransferReview &review) {
	const auto weak = base::make_weak(session.get());
	return AccountRouting().replaceReviewed(review, [=] {
		const auto live = weak.get();
		return live && !live->account().loggingOut() && live->account().maybeSession() == live
			&& SessionAccountKey(live) == review.destination.requester;
	});
}

std::optional<AccountOperationTarget> AccountSyncTarget(not_null<Main::Session*> session) {
	if (session->account().loggingOut() || session->account().maybeSession() != session.get()) {
		return std::nullopt;
	}
	return AccountRouting().operationTarget(SessionAccountKey(session));
}

bool ValidateAccountSyncTarget(not_null<Main::Session*> session,
		const AccountOperationTarget &target, bool sameInput) {
	return !target.legacySend && !session->account().loggingOut()
		&& session->account().maybeSession() == session.get()
		&& SessionAccountKey(session) == target.file.account
		&& AccountRouting().validateOperation(target, sameInput);
}

std::optional<AccountOperationTarget> AccountLegacySendTarget(
		not_null<Main::Session*> session) {
	if (session->account().loggingOut() || session->account().maybeSession() != session.get()) {
		return std::nullopt;
	}
	return AccountRouting().operationTarget(SessionAccountKey(session), true);
}

bool ValidateAccountLegacySendTarget(
		not_null<Main::Session*> session, const AccountOperationTarget &target) {
	return target.legacySend && !session->account().loggingOut()
		&& session->account().maybeSession() == session.get()
		&& SessionAccountKey(session) == target.file.account
		&& AccountRouting().validateOperation(target);
}

QString ConfigImpactDisplayText(const ConfigChangeImpact &impact) {
	if (impact.status != ConfigImpactStatus::Ready) {
		return tr::lng_purple_transfer_unavailable(tr::now);
	}
	auto used = QStringList();
	auto inactive = QStringList();
	auto warnings = QStringList();
	for (const auto &finding : impact.findings) {
		const auto &key = finding.change;
		const auto address = key.table + (key.key.isEmpty() ? QString() : u"."_q + key.key);
		const auto name = finding.label.isEmpty() || finding.label == address
			? address : finding.label + u" ("_q + address + u")"_q;
		if (finding.kind == ConfigImpactKind::ValueChanged) {
			const auto before = key.beforeValue.value_or(tr::lng_purple_transfer_absent(tr::now));
			const auto after = key.afterValue.value_or(tr::lng_purple_transfer_absent(tr::now));
			auto line = name + u": "_q + before + u" → "_q + after;
			if (!ConfigImpactFindingUsedHere(finding)) {
				if (finding.afterUse == ConfigImpactUse::Follow || finding.beforeUse == ConfigImpactUse::Follow) {
					line += tr::lng_purple_transfer_follow(tr::now);
				} else if (finding.afterUse == ConfigImpactUse::Stock || finding.beforeUse == ConfigImpactUse::Stock) {
					line += tr::lng_purple_transfer_stock(tr::now);
				} else if (finding.afterUse == ConfigImpactUse::UndefinedList || finding.beforeUse == ConfigImpactUse::UndefinedList) {
					line += tr::lng_purple_transfer_undefined(tr::now);
				} else if (finding.afterUse == ConfigImpactUse::Unknown || finding.beforeUse == ConfigImpactUse::Unknown) {
					line += tr::lng_purple_transfer_unknown_use(tr::now);
				} else if (finding.afterUse == ConfigImpactUse::AppWideElsewhere || finding.beforeUse == ConfigImpactUse::AppWideElsewhere) {
					line += tr::lng_purple_transfer_elsewhere(tr::now);
				}
				inactive.push_back(line);
			} else {
				used.push_back(line);
			}
			continue;
		}
		auto detail = QString();
		switch (finding.kind) {
		case ConfigImpactKind::Pinned: detail = tr::lng_purple_transfer_pin(tr::now); break;
		case ConfigImpactKind::Unpinned: detail = tr::lng_purple_transfer_unpin(tr::now); break;
		case ConfigImpactKind::AppWideLoss: detail = tr::lng_purple_transfer_appwide_loss(tr::now); break;
		case ConfigImpactKind::ListLoss: detail = tr::lng_purple_transfer_list_loss(tr::now); break;
		case ConfigImpactKind::PresetLoss: detail = tr::lng_purple_transfer_preset_loss(tr::now); break;
		case ConfigImpactKind::ScheduleLoss: detail = tr::lng_purple_transfer_schedule_loss(tr::now); break;
		case ConfigImpactKind::DefinedListEmptied: detail = tr::lng_purple_transfer_empty(tr::now); break;
		case ConfigImpactKind::StrandedMembers: detail = tr::lng_purple_transfer_stranded(tr::now); break;
		case ConfigImpactKind::UnknownRemoval: detail = tr::lng_purple_transfer_unknown(tr::now); break;
		case ConfigImpactKind::ValueChanged: break;
		}
		auto affected = finding.account;
		if (!affected.isEmpty() && Core::App().domain().started()) {
			for (const auto &entry : Core::App().domain().accounts()) {
				const auto live = entry.account->maybeSession();
				if (live && AccountKeyText(SessionAccountKey(live).environment, SessionAccountKey(live).userId) == affected) {
					affected = ImportAccountLabel(live->user()->name(), live->user()->username());
					break;
				}
			}
		}
		warnings.push_back(detail + u": "_q + name
			+ (affected.isEmpty() ? QString() : u" ("_q + affected + u")"_q)
			+ (finding.beforeCount >= 0 ? u" [%1 → %2]"_q.arg(finding.beforeCount).arg(finding.afterCount) : QString()));
	}
	auto lines = QStringList();
	if (!warnings.empty()) { lines.push_back(warnings.join('\n')); }
	if (!used.empty()) {
		lines.push_back(tr::lng_purple_transfer_used(tr::now) + u"\n"_q + used.join('\n'));
	}
	if (!inactive.empty()) {
		lines.push_back(tr::lng_purple_transfer_inactive(tr::now) + u"\n"_q + inactive.join('\n'));
	}
	if (used.empty() && !inactive.empty()) { lines.prepend(tr::lng_purple_transfer_inactive_only(tr::now)); }
	if (lines.empty()) { lines.push_back(tr::lng_purple_transfer_no_changes(tr::now)); }
	return lines.join(u"\n\n"_q);
}

std::optional<AccountTransferReview> ReviewAccountTransfer(
		not_null<Main::Session*> session, const QByteArray &bytes) {
	if (session->account().loggingOut() || session->account().maybeSession() != session.get()) {
		return std::nullopt;
	}
	return AccountRouting().reviewTransfer(SessionAccountKey(session), bytes);
}

AccountTransferResult ImportReviewedAccountTransfer(
		not_null<Main::Session*> session,
		const AccountTransferReview &review,
		const std::function<bool()> &originCurrent) {
	const auto weak = base::make_weak(session.get());
	const auto current = [=] {
		const auto live = weak.get();
		return live && !live->account().loggingOut() && live->account().maybeSession() == live
			&& SessionAccountKey(live) == review.destination.requester
			&& originCurrent && originCurrent();
	};
	const auto result = AccountRouting().importReviewed(review, current);
	if (result.ok && current()) { NoteSettingsImported(session, review.impact.proposedText); }
	return result;
}

SyncHistoryAccess AccountSyncHistoryAccess(not_null<Main::Session*> session,
		const AccountOperationTarget &target) {
	const auto weak = base::make_weak(session.get());
	return { target.files, [weak, target] {
		const auto live = weak.get();
		return live && ValidateAccountSyncTarget(live, target);
	} };
}

bool ValidateAccountSyncStoreTarget(not_null<Main::Session*> session,
		const AccountOperationTarget &target, const SettingsFileInput &expected, bool sameInput) {
	return ValidateAccountSyncTarget(session, target, sameInput)
		&& AccountRouting().validateStore(target, expected, sameInput);
}

bool AccountSyncOperationsAllowed(not_null<Main::Session*> session) {
	return AccountSyncInspectionTarget(session).has_value();
}

GuardedFileWrite ReplaceAccountSyncSettings(not_null<Main::Session*> session,
		const AccountOperationTarget &target, const QByteArray &bytes) {
	return ValidateAccountSyncTarget(session, target)
		? AccountRouting().replace(target, bytes) : GuardedFileWrite::InputChanged;
}

bool LegacyRootSettingsOperationsAllowed() {
	return AccountRouting().permissionsReady()
		&& AccountRouting().globalAccount() == AccountRouting().owner();
}

bool AccountAutomaticOperationsAllowed(not_null<Main::Session*> session) {
	return LegacyRootSettingsOperationsAllowed()
		&& !session->account().loggingOut()
		&& session->account().maybeSession() == session.get()
		&& AccountRouting().accountTrafficReady(SessionAccountKey(session));
}

AccountSettingsRowState AccountSettingsRowFor(
		not_null<Main::Session*> session, AccountSettingsRow row) {
	if (session->account().loggingOut() || session->account().maybeSession() != session.get()) {
		return {};
	}
	return AccountRouting().rowState(SessionAccountKey(session), row);
}

AccountSettingsRowWriteResult WriteAccountSettingsRow(
		not_null<Main::Session*> session,
		const AccountSettingsRowTarget &target,
		std::optional<bool> value,
		const std::function<bool()> &windowCurrent) {
	const auto weak = base::make_weak(session.get());
	const auto originCurrent = [=] {
		const auto live = weak.get();
		return live && !live->account().loggingOut() && live->account().maybeSession() == live
			&& SessionAccountKey(live) == target.destination.requester
			&& ConfigDirectory() == target.destination.root && windowCurrent && windowCurrent();
	};
	if (!originCurrent() || !AccountRouting().validateRow(target)) {
		return { .error = u"Settings changed. Try again."_q };
	}
	Instance().observeAccountFiles();
	if (!originCurrent() || !AccountRouting().validateRow(target)) {
		return { .error = u"Settings changed. Try again."_q };
	}
	auto result = AccountRouting().writeRow(target, value, originCurrent);
	if (!result.ok || !result.target) { return result; }
	if (!originCurrent()) {
		result.ok = false;
		result.target.reset();
		result.error = u"Settings context changed after writing."_q;
		return result;
	}
	Instance().refreshAccountRows();
	if (!originCurrent() || !AccountRouting().validateRow(*result.target)) {
		result.ok = false;
		result.target.reset();
		result.error = u"Settings context changed after writing."_q;
		return result;
	}
	Instance().observeAccountFiles();
	if (!originCurrent() || !AccountRouting().validateRow(*result.target)) {
		result.ok = false;
		result.target.reset();
		result.error = u"Settings context changed after writing."_q;
	}
	return result;
}

namespace {

struct SettingsAccountLabels {
	QString own;
	QString global;
	bool globalSignedIn = false;
	bool multiple = false;
	QStringList overrides;
};

SettingsAccountLabels SettingsRowLabels(
		not_null<Main::Session*> session, std::optional<AccountSettingsRow> row) {
	auto result = SettingsAccountLabels();
	result.own = session->user()->name();
	const auto global = AppWideSettingsAccount(session);
	if (!global) { return result; }
	auto keys = std::vector<AccountKey>{ *global };
	result.global = tr::lng_purple_settings_signed_out_global(tr::now, lt_account,
		AccountKeyText(global->environment, global->userId).value_or(QString()));
	for (const auto &entry : Core::App().domain().accounts()) {
		const auto live = entry.account->maybeSession();
		if (!live || entry.account->loggingOut()) { continue; }
		const auto key = SessionAccountKey(live);
		if (std::find(keys.begin(), keys.end(), key) == keys.end()) { keys.push_back(key); }
		if (key == *global) {
			result.global = live->user()->name();
			result.globalSignedIn = true;
		} else if (row && AccountRouting().rowState(key, *row).pinned) {
			result.overrides.push_back(live->user()->name());
		}
	}
	result.multiple = keys.size() >= 2;
	return result;
}

void ShowOfflineGlobalEditBulletin() {
	static auto shown = false;
	const auto window = Core::App().activeWindow();
	const auto controller = window ? window->sessionController() : nullptr;
	const auto global = AccountRouting().globalAccount();
	if (shown || !controller || !global) { return; }
	const auto labels = SettingsRowLabels(&controller->session(), std::nullopt);
	if (labels.globalSignedIn) { return; }
	shown = true;
	const auto weak = base::make_weak(controller);
	controller->show(Ui::MakeConfirmBox({
		.text = tr::lng_purple_global_offline_edit(
			tr::now,
			lt_account,
			labels.global,
			lt_name,
			labels.global),
		.confirmed = [=] {
			const auto live = weak.get();
			if (live && AccountRouting().globalAccount() == global) {
				live->show(Box(ChooseGlobalAccountBox, not_null{ live }));
			}
		},
		.confirmText = tr::lng_purple_global_change(tr::now),
		.cancelText = tr::lng_box_ok(tr::now),
	}));
}

constexpr auto kAccountListNoticeDuration = crl::time(10000);

void ShowAccountListReloadNotices() {
	if (!Core::App().domain().started()) { return; }
	auto keys = std::vector<AccountKey>();
	for (const auto &entry : Core::App().domain().accounts()) {
		const auto live = entry.account->maybeSession();
		if (live && !live->account().loggingOut()) { keys.push_back(SessionAccountKey(live)); }
	}
	const auto changes = AccountRouting().observeListReloads(keys);
	const auto window = Core::App().activeWindow();
	if (!window) { return; }
	for (const auto &change : changes) {
		auto session = static_cast<Main::Session*>(nullptr);
		for (const auto &entry : Core::App().domain().accounts()) {
			const auto live = entry.account->maybeSession();
			if (live && !live->account().loggingOut() && SessionAccountKey(live) == change.account) { session = live; break; }
		}
		if (!session) { continue; }
		const auto labels = SettingsRowLabels(session, std::nullopt);
		auto lines = QStringList();
		if (change.impact) { lines.push_back(labels.own + u":\n"_q + ConfigImpactDisplayText(*change.impact)); }
		if (change.inactiveChanges) {
			lines.push_back(tr::lng_purple_lists_reload_inactive(tr::now, lt_account, labels.own).arg(change.inactiveChanges));
		}
		for (const auto &[name, count] : change.unusedMembers) {
			lines.push_back(tr::lng_purple_lists_undefined(tr::now, lt_account, labels.own,
				lt_list, name, lt_name, labels.global).arg(count));
		}
		for (const auto &[name, count] : change.emptiedLists) {
			lines.push_back(tr::lng_purple_lists_emptied(tr::now, lt_account, labels.own, lt_list, name));
		}
		const auto opening = AccountSettingsOpenTargetFor(session, SettingsOpenRole::Own);
		const auto weakSession = base::make_weak(session);
		const auto weakWindow = base::make_weak(window);
		const auto current = [=] {
			const auto live = weakSession.get();
			return live && weakWindow && Core::App().activeWindow() == weakWindow.get()
				&& !live->account().loggingOut() && live->account().maybeSession() == live
				&& SessionAccountKey(live) == change.account;
		};
		auto text = tr::marked(lines.join('\n'));
		if (opening) {
			text.append(u" "_q).append(tr::lng_purple_lists_open_own(tr::now, lt_account, tr::marked(labels.own), tr::link));
		}
		window->showToast(Ui::Toast::Config{
			.text = std::move(text),
			.filter = [=](const ClickHandlerPtr &, Qt::MouseButton) {
				if (!opening || !current()) { return false; }
				const auto ready = PrepareAccountSettingsOpen(weakSession.get(), *opening, current);
				if (!ready.target || !current()) { return false; }
				const auto target = *ready.target;
				crl::on_main([=] {
					if (current() && ValidateAccountSettingsOpenTarget(weakSession.get(), target)) {
						Ui::PreventDelayedActivation();
						Platform::File::UnsafeLaunch(target.file.path);
					}
				});
				return false;
			},
			.duration = kAccountListNoticeDuration,
		});
	}
}

}

QString AccountSettingsScopeCaption(
		not_null<Main::Session*> session, std::optional<AccountSettingsRow> row) {
	if (session->account().loggingOut() || session->account().maybeSession() != session.get()) {
		return {};
	}
	const auto labels = SettingsRowLabels(session, row);
	const auto global = AppWideSettingsAccount(session);
	if (!labels.multiple || !global) { return {}; }
	if (!row) {
		return SessionAccountKey(session) == *global ? QString()
			: labels.globalSignedIn
			? tr::lng_purple_settings_scope_appwide(tr::now, lt_account, labels.global)
			: tr::lng_purple_settings_scope_appwide_offline(tr::now, lt_account, labels.global);
	}
	if (*row == AccountSettingsRow::SendAfterSave) {
		return tr::lng_purple_settings_scope_file(tr::now, lt_account, labels.own);
	}
	if (SessionAccountKey(session) != *global) {
		return AccountRouting().rowState(SessionAccountKey(session), *row).pinned
			? tr::lng_purple_settings_scope_pinned(tr::now, lt_account, labels.own)
			: tr::lng_purple_settings_scope_inherited(tr::now, lt_account, labels.global);
	}
	auto result = tr::lng_purple_settings_scope_default(tr::now, lt_account, labels.global);
	for (const auto &name : labels.overrides) {
		result += ' ' + tr::lng_purple_settings_scope_override(tr::now, lt_account, name);
	}
	return result;
}

std::optional<AccountAppWideRemovalTarget> AccountAppWideRemovalFor(Main::Session *session) {
	if (!session || session->account().loggingOut() || session->account().maybeSession() != session) {
		return std::nullopt;
	}
	return PreviewAccountAppWideRemoval(AccountRouting(), SessionAccountKey(session));
}

namespace {

QString AppWideRemovalKeys(const AccountAppWideRemovalTarget &target) {
	auto keys = QStringList();
	for (const auto &key : target.removed) {
		const auto address = key.addressKind == ConfigChangeAddressKind::Key
			? key.table + '.' + (key.key.isEmpty() ? u"\"\""_q : key.key)
			: key.table + (key.addressKind == ConfigChangeAddressKind::ListDefinition
				? u" (definition)"_q : QString());
		keys.push_back(address);
	}
	return keys.join(u", "_q);
}

}

QString AccountWorkModeTitle(not_null<Main::Session*> session) {
	const auto labels = SettingsRowLabels(session, std::nullopt);
	return SessionAccountKey(session) == AppWideSettingsAccount(session)
		? tr::lng_purple_lists_global_title(tr::now, lt_account, labels.own)
		: tr::lng_purple_lists_mode_title(tr::now, lt_account, labels.own);
}

bool AccountWorkModeChoiceVisible(not_null<Main::Session*> session) {
	return SettingsRowLabels(session, std::nullopt).multiple;
}

namespace {

std::map<QString, OwnListMembers> AccountOwnLists(not_null<Main::Session*> session) {
	const auto key = SessionAccountKey(session);
	(void)AccountSettings(session);
	const auto report = AccountRouting().report(key);
	const auto paths = AccountRouting().files(key);
	if (!paths || !report || !report->account.usable) { return {}; }
	const auto path = paths->settings + (report->account.usedGood ? u".good"_q : QString());
	return ReadOwnListMembers(ReadSettingsFileInput(path).bytes, path);
}

}

AccountListPreviewNotes CaptureOwnListPreviewNotes(
		not_null<Main::Session*> session) {
	const auto labels = SettingsRowLabels(session, std::nullopt);
	const auto mode = AccountWorkModeForSession(session);
	const auto global = SessionAccountKey(session) == AppWideSettingsAccount(session);
	return [=](const QByteArray &before, const QByteArray &after) {
		const auto count = InactiveFollowListMemberChanges(before, after, mode, global);
		return count ? tr::lng_purple_lists_follow_inactive(tr::now,
			lt_account, labels.own, lt_name, labels.global).arg(count) : QString();
	};
}

bool AllAccountsWorkModeStock() {
	auto count = 0;
	for (const auto &entry : Core::App().domain().accounts()) {
		const auto live = entry.account->maybeSession();
		if (!live || live->account().loggingOut()) { continue; }
		if (AccountWorkModeForSession(live) != AccountWorkMode::Stock) { return false; }
		++count;
	}
	return count > 0;
}

QString AccountListsSummary(not_null<Main::Session*> session) {
	const auto labels = SettingsRowLabels(session, std::nullopt);
	const auto key = SessionAccountKey(session);
	const auto mode = AccountWorkModeForSession(session);
	if (mode == AccountWorkMode::Stock) {
		return tr::lng_purple_lists_stock(tr::now, lt_account, labels.own);
	}
	if (AppWideSettingsAccount(session) == key) { return {}; }
	if (mode == AccountWorkMode::Follow) {
		return tr::lng_purple_lists_follow_summary(tr::now, lt_account, labels.own, lt_name, labels.global);
	}
	const auto own = AccountOwnLists(session);
	auto count = 0;
	for (const auto &[name, list] : own) {
		if (list.hasMembers && ActiveSettings().list(name)) { ++count; }
	}
	return tr::lng_purple_lists_own_summary(tr::now, lt_account, labels.own, lt_name, labels.global).arg(count);
}

QString AccountListFileWarnings(not_null<Main::Session*> session) {
	const auto labels = SettingsRowLabels(session, std::nullopt);
	const auto key = SessionAccountKey(session);
	const auto paths = AccountRouting().files(key);
	if (!paths || AppWideSettingsAccount(session) == key) { return {}; }
	const auto own = AccountOwnLists(session);
	auto lines = QStringList();
	auto followed = 0;
	for (const auto &[name, list] : own) {
		if (!list.hasMembers) { continue; }
		if (!ActiveSettings().list(name)) {
			lines.push_back(tr::lng_purple_lists_undefined(tr::now, lt_account, labels.own,
				lt_list, name, lt_name, labels.global).arg(list.members.size()));
		} else { ++followed; }
	}
	if (AccountWorkModeForSession(session) == AccountWorkMode::Follow && followed) {
		lines.push_back(tr::lng_purple_lists_follow_inactive(tr::now,
			lt_account, labels.own, lt_name, labels.global).arg(followed));
	}
	return lines.join('\n');
}

QString AccountListEditNotice(
		not_null<Main::Session*> session,
		const AccountListEditTarget &target,
		bool member) {
	const auto labels = SettingsRowLabels(session, std::nullopt);
	const auto key = SessionAccountKey(session);
	const auto mode = AccountWorkModeForSession(session);
	auto result = QString();
	if (key != target.edit.destination.global && mode == AccountWorkMode::Follow) {
		result = member
			? tr::lng_purple_lists_follow_added(tr::now, lt_name, labels.global,
				lt_list, target.list, lt_account, labels.own)
			: tr::lng_purple_lists_follow_removed(tr::now, lt_name, labels.global,
				lt_list, target.list, lt_account, labels.own);
	} else if (key != target.edit.destination.global) {
		const auto own = ReadOwnListMembers(target.edit.destination.file.input.bytes, target.edit.destination.file.path);
		const auto found = own.find(target.list);
		if (found == own.end() || !found->second.hasMembers) {
			result = tr::lng_purple_lists_forked(tr::now, lt_list, target.list,
				lt_account, labels.own, lt_name, labels.global);
		}
	}
	if (mode == AccountWorkMode::Stock) {
		if (!result.isEmpty()) { result += '\n'; }
		result += tr::lng_purple_lists_stock_edit(tr::now, lt_account, labels.own);
	}
	return result;
}

std::vector<AccountListDeleteTarget> AccountListDeletes(not_null<Main::Session*> session) {
	auto result = std::vector<AccountListDeleteTarget>();
	for (const auto &list : AccountSettings(session).lists) {
		const auto target = AccountRouting().listDeleteTarget(SessionAccountKey(session), list.name);
		if (target) { result.push_back(*target); }
	}
	return result;
}

void DeleteAccountListMembersBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		AccountListDeleteTarget target) {
	const auto origin = base::make_weak(session.get());
	const auto weakBox = QPointer<Ui::GenericBox>(box.get());
	const auto labels = SettingsRowLabels(session, std::nullopt);
	const auto stock = AccountWorkModeForSession(session) == AccountWorkMode::Stock;
	const auto current = [=] {
		const auto live = origin.get();
		const auto window = Core::App().activeWindow();
		return weakBox && live && !live->account().loggingOut() && live->account().maybeSession() == live
			&& SessionAccountKey(live) == target.edit.destination.requester
			&& ConfigDirectory() == target.edit.destination.root
			&& window && window->maybeSession() == live;
	};
	box->setTitle(tr::lng_purple_lists_delete());
	box->addRow(object_ptr<Ui::FlatLabel>(box, tr::lng_purple_lists_delete_confirm(tr::now,
		lt_account, labels.own, lt_list, target.list, lt_name, labels.global)));
	box->addButton(tr::lng_purple_lists_delete(), [=] {
		if (!current()) { return; }
		Instance().observeAccountFiles();
		const auto result = AccountRouting().deleteListMembers(target, current);
		if (!current()) { return; }
		if (!result.ok) { box->uiShow()->showToast(result.error); return; }
		Instance().refreshAccountRows();
		Instance().observeAccountFiles();
		if (!current()) { return; }
		if (!result.target || !AccountRouting().validateSettingsEdit(*result.target)) {
			box->uiShow()->showToast(tr::lng_purple_settings_row_changed(tr::now));
			return;
		}
		if (stock) {
			box->uiShow()->showToast(tr::lng_purple_lists_stock_edit(tr::now, lt_account, labels.own));
		}
		if (current()) { box->closeBox(); }
	});
	const auto cancel = box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	box->setFocusCallback([=] { cancel->setFocus(); });
}

void ChooseAccountWorkModeBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto origin = base::make_weak(&controller->session());
	const auto window = base::make_weak(controller.get());
	const auto weakBox = QPointer<Ui::GenericBox>(box.get());
	const auto key = SessionAccountKey(&controller->session());
	const auto target = AccountRouting().workModeTarget(key);
	const auto labels = SettingsRowLabels(&controller->session(), std::nullopt);
	const auto current = [=] {
		const auto live = origin.get();
		const auto ownWindow = window.get();
		return target && weakBox && live && ownWindow && &ownWindow->session() == live
			&& !live->account().loggingOut() && live->account().maybeSession() == live
			&& SessionAccountKey(live) == key && ConfigDirectory() == target->own.root;
	};
	box->setTitle(rpl::single(AccountWorkModeTitle(&controller->session())));
	const auto group = std::make_shared<Ui::RadiobuttonGroup>();
	const auto modes = key == AppWideSettingsAccount(&controller->session())
		? std::vector{ AccountWorkMode::Own, AccountWorkMode::Stock }
		: std::vector{ AccountWorkMode::Own, AccountWorkMode::Follow, AccountWorkMode::Stock };
	for (const auto mode : modes) {
		const auto title = mode == AccountWorkMode::Stock ? tr::lng_purple_lists_mode_stock(tr::now)
			: mode == AccountWorkMode::Follow ? tr::lng_purple_lists_mode_follow(tr::now, lt_name, labels.global)
			: key == AppWideSettingsAccount(&controller->session()) ? tr::lng_purple_lists_mode_on(tr::now)
			: tr::lng_purple_lists_mode_own(tr::now, lt_account, labels.own, lt_name, labels.global);
		box->addRow(object_ptr<Ui::Radiobutton>(box, group, int(mode), title));
	}
	group->setValue(int(AccountWorkModeForSession(&controller->session())));
	box->addButton(tr::lng_box_ok(), [=] {
		if (!current()) { return; }
		const auto result = AccountRouting().writeWorkMode(*target, AccountWorkMode(group->current()), current);
		if (!current()) { return; }
		if (!result.ok) { box->uiShow()->showToast(result.error); return; }
		Instance().refreshAccountRows();
		if (!current() || !result.target || !AccountRouting().validateWorkModeTarget(*result.target)) { return; }
		const auto summary = AccountListsSummary(origin.get());
		if (!summary.isEmpty()) { window.get()->showToast(summary); }
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

QString InactiveAppWideSettingsSummary(Main::Session *session) {
	const auto target = AccountAppWideRemovalFor(session);
	if (!target || target->removed.empty()) { return {}; }
	const auto labels = SettingsRowLabels(session, std::nullopt);
	return tr::lng_purple_remove_appwide_summary(tr::now,
		lt_count, int(target->removed.size()), lt_account, labels.own,
		lt_name, labels.global, lt_text, AppWideRemovalKeys(*target));
}

QString RemoveAccountAppWideSettingsTitle(Main::Session *session) {
	return session ? tr::lng_purple_remove_appwide_title(tr::now,
		lt_account, session->user()->name()) : QString();
}

void RemoveAccountAppWideSettingsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		AccountAppWideRemovalTarget target) {
	const auto origin = base::make_weak(&controller->session());
	const auto key = SessionAccountKey(&controller->session());
	const auto window = base::make_weak(controller.get());
	const auto confirmation = QPointer<Ui::GenericBox>(box.get());
	const auto current = [=] {
		const auto live = origin.get();
		const auto ownWindow = window.get();
		return confirmation && live && ownWindow && &ownWindow->session() == live
			&& !live->account().loggingOut() && live->account().maybeSession() == live
			&& SessionAccountKey(live) == key && target.destination.requester == key;
	};
	box->setTitle(tr::lng_purple_remove_appwide_action());
	const auto name = controller->session().user()->name();
	box->addRow(object_ptr<Ui::FlatLabel>(box, rpl::single(
		tr::lng_purple_remove_appwide_confirm(tr::now, lt_account, name)
			+ u"\n\n"_q + AppWideRemovalKeys(target))));
	const auto cancel = box->addButton(tr::lng_cancel(), [=] {
		if (confirmation) { confirmation->closeBox(); }
	});
	box->setFocusCallback([=] { if (cancel) { cancel->setFocus(); } });
	box->addButton(tr::lng_purple_remove_appwide_action(), [=] {
		if (!current()) { return; }
		auto result = CommitAccountAppWideRemoval(AccountRouting(), target, current);
		if (result.changed) {
			Instance().refreshAccountRows();
			if (!current() || !ValidateCommittedAppWideRemoval(AccountRouting(), target, result)) { result.ok = false; }
		}
		if (!current()) { return; }
		if (result.ok) { confirmation->closeBox(); }
		else {
			window.get()->show(Ui::MakeInformBox(result.changed
				? tr::lng_purple_remove_appwide_saved_incomplete(tr::now)
				: result.backupChanged
				? tr::lng_purple_remove_appwide_backup_only(tr::now)
				: tr::lng_purple_remove_appwide_unavailable(tr::now)));
		}
	});
}

QString AccountSettingsResetCaption(not_null<Main::Session*> session) {
	return tr::lng_purple_settings_scope_reset(tr::now, lt_account,
		SettingsRowLabels(session, std::nullopt).global);
}

bool AccountSettingsResetVisible(not_null<Main::Session*> session, AccountSettingsRow row) {
	return row != AccountSettingsRow::SendAfterSave
		&& SettingsRowLabels(session, row).multiple
		&& AccountSettingsRowFor(session, row).pinned;
}

namespace {

struct GlobalChoiceOption {
	AccountKey key;
	QString name;
	base::weak_ptr<Main::Session> session;
	bool signedIn = false;
};

std::vector<GlobalChoiceOption> GlobalChoiceOptions() {
	auto result = std::vector<GlobalChoiceOption>();
	for (const auto &entry : Core::App().domain().accounts()) {
		const auto live = entry.account->maybeSession();
		if (!live || entry.account->loggingOut()) { continue; }
		const auto key = SessionAccountKey(live);
		auto name = live->user()->name();
		if (key.environment == 1) { name += tr::lng_purple_global_test_servers(tr::now); }
		result.push_back({ key, name, base::make_weak(live), true });
	}
	if (const auto global = AccountRouting().globalAccount()) {
		if (std::none_of(result.begin(), result.end(), [&](const GlobalChoiceOption &entry) { return entry.key == *global; })) {
			auto name = tr::lng_purple_settings_signed_out_global(tr::now, lt_account,
				AccountKeyText(global->environment, global->userId).value_or(QString()));
			if (global->environment == 1) { name += tr::lng_purple_global_test_servers(tr::now); }
			result.push_back({ *global, name, {}, false });
		}
	}
	return result;
}

bool GlobalChoiceRosterCurrent(const std::vector<GlobalChoiceOption> &options) {
	const auto current = GlobalChoiceOptions();
	auto before = std::vector<const GlobalChoiceOption*>();
	auto after = std::vector<const GlobalChoiceOption*>();
	for (const auto &entry : options) { if (entry.signedIn) { before.push_back(&entry); } }
	for (const auto &entry : current) { if (entry.signedIn) { after.push_back(&entry); } }
	if (before.size() != after.size()) { return false; }
	for (auto i = size_t(0); i != before.size(); ++i) {
		if (before[i]->key != after[i]->key || before[i]->session.get() != after[i]->session.get()) { return false; }
	}
	return true;
}

QString GlobalChoiceSummary(const GlobalAccountChoicePreview &preview, const QString &name) {
	const auto schedule = [](const Settings &settings) {
		return settings.schedule.enabled && (!settings.schedule.rules.empty() || !settings.schedule.rulesets.empty());
	};
	const auto onOff = [](bool on) { return on ? tr::lng_purple_global_on(tr::now) : tr::lng_purple_global_off(tr::now); };
	auto result = tr::lng_purple_global_preview(tr::now, lt_account, name)
		+ '\n' + tr::lng_purple_global_lists(tr::now).arg(preview.before.lists.size()).arg(preview.after.lists.size())
		+ '\n' + tr::lng_purple_global_presets(tr::now).arg(preview.before.presets.size()).arg(preview.after.presets.size())
		+ '\n' + tr::lng_purple_global_schedule(tr::now).arg(onOff(schedule(preview.before)), onOff(schedule(preview.after)))
		+ '\n' + tr::lng_purple_global_premium(tr::now).arg(onOff(preview.before.premium.enabled), onOff(preview.after.premium.enabled));
	if (preview.noAppWideSettings) { result += '\n' + tr::lng_purple_global_no_settings(tr::now, lt_account, name); }
	if (preview.selectedUsesGood) { result += '\n' + tr::lng_purple_global_good(tr::now); }
	if (preview.enterNormal) { result += '\n' + tr::lng_purple_global_normal(tr::now, lt_account, name).arg(preview.runningPreset); }
	return result;
}

}

QString GlobalAccountChoiceTitle(not_null<Main::Session*> session) {
	const auto global = AppWideSettingsAccount(session);
	if (!global) { return tr::lng_purple_global_unavailable(tr::now); }
	const auto options = GlobalChoiceOptions();
	const auto found = std::find_if(options.begin(), options.end(), [&](const GlobalChoiceOption &entry) { return entry.key == *global; });
	return found != options.end() ? tr::lng_purple_global_source(tr::now, lt_account, found->name)
		: tr::lng_purple_global_unavailable(tr::now);
}

bool GlobalAccountChoiceVisible(not_null<Main::Session*> session) {
	const auto global = AppWideSettingsAccount(session);
	if (!global) { return false; }
	const auto options = GlobalChoiceOptions();
	const auto count = std::count_if(options.begin(), options.end(), [](const GlobalChoiceOption &entry) { return entry.signedIn; });
	const auto current = std::find_if(options.begin(), options.end(), [&](const GlobalChoiceOption &entry) { return entry.key == *global; });
	return ShouldShowGlobalAccountChoice(int(count), current != options.end() && current->signedIn);
}

void ChooseGlobalAccountBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto origin = base::make_weak(&controller->session());
	const auto originKey = SessionAccountKey(&controller->session());
	const auto window = base::make_weak(controller.get());
	const auto picker = QPointer<Ui::GenericBox>(box.get());
	const auto options = GlobalChoiceOptions();
	const auto current = [=] {
		const auto live = origin.get();
		const auto ownWindow = window.get();
		return picker && live && ownWindow && &ownWindow->session() == live
			&& !live->account().loggingOut() && live->account().maybeSession() == live
			&& SessionAccountKey(live) == originKey && GlobalChoiceRosterCurrent(options);
	};
	box->setTitle(tr::lng_purple_global_choose());
	const auto group = std::make_shared<Ui::RadiobuttonGroup>();
	const auto global = AccountRouting().globalAccount();
	for (auto i = size_t(0); i != options.size(); ++i) {
		box->addRow(object_ptr<Ui::Radiobutton>(box, group, int(i), options[i].name));
		if (global == options[i].key) { group->setValue(int(i)); }
	}
	box->addButton(tr::lng_purple_global_preview_action(), [=] {
		if (!current()) { return; }
		if (!group->hasValue()) { return; }
		const auto index = group->current();
		if (index < 0 || index >= int(options.size())) { return; }
		const auto live = origin.get();
		const auto state = SerializeState(CurrentState());
		auto preview = AccountRouting().previewGlobalChoice(SessionAccountKey(live), options[index].key, CurrentState().activePreset);
		if (!current()) { return; }
		if (!preview || SerializeState(CurrentState()) != state) {
			window.get()->show(Ui::MakeInformBox(tr::lng_purple_global_unavailable(tr::now)));
			return;
		}
		preview->activationState = state;
		const auto labels = GlobalChoiceOptions();
		const auto selected = std::find_if(labels.begin(), labels.end(), [&](const GlobalChoiceOption &entry) {
			return entry.key == preview->selected.file.account;
		});
		if (selected == labels.end() || !current() || !AccountRouting().validateGlobalChoice(*preview)) { return; }
		const auto label = selected->name;
		window.get()->show(Box([=](not_null<Ui::GenericBox*> confirm) {
			const auto confirmation = QPointer<Ui::GenericBox>(confirm.get());
			confirm->setTitle(tr::lng_purple_global_confirm());
			confirm->addRow(object_ptr<Ui::FlatLabel>(confirm, rpl::single(GlobalChoiceSummary(*preview, label))));
			confirm->addButton(tr::lng_purple_global_confirm(), [=] {
				if (!confirmation || !current()) { return; }
				const auto result = Instance().switchGlobalAccount(*preview, [=] { return confirmation && current(); });
				if (!confirmation || !current()) { return; }
				if (result.ok) { confirmation->closeBox(); if (picker) { picker->closeBox(); } }
				else {
					window.get()->show(Ui::MakeInformBox(result.changed
						? tr::lng_purple_global_saved_activation_incomplete(tr::now)
						: tr::lng_purple_settings_row_changed(tr::now)));
				}
			});
			confirm->addButton(tr::lng_cancel(), [=] { confirm->closeBox(); });
		}), Ui::LayerOption::KeepOther);
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

std::optional<AccountKey> SettingsStartupOwner() {
	return AccountRouting().owner();
}

QString SettingsOwnerStatus() {
	const auto &plan = AccountRouting().ownerPlan();
	if (!plan.error.isEmpty()) {
		return plan.error;
	} else if (plan.restoredHandEditedOwnerNotice) {
		const auto owner = AccountRouting().owner();
		return owner ? u"root_file_account in local.toml was changed by hand and has been put back to %1"_q
			.arg(*AccountKeyText(owner->environment, owner->userId)) : QString();
	}
	const auto owner = AccountRouting().owner();
	return owner ? *AccountKeyText(owner->environment, owner->userId)
		: u"settings.toml has no owner on this device · Choose"_q;
}

void ChooseSettingsOwnerBox(not_null<Ui::GenericBox*> box) {
	RefreshDesktopAccounts();
	box->setTitle(rpl::single(u"settings.toml"_q));
	box->addRow(object_ptr<Ui::FlatLabel>(box, rpl::single(SettingsOwnerStatus())));
	AccountRouting().acknowledgeOwnerNotice();
	if (!AccountRouting().owner()) {
		auto &domain = Core::App().domain();
		for (const auto &entry : domain.accounts()) {
			const auto session = entry.account->maybeSession();
			if (!session || entry.account->loggingOut()) {
				continue;
			}
			const auto key = SessionAccountKey(session);
			const auto text = AccountKeyText(key.environment, key.userId);
			if (!text || QFileInfo::exists(ConfigDirectory() + u"/accounts/"_q + *text)) {
				continue;
			}
			const auto weak = base::make_weak(session);
			box->addButton(rpl::single(session->user()->name() + u" ("_q + *text + u")"_q), [=] {
				const auto live = weak.get();
				if (!live || live->account().loggingOut()
					|| live->account().maybeSession() != live
					|| SessionAccountKey(live) != key) {
					box->closeBox();
					return;
				}
				RefreshDesktopAccounts();
				auto accounts = std::vector<StartupAccountCandidate>();
				for (const auto &candidate : Core::App().domain().accounts()) {
					if (const auto current = candidate.account->maybeSession()) {
						const auto id = SessionAccountKey(current);
						const auto canonical = AccountKeyText(id.environment, id.userId);
						if (canonical && !candidate.account->loggingOut()) {
							accounts.push_back({ id, QFileInfo::exists(ConfigDirectory()
								+ u"/accounts/"_q + *canonical), false });
						}
					}
				}
				AccountRouting().start(std::move(accounts), key);
				box->closeBox();
			});
		}
	}
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace Purple
