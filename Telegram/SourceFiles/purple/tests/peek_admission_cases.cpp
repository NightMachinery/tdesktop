#include "purple/purple_account_config.h"
#include "purple/purple_config_file.h"
#include "purple/purple_engine.h"
#include "purple/purple_state.h"
#include "purple/purple_wait_gate.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

#include <cstdio>

using namespace Purple;

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	auto checks = 0;
	auto failures = 0;
	const auto check = [&](bool value, const char *name) {
		++checks;
		if (!value) {
			++failures;
			std::printf("FAIL %s\n", name);
		}
	};
	auto temp = QTemporaryDir();
	check(temp.isValid(), "fixture directory");
	const auto path = temp.filePath(u"settings.toml"_q);
	const auto bytes = QByteArray("version = 1\n[presets.work]\nlist_order = []\n");
	const auto write = [&](const QString &name, const QByteArray &content) {
		auto file = QFile(name);
		return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
			&& file.write(content) == content.size() && file.flush();
	};
	check(write(path, bytes), "create settings fixture");
	check(write(path + u".good"_q, bytes), "create recovery fixture");
	auto routing = DesktopAccountConfig(temp.path());
	check(!routing.globalAccount(), "ownerless settings have no edit route");
	check(routing.ownerPlan().status == AccountOwnerStartupStatus::WaitingForFirstSignIn,
		"missing owner metadata matches the reported admission category");
	check(!routing.settingsEditTarget(AccountKey{ 0, 101 }, SettingsOpenRole::AppWide),
		"real settings edit admission remains refused");
	const auto &settings = routing.globalSettings();
	const auto resolved = Resolve(settings, u"work"_q);
	check(resolved && !resolved->normal, "ownerless settings still run Work Mode");
	const auto loaded = ReadSettingsFileInput(path);
	const auto receipt = routing.globalSettingsRead();
	const auto read = receipt.primaryTarget;
	check(routing.validateSettingsRead(receipt), "actual accepted loader input authorizes Peek");
	check(read.has_value(), "capture readable settings without an owner");
	check(read && ValidateSettingsReadTarget(*read), "unchanged read snapshot admitted");
	check(PeekSettingsInputUsable(loaded), "accepted primary usable for Peek");
	check(!WaitNeedsSettingsEdit(WaitAction::StartPeek, false), "start does not edit settings");
	check(!WaitNeedsSettingsEdit(WaitAction::ExtendPeek, false), "extend does not edit settings");
	check(WaitNeedsSettingsEdit(WaitAction::ChangePreset, false), "preset edit guard preserved");
	check(WaitNeedsSettingsEdit(WaitAction::ChangePeekOption, true), "file option edit guard preserved");
	check(!WaitNeedsSettingsEdit(WaitAction::ChangePeekOption, false), "local option needs no file edit");
	for (const auto delay : { 0, 2 }) {
		auto state = State();
		state.activePreset = u"work"_q;
		auto gate = WaitGate();
		auto origin = WaitOrigin{ true, true, true };
		const auto validate = [&]() -> QString {
			if (!WaitOriginValid(WaitAction::StartPeek, origin)) return u"context"_q;
			if (WaitNeedsSettingsEdit(WaitAction::StartPeek, false) && !routing.globalAccount()) {
				return u"edit admission"_q;
			}
			return read && ValidateSettingsReadTarget(*read) && PeekSettingsInputUsable(loaded)
				? QString() : u"settings changed"_q;
		};
		const auto request = [&] {
			return gate.request({
				.action = WaitAction::StartPeek,
				.delaySeconds = delay,
				.validate = validate,
				.originCurrent = [&] { return WaitOriginValid(WaitAction::StartPeek, origin); },
				.apply = [&](const std::function<bool()> &current) -> QString {
					if (!current()) return u"context"_q;
					[[maybe_unused]] const auto started = StartPeek(state, 1000, 120);
					return {};
				},
			}, 0);
		};
		const auto result = request();
		check(result.outcome == (delay ? WaitOutcome::Pending : WaitOutcome::Applied),
			"ownerless Peek request admitted");
		if (delay) {
			check(gate.tick(2000).outcome == WaitOutcome::Applied, "ownerless countdown applies");
		}
		check(PeekLive(state, 1000), "Peek activated");
		state.peekActive = false;
		request();
		if (delay) {
			check(write(path, bytes + "# edit before watcher settles\n"), "external synthetic edit");
			check(gate.tick(2000).outcome == WaitOutcome::Refused, "synchronous input change refuses");
			check(!state.peekActive, "changed input never reveals chats");
			check(write(path, bytes), "restore synthetic fixture");
		}
		for (auto guard = 0; guard != 3; ++guard) {
			origin = { true, true, true };
			if (guard == 0) origin.accountValid = false;
			if (guard == 1) origin.displayedAccountSame = false;
			if (guard == 2) origin.foregroundUnlocked = false;
			check(request().outcome == WaitOutcome::Refused, "account window and lock guards retained");
		}
	}
	for (const auto invalid : { SettingsFileInput{}, SettingsFileInput{ SettingsFileState::ReadError },
		SettingsFileInput{ SettingsFileState::Readable, "version = [" },
		SettingsFileInput{ SettingsFileState::Readable, QByteArray(1, char(0xff)) },
		SettingsFileInput{ SettingsFileState::Readable, "version = 999\n" } }) {
		check(!PeekSettingsInputUsable(invalid), "unusable primary still refuses");
	}
	check(read && !CaptureSettingsReadTarget(temp.path(), path,
		{ SettingsFileState::Readable, "version = 1\n" }), "stale loaded primary refuses capture");
	check(write(temp.filePath(u"local.toml"_q), "# changed routing\n"), "external routing fixture edit");
	check(read && !ValidateSettingsReadTarget(*read), "routing metadata change refuses");
	for (const auto symlink : { true, false }) {
		auto fixture = QTemporaryDir();
		check(fixture.isValid(), "loader association fixture");
		const auto primary = fixture.filePath(u"settings.toml"_q);
		const auto target = fixture.filePath(u"target.toml"_q);
		const auto normal = QByteArray("version = 1\n");
		check(write(primary + u".good"_q, bytes), "Work recovery fixture");
		if (symlink) {
			check(write(target, normal), "normal symlink target");
			check(QFile::link(target, primary), "primary is symlink");
		} else {
			check(write(primary, normal), "outer watcher reads A");
		}
		const auto outer = ReadSettingsFileInput(primary);
		if (!symlink) {
			check(write(primary, bytes), "loader reads B after outer A");
		}
		auto loader = DesktopAccountConfig(fixture.path());
		const auto effective = loader.globalSettings();
		const auto accepted = loader.globalSettingsRead();
		check(Resolve(effective, u"work"_q).has_value(), "running filter uses Work settings");
		check(accepted.acceptedInput.bytes == bytes, "receipt has exact accepted bytes");
		check(accepted.effectiveAccepted, "receipt records accepted effective result");
		check(accepted.source == (symlink ? AcceptedSettingsSource::Recovery
			: AcceptedSettingsSource::Primary), "receipt records selected source");
		check(accepted.selectedPath == (symlink ? primary + u".good"_q : primary),
			"receipt records selected path");
		if (symlink) {
			check(!CaptureSettingsReadTarget(fixture.path(), primary, outer),
				"read capture rejects symlink like routing loader");
			check(!accepted.primaryTarget, "recovery has no primary read authority");
		} else {
			check(write(primary, normal), "disk returns to A before watcher");
			check(CaptureSettingsReadTarget(fixture.path(), primary, outer).has_value(),
				"outer A remains internally valid but is not authority");
			check(accepted.primaryTarget.has_value(), "B receipt exists for actual load");
			check(!ValidateSettingsReadTarget(*accepted.primaryTarget),
				"accepted B receipt rejects restored A");
		}
		check(!loader.validateSettingsRead(accepted), "Peek refuses wrong or recovered input");
		check(!Resolve(ParseSettings(QString::fromUtf8(outer.bytes), primary).settings, u"work"_q),
			"outer bytes differ from actual running filter");
	}
	check(QFile::remove(temp.filePath(u"local.toml"_q)), "restore routing fixture");
	routing.refreshSettings();
	routing.globalSettings();
	const auto beforeRestart = routing.globalSettingsRead();
	check(routing.validateSettingsRead(beforeRestart), "fresh receipt before routing restart");
	routing.start({});
	check(!routing.validateSettingsRead(beforeRestart), "routing generation change revokes receipt");

	auto state = State();
	check(CheckPeekRequest(WaitAction::StartPeek, false, state, 1000, 120)
		== PeekRequestRefusal::WorkModeOff, "Work Mode off has its own refusal");
	check(CheckPeekRequest(WaitAction::ExtendPeek, true, state, 1000, 120)
		== PeekRequestRefusal::Ended, "inactive Peek has ended refusal");
	state.peekActive = true;
	state.peekDeadlineUnix = 999;
	check(CheckPeekRequest(WaitAction::ExtendPeek, true, state, 1000, 120)
		== PeekRequestRefusal::Ended, "expired Peek has ended refusal");
	state.peekDeadlineUnix = 0;
	check(CheckPeekRequest(WaitAction::ExtendPeek, true, state, 1000, 120)
		== PeekRequestRefusal::Untimed, "unlimited Peek has no timer refusal");
	state.peekDeadlineUnix = 1000 + kPeekExtendCapSeconds;
	check(CheckPeekRequest(WaitAction::ExtendPeek, true, state, 1000, 120)
		== PeekRequestRefusal::Limit, "capped Peek has limit refusal");
	state.peekDeadlineUnix = 1100;
	check(CheckPeekRequest(WaitAction::ExtendPeek, true, state, 1000, 0)
		== PeekRequestRefusal::InvalidExtension, "invalid duration has its own refusal");
	check(CheckPeekRequest(WaitAction::ExtendPeek, true, state, 1000, 120)
		== PeekRequestRefusal::None, "ordinary extension stays available");
	check(CheckPeekRequest(WaitAction::StartPeek, true, State(), 1000, 120)
		== PeekRequestRefusal::None, "active Work Mode permits a new Peek");
	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
