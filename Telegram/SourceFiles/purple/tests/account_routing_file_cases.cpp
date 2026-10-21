/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_account_config.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>

#include <cstdio>

namespace {

using namespace Purple;
auto Checks = 0;
auto Failures = 0;

void Check(bool value, const char *expression, int line) {
	++Checks;
	if (!value) {
		++Failures;
		std::printf("FAIL account_routing_file_cases:%d %s\n", line, expression);
	}
}

#define CHECK(value) Check(bool(value), #value, __LINE__)

void Put(const QString &path, const QByteArray &bytes) {
	CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
	auto file = QSaveFile(path);
	CHECK(file.open(QIODevice::WriteOnly));
	CHECK(file.write(bytes) == bytes.size());
	CHECK(file.commit());
}

QByteArray Get(const QString &path) {
	return ReadSettingsFileInput(path).bytes;
}

void PumpUntil(const std::function<bool()> &done) {
	auto elapsed = QElapsedTimer();
	elapsed.start();
	while (!done() && elapsed.elapsed() < 3000) {
		QCoreApplication::processEvents();
		QThread::msleep(10);
	}
	CHECK(done());
}

void OwnerPersistenceCases() {
	const auto a = AccountKey{ 0, 101 };
	const auto b = AccountKey{ 0, 202 };
	const auto t = AccountKey{ 1, 101 };
	{
		auto temp = QTemporaryDir();
		CHECK(temp.isValid());
		auto config = DesktopAccountConfig(temp.path());
		CHECK(config.ownerPlan().status == AccountOwnerStartupStatus::WaitingForFirstSignIn);
		CHECK(!config.permissionsReady());
		CHECK(!config.files(a));
		CHECK(!QFileInfo::exists(temp.filePath(u"local.toml"_q)));
		CHECK(!QFileInfo::exists(temp.filePath(u"accounts"_q)));
		config.start({ { a, false, false } });
		CHECK(config.owner() == a);
		CHECK(config.globalAccount() == a);
		CHECK(config.permissionsReady());
		CHECK(ParseOwnerRecord(QString::fromUtf8(Get(temp.filePath(u"accounts/root.toml"_q))), u"fixture"_q).account == a);
		CHECK(ParseLocalChoice(QString::fromUtf8(Get(temp.filePath(u"local.toml"_q))), u"fixture"_q).choice.rootFileAccount == a);
		CHECK(config.files(a)->settings == temp.filePath(u"settings.toml"_q));
		CHECK(config.files(b)->settings == temp.filePath(u"accounts/0-202/settings.toml"_q));
		CHECK(config.files(t)->settings == temp.filePath(u"accounts/1-101/settings.toml"_q));
		CHECK(config.files(a)->history == temp.filePath(u"sync/history"_q));
		CHECK(config.files(b)->history == temp.filePath(u"accounts/0-202/sync/history"_q));
		CHECK(config.files(a)->fingerprintFile == temp.filePath(u"state.toml"_q));
		CHECK(config.files(b)->fingerprintFile == temp.filePath(u"accounts/0-202/send-state.toml"_q));
		CHECK(!QFileInfo::exists(temp.filePath(u"settings.toml"_q)));
		CHECK(!QFileInfo::exists(temp.filePath(u"accounts/0-202"_q)));
		config.start({ { b, false, true }, { t, false, true } });
		CHECK(config.owner() == a);
		CHECK(config.accountTrafficReady(a));
		CHECK(!config.accountTrafficReady(b));
	}
	for (const auto bad : { QByteArray(), QByteArray("broken ["), QByteArray("root_file_account='wrong'\n") }) {
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"local.toml"_q), bad);
		Put(temp.filePath(u"settings.toml"_q), QByteArray("# preserve\nversion=1\n"));
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, true } });
		CHECK(config.ownerPlan().status == AccountOwnerStartupStatus::SingleFileError);
		CHECK(!config.owner());
		CHECK(!config.permissionsReady());
		CHECK(Get(temp.filePath(u"local.toml"_q)) == bad);
		CHECK(!QFileInfo::exists(temp.filePath(u"accounts/root.toml"_q)));
		config.start({ { a, false, false } }, a);
		CHECK(config.owner() == a);
		CHECK(config.permissionsReady());
		CHECK(Get(temp.filePath(u"settings.toml"_q)) == QByteArray("# preserve\nversion=1\n"));
	}
	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"accounts/root.toml"_q), QByteArray("root_file_account='1-101'\n"));
		auto config = DesktopAccountConfig(temp.path());
		CHECK(config.owner() == t);
		CHECK(config.globalAccount() == t);
		CHECK(config.permissionsReady());
		CHECK(ParseLocalChoice(QString::fromUtf8(Get(temp.filePath(u"local.toml"_q))), u"fixture"_q).choice.rootFileAccount == t);
		config.start({ { a, false, true }, { b, false, false } });
		CHECK(config.owner() == t);
	}
	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"accounts/root.toml"_q), QByteArray("root_file_account='0-202'\n"));
		Put(temp.filePath(u"local.toml"_q), QByteArray("root_file_account='0-101'\nglobal_account='1-101'\n[account_work_mode]\n'0-101'='stock'\n"));
		auto config = DesktopAccountConfig(temp.path());
		CHECK(config.owner() == b);
		CHECK(config.globalAccount() == t);
		CHECK(config.ownerPlan().restoredHandEditedOwnerNotice);
		const auto repaired = ParseLocalChoice(QString::fromUtf8(Get(temp.filePath(u"local.toml"_q))), u"fixture"_q);
		CHECK(repaired.ok());
		CHECK(repaired.choice.rootFileAccount == b);
		CHECK(repaired.choice.globalAccount == t);
		CHECK(config.workMode(a) == AccountWorkMode::Stock);
		const auto old = config.owner();
		Put(temp.filePath(u"local.toml"_q), QByteArray("root_file_account='0-101'\nglobal_account='0-101'\n"));
		config.refreshLocalChoice();
		CHECK(config.owner() == old);
		CHECK(config.globalAccount() == a);
		CHECK(Get(temp.filePath(u"accounts/root.toml"_q)) == QByteArray("root_file_account='0-202'\n"));
	}
	{
		auto temp = QTemporaryDir();
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false }, { t, false, true } });
		CHECK(config.ownerPlan().status == AccountOwnerStartupStatus::NeedsChoice);
		CHECK(!config.owner() && !config.permissionsReady());
		CHECK(!QFileInfo::exists(temp.filePath(u"accounts"_q)));
		config.start({ { a, false, false }, { t, false, true } }, t);
		CHECK(config.owner() == t);
	}
	{
		auto temp = QTemporaryDir();
		CHECK(QDir().mkpath(temp.filePath(u"accounts/0-202"_q)));
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false }, { b, true, false } }, b);
		CHECK(!config.owner() && !config.permissionsReady());
		CHECK(!QFileInfo::exists(temp.filePath(u"accounts/root.toml"_q)));
	}
	{
		auto temp = QTemporaryDir();
		CHECK(QDir().mkpath(temp.filePath(u"local.toml"_q)));
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		CHECK(!config.owner() && !config.permissionsReady());
		CHECK(config.ownerPlan().status == AccountOwnerStartupStatus::InvalidInput);
		CHECK(QFileInfo(temp.filePath(u"local.toml"_q)).isDir());
	}
	{
		auto temp = QTemporaryDir();
		CHECK(QFile::link(temp.filePath(u"absent"_q), temp.filePath(u"local.toml"_q)));
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		CHECK(!config.owner() && !config.permissionsReady());
		CHECK(QFileInfo(temp.filePath(u"local.toml"_q)).isSymLink());
		CHECK(!QFileInfo::exists(temp.filePath(u"accounts"_q)));
	}
	{
		auto config = DesktopAccountConfig(u"relative"_q);
		CHECK(!config.owner() && !config.permissionsReady());
		CHECK(config.globalSettingsPath().isEmpty());
		CHECK(!config.files(a));
	}
}

