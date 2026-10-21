/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_account_config.h"
#include "purple/purple_account_send_queue.h"
#include "purple/purple_sync_history_guard.h"
#include "purple/purple_sync_config_history.h"
#include "purple/purple_sync_local_store.h"
#include "purple/purple_state.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>

#include <cstdio>
#ifndef Q_OS_WIN
#include <unistd.h>
#endif

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

void AccountSyncIsolationCases() {
	const auto a = AccountKey{ 0, 101 };
	const auto b = AccountKey{ 0, 202 };
	const auto t = AccountKey{ 1, 202 };
	auto temp = QTemporaryDir();
	CHECK(temp.isValid());
	const auto root = temp.filePath(u"settings.toml"_q);
	const auto bPath = temp.filePath(u"accounts/0-202/settings.toml"_q);
	const auto tPath = temp.filePath(u"accounts/1-202/settings.toml"_q);
	const auto aBytes = QByteArray("version=1\n[last_seen]\ntrade_p=true\n[sync]\nsend_after_save_p=true\n");
	const auto bBytes = QByteArray("version=1\n[last_seen]\ntrade_p=false\n[sync]\nsend_after_save_p=true\n");
	const auto bNext = QByteArray("version=1\n[last_seen]\ntrade_p=true\n[sync]\nsend_after_save_p=true\n");
	Put(root, aBytes);
	auto config = DesktopAccountConfig(temp.path());
	CHECK(!config.operationTarget(a));
	config.start({ { a, false, false } });
	CHECK(config.operationTarget(a));
	CHECK(!config.operationTarget(b));
	CHECK(!QFileInfo::exists(bPath));
	CHECK(!QFileInfo::exists(temp.filePath(u"accounts/0-202/sync"_q)));
	CHECK(!config.observeAccountSave(a));
	CHECK(!config.observeAccountSave(b));
	Put(bPath, bBytes);
	CHECK(!config.observeAccountSave(b));
	Put(tPath, bBytes);
	CHECK(!config.observeAccountSave(t));
	const auto af = *config.operationTarget(a);
	const auto bf = *config.operationTarget(b);
	const auto tf = *config.operationTarget(t);
	CHECK(af.files.syncDirectory == temp.filePath(u"sync"_q));
	CHECK(af.files.history == temp.filePath(u"sync/history"_q));
	CHECK(af.files.fingerprintFile == temp.filePath(u"state.toml"_q));
	CHECK(bf.files.syncDirectory == temp.filePath(u"accounts/0-202/sync"_q));
	CHECK(bf.files.history == temp.filePath(u"accounts/0-202/sync/history"_q));
	CHECK(bf.files.fingerprintFile == temp.filePath(u"accounts/0-202/send-state.toml"_q));
	auto wrongBackup = bf;
	wrongBackup.files.backup = af.files.backup;
	CHECK(!config.validateOperation(wrongBackup));
	CHECK(config.replace(wrongBackup, bNext) == GuardedFileWrite::InputChanged);
	auto wrongStore = bf;
	wrongStore.files.syncStore = af.files.syncStore;
	CHECK(!config.validateOperation(wrongStore));
	auto wrongFingerprint = bf;
	wrongFingerprint.files.fingerprintFile = af.files.fingerprintFile;
	CHECK(!config.validateOperation(wrongFingerprint));
	CHECK(tf.files.syncDirectory != bf.files.syncDirectory);
	CHECK(tf.files.fingerprintFile != bf.files.fingerprintFile);
	CHECK(config.fingerprints(bf));
	CHECK(config.fingerprints(bf)->sent.isEmpty());
	CHECK(!QFileInfo::exists(bf.files.fingerprintFile));
	CHECK(config.shouldAutoSend(bf));
	CHECK(config.noteFingerprint(bf, false));
	CHECK(config.fingerprints(bf)->sent == SettingsFingerprint(bBytes));
	CHECK(!config.shouldAutoSend(bf));
	CHECK(!QFileInfo::exists(af.files.fingerprintFile));
	CHECK(!QFileInfo::exists(tf.files.fingerprintFile));
	const auto bMarker = Get(bf.files.fingerprintFile);
	Put(bf.files.fingerprintFile, QByteArray("future_field='keep me'\n"));
	CHECK(!config.fingerprints(bf));
	CHECK(!config.shouldAutoSend(bf));
	CHECK(!config.noteFingerprint(bf, false));
	CHECK(Get(bf.files.fingerprintFile) == QByteArray("future_field='keep me'\n"));
	Put(bf.files.fingerprintFile, bMarker);
	CHECK(bMarker.contains("last_sent_fingerprint"));
	CHECK(!bMarker.contains("preset"));
	CHECK(!bMarker.contains("override"));
	CHECK(!bMarker.contains("recent"));
	Put(bPath, bNext);
	CHECK(!config.validateOperation(bf));
	CHECK(!config.noteFingerprint(bf, false));
	CHECK(Get(bf.files.fingerprintFile) == bMarker);
	const auto queuedB = config.observeAccountSave(b);
	CHECK(queuedB && queuedB->file.account == b);
	CHECK(queuedB && queuedB->file.input.bytes == bNext);
	CHECK(!config.observeAccountSave(a));
	CHECK(!config.observeAccountSave(t));
	CHECK(config.noteFingerprint(*queuedB, true));
	CHECK(!config.shouldAutoSend(*queuedB));
	CHECK(config.fingerprints(*queuedB)->imported == SettingsFingerprint(bNext));
	CHECK(config.fingerprints(*queuedB)->sent == SettingsFingerprint(bBytes));
	CHECK(Get(root) == aBytes);
	CHECK(!QFileInfo::exists(af.files.fingerprintFile));
	CHECK(!QFileInfo::exists(tf.files.fingerprintFile));
	const auto importedMarker = Get(bf.files.fingerprintFile);
	Put(bPath, bBytes);
	config.forgetAccountSave(b);
	CHECK(!config.observeAccountSave(b));
	CHECK(Get(bf.files.fingerprintFile) == importedMarker);
	CHECK(QFile::remove(bPath));
	CHECK(!config.observeAccountSave(b));
	CHECK(!config.operationTarget(b));
	Put(bPath, bNext);
	CHECK(!config.observeAccountSave(b));
	CHECK(Get(root) == aBytes);
	const auto currentB = *config.operationTarget(b);
	const auto currentT = *config.operationTarget(t);
	const auto aHistory = SyncHistoryAccess{ af.files, [&] { return config.validateOperation(af); } };
	const auto bHistory = SyncHistoryAccess{ currentB.files, [&] { return config.validateOperation(currentB); } };
	const auto tHistory = SyncHistoryAccess{ currentT.files, [&] { return config.validateOperation(currentT); } };
	CHECK(ListSyncConfigHistory(aHistory).empty());
	CHECK(ListSyncConfigHistory(bHistory).empty());
	CHECK(!QFileInfo::exists(af.files.syncDirectory));
	CHECK(!QFileInfo::exists(currentB.files.syncDirectory));
	const auto savedB = SaveSyncConfigHistory(bHistory, bNext,
		SyncConfigHistoryReason::BeforeUpdate, u"B synthetic update"_q);
	CHECK(savedB);
	CHECK(savedB && ReadSyncConfigHistory(bHistory, savedB->id) == bNext);
	CHECK(savedB && !ReadSyncConfigHistory(aHistory, savedB->id));
	CHECK(savedB && !ReadSyncConfigHistory(tHistory, savedB->id));
	CHECK(ListSyncConfigHistory(aHistory).empty());
	CHECK(ListSyncConfigHistory(bHistory).size() == 1);
	const auto savedT = SaveSyncConfigHistory(tHistory, bBytes,
		SyncConfigHistoryReason::BeforeUndo, u"Test environment synthetic undo"_q);
	CHECK(savedT);
	CHECK(ListSyncConfigHistory(bHistory).size() == 1);
	CHECK(ListSyncConfigHistory(tHistory).size() == 1);
	CHECK(savedT && !ReadSyncConfigHistory(bHistory, savedT->id));
	const auto beforeRestore = *config.operationTarget(b);
	CHECK(config.replace(beforeRestore, bBytes) == GuardedFileWrite::Written);
	CHECK(Get(bPath) == bBytes);
	CHECK(Get(beforeRestore.files.backup) == bNext);
	CHECK(Get(root) == aBytes);
	CHECK(!QFileInfo::exists(af.files.backup));
	CHECK(!config.validateOperation(beforeRestore));
	CHECK(!SaveSyncConfigHistory(bHistory, bBytes,
		SyncConfigHistoryReason::BeforeUndo, u"stale entry must refuse"_q));
	CHECK(config.replace(beforeRestore, bNext) == GuardedFileWrite::InputChanged);
	const auto afterRestore = *config.operationTarget(b);
	const auto undoText = ReadSyncConfigHistory({ afterRestore.files,
		[&] { return config.validateOperation(afterRestore); } }, savedB->id);
	CHECK(undoText && *undoText == bNext);
	CHECK(undoText && config.replace(afterRestore, *undoText) == GuardedFileWrite::Written);
	CHECK(Get(bPath) == bNext);
	CHECK(Get(afterRestore.files.backup) == bBytes);
	CHECK(Get(root) == aBytes);
	CHECK(Get(tPath) == bBytes);
	CHECK(Get(currentB.files.fingerprintFile) == importedMarker);
	const auto beforeGlobalChange = *config.operationTarget(b);
	Put(temp.filePath(u"local.toml"_q), QByteArray("root_file_account='0-101'\nglobal_account='0-202'\n"));
	config.refreshLocalChoice();
	CHECK(!config.validateOperation(beforeGlobalChange));
	CHECK(config.owner() == a);
	CHECK(config.operationTarget(a)->file.path == root);
	CHECK(Get(root) == aBytes);
	Put(temp.filePath(u"local.toml"_q), QByteArray("root_file_account='0-101'\nglobal_account='0-101'\n"));
	config.refreshLocalChoice();
	auto initialA = SyncLocalState();
	initialA.install = u"in-"_q + QString(26, u'a');
	initialA.space = u"sp-"_q + QString(26, u'a');
	initialA.createdDevice = u"device-a"_q;
	initialA.bindingToken = QString(32, u'a');
	auto initialB = initialA;
	initialB.install = u"in-"_q + QString(25, u'b') + u"a"_q;
	initialB.space = u"sp-"_q + QString(25, u'b') + u"a"_q;
	initialB.createdDevice = u"device-b"_q;
	initialB.bindingToken = QString(32, u'b');
	{
		auto storeA = SyncLocalStore(af.files.syncDirectory);
		auto storeB = SyncLocalStore(currentB.files.syncDirectory);
		CHECK(storeA.Open(true).status == SyncStoreStatus::Uninitialized);
		CHECK(storeB.Open(true).status == SyncStoreStatus::Uninitialized);
		CHECK(storeA.Initialize(initialA));
		CHECK(storeB.Initialize(initialB));
		const auto aStoreBytes = Get(af.files.syncStore);
		const auto bStoreBytes = Get(currentB.files.syncStore);
		CHECK(aStoreBytes != bStoreBytes);
		CHECK(CheckSyncAccountBinding(*storeB.state(), initialA.bindingToken.toLatin1())
			!= SyncAccountBindingVerdict::Bound);
		CHECK(CheckSyncAccountBinding(*storeB.state(), initialB.bindingToken.toLatin1())
			== SyncAccountBindingVerdict::Bound);
		auto input = ConfigRecordBuildInput();
		input.text = bNext;
		input.space = initialB.space;
		input.install = initialB.install;
		input.device = initialB.createdDevice;
		input.platform = u"macos"_q;
		input.app = u"Purple"_q;
		input.seq = 1;
		const auto record = BuildConfigRecord(input);
		CHECK(record);
		CHECK(AccountSyncRecordSupported(record.canonical));
		auto layeredInput = input;
		layeredInput.text = QByteArray("version=2\n[last_seen]\ntrade_p=true\n");
		const auto layeredRecord = BuildConfigRecord(layeredInput);
		CHECK(layeredRecord);
		CHECK(!AccountSyncRecordSupported(layeredRecord.canonical));
		auto next = initialB.configData;
		next.pending = record.version.key;
		CHECK(storeB.StageConfig(record.canonical, next, initialA.bindingToken.toLatin1()).status
			== SyncStoreStatus::AccountUnbound);
		CHECK(Get(currentB.files.syncStore) == bStoreBytes);
		CHECK(storeB.StageConfig(record.canonical, next, initialB.bindingToken.toLatin1()));
		CHECK(storeB.ConfirmConfigReadBack(record.canonical, initialB.createdDevice, 0).status
			== SyncStoreStatus::InvalidRecord);
		CHECK(storeB.state()->config.pendingSeq == 1);
		CHECK(Get(af.files.syncStore) == aStoreBytes);
		CHECK(Get(currentB.files.fingerprintFile) == importedMarker);
		CHECK(storeB.ConfirmConfigReadBack(record.canonical, initialB.createdDevice, 17));
		CHECK(storeB.state()->config.pendingSeq == 0);
		CHECK(Get(af.files.syncStore) == aStoreBytes);
	}
	const auto storeTarget = *config.operationTarget(b);
	const auto storeSnapshot = ReadSettingsFileInput(storeTarget.files.syncStore);
	CHECK(config.validateStore(storeTarget, storeSnapshot));
	Put(storeTarget.files.syncStore, QByteArray("changed external head"));
	CHECK(!config.validateStore(storeTarget, storeSnapshot));
	CHECK(Get(root) == aBytes);
	const auto directoryTarget = *config.operationTarget(b);
	const auto directory = directoryTarget.files.directory;
	CHECK(QDir().rename(directory, directory + u".retained"_q));
	Put(bPath, bNext);
	CHECK(!config.validateOperation(directoryTarget));
	CHECK(config.replace(directoryTarget, bBytes) == GuardedFileWrite::InputChanged);
	CHECK(Get(root) == aBytes);
#ifndef Q_OS_WIN
	CHECK(QFile::remove(bPath));
	CHECK(::link(QFile::encodeName(root).constData(), QFile::encodeName(bPath).constData()) == 0);
	CHECK(!QFileInfo(bPath).isSymLink());
	CHECK(!AccountFilePathSafe(bPath));
	CHECK(!config.operationTarget(b));
	CHECK(!config.manualExportTarget(b));
	CHECK(Get(root) == aBytes);
#endif
	Put(bPath, QByteArray("version=2\n[sync]\nsend_after_save_p=true\n"));
	CHECK(!config.operationTarget(b));
	CHECK(!config.observeAccountSave(b));
	CHECK(Get(root) == aBytes);
}

