#include "purple/purple_config_file.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

#include <iostream>

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	auto directory = QTemporaryDir();
	auto checks = 0;
	auto failures = 0;
	const auto expect = [&](bool value, const char *name) {
		++checks;
		if (!value) { ++failures; std::cerr << name << '\n'; }
	};
	const auto path = directory.filePath(QString::fromUtf8("settings.toml"));
	const auto save = [&](const QByteArray &bytes) {
		auto file = QFile(path);
		return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
			&& file.write(bytes) == bytes.size() && file.flush();
	};
	using namespace Purple;
	expect(directory.isValid(), "fixture directory");
	const auto missing = ReadSettingsFileInput(path);
	expect(missing.state == SettingsFileState::Missing, "missing state");
	expect(save({}), "create empty fixture");
	const auto empty = ReadSettingsFileInput(path);
	expect(empty.state == SettingsFileState::Readable && empty.bytes.isEmpty(), "readable empty state");
	expect(empty != missing, "empty differs from missing");
	const auto original = QByteArray("version = 1\n[peek]\nstart_delay_seconds = 0\n");
	expect(save(original), "write original");
	const auto input = ReadSettingsFileInput(path);
	expect(input.bytes == original, "exact bytes captured");
	const auto external = QByteArray("# external unrelated edit\nversion = 1\n[peek]\nstart_delay_seconds = 30\n");
	expect(save(external), "external save during watcher interval");
	expect(WriteGuardedSettingsFile(path, input, original) == GuardedFileWrite::InputChanged,
		"stale delayed edit rejected");
	expect(ReadSettingsFileInput(path).bytes == external, "new delay and unrelated edit preserved");
	const auto current = ReadSettingsFileInput(path);
	const auto updated = external + "exit_hidden_chats_p = false\n";
	expect(WriteGuardedSettingsFile(path, current, updated) == GuardedFileWrite::Written,
		"matching current edit commits");
	expect(ReadSettingsFileInput(path).bytes == updated, "exact committed bytes");
	expect(QFile::remove(path), "external removal");
	expect(WriteGuardedSettingsFile(path, current, original) == GuardedFileWrite::InputChanged,
		"removed input rejected");
	expect(ReadSettingsFileInput(path).state == SettingsFileState::Missing, "removed input stays absent");
	expect(WriteGuardedSettingsFile(path, missing, original) == GuardedFileWrite::ReadError,
		"missing expected input cannot create or overwrite");
	expect(ReadSettingsFileInput(directory.path()).state == SettingsFileState::ReadError,
		"directory is a read error rather than empty input");
	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
