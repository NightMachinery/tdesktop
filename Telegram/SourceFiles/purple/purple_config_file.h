#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

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
	const QByteArray &bytes);

} // namespace Purple
