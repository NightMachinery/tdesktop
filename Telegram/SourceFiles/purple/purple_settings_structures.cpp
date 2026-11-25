#include "purple/purple_settings_structures.h"

#include "purple/purple_peer_token.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QStringDecoder>

#define TOML_EXCEPTIONS 0
#include "tomlplusplus/toml.hpp"

namespace Purple::SettingsStructures {
namespace {

using Op = SettingsCollectionOp;

bool Prefix(const SettingAddress &prefix, const SettingAddress &address) {
	return address.size() >= prefix.size()
		&& std::equal(prefix.begin(), prefix.end(), address.begin());
}

const toml::node *At(const toml::table &table, const SettingAddress &address) {
	auto node = static_cast<const toml::node*>(&table);
	for (const auto &part : address) {
		if (const auto key = std::get_if<QString>(&part)) {
			const auto object = node->as_table();
			node = object ? object->get(key->toStdString()) : nullptr;
		} else {
			const auto array = node->as_array();
			node = array ? array->get(std::get<size_t>(part)) : nullptr;
		}
		if (!node) break;
	}
	return node;
}

QString Text(const toml::node *node) {
	const auto value = node ? node->value<std::string>() : std::nullopt;
	return value ? QString::fromStdString(*value) : QString();
}

QString Property(const toml::node *node, const char *field) {
	const auto table = node ? node->as_table() : nullptr;
	return Text(table ? table->get(field) : nullptr);
}

bool RuleCollection(SettingId id) {
	return id == SettingId::FlatRules || id == SettingId::NestedRules;
}

bool ListOrder(SettingId id);
bool FolderOrder(SettingId id);

bool Recognized(SettingId id, const toml::node *node) {
	if (!node) return false;
	if (RuleCollection(id)) {
		const auto from = ParseTimeOfDay(Property(node, "from"));
		const auto to = ParseTimeOfDay(Property(node, "to"));
		return from && to && from != to && !Property(node, "preset").isEmpty();
	}
	if (id == SettingId::Budgets) {
		return !Property(node, "target").isEmpty() && ParseDuration(Property(node, "per_day")).has_value();
	}
	if (id == SettingId::Rulesets || id == SettingId::Views) {
		return !Property(node, "name").isEmpty();
	}
	if (ListOrder(id) || FolderOrder(id)) {
		return node->is_table() || SpreadReference(Text(node)).has_value();
	}
	return node->is_string() || node->is_table() || (node->is_integer()
		&& (id == SettingId::Members || id == SettingId::PresetPins
			|| id == SettingId::ViewPins || id == SettingId::PreviewAlways));
}

QString ItemLabel(const toml::node *node, const QByteArray &literal, const SettingAddressPart &last) {
	if (const auto name = std::get_if<QString>(&last)) return *name;
	for (const auto field : { "name", "list", "target", "preset" }) {
		const auto value = Property(node, field);
		if (!value.isEmpty()) return value;
	}
	auto title = QString::fromUtf8(literal).simplified();
	if (title.size() > 100) title = title.left(99) + u'…';
	return title.isEmpty() ? u"Record %1"_q.arg(std::get<size_t>(last) + 1) : title;
}

QByteArray Token(const QByteArray &bytes, const SettingsSourceNode &node) {
	return node.token ? bytes.mid(node.token->begin, node.token->end - node.token->begin) : QByteArray();
}

SettingAddress ReplayAnchor(const SettingsEdit &edit) {
	auto anchor = edit.address;
	const auto ordinal = std::find_if(anchor.begin(), anchor.end(), [](const auto &value) {
		return std::holds_alternative<size_t>(value);
	});
	anchor.erase(ordinal, anchor.end());
	if (std::holds_alternative<SettingsObjectEdit>(edit.operation) && !anchor.empty()) anchor.pop_back();
	return anchor;
}

using Signature = std::vector<std::tuple<SettingAddress, SettingsSourceKind, QByteArray>>;

Signature Sign(const QByteArray &bytes, const SettingsSourceMap &map, const SettingAddress &anchor) {
	auto result = Signature();
	for (const auto &node : map.nodes) {
		if (Prefix(anchor, node.address)) result.emplace_back(node.address, node.kind, Token(bytes, node));
	}
	return result;
}

bool ListOrder(SettingId id) {
	return id == SettingId::PresetListOrder || id == SettingId::ViewListOrder || id == SettingId::ListSetOrder;
}

bool FolderOrder(SettingId id) {
	return id == SettingId::PresetFolders || id == SettingId::FolderSetOrder;
}

bool PresetUse(SettingId id) {
	return id == SettingId::RulePreset || id == SettingId::RulesetOutside || id == SettingId::ScheduleOutside
		|| id == SettingId::EnterPreset || id == SettingId::ExitPreset || id == SettingId::BudgetTarget;
}

QString RefLiteral(const SettingDescriptor *setting, const SettingDescriptor *array,
		SettingId object, const QString &name) {
	if (object == SettingId::ListObject && setting && setting->id == SettingId::EntryList) {
		return name;
	}
	if ((object == SettingId::ListSetObject && array && ListOrder(array->id))
		|| (object == SettingId::FolderSetObject && array && FolderOrder(array->id))) {
		return u'*' + name;
	}
	if (object == SettingId::PresetObject && setting && PresetUse(setting->id)) {
		return setting->id == SettingId::BudgetTarget ? u"preset:"_q + name : name;
	}
	return {};
}

} // namespace

bool IsNamedContainer(SettingId id) {
	return id == SettingId::Lists || id == SettingId::ListSets || id == SettingId::FolderSets
		|| id == SettingId::Presets || id == SettingId::MediaSets;
}

bool IsNamedObject(SettingId id) {
	return id == SettingId::ListObject || id == SettingId::ListSetObject || id == SettingId::FolderSetObject
		|| id == SettingId::PresetObject || id == SettingId::MediaSetObject;
}

bool NeedsCopyName(SettingId id) {
	return id == SettingId::Views || id == SettingId::Rulesets;
}

std::vector<Item> Items(const QByteArray &bytes, const SettingAddress &container, bool named) {
	const auto map = MapSettingsSource(bytes);
	const auto parsed = toml::parse(std::string_view(bytes.constData(), size_t(bytes.size())));
	if (!map.ok() || !parsed) return {};
	const auto descriptor = FindSettingDescriptor(container);
	if (!descriptor) return {};
	auto result = std::vector<Item>();
	for (const auto &node : map.nodes) {
		if (!Prefix(container, node.address) || node.address.size() != container.size() + 1
			|| named != std::holds_alternative<QString>(node.address.back())) {
			continue;
		}
		const auto value = At(parsed.table(), node.address);
		const auto literal = Token(bytes, node);
		const auto integer = value ? value->value<int64>() : std::nullopt;
		result.push_back({ .address = node.address, .literal = literal,
			.value = integer ? QString::number(*integer) : Text(value),
			.label = ItemLabel(value, literal, node.address.back()), .line = node.line,
			.recognized = named ? value && value->is_table() : Recognized(descriptor->id, value),
			.inlineValue = node.token.has_value() && node.kind != SettingsSourceKind::ArrayTableHeader });
	}
	if (!named) {
		std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
			return std::get<size_t>(a.address.back()) < std::get<size_t>(b.address.back());
		});
	}
	return result;
}

