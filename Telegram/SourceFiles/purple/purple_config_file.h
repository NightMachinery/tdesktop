#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <functional>
#include <map>
#include <optional>

namespace Purple {

enum class SettingsFileState { Missing, Readable, ReadError };

struct SettingsFileInput {
	SettingsFileState state = SettingsFileState::Missing;
	QByteArray bytes;

	friend bool operator==(
		const SettingsFileInput &,
		const SettingsFileInput &) = default;
};

struct SettingsReadTarget {
	QString root;
	QString path;
	std::map<QString, SettingsFileInput> inputs;
	quint64 routingGeneration = 0;

	[[nodiscard]] bool operator==(const SettingsReadTarget &) const = default;
};

[[nodiscard]] std::optional<SettingsReadTarget> CaptureSettingsReadTarget(
	const QString &root,
	const QString &path,
	const SettingsFileInput &loadedInput);
[[nodiscard]] bool ValidateSettingsReadTarget(const SettingsReadTarget &target);

enum class GuardedFileWrite { Written, InputChanged, ReadError, WriteError };

[[nodiscard]] SettingsFileInput ReadSettingsFileInput(const QString &path);
[[nodiscard]] GuardedFileWrite WriteGuardedSettingsFile(
	const QString &path,
	const SettingsFileInput &expected,
	const QByteArray &bytes,
	const std::function<bool()> &stillCurrent = {},
	bool allowMissing = false);

} // namespace Purple
