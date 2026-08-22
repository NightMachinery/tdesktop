/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>

#include <optional>
#include <vector>

// The parsed form of settings.toml. Deliberately free of every tdesktop
// dependency beyond base/ and Qt Core, so purple/test_config.sh can compile it
// on its own and drive it against fixture files - the file is hand-owned, so
// the parser has to survive anything a text editor can produce, and that is far
// easier to prove outside a running app.
namespace Purple {

// Peer ids exactly as written in the file. Turning these into tdesktop PeerIds
// is the engine's job; the parser stays ignorant of what a peer is.
using PeerIdValue = int64;

enum class ListKind : uchar {
	Custom,
	Private,
	Groups,
	Channels,
	Bots,
};

[[nodiscard]] bool IsCatchAll(ListKind kind);

// The four catch-alls in their canonical priority order, bottom of the list.
[[nodiscard]] const std::vector<QString> &CatchAllNames();
[[nodiscard]] ListKind CatchAllKind(const QString &name);

struct List {
	QString name;
	QString title;
	bool show = true;
	bool notify = true;
	bool locked = false;
	ListKind kind = ListKind::Custom;

	// Empty for catch-alls, which match by chat type instead.
	std::vector<PeerIdValue> members;
};

// Absent fields inherit from the parent preset, and from the list defaults at
// the root of the chain. Tri-state throughout, so "explicitly false" and "not
// mentioned" stay distinguishable all the way down.
struct ListOverride {
	QString list;
	std::optional<bool> show;
	std::optional<bool> notify;
	std::optional<bool> groupsRequireMention;
};

struct PresetFolder {
	QString name;
	std::optional<bool> notify;

	// `filtered = false' exempts the folder's chats from the preset's hiding.
	// Nothing means filtered, which is what every other folder is.
	std::optional<bool> filtered;

	friend bool operator==(
		const PresetFolder &,
		const PresetFolder &) = default;
};

struct Preset {
	QString name;
	QString inherit;
	std::optional<bool> groupsRequireMention;

	// Inherited whole: a preset that names any folder replaces its parent's
	// selection outright rather than merging element by element.
	std::optional<std::vector<PresetFolder>> folders;

	std::vector<ListOverride> overrides;
};

struct ScheduleRule {
	bool enabled = true;
	std::vector<int> days; // Qt::Monday .. Qt::Sunday, 1 .. 7.
	int from = -1; // Minutes since local midnight.
	int till = -1;
	QString preset;
};

struct Schedule {
	bool enabled = true;
	std::vector<ScheduleRule> rules;
};

struct FocusSync {
	bool enabled = false;
	QString enterPreset;
	QString exitPreset;
};

struct Peek {
	QString hotkey;
	int autoOffSeconds = 0; // Zero disables the timer.
};

struct Premium {
	bool enabled = true;
};

struct Settings {
	Premium premium;

	// Priority order, first match wins, catch-alls forced to the bottom.
	std::vector<List> lists;

	std::vector<Preset> presets;
	Schedule schedule;
	FocusSync focusSync;
	Peek peek;

	[[nodiscard]] const List *list(const QString &name) const;
	[[nodiscard]] const Preset *preset(const QString &name) const;
};

// Everything recoverable is a warning and leaves usable settings behind; only
// a TOML syntax error or an inheritance cycle sets `error`, because neither
// leaves anything meaningful to run on. See docs/purple/config.md.
struct ParseResult {
	Settings settings;
	std::vector<QString> warnings;
	QString error;

	[[nodiscard]] bool ok() const {
		return error.isEmpty();
	}
};

[[nodiscard]] ParseResult ParseSettings(
	const QString &text,
	const QString &path);

// Names the parser will not accept for a user preset, because the engine uses
// them for the implicit root and for the stock-behaviour bypass.
[[nodiscard]] bool IsReservedPresetName(const QString &name);

// Whether exit_preset says "previous" - put back whatever was active when the
// focus mode came on, rather than a preset named outright.
[[nodiscard]] bool IsPreviousPresetName(const QString &name);

// "90s", "2m", "1h", "0" / "off" for no timer. Nothing for unparseable input.
[[nodiscard]] std::optional<int> ParseDuration(const QString &value);

// "HH:MM" to minutes since midnight.
[[nodiscard]] std::optional<int> ParseTimeOfDay(const QString &value);

// "mon" .. "sun" to Qt::Monday .. Qt::Sunday.
[[nodiscard]] std::optional<int> ParseWeekday(const QString &value);

} // namespace Purple
