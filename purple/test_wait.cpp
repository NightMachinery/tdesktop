#include "purple/purple_wait_gate.h"
#include "purple/purple_engine.h"
#include "purple/purple_state.h"
#include "purple/purple_splice.h"
#include "purple/purple_wait_key_filter.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtGui/QAction>
#include <QtGui/QCloseEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QWindow>
#include <QtGui/qpa/qwindowsysteminterface.h>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>

#include <cstdio>

Q_GUI_EXPORT void qt_handleKeyEvent(
	QWindow*, QEvent::Type, int, Qt::KeyboardModifiers, const QString&, bool, ushort);

namespace {

using namespace Purple;
auto Checks = 0;
auto Failures = 0;

void Check(bool ok, int line) {
	++Checks;
	if (!ok) {
		++Failures;
		std::printf("FAIL wait:%d\n", line);
	}
}

#define CHECK(expression) Check(bool(expression), __LINE__)

QString operator""_q(const char16_t *text, size_t size) { return QString::fromUtf16(text, int(size)); }

namespace FocusPreferenceProbe {

namespace Main { struct Session {}; }
template <typename T> using not_null = T;
enum class SettingsOpenRole { AppWide, Own };
struct AccountSettingsWriteTarget {
	struct {
		struct { QString path = QString::fromLatin1("settings.toml"); } file;
		QString root = QString::fromLatin1("fixture");
		SettingsOpenRole role = SettingsOpenRole::AppWide;
		int requester = 1;
	} destination;
	int identity = 0;
};
struct AppWideSettingsEditorState {
	std::optional<AccountSettingsWriteTarget> target;
	std::function<bool()> originCurrent;
};
struct PeekOptionChange {
	std::optional<int> startDelaySeconds;
	std::optional<bool> exitHiddenChats;
	std::optional<bool> endOnFocusLoss;
};
struct LocalSettings {
	bool present = false;
	bool value = false;
	int writes = 0;
	template <typename T> T readPref(const char *key, bool fallback) {
		CHECK(QByteArray(key) == "purple-peek-end-on-focus-loss");
		return present ? value : fallback;
	}
	template <typename T> void writePref(const char *key, bool next) {
		CHECK(QByteArray(key) == "purple-peek-end-on-focus-loss");
		present = true; value = next; ++writes;
	}
};
namespace Core {
struct Application {
	LocalSettings local;
	int saves = 0;
	LocalSettings &settings() { return local; }
	void saveSettingsDelayed() { ++saves; }
};
Application &App() { static auto app = Application(); return app; }
}
namespace tr {
inline constexpr auto now = 0;
QString lng_purple_wait_context_changed(int) { return QString::fromLatin1("context changed"); }
QString lng_purple_wait_editor_closed(int) { return QString::fromLatin1("editor closed"); }
QString lng_purple_wait_delay_invalid(int) { return QString::fromLatin1("invalid delay"); }
}
struct Changes { int delivered = 0; void fire_copy(bool) { ++delivered; } } FocusLossChanges;
struct WaitRequest {
	WaitAction action = WaitAction::StartPeek;
	Main::Session *session = nullptr;
	int seconds = 0;
	std::shared_ptr<AppWideSettingsEditorState> editor;
	bool same = false;
	bool onlyTightening = false;
	bool writesSettings = false;
	std::function<QString()> validate;
	std::function<QString(const std::function<bool()>&)> apply;
};
auto Gate = WaitGate();
auto Last = WaitResult();
auto Loaded = Settings();
auto Text = QString();
auto FileIdentity = 1;
auto AccountIdentity = 1;
auto FileWrites = 0;
auto UsedTarget = 0;
auto FileFails = false;
std::function<void()> BeforeCommit;
const Settings &ActiveSettings() { return Loaded; }
struct WriteResult {
	bool ok = false;
	QString error;
	std::optional<AccountSettingsWriteTarget> target;
};
template <typename Signature> using Fn = std::function<Signature>;
std::function<void()> AfterReload;
auto CompletionChecks = 0;
QString ConfigDirectory() { return QString::fromLatin1("fixture"); }
void ShowOfflineGlobalEditBulletin() { if (AfterReload) AfterReload(); }
struct RoutingPort {
	bool validateSettingsEdit(const AccountSettingsWriteTarget &target) { return target.identity == FileIdentity; }
	std::optional<AccountSettingsWriteTarget> settingsEditTarget(int, SettingsOpenRole, QString*) {
		return AccountSettingsWriteTarget{.identity = FileIdentity};
	}
	struct Result : WriteResult { bool changed = false; };
	template <typename F> Result writeSettingsEdit(const AccountSettingsWriteTarget &target, F splice, const std::function<bool()> &current) {
		UsedTarget = target.identity;
		if (!current() || target.identity != FileIdentity || FileFails) return {{.error = QString::fromLatin1("stale or held")}};
		const auto result = splice(Text);
		if (!result.ok()) return {{.error = result.error}};
		if (BeforeCommit) BeforeCommit();
		if (!current() || target.identity != FileIdentity) return {{.error = QString::fromLatin1("stale")}};
		const auto changed = result.text != Text;
		if (changed) { Text = result.text; ++FileWrites; ++FileIdentity; }
		return {{true, {}, AccountSettingsWriteTarget{ .identity = FileIdentity }}, changed};
	}
};
RoutingPort &AccountRouting() { static auto port = RoutingPort(); return port; }
struct PortConfig {
	WriteResult write(const Fn<SpliceResult(const QString&)> &op, const QString &what,
		const std::optional<AccountSettingsWriteTarget> &expected,
		const std::function<bool()> &originCurrent,
		const std::function<bool()> &committedCurrent);
	std::optional<AccountSettingsWriteTarget> checkedInput(QString*) { return std::nullopt; }
	void observeAccountFiles() {}
	void refreshAccountRows() {
		const auto parsed = ParseSettings(Text, QString()); CHECK(parsed.ok()); Loaded = parsed.settings;
	}
};
#ifndef Expects
#define Expects(value) CHECK(value)
#define PURPLE_TEST_EXPECTS
#endif
#define LOG(...) (void)0
#include "peek-config-write.inc"
#undef LOG
#ifdef PURPLE_TEST_EXPECTS
#undef Expects
#undef PURPLE_TEST_EXPECTS
#endif

template <typename F>
WriteResult WriteSettingsGuarded(const AccountSettingsWriteTarget &target, F splice, const QString &what,
		const std::function<bool()> &current, const std::function<bool()> &committed) {
	return PortConfig().write(splice, what, target, current, [=] { ++CompletionChecks; return committed(); });
}
WaitResult RequestWait(WaitRequest request) {
	CHECK(request.action == WaitAction::ChangePeekOption);
	const auto openingAccount = AccountIdentity;
	const auto target = request.editor ? request.editor->target : std::nullopt;
	const auto origin = [=] {
		return openingAccount == AccountIdentity
			&& (!request.writesSettings || (request.editor && request.editor->originCurrent
				&& request.editor->originCurrent() && target && target->identity == FileIdentity));
	};
	Last = Gate.request({
		.action = request.action,
		.delaySeconds = std::max(PeekWaitDelaySeconds(Loaded), request.seconds),
		.same = request.same,
		.onlyTightening = request.onlyTightening,
		.validate = [=] { return !origin() ? tr::lng_purple_wait_context_changed(0) : request.validate(); },
		.originCurrent = [=] { return openingAccount == AccountIdentity; },
		.apply = request.apply,
	}, 0);
	return Last;
}

void RequestPeekOptionChange(PeekOptionChange, const std::shared_ptr<AppWideSettingsEditorState>&, not_null<Main::Session*>, std::function<bool()> = {});

#include "peek-focus-preference.inc"

auto Reset(int delay = 10, bool exit = true, bool focus = false) {
	Gate.cancel(); BeforeCommit = AfterReload = nullptr; CompletionChecks = 0; FileIdentity = AccountIdentity = 1; FileWrites = UsedTarget = 0; FileFails = false;
	Core::App() = Core::Application(); Core::App().local.present = true; Core::App().local.value = focus;
	FocusLossChanges.delivered = 0;
	Text = QString::fromLatin1("version=1\n[peek]\nstart_delay_seconds=%1\nexit_hidden_chats_p=%2\n")
		.arg(delay).arg(exit ? QString::fromLatin1("true") : QString::fromLatin1("false"));
	Loaded = ParseSettings(Text, QString()).settings;
	auto editor = std::make_shared<AppWideSettingsEditorState>();
	editor->target = AccountSettingsWriteTarget{.identity = FileIdentity};
	editor->originCurrent = [] { return true; };
	return editor;
}

void Cases() {
	auto session = Main::Session();
	{
		auto editor = Reset();
		RequestPeekDelayChange(20, editor, &session);
		std::printf("POSTWRITE: outcome=%d writes=%d delay=%d editorTarget=%d liveTarget=%d\n",
			int(Last.outcome), FileWrites, PeekWaitDelaySeconds(Loaded), editor->target->identity, FileIdentity);
		CHECK(Last.outcome == WaitOutcome::Applied && FileWrites == 1);
		CHECK(CompletionChecks == 1 && editor->target->identity == FileIdentity);
		RequestPeekOptionChange({.exitHiddenChats = false, .endOnFocusLoss = true}, editor, &session, {});
		CHECK(Last.outcome == WaitOutcome::Pending);
		CHECK(Gate.tick(20000).outcome == WaitOutcome::Applied);
		CHECK(CompletionChecks == 2 && editor->target->identity == FileIdentity);
		CHECK(!PeekExitHiddenChats(Loaded) && PeekEndsOnFocusLoss());
		RequestPeekDelayChange(10, editor, &session);
		CHECK(Last.outcome == WaitOutcome::Pending);
		CHECK(Gate.tick(20000).outcome == WaitOutcome::Applied);
		CHECK(CompletionChecks == 3 && editor->target->identity == FileIdentity);
	}
	{
		auto editor = Reset(); auto reenter = false;
		editor->originCurrent = [&] { if (reenter) Gate.cancel(); return true; };
		AfterReload = [&] { reenter = true; };
		RequestPeekDelayChange(20, editor, &session);
		CHECK(Last.outcome == WaitOutcome::Refused && FileWrites == 1);
		CHECK(editor->target->identity == 1);
	}
	for (const auto changed : {0, 1, 2}) {
		auto editor = Reset();
		BeforeCommit = [&] {
			if (changed == 0) Loaded.peek.startDelaySeconds = 20;
			else if (changed == 1) Loaded.peek.exitHiddenChats = false;
			else Core::App().local.value = true;
		};
		RequestPeekOptionChange({.startDelaySeconds = 20, .exitHiddenChats = false, .endOnFocusLoss = true}, editor, &session, {});
		CHECK(Last.outcome == WaitOutcome::Pending);
		CHECK(Gate.tick(20000).outcome == WaitOutcome::Refused);
		CHECK(FileWrites == 0 && Core::App().local.writes == 0 && CompletionChecks == 0);
	}
	for (const auto stale : {0, 1, 2, 3, 4, 5}) {
		auto editor = Reset(); auto open = true; editor->originCurrent = [&] { return open; };
		AfterReload = [&] {
			if (stale == 0) ++AccountIdentity;
			else if (stale == 1) open = false;
			else if (stale == 2) Loaded.peek.startDelaySeconds = 30;
			else if (stale == 3) Core::App().local.value = true;
			else if (stale == 4) ++FileIdentity;
			else Gate.cancel();
		};
		RequestPeekOptionChange({.startDelaySeconds = 20, .endOnFocusLoss = true}, editor, &session, {});
		CHECK(Last.outcome == WaitOutcome::Refused && FileWrites == 1);
		CHECK(Core::App().local.writes == 0);
		CHECK(editor->target->identity == 1);
	}

	for (const auto oldDelay : {0, 10, 20}) for (const auto nextDelay : {0, 10, 20}) {
		for (const auto oldExit : {false, true}) for (const auto nextExit : {false, true}) {
			for (const auto oldFocus : {false, true}) for (const auto nextFocus : {false, true}) {
				auto editor = Reset(oldDelay, oldExit, oldFocus);
				const auto original = Text;
				const auto same = oldDelay == nextDelay && oldExit == nextExit && oldFocus == nextFocus;
				const auto tightening = nextDelay >= oldDelay && (!oldExit || nextExit) && (!oldFocus || nextFocus);
				const auto delay = std::max(oldDelay, nextDelay);
				const auto request = [&] {
					RequestPeekOptionChange({nextDelay, nextExit, nextFocus}, editor, &session, {});
				};
				request();
				if (same) {
					CHECK(Last.outcome == WaitOutcome::NoOp);
				} else if (tightening || !delay) {
					CHECK(Last.outcome == WaitOutcome::Applied);
				} else {
					CHECK(Last.outcome == WaitOutcome::Pending);
					CHECK(Text == original && FileWrites == 0 && Core::App().local.writes == 0);
					Gate.cancel(); CHECK(Gate.tick(int64_t(delay) * 1000).outcome == WaitOutcome::NoOp);
					CHECK(Text == original && FileWrites == 0 && Core::App().local.writes == 0);
					request(); CHECK(Gate.tick(int64_t(delay) * 1000 - 1).outcome == WaitOutcome::Pending);
					CHECK(Gate.tick(int64_t(delay) * 1000).outcome == WaitOutcome::Applied);
				}
				CHECK(PeekWaitDelaySeconds(Loaded) == nextDelay);
				CHECK(PeekExitHiddenChats(Loaded) == nextExit);
				CHECK(PeekEndsOnFocusLoss() == nextFocus);
				CHECK(FileWrites == int(oldDelay != nextDelay || oldExit != nextExit));
				CHECK(Core::App().local.writes == int(oldFocus != nextFocus));
				CHECK(editor->target->identity == FileIdentity);
				CHECK(CompletionChecks == FileWrites);
			}
		}
	}
	{
		Reset(); Core::App().local.present = false;
		CHECK(!PeekEndsOnFocusLoss());
		RequestPeekFocusLossChange(true, &session, {});
		CHECK(Last.outcome == WaitOutcome::Applied && Core::App().local.writes == 1);
	}
	{
		auto editor = Reset(); auto open = true;
		editor->originCurrent = [&] { return open; };
		BeforeCommit = [&] { open = false; };
		RequestPeekDelayChange(20, editor, &session);
		CHECK(Last.outcome == WaitOutcome::Refused && FileWrites == 0);
		CHECK(PeekWaitDelaySeconds(Loaded) == 10);
	}
	{
		auto editor = Reset(); auto reenter = false;
		editor->originCurrent = [&] { if (reenter) Gate.cancel(); return true; };
		BeforeCommit = [&] { reenter = true; };
		RequestPeekDelayChange(20, editor, &session);
		CHECK(Last.outcome == WaitOutcome::Refused && FileWrites == 0);
		CHECK(PeekWaitDelaySeconds(Loaded) == 10);
	}
	for (const auto stale : {0, 1, 2, 3}) {
		auto editor = Reset(); auto open = true; editor->originCurrent = [&] { return open; };
		RequestPeekOptionChange({.exitHiddenChats = false}, editor, &session, {});
		CHECK(Last.outcome == WaitOutcome::Pending);
		if (stale == 0) open = false;
		else if (stale == 1) ++AccountIdentity;
		else if (stale == 2) ++FileIdentity;
		else Loaded.peek.exitHiddenChats = false;
		CHECK(Gate.tick(10000).outcome == WaitOutcome::Refused); CHECK(FileWrites == 0);
	}
	{
		auto editor = Reset();
		RequestPeekOptionChange({.exitHiddenChats = false}, editor, &session, {});
		editor->target->identity = 77;
		CHECK(Gate.tick(10000).outcome == WaitOutcome::Applied);
		CHECK(UsedTarget == 1 && FileWrites == 1);
	}
	{
		auto editor = Reset(); editor->target.reset();
		RequestPeekOptionChange({.exitHiddenChats = false}, editor, &session, {});
		CHECK(Last.outcome == WaitOutcome::Refused && FileWrites == 0);
		RequestPeekDelayChange(-1, editor, &session);
		CHECK(Last.outcome == WaitOutcome::Refused && FileWrites == 0);
	}
	{
		auto editor = Reset(); Text = QString::fromLatin1("version=1\n"); Loaded = ParseSettings(Text, QString()).settings;
		RequestPeekDelayChange(10, editor, &session);
		CHECK(Last.outcome == WaitOutcome::NoOp && FileWrites == 0);
		CHECK(Text == QString::fromLatin1("version=1\n"));
	}
	{
		auto editor = Reset(); FileFails = true;
		RequestPeekOptionChange({.exitHiddenChats = false, .endOnFocusLoss = true}, editor, &session, {});
		CHECK(Gate.tick(10000).outcome == WaitOutcome::Refused);
		CHECK(FileWrites == 0 && Core::App().local.writes == 0);
	}
	{
		Reset(); auto open = true;
		RequestPeekFocusLossChange(true, &session, [&] { return open; });
		CHECK(Last.outcome == WaitOutcome::Applied && PeekEndsOnFocusLoss());
		RequestPeekFocusLossChange(true, &session, [&] { return open; });
		CHECK(Last.outcome == WaitOutcome::NoOp && Core::App().local.writes == 1);
		RequestPeekFocusLossChange(false, &session, [&] { return open; });
		CHECK(Last.outcome == WaitOutcome::Pending); Gate.cancel();
		CHECK(Gate.tick(10000).outcome == WaitOutcome::NoOp && PeekEndsOnFocusLoss());
		RequestPeekFocusLossChange(false, &session, [&] { return open; }); open = false;
		CHECK(Gate.tick(10000).outcome == WaitOutcome::Refused && PeekEndsOnFocusLoss());
		open = true; RequestPeekFocusLossChange(false, &session, [&] { return open; });
		CHECK(Gate.tick(10000).outcome == WaitOutcome::Applied && !PeekEndsOnFocusLoss());
	}
}

}

void FocusCases() {
	auto focus = PeekFocusGate();
	auto absent = State();
	auto live = State();
	live.peekActive = true;
	CHECK(!focus.update(true, absent, false, false, 0));
	CHECK(!focus.remaining(0));
	CHECK(!focus.update(true, absent, false, true, 0));
	CHECK(!focus.update(true, live, true, false, 0));
	CHECK(!focus.update(true, live, false, false, 100));
	CHECK(focus.remaining(100) == 2000);
	CHECK(!focus.update(true, live, false, false, 2099));
	CHECK(focus.remaining(2099) == 1);
	CHECK(!focus.update(true, live, true, false, 2099));
	CHECK(!focus.remaining(2100));
	CHECK(!focus.update(true, live, false, false, 5000));
	CHECK(!focus.update(true, live, false, false, 6000));
	CHECK(focus.remaining(6000) == 1000);
	CHECK(focus.update(true, live, false, false, 7000));
	for (const auto now : {0, 500, 1999}) {
		CHECK(!focus.update(true, live, false, false, now));
		CHECK(focus.update(true, live, false, true, now));
		CHECK(!focus.remaining(now));
	}
	auto replacement = PeekFocusGate();
	auto peekA = ParseState(u"peek_active=true\npeek_deadline_unix=1000\n"_q, QString());
	CHECK(peekA.peekActive && peekA.peekDeadlineUnix == 1000);
	CHECK(!replacement.update(true, peekA, false, false, 0));
	CHECK(replacement.remaining(0) == 2000);
	auto peekB = ParseState(u"peek_active=true\npeek_deadline_unix=2000\n"_q, QString());
	CHECK(peekB.peekActive && peekB.peekDeadlineUnix == 2000);
	CHECK(!replacement.update(true, peekB, false, false, 1500));
	CHECK(replacement.remaining(1500) == 2000);
	CHECK(!replacement.update(true, peekB, false, false, 2000));
	CHECK(replacement.remaining(2000) == 1500);
	CHECK(!replacement.update(true, peekB, false, false, 3499));
	CHECK(replacement.update(true, peekB, false, false, 3500));
	CHECK(!replacement.remaining(3500));
	CHECK(!replacement.update(true, peekA, false, false, 4000));
	auto unrelated = peekA;
	unrelated.scheduleTarget = u"unrelated reload"_q;
	CHECK(!replacement.update(true, unrelated, false, false, 5500));
	CHECK(replacement.remaining(5500) == 500);
	CHECK(replacement.update(true, unrelated, false, false, 6000));
	CHECK(!replacement.update(true, peekA, false, false, 7000));
	CHECK(!replacement.update(true, absent, false, false, 8500));
	CHECK(!replacement.update(true, peekA, false, false, 9000));
	CHECK(replacement.remaining(9000) == 2000);
	const auto applicationState = [](Qt::ApplicationState state) {
		QWindowSystemInterface::handleApplicationStateChanged(state, true);
		QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
	};
	CHECK(!kPeekFocusLossDefault);
	CHECK(!focus.update(kPeekFocusLossDefault, live, false, false, 0));
	CHECK(!focus.update(kPeekFocusLossDefault, live, false, false, 10000));
	CHECK(!focus.update(false, live, false, true, 10000));
	CHECK(!focus.remaining(10000));
	auto enabled = false;
	auto state = State();
	state.peekActive = true;
	auto stops = 0;
	auto now = int64_t(10000);
	const auto signal = QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged,
		[&](Qt::ApplicationState applicationState) {
			if (focus.update(enabled, state, applicationState == Qt::ApplicationActive, false, now)) {
				StopPeek(state);
				++stops;
			}
		});
	applicationState(Qt::ApplicationActive);
	applicationState(Qt::ApplicationInactive);
	now += 10000;
	applicationState(Qt::ApplicationInactive);
	CHECK(state.peekActive && stops == 0);
	enabled = true;
	applicationState(Qt::ApplicationActive);
	CHECK(QGuiApplication::applicationState() == Qt::ApplicationActive);
	applicationState(Qt::ApplicationInactive);
	CHECK(QGuiApplication::applicationState() != Qt::ApplicationActive);
	now += 1999;
	applicationState(Qt::ApplicationActive);
	CHECK(state.peekActive && stops == 0);
	auto host = QWidget();
	auto viewer = QDialog(&host);
	host.show();
	viewer.show();
	viewer.activateWindow();
	QCoreApplication::processEvents();
	applicationState(Qt::ApplicationActive);
	auto deactivateWindow = QEvent(QEvent::WindowDeactivate);
	QApplication::sendEvent(&host, &deactivateWindow);
	now += 5000;
	CHECK(!focus.update(enabled, state, QGuiApplication::applicationState() == Qt::ApplicationActive, false, now));
	CHECK(state.peekActive && stops == 0);
	applicationState(Qt::ApplicationInactive);
	now += 2000;
	applicationState(Qt::ApplicationInactive);
	CHECK(!state.peekActive && stops == 1);
	applicationState(Qt::ApplicationActive);
	CHECK(!state.peekActive && stops == 1);
	QObject::disconnect(signal);

