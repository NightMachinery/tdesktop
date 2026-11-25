#pragma once

#include "purple/purple_config_diff.h"
#include "purple/purple_settings_edit.h"

#include <QtCore/QStringList>

namespace Purple::SettingsStructures {

struct Item {
	SettingAddress address;
	QByteArray literal;
	QString value;
	QString label;
	int line = 0;
	bool recognized = true;
	bool inlineValue = false;
};

[[nodiscard]] bool IsNamedContainer(SettingId id);
[[nodiscard]] bool IsNamedObject(SettingId id);
[[nodiscard]] bool NeedsCopyName(SettingId id);
[[nodiscard]] std::vector<Item> Items(const QByteArray &bytes, const SettingAddress &container, bool named);
[[nodiscard]] SettingsEdit ArrayEdit(SettingAddress address, SettingsCollectionOp operation, size_t index,
	size_t destination = 0, SettingsArrayValue value = false, std::optional<QString> name = std::nullopt);
[[nodiscard]] bool Overnight(const QString &from, const QString &to);
[[nodiscard]] QByteArray RecordTemplate(SettingId id);
[[nodiscard]] QString RecordEffect(const QByteArray &source, const SettingAddress &address);

enum class FieldKind { String, Choice, Boolean, Time, Duration, Integer, Weekdays, FirstList, FirstFolder };
struct RecordField {
	QString key;
	QString label;
	FieldKind kind;
	QString initial;
	QStringList choices;
	bool required = false;
};
[[nodiscard]] std::vector<RecordField> RecordFields(SettingId id);
[[nodiscard]] std::optional<QByteArray> EncodeRecord(SettingId id, const QStringList &values, QString &error);

struct ReferenceChanges {
	std::vector<SettingAddress> uses;
	std::vector<SettingsEdit> edits;
};

[[nodiscard]] ReferenceChanges RewriteReferences(
	const QByteArray &bytes, const SettingAddress &object, const QString &replacement);
[[nodiscard]] bool CanReplay(const QByteArray &opened, const QByteArray &current,
	const std::vector<SettingsEdit> &edits);

enum class PeerField { Members, Pins, Notifications, Media, Budget };
[[nodiscard]] std::optional<SettingsArrayValue> ParsePeerLiteral(
	PeerField field, const QString &text);
[[nodiscard]] QString PeerLabel(const QString &literal, const QString &name, bool visible);

[[nodiscard]] QStringList ReviewLines(const ConfigTextDiff &diff, const QByteArray &before, const QByteArray &after);
[[nodiscard]] QString SourceExcerpt(const QByteArray &bytes, int line, int column);

} // namespace Purple::SettingsStructures
