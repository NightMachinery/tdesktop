/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QString>
#include <QtCore/QTemporaryDir>

#include <cstdio>
#include <functional>
#include <memory>
#include <optional>

template <typename Signature>
using Fn = std::function<Signature>;

template <typename T>
using not_null = T;

struct MsgId {
};

#include "purple/purple_sync.h"

namespace {

auto checks = 0;
auto failures = 0;

void Check(bool ok, int line) {
	++checks;
	if (!ok) {
		++failures;
		std::printf("FAIL line %d\n", line);
	}
}

#define CHECK(value) Check(bool(value), __LINE__)

[[nodiscard]] std::optional<QByteArray> Read(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return std::nullopt;
	}
	return file.readAll();
}

[[nodiscard]] bool Write(const QString &path, const QByteArray &bytes) {
	auto file = QFile(path);
	return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
		&& (file.write(bytes) == bytes.size());
}

[[nodiscard]] bool Holds(const QString &path, const QByteArray &bytes) {
	const auto read = Read(path);
	return read && (*read == bytes);
}

void CheckOfferVerdicts() {
	using Purple::ImportOfferVerdict;
	using Purple::JudgeImportOffer;

	CHECK(JudgeImportOffer(false, 10, 5, 200, 100)
		== ImportOfferVerdict::InactiveAccount);
	CHECK(JudgeImportOffer(false, 3, 5, 200, 100)
		== ImportOfferVerdict::InactiveAccount);
	CHECK(JudgeImportOffer(false, 10, 5, 50, 100)
		== ImportOfferVerdict::InactiveAccount);

	CHECK(JudgeImportOffer(true, 5, 5, 200, 100)
		== ImportOfferVerdict::AlreadyOffered);
	CHECK(JudgeImportOffer(true, 4, 5, 200, 100)
		== ImportOfferVerdict::AlreadyOffered);
	CHECK(JudgeImportOffer(true, 6, 5, 100, 100)
		== ImportOfferVerdict::NotNewer);
	CHECK(JudgeImportOffer(true, 6, 5, 99, 100)
		== ImportOfferVerdict::NotNewer);
	CHECK(JudgeImportOffer(true, 6, 5, 101, 100)
		== ImportOfferVerdict::Offer);
	CHECK(JudgeImportOffer(true, 1, 0, 1, 0)
		== ImportOfferVerdict::Offer);
}

void CheckAccountLabels() {
	using Purple::ImportAccountLabel;

	CHECK(ImportAccountLabel(u"Ada"_q, QString()) == u"Ada"_q);
	CHECK(ImportAccountLabel(u"Ada Test"_q, u"tester"_q)
		== u"Ada Test (@tester)"_q);
	CHECK(ImportAccountLabel(u"  Ada  "_q, u"tester"_q)
		== u"Ada (@tester)"_q);
	CHECK(ImportAccountLabel(QString(), u"tester"_q) == u"@tester"_q);
	CHECK(ImportAccountLabel(u"  "_q, u"tester"_q) == u"@tester"_q);
	CHECK(ImportAccountLabel(u"  "_q, QString()).isEmpty());
}

void CheckSwitchedWindowText() {
	const auto text = Purple::ImportWindowSwitchedText(
		Purple::ImportAccountLabel(u"Ada Test"_q, u"tester"_q));
	CHECK(text.contains(u"Saved Messages of Ada Test (@tester)"_q));
	CHECK(text.contains(u"window"_q));
	CHECK(text.startsWith(u"Nothing was imported"_q));
	CHECK(!text.contains(u"active"_q));
}

void CheckImportBackups() {
	auto root = QTemporaryDir();
	CHECK(root.isValid());
	const auto directory = root.path();
	const auto path = directory + u"/settings.toml"_q;
	const auto importBackup = path + u".import.bak"_q;
	const auto backup = path + u".bak"_q;
	CHECK(Purple::SettingsImportBackupPath(path) == importBackup);
	CHECK(Purple::SettingsBackupPath(path) == backup);

	auto writes = 0;
	const auto writer = [&](const QByteArray &bytes) {
		return [&, bytes] {
			++writes;
			return Write(path, bytes);
		};
	};

	CHECK(Purple::WriteImportedSettings(path, writer("first")).isEmpty());
	CHECK(writes == 1);
	CHECK(Holds(path, "first"));
	CHECK(!QFile::exists(importBackup));
	CHECK(!QFile::exists(backup));

	const auto exact = QByteArray("version = 1\r\n# \xff\xfe not UTF-8\0end", 31);
	CHECK(Write(path, exact));
	auto seenAtWrite = false;
	CHECK(Purple::WriteImportedSettings(path, [&] {
		seenAtWrite = Holds(importBackup, exact) && Holds(backup, exact);
		return Write(path, "second");
	}).isEmpty());
	CHECK(seenAtWrite);
	CHECK(Holds(importBackup, exact));
	CHECK(Holds(backup, exact));
	CHECK(Holds(path, "second"));

	writes = 0;
	CHECK(Purple::WriteImportedSettings(path, writer("third")).isEmpty());
	CHECK(writes == 1);
	CHECK(Holds(importBackup, "second"));
	CHECK(Holds(backup, "second"));
	CHECK(Holds(path, "third"));

	CHECK(QFile::setPermissions(
		directory,
		QFile::ReadOwner | QFile::ExeOwner));
	writes = 0;
	CHECK(Purple::WriteImportedSettings(path, writer("fourth"))
		== importBackup);
	CHECK(QFile::setPermissions(
		directory,
		QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
	CHECK(writes == 0);
	CHECK(Holds(importBackup, "second"));
	CHECK(Holds(backup, "second"));
	CHECK(Holds(path, "third"));

	CHECK(QFile::setPermissions(path, QFile::WriteOwner));
	writes = 0;
	CHECK(Purple::WriteImportedSettings(path, writer("fourth"))
		== importBackup);
	CHECK(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner));
	CHECK(writes == 0);
	CHECK(Holds(importBackup, "second"));
	CHECK(Holds(backup, "second"));
	CHECK(Holds(path, "third"));

	CHECK(QFile::remove(backup));
	CHECK(QDir(directory).mkdir(u"settings.toml.bak"_q));
	writes = 0;
	CHECK(Purple::WriteImportedSettings(path, writer("fourth")) == backup);
	CHECK(writes == 0);
	CHECK(Holds(importBackup, "third"));
	CHECK(Holds(path, "third"));
	CHECK(QDir(directory).rmdir(u"settings.toml.bak"_q));

	writes = 0;
	CHECK(Purple::WriteImportedSettings(path, [&] {
		++writes;
		return false;
	}) == path);
	CHECK(writes == 1);
	CHECK(Holds(importBackup, "third"));
	CHECK(Holds(backup, "third"));
	CHECK(Holds(path, "third"));

	auto large = QByteArray(4 * 1024 * 1024, 'x');
	large[0] = '\0';
	large[large.size() - 1] = '\n';
	CHECK(Write(path, large));
	CHECK(Purple::WriteImportedSettings(path, writer("fifth")).isEmpty());
	CHECK(Holds(importBackup, large));
	CHECK(Holds(backup, large));
	CHECK(Holds(path, "fifth"));
}

} // namespace

int main() {
	CheckOfferVerdicts();
	CheckAccountLabels();
	CheckSwitchedWindowText();
	CheckImportBackups();

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