SettingsEdit ArrayEdit(SettingAddress address, SettingsCollectionOp operation, size_t index,
		size_t destination, SettingsArrayValue value, std::optional<QString> name) {
	return { std::move(address), SettingsArrayEdit{
		operation, index, destination, std::move(value), std::move(name) } };
}

bool Overnight(const QString &from, const QString &to) {
	const auto start = ParseTimeOfDay(from);
	const auto end = ParseTimeOfDay(to);
	return start && end && *end < *start;
}

QByteArray RecordTemplate(SettingId id) {
	if (RuleCollection(id)) return "{ from = \"22:00\", to = \"06:00\", preset = \"normal\" }";
	if (id == SettingId::Rulesets) return "{ name = \"weekdays\", device = \"desktop\", mode = \"enabled\", rules = [] }";
	if (id == SettingId::Budgets) return "{ target = \"all\", per_day = \"45m\", mode = \"soft\" }";
	if (id == SettingId::Views) return "{ name = \"view\", list_order = [] }";
	if (id == SettingId::Presets || id == SettingId::ListSets) return "{ list_order = [] }";
	if (id == SettingId::FolderSets) return "{ folders = [] }";
	if (id == SettingId::Lists || id == SettingId::MediaSets) return "{}";
	if (ListOrder(id)) return "{ list = \"list\", notify_p = true }";
	if (FolderOrder(id)) return "{ name = \"Inbox\", enabled_p = true }";
	return "\"\"";
}

