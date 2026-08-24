/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_config.h"

#include "base/timer.h"
#include "purple/purple_readme.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QSaveFile>

namespace Purple {
namespace {

// Editors write a file in several steps, and some replace it wholesale. Wait
// for the flurry to settle rather than reloading a half-written file - which
// would parse as broken and flash an error banner on every save.
constexpr auto kReloadDelay = crl::time(250);

// Written once, on first run. Everything the user may want to change should be
// present and commented, because an empty file teaches nobody what is possible.
constexpr auto kStarterSettings = R"([premium]
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
# # An extra tab of its own, with its own unread badge and its own pins.
# [[presets.work.views]]
# name = "People"
# list_order = [ { list = "private" } ]

[schedule]
enabled_p = true

# Disabled until you point it at a preset you have actually written.
[[schedule.rules]]
enabled_p = false
days      = ["mon", "tue", "wed", "thu", "fri"]
from      = "09:00"
to        = "17:00"
preset    = "work"

[peek]
# Temporarily reveal what the active preset is hiding.
hotkey   = "Ctrl+Shift+P"
auto_off = "2m"

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

// QSaveFile writes a temporary alongside the target and renames over it, so a
// crash mid-write cannot leave a truncated config behind.
[[nodiscard]] bool WriteFile(const QString &path, const QString &text) {
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
		const SpliceResult &result,
		const QString &what);

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

};

Config::Config() : _reload([=] { reloadFromDisk(); }) {
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
	if (WriteFile(path, starter)) {
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
	} else if (!WriteFile(SettingsFilePath(), text)) {
		return false;
	}
	applyText(text);
	return true;
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
	const auto result = SetTableBool(
		_text,
		SettingsFilePath(),
		u"premium"_q,
		u"enabled_p"_q,
		value);
	if (!splice(result, u"the Premium toggle"_q)) {
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

bool Config::splice(const SpliceResult &result, const QString &what) {
	if (!result.ok()) {
		LOG(("Purple Error: Could not write %1 to %2: %3."
			).arg(what, SettingsFilePath(), result.error));
		return false;
	} else if (!result.changed) {
		return true;
	}
	return writeSettings(result.text);
}

bool Config::addToList(
		const QString &list,
		PeerIdValue id,
		const MemberTitle &title) {
	return splice(
		AddListMember(_text, SettingsFilePath(), list, id, title),
		u"list '%1'"_q.arg(list));
}

bool Config::removeFromList(
		const QString &list,
		PeerIdValue id,
		const MemberTitle &title) {
	return splice(
		RemoveListMember(_text, SettingsFilePath(), list, id, title),
		u"list '%1'"_q.arg(list));
}

bool Config::setViewPins(
		const QString &preset,
		const QString &view,
		const std::vector<PeerIdValue> &ids,
		const MemberTitle &title) {
	return splice(
		SetViewPinned(_text, SettingsFilePath(), preset, view, ids, title),
		u"the pins of view '%1'"_q.arg(view));
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
		|| !WriteFile(StateFilePath(), _stateText)) {
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
