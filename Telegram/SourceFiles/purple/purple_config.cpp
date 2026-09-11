/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_config.h"

#include "base/timer.h"
#include "purple/purple_readme.h"
#include "purple/purple_sync.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QSaveFile>

namespace Purple {

bool WriteConfigFile(const QString &path, const QString &text) {
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		LOG(("Purple Error: Could not write %1.").arg(path));
		return false;
	}
	file.write(text.toUtf8());
	if (!file.commit()) {
		LOG(("Purple Error: Could not commit %1.").arg(path));
		return false;
	}
	return true;
}

namespace {

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
# Temporarily reveal what the active preset is hiding.
hotkey   = "Ctrl+Shift+E"
auto_off = "2m"

# How long each way of starting one lasts, when they want to differ: tap is
# what the checkbox and the chips in the Work Mode box start, hotkey_length
# what the key starts. Unset means "whatever auto_off says"; "off" is a peek
# with no clock on it. Pressing the key again while one is running adds another
# hotkey_length, up to an hour.
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
class Config final {
public:
	Config();

	[[nodiscard]] bool localPremium() const;
	[[nodiscard]] rpl::producer<bool> localPremiumValue() const;
	void setLocalPremium(bool value);

	[[nodiscard]] const Settings &settings() const;
	[[nodiscard]] const Problems &problems() const;
	[[nodiscard]] rpl::producer<> changes() const;

	bool addToList(
		const QString &list,
		PeerIdValue id,
		const MemberTitle &title);
	bool removeFromList(
		const QString &list,
		PeerIdValue id,
		const MemberTitle &title);
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
		const QString &what);
	void noteSent(const QByteArray &bytes);
	void noteImported(const QByteArray &bytes);

	[[nodiscard]] const State &state() const;
	[[nodiscard]] rpl::producer<> stateChanges() const;
	void updateState(Fn<void(State&)> apply);

private:
	void loadSettings();
	void loadState();
	void applyText(const QString &text);
	[[nodiscard]] bool writeSettings(const QString &text);
	void startWatching();
	void reloadFromDisk();
	void reloadStateFromDisk();
	[[nodiscard]] bool splice(
		const Fn<SpliceResult(const QString&)> &op,
		const QString &what);
	void autoSendLater();
	void autoSendNow();

	QString _text;
	Settings _settings;
	Problems _problems;

	State _state;
	QString _stateText;

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
}

void Config::loadSettings() {
	const auto path = SettingsFilePath();
	if (auto text = ReadFile(path)) {
		applyText(*text);
		return;
	} else if (!QDir().mkpath(ConfigDirectory())) {
		LOG(("Purple Error: Could not create %1.").arg(ConfigDirectory()));
		applyText(QString());
		return;
	}
	const auto starter = QString::fromUtf8(kStarterSettings);
	if (WriteConfigFile(path, starter)) {
		applyText(starter);
	} else {
		applyText(QString());
	}
}

void Config::applyText(const QString &text) {
	_text = text;
	auto parsed = ParseSettings(text, SettingsFilePath());
	_problems.warnings = std::move(parsed.warnings);
	_problems.error = parsed.error;
	for (const auto &warning : _problems.warnings) {
		LOG(("Purple Warning: %1").arg(warning));
	}
	if (!parsed.ok()) {
		// Keep the last good settings. A file that stops parsing mid-edit
		// should not reshuffle the chat list under the user's hands.
		LOG(("Purple Error: %1: %2.").arg(SettingsFilePath(), parsed.error));
		_changes.fire({});
		return;
	}
	_settings = std::move(parsed.settings);

	// One line per load, so "the toggle looks off" can be answered from the log
	// instead of from a rebuild. The file is hand-edited and lives outside
	// tdata, so which one we actually read is worth stating too.
	LOG(("Purple: %1, %2 lists, %3 presets, read from %4."
		).arg(_settings.premium.enabled
			? u"local premium ON"_q
			: u"local premium OFF"_q
		).arg(_settings.lists.size()
		).arg(_settings.presets.size()
		).arg(SettingsFilePath()));

	_localPremium = _settings.premium.enabled;
	_changes.fire({});
}