	auto gate = WaitGate();
	auto writes = 0;
	auto validations = 0;
	const auto pending = gate.request({
		.action = WaitAction::StartPeek,
		.delaySeconds = 10,
		.validate = [&] { ++validations; return QString(); },
		.apply = [&](const std::function<bool()> &) { ++writes; return QString(); },
	}, 0);
	CHECK(pending.outcome == WaitOutcome::Pending);
	const auto before = validations;
	const auto cancelled = gate.tick(250, false);
	CHECK(cancelled.outcome == WaitOutcome::NoOp);
	CHECK(cancelled.error.isEmpty());
	CHECK(validations == before && writes == 0);
	CHECK(!gate.pending());
	CHECK(gate.tick(10000).outcome == WaitOutcome::NoOp);
}

void FocusPreferenceWaitCases() {
	auto gate = WaitGate();
	auto enabled = false;
	auto editorOpen = true;
	auto writes = 0;
	const auto operation = [&](bool wanted) {
		const auto opening = enabled;
		return WaitOperation{
			.action = WaitAction::ChangePeekFocus,
			.delaySeconds = 10,
			.same = wanted == opening,
			.toBypass = !wanted,
			.validate = [&, opening] {
				return enabled == opening && editorOpen ? QString() : QString::fromLatin1("changed");
			},
			.originCurrent = [&] { return editorOpen; },
			.apply = [&, wanted](const std::function<bool()> &current) {
				CHECK(current());
				enabled = wanted;
				++writes;
				return QString();
			},
		};
	};
	CHECK(!WaitRequired(WaitAction::ChangePeekFocus, false, true, false));
	CHECK(WaitRequired(WaitAction::ChangePeekFocus, false, false, true));
	CHECK(gate.request(operation(true), 0).outcome == WaitOutcome::Applied);
	CHECK(enabled && writes == 1);
	CHECK(gate.request(operation(false), 0).outcome == WaitOutcome::Pending);
	CHECK(enabled && writes == 1);
	gate.cancel();
	CHECK(gate.tick(10000).outcome == WaitOutcome::NoOp);
	CHECK(enabled && writes == 1);
	CHECK(gate.request(operation(false), 0).outcome == WaitOutcome::Pending);
	CHECK(gate.tick(0, false).outcome == WaitOutcome::NoOp);
	CHECK(enabled && writes == 1);
	CHECK(gate.tick(10000).outcome == WaitOutcome::NoOp);
	CHECK(gate.request(operation(false), 0).outcome == WaitOutcome::Pending);
	CHECK(gate.tick(9999).outcome == WaitOutcome::Pending);
	CHECK(enabled && writes == 1);
	CHECK(gate.tick(10000).outcome == WaitOutcome::Applied);
	CHECK(!enabled && writes == 2);
	CHECK(gate.request(operation(true), 0).outcome == WaitOutcome::Applied);
	CHECK(gate.request(operation(false), 0).outcome == WaitOutcome::Pending);
	editorOpen = false;
	CHECK(gate.tick(10000).outcome == WaitOutcome::Refused);
	CHECK(enabled && writes == 3);
}