void PreconditionsAndInterruptedWriteCases() {
	CHECK(!CanReplaceLegacyRootSettings(QByteArray("[premium]\nenabled_p=false\n"), QByteArray("[last_seen]\ntrade_p=false\n")));
	CHECK(CanReplaceLegacyRootSettings(QByteArray("[premium]\nenabled_p=false\n[last_seen]\ntrade_p=true\n"), QByteArray("[premium]\nenabled_p=false\n[last_seen]\ntrade_p=false\n")));
	CHECK(CanReplaceLegacyRootSettings(QByteArray("[last_seen]\ntrade_p=false\n"), QByteArray()));
	CHECK(!CanReplaceLegacyRootSettings(QByteArray(), QByteArray("version=2\n")));
	CHECK(!CanReplaceLegacyRootSettings(QByteArray(), QByteArray("broken [")));
	CHECK(!CanReplaceLegacyRootSettings(QByteArray("future_key=1\n"), QByteArray()));

	auto temp = QTemporaryDir();
	const auto local = temp.filePath(u"local.toml"_q);
	const auto second = temp.filePath(u"accounts/root.toml"_q);
	const auto missing = ReadSettingsFileInput(local);
	CHECK(missing.state == SettingsFileState::Missing);
	CHECK(WriteGuardedSettingsFile(local, missing, QByteArray("x")) == GuardedFileWrite::ReadError);
	Put(local, QByteArray("new external bytes"));
	CHECK(WriteGuardedSettingsFile(local, missing, QByteArray("x"), {}, true) == GuardedFileWrite::InputChanged);
	CHECK(Get(local) == QByteArray("new external bytes"));
	const auto expected = ReadSettingsFileInput(local);
	CHECK(WriteGuardedSettingsFile(local, expected, QByteArray("replacement"), [] { return false; }) == GuardedFileWrite::InputChanged);
	CHECK(Get(local) == expected.bytes);
	CHECK(QDir().mkpath(temp.filePath(u"accounts"_q)));
	const auto secondMissing = ReadSettingsFileInput(second);
	CHECK(WriteGuardedSettingsFile(second, secondMissing, QByteArray("root_file_account='0-101'\n"), [&] {
		return ReadSettingsFileInput(local) == expected;
	}, true) == GuardedFileWrite::Written);
	CHECK(QFile::remove(local));
	auto config = DesktopAccountConfig(temp.path());
	CHECK((config.owner() == AccountKey{ 0, 101 }));
	CHECK(config.permissionsReady());
	CHECK(ParseLocalChoice(QString::fromUtf8(Get(local)), local).choice.rootFileAccount == config.owner());
	const auto original = ReadSettingsFileInput(local);
	const auto record = ReadSettingsFileInput(second);
	CHECK(WriteGuardedSettingsFile(local, original, QByteArray("would overwrite"), [&] {
		Put(second, QByteArray("external change"));
		return ReadSettingsFileInput(second) == record;
	}) == GuardedFileWrite::InputChanged);
	CHECK(Get(local) == original.bytes);
	CHECK(Get(second) == QByteArray("external change"));
}

