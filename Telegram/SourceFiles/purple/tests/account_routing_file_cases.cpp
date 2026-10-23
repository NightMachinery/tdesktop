/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_account_config.h"
#include "purple/purple_account_appwide_removal.h"
#include "purple/purple_account_send_queue.h"
#include "purple/purple_peek_controls.h"
#include "purple/purple_sync_history_guard.h"
#include "purple/purple_sync_config_history.h"
#include "purple/purple_sync_local_store.h"
#include "purple/purple_state.h"
#include "purple/purple_engine.h"
#include "purple/purple_global_choice_activation.h"
#include <QtCore/QDirIterator>

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtCore/QObject>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>

#include <cstdio>
#include <memory>
#include <utility>
#ifndef Q_OS_WIN
#include <unistd.h>
#endif

namespace {

using namespace Purple;
auto Checks = 0;
auto Failures = 0;
auto FailureContext = QByteArray();

void Check(bool value, const char *expression, int line) {
	++Checks;
	if (!value) {
		++Failures;
		std::printf("FAIL account_routing_file_cases:%d %s context=%s\n", line, expression, FailureContext.constData());
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


void AccountSettingsCreationAndDocumentCases() {
	const auto a = AccountKey{ 0, 101 };
	const auto b = AccountKey{ 0, 202 };
	const auto t = AccountKey{ 1, 202 };
	const auto expectedHeader = QByteArray(
		"# Purple Telegram settings for one account.\n"
		"# This file holds only what differs for this account. Anything it leaves out\n"
		"# comes from the account that supplies app-wide settings on each device.\n"
		"# Work Mode, Local Premium and the other app-wide settings written here take\n"
		"# effect only on devices where this account supplies the app-wide settings.\n"
		"# See config.md, \"Settings per account\".\n"
		"version = 2\n");
	CHECK(NewAccountSettingsText() == expectedHeader);
	CHECK(!expectedHeader.contains("101"));
	CHECK(!expectedHeader.contains("202"));
	CHECK(!expectedHeader.contains('['));
	CHECK(ParseSettings(QString::fromUtf8(expectedHeader), u"created"_q).settings.version == 2);
	{
		auto temp = QTemporaryDir();
		CHECK(temp.isValid());
		const auto root = temp.filePath(u"settings.toml"_q);
		const auto rootBytes = QByteArray("# unchanged root\nversion=1\n[premium]\nenabled_p=false\n");
		Put(root, rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		config.start({ { a, false, false }, { b, false, false } });
		CHECK(config.owner() == a);
		const auto localBefore = Get(temp.filePath(u"local.toml"_q));
		const auto recordBefore = Get(temp.filePath(u"accounts/root.toml"_q));
		const auto directory = temp.filePath(u"accounts/0-202"_q);
		const auto path = directory + u"/settings.toml"_q;
		(void)config.settings(b);
		CHECK(!QFileInfo::exists(directory));
		CHECK(!config.operationTarget(b));
		const auto target = config.openTarget(b, SettingsOpenRole::Own);
		CHECK(target);
		if (!target) { return; }
		CHECK(target->requester == b);
		CHECK(target->file.account == b);
		CHECK(target->file.path == path);
		CHECK(target->file.input.state == SettingsFileState::Missing);
		CHECK(!QFileInfo::exists(directory));
		auto wrong = *target;
		wrong.requester = t;
		CHECK(!config.validateOpen(wrong));
		wrong = *target;
		wrong.file.path = root;
		CHECK(!config.validateOpen(wrong));
		CHECK(config.prepareOpen(wrong, [] { return true; }).status == SettingsOpenStatus::Changed);
		CHECK(config.prepareOpen(*target, [] { return false; }).status == SettingsOpenStatus::Changed);
		CHECK(!QFileInfo::exists(directory));
		const auto rootOpen = config.openTarget(b, SettingsOpenRole::AppWide);
		CHECK(rootOpen && rootOpen->file.account == a && rootOpen->file.path == root);
		CHECK(rootOpen && config.prepareOpen(*rootOpen, [] { return true; }).status == SettingsOpenStatus::Ready);
		auto queue = AccountSendQueue<int>();
		queue.observe(config, b, 1, [](int owner) { return owner == 1; });
		CHECK(queue.empty());
		const auto result = config.prepareOpen(*target, [] { return true; });
		CHECK(result.status == SettingsOpenStatus::Created);
		CHECK(result.target && config.validateOpen(*result.target));
		CHECK(Get(path) == expectedHeader);
		CHECK(Get(root) == rootBytes);
		CHECK(Get(temp.filePath(u"local.toml"_q)) == localBefore);
		CHECK(Get(temp.filePath(u"accounts/root.toml"_q)) == recordBefore);
		CHECK(!config.operationTarget(b));
		CHECK(!config.editTarget(b));
		queue.observe(config, b, 1, [](int owner) { return owner == 1; });
		CHECK(queue.empty());
		CHECK(queue.takeReady(config).empty());
		CHECK(!QFileInfo::exists(directory + u"/sync"_q));
		CHECK(!QFileInfo::exists(directory + u"/send-state.toml"_q));
		CHECK(!config.validateOpen(*target));
		const auto existing = config.openTarget(b, SettingsOpenRole::Own);
		CHECK(existing);
		if (!existing) { return; }
		CHECK(config.prepareOpen(*existing, [] { return true; }).status == SettingsOpenStatus::Ready);
		CHECK(Get(path) == expectedHeader);
		const auto v1 = QByteArray("# preserve this byte for byte\nversion=1\n[last_seen]\ntrade_p=false\n");
		Put(path, v1);
		const auto oldFile = config.openTarget(b, SettingsOpenRole::Own);
		CHECK(oldFile);
		if (!oldFile) { return; }
		CHECK(config.prepareOpen(*oldFile, [] { return true; }).status == SettingsOpenStatus::Ready);
		CHECK(Get(path) == v1);
		Put(temp.filePath(u"local.toml"_q), QByteArray("root_file_account='0-101'\nglobal_account='0-202'\n"));
		const auto ownA = config.openTarget(a, SettingsOpenRole::Own);
		const auto wideB = config.openTarget(a, SettingsOpenRole::AppWide);
		CHECK(ownA && ownA->owner == a && ownA->global == b && ownA->file.account == a);
		CHECK(wideB && wideB->owner == a && wideB->global == b && wideB->file.account == b);
		CHECK(wideB && wideB->file.path == path);
		CHECK(!config.validateOpen(*oldFile));
		CHECK(wideB && config.prepareOpen(*wideB, [] { return true; }).status == SettingsOpenStatus::Ready);
		CHECK(Get(root) == rootBytes);
	}
	{
		auto temp = QTemporaryDir();
		CHECK(temp.isValid());
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto missingRoot = config.openTarget(a, SettingsOpenRole::Own);
		CHECK(missingRoot);
		CHECK(missingRoot && config.prepareOpen(*missingRoot, [] { return true; }).status == SettingsOpenStatus::CreateError);
		CHECK(!QFileInfo::exists(temp.filePath(u"settings.toml"_q)));
		const auto target = config.openTarget(b, SettingsOpenRole::Own);
		CHECK(target);
		if (!target) { return; }
		const auto concurrent = QByteArray("# another creator\nversion=1\n");
		Put(target->file.path, concurrent);
		CHECK(config.prepareOpen(*target, [] { return true; }).status == SettingsOpenStatus::Changed);
		CHECK(Get(target->file.path) == concurrent);
		CHECK(QFile::remove(target->file.path));
		const auto during = config.openTarget(b, SettingsOpenRole::Own);
		CHECK(during);
		if (!during) { return; }
		auto guards = 0;
		const auto outcome = config.prepareOpen(*during, [&] {
			if (++guards == 4) { Put(during->file.path, concurrent); }
			return true;
		});
		CHECK(guards >= 4);
		CHECK(!outcome.target);
		CHECK(Get(during->file.path) == concurrent);
		CHECK(QFile::remove(during->file.path));
		const auto staleOwner = config.openTarget(b, SettingsOpenRole::Own);
		CHECK(staleOwner);
		Put(temp.filePath(u"accounts/root.toml"_q), QByteArray("root_file_account='1-202'\n"));
		CHECK(!config.openTarget(b, SettingsOpenRole::Own));
		CHECK(staleOwner && !config.validateOpen(*staleOwner));
		CHECK(staleOwner && config.prepareOpen(*staleOwner, [] { return true; }).status == SettingsOpenStatus::Changed);
		CHECK(!QFileInfo::exists(during->file.path));
	}
	{
		auto temp = QTemporaryDir();
		CHECK(temp.isValid());
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto target = config.openTarget(b, SettingsOpenRole::Own);
		CHECK(target);
		if (!target) { return; }
		CHECK(QDir().mkpath(target->file.path));
		CHECK(!config.openTarget(b, SettingsOpenRole::Own));
		CHECK(config.prepareOpen(*target, [] { return true; }).status == SettingsOpenStatus::Changed);
		CHECK(QFileInfo(target->file.path).isDir());
	}
#ifndef Q_OS_WIN
	{
		auto temp = QTemporaryDir();
		CHECK(temp.isValid());
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto path = temp.filePath(u"accounts/0-202/settings.toml"_q);
		CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
		const auto target = config.openTarget(b, SettingsOpenRole::Own);
		CHECK(target);
		if (!target) { return; }
		CHECK(::symlink("absent.toml", QFile::encodeName(path).constData()) == 0);
		CHECK(QFileInfo(path).isSymLink());
		CHECK(!config.openTarget(b, SettingsOpenRole::Own));
		CHECK(config.prepareOpen(*target, [] { return true; }).status == SettingsOpenStatus::Changed);
		CHECK(QFileInfo(path).isSymLink());
		CHECK(QFile::remove(path));
		const auto bytes = QByteArray("version=1\n");
		Put(path, bytes);
		CHECK(QFile::setPermissions(path, QFileDevice::Permissions()));
		const auto unreadable = config.openTarget(b, SettingsOpenRole::Own);
		CHECK(unreadable);
		CHECK(unreadable && unreadable->file.input.state == SettingsFileState::ReadError);
		CHECK(unreadable && config.prepareOpen(*unreadable, [] { return true; }).status == SettingsOpenStatus::ReadError);
		CHECK(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
		CHECK(Get(path) == bytes);
	}
	{
		auto temp = QTemporaryDir();
		auto outside = QTemporaryDir();
		CHECK(temp.isValid() && outside.isValid());
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto target = config.openTarget(b, SettingsOpenRole::Own);
		CHECK(target);
		if (!target) { return; }
		const auto directory = QFileInfo(target->file.path).absolutePath();
		CHECK(::symlink(QFile::encodeName(outside.path()).constData(), QFile::encodeName(directory).constData()) == 0);
		CHECK(!config.openTarget(b, SettingsOpenRole::Own));
		CHECK(!config.prepareOpen(*target, [] { return true; }).target);
		CHECK(!QFileInfo::exists(outside.filePath(u"settings.toml"_q)));
	}
#endif
	{
		auto temp = QTemporaryDir();
		CHECK(temp.isValid());
		const auto root = temp.filePath(u"settings.toml"_q);
		const auto rootBytes = QByteArray("# root owner A\nversion=1\n");
		Put(root, rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		Put(temp.filePath(u"local.toml"_q), QByteArray("root_file_account='0-101'\nglobal_account='0-202'\n"));
		const auto selected = config.openTarget(a, SettingsOpenRole::AppWide);
		CHECK(selected && selected->owner == a && selected->global == b);
		CHECK(selected && selected->file.account == b);
		if (!selected) { return; }
		CHECK(!QFileInfo::exists(selected->file.path));
		const auto opened = config.prepareOpen(*selected, [] { return true; });
		CHECK(opened.status == SettingsOpenStatus::Created);
		CHECK(opened.target && opened.target->file.account == b);
		CHECK(Get(selected->file.path) == expectedHeader);
		CHECK(Get(root) == rootBytes);
		const auto testAccount = config.openTarget(t, SettingsOpenRole::Own);
		CHECK(testAccount && testAccount->file.path == temp.filePath(u"accounts/1-202/settings.toml"_q));
		CHECK(testAccount && config.prepareOpen(*testAccount, [] { return true; }).status == SettingsOpenStatus::Created);
		CHECK(testAccount && Get(testAccount->file.path) == expectedHeader);
		CHECK(Get(selected->file.path) == expectedHeader);
	}
	{
		auto temp = QTemporaryDir();
		CHECK(temp.isValid());
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto target = config.openTarget(b, SettingsOpenRole::Own);
		CHECK(target);
		if (!target) { return; }
		auto origin = std::make_unique<QObject>();
		const auto guard = QPointer<QObject>(origin.get());
		config.setChanged([&] { origin.reset(); });
		const auto result = config.prepareOpen(*target, [=] { return !guard.isNull(); });
		CHECK(!guard);
		CHECK(result.status == SettingsOpenStatus::Changed);
		CHECK(!result.target);
		CHECK(Get(target->file.path) == expectedHeader);
		CHECK(!config.observeAccountSave(b));
	}
	{
		auto temp = QTemporaryDir();
		CHECK(temp.isValid());
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto path = temp.filePath(u"accounts/0-202/settings.toml"_q);
		const auto bytes = QByteArray("version=1\n");
		Put(path, bytes);
		const auto oldDirectory = config.openTarget(b, SettingsOpenRole::Own);
		CHECK(oldDirectory);
		if (!oldDirectory) { return; }
		const auto directory = QFileInfo(path).absolutePath();
		CHECK(QDir().rename(directory, directory + u".retained"_q));
		Put(path, bytes);
		CHECK(!config.validateOpen(*oldDirectory));
		CHECK(!config.prepareOpen(*oldDirectory, [] { return true; }).target);
		CHECK(Get(path) == bytes);
		CHECK(Get(directory + u".retained/settings.toml"_q) == bytes);
	}
	CHECK(SettingsDocumentFileName(1) == u"settings.toml"_q);
	CHECK(SettingsDocumentFileName(2) == u"account-settings.toml"_q);
	CHECK(SettingsDocumentFileName(3) == u"account-settings.toml"_q);
	CHECK(ClassifySettingsDocumentName(u"SETTINGS.TOML"_q) == LegacySettingsDocumentKind::Whole);
	CHECK(ClassifySettingsDocumentName(u"ACCOUNT-SETTINGS.TOML"_q) == LegacySettingsDocumentKind::Layered);
	CHECK(ClassifySettingsDocumentName(u"../settings.toml"_q) == LegacySettingsDocumentKind::Unrelated);
	CHECK(ClassifySettingsDocumentName(u"settings.toml.bak"_q) == LegacySettingsDocumentKind::Unrelated);
	CHECK(CanApplyWholeSettingsDocument(u"settings.toml"_q, QByteArray("version=1\n")));
	CHECK(!CanApplyWholeSettingsDocument(u"settings.toml"_q, expectedHeader));
	CHECK(!CanApplyWholeSettingsDocument(u"account-settings.toml"_q, QByteArray("version=1\n")));
	CHECK(!CanApplyWholeSettingsDocument(u"account-settings.toml"_q, expectedHeader));
	CHECK(!CanApplyWholeSettingsDocument(u"other.toml"_q, QByteArray("version=1\n")));
	CHECK(!CanApplyWholeSettingsDocument(u"settings.toml"_q, QByteArray("broken [")));
	CHECK(!CanReplaceLegacyRootSettings(QByteArray("[premium]\nenabled_p=true\n"), expectedHeader));
	CHECK(!CanReplaceLegacyRootSettings(QByteArray("[premium]\nenabled_p=true\n"), QByteArray("version=1\n")));
}


void AccountSettingsRowsAndResetCases() {
	const auto a = AccountKey{ 0, 101 };
	const auto b = AccountKey{ 0, 202 };
	const auto c = AccountKey{ 1, 303 };
	const auto rootBytes = QByteArray("# root stays whole\nversion=1\n[premium]\nenabled_p=false\n[last_seen]\nreasons_p=true\ntrade_p=true\n[sync]\nsend_after_save_p=true\n");
	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false }, { b, false, false } });
		CHECK(!config.owner());
		const auto state = config.rowState(b, AccountSettingsRow::LastSeenReasons);
		CHECK(state.value && !state.pinned && !state.target);
		CHECK(Get(temp.filePath(u"settings.toml"_q)) == rootBytes);
	}

	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		config.start({ { a, false, false }, { b, false, false } });
		const auto own = config.files(b);
		CHECK(own.has_value());
		if (!own) { return; }
		const auto beforeLocal = Get(temp.filePath(u"local.toml"_q));
		const auto beforeOwner = Get(temp.filePath(u"accounts/root.toml"_q));
		CHECK(!QFileInfo::exists(own->directory));
		CHECK(config.rowState(b, AccountSettingsRow::LastSeenReasons).value);
		CHECK(!config.rowState(b, AccountSettingsRow::LastSeenReasons).pinned);
		CHECK(!config.rowState(b, AccountSettingsRow::SendAfterSave).value);
		const auto absent = config.rowTarget(b, AccountSettingsRow::LastSeenReasons);
		CHECK(absent.has_value());
		if (!absent) { return; }
		CHECK(config.writeRow(*absent, std::nullopt, [] { return true; }).ok);
		CHECK(!QFileInfo::exists(own->directory));
		(void)config.observeAccountSave(b);
		const auto created = config.writeRow(*absent, false, [] { return true; });
		CHECK(created.ok && created.changed && created.created && created.target.has_value());
		CHECK(!config.observeAccountSave(b));
		const auto first = Get(own->settings);
		CHECK(first.startsWith("# Purple Telegram settings for one account.\n"));
		CHECK(first.contains("version = 2\n"));
		CHECK(!first.contains("0-202"));
		CHECK(!first.contains("trade_p"));
		CHECK(!first.contains("send_after_save_p"));
		CHECK(!first.contains("enabled_p"));
		CHECK(Get(own->good) == first);
		CHECK(!QFileInfo::exists(own->fingerprintFile));
		CHECK(!QFileInfo::exists(own->syncStore));
		CHECK(config.rowState(b, AccountSettingsRow::LastSeenReasons).pinned);
		CHECK(!config.rowState(b, AccountSettingsRow::LastSeenReasons).value);
		CHECK(config.rowState(a, AccountSettingsRow::LastSeenReasons).value);
		CHECK(Get(temp.filePath(u"settings.toml"_q)) == rootBytes);
		CHECK(Get(temp.filePath(u"local.toml"_q)) == beforeLocal);
		CHECK(Get(temp.filePath(u"accounts/root.toml"_q)) == beforeOwner);
		const auto pinned = config.rowTarget(b, AccountSettingsRow::LastSeenReasons);
		CHECK(pinned.has_value());
		if (!pinned) { return; }
		const auto reset = config.writeRow(*pinned, std::nullopt, [] { return true; });
		CHECK(reset.ok && reset.changed && !reset.created);
		CHECK(!Get(own->settings).contains("reasons_p"));
		CHECK(Get(own->settings).contains("version = 2\n"));
		CHECK(Get(own->backup) == first);
		CHECK(Get(own->good) == Get(own->settings));
		CHECK(config.rowState(b, AccountSettingsRow::LastSeenReasons).value);
		CHECK(!config.rowState(b, AccountSettingsRow::LastSeenReasons).pinned);
		const auto global = config.rowTarget(a, AccountSettingsRow::LastSeenReasons);
		CHECK(global.has_value());
		if (!global) { return; }
		CHECK(!config.writeRow(*global, std::nullopt, [] { return true; }).ok);
		CHECK(Get(temp.filePath(u"settings.toml"_q)) == rootBytes);
	}
	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto own = config.files(b);
		const auto other = config.files(c);
		CHECK(own && other);
		if (!own || !other) { return; }
		const auto ownBytes = QByteArray("# unknown prefix\nversion=1\nfuture_key='untouched'\n[last_seen]\n# keep table note\nreasons_p=false # own scalar\ntrade_p=false\ntrade_hold=0\n[last_seen.extra]\nreasons_p=false\n[other.last_seen]\nreasons_p=true\n[premium]\nenabled_p=true\n[sync]\nsend_after_save_p=true\n");
		Put(own->settings, ownBytes);
		Put(other->settings, QByteArray("version=2\n[last_seen]\ntrade_p=false\n"));
		const auto otherBytes = Get(other->settings);
		config.start({ { a, false, false }, { b, true, false }, { c, true, false } });
		(void)config.observeAccountSave(b);
		const auto peek = config.rowTarget(b, AccountSettingsRow::LastSeenPeek);
		CHECK(peek.has_value());
		if (!peek) { return; }
		const auto saved = config.writeRow(*peek, true, [] { return true; });
		CHECK(saved.ok && saved.changed && !saved.created);
		CHECK(Get(own->backup) == ownBytes);
		CHECK(Get(own->settings).contains("trade_hold=0\n"));
		CHECK(Get(own->settings).contains("future_key='untouched'\n"));
		CHECK(Get(own->settings).contains("[premium]\nenabled_p=true\n"));
		CHECK(Get(own->settings).contains("version=1\n"));
		const auto operation = config.observeAccountSave(b);
		CHECK(operation.has_value());
		CHECK(!operation || operation->file.account == b);
		const auto reasons = config.rowTarget(b, AccountSettingsRow::LastSeenReasons);
		CHECK(reasons.has_value());
		if (!reasons) { return; }
		CHECK(config.writeRow(*reasons, std::nullopt, [] { return true; }).ok);
		const auto afterReset = Get(own->settings);
		CHECK(!afterReset.contains("reasons_p=false # own scalar"));
		CHECK(afterReset.contains("# keep table note\n"));
		CHECK(afterReset.contains("[last_seen.extra]\nreasons_p=false\n"));
		CHECK(afterReset.contains("[other.last_seen]\nreasons_p=true\n"));
		CHECK(afterReset.contains("trade_hold=0\n"));
		CHECK(config.rowState(b, AccountSettingsRow::LastSeenReasons).value);
		const auto local = config.rowTarget(b, AccountSettingsRow::SendAfterSave);
		CHECK(local.has_value());
		if (!local) { return; }
		CHECK(!config.writeRow(*local, std::nullopt, [] { return true; }).ok);
		CHECK(config.writeRow(*local, false, [] { return true; }).ok);
		CHECK(!config.rowState(b, AccountSettingsRow::SendAfterSave).value);
		CHECK(config.rowState(a, AccountSettingsRow::SendAfterSave).value);
		CHECK(Get(temp.filePath(u"settings.toml"_q)) == rootBytes);
		CHECK(Get(other->settings) == otherBytes);
		CHECK(!QFileInfo::exists(other->backup));
		const auto capturedOwn = config.rowTarget(b, AccountSettingsRow::LastSeenReasons);
		CHECK(capturedOwn.has_value());
		if (!capturedOwn) { return; }
		auto forged = *capturedOwn;
		const auto appWide = config.openTarget(b, SettingsOpenRole::AppWide);
		CHECK(appWide.has_value());
		if (!appWide) { return; }
		forged.destination = *appWide;
		CHECK(!config.validateRow(forged));
		CHECK(!config.writeRow(forged, false, [] { return true; }).ok);
		CHECK(Get(temp.filePath(u"settings.toml"_q)) == rootBytes);
	}
	for (auto failure = 0; failure != 4; ++failure) {
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto own = config.files(b);
		CHECK(own.has_value());
		if (!own) { return; }
		const auto ownBytes = QByteArray("version=2\n[last_seen]\ntrade_p=false\n");
		Put(own->settings, ownBytes);
		config.start({ { a, false, false }, { b, true, false } });
		const auto capture = config.rowTarget(b, AccountSettingsRow::LastSeenPeek);
		CHECK(capture.has_value());
		if (!capture) { return; }
		const auto localPath = temp.filePath(u"local.toml"_q);
		const auto localBefore = Get(localPath);
		if (failure == 0) {
			Put(temp.filePath(u"settings.toml"_q), rootBytes + QByteArray("# new global bytes\n"));
		} else if (failure == 1) {
			auto choice = ParseLocalChoice(QString::fromUtf8(localBefore), localPath).choice;
			choice.globalAccount = b;
			const auto serialized = SerializeLocalChoice(choice);
			CHECK(serialized.has_value());
			if (!serialized) { return; }
			Put(localPath, serialized->toUtf8());
		} else if (failure == 2) {
			CHECK(QDir().rename(own->directory, own->directory + u".retained"_q));
			Put(own->settings, ownBytes);
		} else {
			auto choice = ParseLocalChoice(QString::fromUtf8(localBefore), localPath).choice;
			choice.globalAccount = b;
			const auto serialized = SerializeLocalChoice(choice);
			CHECK(serialized.has_value());
			if (!serialized) { return; }
			Put(localPath, serialized->toUtf8());
			config.refreshLocalChoice();
			Put(localPath, localBefore);
			config.refreshLocalChoice();
			CHECK(Get(localPath) == localBefore);
			CHECK(config.globalAccount() == std::optional<AccountKey>(a));
			const auto current = config.rowTarget(b, AccountSettingsRow::LastSeenPeek);
			CHECK(current.has_value());
			if (!current) { return; }
			CHECK(config.validateRow(*current));
		}
		CHECK(!config.validateRow(*capture));
		CHECK(!config.writeRow(*capture, true, [] { return true; }).ok);
		CHECK(Get(own->settings) == ownBytes);
	}
	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto own = config.files(b);
		CHECK(own.has_value());
		if (!own) { return; }
		const auto ownBytes = QByteArray("version=2\n[last_seen]\ntrade_p=false\n");
		Put(own->settings, ownBytes);
		config.start({ { a, false, false }, { b, true, false } });
		const auto capture = config.rowTarget(b, AccountSettingsRow::LastSeenPeek);
		CHECK(capture.has_value());
		if (!capture) { return; }
		auto context = std::make_unique<QObject>();
		const auto weak = QPointer<QObject>(context.get());
		auto checks = 0;
		CHECK(!config.writeRow(*capture, true, [&] {
			if (++checks == 2) { context.reset(); }
			return !weak.isNull();
		}).ok);
		CHECK(Get(own->settings) == ownBytes);
		CHECK(Get(temp.filePath(u"settings.toml"_q)) == rootBytes);
	}
	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto own = config.files(b);
		CHECK(own.has_value());
		if (!own) { return; }
		config.start({ { a, false, false }, { b, false, false } });
		const auto capture = config.rowTarget(b, AccountSettingsRow::LastSeenPeek);
		CHECK(capture.has_value());
		if (!capture) { return; }
		const auto foreign = QByteArray("version=2\n# foreign creator\n");
		auto once = false;
		CHECK(!config.writeRow(*capture, true, [&] {
			if (std::exchange(once, true)) { Put(own->settings, foreign); }
			return true;
		}).ok);
		CHECK(Get(own->settings) == foreign);
		CHECK(Get(temp.filePath(u"settings.toml"_q)) == rootBytes);
	}
	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), QByteArray("broken ["));
		Put(temp.filePath(u"settings.toml.good"_q), rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto own = config.files(b);
		CHECK(own.has_value());
		if (!own) { return; }
		const auto ownBytes = QByteArray("version=2\n[last_seen]\ntrade_p=false\n");
		Put(own->settings, ownBytes);
		config.start({ { a, false, false }, { b, true, false } });
		const auto capture = config.rowTarget(b, AccountSettingsRow::LastSeenPeek);
		CHECK(capture.has_value());
		if (!capture) { return; }
		CHECK(capture->globalFallback);
		Put(temp.filePath(u"settings.toml.good"_q), rootBytes + QByteArray("# new fallback source\n"));
		CHECK(!config.validateRow(*capture));
		CHECK(!config.writeRow(*capture, true, [] { return true; }).ok);
		CHECK(Get(own->settings) == ownBytes);
	}
	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto own = config.files(b);
		CHECK(own.has_value());
		if (!own) { return; }
		Put(own->good, rootBytes);
		config.start({ { a, false, false }, { b, true, false } });
		const auto localPath = temp.filePath(u"local.toml"_q);
		auto choice = ParseLocalChoice(QString::fromUtf8(Get(localPath)), localPath).choice;
		choice.globalAccount = b;
		const auto serialized = SerializeLocalChoice(choice);
		CHECK(serialized.has_value());
		if (!serialized) { return; }
		Put(localPath, serialized->toUtf8());
		const auto capture = config.rowTarget(b, AccountSettingsRow::LastSeenPeek);
		CHECK(!capture);
		CHECK(!QFileInfo::exists(own->settings));
		CHECK(Get(own->good) == rootBytes);
		CHECK(Get(temp.filePath(u"settings.toml"_q)) == rootBytes);
	}

	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto own = config.files(b);
		CHECK(own.has_value());
		if (!own) { return; }
		Put(own->settings, QByteArray("broken ["));
		Put(own->good, QByteArray("version=2\n[last_seen]\nreasons_p=false\n"));
		config.start({ { a, false, false }, { b, true, false } });
		const auto state = config.rowState(b, AccountSettingsRow::LastSeenReasons);
		CHECK(!state.value);
		CHECK(state.pinned);
		CHECK(!state.target);
		CHECK(Get(own->settings) == QByteArray("broken ["));
	}

	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml.good"_q), rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto own = config.files(b);
		CHECK(own.has_value());
		if (!own) { return; }
		Put(own->settings, QByteArray("version=2\n"));
		config.start({ { a, false, false }, { b, true, false } });
		const auto inherited = config.rowState(b, AccountSettingsRow::LastSeenReasons);
		CHECK(inherited.value && !inherited.pinned && inherited.target.has_value());
		if (!inherited.target) { return; }
		CHECK(config.writeRow(*inherited.target, false, [] { return true; }).ok);
		CHECK(!config.rowState(b, AccountSettingsRow::LastSeenReasons).value);
		CHECK(!QFileInfo::exists(temp.filePath(u"settings.toml"_q)));
		CHECK(Get(temp.filePath(u"settings.toml.good"_q)) == rootBytes);
	}
	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		config.start({ { a, false, false }, { b, false, false } });
		const auto own = config.files(b);
		const auto capture = config.rowTarget(b, AccountSettingsRow::LastSeenReasons);
		CHECK(own && capture);
		if (!own || !capture) { return; }
		auto context = std::make_unique<QObject>();
		const auto weak = QPointer<QObject>(context.get());
		config.setChanged([&] { context.reset(); });
		const auto result = config.writeRow(*capture, false, [&] { return !weak.isNull(); });
		CHECK(!result.ok && result.changed && result.created);
		CHECK(Get(own->settings).contains("version = 2\n"));
		CHECK(Get(own->settings).contains("reasons_p"));
		CHECK(!config.observeAccountSave(b));
		CHECK(!QFileInfo::exists(own->fingerprintFile));
		CHECK(Get(temp.filePath(u"settings.toml"_q)) == rootBytes);
	}