std::vector<RecordField> RecordFields(SettingId id) {
	using Kind = FieldKind;
	if (RuleCollection(id)) {
		return { { u"from"_q, u"From"_q, Kind::Time, u"22:00"_q, {}, true },
			{ u"to"_q, u"To"_q, Kind::Time, u"06:00"_q, {}, true },
			{ u"preset"_q, u"Preset"_q, Kind::String, u"normal"_q, {}, true },
			{ u"days"_q, u"Start days"_q, Kind::Weekdays },
			{ u"enabled_p"_q, u"Enabled"_q, Kind::Boolean, u"true"_q } };
	}
	if (id == SettingId::Rulesets) {
		return { { u"name"_q, u"Name"_q, Kind::String, {}, {}, true },
			{ u"device"_q, u"Device class or ID"_q, Kind::String, u"desktop"_q },
			{ u"mode"_q, u"Mode"_q, Kind::Choice, u"enabled"_q,
				{ u"enabled"_q, u"disabled"_q, u"always"_q } },
			{ u"outside"_q, u"Outside preset"_q, Kind::String } };
	}
	if (id == SettingId::Budgets) {
		return { { u"target"_q, u"Target"_q, Kind::String, u"all"_q, {}, true },
			{ u"per_day"_q, u"Per day"_q, Kind::Duration, u"45m"_q, {}, true },
			{ u"mode"_q, u"Mode"_q, Kind::Choice, u"soft"_q, { u"soft"_q, u"hard"_q } },
			{ u"snooze"_q, u"Snooze"_q, Kind::Duration, u"5m"_q },
			{ u"snoozes_per_day"_q, u"Snoozes per day"_q, Kind::Integer, u"2"_q } };
	}
	if (id == SettingId::Views) {
		return { { u"name"_q, u"View name"_q, Kind::String, {}, {}, true },
			{ u"list_order"_q, u"First list"_q, Kind::FirstList, {}, {}, true } };
	}
	if (id == SettingId::Presets || id == SettingId::ListSets) {
		return { { u"list_order"_q, u"First list"_q, Kind::FirstList } };
	}
	if (id == SettingId::FolderSets) {
		return { { u"folders"_q, u"First folder"_q, Kind::FirstFolder } };
	}
	if (id == SettingId::Lists) return { { u"title"_q, u"Display title"_q, Kind::String } };
	if (id == SettingId::MediaSets) return { { u"list"_q, u"Display label"_q, Kind::String } };
	return {};
}

