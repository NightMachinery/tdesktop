#pragma once

#include "purple/purple_settings_edit.h"

#include <QtCore/QSet>
#include <functional>

namespace Purple::SettingsTree {

struct Source {
	QString name;
	QString path;
	QByteArray bytes;
	bool writable = false;
	bool recovery = false;
};

struct PhysicalValue {
	QString file;
	QString path;
	QString raw;
	int line = 0;
	int column = 0;
	bool inactive = false;
};

struct Row {
	QString key;
	QString parent;
	QString label;
	QString source;
	QString destination;
	QString help;
	SettingAddress address;
	const SettingDescriptor *descriptor = nullptr;
	QVariant effective;
	QString defaultValue;
	QString resetLabel;
	std::vector<PhysicalValue> physical;
	std::optional<QString> listCaseFix;
	SettingScope scope = SettingScope::Unknown;
	bool branch = false;
	bool editable = false;
	bool explicitHere = false;
	bool invalid = false;
};

struct VisibleRows {
	std::vector<size_t> rows;
	int matches = 0;
};

[[nodiscard]] QString Address(const SettingAddress &address);
[[nodiscard]] QString TomlPath(const SettingAddress &address);
[[nodiscard]] QString ValueText(const Row &row);
[[nodiscard]] QString ScopeText(SettingScope scope);
[[nodiscard]] std::vector<Row> Build(
	const LayerSettingsResult &layers,
	const std::vector<Source> &sources,
	const std::function<ArchiveV2::ChatClass(const QString &)> &archiveChatClass = {});
[[nodiscard]] VisibleRows View(
	const std::vector<Row> &rows,
	const QSet<QString> &expanded,
	const QString &query,
	const QString &anchor = {});

[[nodiscard]] QStringList Choices(const SettingDescriptor &descriptor);
[[nodiscard]] bool IsSecondsInteger(SettingId id);
[[nodiscard]] bool IsScalar(SettingCodec codec);
[[nodiscard]] bool CanReset(const SettingDescriptor &descriptor);
[[nodiscard]] std::optional<SettingScalar> Decode(
	const SettingDescriptor &descriptor,
	const QString &input);
[[nodiscard]] std::optional<SettingScalar> Step(
	const SettingDescriptor &descriptor,
	const QVariant &effective,
	int delta);
[[nodiscard]] std::optional<QString> StepDurationField(
	const SettingDescriptor &descriptor,
	const QString &text,
	int delta);

enum class EditorKey { Native, Stage, Back };
[[nodiscard]] EditorKey ShouldHandleEditorKey(
	bool preedit,
	bool submit,
	bool singleLineOrExplicitSubmit,
	bool back);

struct Rgb {
	int red = 0;
	int green = 0;
	int blue = 0;
	friend bool operator==(const Rgb &, const Rgb &) = default;
};

[[nodiscard]] std::optional<Rgb> ParseFixtureColor(const QString &hex);
[[nodiscard]] QString FixtureColorHex(Rgb color);
[[nodiscard]] Rgb EditFixtureColor(Rgb color, int channel, int delta);

} // namespace Purple::SettingsTree