void LayeringAndWatcherCases() {
	const auto a = AccountKey{ 0, 101 };
	const auto b = AccountKey{ 0, 202 };
	const auto t = AccountKey{ 1, 202 };
	auto temp = QTemporaryDir();
	const auto rootPath = temp.filePath(u"settings.toml"_q);
	const auto bPath = temp.filePath(u"accounts/0-202/settings.toml"_q);
	const auto tPath = temp.filePath(u"accounts/1-202/settings.toml"_q);
	const auto localPath = temp.filePath(u"local.toml"_q);
	const auto rootText = QByteArray("version=1\n[premium]\nenabled_p=false\n[lists.work]\ntitle='Work'\nmembers=[11,12]\n[last_seen]\ntrade_p=true\n[sync]\nsend_after_save_p=true\n");
	Put(rootPath, rootText);
	auto config = DesktopAccountConfig(temp.path());
	config.start({ { a, false, false } });
	CHECK(!config.settings(a).premium.enabled);
	CHECK(config.settings(a).sync.sendAfterSave);
	const auto rootMembers = config.settings(a).list(u"work"_q);
	CHECK(rootMembers && rootMembers->members == std::vector<PeerIdValue>({11,12}));
	CHECK(!config.settings(b).premium.enabled);
	CHECK(config.settings(b).lastSeen.trade);
	CHECK(!config.settings(b).sync.sendAfterSave);
	CHECK(!QFileInfo::exists(temp.filePath(u"accounts/0-202"_q)));
	Put(bPath, QByteArray("version=2\n[premium]\nenabled_p=true\n[lists.work]\nmembers=[23]\n[last_seen]\ntrade_p=false\n"));
	config.start({ { a, false, false }, { b, true, false }, { t, false, false } });
	config.refreshSettings();
	CHECK(!config.settings(b).premium.enabled);
	CHECK(!config.settings(b).lastSeen.trade);
	CHECK(config.settings(b).version == 2);
	const auto members = config.settings(b).list(u"work"_q);
	CHECK(members && members->members == std::vector<PeerIdValue>({23}));
	CHECK(config.settings(a).lastSeen.trade);
	CHECK(Get(rootPath) == rootText);
	CHECK(Get(bPath + u".good"_q) == Get(bPath));
	const auto bGood = Get(bPath + u".good"_q);
	Put(bPath, QByteArray("invalid ["));
	config.refreshSettings();
	CHECK(!config.settings(b).lastSeen.trade);
	CHECK(config.report(b) && config.report(b)->account.usedGood);
	CHECK(config.report(b) && !config.report(b)->account.current.parsed.ok());
	CHECK(Get(bPath + u".good"_q) == bGood);
	CHECK(QFile::remove(bPath));
	config.refreshSettings();
	CHECK(!config.settings(b).lastSeen.trade);
	CHECK(config.report(b) && config.report(b)->account.usedGood);
	CHECK(!QFileInfo::exists(bPath));
	Put(tPath, QByteArray("version=2\n[last_seen]\ntrade_p=true\n"));
	config.refreshSettings();
	CHECK(config.settings(t).lastSeen.trade);
	CHECK(!config.settings(b).lastSeen.trade);
	Put(localPath, QByteArray("root_file_account='0-101'\nglobal_account='0-101'\n[account_work_mode]\n'0-202'='follow'\n'1-202'='stock'\n"));
	config.refreshLocalChoice();
	CHECK(config.workMode(b) == AccountWorkMode::Follow);
	CHECK(config.workMode(t) == AccountWorkMode::Stock);
	const auto following = config.settings(b).list(u"work"_q);
	CHECK(following && following->members == std::vector<PeerIdValue>({11,12}));
	const auto target = config.target(a);
	CHECK(target && config.validate(*target));
	Put(rootPath, QByteArray("[premium]\nenabled_p=true\n"));
	CHECK(target && !config.validate(*target));
	Put(rootPath, rootText);
	const auto beforeChoice = config.target(a);
	Put(localPath, QByteArray("root_file_account='0-202'\nglobal_account='0-202'\n"));
	config.refreshLocalChoice();
	CHECK(config.owner() == a);
	CHECK(config.globalAccount() == b);
	CHECK(beforeChoice && !config.validate(*beforeChoice));
	CHECK(config.globalSettingsPath() == bPath);
	CHECK(config.globalSettings().premium.enabled);
	CHECK(!config.globalSettings().lastSeen.trade);
	CHECK(Get(rootPath) == rootText);
	Put(localPath, QByteArray("root_file_account='0-202'\nglobal_account='0-101'\n"));
	config.refreshLocalChoice();
	auto changed = 0;
	config.setChanged([&] { ++changed; });
	config.watch();
	QCoreApplication::processEvents();
	changed = 0;
	const auto previousGeneration = config.generation();
	Put(rootPath, QByteArray("[premium]\nenabled_p=true\n"));
	PumpUntil([&] { return changed > 0; });
	CHECK(config.generation() > previousGeneration);
	CHECK(config.settings(a).premium.enabled);
	const auto previous = changed;
	CHECK(QFile::remove(rootPath));
	PumpUntil([&] { return changed > previous; });
	CHECK(config.settings(a).premium.enabled);
	CHECK(config.report(a) && config.report(a)->global.usedGood);
	const auto removed = changed;
	Put(rootPath, rootText);
	PumpUntil([&] { return changed > removed; });
	CHECK(!config.settings(a).premium.enabled);
	CHECK(config.owner() == a);
}