void GateCases() {
	for (const auto action : { WaitAction::ChangePreset, WaitAction::ChangeAccountMode }) {
		CHECK(!WaitRequired(action, true, false, true));
		CHECK(!WaitRequired(action, false, true, false));
		CHECK(WaitRequired(action, false, false, true));
		CHECK(WaitRequired(action, false, false, false));
	}
	for (const auto action : { WaitAction::StartPeek, WaitAction::ExtendPeek,
		WaitAction::ChangePeekOption, WaitAction::ChangePreset,
		WaitAction::ChangeAccountMode, WaitAction::ChangeGlobalAccount, WaitAction::ChangePeekFocus }) {
		CHECK(WaitOriginValid(action, { true, true, true }));
		CHECK(!WaitOriginValid(action, { false, true, true }));
		CHECK(!WaitOriginValid(action, { true, true, false }));
		CHECK(WaitOriginValid(action, { true, false, true }) == WorkModeWait(action));
	}
	CHECK(WaitRequired(WaitAction::ChangeGlobalAccount, false, false, true));
	CHECK(!WaitRequired(WaitAction::ChangeGlobalAccount, false, false, false));
	for (const auto action : { WaitAction::StartPeek, WaitAction::ExtendPeek, WaitAction::ChangePeekOption }) {
		CHECK(WaitRequired(action, false, true, false));
	}
	auto gate = WaitGate();
	auto writes = 0;
	auto valid = true;
	auto held = false;
	auto openingAccount = 10;
	auto displayedAccount = 10;
	auto liveAccount = 10;
	const auto operation = [&] {
		return WaitOperation{
			.action = WaitAction::ChangeAccountMode,
			.delaySeconds = 10,
			.validate = [&, account = openingAccount] {
				return !held && WaitOriginValid(WaitAction::ChangeAccountMode,
					{ valid && liveAccount == account, displayedAccount == account, true })
					? QString() : QString::fromLatin1("held");
			},
			.originCurrent = [&] { return valid; },
			.apply = [&](const std::function<bool()> &current) {
				CHECK(current());
				++writes;
				return QString();
			},
		};
	};
	CHECK(gate.request(operation(), 0).outcome == WaitOutcome::Pending);
	CHECK(gate.remaining(0) == 10);
	CHECK(gate.remaining(1) == 10);
	CHECK(gate.remaining(1000) == 9);
	CHECK(gate.tick(9999).outcome == WaitOutcome::Pending);
	CHECK(writes == 0);
	displayedAccount = 20;
	CHECK(displayedAccount != openingAccount);
	CHECK(gate.tick(10000).outcome == WaitOutcome::Applied);
	CHECK(writes == 1);
	CHECK(!gate.pending());
	CHECK(gate.tick(20000).outcome == WaitOutcome::NoOp);
	CHECK(writes == 1);
	CHECK(gate.request(operation(), 0).outcome == WaitOutcome::Pending);
	gate.cancel();
	CHECK(gate.tick(10000).outcome == WaitOutcome::NoOp);
	CHECK(writes == 1);
	held = true;
	CHECK(gate.request(operation(), 0).outcome == WaitOutcome::Refused);
	CHECK(!gate.pending());
	held = false;
	CHECK(gate.request(operation(), 0).outcome == WaitOutcome::Pending);
	liveAccount = 30;
	CHECK(gate.tick(10000).outcome == WaitOutcome::Refused);
	CHECK(writes == 1);
	liveAccount = openingAccount;
	CHECK(gate.request(operation(), 0).outcome == WaitOutcome::Pending);
	valid = false;
	CHECK(gate.tick(10000).outcome == WaitOutcome::Refused);
	CHECK(!gate.pending());
	valid = true;
	auto instant = operation();
	instant.fromBypass = true;
	CHECK(gate.request(instant, 0).outcome == WaitOutcome::Applied);
	CHECK(writes == 2);
	auto same = operation();
	same.same = true;
	CHECK(gate.request(same, 0).outcome == WaitOutcome::NoOp);
	CHECK(writes == 2);
	same.validate = [&] { return QString::fromLatin1("refused before no-op"); };
	CHECK(gate.request(same, 0).outcome == WaitOutcome::Refused);
	auto zero = operation();
	zero.delaySeconds = 0;
	CHECK(gate.request(zero, 0).outcome == WaitOutcome::Applied);
	CHECK(writes == 3);
	auto finalChecks = 0;
	auto reentrant = operation();
	reentrant.validate = [&] {
		if (++finalChecks == 3) { gate.cancel(); }
		return QString();
	};
	CHECK(gate.request(reentrant, 0).outcome == WaitOutcome::Pending);
	CHECK(gate.tick(10000).outcome == WaitOutcome::Refused);
	CHECK(writes == 3);
	auto replace = operation();
	auto validations = 0;
	replace.validate = [&] {
		if (++validations == 3) { gate.request(operation(), 20000); }
		return QString();
	};
	CHECK(gate.request(replace, 0).outcome == WaitOutcome::Pending);
	CHECK(gate.tick(10000).outcome == WaitOutcome::Refused);
	CHECK(gate.pending());
	CHECK(gate.tick(30000).outcome == WaitOutcome::Applied);
	CHECK(writes == 4);
	auto mutation = operation();
	mutation.delaySeconds = 0;
	mutation.apply = [&](const std::function<bool()> &current) {
		held = true;
		CHECK(current());
		return QString::fromLatin1("saved but recovery failed");
	};
	const auto result = gate.request(mutation, 0);
	CHECK(result.outcome == WaitOutcome::Refused);
	CHECK(result.error == QString::fromLatin1("saved but recovery failed"));
	CHECK(!gate.pending());
}

