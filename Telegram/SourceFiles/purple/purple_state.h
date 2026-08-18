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
	std::vector<ResolvedList> lists;
	bool groupsRequireMention = true;
	std::vector<PresetFolder> folders;

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

	bool schedulePaused = false;
	bool peekActive = false;
	int64 peekDeadlineUnix = 0;
	ResolvedCache resolvedCache;
};

// The name that means "behave exactly like stock Telegram Desktop".
[[nodiscard]] const QString &NormalPreset();

[[nodiscard]] State ParseState(const QString &text, const QString &path);
[[nodiscard]] QString SerializeState(const State &state);

} // namespace Purple
