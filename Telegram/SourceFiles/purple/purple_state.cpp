/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_state.h"

#define TOML_EXCEPTIONS 0
#include <toml.hpp>

namespace Purple {
namespace {

[[nodiscard]] QString Text(std::string_view value) {
	return QString::fromUtf8(value.data(), int(value.size()));
}

// Preset and folder names come from settings.toml, which the user writes, so
// they can hold quotes and backslashes even though they rarely will.
[[nodiscard]] QString Quoted(const QString &value) {
	auto escaped = QString();
	escaped.reserve(value.size() + 2);
	for (const auto ch : value) {
		if (ch == '"' || ch == '\\') {
			escaped += '\\';
			escaped += ch;
		} else if (ch == '\n') {
			escaped += u"\\n"_q;
		} else if (ch == '\r') {
			escaped += u"\\r"_q;
		} else if (ch == '\t') {
			escaped += u"\\t"_q;
		} else {
			escaped += ch;
		}
	}
	return '"' + escaped + '"';
}

[[nodiscard]] QString Boolean(bool value) {
	return value ? u"true"_q : u"false"_q;
}

[[nodiscard]] QString ReadString(
		const toml::table &table,
		std::string_view key,
		const QString &fallback = QString()) {
	const auto node = table.get(key);
	if (!node) {
		return fallback;
	}
	const auto value = node->value<std::string_view>();
	return value ? Text(*value) : fallback;
}

[[nodiscard]] bool ReadBool(
		const toml::table &table,
		std::string_view key,
		bool fallback) {
	const auto node = table.get(key);
	return node ? node->value_or(fallback) : fallback;
}

[[nodiscard]] QString SerializeLists(const std::vector<ResolvedList> &lists) {
	auto result = u"lists = [\n"_q;
	for (const auto &list : lists) {
		result += u"  { list = %1, show = %2, notify = %3, "
			"groups_require_mention = %4 },\n"_q.arg(
				Quoted(list.list),
				Boolean(list.show),
				Boolean(list.notify),
				Boolean(list.groupsRequireMention));
	}
	return result + u"]\n"_q;
}

void ReadOptionalBool(
		const toml::table &table,
		std::string_view key,
		std::optional<bool> &into) {
	if (const auto node = table.get(key)) {
		if (const auto value = node->value<bool>()) {
			into = *value;
		}
	}
}

[[nodiscard]] std::vector<ResolvedList> ReadResolvedLists(
		const toml::table &table,
		std::string_view key) {
	auto result = std::vector<ResolvedList>();
	const auto node = table.get(key);
	const auto array = node ? node->as_array() : nullptr;
	if (!array) {
		return result;
	}
	for (auto &&element : *array) {
		const auto fields = element.as_table();
		if (!fields) {
			continue;
		}
		auto entry = ResolvedList();
		entry.list = ReadString(*fields, "list");
		if (entry.list.isEmpty()) {
			continue;
		}
		entry.show = ReadBool(*fields, "show", true);
		entry.notify = ReadBool(*fields, "notify", true);
		entry.groupsRequireMention = ReadBool(
			*fields,
			"groups_require_mention",
			false);
		result.push_back(std::move(entry));
	}
	return result;
}

[[nodiscard]] ResolvedCache ReadResolvedCache(const toml::table &root) {
	auto result = ResolvedCache();
	const auto node = root.get("resolved_cache");
	const auto table = node ? node->as_table() : nullptr;
	if (!table) {
		return result;
	}
	result.preset = ReadString(*table, "preset");
	if (result.preset.isEmpty()) {
		return result;
	}
	result.viewName = ReadString(*table, "view_name");
	if (result.viewName.isEmpty()) {
		result.viewName = DefaultViewName(result.preset);
	}
	result.hideEverywhere = ReadBool(*table, "hide_everywhere", false);
	result.lists = ReadResolvedLists(*table, "lists");
	if (const auto folders = table->get("folders")) {
		if (const auto array = folders->as_array()) {
			for (auto &&element : *array) {
				const auto fields = element.as_table();
				if (!fields) {
					continue;
				}
				auto folder = PresetFolder();
				folder.name = ReadString(*fields, "name");
				if (folder.name.isEmpty()) {
					continue;
				}
				ReadOptionalBool(*fields, "show", folder.show);
				ReadOptionalBool(*fields, "notify", folder.notify);
				ReadOptionalBool(
					*fields,
					"include_in_main_view",
					folder.includeInMainView);
				result.folders.push_back(std::move(folder));
			}
		}
	}
	if (const auto views = table->get("views")) {
		if (const auto array = views->as_array()) {
			for (auto &&element : *array) {
				const auto fields = element.as_table();
				if (!fields) {
					continue;
				}
				auto view = ResolvedCacheView();
				view.name = ReadString(*fields, "name");
				view.lists = ReadResolvedLists(*fields, "lists");
				if (view.name.isEmpty() || view.lists.empty()) {
					continue;
				}
				if (const auto pinned = fields->get("pinned")) {
					if (const auto ids = pinned->as_array()) {
						for (auto &&id : *ids) {
							if (const auto value = id.value<int64>()) {
								view.pinned.push_back(*value);
							}
						}
					}
				}
				result.views.push_back(std::move(view));
			}
		}
	}

	// A cache naming lists it cannot describe is worse than none: the engine
	// would resolve half the chats and silently default the rest.
	if (result.lists.empty()) {
		result = ResolvedCache();
	}
	return result;
}

} // namespace

const QString &NormalPreset() {
	static const auto result = u"normal"_q;
	return result;
}

QString PresetSourceName(PresetSource source) {
	switch (source) {
	case PresetSource::Schedule: return u"schedule"_q;
	case PresetSource::Focus: return u"focus"_q;
	case PresetSource::Manual: return u"manual"_q;
	}
	return u"manual"_q;
}

PresetSource PresetSourceFromName(const QString &name) {
	return (name == u"schedule"_q)
		? PresetSource::Schedule
		: (name == u"focus"_q)
		? PresetSource::Focus
		: PresetSource::Manual;
}

// Nothing here reports errors. The file is ours, it is rewritten whenever
// anything changes, and a state we cannot read is not worth interrupting the
// user over - starting from stock behaviour is a safe place to begin.
State ParseState(const QString &text, const QString &path) {
	auto result = State();
	result.activePreset = NormalPreset();
	if (text.trimmed().isEmpty()) {
		return result;
	}
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		return result;
	}
	const auto &root = parsed.table();
	result.activePreset = ReadString(root, "active_preset", NormalPreset());
	result.activeSource = PresetSourceFromName(
		ReadString(root, "active_preset_source"));
	result.previousPreset = ReadString(root, "previous_preset");
	result.previousSource = PresetSourceFromName(
		ReadString(root, "previous_source"));
	result.focusActive = ReadBool(root, "focus_active", false);
	result.focusSeen = ReadBool(root, "focus_seen", false);
	result.schedulePaused = ReadBool(root, "schedule_paused", false);
	result.scheduleTarget = ReadString(root, "schedule_target");
	result.peekActive = ReadBool(root, "peek_active", false);
	if (const auto deadline = root.get("peek_deadline_unix")) {
		result.peekDeadlineUnix = deadline->value_or(int64(0));
	}
	result.resolvedCache = ReadResolvedCache(root);
	if (result.activePreset.isEmpty()) {
		result.activePreset = NormalPreset();
	}
	return result;
}

