#include "purple/purple_config_file.h"

#include "purple/purple_types.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>

#include <utility>

namespace Purple {

SettingsFileInput ReadSettingsFileInput(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return { QFileInfo::exists(path)
			? SettingsFileState::ReadError
			: SettingsFileState::Missing };
	}
	auto bytes = file.readAll();
	if (file.error() != QFileDevice::NoError) {
		return { SettingsFileState::ReadError };
	}
	return { SettingsFileState::Readable, std::move(bytes) };
}

std::optional<SettingsReadTarget> CaptureSettingsReadTarget(
		const QString &root,
		const QString &path,
		const SettingsFileInput &loadedInput) {
	if (!QDir::isAbsolutePath(root) || !QDir::isAbsolutePath(path)
		|| loadedInput.state != SettingsFileState::Readable
		|| QFileInfo(path).isSymLink()) {
		return std::nullopt;
	}
	auto result = SettingsReadTarget{ root, path };
	for (const auto &file : {
		path, root + u"/local.toml"_q, root + u"/accounts/root.toml"_q,
	}) {
		const auto input = ReadSettingsFileInput(file);
		if (input.state == SettingsFileState::ReadError
			|| (file == path && input != loadedInput)) {
			return std::nullopt;
		}
		result.inputs.emplace(file, input);
	}
	return result;
}

bool ValidateSettingsReadTarget(const SettingsReadTarget &target) {
	const auto primary = target.inputs.find(target.path);
	if (primary == target.inputs.end()) {
		return false;
	}
	auto current = CaptureSettingsReadTarget(target.root, target.path, primary->second);
	if (current) {
		current->routingGeneration = target.routingGeneration;
	}
	return current && *current == target;
}

GuardedFileWrite WriteGuardedSettingsFile(
		const QString &path,
		const SettingsFileInput &expected,
		const QByteArray &bytes,
		const std::function<bool()> &stillCurrent,
		bool allowMissing) {
	if (expected.state != SettingsFileState::Readable
		&& !(allowMissing && expected.state == SettingsFileState::Missing)) {
		return GuardedFileWrite::ReadError;
	}
	const auto current = ReadSettingsFileInput(path);
	if (current.state == SettingsFileState::ReadError) {
		return GuardedFileWrite::ReadError;
	}
	if (current != expected) {
		return GuardedFileWrite::InputChanged;
	}
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
		return GuardedFileWrite::WriteError;
	}
	const auto finalInput = ReadSettingsFileInput(path);
	if (finalInput != expected || (stillCurrent && !stillCurrent())) {
		file.cancelWriting();
		return finalInput.state == SettingsFileState::ReadError
			? GuardedFileWrite::ReadError
			: GuardedFileWrite::InputChanged;
	}
	return file.commit()
		? GuardedFileWrite::Written
		: GuardedFileWrite::WriteError;
}

} // namespace Purple