#ifndef Q_OS_WIN
	for (auto kind = 0; kind != 4; ++kind) {
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto own = config.files(b);
		CHECK(own.has_value());
		if (!own) { return; }
		const auto ownBytes = QByteArray("version=2\n[last_seen]\ntrade_p=false\n");
		Put(own->settings, ownBytes);
		config.start({ { a, false, false }, { b, true, false } });
		const auto capture = config.rowTarget(b, AccountSettingsRow::LastSeenPeek);
		CHECK(capture.has_value());
		if (!capture) { return; }
		const auto outside = temp.filePath(u"outside"_q);
		Put(outside, QByteArray("outside untouched"));
		if (kind == 0) {
			CHECK(QFile::remove(own->settings));
			CHECK(QFile::link(outside, own->settings));
		} else if (kind == 1) {
			CHECK(QFile::remove(own->settings));
			CHECK(QDir().mkpath(own->settings));
		} else if (kind == 2) {
			CHECK(QFile::setPermissions(own->settings, QFile::Permissions()));
		} else {
			CHECK(QFile::link(outside, own->good));
		}
		CHECK(!config.writeRow(*capture, true, [] { return true; }).ok);
		CHECK(Get(outside) == QByteArray("outside untouched"));
		CHECK(Get(temp.filePath(u"settings.toml"_q)) == rootBytes);
		if (kind == 2) {
			CHECK(QFile::setPermissions(own->settings, QFile::ReadOwner | QFile::WriteOwner));
			CHECK(Get(own->settings) == ownBytes);
		}
	}
	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), rootBytes);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto own = config.files(b);
		CHECK(own.has_value());
		if (!own) { return; }
		const auto ownBytes = QByteArray("version=2\n[last_seen]\ntrade_p=false\n");
		Put(own->settings, ownBytes);
		config.start({ { a, false, false }, { b, true, false } });
		const auto capture = config.rowTarget(b, AccountSettingsRow::LastSeenPeek);
		CHECK(capture.has_value());
		if (!capture) { return; }
		const auto outside = temp.filePath(u"outside-directory"_q);
		const auto foreign = QByteArray("version=2\n# outside unchanged\n");
		Put(outside + u"/settings.toml"_q, foreign);
		CHECK(QDir().rename(own->directory, own->directory + u".retained"_q));
		CHECK(QFile::link(outside, own->directory));
		CHECK(!config.rowTarget(b, AccountSettingsRow::LastSeenPeek));
		CHECK(!config.writeRow(*capture, true, [] { return true; }).ok);
		CHECK(Get(outside + u"/settings.toml"_q) == foreign);
		CHECK(Get(own->directory + u".retained/settings.toml"_q) == ownBytes);
	}

