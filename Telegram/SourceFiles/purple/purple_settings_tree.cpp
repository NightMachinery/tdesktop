#include "purple/purple_settings_tree.h"

#include "purple/purple_keys_mx.h"

#include <QtCore/QRegularExpression>

#include <array>
#include <map>

namespace Purple::SettingsTree {
namespace {

constexpr auto kSections = std::array{
	"File & account", "Work Mode", "Schedule & Focus", "Peek",
	"Recent & suggestions", "Notifications & Last Seen", "Playback & playlists",
	"Recording & replies", "Screen time", "Diagnostics",
};

QString SectionKey(size_t index) {
	return u":"_q + QString::number(index);
}

size_t Section(const SettingAddress &address, bool known) {
	if (!known) {
		return kSections.size() - 1;
	}
	const auto root = std::get<QString>(address.front());
	if (root == u"lists"_q || root == u"presets"_q
		|| root == u"list_sets"_q || root == u"folder_sets"_q) return 1;
	if (root == u"schedule"_q || root == u"focus_sync"_q || root == u"devices"_q) return 2;
	if (root == u"peek"_q) return 3;
	if (root == u"recent"_q || root == u"suggestions"_q || root == u"overrides"_q) return 4;
	if (root == u"notifications"_q || root == u"last_seen"_q) return 5;
	if (root == u"playback"_q || root == u"playlists"_q) return 6;
	if (root == u"recording"_q || root == u"media_reply"_q) return 7;
	if (root == u"screen_time"_q) return 8;
	return 0;
}

QString SpellKey(const QString &key) {
	static const auto bare = QRegularExpression(u"^[A-Za-z0-9_-]+\\z"_q);
	if (bare.match(key).hasMatch()) {
		return key;
	}
	auto result = u"\""_q;
	for (const auto ch : key) {
		if (ch == u'\\' || ch == u'"') {
			result += u'\\';
			result += ch;
		} else if (ch.unicode() < 0x20 || ch.unicode() == 0x7f) {
			result += u"\\u"_q + QString::number(ch.unicode(), 16).rightJustified(4, u'0');
		} else {
			result += ch;
		}
	}
	return result + u'"';
}

QString Label(const SettingAddress &address, const SettingDescriptor *descriptor) {
	if (const auto position = std::get_if<size_t>(&address.back())) {
		return u"Item %1"_q.arg(*position + 1);
	}
	auto text = std::get<QString>(address.back());
	if (descriptor && descriptor->key == "*") {
		return text;
	}
	if (descriptor && descriptor->id == SettingId::ReadyTimeout) {
		return u"Ready timeout"_q;
	}
	if (text.endsWith(u"_p"_q)) {
		text.chop(2);
	}
	text.replace(u'_', u' ');
	if (!text.isEmpty()) {
		text[0] = text[0].toUpper();
	}
	return text;
}

bool LayerMatches(const SettingsLayerKey &key, const SettingAddress &address) {
	if (key.table.size() + 1 > address.size()) {
		return false;
	}
	for (auto i = size_t(0); i != key.table.size(); ++i) {
		if (address[i] != SettingAddressPart(key.table[i])) {
			return false;
		}
	}
	return address[key.table.size()] == SettingAddressPart(key.key);
}

QString InactiveReason(SettingsInactiveReason reason) {
	switch (reason) {
	case SettingsInactiveReason::AppWide: return u"App-wide setting"_q;
	case SettingsInactiveReason::UndefinedGlobalList: return u"List is absent from the app-wide file"_q;
	case SettingsInactiveReason::FollowListMembers: return u"This account follows global list members"_q;
	}
	return {};
}

QString DurationText(int64 seconds) {
	if (seconds && seconds % 3600 == 0) return u"%1 h"_q.arg(seconds / 3600);
	if (seconds && seconds % 60 == 0) return u"%1 min"_q.arg(seconds / 60);
	return u"%1 s"_q.arg(seconds);
}

bool IsSensitive(const Row &row) {
	return !row.descriptor || row.descriptor->codec == SettingCodec::Array
		|| row.descriptor->id == SettingId::ReplyText
		|| row.descriptor->id == SettingId::BudgetTarget;
}

std::pair<int64, int64> Bounds(const SettingDescriptor &descriptor) {
	const auto parts = QString::fromLatin1(descriptor.constraints).split(u".."_q);
	if (parts.size() != 2) return { 0, INT_MAX };
	return { parts.front().toLongLong(),
		parts.back() == u"INT_MAX"_q ? INT_MAX : parts.back().toLongLong() };
}

int DurationMinimum(SettingId id) {
	return id == SettingId::BinKeep ? 3600 : id == SettingId::LongNudge ? 60 : 1;
}

std::optional<int> Duration(const SettingDescriptor &descriptor, const QString &input) {
	const auto seconds = ParseDuration(input);
	if (!seconds) return std::nullopt;
	const auto options = Choices(descriptor);
	if (!options.isEmpty() && std::none_of(options.begin(), options.end(), [&](const auto &option) {
		return ParseDuration(option) == seconds;
	})) {
		return std::nullopt;
	}
	return (*seconds > 0 && *seconds < DurationMinimum(descriptor.id)) ? std::nullopt : seconds;
}

QString DefaultText(const Row &row, const Settings &global) {
	if (row.scope == SettingScope::PerAccount) {
		auto inherited = row;
		inherited.effective = EffectiveSettingValue(global, row.address);
		return inherited.effective.isValid() ? ValueText(inherited) : u"Context dependent"_q;
	}
	const auto fallback = QString::fromLatin1(row.descriptor->fallback);
	if (fallback == u"@required"_q) return u"Required"_q;
	if (fallback.startsWith(u'@')) return u"Context dependent"_q;
	if (row.descriptor->codec == SettingCodec::Boolean) return fallback == u"true"_q ? u"On"_q : u"Off"_q;
	if (IsSecondsInteger(row.descriptor->id)) return DurationText(fallback.toLongLong());
	if (fallback.startsWith(u'"') && fallback.endsWith(u'"')) {
		const auto text = fallback.mid(1, fallback.size() - 2);
		if (row.descriptor->codec == SettingCodec::Duration) {
			if (const auto seconds = ParseDuration(text)) return *seconds ? DurationText(*seconds) : u"Off"_q;
		}
		return text;
	}
	return fallback;
}

bool WarnsAbout(const ParseResult &parsed, const Row &row, int line) {
	const auto key = u"'"_q + QString::fromLatin1(row.descriptor->key) + u"'"_q;
	const auto table = std::get<QString>(row.address.front()) + u":"_q;
	const auto here = QRegularExpression(u"\\bline %1\\b"_q.arg(line));
	const auto anyLine = QRegularExpression(u"\\bline \\d+\\b"_q);
	return std::any_of(parsed.warnings.begin(), parsed.warnings.end(), [&](const auto &warning) {
		return warning.contains(key) && (here.match(warning).hasMatch()
			|| (row.address.size() == 2 && warning.startsWith(table)
				&& !anyLine.match(warning).hasMatch()));
	});
}

} // namespace

QString Address(const SettingAddress &address) {
	auto result = QString();
	for (const auto &part : address) {
		if (const auto key = std::get_if<QString>(&part)) {
			if (!result.isEmpty()) result += u'.';
			result += SpellKey(*key);
		} else {
			result += u"[%1]"_q.arg(std::get<size_t>(part));
		}
	}
	return result;
}

QString TomlPath(const SettingAddress &address) {
	auto names = SettingAddress();
	for (const auto &part : address) {
		if (std::holds_alternative<QString>(part)) names.push_back(part);
	}
	return Address(names);
}

QString ScopeText(SettingScope scope) {
	switch (scope) {
	case SettingScope::AppWide: return u"App-wide"_q;
	case SettingScope::PerAccount: return u"Per-account"_q;
	case SettingScope::FileLocal: return u"File-local"_q;
	case SettingScope::Unknown: return {};
	}
	return {};
}

QString ValueText(const Row &row) {
	if (!row.effective.isValid()) return {};
	const auto codec = row.descriptor->codec;
	if (codec == SettingCodec::Boolean) return row.effective.toBool() ? u"On"_q : u"Off"_q;
	if (codec == SettingCodec::Duration || IsSecondsInteger(row.descriptor->id)) {
		return (codec == SettingCodec::Duration && !row.effective.toInt())
			? u"Off"_q : DurationText(row.effective.toLongLong());
	}
	if (codec == SettingCodec::Time) {
		const auto minutes = row.effective.toInt();
		return u"%1:%2"_q.arg(minutes / 60, 2, 10, u'0').arg(minutes % 60, 2, 10, u'0');
	}
	return row.effective.toString();
}

std::vector<Row> Build(const LayerSettingsResult &layers, const std::vector<Source> &sources) {
	if (sources.empty()) return {};
	const auto &global = sources.front();
	const auto &own = sources.back();
	const auto sameFile = global.path == own.path;
	auto rows = std::map<QString, Row>();
	for (auto i = size_t(0); i != kSections.size(); ++i) {
		const auto key = SectionKey(i);
		rows[key] = { .key = key, .label = QString::fromLatin1(kSections[i]), .branch = true };
	}
	const auto ensure = [&](const SettingAddress &address) -> Row& {
		const auto id = Address(address);
		auto &row = rows[id];
		if (row.key.isEmpty()) {
			row.key = id;
			row.address = address;
			row.descriptor = FindSettingDescriptor(address);
			row.label = row.descriptor ? Label(address, row.descriptor) : id;
			const auto known = row.descriptor && row.descriptor->codec != SettingCodec::Retired
				&& row.descriptor->codec != SettingCodec::Future;
			const auto section = Section(address, known);
			auto parent = address;
			parent.pop_back();
			row.parent = (known && !parent.empty()) ? Address(parent) : SectionKey(section);
			if (row.descriptor) {
				row.help = QString::fromLatin1(row.descriptor->help);
			}
		}
		return row;
	};
	auto mapped = QSet<QString>();
	for (const auto &source : sources) {
		if (mapped.contains(source.path)) continue;
		mapped.insert(source.path);
		const auto map = MapSettingsSource(source.bytes);
		for (const auto &node : map.nodes) {
			auto &row = ensure(node.address);
			const auto inactive = (!sameFile && source.path == own.path)
				? std::find_if(layers.inactive.begin(), layers.inactive.end(), [&](const auto &key) {
					return LayerMatches(key.source, node.address);
				}) : layers.inactive.end();
			const auto isInactive = inactive != layers.inactive.end();
			const auto raw = node.token
				? QString::fromUtf8(source.bytes.mid(node.token->begin, node.token->end - node.token->begin))
				: QString();
			row.physical.push_back({
				.file = source.name + (isInactive ? u" · Inactive · "_q + InactiveReason(inactive->reason) : QString()),
				.path = source.path,
				.raw = raw,
				.line = node.line,
				.column = node.column,
				.inactive = isInactive,
			});
			if (row.descriptor && row.descriptor->codec == SettingCodec::Table) row.branch = true;
		}
	}
	const auto seed = [&](const SettingAddress &address) {
		auto prefix = SettingAddress();
		for (const auto &part : address) {
			prefix.push_back(part);
			ensure(prefix);
		}
	};
	for (const auto &read : layers.effective.reads) {
		const auto codec = DescribeSetting(read.id).codec;
		if (!read.present && (codec == SettingCodec::Retired || codec == SettingCodec::Future)) continue;
		seed(read.address);
	}
	for (auto i = 0; i != int(SettingId::Count); ++i) {
		const auto &descriptor = DescribeSetting(SettingId(i));
		if (descriptor.codec == SettingCodec::Retired || descriptor.codec == SettingCodec::Future) continue;
		for (const auto &pattern : QString::fromLatin1(descriptor.patterns).split(u'|')) {
			if (pattern.contains(u'*') || pattern.contains(u'[')) continue;
			auto address = SettingAddress();
			for (const auto &part : pattern.split(u'.')) address.push_back(part);
			seed(address);
		}
	}
	for (auto &[key, row] : rows) {
		if (!row.parent.isEmpty() && !rows.contains(row.parent)) {
			row.parent = SectionKey(Section(row.address, row.descriptor != nullptr));
		}
		if (!row.parent.isEmpty()) rows[row.parent].branch = true;
		if (!row.descriptor || !IsScalar(row.descriptor->codec)) continue;
		const auto read = std::find_if(layers.effective.reads.rbegin(), layers.effective.reads.rend(),
			[&](const SettingRead &item) { return item.address == row.address && item.effective.isValid(); });
		row.effective = read != layers.effective.reads.rend()
			? read->effective : EffectiveSettingValue(layers.effective.settings, row.address);
		row.scope = SettingsScope(row.address);
		const auto &file = row.scope == SettingScope::AppWide ? global : own;
		row.destination = file.name;
		row.editable = row.scope != SettingScope::Unknown && file.writable && !file.recovery;
		const auto active = std::find_if(row.physical.rbegin(), row.physical.rend(), [&](const PhysicalValue &value) {
			return !value.inactive && (row.scope != SettingScope::FileLocal || value.path == own.path);
		});
		const auto explicitSource = active != row.physical.rend();
		row.explicitHere = explicitSource && active->path == file.path;
		const auto inherited = explicitSource && !sameFile && active->path == global.path
			&& row.scope != SettingScope::AppWide;
		const auto &globalParse = layers.global.usedGood && layers.global.good
			? layers.global.good->parsed : layers.global.current.parsed;
		row.defaultValue = DefaultText(row, globalParse.settings);
		row.resetLabel = !sameFile && row.scope == SettingScope::PerAccount
			? u"Use global account value"_q
			: (row.descriptor->fallback.starts_with("@") ? u"Use local fallback"_q : u"Use built-in"_q);
		const auto builtIn = QString::fromLatin1(row.descriptor->fallback);
		const auto candidateDefault = builtIn.startsWith(u'"') ? builtIn.mid(1, builtIn.size() - 2) : builtIn;
		const auto normalized = row.descriptor->codec == SettingCodec::Duration
			? (ParseDuration(candidateDefault) == std::optional<int>(row.effective.toInt()))
			: row.effective.toString() == candidateDefault;
		const auto inheritedDefault = EffectiveSettingValue(globalParse.settings, row.address);
		const auto isDefault = row.effective.isValid() && (row.scope == SettingScope::PerAccount
			&& !sameFile && !inherited && inheritedDefault.isValid()
			? row.effective == inheritedDefault : normalized);
		if (explicitSource) {
			const auto &source = active->path == global.path ? global : own;
			row.source = (source.recovery ? u"Last-good copy"_q
				: inherited ? (isDefault ? u"Inherited · same value"_q : u"Inherited"_q)
				: isDefault ? u"Explicit · default value"_q : u"Explicit"_q)
				+ u" · "_q + source.name;
			const auto &report = active->path == global.path ? layers.global : layers.account;
			const auto &parsed = report.usedGood && report.good ? report.good->parsed : report.current.parsed;
			row.invalid = (!row.effective.isValid() && !active->raw.isEmpty())
				|| WarnsAbout(parsed, row, active->line);
		} else {
			row.source = builtIn.startsWith(u'@') ? u"Local fallback"_q : u"Built-in"_q;
		}
	}
	for (const auto *report : { &layers.global, &layers.account }) {
		const auto &parse = report->current;
		if (parse.path.isEmpty()) continue;
		auto messages = parse.parsed.warnings;
		if (!parse.parsed.error.isEmpty() && parse.present) messages.insert(messages.begin(), parse.parsed.error);
		if (parse.parsed.traceIncomplete) messages.push_back(u"Some settings reads could not be traced."_q);
		for (auto i = size_t(0); i != messages.size(); ++i) {
			const auto key = u":diagnostic:"_q + parse.path + u':' + QString::number(i);
			rows[key] = { .key = key, .parent = SectionKey(9), .label = u"Source warning"_q,
				.source = parse.path, .help = messages[i], .invalid = true };
		}
	}
	auto result = std::vector<Row>();
	result.reserve(rows.size());
	for (auto &[key, row] : rows) result.push_back(std::move(row));
	return result;
}

VisibleRows View(const std::vector<Row> &rows, const QSet<QString> &expanded,
		const QString &query, const QString &anchor) {
	const auto tokens = ModalMx::ParseTokens(query);
	auto included = QSet<QString>();
	auto parents = std::map<QString, std::vector<size_t>>();
	auto byKey = std::map<QString, const Row*>();
	for (auto i = size_t(0); i != rows.size(); ++i) {
		parents[rows[i].parent].push_back(i);
		byKey[rows[i].key] = &rows[i];
	}
	for (auto &[key, children] : parents) {
		std::stable_sort(children.begin(), children.end(), [&](size_t a, size_t b) {
			return rows[a].address < rows[b].address;
		});
	}
	auto result = VisibleRows();
	if (!tokens.empty()) {
		for (const auto &row : rows) {
			if (row.branch) continue;
			const auto section = SectionKey(row.address.empty() ? 9 : Section(row.address, row.descriptor != nullptr));
			const auto haystack = (row.key + u' ' + row.label + u' ' + row.source + u' '
				+ ScopeText(row.scope) + u' ' + byKey.at(section)->label + u' '
				+ (row.descriptor ? row.help : QString()) + u' '
				+ (IsSensitive(row) ? QString() : ValueText(row))).toCaseFolded();
			const auto match = std::all_of(tokens.begin(), tokens.end(), [&](const auto &token) {
				return ModalMx::MatchToken(token, haystack, {});
			});
			result.matches += match ? 1 : 0;
			if (match || row.key == anchor) {
				for (auto at = &row; at && !included.contains(at->key);) {
					included.insert(at->key);
					const auto p = byKey.find(at->parent);
					at = p != byKey.end() ? p->second : nullptr;
				}
			}
		}
	}
	const auto append = [&](auto &&self, const QString &parent) -> void {
		const auto found = parents.find(parent);
		if (found == parents.end()) return;
		for (const auto i : found->second) {
			const auto &row = rows[i];
			if (!tokens.empty() && !included.contains(row.key)) continue;
			result.rows.push_back(i);
			if (!tokens.empty() || expanded.contains(row.key)) self(self, row.key);
		}
	};
	append(append, {});
	return result;
}

QStringList Choices(const SettingDescriptor &descriptor) {
	const auto spec = QString::fromLatin1(descriptor.constraints);
	return (descriptor.codec == SettingCodec::Choice || (descriptor.codec == SettingCodec::Duration
		&& spec.contains(u'/'))) ? spec.split(u'/', Qt::SkipEmptyParts) : QStringList();
}

bool IsSecondsInteger(SettingId id) {
	return id == SettingId::StartDelay || id == SettingId::ReadyTimeout;
}

bool IsScalar(SettingCodec codec) {
	return codec == SettingCodec::Boolean || codec == SettingCodec::Integer
		|| codec == SettingCodec::String || codec == SettingCodec::Choice
		|| codec == SettingCodec::Duration || codec == SettingCodec::Time;
}

bool CanReset(const SettingDescriptor &descriptor) {
	return descriptor.fallback != "@required";
}

std::optional<SettingScalar> Decode(const SettingDescriptor &descriptor, const QString &input) {
	const auto text = input.trimmed();
	switch (descriptor.codec) {
	case SettingCodec::Boolean:
		if (text == u"true"_q || text == u"false"_q) return SettingScalar(text == u"true"_q);
		break;
	case SettingCodec::Integer: {
		auto ok = false;
		auto value = text.toLongLong(&ok);
		if (!ok && IsSecondsInteger(descriptor.id)) {
			if (const auto seconds = ParseDuration(text)) {
				value = *seconds;
				ok = true;
			}
		}
		const auto [lower, upper] = Bounds(descriptor);
		if (ok && value >= lower && value <= upper) return SettingScalar(int64(value));
	} break;
	case SettingCodec::Choice:
		if (Choices(descriptor).contains(text)) return SettingScalar(text);
		break;
	case SettingCodec::Duration:
		if (const auto seconds = Duration(descriptor, text)) {
			return SettingScalar(*seconds == 0 ? u"off"_q : u"%1s"_q.arg(*seconds));
		}
		break;
	case SettingCodec::Time:
		if (ParseTimeOfDay(text)) return SettingScalar(text);
		break;
	case SettingCodec::String:
		if (input.isValidUtf16() && (CanReset(descriptor) || !text.isEmpty())
			&& (descriptor.id != SettingId::ReplyText || (!text.isEmpty() && input.size() <= 4096))) {
			return SettingScalar(input);
		}
		break;
	default: break;
	}
	return std::nullopt;
}

std::optional<SettingScalar> Step(const SettingDescriptor &descriptor, const QVariant &effective, int delta) {
	if (!effective.isValid()) return std::nullopt;
	const auto value = effective.toLongLong();
	const auto direction = delta > 0 ? 1 : -1;
	if (descriptor.codec == SettingCodec::Integer) {
		const auto [lower, upper] = Bounds(descriptor);
		const auto next = std::clamp(value + direction, lower, upper);
		return next != value ? std::make_optional(SettingScalar(int64(next))) : std::nullopt;
	}
	const auto options = Choices(descriptor);
	if (descriptor.codec != SettingCodec::Duration) return std::nullopt;
	if (options.isEmpty()) {
		const auto minimum = DurationMinimum(descriptor.id);
		const auto next = !value && direction > 0 ? minimum
			: value == minimum && direction < 0 ? 0
			: std::clamp(value + direction, 0LL, qlonglong(INT_MAX));
		return next != value ? Decode(descriptor, u"%1s"_q.arg(next)) : std::nullopt;
	}
	const auto at = std::find_if(options.begin(), options.end(), [&](const QString &text) {
		return ParseDuration(text) == value;
	});
	if (at == options.end()) return std::nullopt;
	const auto next = at - options.begin() + direction;
	return (next >= 0 && next < options.size()) ? Decode(descriptor, options[next]) : std::nullopt;
}

EditorKey ShouldHandleEditorKey(bool preedit, bool submit, bool singleLineOrExplicitSubmit, bool back) {
	if (preedit) return EditorKey::Native;
	if (back) return EditorKey::Back;
	return (submit && singleLineOrExplicitSubmit) ? EditorKey::Stage : EditorKey::Native;
}

std::optional<Rgb> ParseFixtureColor(const QString &hex) {
	static const auto pattern = QRegularExpression(u"^#[0-9a-fA-F]{6}\\z"_q);
	if (!pattern.match(hex).hasMatch()) return std::nullopt;
	return Rgb{ hex.mid(1, 2).toInt(nullptr, 16), hex.mid(3, 2).toInt(nullptr, 16),
		hex.mid(5, 2).toInt(nullptr, 16) };
}

QString FixtureColorHex(Rgb color) {
	return u"#%1%2%3"_q.arg(color.red, 2, 16, u'0').arg(color.green, 2, 16, u'0')
		.arg(color.blue, 2, 16, u'0').toUpper();
}

Rgb EditFixtureColor(Rgb color, int channel, int delta) {
	const auto parts = std::array{ &color.red, &color.green, &color.blue };
	if (channel >= 0 && channel < int(parts.size())) {
		auto &value = *parts[channel];
		value = int(std::clamp(int64(value) + delta, int64(0), int64(255)));
	}
	return color;
}

} // namespace Purple::SettingsTree