std::optional<QByteArray> EncodeRecord(SettingId id, const QStringList &values, QString &error) {
	const auto fields = RecordFields(id);
	if (values.size() != qsizetype(fields.size())) return std::nullopt;
	const auto quote = [](const QString &input) {
		auto bytes = QByteArray("\"");
		for (const auto ch : input.toUtf8()) {
			const auto code = static_cast<unsigned char>(ch);
			if (ch == '\\' || ch == '"') bytes += '\\';
			if (code < 0x20 || code == 0x7f) {
				bytes += "\\u00" + QByteArray::number(code, 16).rightJustified(2, '0');
			} else {
				bytes += ch;
			}
		}
		return bytes + '"';
	};
	if (RuleCollection(id) && ParseTimeOfDay(values[0]) == ParseTimeOfDay(values[1])) {
		error = u"Start and end times must differ."_q;
		return std::nullopt;
	}
	if (id == SettingId::Budgets) {
		const auto target = values[0].trimmed();
		const auto valid = target == u"all"_q || (target.startsWith(u"chat:"_q) && ParsePeerLiteral(PeerField::Budget, target))
			|| (target.startsWith(u"kind:"_q) && ParseScreenTimeKind(target.mid(5)))
			|| (target.startsWith(u"preset:"_q) && !target.mid(7).trimmed().isEmpty());
		if (!valid) {
			error = u"Use all, chat:ID, kind:NAME or preset:NAME."_q;
			return std::nullopt;
		}
	}
	auto result = QByteArray("{ ");
	for (auto i = 0; i != int(fields.size()); ++i) {
		const auto &field = fields[i];
		const auto value = values[i].trimmed();
		if (value.isEmpty() && !field.required && field.kind != FieldKind::FirstList
			&& field.kind != FieldKind::FirstFolder) continue;
		auto allowed = !value.isEmpty() || !field.required;
		auto literal = quote(value);
		switch (field.kind) {
		case FieldKind::Choice: allowed = field.choices.contains(value); break;
		case FieldKind::Time: allowed = ParseTimeOfDay(value).has_value(); break;
		case FieldKind::Duration: allowed = ParseDuration(value).has_value(); break;
		case FieldKind::Integer: {
			auto ok = false;
			const auto number = value.toInt(&ok);
			allowed = ok && number >= 0;
			literal = QByteArray::number(number);
		} break;
		case FieldKind::Boolean:
			allowed = value == u"true"_q || value == u"false"_q;
			literal = value.toUtf8();
			break;
		case FieldKind::Weekdays: {
			literal = "[";
			for (const auto &day : value.split(u',', Qt::SkipEmptyParts)) {
				allowed = allowed && ParseWeekday(day).has_value();
				if (literal.size() > 1) literal += ", ";
				literal += quote(day);
			}
			literal += ']';
		} break;
		case FieldKind::FirstList:
		case FieldKind::FirstFolder:
			literal = value.isEmpty() ? QByteArray("[]")
				: (field.kind == FieldKind::FirstList ? QByteArray("[{ list = ") : QByteArray("[{ name = "))
					+ quote(value) + " }]";
			break;
		case FieldKind::String: break;
		}
		if (!allowed || !value.isValidUtf16()) {
			error = u"Enter a valid value for %1."_q.arg(field.label);
			return std::nullopt;
		}
		if (result.size() > 2) result += ", ";
		result += field.key.toUtf8() + " = " + literal;
	}
	return result + " }";
}

QString RecordEffect(const QByteArray &source, const SettingAddress &address) {
	const auto parsed = toml::parse(std::string_view(source.constData(), size_t(source.size())));
	if (!parsed) return {};
	const auto node = At(parsed.table(), address);
	const auto from = Property(node, "from");
	const auto to = Property(node, "to");
	if (from.isEmpty() || to.isEmpty()) return {};
	return Overnight(from, to) ? u"%1 to %2 the next day, counted from the selected start days."_q.arg(from, to)
		: u"%1 to %2 on the selected days."_q.arg(from, to);
}

ReferenceChanges RewriteReferences(const QByteArray &bytes, const SettingAddress &object,
		const QString &replacement) {
	const auto descriptor = FindSettingDescriptor(object);
	const auto parsed = toml::parse(std::string_view(bytes.constData(), size_t(bytes.size())));
	auto result = ReferenceChanges();
	if (!descriptor || !parsed || object.empty() || !IsNamedObject(descriptor->id)) return result;
	const auto name = std::get<QString>(object.back());
	for (const auto &node : MapSettingsSource(bytes).nodes) {
		if (Prefix(object, node.address)) continue;
		auto array = node.address;
		const auto index = std::get_if<size_t>(&node.address.back());
		if (index) array.pop_back();
		const auto arrayField = index ? FindSettingDescriptor(array) : nullptr;
		const auto field = FindSettingDescriptor(node.address);
		const auto reference = RefLiteral(field, arrayField, descriptor->id, name);
		if (reference.isEmpty() || Text(At(parsed.table(), node.address)).compare(reference, Qt::CaseInsensitive)) continue;
		result.uses.push_back(node.address);
		if (replacement.isEmpty()) continue;
		const auto next = RefLiteral(field, arrayField, descriptor->id, replacement);
		result.edits.push_back(index
			? ArrayEdit(std::move(array), Op::Replace, *index, 0, next)
			: SettingsEdit{ node.address, std::optional<SettingScalar>(next) });
	}
	return result;
}

bool CanReplay(const QByteArray &opened, const QByteArray &current, const std::vector<SettingsEdit> &edits) {
	auto before = opened;
	auto after = current;
	for (const auto &edit : edits) {
		const auto baseMap = MapSettingsSource(before);
		const auto diskMap = MapSettingsSource(after);
		const auto anchor = ReplayAnchor(edit);
		if (!baseMap.ok() || !diskMap.ok() || Sign(before, baseMap, anchor) != Sign(after, diskMap, anchor)) {
			return false;
		}
		auto first = ApplySettingsEdits(before, baseMap.revision, { edit });
		auto second = ApplySettingsEdits(after, diskMap.revision, { edit });
		if (!first.ok() || !second.ok()) return false;
		before = std::move(first.bytes);
		after = std::move(second.bytes);
	}
	return !edits.empty();
}