#endif
}

std::map<QString, QByteArray> ChoiceOtherFiles(const QString &root) {
	auto result = std::map<QString, QByteArray>();
	auto iterator = QDirIterator(root, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
	while (iterator.hasNext()) {
		const auto path = iterator.next();
		if (QDir(root).relativeFilePath(path) != u"local.toml"_q) { result[path] = Get(path); }
	}
	return result;
}

void GlobalAccountChoiceCases() {
	const auto a = AccountKey{ 0, 101 };
	const auto b = AccountKey{ 0, 202 };
	const auto t = AccountKey{ 1, 101 };
	const auto root = QByteArray("version=1\n[presets.work]\n[premium]\nenabled_p=false\n");
	CHECK(!ShouldShowGlobalAccountChoice(1, true));
	CHECK(ShouldShowGlobalAccountChoice(2, true));
	CHECK(ShouldShowGlobalAccountChoice(1, false));
	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), root);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		config.start({ { a, false, false }, { b, false, false } });
		const auto localPath = temp.filePath(u"local.toml"_q);
		const auto before = Get(localPath);
		const auto others = ChoiceOtherFiles(temp.path());
		const auto preview = config.previewGlobalChoice(a, b, u"work"_q);
		CHECK(preview.has_value());
		if (!preview) { return; }
		CHECK(preview->enterNormal && preview->noAppWideSettings && !preview->selectedUsesGood);
		CHECK(preview->before.presets.size() == 1 && preview->after.presets.empty());
		CHECK(Get(localPath) == before && ChoiceOtherFiles(temp.path()) == others);
		CHECK(!QFileInfo::exists(preview->selected.file.path));
		auto commit = config.commitGlobalChoice(*preview, [] { return true; });
		CHECK(commit.ok && commit.changed && !commit.activated);
		CHECK(config.globalAccount() == a);
		CHECK(ParseLocalChoice(QString::fromUtf8(Get(localPath)), localPath).choice.globalAccount == b);
		CHECK(config.owner() == a && config.validateGlobalChoiceCommit(*preview, commit));
		auto deviceState = State();
		deviceState.activePreset = preview->runningPreset;
		CHECK(!Resolve(preview->after, deviceState.activePreset));
		deviceState.activePreset = NormalPreset();
		deviceState.activeSource = PresetSource::Manual;
		const auto normal = Resolve(preview->after, deviceState.activePreset);
		CHECK(normal && normal->normal);
		CHECK(config.activateGlobalChoice(*preview, commit));
		CHECK(commit.activated && config.globalAccount() == b && config.owner() == a);
		CHECK(config.validateGlobalChoiceCommit(*preview, commit));
		CHECK(ChoiceOtherFiles(temp.path()) == others);
		CHECK(!QFileInfo::exists(preview->selected.file.path));
		CHECK(Get(localPath).contains("accounts/0-202/settings.toml"));
		CHECK(!config.observeAccountSave(b));
		config.start({ { a, false, false } });
		const auto offline = config.previewGlobalChoice(a, b, NormalPreset());
		CHECK(offline && offline->selected.file.account == b && !offline->enterNormal);
		CHECK(!config.previewGlobalChoice(a, t, NormalPreset()));
	}
	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), root);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto files = config.files(b);
		CHECK(files);
		if (!files) { return; }
		const auto selected = QByteArray("version=2\n[presets.work]\n[premium]\nenabled_p=true\n[schedule]\nenabled_p=false\n");
		Put(files->settings, selected);
		Put(files->syncStore, QByteArray("opaque synthetic store unchanged"));
		Put(files->fingerprintFile, QByteArray("opaque synthetic fingerprints unchanged"));
		config.start({ { a, false, false }, { b, true, false } });
		const auto local = temp.filePath(u"local.toml"_q);
		const auto original = Get(local) + QByteArray("\nunknown_local_key='retain' # untouched\n[account_work_mode]\n\"0-101\"='follow'\n\"0-202\"='stock'\n[future]\nname='non-ascii \xF0\x9F\x8C\xB1'\n");
		Put(local, original);
		config.refreshLocalChoice();
		const auto others = ChoiceOtherFiles(temp.path());
		const auto preview = config.previewGlobalChoice(a, b, u"work"_q);
		CHECK(preview);
		if (!preview) { return; }
		CHECK(!preview->enterNormal && !preview->noAppWideSettings);
		CHECK(preview->after.premium.enabled && !preview->before.premium.enabled);
		const auto oldChoice = ParseLocalChoice(QString::fromUtf8(original), local);
		auto committed = config.commitGlobalChoice(*preview, [] { return true; });
		CHECK(committed.ok && config.activateGlobalChoice(*preview, committed));
		const auto after = Get(local);
		const auto choice = ParseLocalChoice(QString::fromUtf8(after), local);
		CHECK(choice.ok() && choice.choice.globalAccount == b && choice.choice.rootFileAccount == a);
		CHECK(choice.choice.accountWorkMode == oldChoice.choice.accountWorkMode);
		CHECK(after.contains("unknown_local_key='retain' # untouched\n"));
		CHECK(after.contains("[future]\nname='non-ascii \xF0\x9F\x8C\xB1'\n"));
		CHECK(ChoiceOtherFiles(temp.path()) == others);
		CHECK(Get(files->settings) == selected);
	}
	for (auto stale = 0; stale != 6; ++stale) {
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), root);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto files = config.files(b);
		CHECK(files);
		if (!files) { return; }
		Put(files->settings, QByteArray("version=2\n"));
		config.start({ { a, false, false }, { b, true, false } });
		const auto preview = config.previewGlobalChoice(a, b, u"work"_q);
		CHECK(preview);
		if (!preview) { return; }
		const auto local = temp.filePath(u"local.toml"_q);
		const auto before = Get(local);
		if (stale == 0) { Put(local, before + QByteArray("# foreign writer\n")); }
		else if (stale == 1) { Put(files->settings, QByteArray("version=2\n[premium]\nenabled_p=true\n")); }
		else if (stale == 2) { Put(temp.filePath(u"accounts/root.toml"_q), Get(temp.filePath(u"accounts/root.toml"_q))); }
		else if (stale == 3) { config.start({ { a, false, false } }); }
		else if (stale == 4) { Put(local, before); }
		else {
			auto choice = ParseLocalChoice(QString::fromUtf8(before), local).choice;
			choice.globalAccount = b;
			const auto changed = SerializeLocalChoice(choice);
			CHECK(changed);
			if (!changed) { return; }
			Put(local, changed->toUtf8()); config.refreshLocalChoice();
			Put(local, before); config.refreshLocalChoice();
		}
		const auto expectedLocal = Get(local);
		const auto expectedOther = ChoiceOtherFiles(temp.path());
		CHECK(!config.validateGlobalChoice(*preview));
		CHECK(!config.commitGlobalChoice(*preview, [] { return true; }).ok);
		CHECK(Get(local) == expectedLocal && ChoiceOtherFiles(temp.path()) == expectedOther);
		CHECK(config.owner() == a);
	}
	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), root);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		const auto files = config.files(t);
		CHECK(files);
		if (!files) { return; }
		Put(files->settings, QByteArray("invalid ["));
		config.start({ { a, false, false }, { t, true, false } });
		CHECK(!config.previewGlobalChoice(a, t, u"work"_q));
		Put(files->good, QByteArray("version=2\n[premium]\nenabled_p=true\n"));
		const auto preview = config.previewGlobalChoice(a, t, u"work"_q);
		CHECK(preview && preview->selectedUsesGood && preview->after.premium.enabled);
		if (!preview) { return; }
		CHECK(preview->selected.file.path.contains(u"accounts/1-101/"_q));
		Put(files->good, QByteArray("version=2\n[premium]\nenabled_p=false\n"));
		CHECK(!config.validateGlobalChoice(*preview));
		CHECK(config.owner() == a);
	}
	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), root);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		config.start({ { a, false, false }, { b, false, false } });
		const auto preview = config.previewGlobalChoice(a, b, u"work"_q);
		CHECK(preview);
		if (!preview) { return; }
		const auto local = temp.filePath(u"local.toml"_q);
		const auto before = Get(local);
		auto context = std::make_unique<QObject>();
		const auto guard = QPointer<QObject>(context.get());
		auto checks = 0;
		const auto result = config.commitGlobalChoice(*preview, [&] {
			if (++checks == 2) { context.reset(); }
			return !guard.isNull();
		});
		CHECK(!result.ok && !result.changed && Get(local) == before);
		const auto foreign = before + QByteArray("# newer local writer\n");
		checks = 0;
		const auto conflict = config.commitGlobalChoice(*preview, [&] {
			if (++checks == 2) { Put(local, foreign); }
			return true;
		});
		CHECK(!conflict.ok && !conflict.changed && Get(local) == foreign);
	}
	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), root);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false }, { b, false, false } }, a);
		const auto path = temp.filePath(u"accounts/0-202/settings.toml"_q);
		Put(path, QByteArray("version=2\n"));
		auto preview = config.previewGlobalChoice(a, b, u"work"_q);
		CHECK(preview.has_value());
		if (!preview) { return; }
		auto forged = *preview;
		forged.enterNormal = false;
		CHECK(!config.validateGlobalChoice(forged));
		Put(path + u".good"_q, QByteArray("version=2\n# unrelated recovery maintenance\n"));
		CHECK(config.validateGlobalChoice(*preview));
		auto committed = config.commitGlobalChoice(*preview, [] { return true; });
		CHECK(committed.ok && committed.changed && !committed.activated);
		const auto local = temp.filePath(u"local.toml"_q);
		const auto foreign = Get(local) + QByteArray("# foreign write after publication\n");
		Put(local, foreign);
		CHECK(!config.activateGlobalChoice(*preview, committed));
		CHECK(Get(local) == foreign && config.owner() == a);
	}