void QueueAndHistoryLifetimeRepairCases() {
	const auto a = AccountKey{ 0, 101 };
	const auto b = AccountKey{ 0, 202 };
	auto temp = QTemporaryDir();
	CHECK(temp.isValid());
	const auto root = temp.filePath(u"settings.toml"_q);
	const auto path = temp.filePath(u"accounts/0-202/settings.toml"_q);
	const auto first = QByteArray("version=1\n[last_seen]\ntrade_p=false\n[sync]\nsend_after_save_p=true\n");
	const auto second = QByteArray("version=1\n[last_seen]\ntrade_p=true\n[sync]\nsend_after_save_p=true\n");
	const auto latest = second + QByteArray("\n# latest ordinary save\n");
	const auto fresh = first + QByteArray("\n# fresh ordinary save\n");
	Put(root, first);
	auto config = DesktopAccountConfig(temp.path());
	config.start({ { a, false, false } });
	Put(path, first);
	auto queue = AccountSendQueue<QPointer<QObject>>();
	auto sessionA = std::make_unique<QObject>();
	auto sessionB = std::make_unique<QObject>();
	auto currentA = QPointer<QObject>(sessionA.get());
	auto currentB = QPointer<QObject>(sessionB.get());
	const auto ownerA = [&](const QPointer<QObject> &owner) { return owner && owner == currentA; };
	const auto ownerB = [&](const QPointer<QObject> &owner) { return owner && owner == currentB; };
	const auto observeA = [&] { queue.observe(config, a, currentA, ownerA); };
	const auto observeB = [&] { queue.observe(config, b, currentB, ownerB); };
	observeA();
	observeB();
	CHECK(queue.empty());
	Put(path, second);
	observeB();
	CHECK(!queue.empty());
	CHECK(QFile::remove(path));
	observeB();
	CHECK(queue.empty());
	Put(path, latest);
	observeB();
	CHECK(queue.empty());
	CHECK(queue.takeReady(config).empty());
	CHECK(Get(root) == first);
	Put(path, first);
	observeB();
	CHECK(!queue.empty());
	Put(path, latest);
	observeB();
	auto pending = queue.takeReady(config);
	CHECK(pending.size() == 1);
	if (pending.size() != 1) { return; }
	CHECK(pending[0].target.file.account == b);
	CHECK(pending[0].target.file.input.bytes == latest);
	CHECK(pending[0].target.file.path == path);
	CHECK(pending[0].owner == currentB);
	CHECK(queue.empty());
	const auto receipt = queue.beginSend(config, pending[0]);
	CHECK(receipt);
	if (!receipt) { return; }
	CHECK(!queue.beginSend(config, pending[0]));
	queue.finishSend(*receipt);
	const auto nextReceipt = queue.beginSend(config, pending[0]);
	CHECK(nextReceipt);
	if (!nextReceipt) { return; }
	queue.finishSend(*receipt);
	CHECK(!queue.beginSend(config, pending[0]));
	queue.finishSend(*nextReceipt);
	Put(root, second);
	observeA();
	observeB();
	pending = queue.takeReady(config);
	CHECK(pending.size() == 1);
	if (pending.size() != 1) { return; }
	CHECK(pending[0].target.file.account == a);
	CHECK(pending[0].target.file.path == root);
	CHECK(pending[0].target.file.input.bytes == second);
	Put(path, second);
	observeB();
	CHECK(!queue.empty());
	Put(path, first);
	CHECK(queue.takeReady(config).empty());
	observeB();
	CHECK(!queue.empty());
	Put(path, QByteArray("version=["));
	observeB();
	CHECK(queue.empty());
	CHECK(queue.takeReady(config).empty());
	Put(path, first);
	observeB();
	CHECK(!queue.empty());
	Put(path, QByteArray("version=2\n[sync]\nsend_after_save_p=true\n"));
	observeB();
	CHECK(queue.empty());
	CHECK(queue.takeReady(config).empty());
	Put(path, second);
	observeB();
	CHECK(!queue.empty());
	Put(path, QByteArray("version=1\n[sync]\nsend_after_save_p=false\n"));
	observeB();
	CHECK(queue.empty());
	Put(path, first);
	observeB();
	CHECK(!queue.empty());
	sessionB.reset();
	CHECK(!currentB);
	CHECK(queue.takeReady(config).empty());
	sessionB = std::make_unique<QObject>();
	currentB = sessionB.get();
	Put(path, second);
	observeB();
	CHECK(queue.empty());
	Put(path, latest);
	observeB();
	pending = queue.takeReady(config);
	CHECK(pending.size() == 1);
	if (pending.size() != 1) { return; }
	const auto oldSessionReceipt = queue.beginSend(config, pending[0]);
	CHECK(oldSessionReceipt);
	if (!oldSessionReceipt) { return; }
	sessionB.reset();
	sessionB = std::make_unique<QObject>();
	currentB = sessionB.get();
	observeB();
	CHECK(queue.empty());
	Put(path, second);
	observeB();
	Put(path, latest);
	observeB();
	pending = queue.takeReady(config);
	CHECK(pending.size() == 1);
	if (pending.size() != 1) { return; }
	const auto freshSessionReceipt = queue.beginSend(config, pending[0]);
	CHECK(freshSessionReceipt);
	if (!freshSessionReceipt) { return; }
	queue.finishSend(*oldSessionReceipt);
	CHECK(!queue.beginSend(config, pending[0]));
	queue.finishSend(*freshSessionReceipt);
	CHECK(config.noteFingerprint(pending[0].target, false));
	Put(path, second);
	observeB();
	CHECK(!queue.empty());
	Put(path, latest);
	observeB();
	CHECK(queue.empty());
	Put(path, second);
	observeB();
	CHECK(!queue.empty());
	const auto imported = *config.operationTarget(b);
	CHECK(config.noteFingerprint(imported, true));
	observeB();
	CHECK(queue.empty());
	CHECK(queue.takeReady(config).empty());
	Put(path, first);
	observeB();
	CHECK(!queue.empty());
	Put(temp.filePath(u"local.toml"_q), QByteArray("root_file_account='0-101'\nglobal_account='0-202'\n"));
	config.refreshLocalChoice();
	CHECK(queue.takeReady(config).empty());
	CHECK(config.operationTarget(a)->file.path == root);
	Put(path, fresh);
	observeB();
	CHECK(!queue.empty());
	const auto directory = QFileInfo(path).absolutePath();
	CHECK(QDir().rename(directory, directory + u".retained"_q));
	Put(path, first);
	CHECK(queue.takeReady(config).empty());
	observeB();
	Put(path, fresh);
	observeB();
	CHECK(!queue.empty());
	queue.forget(config, b);
	CHECK(queue.empty());
	observeB();
	CHECK(queue.empty());
	Put(path, first);
	observeB();
	CHECK(!queue.empty());
	CHECK(QFile::remove(path));
	CHECK(QFile::link(root, path));
	observeB();
	CHECK(queue.empty());
	CHECK(queue.takeReady(config).empty());
	CHECK(Get(root) == second);

	auto setup = std::make_unique<QObject>();
	auto retainedEntry = std::make_unique<QObject>();
	auto logicalSession = std::make_unique<QObject>();
	const auto sessionGuard = QPointer<QObject>(logicalSession.get());
	auto stateReads = 0;
	const auto restore = GuardSyncHistoryRestore(setup.get(),
		[&](const SyncConfigHistoryEntry &) {
			++stateReads;
			return bool(sessionGuard);
		});
	const auto entry = SyncConfigHistoryEntry();
	CHECK(restore(entry));
	CHECK(stateReads == 1);
	setup.reset();
	CHECK(bool(sessionGuard));
	CHECK(bool(retainedEntry));
	CHECK(!restore(entry));
	CHECK(stateReads == 1);
	setup = std::make_unique<QObject>();
	CHECK(!restore(entry));
	CHECK(stateReads == 1);
	const auto liveRestore = GuardSyncHistoryRestore(setup.get(),
		[&](const SyncConfigHistoryEntry &) { ++stateReads; return bool(sessionGuard); });
	CHECK(liveRestore(entry));
	logicalSession.reset();
	CHECK(!liveRestore(entry));
	CHECK(stateReads == 3);
	CHECK(!GuardSyncHistoryRestore(nullptr, [](const SyncConfigHistoryEntry &) { return true; })(entry));
	CHECK(!GuardSyncHistoryRestore(setup.get(), {})(entry));
}


