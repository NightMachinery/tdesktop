/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_peek_controls.h"

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_device.h"
#include "purple/purple_gate.h"
#include "purple/purple_wait_key_filter.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QPointer>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>

#include "styles/style_layers.h"
#include "styles/style_purple_peek_controls.h"

namespace Purple {
namespace {

constexpr auto kCountdownTick = crl::time(250);
constexpr auto kPeekFocusLossPref = "purple-peek-end-on-focus-loss";
rpl::event_stream<bool> FocusLossChanges;

class WaitCover final : public Ui::RpWidget {
public:
	WaitCover(
		not_null<Ui::RpWidget*> parent,
		WaitAction action,
		Fn<void()> abort);

	void setRemaining(int seconds);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;

private:
	void layout();

	Fn<void()> _abortCallback;
	object_ptr<Ui::FlatLabel> _title;
	object_ptr<Ui::FlatLabel> _number;
	object_ptr<Ui::RoundButton> _abort;

};

class PendingWait final {
public:
	PendingWait();

	void watch(not_null<Ui::RpWidget*> widget);
	[[nodiscard]] bool pending() const;
	[[nodiscard]] bool starting() const;
	[[nodiscard]] rpl::producer<> changes() const;
	WaitResult request(WaitRequest request);
	void cancel();
	void quietCancel();
	void closeHiddenChats();
	void locked();

private:
	void tick();
	void focusChanged();
	void closeCover();
	void report(const WaitResult &result, base::weak_ptr<Window::Controller> window);