#ifndef Q_OS_WIN
	{
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), root);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false }, { b, false, false } }, a);
		const auto path = temp.filePath(u"accounts/0-202/settings.toml"_q);
		Put(path, QByteArray("version=2\n"));
		const auto preview = config.previewGlobalChoice(a, b, u"work"_q);
		CHECK(preview.has_value());
		if (!preview) { return; }
		const auto replacement = temp.filePath(u"replacement.toml"_q);
		Put(replacement, Get(path));
		CHECK(QFile::remove(path));
		CHECK(QFile::link(replacement, path));
		const auto local = Get(temp.filePath(u"local.toml"_q));
		CHECK(!config.previewGlobalChoice(a, b, u"work"_q));
		CHECK(!config.commitGlobalChoice(*preview, [] { return true; }).changed);
		CHECK(Get(temp.filePath(u"local.toml"_q)) == local);
	}
#endif

}


void GlobalChoiceActivationOrderingCases() {
	const auto a = AccountKey{ 0, 101 };
	const auto b = AccountKey{ 0, 202 };
	const auto oldSettings = QByteArray("version=1\n[presets.work]\n");
	for (const auto conflict : { false, true }) {
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), oldSettings);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false }, { b, false, false } }, a);
		const auto target = temp.filePath(u"accounts/0-202/settings.toml"_q);
		Put(target, QByteArray("version=2\n"));
		const auto preview = config.previewGlobalChoice(a, b, u"work"_q);
		CHECK(preview.has_value());
		if (!preview) { return; }
		CHECK(preview->enterNormal);
		const auto local = temp.filePath(u"local.toml"_q);
		const auto foreign = QByteArray("version=2\n# foreign selected-file writer\n");
		auto injected = false;
		auto committed = config.commitGlobalChoice(*preview, [&] {
			if (conflict && !injected && Get(local) != preview->current.localInput.bytes) {
				Put(target, foreign);
				injected = true;
			}
			return true;
		});
		CHECK(committed.changed && committed.ok == !conflict);
		CHECK(injected == conflict);
		CHECK(ParseLocalChoice(QString::fromUtf8(Get(local)), local).choice.globalAccount == b);
		const auto saved = Get(local);
		auto state = State();
		state.activePreset = u"work"_q;
		state.activeSource = PresetSource::Manual;
		const auto before = SerializeState(state);
		auto transitions = QStringList();
		auto activationCalls = 0;
		const auto activated = RunGlobalAccountChoiceActivation(committed, preview->enterNormal,
			[&] { return config.validateGlobalChoiceCommit(*preview, committed); },
			[&] {
				state.activePreset = NormalPreset();
				state.activeSource = PresetSource::Manual;
				transitions.push_back(state.activePreset);
				const auto resolved = Resolve(preview->before, state.activePreset);
				return resolved && resolved->normal;
			}, [&] {
				++activationCalls;
				CHECK(state.activePreset == NormalPreset());
				CHECK(transitions == QStringList{ NormalPreset() });
				return config.activateGlobalChoice(*preview, committed);
			});
		CHECK(activated == !conflict);
		CHECK(committed.activated == !conflict);
		CHECK(activationCalls == (conflict ? 0 : 1));
		CHECK(Get(local) == saved && config.owner() == a);
		if (conflict) {
			CHECK(SerializeState(state) == before);
			CHECK(state.activePreset == u"work"_q);
			CHECK(transitions.empty());
			CHECK(Get(target) == foreign);
			CHECK(config.globalAccount() == a);
		} else {
			CHECK(state.activePreset == NormalPreset());
			CHECK(transitions == QStringList{ NormalPreset() });
			CHECK(config.globalAccount() == b);
		}
	}
	for (const auto retireBeforeNormal : { false, true }) {
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), oldSettings);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false }, { b, false, false } }, a);
		const auto target = temp.filePath(u"accounts/0-202/settings.toml"_q);
		Put(target, QByteArray("version=2\n"));
		const auto preview = config.previewGlobalChoice(a, b, u"work"_q);
		CHECK(preview.has_value());
		if (!preview) { return; }
		auto committed = config.commitGlobalChoice(*preview, [] { return true; });
		CHECK(committed.ok && committed.changed);
		const auto local = temp.filePath(u"local.toml"_q);
		const auto saved = Get(local);
		auto state = State();
		state.activePreset = u"work"_q;
		const auto before = SerializeState(state);
		auto context = std::make_unique<QObject>();
		const auto guard = QPointer<QObject>(context.get());
		if (retireBeforeNormal) { context.reset(); }
		auto gateCalls = 0;
		auto activationCalls = 0;
		CHECK(!RunGlobalAccountChoiceActivation(committed, preview->enterNormal,
			[&] { return guard && config.validateGlobalChoiceCommit(*preview, committed); },
			[&] {
				++gateCalls;
				state.activePreset = NormalPreset();
				context.reset();
				return true;
			}, [&] {
				++activationCalls;
				return config.activateGlobalChoice(*preview, committed);
			}));
		CHECK(!committed.ok && !committed.activated);
		CHECK(activationCalls == 0);
		CHECK(gateCalls == (retireBeforeNormal ? 0 : 1));
		CHECK(Get(local) == saved && config.owner() == a && config.globalAccount() == a);
		if (retireBeforeNormal) { CHECK(SerializeState(state) == before); }
		else { CHECK(state.activePreset == NormalPreset()); }
	}
}