void PeekCases() {
	auto state = ParseState(QString(), QString());
	CHECK(StartPeek(state, 100, 300));
	const auto original = SerializeState(state);
	CHECK(SerializeState(ParseState(original, QString())) == original);
	auto gate = WaitGate();
	auto now = int64(100);
	const auto operation = [&](WaitAction action) {
		return WaitOperation{
			.action = action,
			.delaySeconds = 10,
			.validate = [&, original] {
				return SerializeState(state) == original && PeekLive(state, now)
					? QString() : QString::fromLatin1("peek changed");
			},
			.apply = [&, action](const std::function<bool()> &current) {
				CHECK(current());
				const auto applied = action == WaitAction::StartPeek
					? StartPeek(state, now, 300) : ExtendPeek(state, now, 300, kPeekExtendCapSeconds);
				CHECK(applied);
				return QString();
			},
		};
	};
	CHECK(gate.request(operation(WaitAction::StartPeek), 0).outcome == WaitOutcome::Pending);
	CHECK(SerializeState(state) == original);
	gate.cancel();
	CHECK(SerializeState(state) == original);
	CHECK(gate.request(operation(WaitAction::ExtendPeek), 0).outcome == WaitOutcome::Pending);
	CHECK(SerializeState(state) == original);
	gate.cancel();
	CHECK(SerializeState(state) == original);
	CHECK(gate.request(operation(WaitAction::ExtendPeek), 0).outcome == WaitOutcome::Pending);
	now = 110;
	CHECK(gate.tick(10000).outcome == WaitOutcome::Applied);
	CHECK(state.peekDeadlineUnix == 700);
	state = ParseState(original, QString());
	CHECK(gate.request(operation(WaitAction::StartPeek), 0).outcome == WaitOutcome::Pending);
	now = 120;
	CHECK(gate.tick(10000).outcome == WaitOutcome::Applied);
	CHECK(state.peekDeadlineUnix == 420);
	state = ParseState(original, QString());
	CHECK(gate.request(operation(WaitAction::ExtendPeek), 0).outcome == WaitOutcome::Pending);
	now = 401;
	CHECK(gate.tick(10000).outcome == WaitOutcome::Refused);
	CHECK(SerializeState(state) == original);
	CHECK(!ExtendPeek(state, now, 300, kPeekExtendCapSeconds));
	CHECK(StartPeek(state, 100, kPeekExtendCapSeconds));
	CHECK(!ExtendPeek(state, 100, 300, kPeekExtendCapSeconds));
	CHECK(StartPeek(state, 100, 0));
	CHECK(!ExtendPeek(state, 100, 300, kPeekExtendCapSeconds));
	CHECK(!WaitRequired(WaitAction::EndPeek, false, false, false));
	for (const auto oldDelay : { 0, 10, 20 }) {
		for (const auto newDelay : { 0, 10, 20 }) {
			auto writes = 0;
			auto delay = WaitOperation{
				.action = WaitAction::ChangePeekOption,
				.delaySeconds = std::max(oldDelay, newDelay),
				.validate = [] { return QString(); },
				.apply = [&](const std::function<bool()> &current) {
					CHECK(current());
					++writes;
					return QString();
				},
			};
			gate.request(delay, 0);
			if (std::max(oldDelay, newDelay)) {
				CHECK(writes == 0);
				CHECK(gate.tick(std::max(oldDelay, newDelay) * 1000 - 1).outcome == WaitOutcome::Pending);
				CHECK(gate.tick(std::max(oldDelay, newDelay) * 1000).outcome == WaitOutcome::Applied);
			}
			CHECK(writes == 1);
		}
	}
}