bool Config::writeSettings(const QString &text) {
	if (!QDir().mkpath(ConfigDirectory())) {
		LOG(("Purple Error: Could not create %1.").arg(ConfigDirectory()));
		return false;
	} else if (!WriteConfigFile(SettingsFilePath(), text)) {
		return false;
	}
	applyText(text);

	// Every write the app makes to the file passes through here, which is the
	// whole reason there is one of these rather than a QSaveFile in each
	// writer: the automatic send has exactly one place to hook, and a switch
	// added next year gets it for free. Writes from outside - an editor, an
	// import - come in through the watcher instead and are deliberately not
	// this. See ShouldAutoSend() in purple_state.h.
	autoSendLater();
	return true;
}

void Config::autoSendLater() {
	if (ShouldAutoSend(_settings, _state, _text.toUtf8(), false)) {
		// Restarted rather than left to run, so the clock measures the quiet
		// after the last write instead of the noise after the first.
		_autoSend.callOnce(kAutoSendDelay);
	}
}

void Config::autoSendNow() {
	// Asked again rather than trusted from five seconds ago. In between, the
	// switch can have been turned off, the file can have been put back to what
	// it was, and an import can have landed the very bytes we were about to
	// offer the machine that sent them.
	const auto bytes = _text.toUtf8();
	if (!ShouldAutoSend(_settings, _state, bytes, false)) {
		return;
	} else if (!Upload(bytes, _settings.version)) {
		// No session to post into - not signed in yet, or signed out since.
		// Nothing is recorded, so the next write tries again, which is what
		// somebody who turned this on would expect over a silent giving up.
		LOG(("Purple: nothing to send settings.toml to, not sending."));
		return;
	}
	noteSent(bytes);
}

void Config::noteSent(const QByteArray &bytes) {
	const auto fingerprint = SettingsFingerprint(bytes);
	updateState([&](State &state) {
		state.lastSentFingerprint = fingerprint;
	});
}

void Config::noteImported(const QByteArray &bytes) {
	const auto fingerprint = SettingsFingerprint(bytes);
	updateState([&](State &state) {
		state.lastImportedFingerprint = fingerprint;
	});
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
	if (_watcher
		&& !_watcher->files().contains(SettingsFilePath())
		&& QFileInfo::exists(SettingsFilePath())) {
		// Re-arm after a rename-on-save replaced the file we were watching.
		_watcher->addPath(SettingsFilePath());
	}
	reloadStateFromDisk();

	auto text = ReadFile(SettingsFilePath());
	if (!text || *text == _text) {
		// Our own writes come back through the watcher too.
		return;
	}
	applyText(*text);
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
		const QString &what) {
	Expects(op != nullptr);

	const auto result = op(_text);
	if (!result.ok()) {
		LOG(("Purple Error: Could not write %1 to %2: %3."
			).arg(what, SettingsFilePath(), result.error));
		return { .error = result.error };
	} else if (!result.changed) {
		// The file already said that. Not an error and not worth a message:
		// the screen that asked is now showing what the file holds, which is
		// what it wanted.
		return { .ok = true };
	} else if (!writeSettings(result.text)) {
		return { .error = u"Could not write %1. See the log."_q.arg(
			SettingsFilePath()) };
	}
	return { .ok = true };
}

bool Config::splice(
		const Fn<SpliceResult(const QString&)> &op,
		const QString &what) {
	return write(op, what).ok;
}

bool Config::addToList(
		const QString &list,
		PeerIdValue id,
		const MemberTitle &title) {
	return splice([&](const QString &text) {
		return AddListMember(text, SettingsFilePath(), list, id, title);
	}, u"list '%1'"_q.arg(list));
}

bool Config::removeFromList(
		const QString &list,
		PeerIdValue id,
		const MemberTitle &title) {
	return splice([&](const QString &text) {
		return RemoveListMember(text, SettingsFilePath(), list, id, title);
	}, u"list '%1'"_q.arg(list));
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
	_stateChanges.fire({});
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
	return ConfigDirectory() + u"/settings.toml"_q;
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

const Problems &SettingsProblems() {
	return Instance().problems();
}

rpl::producer<> SettingsChanges() {
	return Instance().changes();
}

bool AddToList(
		const QString &list,
		PeerIdValue id,
		const MemberTitle &title) {
	return Instance().addToList(list, id, title);
}

bool RemoveFromList(
		const QString &list,
		PeerIdValue id,
		const MemberTitle &title) {
	return Instance().removeFromList(list, id, title);
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

void NoteSettingsSent(const QByteArray &bytes) {
	Instance().noteSent(bytes);
}

void NoteSettingsImported(const QByteArray &bytes) {
	Instance().noteImported(bytes);
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

} // namespace Purple
