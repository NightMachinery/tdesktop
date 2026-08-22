/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_engine.h"

#include <QtCore/QDateTime>

#include <algorithm>

namespace Purple {
namespace {

constexpr auto kDefaultPresetName = "default";

[[nodiscard]] bool IsDefaultName(const QString &name) {
	return name.isEmpty()
		|| !name.compare(
			QLatin1String(kDefaultPresetName),
			Qt::CaseInsensitive);
}

// The chain from the named preset up to the implicit default, most derived
// first. Empty for "default" itself. Nothing if a link is missing or loops -
// the parser rejects loops, but this is reached from the state file too, which
// can name a preset the current settings no longer describe.
[[nodiscard]] std::optional<std::vector<const Preset*>> Chain(
		const Settings &settings,
		const QString &preset) {
	auto result = std::vector<const Preset*>();
	auto seen = QStringList();
	auto current = preset;
	while (!IsDefaultName(current)) {
		if (seen.contains(current)) {
			return std::nullopt;
		}
		seen.push_back(current);
		const auto found = settings.preset(current);
		if (!found) {
			return std::nullopt;
		}
		result.push_back(found);
		current = found->inherit;
	}
	return result;
}

// First explicit value wins, walking from the most derived preset upwards.
// Absent everywhere means the list default applies.
[[nodiscard]] std::optional<bool> Inherited(
		const std::vector<const Preset*> &chain,
		const QString &list,
		std::optional<bool> ListOverride::*field) {
	for (const auto preset : chain) {
		const auto i = std::find_if(
			preset->overrides.begin(),
			preset->overrides.end(),
			[&](const ListOverride &entry) { return entry.list == list; });
		if (i != preset->overrides.end() && ((*i).*field)) {
			return (*i).*field;
		}
	}
	return std::nullopt;
}

} // namespace

ListKind CatchAllFor(ChatKind kind) {
	switch (kind) {
	case ChatKind::Private: return ListKind::Private;
	case ChatKind::Group: return ListKind::Groups;
	case ChatKind::Channel: return ListKind::Channels;
	case ChatKind::Bot: return ListKind::Bots;
	}
	return ListKind::Private;
}

const EffectiveList *Resolved::list(const QString &name) const {
	const auto i = std::find_if(
		lists.begin(),
		lists.end(),
		[&](const EffectiveList &entry) { return entry.list == name; });
	return (i == lists.end()) ? nullptr : &*i;
}

std::optional<Resolved> Resolve(
		const Settings &settings,
		const QString &preset) {
	auto result = Resolved();
	if (!preset.compare(NormalPreset(), Qt::CaseInsensitive)) {
		result.preset = NormalPreset();
		result.normal = true;
		return result;
	}
	const auto chain = Chain(settings, preset);
	if (!chain) {
		return std::nullopt;
	}
	result.preset = IsDefaultName(preset)
		? QString::fromLatin1(kDefaultPresetName)
		: preset;

	for (const auto entry : *chain) {
		if (entry->groupsRequireMention) {
			result.groupsRequireMention = *entry->groupsRequireMention;
			break;
		}
	}

	// Folders are inherited whole: the first preset in the chain that names any
	// replaces its parent's selection outright, rather than merging entry by
	// entry, so a child can also deliberately show none by writing "[]".
	for (const auto entry : *chain) {
		if (entry->folders) {
			result.folders = *entry->folders;
			break;
		}
	}
	result.exemptFolders = ExemptFolderNames(result.folders);

	result.lists.reserve(settings.lists.size());
	for (const auto &list : settings.lists) {
		auto effective = EffectiveList();
		effective.list = list.name;
		effective.show = list.show;
		effective.notify = list.notify;
		effective.groupsRequireMention = result.groupsRequireMention;

		// A locked list keeps its own defaults whatever any preset says. The
		// parser drops overrides of locked lists with a warning, so reaching
		// one here would mean the file changed under us; ignore it either way.
		if (!list.locked) {
			effective.show = Inherited(*chain, list.name, &ListOverride::show)
				.value_or(list.show);
			effective.notify = Inherited(
				*chain,
				list.name,
				&ListOverride::notify).value_or(list.notify);
			effective.groupsRequireMention = Inherited(
				*chain,
				list.name,
				&ListOverride::groupsRequireMention
			).value_or(result.groupsRequireMention);
		}
		result.lists.push_back(std::move(effective));
	}
	return result;
}

std::vector<QString> ExemptFolderNames(
		const std::optional<std::vector<PresetFolder>> &folders) {
	auto result = std::vector<QString>();
	if (!folders) {
		return result;
	}
	for (const auto &folder : *folders) {
		// Only an explicit false exempts. Saying nothing leaves the folder
		// filtered, which is what every folder the preset does not name is.
		if (folder.filtered.has_value() && !*folder.filtered) {
			result.push_back(folder.name);
		}
	}
	return result;
}

const EffectiveList *MatchList(
		const Settings &settings,
		const Resolved &resolved,
		PeerIdValue id,
		ChatKind kind) {
	if (resolved.normal) {
		return nullptr;
	}
	const auto wanted = CatchAllFor(kind);

	// Priority order, first match wins. Catch-alls sit at the bottom and match
	// by chat type, so every chat matches exactly one list and there is no
	// "unlisted" case anywhere above this.
	for (const auto &effective : resolved.lists) {
		const auto list = settings.list(effective.list);
		if (!list) {
			continue;
		} else if (IsCatchAll(list->kind)) {
			if (list->kind == wanted) {
				return &effective;
			}
		} else if (std::find(list->members.begin(), list->members.end(), id)
			!= list->members.end()) {
			return &effective;
		}
	}
	return nullptr;
}

Visibility Visible(
		const Settings &settings,
		const Resolved &resolved,
		PeerIdValue id,
		ChatKind kind) {
	auto result = Visibility();
	if (resolved.normal) {
		return result;
	}
	const auto effective = MatchList(settings, resolved, id, kind);
	if (!effective) {
		return result;
	}
	result.show = effective->show;
	result.notify = effective->notify;

	// Only groups are gated, and only while they are visible at all. Channels
	// have no mentions in the relevant sense, and a hidden chat is hidden
	// whether or not anyone mentioned us in it.
	result.mentionGated = result.show
		&& (kind == ChatKind::Group)
		&& effective->groupsRequireMention;

	// Peek reveals; it does not un-silence. Both halves of a preset could be
	// suspended together, but they answer different questions: hiding is about
	// what you can find, silencing is about what may interrupt you, and a peek
	// is a deliberate look at the chat list. Unmuting for it would deliver a
	// burst of notifications for chats you are already looking at, and then
	// take the mute back before you had dealt with them.
	if (resolved.peeking) {
		result.show = true;
		result.mentionGated = false;
	}
	return result;
}

ResolvedCache ToCache(const Resolved &resolved) {
	auto result = ResolvedCache();
	if (resolved.normal) {
		return result;
	}
	result.preset = resolved.preset;
	result.groupsRequireMention = resolved.groupsRequireMention;
	result.folders = resolved.folders;
	result.lists.reserve(resolved.lists.size());
	for (const auto &entry : resolved.lists) {
		result.lists.push_back({ entry.list, entry.show, entry.notify });
	}
	return result;
}

std::optional<Resolved> FromCache(const ResolvedCache &cache) {
	if (!cache.valid()) {
		return std::nullopt;
	}
	auto result = Resolved();
	result.preset = cache.preset;
	result.groupsRequireMention = cache.groupsRequireMention;
	result.folders = cache.folders;
	result.exemptFolders = ExemptFolderNames(result.folders);
	result.lists.reserve(cache.lists.size());
	for (const auto &entry : cache.lists) {
		// The cache does not carry per-list mention gating: it exists to keep
		// show and notify stable through a broken reload, and one preset-wide
		// value is enough for that.
		result.lists.push_back({
			entry.list,
			entry.show,
			entry.notify,
			cache.groupsRequireMention,
		});
	}
	return result;
}

std::optional<QString> ScheduleTarget(
		const Schedule &schedule,
		const QDateTime &now) {
	if (!schedule.enabled || schedule.rules.empty()) {
		return std::nullopt;
	}
	const auto covers = [](const ScheduleRule &rule, int day) {
		return std::find(rule.days.begin(), rule.days.end(), day)
			!= rule.days.end();
	};
	const auto time = now.time();
	const auto minutes = time.hour() * 60 + time.minute();
	const auto today = now.date().dayOfWeek();
	const auto yesterday = (today == 1) ? 7 : (today - 1);

	// First match wins, in file order, the same rule the lists follow. Two
	// rules covering one moment is a thing a hand-written file will do, and
	// picking by position is the only answer that can be predicted by reading.
	for (const auto &rule : schedule.rules) {
		if (!rule.enabled) {
			continue;
		} else if (rule.from < rule.till) {
			// Half-open, so 09:00-12:00 and 12:00-17:00 hand over cleanly
			// rather than both claiming noon. The parser has already refused
			// a rule whose ends are equal, so there is no empty window here.
			if (covers(rule, today)
				&& minutes >= rule.from
				&& minutes < rule.till) {
				return rule.preset;
			}
		} else if ((covers(rule, today) && minutes >= rule.from)
			|| (covers(rule, yesterday) && minutes < rule.till)) {
			// A window crossing midnight belongs to the day it starts on, so
			// "mon, 22:00 to 06:00" runs into Tuesday morning instead of
			// stopping at midnight or needing Tuesday listed as well - which
			// would also have claimed Tuesday 00:00 to 06:00 twice over.
			return rule.preset;
		}
	}
	return NormalPreset();
}

} // namespace Purple
