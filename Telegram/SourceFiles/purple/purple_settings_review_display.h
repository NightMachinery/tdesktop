#pragma once

#include "purple/purple_config_diff.h"
#include "purple/purple_keys_colors.h"

#include <QtCore/QStringList>
#include <QtGui/QColor>

#include <algorithm>
#include <map>
#include <tuple>

namespace Purple::SettingsReviewDisplay {

struct Values {
	std::optional<QString> before;
	std::optional<QString> after;
};

struct DiffInk {
	QColor removed;
	QColor added;
};

inline DiffInk InkFor(QColor surface) {
	return {
		ModalUi::ReadableText(QColor(0xb9, 0x3a, 0x42), surface),
		ModalUi::ReadableText(QColor(0x31, 0x81, 0x52), surface),
	};
}

struct Address {
	QString table;
	QString key;
	ConfigChangeAddressKind kind;

	bool operator<(const Address &other) const {
		return std::tie(table, key, kind) < std::tie(other.table, other.key, other.kind);
	}

	QString path() const {
		return key.isEmpty() ? table : table.isEmpty() ? key : table + u'.' + key;
	}
};

using Rows = std::map<QString, std::vector<Values>>;
using Changes = std::map<Address, Rows>;

struct Rename {
	QString from;
	QString to;
	QString fromName;
	QString toName;
	QStringList references;
};

inline Changes Flatten(const ConfigChangeSummary &summary) {
	auto result = Changes();
	for (const auto &entry : summary.entries) {
		for (const auto &key : entry.keys) {
			result[{ key.table, key.key, key.addressKind }][entry.label].push_back({
				key.beforeValue, key.afterValue,
			});
		}
	}
	return result;
}

inline bool Owns(const QString &object, const QString &path) {
	return path == object || path.startsWith(object + u'.');
}

inline Address RenamedAddress(Address address, const Rename &rename) {
	address.table = rename.to + address.table.mid(rename.from.size());
	return address;
}

inline const Values *SingleValue(const Rows &rows) {
	return (rows.size() == 1 && rows.begin()->second.size() == 1)
		? &rows.begin()->second.front()
		: nullptr;
}

inline bool Removal(const Values &value) {
	return value.before && !value.after;
}

inline bool CanPair(const Values *before, const Values *after) {
	return before && Removal(*before) && after && !after->before && after->after;
}

inline bool IsPureRename(const Changes &changes, const Rename &rename) {
	auto matched = size_t(0);
	auto destinations = size_t(0);
	for (const auto &[address, rows] : changes) {
		if (Owns(rename.to, address.table)) {
			++destinations;
		}
		if (!Owns(rename.from, address.table)) continue;
		const auto found = changes.find(RenamedAddress(address, rename));
		const auto before = SingleValue(rows);
		const auto after = (found == changes.end()) ? nullptr : SingleValue(found->second);
		if (!CanPair(before, after) || before->before != after->after) return false;
		++matched;
	}
	return matched != 0 && matched == destinations;
}

inline bool IsRenameOnlyLoss(
		const ConfigImpactFinding &finding,
		const Changes &changes,
		const std::vector<Rename> &renames) {
	if (finding.change.kind != ConfigChangeKind::Removed || !finding.account.isEmpty()) {
		return false;
	}
	return std::any_of(renames.begin(), renames.end(), [&](const auto &rename) {
		const auto &table = finding.change.table;
		const auto objectLoss = (finding.kind == ConfigImpactKind::ListLoss
			|| finding.kind == ConfigImpactKind::PresetLoss) && table == rename.from;
		const auto settingLoss = (finding.kind == ConfigImpactKind::AppWideLoss)
			&& Owns(rename.from, table);
		return (objectLoss || settingLoss) && IsPureRename(changes, rename);
	});
}

inline Changes RebaseRename(const Changes &changes, const Rename &rename) {
	auto result = changes;
	for (const auto &[address, rows] : changes) {
		if (!Owns(rename.from, address.table)) continue;
		const auto moved = RenamedAddress(address, rename);
		const auto found = changes.find(moved);
		const auto before = SingleValue(rows);
		const auto after = (found == changes.end()) ? nullptr : SingleValue(found->second);
		result.erase(address);
		if (CanPair(before, after)) {
			result.erase(moved);
			if (before->before != after->after) {
				result[moved][found->second.begin()->first].push_back({
					before->before, after->after,
				});
			}
		} else {
			for (const auto &[label, values] : rows) {
				for (const auto &value : values) {
					result[Removal(value) ? moved : address][label].push_back(value);
				}
			}
		}
	}
	return result;
}

inline QString Heading(const Address &address, const QString &label, bool shared) {
	const auto path = address.path();
	return path.isEmpty() ? label
		: (shared && !label.isEmpty())
		? path + QStringLiteral(" · ") + label
		: path;
}

inline QString Value(const std::optional<QString> &value) {
	return !value ? QStringLiteral("absent")
		: value->trimmed().isEmpty() || value->trimmed() == QStringLiteral("{}")
		? QStringLiteral("empty")
		: *value;
}

inline QString Escaped(QString value) {
	return value.toHtmlEscaped().replace(u'\n', QStringLiteral("<br/>"));
}

inline QString Block(const QString &title, const QStringList &lines) {
	return QStringLiteral("<p><b>%1</b><br/>%2</p>")
		.arg(Escaped(title), lines.join(QStringLiteral("<br/>")));
}

inline QString DiffLine(const QString &mark, const std::optional<QString> &value, QColor ink) {
	return QStringLiteral("<span style=\"color:%1\">%2 %3</span>")
		.arg(ink.name(), mark, Escaped(Value(value)));
}

inline QString Effects(
		Changes changes,
		const std::vector<Rename> &renames,
		QColor removed,
		QColor added,
		const std::map<QString, QString> &effective = {}) {
	auto result = QString();
	for (const auto &rename : renames) {
		auto lines = QStringList{ Escaped(QStringLiteral("Renamed %1 to %2")
			.arg(rename.fromName, rename.toName)) };
		if (!rename.references.isEmpty()) {
			lines.push_back(Escaped(QStringLiteral("References updated · %1")
				.arg(rename.references.join(QStringLiteral(", ")))));
		}
		result += Block(rename.to, lines);
		changes = RebaseRename(changes, rename);
	}
	for (const auto &[address, rows] : changes) {
		const auto annotation = effective.find(address.path());
		for (const auto &[label, values] : rows) {
			for (const auto &value : values) {
				auto lines = QStringList();
				if (!value.before && Value(value.after) == QStringLiteral("empty")) {
					lines.push_back(QStringLiteral("<span style=\"color:%1\">+ Added, empty</span>").arg(added.name()));
				} else {
					lines.push_back(DiffLine(QStringLiteral("−"), value.before, removed));
					lines.push_back(DiffLine(QStringLiteral("+"), value.after, added));
				}
				if (annotation != effective.end()) {
					lines.push_back(Escaped(annotation->second));
				}
				result += Block(Heading(address, label, rows.size() > 1), lines);
			}
		}
	}
	return result;
}

inline QString Conflict(const ConfigChangeSummary &draft, const ConfigChangeSummary &disk) {
	struct Comparison {
		std::optional<QString> original;
		std::optional<QString> yours;
		std::optional<QString> current;
		bool diskCompared = false;
	};
	auto comparisons = std::map<Address, std::map<QString, std::vector<Comparison>>>();
	for (const auto &[address, rows] : Flatten(draft)) {
		for (const auto &[label, values] : rows) {
			auto &compared = comparisons[address][label];
			for (const auto &value : values) {
				compared.push_back({ value.before, value.after, value.before });
			}
		}
	}
	for (const auto &[address, rows] : Flatten(disk)) {
		for (const auto &[label, values] : rows) {
			auto &compared = comparisons[address][label];
			for (const auto &value : values) {
				const auto matching = std::find_if(compared.begin(), compared.end(), [&](const auto &row) {
					return !row.diskCompared && row.original == value.before;
				});
				if (matching != compared.end()) {
					matching->current = value.after;
					matching->diskCompared = true;
				} else {
					compared.push_back({ value.before, value.before, value.after, true });
				}
			}
		}
	}
	auto result = QString();
	for (const auto &[address, rows] : comparisons) {
		for (const auto &[label, values] : rows) {
			for (const auto &value : values) {
				result += Block(Heading(address, label, rows.size() > 1), {
					QStringLiteral("<b>Yours</b> · ") + Escaped(Value(value.yours)),
					QStringLiteral("<b>On disk</b> · ") + (disk.parsed
						? Escaped(Value(value.current)) : QStringLiteral("Unavailable")),
					QStringLiteral("<b>Original</b> · ") + Escaped(Value(value.original)),
				});
			}
		}
	}
	return result;
}

} // namespace Purple::SettingsReviewDisplay