bool SourceMatches(const QByteArray &opened, const QByteArray &current, const SettingAddress &address) {
	const auto first = MapSettingsSource(opened);
	const auto second = MapSettingsSource(current);
	return first.ok() && second.ok() && Sign(opened, first, address) == Sign(current, second, address);
}

std::optional<SettingsArrayValue> ParsePeerLiteral(PeerField field, const QString &text) {
	const auto value = text.trimmed();
	if (field == PeerField::Media) {
		const auto peer = ParsePeerToken(value);
		const auto canonical = peer && peer->kind != PeerTokenKind::Self ? PeerTokenText(*peer) : std::nullopt;
		return canonical ? std::make_optional(SettingsArrayValue(*canonical)) : std::nullopt;
	}
	if (field == PeerField::Notifications
		&& (value == u"MAGIC_BOTS"_q || value == u"MAGIC_CHANNELS"_q)) {
		return SettingsArrayValue(value);
	}
	auto ok = false;
	if (field == PeerField::Notifications) {
		static const auto format = QRegularExpression(u"^(private|user|bot|group|channel):([1-9][0-9]*)\\z"_q);
		const auto match = format.match(value);
		const auto id = match.captured(2).toLongLong(&ok);
		return ok && id > 0 ? std::make_optional(SettingsArrayValue(value)) : std::nullopt;
	}
	if (field == PeerField::Budget) {
		if (!value.startsWith(u"chat:"_q)) return std::nullopt;
		const auto id = value.mid(5).toLongLong(&ok);
		return ok && id ? std::make_optional(SettingsArrayValue(u"chat:%1"_q.arg(id))) : std::nullopt;
	}
	const auto id = value.toLongLong(&ok);
	return ok ? std::make_optional(SettingsArrayValue(int64(id))) : std::nullopt;
}

QString PeerLabel(const QString &literal, const QString &name, bool visible) {
	return visible && !name.isEmpty() ? name : u"Unavailable chat · %1"_q.arg(literal);
}

QStringList ReviewLines(const ConfigTextDiff &diff, const QByteArray &before, const QByteArray &after) {
	constexpr auto kPageLines = 100;
	auto pages = QStringList();
	const auto append = [&](const QString &title, const QStringList &lines) {
		for (auto from = qsizetype(0); from < lines.size(); from += kPageLines) {
			pages.push_back(title + u'\n' + lines.mid(from, kPageLines).join(u'\n'));
		}
	};
	if (diff.truncated) {
		append(u"Original file · complete text"_q, QString::fromUtf8(before).split(u'\n'));
		append(u"Candidate file · complete text"_q, QString::fromUtf8(after).split(u'\n'));
	} else {
		for (const auto &hunk : diff.hunks) {
			auto lines = QStringList();
			for (const auto &line : hunk.lines) {
				const auto marker = line.kind == ConfigDiffLineKind::Added ? u'+' :
					line.kind == ConfigDiffLineKind::Removed ? u'-' : u' ';
				lines.push_back(QString(marker) + u' ' + line.text);
			}
			append(u"@@ -%1,%2 +%3,%4 @@"_q.arg(hunk.oldStart).arg(hunk.oldCount)
				.arg(hunk.newStart).arg(hunk.newCount), lines);
		}
	}
	return pages;
}

QString SourceExcerpt(const QByteArray &bytes, int line, int column) {
	if (line <= 0) return {};
	auto decoder = QStringDecoder(QStringDecoder::Utf8);
	const auto text = QString(decoder(bytes));
	if (decoder.hasError()) return {};
	const auto lines = text.split(u'\n');
	auto result = QStringList();
	for (auto i = std::max(0, line - 3); i < lines.size() && i < line + 2; ++i) {
		result.push_back(u"%1  %2"_q.arg(i + 1).arg(lines[i]));
		if (i + 1 == line && column > 0) result.push_back(u"Column %1"_q.arg(column));
	}
	return result.join(u'\n');
}

} // namespace Purple::SettingsStructures
