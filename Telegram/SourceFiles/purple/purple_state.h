/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_settings.h"

// state.toml, the machine-owned half of the configuration. Everything here is
// rewritten whenever it changes and carries no comments, which is exactly why
// it is a separate file: state churns constantly, and it must never touch the
// mtime of the settings.toml the user is editing by hand.
namespace Purple {

// What put the current preset in place. The distinction is load-bearing rather
// than informational: a schedule boundary aiming at Normal is skipped when the
// user chose the current preset themselves. See spec 6.2.
enum class PresetSource : uchar {
	Manual,
	Schedule,
	Focus,
};

[[nodiscard]] QString PresetSourceName(PresetSource source);
[[nodiscard]] PresetSource PresetSourceFromName(const QString &name);

struct ResolvedList {
	QString list;
	bool show = true;
	bool notify = true;
};

// The last resolution that worked. When a reload leaves the active preset
// unresolvable - deleted, or its inheritance chain broken - the engine keeps
// running on this rather than falling back to showing everything. See spec 8.5.
struct ResolvedCache {
	QString preset;

	// Cached alongside the rest so a broken reload does not also rename the
	// tab. Absent in a file written by an older build, which is why reading it
	// falls back to the preset name rather than to an empty label.
	QString viewName;

	std::vector<ResolvedList> lists;
	bool groupsRequireMention = false;
	bool hideEverywhere = false;

	// Nothing and an empty vector mean different things - see Resolved::folders
	// - so the key is written only when the preset said something, and its
	// absence round-trips as "said nothing".
	std::optional<std::vector<PresetFolder>> folders;

	[[nodiscard]] bool valid() const {
		return !preset.isEmpty();
	}
};

struct State {
	QString activePreset;
	PresetSource activeSource = PresetSource::Manual;

	// Remembered when OS focus takes over, so exiting focus can put back both
	// the preset and the reason it was active.
	QString previousPreset;
	PresetSource previousSource = PresetSource::Manual;

	// Whether the OS says a focus mode is on. Written by whatever is watching
	// for that, which is deliberately not the same thing as what acts on it:
	// the policy below is worth proving, and the detection is a moving target
	// that Apple owns. See docs/purple/work_mode.md.
	bool focusActive = false;

	// The last value of focusActive that was acted on, so focus sync moves on
	// a change rather than on a value - the same shape as scheduleTarget, and
	// for the same reason. A preset chosen by hand in the middle of a focus
	// session stands, because nothing fires again until focus itself changes.
	bool focusSeen = false;

	bool schedulePaused = false;

	// The last preset the schedule computed, so it can act on a change rather
	// than on every tick. That is what lets a preset chosen by hand survive
	// until the next boundary instead of being overwritten a second later, and
	// what lets a boundary missed while the app was closed still be caught up
	// on the next launch. Empty means it has never run.
	QString scheduleTarget;

	bool peekActive = false;
	int64 peekDeadlineUnix = 0;
	ResolvedCache resolvedCache;
};

// Whether the peek recorded in state is still running. A deadline of zero means
// auto_off is turned off, so the peek runs until it is turned off by hand -
// which is the only reason the flag is persisted rather than kept in memory.
// The comparison is what makes a peek that outlived the app expire on its own:
// the deadline is in the past by the time anything reads it again.
[[nodiscard]] bool PeekLive(const State &state, int64 nowUnix);

// The name that means "behave exactly like stock Telegram Desktop".
[[nodiscard]] const QString &NormalPreset();

[[nodiscard]] State ParseState(const QString &text, const QString &path);
[[nodiscard]] QString SerializeState(const State &state);

} // namespace Purple