void RemoveAppWideSettingsCases() {
	const auto a = AccountKey{ 0, 101 };
	const auto b = AccountKey{ 0, 202 };
	const auto original = QByteArray("# retained heading\nversion=1\n[premium]\nenabled_p=true\n[presets.work]\n[lists.friends]\ntitle='Friends'\nmembers=[101]\n[last_seen]\nreasons_p=false\ntrade_p=true\n[sync]\nsend_after_save_p=true\n[foreign]\nnote='untouched' # retained unknown\n");
	for (const auto rootFile : { false, true }) {
		FailureContext = QByteArray("removeAppWide:rootFile=") + QByteArray::number(rootFile);
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), rootFile ? original : QByteArray("version=1\n"));
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false }, { b, false, false } }, a);
		const auto account = rootFile ? a : b;
		if (rootFile) {
			Put(temp.filePath(u"accounts/0-202/settings.toml"_q), QByteArray("version=1\n"));
			const auto choice = config.previewGlobalChoice(a, b, NormalPreset());
			CHECK(choice.has_value());
			if (!choice) { return; }
			CHECK(config.commitGlobalChoice(*choice, [] { return true; }).ok);
		} else {
			Put(config.files(account)->settings, original);
		}
		const auto paths = *config.files(account);
		const auto other = *config.files(rootFile ? b : a);
		const auto local = Get(temp.filePath(u"local.toml"_q));
		const auto owner = Get(temp.filePath(u"accounts/root.toml"_q));
		const auto otherBytes = Get(other.settings);
		Put(paths.good, QByteArray("preserved recovery sentinel"));
		Put(paths.fingerprintFile, QByteArray("preserved fingerprint sentinel"));
		const auto target = PreviewAccountAppWideRemoval(config, account);
		CHECK(target && target->writable && !target->removed.empty());
		if (!target) { return; }
		CHECK(ValidateAccountAppWideRemoval(config, *target));
		CHECK(!PreviewAccountAppWideRemoval(config, rootFile ? b : a));
		const auto result = CommitAccountAppWideRemoval(config, *target, [] { return true; });
		CHECK(result.ok && result.changed && result.backupChanged);
		CHECK(Get(paths.backup) == original);
		const auto after = Get(paths.settings);
		CHECK(after.startsWith("# retained heading\nversion=2\n"));
		CHECK(!after.contains("[premium]") && !after.contains("[presets.work]"));
		CHECK(!after.contains("title='Friends'"));
		CHECK(after.contains("members=[101]"));
		CHECK(after.contains("reasons_p=false") && after.contains("trade_p=true"));
		CHECK(after.contains("send_after_save_p=true"));
		CHECK(after.contains("note='untouched' # retained unknown"));
		CHECK(Get(paths.good) == QByteArray("preserved recovery sentinel"));
		CHECK(Get(paths.fingerprintFile) == QByteArray("preserved fingerprint sentinel"));
		CHECK(Get(other.settings) == otherBytes);
		CHECK(Get(temp.filePath(u"local.toml"_q)) == local);
		CHECK(Get(temp.filePath(u"accounts/root.toml"_q)) == owner && config.owner() == a);
		CHECK(ParseSettings(QString::fromUtf8(after), paths.settings).settings.version == 2);
		CHECK(!config.operationTarget(account));
		CHECK(!config.observeAccountSave(account));
		CHECK(!CanApplyWholeSettingsDocument(u"settings.toml"_q, after));
		const auto second = PreviewAccountAppWideRemoval(config, account);
		CHECK(second && second->removed.empty());
		if (!second) { return; }
		const auto noop = CommitAccountAppWideRemoval(config, *second, [] { return true; });
		CHECK(noop.ok && !noop.changed && !noop.backupChanged);
		CHECK(Get(paths.backup) == original && Get(paths.settings) == after);
	}
	for (auto scenario = 0; scenario != 18; ++scenario) {
		FailureContext = QByteArray("removeAppWide:scenario=") + QByteArray::number(scenario);
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), QByteArray("version=1\n"));
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false }, { b, false, false } }, a);
		const auto paths = *config.files(b);
		Put(paths.settings, original);
		Put(paths.backup, QByteArray("old backup sentinel"));
		const auto target = PreviewAccountAppWideRemoval(config, b);
		CHECK(target && target->writable);
		if (!target) { return; }
		auto origin = std::make_unique<QObject>();
		const auto lifetime = QPointer<QObject>(origin.get());
		auto expected = original;
		if (scenario == 0) { origin.reset(); }
		if (scenario == 1) { expected += "# external edit\n"; Put(paths.settings, expected); }
		if (scenario == 2) { Put(paths.settings, original); }
		if (scenario == 3) { config.refreshSettings(); }
		if (scenario == 4) { Put(temp.filePath(u"local.toml"_q), Get(temp.filePath(u"local.toml"_q))); }
		if (scenario == 5) { Put(temp.filePath(u"accounts/root.toml"_q), Get(temp.filePath(u"accounts/root.toml"_q))); }
		if (scenario == 6) { Put(paths.backup, QByteArray("foreign backup")); }
		if (scenario == 7) {
			CHECK(QDir().rename(paths.directory, paths.directory + u".retained"_q));
			Put(paths.settings, original);
			Put(paths.backup, QByteArray("old backup sentinel"));
		}
		if (scenario == 8) { CHECK(QFile::remove(paths.settings)); expected.clear(); }
		if (scenario == 9) { CHECK(QFile::remove(paths.backup)); CHECK(QDir().mkdir(paths.backup)); }
		if (scenario == 10) {
			CHECK(QFile::setPermissions(paths.settings, QFileDevice::ReadOwner));
			const auto readonly = PreviewAccountAppWideRemoval(config, b);
			CHECK(readonly && !readonly->writable && !readonly->removed.empty());
		}
		if (scenario == 11) {
			const auto choice = config.previewGlobalChoice(a, b, NormalPreset());
			CHECK(choice.has_value());
			if (!choice) { return; }
			CHECK(config.commitGlobalChoice(*choice, [] { return true; }).ok);
		}