void DefaultDelayCases() {
	const auto absent = ParseSettings(QString::fromLatin1("version=1\n"), QString());
	CHECK(absent.ok());
	CHECK(!absent.settings.peek.startDelaySeconds);
	CHECK(PeekWaitDelaySeconds(absent.settings) == 10);
	CHECK(PeekWaitDelaySeconds(absent.settings) == kPeekStartDelayMobileDefaultSeconds);
	auto mobile = DeviceIdentity();
	mobile.cls = QString::fromLatin1("mobile");
	CHECK(PeekWaitDelaySeconds(absent.settings) == PeekStartDelaySeconds(absent.settings, mobile));
	for (const auto seconds : { 0, 1, 17 }) {
		auto explicitValue = absent.settings;
		explicitValue.peek.startDelaySeconds = seconds;
		CHECK(PeekWaitDelaySeconds(explicitValue) == seconds);
		CHECK(PeekWaitDelaySeconds(explicitValue) == PeekStartDelaySeconds(explicitValue, mobile));
	}
	for (const auto value : { "-1", "'bad'", "1.5" }) {
		const auto invalid = ParseSettings(QString::fromLatin1("version=1\n[peek]\nstart_delay_seconds=")
			+ QString::fromLatin1(value), QString());
		CHECK(invalid.ok());
		CHECK(PeekWaitDelaySeconds(invalid.settings) == 10);
	}
}

