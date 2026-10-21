#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <functional>

namespace Purple {

enum class SettingsFileState { Missing, Readable, ReadError };

struct SettingsFileInput {
	SettingsFileState state = SettingsFileState::Missing;
	QByteArray bytes;

	friend bool operator==(
		const SettingsFileInput &,
		const SettingsFileInput &) = default;
};

enum class GuardedFileWrite { Written, InputChanged, ReadError, WriteError };

[[nodiscard]] SettingsFileInput ReadSettingsFileInput(const QString &path);
[[nodiscard]] GuardedFileWrite WriteGuardedSettingsFile(
	const QString &path,
	const SettingsFileInput &expected,
	const QByteArray &bytes,
	const std::function<bool()> &stillCurrent = {},
	bool allowMissing = false);

} // namespace Purple