#ifndef Q_OS_WIN
		if (scenario == 12) {
			const auto foreign = temp.filePath(u"foreign.toml"_q);
			Put(foreign, original);
			CHECK(QFile::remove(paths.settings));
			CHECK(QFile::link(foreign, paths.settings));
		}
		if (scenario == 13) {
			const auto foreign = temp.filePath(u"foreign-backup.toml"_q);
			Put(foreign, QByteArray("old backup sentinel"));
			CHECK(QFile::remove(paths.backup));
			CHECK(QFile::link(foreign, paths.backup));
		}
#endif
		if (scenario == 14) { config.start({ { a, false, false } }); origin.reset(); }
		if (scenario == 15) { CHECK(QFile::setPermissions(paths.backup, QFileDevice::ReadOwner)); }
#ifndef Q_OS_WIN
		if (scenario == 16) {
			CHECK(::link(QFile::encodeName(paths.settings).constData(), QFile::encodeName(temp.filePath(u"hard-linked.toml"_q)).constData()) == 0);
		}
#endif
		if (scenario == 17) { expected = QByteArray("version=1\n# ") + QByteArray(1, char(0xff)); Put(paths.settings, expected); }
		const auto backup = ReadSettingsFileInput(paths.backup);
		const auto result = CommitAccountAppWideRemoval(config, *target, [=] { return bool(lifetime); });
#ifndef Q_OS_WIN
		CHECK(!result.ok && !result.changed && !result.backupChanged);
#else
		if (scenario != 12 && scenario != 13 && scenario != 16) { CHECK(!result.ok && !result.changed && !result.backupChanged); }
#endif
		if (scenario != 9) { CHECK(ReadSettingsFileInput(paths.backup) == backup); }
		if (scenario != 8) { CHECK(Get(paths.settings) == expected); }
		if (scenario == 10) { CHECK(QFile::setPermissions(paths.settings, QFileDevice::ReadOwner | QFileDevice::WriteOwner)); }
		if (scenario == 15) { CHECK(QFile::setPermissions(paths.backup, QFileDevice::ReadOwner | QFileDevice::WriteOwner)); }
	}
	auto inputIndex = 0;
	for (const auto input : { QByteArray("version=1\n[last_seen]\nreasons_p=false\n"),
		QByteArray("version=3\n[premium]\nenabled_p=true\n"), QByteArray("version=1\n[premium\n"),
		QByteArray("version=1\n[last_seen]\ntrade_remember={bad=true}\n") }) {
		FailureContext = QByteArray("removeAppWide:invalidInput=") + QByteArray::number(inputIndex++);
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), QByteArray("version=1\n"));
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false }, { b, false, false } }, a);
		const auto paths = *config.files(b);
		Put(paths.settings, input);
		Put(paths.good, original);
		const auto preview = PreviewAccountAppWideRemoval(config, b);
		if (input.startsWith("version=1\n[last_seen]\nreasons_p=false")) {
			CHECK(preview && preview->removed.empty());
			if (!preview) { return; }
			const auto result = CommitAccountAppWideRemoval(config, *preview, [] { return true; });
			CHECK(result.ok && !result.changed && !result.backupChanged);
		} else { CHECK(!preview); }
		CHECK(Get(paths.settings) == input && Get(paths.good) == original);
		CHECK(!QFileInfo::exists(paths.backup));
	}
	for (const auto afterMain : { false, true }) {
		FailureContext = QByteArray("removeAppWide:foreignAfterMain=") + QByteArray::number(afterMain);
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), QByteArray("version=1\n"));
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false }, { b, false, false } }, a);
		const auto paths = *config.files(b);
		Put(paths.settings, original);
		Put(paths.backup, QByteArray("old backup sentinel"));
		const auto target = PreviewAccountAppWideRemoval(config, b);
		CHECK(target.has_value());
		if (!target) { return; }
		auto foreignWritten = false;
		const auto foreign = QByteArray("version=1\n# foreign writer wins\n");
		const auto result = CommitAccountAppWideRemoval(config, *target, [&] {
			if (!foreignWritten && Get(paths.backup) == original
				&& (Get(paths.settings) != original) == afterMain) {
				Put(paths.settings, foreign);
				foreignWritten = true;
			}
			return true;
		});
		CHECK(foreignWritten && !result.ok && result.backupChanged);
		CHECK(result.changed == afterMain);
		CHECK(Get(paths.settings) == foreign && Get(paths.backup) == original);
	}
	{
		FailureContext = "removeAppWide:ordinaryStructuredPerAccount";
		auto temp = QTemporaryDir();
		Put(temp.filePath(u"settings.toml"_q), QByteArray("version=1\n"));
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false }, { b, false, false } }, a);
		const auto paths = *config.files(b);
		const auto originalStructured = QByteArray("version=1\n[premium]\nenabled_p=true\n[last_seen]\nreasons_p={bad=true}\n[unknown]\nnote='preserved' # comment\n");
		Put(paths.settings, originalStructured);
		const auto preview = PreviewAccountAppWideRemoval(config, b);
		CHECK(preview && preview->writable && !preview->removed.empty());
		if (!preview) { return; }
		const auto result = CommitAccountAppWideRemoval(config, *preview, [] { return true; });
		CHECK(result.ok && result.changed && result.backupChanged);
		CHECK(Get(paths.backup) == originalStructured);
		const auto after = Get(paths.settings);
		CHECK(after.startsWith("version=2\n"));
		CHECK(!after.contains("[premium]") && !after.contains("enabled_p=true"));
		CHECK(after.contains("reasons_p={bad=true}"));
		CHECK(after.contains("note='preserved' # comment"));
		CHECK(!config.operationTarget(b));
	}
	FailureContext.clear();
}