	std::vector<not_null<Ui::RpWidget*>> _widgets;
	base::weak_ptr<Window::Controller> _window;
	WaitGate _gate;
	PeekFocusGate _focusGate;
	uint64_t _quietFocusGeneration = 0;
	WaitKeyFilter _keys;
	QPointer<WaitCover> _cover;
	QPointer<QWidget> _previousFocus;
	base::Timer _timer;
	base::Timer _focusTimer;
	rpl::lifetime _focusLifetime;
	rpl::event_stream<> _changes;
	rpl::lifetime _pendingLifetime;

};

WaitCover::WaitCover(
		not_null<Ui::RpWidget*> parent,
		WaitAction action,
		Fn<void()> abort)
: RpWidget(parent)
, _abortCallback(std::move(abort))
, _title(
	this,
	action == WaitAction::ChangePeekOption
		? tr::lng_purple_peek_delay_changing()
		: action == WaitAction::StartPeek
		? tr::lng_purple_peek_starting()
		: action == WaitAction::ExtendPeek
		? tr::lng_purple_peek_extending()
		: tr::lng_purple_work_mode_changing(),
	st::purplePeekHeading)
, _number(this, QString(), st::purplePeekCountdown)
, _abort(this, tr::lng_purple_wait_abort(), st::purplePeekAbort) {
	setAttribute(Qt::WA_OpaquePaintEvent);
	setFocusPolicy(Qt::StrongFocus);
	_abort->setClickedCallback([=] { _abortCallback(); });
	parent->sizeValue() | rpl::on_next([=](QSize size) {
		setGeometry(QRect(QPoint(), size));
	}, lifetime());
	show();
	raise();
	setFocus();
}

void WaitCover::setRemaining(int seconds) {
	_number->setText(QString::number(seconds));
	layout();
	raise();
}

void WaitCover::paintEvent(QPaintEvent *e) {
	QPainter(this).fillRect(e->rect(), st::boxBg);
}

void WaitCover::resizeEvent(QResizeEvent *e) {
	RpWidget::resizeEvent(e);
	layout();
}

void WaitCover::layout() {
	const auto available = std::max(width() - 2 * st::purplePeekPadding, 0);
	_title->resizeToWidth(available);
	_number->resizeToWidth(available);
	const auto total = _title->height() + _number->height()
		+ _abort->height() + 2 * st::purplePeekGap;
	auto top = std::max((height() - total) / 2, 0);
	_title->move((width() - _title->width()) / 2, top);
	top += _title->height() + st::purplePeekGap;
	_number->move((width() - _number->width()) / 2, top);
	top += _number->height() + st::purplePeekGap;
	_abort->move((width() - _abort->width()) / 2, top);
}

void WaitCover::keyPressEvent(QKeyEvent *e) {
	e->accept();
	_abortCallback();
}

void WaitCover::mousePressEvent(QMouseEvent *e) {
	e->accept();
}

PendingWait::PendingWait()
: _timer([=] { tick(); })
, _focusTimer([=] { focusChanged(); }) {
	QObject::connect(
		QCoreApplication::instance(),
		&QCoreApplication::aboutToQuit,
		QCoreApplication::instance(),
		[=] {
			cancel();
			_focusTimer.cancel();
			_focusLifetime.destroy();
		});
	const auto focusConnection = QObject::connect(
		qGuiApp,
		&QGuiApplication::applicationStateChanged,
		qGuiApp,
		[=] { focusChanged(); });
	_focusLifetime.add([focusConnection] { QObject::disconnect(focusConnection); });
	StateChanges() | rpl::on_next([=] { focusChanged(); }, _focusLifetime);
	FocusLossChanges.events() | rpl::on_next([=] { focusChanged(); }, _focusLifetime);
	_focusTimer.callOnce(1);
}

void PendingWait::focusChanged() {
	const auto applicationActive = QGuiApplication::applicationState() == Qt::ApplicationActive;
	const auto locked = Core::App().passcodeLocked() || Core::App().screenIsLocked();
	if (!applicationActive || locked) {
		quietCancel();
	}
	const auto now = crl::now();
	_focusTimer.cancel();
	if (_focusGate.update(
			PeekEndsOnFocusLoss(),
			CurrentState(),
			applicationActive,
			locked,
			now)) {
		details::DeactivatePeek();
		return;
	}
	if (const auto remaining = _focusGate.remaining(now)) {
		_focusTimer.callOnce(std::max(crl::time(1), crl::time(*remaining)));
	}
}

void PendingWait::locked() {
	quietCancel();
	_focusTimer.cancel();
	if (_focusGate.update(
			PeekEndsOnFocusLoss(),
			CurrentState(),
			false,
			true,
			crl::now())) {
		details::DeactivatePeek();
	}
}

void PendingWait::watch(not_null<Ui::RpWidget*> widget) {
	if (ranges::contains(_widgets, widget)) {
		return;
	}
	_widgets.push_back(widget);
	widget->lifetime().add([=] {
		if (const auto window = _window.get()) {
			if (window->widget() == widget) {
				cancel();
			}
		}
		_widgets.erase(ranges::remove(_widgets, widget), _widgets.end());
	});
}

bool PendingWait::pending() const {
	return _gate.pending();
}

bool PendingWait::starting() const {
	return _gate.action() == WaitAction::StartPeek;
}

rpl::producer<> PendingWait::changes() const {
	return _changes.events();
}

void PendingWait::closeCover() {
	_timer.cancel();
	_keys.disarm();
	_pendingLifetime.destroy();
	const auto cover = std::exchange(_cover, nullptr);
	if (cover) {
		cover->hide();
		cover->deleteLater();
	}
	const auto focus = std::exchange(_previousFocus, nullptr);
	if (focus && focus->isVisible() && focus->window()->isActiveWindow()) {
		focus->setFocus();
	}
	_window = nullptr;
	if (cover) {
		_changes.fire({});
	}
}

void PendingWait::quietCancel() {
	++_quietFocusGeneration;
	cancel();
}

void PendingWait::cancel() {
	_gate.cancel();
	closeCover();
}

void PendingWait::report(
		const WaitResult &result,
		base::weak_ptr<Window::Controller> window) {
	if (!result.error.isEmpty()) {
		if (const auto live = window.get()) {
			live->showToast(result.error);
		}
	}
}

WaitResult PendingWait::request(WaitRequest request) {
	if (request.action == WaitAction::EndPeek) {
		cancel();
		if (!CurrentState().peekActive) {
			return {};
		}
		details::DeactivatePeek();
		return { WaitOutcome::Applied };
	}
	if (pending()) {
		cancel();
		return {};
	}
	const auto window = Core::App().activeWindow();
	const auto origin = base::make_weak(window);
	const auto session = request.session ? request.session : window ? window->maybeSession() : nullptr;
	if (!window || !session || window->maybeSession() != session) {
		const auto result = WaitResult{ WaitOutcome::Refused,
			tr::lng_purple_wait_account_changed(tr::now) };
		report(result, origin);
		return result;
	}
	const auto workMode = WorkModeWait(request.action);
	const auto weak = base::make_weak(session);
	details::RefreshWaitSettings();
	if (!weak || !origin) {
		return { WaitOutcome::Refused, tr::lng_purple_wait_account_changed(tr::now) };
	}
	const auto account = SessionAccountKey(session);
	const auto domain = &Core::App().activeAccount();
	const auto mode = AccountWorkModeForSession(session);
	const auto state = SerializeState(CurrentState());
	const auto ownInput = workMode ? AccountSettingsTarget(session) : std::nullopt;
	const auto action = request.action;
	const auto seconds = request.seconds;
	auto inputError = QString();
	const auto needsInput = action != WaitAction::ChangeAccountMode
		&& action != WaitAction::ChangeGlobalAccount
		&& action != WaitAction::ChangePeekFocus
		&& (action != WaitAction::ChangePeekOption || request.writesSettings);
	const auto input = !needsInput
		? std::optional<AccountSettingsWriteTarget>()
		: action == WaitAction::ChangePeekOption
		? request.editor ? request.editor->target : std::nullopt
		: SettingsInputForOperation(&inputError);
	const auto external = request.validate;
	const auto editor = request.editor;
	const auto originCurrent = [=]() -> QString {
		const auto live = weak.get();
		const auto host = origin.get();
		const auto accountValid = host && live && !live->account().loggingOut()
			&& live->account().maybeSession() == live && SessionAccountKey(live) == account;
		if (!accountValid) {
			return tr::lng_purple_wait_account_changed(tr::now);
		}
		const auto current = WaitOriginValid(action, {
			.accountValid = accountValid,
			.displayedAccountSame = host->maybeSession() == live && &Core::App().activeAccount() == domain,
			.foregroundUnlocked = QGuiApplication::applicationState() == Qt::ApplicationActive
				&& host->widget()->isActive() && Core::App().activeWindow() == host
				&& !host->locked() && !Core::App().passcodeLocked() && !Core::App().screenIsLocked(),
		});
		return current ? QString() : tr::lng_purple_wait_context_changed(tr::now);
	};
	const auto validate = [=]() -> QString {
		const auto live = weak.get();
		const auto originError = originCurrent();
		if (!originError.isEmpty()) {
			return originError;
		}
		if (SerializeState(CurrentState()) != state || AccountWorkModeForSession(live) != mode) {
			return tr::lng_purple_wait_context_changed(tr::now);
		}
		if (action == WaitAction::ChangePeekOption && needsInput
			&& (!editor || !editor->originCurrent || !editor->originCurrent())) {
			return tr::lng_purple_wait_editor_closed(tr::now);
		}
		if (needsInput && (!input || !ValidateSettingsInput(*input))) {
			return input ? tr::lng_purple_wait_settings_changed(tr::now)
				: editor && !editor->error.isEmpty() ? editor->error
				: !inputError.isEmpty() ? inputError
				: tr::lng_purple_wait_settings_changed(tr::now);
		}
		if (external) {
			const auto error = external();
			if (!error.isEmpty()) {
				return error;
			}
		}
		if (workMode && (!ownInput || !ValidateAccountSettingsTarget(live, *ownInput))) {
			return tr::lng_purple_wait_settings_changed(tr::now);
		}
		return {};
	};
	const auto before = validate();
	if (!before.isEmpty()) {
		const auto result = WaitResult{ WaitOutcome::Refused, before };
		report(result, origin);
		return result;
	}
	auto delay = PeekWaitDelaySeconds(ActiveSettings());
	if (action == WaitAction::ChangePeekOption) {
		if (seconds < 0) {
			const auto result = WaitResult{ WaitOutcome::Refused,
				tr::lng_purple_wait_delay_invalid(tr::now) };
			report(result, origin);
			return result;
		}
		delay = std::max(delay, seconds);
	} else if (action == WaitAction::ChangePreset) {
		request.same = CurrentState().activePreset == request.preset;
		request.fromBypass = ActiveResolved().normal;
		request.toBypass = request.preset == NormalPreset();
		if (!request.toBypass && !ActiveSettings().preset(request.preset)) {
			const auto result = WaitResult{ WaitOutcome::Refused,
				tr::lng_purple_wait_settings_changed(tr::now) };
			report(result, origin);
			return result;
		}
	} else if (action == WaitAction::StartPeek || action == WaitAction::ExtendPeek) {
		auto preview = CurrentState();
		if (!Filtering() || (action == WaitAction::ExtendPeek
			&& !ExtendPeek(
				preview,
				QDateTime::currentSecsSinceEpoch(),
				seconds,
				kPeekExtendCapSeconds))) {
			const auto result = WaitResult{ WaitOutcome::Refused,
				tr::lng_purple_wait_peek_unavailable(tr::now) };
			report(result, origin);
			return result;
		}
	}
	const auto apply = [=](const std::function<bool()> &current) -> QString {
		closeCover();
		const auto error = validate();
		if (!error.isEmpty()) {
			return error;
		}
		if (!current()) {
			return tr::lng_purple_wait_context_changed(tr::now);
		}
		if (action == WaitAction::StartPeek) {
			if (details::ActivatePeekFor(seconds).refused) {
				return tr::lng_purple_wait_peek_unavailable(tr::now);
			}
		} else if (action == WaitAction::ExtendPeek) {
			if (!details::ActivatePeekExtension(seconds).extended) {
				return tr::lng_purple_wait_peek_unavailable(tr::now);
			}
		} else if (action == WaitAction::ChangePreset) {
			UpdateState([&](State &updated) {
				updated.activePreset = request.preset;
				updated.activeSource = PresetSource::Manual;
			});
		} else if (request.apply) {
			const auto error = request.apply(current);
			if (!error.isEmpty()) {
				return error;
			}
		} else {
			return tr::lng_purple_wait_context_changed(tr::now);
		}
		if (request.finished) {
			request.finished();
		}
		return {};
	};
	const auto result = _gate.request({
		.action = action,
		.delaySeconds = delay,
		.same = request.same,
		.onlyTightening = request.onlyTightening,
		.fromBypass = request.fromBypass,
		.toBypass = request.toBypass,
		.validate = validate,
		.originCurrent = [=] { return originCurrent().isEmpty(); },
		.apply = apply,
	}, crl::now());
	if (result.outcome == WaitOutcome::Pending) {
		_window = origin;
		_previousFocus = QApplication::focusWidget();
		_cover = new WaitCover(window->widget()->body(), action, [=] { quietCancel(); });
		_keys.arm(window->widget(), _cover, [=] { quietCancel(); });
		if (!workMode) {
			window->sessionControllerChanges() | rpl::on_next([=] { cancel(); }, _pendingLifetime);
			Core::App().domain().activeChanges() | rpl::on_next([=] { cancel(); }, _pendingLifetime);
		}
		SettingsChanges() | rpl::on_next([=] { cancel(); }, _pendingLifetime);
		StateChanges() | rpl::on_next([=] { cancel(); }, _pendingLifetime);
		_timer.callEach(kCountdownTick);
		_changes.fire({});
		tick();
	}
	report(result, origin);
	return result;
}

void PendingWait::tick() {
	const auto origin = _window;
	const auto window = origin.get();
	const auto foregroundUnlocked = QGuiApplication::applicationState() == Qt::ApplicationActive
		&& window && window->widget()->isActive()
		&& Core::App().activeWindow() == window && !window->locked()
		&& !Core::App().passcodeLocked() && !Core::App().screenIsLocked();
	const auto quietGeneration = _quietFocusGeneration;
	const auto result = _gate.tick(crl::now(), foregroundUnlocked);
	if (pending() && _cover) {
		_cover->setRemaining(_gate.remaining(crl::now()));
	} else {
		closeCover();
	}
	if (quietGeneration == _quietFocusGeneration
		&& QGuiApplication::applicationState() == Qt::ApplicationActive
		&& !Core::App().passcodeLocked() && !Core::App().screenIsLocked()) {
		report(result, origin);
	}
}

bool ShouldExit(not_null<History*> history) {
	auto hidden = history->purpleHiddenByPreset(false);
	if (const auto decision = OverrideFor(history->peer)) {
		if (*decision == OverrideKind::Show) {
			return false;
		} else if (*decision == OverrideKind::Hide) {
			if (HideUntilScope() == HideScope::Everywhere) {
				return true;
			}
			hidden = true;
		}
	}
	return hidden && (HideEverywhere() || !history->purpleReachableElsewhere(false));
}

void PendingWait::closeHiddenChats() {
	if (!Filtering() || Peeking() || !PeekExitHiddenChats(ActiveSettings())) {
		return;
	}
	auto peers = std::vector<not_null<PeerData*>>();
	for (const auto widget : _widgets) {
		const auto window = Core::App().findWindow(widget);
		const auto controller = window ? window->sessionController() : nullptr;
		const auto history = controller
			? controller->activeChatCurrent().owningHistory()
			: nullptr;
		if (history && ShouldExit(history)
			&& !ranges::contains(peers, history->peer)) {
			peers.push_back(history->peer);
		}
	}
	for (const auto peer : peers) {
		Core::App().closeChatFromWindows(peer);
	}
}

PendingWait &Pending() {
	static const auto result = new PendingWait();
	return *result;
}

} // namespace

void RegisterPeekWindow(not_null<Ui::RpWidget*> widget) {
	Pending().watch(widget);
}

bool PeekStartPending() {
	return Pending().starting();
}

bool PeekCountdownPending() {
	return Pending().pending();
}

rpl::producer<> PeekCountdownChanges() {
	return Pending().changes();
}

void CancelPendingPeek() {
	Pending().cancel();
}

void EndPeekOnApplicationLock() {
	Pending().locked();
}

bool PeekEndsOnFocusLoss() {
	return Core::App().settings().readPref<bool>(kPeekFocusLossPref, kPeekFocusLossDefault);
}

rpl::producer<bool> PeekFocusLossValue() {
	return rpl::single(PeekEndsOnFocusLoss()) | rpl::then(FocusLossChanges.events());
}

void RequestPeekOptionChange(
		PeekOptionChange change,
		const std::shared_ptr<AppWideSettingsEditorState> &editor,
		not_null<Main::Session*> session,
		std::function<bool()> editorCurrent) {
	const auto target = editor ? editor->target : std::nullopt;
	const auto openingDelay = PeekWaitDelaySeconds(ActiveSettings());
	const auto openingExit = PeekExitHiddenChats(ActiveSettings());
	const auto openingFocus = PeekEndsOnFocusLoss();
	const auto nextDelay = change.startDelaySeconds.value_or(openingDelay);
	const auto nextExit = change.exitHiddenChats.value_or(openingExit);
	const auto nextFocus = change.endOnFocusLoss.value_or(openingFocus);
	const auto writesSettings = change.startDelaySeconds.has_value()
		|| change.exitHiddenChats.has_value();
	const auto validateValues = [=](bool committed) -> QString {
		const auto expectedDelay = committed ? nextDelay : openingDelay;
		const auto expectedExit = committed ? nextExit : openingExit;
		if (writesSettings && (!target || !editor || !editor->originCurrent || !editor->originCurrent())) {
			return tr::lng_purple_wait_editor_closed(tr::now);
		}
		if (nextDelay < 0) {
			return tr::lng_purple_wait_delay_invalid(tr::now);
		}
		if ((editorCurrent && !editorCurrent())
			|| (change.startDelaySeconds && PeekWaitDelaySeconds(ActiveSettings()) != expectedDelay)
			|| (change.exitHiddenChats && PeekExitHiddenChats(ActiveSettings()) != expectedExit)
			|| (change.endOnFocusLoss && PeekEndsOnFocusLoss() != openingFocus)) {
			return tr::lng_purple_wait_context_changed(tr::now);
		}
		return {};
	};
	const auto validate = [=] { return validateValues(false); };
	RequestWait({
		.action = WaitAction::ChangePeekOption,
		.session = session,
		.seconds = nextDelay,
		.editor = editor,
		.same = nextDelay == openingDelay
			&& nextExit == openingExit && nextFocus == openingFocus,
		.onlyTightening = nextDelay >= openingDelay
			&& (!openingExit || nextExit) && (!openingFocus || nextFocus),
		.writesSettings = writesSettings,
		.validate = validate,
		.apply = [=](const std::function<bool()> &current) -> QString {
			const auto valid = [=] { return current() && validate().isEmpty() && current(); };
			const auto committed = [=] {
				return current() && validateValues(true).isEmpty() && current();
			};
			if (!valid()) {
				return tr::lng_purple_wait_context_changed(tr::now);
			}
			if (nextDelay != openingDelay || nextExit != openingExit) {
				const auto result = WriteSettingsGuarded(
					*target,
					[=](const QString &text) {
						auto result = SpliceResult{ .text = text };
						if (nextDelay != openingDelay) {
							result = SetPeekStartDelay(
								result.text,
								target->destination.file.path,
								nextDelay);
						}
						if (result.ok() && nextExit != openingExit) {
							const auto changed = result.changed;
							result = SetTableBool(result.text, target->destination.file.path,
								u"peek"_q, u"exit_hidden_chats_p"_q, nextExit);
							result.changed = result.changed || changed;
						}
						return result;
					},
					u"Peek options"_q,
					valid,
					committed);
				if (!result.ok) {
					return result.error;
				}
				editor->target = result.target;
			}
			if (nextFocus != openingFocus) {
				Core::App().settings().writePref<bool>(kPeekFocusLossPref, nextFocus);
				Core::App().saveSettingsDelayed();
				FocusLossChanges.fire_copy(nextFocus);
			}
			return {};
		},
	});
}

void RequestPeekFocusLossChange(
		bool enabled,
		not_null<Main::Session*> session,
		std::function<bool()> editorCurrent) {
	RequestPeekOptionChange(
		{ .endOnFocusLoss = enabled },
		nullptr,
		session,
		std::move(editorCurrent));
}

WaitResult RequestWait(WaitRequest request) {
	return Pending().request(std::move(request));
}

void RequestPeekDelayChange(
		int seconds,
		const std::shared_ptr<AppWideSettingsEditorState> &editor,
		not_null<Main::Session*> session) {
	RequestPeekOptionChange({ .startDelaySeconds = seconds }, editor, session);
}

void CloseHiddenPeekChats() {
	Pending().closeHiddenChats();
}

PeekChange StartPeekFor(int seconds) {
	const auto result = RequestWait({ .seconds = seconds });
	return {
		.peeking = Peeking(),
		.pending = result.outcome == WaitOutcome::Pending,
		.refused = result.outcome == WaitOutcome::Refused,
		.seconds = seconds,
		.leftSeconds = PeekLeftSeconds(CurrentState(), QDateTime::currentSecsSinceEpoch()),
	};
}

} // namespace Purple
