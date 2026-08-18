/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_settings.h"

#include <algorithm>

// Return parse errors instead of throwing, so a hand-edited file with a typo
// degrades to "keep the last good settings and show a banner" rather than to an
// exception crossing a Qt event handler.
#define TOML_EXCEPTIONS 0
#include <toml.hpp>

namespace Purple {
namespace {

constexpr auto kDefaultPresetName = "default";
constexpr auto kNormalPresetName = "normal";
constexpr auto kPreviousPreset = "previous";
constexpr auto kDefaultHotkey = "Ctrl+Shift+P";
constexpr auto kDefaultPeekAutoOff = 120;

[[nodiscard]] QString Text(std::string_view value) {
	return QString::fromUtf8(value.data(), int(value.size()));
}

[[nodiscard]] QString At(const toml::node &node) {
	return u"line %1"_q.arg(node.source().begin.line);
}

// toml++ keeps tables sorted by key, but the user wrote them in some order and
// both the priority fallback and the preset list in the UI should follow the
// file rather than the alphabet.
[[nodiscard]] auto TablesInFileOrder(
		const toml::table &parent,
		const QString &context,
		std::vector<QString> &warnings) {
	auto result = std::vector<std::pair<QString, const toml::table*>>();
	for (auto &&[key, value] : parent) {
		const auto name = Text(key.str());
		if (const auto table = value.as_table()) {
			result.emplace_back(name, table);
		} else {
			warnings.push_back(u"%1: '%2' is not a table (%3), ignoring it."_q
				.arg(context, name, At(value)));
		}
	}
	std::stable_sort(result.begin(), result.end(), [](
			const auto &a,
			const auto &b) {
		return a.second->source().begin.line < b.second->source().begin.line;
	});
	return result;
}

[[nodiscard]] std::optional<bool> ReadBool(
		const toml::table &table,
		std::string_view key,
		const QString &context,
		std::vector<QString> &warnings) {
	const auto node = table.get(key);
	if (!node) {
		return std::nullopt;
	} else if (const auto value = node->value<bool>()) {
		return *value;
	}
	warnings.push_back(u"%1: '%2' should be true or false (%3), ignoring it."_q
		.arg(context, Text(key), At(*node)));
	return std::nullopt;
}

[[nodiscard]] std::optional<QString> ReadString(
		const toml::table &table,
		std::string_view key,
		const QString &context,
		std::vector<QString> &warnings) {
	const auto node = table.get(key);
	if (!node) {
		return std::nullopt;
	} else if (const auto value = node->value<std::string_view>()) {
		return Text(*value);
	}
	warnings.push_back(u"%1: '%2' should be a string (%3), ignoring it."_q
		.arg(context, Text(key), At(*node)));
	return std::nullopt;
}

[[nodiscard]] bool KnownPresetReference(
		const std::vector<Preset> &presets,
		const QString &name) {
	if (!name.compare(QLatin1String(kDefaultPresetName), Qt::CaseInsensitive)
		|| !name.compare(
			QLatin1String(kNormalPresetName),
			Qt::CaseInsensitive)) {
		return true;
	}
	return std::any_of(presets.begin(), presets.end(), [&](const Preset &p) {
		return p.name == name;
	});
}

void ReadMembers(
		List &list,
		const toml::table &table,
		const QString &context,
		std::vector<QString> &warnings) {
	const auto node = table.get("members");
	if (!node) {
		return;
	} else if (IsCatchAll(list.kind)) {
		warnings.push_back(
			u"%1: catch-all lists match by chat type and cannot have members "
			"(%2), ignoring them."_q.arg(context, At(*node)));
		return;
	}
	const auto array = node->as_array();
	if (!array) {
		warnings.push_back(u"%1: 'members' should be an array (%2)."_q
			.arg(context, At(*node)));
		return;
	}
	for (auto &&element : *array) {
		if (const auto id = element.value<int64>()) {
			list.members.push_back(*id);
		} else {
			warnings.push_back(
				u"%1: member entries should be peer ids (%2), ignoring one."_q
					.arg(context, At(element)));
		}
	}

	// Dedupe without reordering: the file is the user's, and the order they
	// wrote is the order the settings page shows back to them.
	auto unique = std::vector<PeerIdValue>();
	unique.reserve(list.members.size());
	for (const auto id : list.members) {
		if (std::find(unique.begin(), unique.end(), id) == unique.end()) {
			unique.push_back(id);
		}
	}
	list.members = std::move(unique);
}

[[nodiscard]] std::vector<List> ReadLists(
		const toml::table &root,
		std::vector<QString> &warnings) {
	auto result = std::vector<List>();
	const auto node = root.get("lists");
	if (node && !node->as_table()) {
		warnings.push_back(u"'lists' should be a table (%1)."_q.arg(At(*node)));
	} else if (node) {
		const auto &entries = TablesInFileOrder(
			*node->as_table(),
			u"lists"_q,
			warnings);
		for (const auto &[name, table] : entries) {
			const auto context = u"list '%1'"_q.arg(name);
			auto list = List();
			list.name = name;
			list.kind = CatchAllKind(name);
			if (list.kind == ListKind::Custom && name.startsWith('@')) {
				warnings.push_back(
					u"%1: names starting with '@' are reserved for the "
					"built-in catch-all lists, ignoring this list."_q
						.arg(context));
				continue;
			}
			list.title = ReadString(*table, "title", context, warnings)
				.value_or(name);
			list.show = ReadBool(*table, "show", context, warnings)
				.value_or(true);
			list.notify = ReadBool(*table, "notify", context, warnings)
				.value_or(true);
			if (const auto locked = ReadBool(
					*table,
					"locked",
					context,
					warnings)) {
				if (IsCatchAll(list.kind) && *locked) {
					warnings.push_back(
						u"%1: catch-all lists cannot be locked, ignoring it."_q
							.arg(context));
				} else {
					list.locked = *locked;
				}
			}
			ReadMembers(list, *table, context, warnings);
			result.push_back(std::move(list));
		}
	}

	// Every chat has to match exactly one list, so the four catch-alls always
	// exist whether or not the file mentions them. That is what lets the rest
	// of the engine skip an "unlisted" special case entirely.
	for (const auto &name : CatchAllNames()) {
		const auto i = std::find_if(
			result.begin(),
			result.end(),
			[&](const List &list) { return list.name == name; });
		if (i == result.end()) {
			auto list = List();
			list.name = name;
			list.title = name;
			list.kind = CatchAllKind(name);
			result.push_back(std::move(list));
		}
	}
	return result;
}

// Applies `list_order` as the priority order, leniently: anything the file
// forgot is appended, anything it names that does not exist is dropped, and the
// catch-alls are forced to the bottom whatever the file says.
[[nodiscard]] std::vector<List> ApplyListOrder(
		std::vector<List> lists,
		const toml::table &root,
		std::vector<QString> &warnings) {
	auto order = std::vector<QString>();
	if (const auto node = root.get("list_order")) {
		if (const auto array = node->as_array()) {
			for (auto &&element : *array) {
				if (const auto name = element.value<std::string_view>()) {
					order.push_back(Text(*name));
				} else {
					warnings.push_back(
						u"'list_order' entries should be strings (%1)."_q
							.arg(At(element)));
				}
			}
		} else {
			warnings.push_back(u"'list_order' should be an array (%1)."_q
				.arg(At(*node)));
		}
	}

	const auto find = [&](const QString &name) {
		return std::find_if(
			lists.begin(),
			lists.end(),
			[&](const List &list) { return list.name == name; });
	};

	auto custom = std::vector<List>();
	auto catchAll = std::vector<List>();
	auto taken = std::vector<QString>();
	auto lastCustom = -1;
	auto firstCatchAll = -1;
	for (auto i = 0; i != int(order.size()); ++i) {
		const auto &name = order[i];
		if (std::find(taken.begin(), taken.end(), name) != taken.end()) {
			warnings.push_back(
				u"'list_order' names '%1' more than once, keeping the first."_q
					.arg(name));
			continue;
		}
		const auto j = find(name);
		if (j == lists.end()) {
			warnings.push_back(
				u"'list_order' names '%1', which has no [lists.%1] table."_q
					.arg(name));
			continue;
		}
		taken.push_back(name);
		if (IsCatchAll(j->kind)) {
			if (firstCatchAll < 0) {
				firstCatchAll = i;
			}
			catchAll.push_back(std::move(*j));
		} else {
			lastCustom = i;
			custom.push_back(std::move(*j));
		}
		lists.erase(j);
	}
	if (firstCatchAll >= 0 && firstCatchAll < lastCustom) {
		warnings.push_back(
			u"'list_order': the built-in @private, @groups, @channels and "
			"@bots lists always sort below your own lists, so their position "
			"in list_order only orders them among themselves."_q);
	}

	// Whatever the file left out. Custom lists keep file order and get a
	// warning, because a forgotten list silently landing at the bottom of the
	// priority order is exactly the kind of surprise this config should not
	// have. Catch-alls are documented as optional, so they stay quiet.
	for (auto &list : lists) {
		if (!IsCatchAll(list.kind)) {
			warnings.push_back(
				u"list '%1' is missing from 'list_order', adding it at the "
				"bottom priority."_q.arg(list.name));
			custom.push_back(std::move(list));
		}
	}

	// The catch-alls list_order did name keep the order it gave them - the user
	// is allowed to reorder them among themselves - and the rest follow in the
	// canonical order rather than in whatever order they were declared in.
	for (const auto &name : CatchAllNames()) {
		const auto i = find(name);
		if (i != lists.end()) {
			catchAll.push_back(std::move(*i));
			lists.erase(i);
		}
	}

	auto result = std::move(custom);
	result.insert(
		result.end(),
		std::make_move_iterator(catchAll.begin()),
		std::make_move_iterator(catchAll.end()));
	return result;
}

[[nodiscard]] std::vector<PresetFolder> ReadFolders(
		const toml::array &array,
		const QString &context,
		std::vector<QString> &warnings) {
	auto result = std::vector<PresetFolder>();
	for (auto &&element : array) {
		const auto table = element.as_table();
		if (!table) {
			warnings.push_back(
				u"%1: 'folders' entries should look like "
				"{ name = \"Music\" } (%2)."_q.arg(context, At(element)));
			continue;
		}
		auto folder = PresetFolder();
		const auto name = ReadString(*table, "name", context, warnings);
		if (!name || name->isEmpty()) {
			warnings.push_back(u"%1: a folder entry has no 'name' (%2)."_q
				.arg(context, At(element)));
			continue;
		}
		folder.name = *name;
		folder.notify = ReadBool(*table, "notify", context, warnings);
		if (ReadBool(*table, "filtered", context, warnings).value_or(false)) {
			warnings.push_back(
				u"%1: folder '%2' asks for 'filtered', which is not "
				"implemented yet; showing the folder unfiltered."_q
					.arg(context, folder.name));
		}
		result.push_back(std::move(folder));
	}
	return result;
}

[[nodiscard]] std::vector<ListOverride> ReadOverrides(
		const toml::table &table,
		const std::vector<List> &lists,
		const QString &context,
		std::vector<QString> &warnings) {
	auto result = std::vector<ListOverride>();
	const auto entries = TablesInFileOrder(table, context, warnings);
	for (const auto &[name, fields] : entries) {
		const auto i = std::find_if(
			lists.begin(),
			lists.end(),
			[&](const List &list) { return list.name == name; });
		if (i == lists.end()) {
			warnings.push_back(
				u"%1: overrides '%2', which is not a list, ignoring it."_q
					.arg(context, name));
			continue;
		} else if (i->locked) {
			warnings.push_back(
				u"%1: list '%2' is locked, so its defaults win and this "
				"override is ignored."_q.arg(context, name));
			continue;
		}
		const auto nested = u"%1 override '%2'"_q.arg(context, name);
		auto entry = ListOverride();
		entry.list = name;
		entry.show = ReadBool(*fields, "show", nested, warnings);
		entry.notify = ReadBool(*fields, "notify", nested, warnings);
		entry.groupsRequireMention = ReadBool(
			*fields,
			"groups_require_mention",
			nested,
			warnings);
		result.push_back(std::move(entry));
	}
	return result;
}

[[nodiscard]] std::vector<Preset> ReadPresets(
		const toml::table &root,
		const std::vector<List> &lists,
		std::vector<QString> &warnings) {
	auto result = std::vector<Preset>();
	const auto node = root.get("presets");
	if (!node) {
		return result;
	} else if (!node->as_table()) {
		warnings.push_back(u"'presets' should be a table (%1)."_q
			.arg(At(*node)));
		return result;
	}
	const auto entries = TablesInFileOrder(
		*node->as_table(),
		u"presets"_q,
		warnings);

	// Two passes: `inherit` can name a preset declared further down the file,
	// so nothing can be validated against the preset list until it is complete.
	for (const auto &[name, table] : entries) {
		if (IsReservedPresetName(name)) {
			warnings.push_back(
				u"preset '%1': that name is reserved, ignoring this preset."_q
					.arg(name));
			continue;
		}
		const auto context = u"preset '%1'"_q.arg(name);
		auto preset = Preset();
		preset.name = name;
		preset.inherit = ReadString(*table, "inherit", context, warnings)
			.value_or(QString::fromLatin1(kDefaultPresetName));
		preset.groupsRequireMention = ReadBool(
			*table,
			"groups_require_mention",
			context,
			warnings);
		if (const auto folders = table->get("folders")) {
			if (const auto array = folders->as_array()) {
				preset.folders = ReadFolders(*array, context, warnings);
			} else {
				warnings.push_back(u"%1: 'folders' should be an array (%2)."_q
					.arg(context, At(*folders)));
			}
		}
		if (const auto overrides = table->get("overrides")) {
			if (const auto nested = overrides->as_table()) {
				preset.overrides = ReadOverrides(
					*nested,
					lists,
					context,
					warnings);
			} else {
				warnings.push_back(u"%1: 'overrides' should be a table (%2)."_q
					.arg(context, At(*overrides)));
			}
		}
		result.push_back(std::move(preset));
	}

	for (auto &preset : result) {
		const auto normal = !preset.inherit.compare(
			QLatin1String(kNormalPresetName),
			Qt::CaseInsensitive);
		if (normal) {
			warnings.push_back(
				u"preset '%1' inherits 'normal', which bypasses the whole "
				"feature and cannot be a parent; using 'default'."_q
					.arg(preset.name));
			preset.inherit = QString::fromLatin1(kDefaultPresetName);
		} else if (!KnownPresetReference(result, preset.inherit)) {
			warnings.push_back(
				u"preset '%1' inherits '%2', which does not exist; using "
				"'default'."_q.arg(preset.name, preset.inherit));
			preset.inherit = QString::fromLatin1(kDefaultPresetName);
		}
	}
	return result;
}

// A cycle has no root to resolve against, so unlike everything else in this
// parser it cannot be repaired into something usable. Spec 1.2: reject the
// whole config and keep running on the last good one.
[[nodiscard]] QString DetectInheritanceCycle(
		const std::vector<Preset> &presets) {
	for (const auto &preset : presets) {
		auto chain = QStringList{ preset.name };
		auto current = preset.inherit;
		while (!current.isEmpty()
			&& current.compare(
				QLatin1String(kDefaultPresetName),
				Qt::CaseInsensitive)) {
			if (chain.contains(current)) {
				chain.push_back(current);
				return u"preset inheritance loops: %1."_q
					.arg(chain.join(u" -> "_q));
			}
			chain.push_back(current);
			const auto i = std::find_if(
				presets.begin(),
				presets.end(),
				[&](const Preset &p) { return p.name == current; });
			if (i == presets.end()) {
				break;
			}
			current = i->inherit;
		}
	}
	return QString();
}

[[nodiscard]] Schedule ReadSchedule(
		const toml::table &root,
		const std::vector<Preset> &presets,
		std::vector<QString> &warnings) {
	auto result = Schedule();
	const auto node = root.get("schedule");
	if (!node) {
		return result;
	} else if (!node->as_table()) {
		warnings.push_back(u"'schedule' should be a table (%1)."_q
			.arg(At(*node)));
		return result;
	}
	const auto &table = *node->as_table();
	result.enabled = ReadBool(table, "enabled", u"schedule"_q, warnings)
		.value_or(true);

	const auto rules = table.get("rules");
	if (!rules) {
		return result;
	}
	const auto array = rules->as_array();
	if (!array) {
		warnings.push_back(u"'schedule.rules' should be an array (%1)."_q
			.arg(At(*rules)));
		return result;
	}
	auto index = 0;
	for (auto &&element : *array) {
		const auto context = u"schedule rule %1"_q.arg(++index);
		const auto fields = element.as_table();
		if (!fields) {
			warnings.push_back(u"%1: should be a [[schedule.rules]] table "
				"(%2)."_q.arg(context, At(element)));
			continue;
		}
		auto rule = ScheduleRule();
		rule.enabled = ReadBool(*fields, "enabled", context, warnings)
			.value_or(true);
		const auto from = ReadString(*fields, "from", context, warnings);
		const auto till = ReadString(*fields, "to", context, warnings);
		const auto preset = ReadString(*fields, "preset", context, warnings);
		if (!from || !till || !preset) {
			warnings.push_back(
				u"%1: needs 'from', 'to' and 'preset', skipping it."_q
					.arg(context));
			continue;
		}
		const auto parsedFrom = ParseTimeOfDay(*from);
		const auto parsedTill = ParseTimeOfDay(*till);
		if (!parsedFrom || !parsedTill) {
			warnings.push_back(
				u"%1: 'from' and 'to' should look like \"09:00\", "
				"skipping it."_q.arg(context));
			continue;
		} else if (*parsedFrom == *parsedTill) {
			warnings.push_back(
				u"%1: 'from' and 'to' are the same time, skipping it."_q
					.arg(context));
			continue;
		} else if (!KnownPresetReference(presets, *preset)) {
			warnings.push_back(
				u"%1: preset '%2' does not exist, skipping it."_q
					.arg(context, *preset));
			continue;
		}
		rule.from = *parsedFrom;
		rule.till = *parsedTill;
		rule.preset = *preset;

		if (const auto days = fields->get("days")) {
			if (const auto list = days->as_array()) {
				for (auto &&day : *list) {
					const auto name = day.value<std::string_view>();
					const auto parsed = name
						? ParseWeekday(Text(*name))
						: std::nullopt;
					if (parsed) {
						rule.days.push_back(*parsed);
					} else {
						warnings.push_back(
							u"%1: '%2' is not a weekday like \"mon\" (%3)."_q
								.arg(
									context,
									name ? Text(*name) : u"?"_q,
									At(day)));
					}
				}
			} else {
				warnings.push_back(u"%1: 'days' should be an array (%2)."_q
					.arg(context, At(*days)));
			}
		}
		if (rule.days.empty()) {
			warnings.push_back(
				u"%1: no weekdays given, applying it every day."_q
					.arg(context));
			rule.days = { 1, 2, 3, 4, 5, 6, 7 };
		}
		result.rules.push_back(std::move(rule));
	}
	return result;
}

[[nodiscard]] FocusSync ReadFocusSync(
		const toml::table &root,
		const std::vector<Preset> &presets,
		std::vector<QString> &warnings) {
	auto result = FocusSync();
	result.exitPreset = QString::fromLatin1(kPreviousPreset);
	const auto node = root.get("focus_sync");
	if (!node) {
		return result;
	} else if (!node->as_table()) {
		warnings.push_back(u"'focus_sync' should be a table (%1)."_q
			.arg(At(*node)));
		return result;
	}
	const auto &table = *node->as_table();
	const auto context = u"focus_sync"_q;
	result.enabled = ReadBool(table, "enabled", context, warnings)
		.value_or(false);
	result.enterPreset = ReadString(table, "enter_preset", context, warnings)
		.value_or(QString());
	result.exitPreset = ReadString(table, "exit_preset", context, warnings)
		.value_or(QString::fromLatin1(kPreviousPreset));

	if (result.enabled
		&& (result.enterPreset.isEmpty()
			|| !KnownPresetReference(presets, result.enterPreset))) {
		warnings.push_back(
			u"focus_sync: 'enter_preset' is missing or names a preset that "
			"does not exist, turning focus sync off."_q);
		result.enabled = false;
	}
	if (result.exitPreset.compare(
			QLatin1String(kPreviousPreset),
			Qt::CaseInsensitive)
		&& !KnownPresetReference(presets, result.exitPreset)) {
		warnings.push_back(
			u"focus_sync: 'exit_preset' names '%1', which does not exist; "
			"restoring the previous preset instead."_q
				.arg(result.exitPreset));
		result.exitPreset = QString::fromLatin1(kPreviousPreset);
	}
	return result;
}

[[nodiscard]] Peek ReadPeek(
		const toml::table &root,
		std::vector<QString> &warnings) {
	auto result = Peek();
	result.hotkey = QString::fromLatin1(kDefaultHotkey);
	result.autoOffSeconds = kDefaultPeekAutoOff;
	const auto node = root.get("peek");
	if (!node) {
		return result;
	} else if (!node->as_table()) {
		warnings.push_back(u"'peek' should be a table (%1)."_q.arg(At(*node)));
		return result;
	}
	const auto &table = *node->as_table();
	const auto context = u"peek"_q;
	result.hotkey = ReadString(table, "hotkey", context, warnings)
		.value_or(QString::fromLatin1(kDefaultHotkey));
	if (const auto autoOff = ReadString(table, "auto_off", context, warnings)) {
		if (const auto seconds = ParseDuration(*autoOff)) {
			result.autoOffSeconds = *seconds;
		} else {
			warnings.push_back(
				u"peek: 'auto_off' should look like \"2m\", \"90s\" or "
				"\"off\", keeping the default."_q);
		}
	}
	return result;
}

} // namespace

bool IsCatchAll(ListKind kind) {
	return kind != ListKind::Custom;
}

const std::vector<QString> &CatchAllNames() {
	static const auto result = std::vector<QString>{
		u"@private"_q,
		u"@groups"_q,
		u"@channels"_q,
		u"@bots"_q,
	};
	return result;
}

ListKind CatchAllKind(const QString &name) {
	return (name == u"@private"_q)
		? ListKind::Private
		: (name == u"@groups"_q)
		? ListKind::Groups
		: (name == u"@channels"_q)
		? ListKind::Channels
		: (name == u"@bots"_q)
		? ListKind::Bots
		: ListKind::Custom;
}

bool IsReservedPresetName(const QString &name) {
	return !name.compare(QLatin1String(kDefaultPresetName), Qt::CaseInsensitive)
		|| !name.compare(
			QLatin1String(kNormalPresetName),
			Qt::CaseInsensitive);
}

std::optional<int> ParseDuration(const QString &value) {
	const auto trimmed = value.trimmed();
	if (trimmed.isEmpty()) {
		return std::nullopt;
	} else if (!trimmed.compare(u"off"_q, Qt::CaseInsensitive)
		|| !trimmed.compare(u"none"_q, Qt::CaseInsensitive)) {
		return 0;
	}
	auto multiplier = 1;
	auto digits = trimmed;
	const auto last = trimmed.back().toLower();
	if (last == 's' || last == 'm' || last == 'h') {
		multiplier = (last == 's') ? 1 : (last == 'm') ? 60 : 3600;
		digits = trimmed.left(trimmed.size() - 1).trimmed();
	}
	auto ok = false;
	const auto number = digits.toInt(&ok);
	if (!ok || number < 0) {
		return std::nullopt;
	}
	return number * multiplier;
}

std::optional<int> ParseTimeOfDay(const QString &value) {
	const auto parts = value.trimmed().split(':');
	if (parts.size() != 2) {
		return std::nullopt;
	}
	auto hoursOk = false;
	auto minutesOk = false;
	const auto hours = parts[0].toInt(&hoursOk);
	const auto minutes = parts[1].toInt(&minutesOk);
	if (!hoursOk || !minutesOk
		|| hours < 0 || hours > 23
		|| minutes < 0 || minutes > 59) {
		return std::nullopt;
	}
	return hours * 60 + minutes;
}

std::optional<int> ParseWeekday(const QString &value) {
	static const auto names = std::vector<QString>{
		u"mon"_q, u"tue"_q, u"wed"_q, u"thu"_q, u"fri"_q, u"sat"_q, u"sun"_q,
	};
	const auto trimmed = value.trimmed().toLower();
	for (auto i = 0; i != int(names.size()); ++i) {
		if (trimmed == names[i] || trimmed.startsWith(names[i])) {
			return i + 1;
		}
	}
	return std::nullopt;
}

const List *Settings::list(const QString &name) const {
	const auto i = std::find_if(
		lists.begin(),
		lists.end(),
		[&](const List &list) { return list.name == name; });
	return (i == lists.end()) ? nullptr : &*i;
}

const Preset *Settings::preset(const QString &name) const {
	const auto i = std::find_if(
		presets.begin(),
		presets.end(),
		[&](const Preset &preset) { return preset.name == name; });
	return (i == presets.end()) ? nullptr : &*i;
}

ParseResult ParseSettings(const QString &text, const QString &path) {
	auto result = ParseResult();
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		result.error = u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description()));
		return result;
	}
	const auto root = std::move(parsed).table();

	if (const auto premium = root.get("premium")) {
		if (const auto table = premium->as_table()) {
			result.settings.premium.enabled = ReadBool(
				*table,
				"enabled",
				u"premium"_q,
				result.warnings).value_or(true);
		} else {
			result.warnings.push_back(u"'premium' should be a table (%1)."_q
				.arg(At(*premium)));
		}
	}

	result.settings.lists = ApplyListOrder(
		ReadLists(root, result.warnings),
		root,
		result.warnings);
	result.settings.presets = ReadPresets(
		root,
		result.settings.lists,
		result.warnings);
	if (auto cycle = DetectInheritanceCycle(result.settings.presets);
		!cycle.isEmpty()) {
		result.error = std::move(cycle);
		return result;
	}
	result.settings.schedule = ReadSchedule(
		root,
		result.settings.presets,
		result.warnings);
	result.settings.focusSync = ReadFocusSync(
		root,
		result.settings.presets,
		result.warnings);
	result.settings.peek = ReadPeek(root, result.warnings);
	return result;
}

} // namespace Purple