void RoutedWritersAndListCases() {
	const auto a = AccountKey{ 0, 101 };
	const auto b = AccountKey{ 0, 202 };
	const auto root = QByteArray(
		"# whole source\nversion=1\n[premium]\nenabled_p=false\n"
		"[lists.\"work.one\"]\ntitle='Work'\nkind='users'\nmembers=[\n"
		"  11, # inherited eleven\n  22, # inherited  twenty-two\n]\n"
		"[lists.other]\nmembers=[44]\n[unknown]\nkeep='root'\n");
	const auto switchGlobal = [&](DesktopAccountConfig &config, const AccountKey &key) {
		const auto preview = config.previewGlobalChoice(a, key, NormalPreset());
		CHECK(preview.has_value());
		if (!preview) { return; }
		auto commit = config.commitGlobalChoice(*preview, [] { return true; });
		CHECK(commit.ok);
		CHECK(config.activateGlobalChoice(*preview, commit));
	};
	{
		auto temp = QTemporaryDir();
		const auto rootPath = temp.filePath(u"settings.toml"_q);
		Put(rootPath, root);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		config.start({ { a, false, false }, { b, false, false } });
		const auto ownPath = config.files(b)->settings;
		const auto target = config.listEditTarget(b, u"work.one"_q);
		CHECK(target && target->edit.destination.file.account == b);
		if (!target) { return; }
		CHECK(target->members == (std::vector<PeerIdValue>{ 11, 22 }));
		CHECK(target->comments.contains(11) && target->comments.at(11) == u"inherited eleven"_q);
		CHECK(!QFileInfo::exists(QFileInfo(ownPath).absolutePath()));
		auto resolved = std::vector<PeerIdValue>();
		const auto created = config.writeListMember(*target, 33, true, [&](PeerIdValue id) {
			resolved.push_back(id);
			return u"changed chat"_q;
		}, [] { return true; });
		CHECK(created.ok && created.changed && created.created && created.target);
		CHECK(resolved == (std::vector<PeerIdValue>{ 33 }));
		CHECK(Get(rootPath) == root);
		CHECK(Get(ownPath).startsWith(NewAccountSettingsText()));
		CHECK(!Get(ownPath).contains("title=") && !Get(ownPath).contains("kind="));
		CHECK(!Get(ownPath).contains("[premium]") && !Get(ownPath).contains("[lists.other]"));
		CHECK(Get(ownPath + u".good"_q) == Get(ownPath));
		CHECK(!QFileInfo::exists(ownPath + u".bak"_q));
		const auto comments = ListMemberComments(QString::fromUtf8(Get(ownPath)), ownPath, u"work.one"_q);
		CHECK(comments.contains(11) && comments.at(11) == u"inherited eleven"_q);
		CHECK(comments.contains(22) && comments.at(22) == u"inherited  twenty-two"_q);
		CHECK(comments.contains(33) && comments.at(33) == u"changed chat"_q);
		CHECK(!config.writeListMember(*target, 55, true, {}, [] { return true; }).ok);
		for (const auto id : { 11, 22, 33 }) {
			const auto next = config.listEditTarget(b, u"work.one"_q);
			CHECK(next.has_value());
			if (!next) { return; }
			CHECK(config.writeListMember(*next, id, false, {}, [] { return true; }).ok);
		}
		CHECK(config.settings(b).lists.front().members.empty());
		CHECK(Get(ownPath).contains("members"));
		CHECK(Get(rootPath) == root);
		const auto ownBefore = Get(ownPath);
		const auto localPath = temp.filePath(u"local.toml"_q);
		const auto follow = SetAccountWorkMode(QString::fromUtf8(Get(localPath)), localPath, b, AccountWorkMode::Follow);
		CHECK(follow.ok() && follow.changed);
		Put(localPath, follow.text.toUtf8());
		config.refreshLocalChoice();
		const auto followed = config.listEditTarget(b, u"work.one"_q);
		CHECK(followed && followed->edit.destination.file.account == a);
		if (!followed) { return; }
		CHECK(followed->members == (std::vector<PeerIdValue>{ 11, 22 }));
		CHECK(config.writeListMember(*followed, 22, false, {}, [] { return true; }).ok);
		CHECK(Get(ownPath) == ownBefore);
		CHECK(Get(rootPath + u".bak"_q) == root);
		CHECK(ListMemberComments(QString::fromUtf8(Get(rootPath)), rootPath, u"work.one"_q)
			== (std::map<PeerIdValue, QString>{ { 11, u"inherited eleven"_q } }));
		const auto stock = SetAccountWorkMode(QString::fromUtf8(Get(localPath)), localPath, b, AccountWorkMode::Stock);
		CHECK(stock.ok());
		Put(localPath, stock.text.toUtf8());
		CHECK(!config.writeListMember(*followed, 66, true, {}, [] { return true; }).ok);
		const auto stocked = config.listEditTarget(b, u"work.one"_q);
		CHECK(stocked && stocked->edit.destination.file.account == b && stocked->members.empty());
		if (!stocked) { return; }
		CHECK(config.writeListMember(*stocked, 77, true, {}, [] { return true; }).ok);
		CHECK(config.settings(b).lists.front().members == (std::vector<PeerIdValue>{ 77 }));
	}
	{
		auto temp = QTemporaryDir();
		const auto rootPath = temp.filePath(u"settings.toml"_q);
		Put(rootPath, root);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		config.start({ { a, false, false }, { b, false, false } });
		const auto ownPath = config.files(b)->settings;
		Put(ownPath, QByteArray("version=1\n[lists.\"work.one\"]\ntitle='Own whole list'\nmembers=[11] # retain array\n"));
		const auto own = config.listEditTarget(b, u"work.one"_q);
		CHECK(own.has_value());
		if (!own) { return; }
		CHECK(config.writeListMember(*own, 11, false, {}, [] { return true; }).ok);
		const auto bytes = Get(ownPath);
		CHECK(bytes.contains("title='Own whole list'"));
		CHECK(bytes.contains("# retain array"));
		const auto parsed = ParseSettings(QString::fromUtf8(bytes), ownPath);
		CHECK(parsed.ok() && parsed.settings.list(u"work.one"_q)
			&& parsed.settings.list(u"work.one"_q)->members.empty());
		CHECK(bytes.startsWith("version=1\n"));
		CHECK(Get(rootPath) == root);
		switchGlobal(config, b);
		config.start({ { a, false, false } });
		const auto offline = config.settingsEditTarget(a, SettingsOpenRole::AppWide);
		CHECK(offline && offline->destination.file.account == b && offline->destination.requester == a);
		if (!offline) { return; }
		const auto before = Get(ownPath);
		const auto changed = config.writeSettingsEdit(*offline, [&](const QString &text) {
			return SetTableBool(text, ownPath, u"premium"_q, u"enabled_p"_q, true);
		}, [] { return true; });
		CHECK(changed.ok && changed.changed && !changed.created && changed.target);
		CHECK(Get(rootPath) == root && Get(ownPath + u".bak"_q) == before);
		CHECK(Get(ownPath + u".good"_q) == Get(ownPath));
		CHECK(config.globalSettings().premium.enabled);
		CHECK(!config.writeSettingsEdit(*offline, [](const QString &text) {
			return SpliceResult{ text, false };
		}, [] { return true; }).ok);
		const auto current = config.settingsEditTarget(a, SettingsOpenRole::AppWide);
		CHECK(current.has_value());
		if (!current) { return; }
		const auto noOp = config.writeSettingsEdit(*current, [&](const QString &text) {
			return SetTableBool(text, ownPath, u"premium"_q, u"enabled_p"_q, true);
		}, [] { return true; });
		CHECK(noOp.ok && !noOp.changed);
		Put(ownPath, Get(ownPath));
		CHECK(!config.validateSettingsEdit(*current));
		const auto fresh = config.settingsEditTarget(a, SettingsOpenRole::AppWide);
		CHECK(fresh.has_value());
		if (!fresh) { return; }
		const auto unchanged = Get(ownPath);
		CHECK(!config.writeSettingsEdit(*fresh, [&](const QString &text) {
			return SetTableBool(text, ownPath, u"premium"_q, u"enabled_p"_q, false);
		}, [] { return false; }).ok);
		CHECK(Get(ownPath) == unchanged);
		switchGlobal(config, a);
		CHECK(!config.validateSettingsEdit(*fresh));
		CHECK(Get(ownPath) == unchanged && Get(rootPath) == root);
	}
	{
		auto temp = QTemporaryDir();
		const auto rootPath = temp.filePath(u"settings.toml"_q);
		Put(rootPath, root);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		config.start({ { a, false, false }, { b, false, false } });
		switchGlobal(config, b);
		config.start({ { a, false, false } });
		const auto missing = config.settingsEditTarget(a, SettingsOpenRole::AppWide);
		CHECK(missing.has_value());
		if (!missing) { return; }
		const auto path = missing->destination.file.path;
		const auto created = config.writeSettingsEdit(*missing, [&](const QString &text) {
			return AddList(text, path, u"offline"_q, u"Offline list"_q);
		}, [] { return true; });
		CHECK(created.ok && created.created && Get(path).startsWith(NewAccountSettingsText()));
		CHECK(Get(rootPath) == root && !config.observeAccountSave(b));
		Put(path, QByteArray("broken ["));
		CHECK(!config.settingsEditTarget(a, SettingsOpenRole::AppWide));
		const auto inherited = config.listEditTarget(a, u"offline"_q);
		CHECK(inherited.has_value());
		if (!inherited) { return; }
		const auto capturedGood = Get(path + u".good"_q);
		Put(path + u".good"_q, capturedGood + "\n# external recovery edit\n");
		CHECK(!config.writeListMember(*inherited, 11, true, {}, [] { return true; }).ok);
		CHECK(Get(rootPath) == root);
		const auto repairedSource = config.listEditTarget(a, u"offline"_q);
		CHECK(repairedSource.has_value());
		if (!repairedSource) { return; }
		CHECK(config.writeListMember(*repairedSource, 11, true, {}, [] { return true; }).ok);
		CHECK(Get(path) == QByteArray("broken ["));
		CHECK(Get(rootPath).contains("[lists.offline]"));
		CHECK(!CanReplaceLegacyRootSettings(root, NewAccountSettingsText()));
		CHECK(!AccountSyncRecordSupported(NewAccountSettingsText()));
	}
	{
		auto temp = QTemporaryDir();
		const auto path = temp.filePath(u"settings.toml"_q);
		Put(path, root);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		config.start({ { a, false, false }, { b, false, false } });
		const auto ownPath = config.files(b)->settings;
		const auto inherited = config.listEditTarget(b, u"work.one"_q);
		CHECK(inherited.has_value());
		if (!inherited) { return; }
		const auto unchanged = config.writeListMember(*inherited, 11, true, {}, [] { return true; });
		CHECK(unchanged.ok && !unchanged.changed && !QFileInfo::exists(ownPath));
		const auto stale = config.writeListMember(*inherited, 33, true, [&](PeerIdValue id) {
			CHECK(id == 33);
			Put(path, root + "\n# concurrent external edit\n");
			return u"new chat"_q;
		}, [] { return true; });
		CHECK(!stale.ok && !stale.changed && !QFileInfo::exists(ownPath));
		CHECK(Get(path) == root + "\n# concurrent external edit\n");
		const auto fresh = config.listEditTarget(b, u"work.one"_q);
		CHECK(fresh.has_value());
		if (!fresh) { return; }
		auto alive = true;
		config.setChanged([&] { alive = false; });
		const auto interrupted = config.writeListMember(*fresh, 33, true, {}, [&] { return alive; });
		CHECK(!interrupted.ok && interrupted.changed && interrupted.created && !interrupted.target);
		CHECK(Get(path) == root + "\n# concurrent external edit\n");
		CHECK(ParseSettings(QString::fromUtf8(Get(ownPath)), ownPath).ok());
	}
	{
		auto temp = QTemporaryDir();
		const auto path = temp.filePath(u"settings.toml"_q);
		Put(path, root);
		auto config = DesktopAccountConfig(temp.path());
		config.start({ { a, false, false } });
		config.start({ { a, false, false }, { b, false, false } });
		const auto inherited = config.listEditTarget(b, u"work.one"_q);
		CHECK(inherited.has_value());
		if (!inherited) { return; }
		const auto before = ChoiceOtherFiles(temp.path());
		auto forged = *inherited;
		forged.members = { 99 };
		CHECK(!config.writeListMember(forged, 33, true, {}, [] { return true; }).ok);
		CHECK(ChoiceOtherFiles(temp.path()) == before);
		Put(path + u".good"_q, root);
		CHECK(QFile::remove(path));
		const auto recovery = config.listEditTarget(b, u"work.one"_q);
		CHECK(recovery && recovery->members == (std::vector<PeerIdValue>{ 11, 22 }));
		CHECK(!config.settingsEditTarget(b, SettingsOpenRole::AppWide));
		if (!recovery) { return; }
		CHECK(config.writeListMember(*recovery, 33, true, {}, [] { return true; }).ok);
		CHECK(!QFileInfo::exists(path));
		CHECK(Get(path + u".good"_q) == root);
		const auto ownPath = config.files(b)->settings;
		CHECK(Get(ownPath).startsWith(NewAccountSettingsText()));
	}

}

