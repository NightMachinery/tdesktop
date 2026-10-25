#include "purple/purple_wait_gate.h"
#include "purple/purple_engine.h"
#include "purple/purple_state.h"
#include "purple/purple_wait_key_filter.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtGui/QAction>
#include <QtGui/QCloseEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QWindow>
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

void GateCases() {
	for (const auto action : { WaitAction::ChangePreset, WaitAction::ChangeAccountMode }) {
		CHECK(!WaitRequired(action, true, false, true));
		CHECK(!WaitRequired(action, false, true, false));
		CHECK(WaitRequired(action, false, false, true));
		CHECK(WaitRequired(action, false, false, false));
	}
	for (const auto action : { WaitAction::StartPeek, WaitAction::ExtendPeek,
		WaitAction::ChangeDelay, WaitAction::ChangePreset,
		WaitAction::ChangeAccountMode, WaitAction::ChangeGlobalAccount }) {
		CHECK(WaitOriginValid(action, { true, true, true }));
		CHECK(!WaitOriginValid(action, { false, true, true }));
		CHECK(!WaitOriginValid(action, { true, true, false }));
		CHECK(WaitOriginValid(action, { true, false, true }) == WorkModeWait(action));
	}
	CHECK(WaitRequired(WaitAction::ChangeGlobalAccount, false, false, true));
	CHECK(!WaitRequired(WaitAction::ChangeGlobalAccount, false, false, false));
	for (const auto action : { WaitAction::StartPeek, WaitAction::ExtendPeek, WaitAction::ChangeDelay }) {
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
				.action = WaitAction::ChangeDelay,
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
	cover.hide();
	auto press = QKeyEvent(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, "x");
	QApplication::sendEvent(&editor, &press);
	CHECK(editor.text().endsWith('x'));
	CHECK(aborted == before);
}

} // namespace

int main(int argc, char *argv[]) {
	auto app = QApplication(argc, argv);
	GateCases();
	PeekCases();
	KeyCases();
	DefaultDelayCases();
	SourceNoOpCases();
	PlatformKeyCases();
	std::printf("wait: %d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