void SourceNoOpCases() {
	auto picker = QDialog();
	auto confirmation = QDialog(&picker);
	const auto weakPicker = QPointer<QDialog>(&picker);
	const auto weakConfirmation = QPointer<QDialog>(&confirmation);
	picker.show();
	confirmation.show();
	auto gate = WaitGate();
	auto writes = 0;
	const auto result = gate.request({
		.action = WaitAction::ChangeGlobalAccount,
		.delaySeconds = 10,
		.same = true,
		.validate = [] { return QString(); },
		.apply = [&](const std::function<bool()>&) {
			++writes;
			return QString();
		},
	}, 0);
	CHECK(result.outcome == WaitOutcome::NoOp);
	CHECK(!gate.pending());
	CHECK(writes == 0);
	CHECK(picker.isVisible() && confirmation.isVisible());
	const auto finished = [=] {
		if (weakConfirmation) { weakConfirmation->close(); }
		if (weakPicker) { weakPicker->close(); }
	};
	for (const auto outcome : { WaitOutcome::Pending, WaitOutcome::Applied, WaitOutcome::Refused }) {
		FinishNoOpWait({ outcome }, finished);
		CHECK(picker.isVisible() && confirmation.isVisible());
	}
	FinishNoOpWait(result, finished);
	CHECK(!picker.isVisible() && !confirmation.isVisible());
	CHECK(writes == 0);
}