bool PeekLive(const State &state, int64 nowUnix) {
	return state.peekActive
		&& (!state.peekDeadlineUnix || state.peekDeadlineUnix > nowUnix);
}

QString SerializeState(const State &state) {
	auto result = QString();
	result += u"# Purple Telegram runtime state. This file is written by the "
		"app and\n# rewritten whenever anything in it changes - edit "
		"settings.toml instead.\n\n"_q;
	result += u"active_preset        = %1\n"_q.arg(Quoted(state.activePreset));
	result += u"active_preset_source = %1\n"_q
		.arg(Quoted(PresetSourceName(state.activeSource)));
	result += u"previous_preset      = %1\n"_q
		.arg(Quoted(state.previousPreset));
	result += u"previous_source      = %1\n"_q
		.arg(Quoted(PresetSourceName(state.previousSource)));
	result += u"focus_active         = %1\n"_q.arg(Boolean(state.focusActive));
	result += u"focus_seen           = %1\n"_q.arg(Boolean(state.focusSeen));
	result += u"schedule_paused      = %1\n"_q
		.arg(Boolean(state.schedulePaused));
	result += u"schedule_target      = %1\n"_q
		.arg(Quoted(state.scheduleTarget));
	result += u"peek_active          = %1\n"_q.arg(Boolean(state.peekActive));
	result += u"peek_deadline_unix   = %1\n"_q.arg(state.peekDeadlineUnix);

	const auto &cache = state.resolvedCache;
	if (!cache.valid()) {
		return result;
	}
	result += u"\n# The last resolution that worked, so a broken settings.toml "
		"leaves the\n# app running on known-good settings instead of on "
		"defaults.\n[resolved_cache]\n"_q;
	result += u"preset = %1\n"_q.arg(Quoted(cache.preset));
	result += u"view_name = %1\n"_q.arg(Quoted(cache.viewName));
	result += u"hide_everywhere = %1\n"_q
		.arg(Boolean(cache.hideEverywhere));
	result += SerializeLists(cache.lists);
	if (!cache.folders.empty()) {
		result += u"folders = [\n"_q;
		for (const auto &folder : cache.folders) {
			//: Each key is written only when the preset said something about
			//: it, because nothing and false mean different things all the way
			//: down - see PresetFolder.
			auto fields = u"name = %1"_q.arg(Quoted(folder.name));
			if (folder.show) {
				fields += u", show = %1"_q.arg(Boolean(*folder.show));
			}
			if (folder.notify) {
				fields += u", notify = %1"_q.arg(Boolean(*folder.notify));
			}
			if (folder.includeInMainView) {
				fields += u", include_in_main_view = %1"_q
					.arg(Boolean(*folder.includeInMainView));
			}
			result += u"  { %1 },\n"_q.arg(fields);
		}
		result += u"]\n"_q;
	}

	// Their own tables rather than inline ones: a view carries a nested array,
	// and TOML inline tables have to fit on a single line.
	for (const auto &view : cache.views) {
		result += u"\n[[resolved_cache.views]]\n"_q;
		result += u"name = %1\n"_q.arg(Quoted(view.name));
		if (!view.pinned.empty()) {
			auto ids = QStringList();
			for (const auto id : view.pinned) {
				ids.push_back(QString::number(id));
			}
			result += u"pinned = [%1]\n"_q.arg(ids.join(u", "_q));
		}
		result += SerializeLists(view.lists);
	}
	return result;
}

} // namespace Purple