void ReviewedRoutingRegressionCases() {
	const auto a = AccountKey{ 0, 101 };
	const auto b = AccountKey{ 0, 202 };
	const auto rootBytes = QByteArray("[last_seen]\ntrade_p=true\nreasons_p=true\n[sync]\nsend_after_save_p=true\n[premium]\nenabled_p=true\n");
	{
		auto temp = QTemporaryDir();
		CHECK(temp.isValid());
		const auto root = temp.filePath(u"settings.toml"_q);
		Put(root, rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		const auto single = config.manualExportTarget(b);
		CHECK(!config.owner());
		CHECK(single && single->role == ManualSettingsExportRole::SingleFile);
		CHECK(single && single->file.account == b);
		CHECK(single && single->file.path == root);
		CHECK(single && single->file.input.bytes == rootBytes);
		CHECK(single && config.validateManualExport(*single));
		CHECK(!config.editTarget(b));
		config.start({ { a, false, false } });
		CHECK(config.owner() == a);
		CHECK(single && !config.validateManualExport(*single));
		const auto missingB = config.manualExportTarget(b);
		CHECK(missingB && missingB->role == ManualSettingsExportRole::AccountFile);
		CHECK(missingB && missingB->file.path == temp.filePath(u"accounts/0-202/settings.toml"_q));
		CHECK(missingB && missingB->file.input.state == SettingsFileState::Missing);
		CHECK(missingB && missingB->file.input.bytes.isEmpty());
		CHECK(Get(root) == rootBytes);
	}
	{
		auto temp = QTemporaryDir();
		auto alias = QTemporaryDir();
		CHECK(temp.isValid() && alias.isValid());
		const auto root = temp.filePath(u"settings.toml"_q);
		Put(root, rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		CHECK(QFile::link(alias.path(), temp.filePath(u"accounts/0-202"_q)));
		CHECK(config.owner() == a);
		CHECK(config.globalAccount() == a);
		CHECK(!config.files(b));
		CHECK(!config.target(b));
		CHECK(!config.manualExportTarget(b));
		CHECK(!config.editTarget(b));
		CHECK(config.manualExportTarget(a)->role == ManualSettingsExportRole::AccountFile);
		CHECK(config.manualExportTarget(a)->file.input.bytes == rootBytes);
		CHECK(Get(root) == rootBytes);
	}
	{
		auto temp = QTemporaryDir();
		CHECK(temp.isValid());
		const auto root = temp.filePath(u"settings.toml"_q);
		const auto local = temp.filePath(u"local.toml"_q);
		const auto own = temp.filePath(u"accounts/0-202/settings.toml"_q);
		Put(root, rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		CHECK(config.settings(a).lastSeen.trade);
		CHECK(config.settings(a).lastSeen.reasons);
		CHECK(config.settings(a).sync.sendAfterSave);
		CHECK(!config.settings(b).sync.sendAfterSave);
		CHECK(config.settings(b).premium.enabled);
		CHECK(!QFileInfo::exists(own));
		Put(own, QByteArray("version=2\n[last_seen]\ntrade_p=false\nreasons_p=false\n"));
		config.refreshSettings();
		CHECK(!config.settings(b).lastSeen.trade);
		CHECK(!config.settings(b).lastSeen.reasons);
		CHECK(!config.settings(b).sync.sendAfterSave);
		CHECK(config.settings(a).lastSeen.trade);
		CHECK(config.settings(a).sync.sendAfterSave);
		CHECK(config.settings(b).premium.enabled);
		const auto rootEdit = config.editTarget(a);
		CHECK(rootEdit && rootEdit->path == root);
		CHECK(rootEdit && config.validate(*rootEdit));
		CHECK(!config.editTarget(b));
		const auto accountExport = config.manualExportTarget(b);
		CHECK(accountExport && accountExport->role == ManualSettingsExportRole::AccountFile);
		CHECK(accountExport && accountExport->file.path == own);
		CHECK(accountExport && accountExport->file.input.bytes == Get(own));
		CHECK(accountExport && config.validateManualExport(*accountExport));
		Put(local, QByteArray("root_file_account='0-101'\nglobal_account='0-202'\n"));
		config.refreshLocalChoice();
		CHECK(config.globalAccount() == b);
		CHECK(!config.editTarget(a));
		CHECK(!config.editTarget(b));
		CHECK(rootEdit && !config.validate(*rootEdit));
		CHECK(accountExport && !config.validateManualExport(*accountExport));
		CHECK(Get(root) == rootBytes);
		const auto changedRole = config.manualExportTarget(b);
		CHECK(changedRole && config.validateManualExport(*changedRole));
		Put(own, QByteArray("version=2\n[last_seen]\ntrade_p=true\n"));
		CHECK(changedRole && !config.validateManualExport(*changedRole));
		Put(local, QByteArray("root_file_account='0-101'\nglobal_account='0-101'\n[account_work_mode]\n'0-202'='stock'\n"));
		config.refreshLocalChoice();
		CHECK(config.workMode(b) == AccountWorkMode::Stock);
		CHECK(!AccountFilteringEnabled(true, config.workMode(b)));
		CHECK(AccountFilteringEnabled(true, config.workMode(a)));
		CHECK(!AccountFilteringEnabled(false, AccountWorkMode::Own));
		CHECK(!AccountFilteringEnabled(false, AccountWorkMode::Follow));
		CHECK(!AccountFilteringEnabled(false, AccountWorkMode::Stock));
		CHECK(AccountFilteringEnabled(true, AccountWorkMode::Own));
		CHECK(AccountFilteringEnabled(true, AccountWorkMode::Follow));
		CHECK(config.editTarget(a));
		CHECK(!config.editTarget(b));
		CHECK(Get(root) == rootBytes);
	}
}

}

int main(int argc, char *argv[]) {
	auto application = QCoreApplication(argc, argv);
	OwnerPersistenceCases();
	PreconditionsAndInterruptedWriteCases();
	LayeringAndWatcherCases();
	std::printf("baseline %d checks, %d failures\n", Checks, Failures);
	const auto beforeFocused = Checks;
	ReviewedRoutingRegressionCases();
	std::printf("focused %d checks\n", Checks - beforeFocused);
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