void PlatformKeyCases() {
	auto host = QWidget();
	auto cover = QWidget(&host);
	auto child = QPushButton(&cover);
	auto editor = QLineEdit(&host);
	auto filter = WaitKeyFilter();
	auto gate = WaitGate();
	auto aborted = 0;
	auto clicked = 0;
	auto submitted = 0;
	auto shortcuts = 0;
	auto peekActions = 0;
	auto writes = 0;
	auto state = ParseState(QString::fromLatin1("active_preset='focus'\n"), QString());
	CHECK(StartPeek(state, 100, 60));
	const auto originalPeek = SerializeState(state);
	QObject::connect(&child, &QPushButton::clicked, [&] { ++clicked; });
	QObject::connect(&editor, &QLineEdit::returnPressed, [&] { ++submitted; });
	auto action = QAction(&host);
	action.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
	action.setShortcutContext(Qt::ApplicationShortcut);
	host.addAction(&action);
	QObject::connect(&action, &QAction::triggered, [&] { ++shortcuts; });
	auto peek = QAction(&host);
	peek.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
	peek.setShortcutContext(Qt::ApplicationShortcut);
	host.addAction(&peek);
	QObject::connect(&peek, &QAction::triggered, [&] {
		++peekActions;
		StopPeek(state);
	});
	host.show();
	cover.show();
	child.show();
	host.activateWindow();
	child.setFocus();
	QCoreApplication::processEvents();
	CHECK(host.windowHandle());
	const auto arm = [&] {
		CHECK(gate.request({
			.action = WaitAction::StartPeek,
			.delaySeconds = 10,
			.validate = [] { return QString(); },
			.apply = [&](const std::function<bool()>&) {
				++writes;
				CHECK(StartPeek(state, 110, 120));
				return QString();
			},
		}, 0).outcome == WaitOutcome::Pending);
		cover.show();
		child.setFocus();
		filter.arm(&host, &cover, [&] {
			++aborted;
			gate.cancel();
			filter.disarm();
			cover.hide();
			editor.setFocus();
		});
	};
	arm();
	auto override = QKeyEvent(QEvent::ShortcutOverride, Qt::Key_K, Qt::ControlModifier);
	QApplication::sendEvent(host.windowHandle(), &override);
	CHECK(override.isAccepted());
	CHECK(aborted == 0 && gate.pending());
	filter.disarm();
	gate.cancel();
	for (const auto key : { Qt::Key_X, Qt::Key_Return, Qt::Key_K, Qt::Key_E }) {
		editor.clear();
		arm();
		const auto before = aborted;
		const auto modifiers = key == Qt::Key_K || key == Qt::Key_E
			? Qt::ControlModifier : Qt::NoModifier;
		qt_handleKeyEvent(host.windowHandle(), QEvent::KeyPress, key, modifiers,
			key == Qt::Key_X ? QString::fromLatin1("x") : QString(), false, 1);
		CHECK(aborted == before + 1);
		CHECK(cover.isHidden() && !gate.pending());
		CHECK(editor.text().isEmpty());
		CHECK(clicked == 0 && submitted == 0);
		CHECK(shortcuts == 0 && peekActions == 0);
		CHECK(SerializeState(state) == originalPeek);
		CHECK(writes == 0);
		CHECK(gate.tick(10000).outcome == WaitOutcome::NoOp);
		qt_handleKeyEvent(host.windowHandle(), QEvent::KeyPress, Qt::Key_Z,
			Qt::NoModifier, QString::fromLatin1("z"), false, 1);
		CHECK(editor.text() == QString::fromLatin1("z"));
		CHECK(aborted == before + 1);
	}
	const auto expectCancelled = [&] {
		CHECK(!gate.pending());
		CHECK(gate.tick(10000).outcome == WaitOutcome::NoOp);
		CHECK(writes == 0);
		CHECK(SerializeState(state) == originalPeek);
	};
	arm();
	QWindowSystemInterface::handleApplicationStateChanged(Qt::ApplicationInactive, true);
	QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
	expectCancelled();
	QWindowSystemInterface::handleApplicationStateChanged(Qt::ApplicationActive, true);
	QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
	arm();
	auto hostDeactivated = QEvent(QEvent::WindowDeactivate);
	QApplication::sendEvent(host.windowHandle(), &hostDeactivated);
	expectCancelled();
	auto ownDialog = QDialog(&host);
	ownDialog.show();
	QCoreApplication::processEvents();
	arm();
	auto ownDialogActivated = QEvent(QEvent::WindowActivate);
	QApplication::sendEvent(&ownDialog, &ownDialogActivated);
	expectCancelled();
	ownDialog.hide();
	for (const auto kind : {LockKind::App, LockKind::Screen}) {
		arm();
		gate.cancel();
		filter.disarm();
		cover.hide();
		expectCancelled();
		CHECK(kind == LockKind::App || kind == LockKind::Screen);
	}
	qt_handleKeyEvent(host.windowHandle(), QEvent::KeyPress, Qt::Key_K,
		Qt::ControlModifier, QString(), false, 1);
	CHECK(shortcuts == 1);
	qt_handleKeyEvent(host.windowHandle(), QEvent::KeyPress, Qt::Key_E,
		Qt::ControlModifier, QString(), false, 1);
	CHECK(peekActions == 1 && !state.peekActive);
}