void PeekOpeningEditorCases() {
	const auto a = AccountKey{ 0, 101 };
	const auto b = AccountKey{ 0, 202 };
	const auto bytes = QByteArray("version=1\n[peek]\nstart_delay_seconds=3\n");
	auto temp = QTemporaryDir();
	const auto aPath = temp.filePath(u"settings.toml"_q);
	Put(aPath, bytes);
	auto config = DesktopAccountConfig(temp.path());
	config.start({ { a, false, false } });
	config.start({ { a, false, false }, { b, false, false } });
	const auto bPath = config.files(b)->settings;
	Put(bPath, bytes);
	const auto opening = config.settingsEditTarget(a, SettingsOpenRole::AppWide);
	CHECK(opening.has_value());
	if (!opening) { return; }
	auto sessionCurrent = true;
	const auto originCurrent = [&] { return sessionCurrent; };
	const auto validate = [&](const AccountSettingsWriteTarget &target) {
		return config.validateSettingsEdit(target);
	};
	const auto countdown = PreparePeekDelayChange(opening, originCurrent, validate);
	CHECK(countdown && countdown->destination.file.account == a);
	const auto switchTo = [&](const AccountKey &key) {
		const auto preview = config.previewGlobalChoice(a, key, NormalPreset());
		CHECK(preview.has_value());
		if (!preview) { return; }
		auto commit = config.commitGlobalChoice(*preview, [] { return true; });
		CHECK(commit.ok && config.activateGlobalChoice(*preview, commit));
	};
	switchTo(b);
	CHECK(!PreparePeekDelayChange(opening, originCurrent, validate));
	const auto before = ChoiceOtherFiles(temp.path());
	if (countdown) {
		const auto completed = config.writeSettingsEdit(*countdown, [&](const QString &text) {
			return SetPeekStartDelay(text, countdown->destination.file.path, 9);
		}, originCurrent);
		CHECK(!completed.ok && !completed.changed);
	}
	CHECK(ChoiceOtherFiles(temp.path()) == before);
	CHECK(Get(aPath) == bytes && Get(bPath) == bytes);
	const auto fresh = config.settingsEditTarget(a, SettingsOpenRole::AppWide);
	const auto accepted = PreparePeekDelayChange(fresh, originCurrent, validate);
	CHECK(accepted && accepted->destination.file.account == b);
	if (!accepted) { return; }
	sessionCurrent = false;
	CHECK(!PreparePeekDelayChange(fresh, originCurrent, validate));
	CHECK(!config.writeSettingsEdit(*accepted, [&](const QString &text) {
		return SetPeekStartDelay(text, bPath, 9);
	}, originCurrent).ok);
	CHECK(Get(aPath) == bytes && Get(bPath) == bytes);
	sessionCurrent = true;
	CHECK(!PreparePeekDelayChange(fresh, originCurrent, [&](const AccountSettingsWriteTarget &target) {
		const auto valid = config.validateSettingsEdit(target);
		sessionCurrent = false;
		return valid;
	}));
	sessionCurrent = true;
	const auto written = config.writeSettingsEdit(*accepted, [&](const QString &text) {
		return SetPeekStartDelay(text, bPath, 9);
	}, originCurrent);
	CHECK(written.ok && written.changed && written.target);
	CHECK(Get(aPath) == bytes);
	CHECK(ParseSettings(QString::fromUtf8(Get(bPath)), bPath).settings.peek.startDelaySeconds == 9);
	CHECK(PreparePeekDelayChange(written.target, originCurrent, validate).has_value());
	CHECK(!PreparePeekDelayChange(fresh, originCurrent, validate));
	switchTo(a);
	CHECK(!PreparePeekDelayChange(opening, originCurrent, validate));
	CHECK(!PreparePeekDelayChange(written.target, originCurrent, validate));
	CHECK(Get(aPath) == bytes);
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
	std::printf("priorBatch789 %d checks, %d failures\n", Checks, Failures);
	const auto beforeCreation = Checks;
	AccountSettingsCreationAndDocumentCases();
	std::printf("accountCreationDocument %d checks\n", Checks - beforeCreation);
	std::printf("priorBatch967 %d checks, %d failures\n", Checks, Failures);
	const auto beforeRows = Checks;
	AccountSettingsRowsAndResetCases();
	std::printf("accountRowsReset %d checks\n", Checks - beforeRows);
	std::printf("priorBatch1340 %d checks, %d failures\n", Checks, Failures);
	const auto beforeGlobalChoice = Checks;
	GlobalAccountChoiceCases();
	std::printf("globalAccountChoice %d checks\n", Checks - beforeGlobalChoice);
	std::printf("priorBatch1580 %d checks, %d failures\n", Checks, Failures);
	const auto beforeActivationRepair = Checks;
	GlobalChoiceActivationOrderingCases();
	std::printf("activationOrdering %d checks\n", Checks - beforeActivationRepair);
	std::printf("priorBatch1660 %d checks, %d failures\n", Checks, Failures);
	const auto beforeRemoval = Checks;
	RemoveAppWideSettingsCases();
	std::printf("removeAppWide %d checks\n", Checks - beforeRemoval);
	const auto beforeRoutedWriters = Checks;
	RoutedWritersAndListCases();
	std::printf("routedWritersLists %d checks\n", Checks - beforeRoutedWriters);
	const auto beforePeekEditor = Checks;
	PeekOpeningEditorCases();
	std::printf("peekOpeningEditor %d checks\n", Checks - beforePeekEditor);
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
