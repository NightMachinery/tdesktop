#pragma once

#include "purple/purple_config_diff.h"

#include <QtCore/QStringList>
#include <QtGui/QColor>

#include <map>

namespace Purple::SettingsReviewDisplay {

struct Values {
	std::optional<QString> before;
	std::optional<QString> after;
};

using Changes = std::map<QString, Values>;

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
			auto path = key.key.isEmpty() ? key.table
				: key.table.isEmpty() ? key.key : key.table + u'.' + key.key;
			if (path.isEmpty()) path = entry.label;
			if (result.contains(path) && !entry.label.isEmpty()) {
				path += QStringLiteral(" · ") + entry.label;
			}
			result.insert_or_assign(std::move(path), Values{ key.beforeValue, key.afterValue });
		}
	}
	return result;
}

inline bool Owns(const QString &object, const QString &path) {
	return path == object || path.startsWith(object + u'.');
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
		const ConfigChangeSummary &summary,
		const std::vector<Rename> &renames,
		QColor removed,
		QColor added,
		const std::map<QString, QString> &effective = {}) {
	auto changes = Flatten(summary);
	auto result = QString();
	for (const auto &rename : renames) {
		auto lines = QStringList{ Escaped(QStringLiteral("Renamed %1 to %2")
			.arg(rename.fromName, rename.toName)) };
		if (!rename.references.isEmpty()) {
			lines.push_back(Escaped(QStringLiteral("References updated · %1")
				.arg(rename.references.join(QStringLiteral(", ")))));
		}
		result += Block(rename.to, lines);
		for (auto it = changes.begin(); it != changes.end();) {
			if (it->first == rename.from || it->first == rename.to) {
				it = changes.erase(it);
			} else if (Owns(rename.from, it->first)) {
				const auto moved = rename.to + it->first.mid(rename.from.size());
				const auto target = changes.find(moved);
				if (target != changes.end()) {
					target->second.before = it->second.before;
					if (target->second.before == target->second.after) changes.erase(target);
				}
				it = changes.erase(it);
			} else {
				++it;
			}
		}
	}
	for (const auto &[path, values] : changes) {
		auto lines = QStringList();
		if (!values.before && Value(values.after) == QStringLiteral("empty")) {
			lines.push_back(QStringLiteral("<span style=\"color:%1\">+ Added, empty</span>").arg(added.name()));
		} else {
			lines.push_back(DiffLine(QStringLiteral("−"), values.before, removed));
			lines.push_back(DiffLine(QStringLiteral("+"), values.after, added));
		}
		if (const auto it = effective.find(path); it != effective.end()) {
			lines.push_back(Escaped(it->second));
		}
		result += Block(path, lines);
	}
	return result;
}

inline QString Conflict(const ConfigChangeSummary &draft, const ConfigChangeSummary &disk) {
	const auto yours = Flatten(draft);
	const auto current = Flatten(disk);
	auto original = std::map<QString, std::optional<QString>>();
	for (const auto &[path, values] : yours) original.emplace(path, values.before);
	for (const auto &[path, values] : current) original.emplace(path, values.before);
	auto result = QString();
	for (const auto &[path, opened] : original) {
		const auto mine = yours.find(path);
		const auto theirs = current.find(path);
		result += Block(path, {
			QStringLiteral("<b>Yours</b> · ") + Escaped(Value(mine == yours.end() ? opened : mine->second.after)),
			QStringLiteral("<b>On disk</b> · ") + (!disk.parsed ? QStringLiteral("Unavailable")
				: Escaped(Value(theirs == current.end() ? opened : theirs->second.after))),
			QStringLiteral("<b>Original</b> · ") + Escaped(Value(opened)),
		});
	}
	return result;
}

} // namespace Purple::SettingsReviewDisplay