void KeyCases() {
	auto host = QWidget();
	auto cover = QWidget(&host);
	auto child = QPushButton(&cover);
	auto editor = QLineEdit(&host);
	auto filter = WaitKeyFilter();
	auto aborted = 0;
	auto clicked = 0;
	auto shortcuts = 0;
	QObject::connect(&child, &QPushButton::clicked, [&] { ++clicked; });
	auto action = QAction(&host);
	action.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
	action.setShortcutContext(Qt::ApplicationShortcut);
	host.addAction(&action);
	QObject::connect(&action, &QAction::triggered, [&] { ++shortcuts; });
	host.show();
	cover.show();
	child.show();
	QCoreApplication::processEvents();
	const auto arm = [&] {
		cover.show();
		child.setFocus();
		filter.arm(&host, &cover, [&] {
			++aborted;
			filter.disarm();
			cover.hide();
		});
	};
	for (const auto key : { Qt::Key_A, Qt::Key_Shift, Qt::Key_Return, Qt::Key_Tab, Qt::Key_Escape, Qt::Key_K }) {
		arm();
		const auto before = aborted;
		auto shortcut = QKeyEvent(QEvent::ShortcutOverride, key, Qt::ControlModifier);
		QApplication::sendEvent(&child, &shortcut);
		CHECK(shortcut.isAccepted());
		CHECK(aborted == before);
		auto press = QKeyEvent(QEvent::KeyPress, key, Qt::ControlModifier);
		QApplication::sendEvent(&child, &press);
		CHECK(aborted == before + 1);
		CHECK(cover.isHidden());
		CHECK(clicked == 0);
		CHECK(shortcuts == 0);
		editor.setFocus();
		auto after = QKeyEvent(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, "x");
		QApplication::sendEvent(&editor, &after);
		CHECK(editor.text().endsWith('x'));
		CHECK(aborted == before + 1);
		editor.clear();
	}
	arm();
	auto release = QKeyEvent(QEvent::KeyRelease, Qt::Key_K, Qt::ControlModifier);
	auto before = aborted;
	QApplication::sendEvent(&child, &release);
	CHECK(aborted == before);
	auto repeat = QKeyEvent(QEvent::KeyPress, Qt::Key_K, Qt::ControlModifier, {}, true);
	QApplication::sendEvent(&child, &repeat);
	CHECK(aborted == before + 1);
	for (const auto type : { QEvent::WindowDeactivate, QEvent::Close, QEvent::Hide }) {
		arm();
		before = aborted;
		auto event = QEvent(type);
		auto close = QCloseEvent();
		QApplication::sendEvent(&host, type == QEvent::Close ? &close : &event);
		CHECK(aborted == before + 1);
	}
	arm();
	before = aborted;
	auto background = QEvent(QEvent::ApplicationDeactivate);
	QApplication::sendEvent(qApp, &background);
	CHECK(aborted == before + 1);
	arm();
	before = aborted;
	filter.disarm();
	const auto afterAbort = [&] {
		CHECK(aborted == before + 1);
	};
	arm();
	before = aborted;
	QWindowSystemInterface::handleApplicationStateChanged(Qt::ApplicationInactive, true);
	QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
	afterAbort();
	QWindowSystemInterface::handleApplicationStateChanged(Qt::ApplicationActive, true);
	QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
	arm();
	before = aborted;
	auto hostInactive = QEvent(QEvent::WindowDeactivate);
	QApplication::sendEvent(host.windowHandle(), &hostInactive);
	afterAbort();
	auto dialog = QDialog(&host);
	dialog.show();
	QCoreApplication::processEvents();
	arm();
	before = aborted;
	auto dialogActive = QEvent(QEvent::WindowActivate);
	QApplication::sendEvent(&dialog, &dialogActive);
	afterAbort();
	arm();
	before = aborted;
	filter.disarm();
	cover.hide();
	auto press = QKeyEvent(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, "x");
	QApplication::sendEvent(&editor, &press);
	CHECK(editor.text().endsWith('x'));
	CHECK(aborted == before);
}

} // namespace

int main(int argc, char *argv[]) {
	auto app = QApplication(argc, argv);
	FocusCases();
	FocusPreferenceWaitCases();
	FocusPreferenceProbe::Cases();
	GateCases();
	PeekCases();
	KeyCases();
	DefaultDelayCases();
	SourceNoOpCases();
	PlatformKeyCases();
	std::printf("wait: %d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