void OwnerPolicyRecoveryCases() {
	const auto a = AccountKey{ 0, 101 };
	const auto b = AccountKey{ 0, 202 };
	auto temp = QTemporaryDir();
	CHECK(temp.isValid());
	const auto root = temp.filePath(u"settings.toml"_q);
	const auto path = temp.filePath(u"accounts/0-202/settings.toml"_q);
	const auto first = QByteArray("version=1\n[last_seen]\ntrade_p=false\n[sync]\nsend_after_save_p=true\n");
	const auto corrected = QByteArray("version=1\n[last_seen]\ntrade_p=true\n[sync]\nsend_after_save_p=true\n");
	Put(root, first);
	auto config = DesktopAccountConfig(temp.path());
	config.start({ { a, false, false } });
	CHECK(config.owner() == a);
	Put(path, first);
	auto origin = std::make_unique<QObject>();
	auto currentOwner = QPointer<QObject>(origin.get());
	const auto ownerPolicy = [&](const QPointer<QObject> &owner) {
		return owner && owner == currentOwner;
	};
	auto queue = AccountSendQueue<QPointer<QObject>>();
	const auto observe = [&] { queue.observe(config, b, currentOwner, ownerPolicy); };
	observe();
	CHECK(queue.empty());
	Put(path, QByteArray("version=[\n"));
	CHECK(ownerPolicy(currentOwner));
	CHECK(config.target(b));
	CHECK(config.target(b)->input.state == SettingsFileState::Readable);
	CHECK(!config.operationTarget(b));
	observe();
	CHECK(queue.empty());
	CHECK(queue.takeReady(config).empty());
	Put(path, corrected);
	CHECK(ownerPolicy(currentOwner));
	CHECK(config.operationTarget(b));
	observe();
	auto pending = queue.takeReady(config);
	CHECK(pending.size() == 1);
	if (pending.size() != 1) { return; }
	CHECK(pending[0].target.file.account == b);
	CHECK(pending[0].target.file.input.bytes == corrected);
	Put(path, QByteArray("version=2\n[sync]\nsend_after_save_p=true\n"));
	CHECK(ownerPolicy(currentOwner));
	CHECK(!config.operationTarget(b));
	observe();
	CHECK(queue.takeReady(config).empty());
	Put(path, first);
	observe();
	pending = queue.takeReady(config);
	CHECK(pending.size() == 1);
	if (pending.size() != 1) { return; }
	CHECK(pending[0].target.file.input.bytes == first);
	Put(path, QByteArray("version=1\n[sync]\nsend_after_save_p=false\n"));
	CHECK(ownerPolicy(currentOwner));
	const auto disabled = config.operationTarget(b);
	CHECK(disabled);
	if (!disabled) { return; }
	CHECK(!config.shouldAutoSend(*disabled));
	observe();
	CHECK(queue.takeReady(config).empty());
	Put(path, corrected);
	observe();
	pending = queue.takeReady(config);
	CHECK(pending.size() == 1);
	if (pending.size() != 1) { return; }
	CHECK(pending[0].target.file.input.bytes == corrected);
	CHECK(QFile::remove(path));
	CHECK(ownerPolicy(currentOwner));
	observe();
	CHECK(queue.empty());
	Put(path, first);
	observe();
	CHECK(queue.takeReady(config).empty());
	Put(path, corrected);
	observe();
	CHECK(!queue.empty());
	origin.reset();
	CHECK(!ownerPolicy(currentOwner));
	CHECK(queue.takeReady(config).empty());
	origin = std::make_unique<QObject>();
	currentOwner = origin.get();
	Put(path, first);
	observe();
	CHECK(queue.takeReady(config).empty());
	Put(path, corrected);
	observe();
	pending = queue.takeReady(config);
	CHECK(pending.size() == 1);
	if (pending.size() != 1) { return; }
	CHECK(pending[0].target.file.input.bytes == corrected);
	CHECK(Get(root) == first);
}


}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	auto application = QCoreApplication(argc, argv);
	OwnerPersistenceCases();
	PreconditionsAndInterruptedWriteCases();
	LayeringAndWatcherCases();
	std::printf("baseline %d checks, %d failures\n", Checks, Failures);
	const auto beforeFocused = Checks;
	ReviewedRoutingRegressionCases();
	std::printf("focused %d checks\n", Checks - beforeFocused);
	std::printf("priorBatch %d checks, %d failures\n", Checks, Failures);
	const auto beforeAccountSync = Checks;
	AccountSyncIsolationCases();
	std::printf("accountSync %d checks\n", Checks - beforeAccountSync);
	std::printf("priorBatch515 %d checks, %d failures\n", Checks, Failures);
	const auto beforeQueueRepair = Checks;
	QueueAndHistoryLifetimeRepairCases();
	std::printf("queueHistoryRepair %d checks\n", Checks - beforeQueueRepair);
	std::printf("priorBatch705 %d checks, %d failures\n", Checks, Failures);
	const auto beforeOwnerPolicy = Checks;
	OwnerPolicyRecoveryCases();
	std::printf("ownerPolicyRecovery %d checks\n", Checks - beforeOwnerPolicy);
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
