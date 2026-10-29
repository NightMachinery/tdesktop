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
#include "purple/purple_schedule.h"
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

QString PeekRefusalText(PeekRequestRefusal reason) {
	switch (reason) {
	case PeekRequestRefusal::WorkModeOff: return tr::lng_purple_wait_peek_unavailable(tr::now);
	case PeekRequestRefusal::Ended: return tr::lng_purple_wait_peek_ended(tr::now);
	case PeekRequestRefusal::Untimed: return tr::lng_purple_wait_peek_untimed(tr::now);
	case PeekRequestRefusal::Limit: return tr::lng_purple_wait_peek_limit(tr::now);
	case PeekRequestRefusal::InvalidExtension: return tr::lng_purple_wait_peek_invalid_extension(tr::now);
	case PeekRequestRefusal::None: return {};
	}
	return {};
}

QString ReadyTitle(WaitAction action) {
	switch (action) {
	case WaitAction::StartPeek: return tr::lng_purple_peek_ready_start(tr::now);
	case WaitAction::ExtendPeek: return tr::lng_purple_peek_ready_extend(tr::now);
	case WaitAction::ChangePeekOption: return tr::lng_purple_peek_options_ready(tr::now);
	default: return tr::lng_purple_work_mode_ready(tr::now);
	}
}

QString ReadyActionText(WaitAction action, const QString &preset) {
	switch (action) {
	case WaitAction::StartPeek: return tr::lng_purple_wait_ready_start_peek(tr::now);
	case WaitAction::ExtendPeek: return tr::lng_purple_wait_ready_extend_peek(tr::now);
	case WaitAction::ChangePreset:
		return tr::lng_purple_wait_ready_switch(
			tr::now,
			lt_preset,
			PresetDisplayName(ActiveSettings(), preset));
	default: return tr::lng_purple_wait_ready_apply(tr::now);
	}
}

class WaitCover final : public Ui::RpWidget {
public:
	WaitCover(
		not_null<Ui::RpWidget*> parent,
		WaitAction action,
		const QString &actionText,
		Fn<void()> abort,
		Fn<void()> confirm,
		Fn<void(bool enter, bool autoRepeat)> key);

	void setRemaining(int seconds);
	// The ready step: the action and Abort, the action greyed and deaf to
	// the mouse while the input guard is on.
	void showReady(bool guarded);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;

private:
	void layout();

	Fn<void()> _abortCallback;
	Fn<void(bool, bool)> _keyCallback;
	object_ptr<Ui::FlatLabel> _title;
	object_ptr<Ui::FlatLabel> _number;
	object_ptr<Ui::FlatLabel> _readyTitle;
	object_ptr<Ui::FlatLabel> _hint;
	object_ptr<Ui::RoundButton> _abort;
	object_ptr<Ui::RoundButton> _confirm;
	object_ptr<Ui::RoundButton> _readyAbort;
	bool _ready = false;
	std::optional<bool> _guarded;

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
	void render(int64_t now);
	void confirm();
	void key(bool enter, bool autoRepeat);
	void presenceChanged();
	void focusChanged();
	void closeCover();
	void report(const WaitResult &result, base::weak_ptr<Window::Controller> window);

	std::vector<not_null<Ui::RpWidget*>> _widgets;
	base::weak_ptr<Window::Controller> _window;
	WaitGate _gate;
	WaitPresence _presence;
	bool _coverHidden = false;
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
		const QString &actionText,
		Fn<void()> abort,
		Fn<void()> confirm,
		Fn<void(bool enter, bool autoRepeat)> key)
: RpWidget(parent)
, _abortCallback(std::move(abort))
, _keyCallback(std::move(key))
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
, _readyTitle(this, ReadyTitle(action), st::purplePeekHeading)
, _hint(this, tr::lng_purple_wait_ready_hint(), st::purplePeekHint)
, _abort(this, tr::lng_purple_wait_abort(), st::purplePeekAbort)
, _confirm(this, rpl::single(actionText), st::purplePeekConfirm)
, _readyAbort(this, tr::lng_purple_wait_abort(), st::purplePeekReadyAbort) {
	setAttribute(Qt::WA_OpaquePaintEvent);
	setFocusPolicy(Qt::StrongFocus);
	_abort->setClickedCallback([=] { _abortCallback(); });
	// Abort stays live through the input guard: aborting is always safe, and
	// a late click meant for the countdown's Abort should still abort.
	_readyAbort->setClickedCallback([=] { _abortCallback(); });
	_confirm->setClickedCallback(std::move(confirm));
	if (_confirm->width() < _readyAbort->width()) {
		_confirm->setFullWidth(st::purplePeekAbort.width);
	}
	_readyTitle->hide();
	_hint->hide();
	_confirm->hide();
	_readyAbort->hide();
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

void WaitCover::showReady(bool guarded) {
	if (!_ready) {
		_ready = true;
		// The countdown's title and number stay measurable: the ready step
		// is laid out from where the countdown's Abort was.
		_title->hide();
		_number->hide();
		_readyTitle->show();
		_abort->hide();
		_hint->show();
		_confirm->show();
		_readyAbort->show();
	}
	if (_guarded != guarded) {
		_guarded = guarded;
		_confirm->setDisabled(guarded);
		_confirm->setTextFgOverride(guarded
			? std::make_optional(st::windowSubTextFg->c)
			: std::nullopt);
		_confirm->setAttribute(Qt::WA_TransparentForMouseEvents, guarded);
	}
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
	_readyTitle->resizeToWidth(available);
	_hint->resizeToWidth(available);
	const auto rects = LayoutWaitCover({
		.width = width(),
		.height = height(),
		.gap = st::purplePeekGap,
		.countdownTitle = _title->height(),
		.number = _number->height(),
		.abort = _abort->size(),
		.readyTitle = _readyTitle->height(),
		.hint = _hint->height(),
		.confirm = _confirm->size(),
		.readyAbort = _readyAbort->size(),
	});
	const auto place = [&](not_null<QWidget*> label, const QRect &rect) {
		label->move((width() - label->width()) / 2, rect.top());
	};
	place(_title.data(), rects.countdownTitle);
	place(_number.data(), rects.number);
	_abort->move(rects.abort.topLeft());
	place(_readyTitle.data(), rects.readyTitle);
	place(_hint.data(), rects.hint);
	_confirm->move(rects.confirm.topLeft());
	_readyAbort->move(rects.readyAbort.topLeft());
}

void WaitCover::keyPressEvent(QKeyEvent *e) {
	e->accept();
	_keyCallback(
		e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter,
		e->isAutoRepeat());
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

void PendingWait::presenceChanged() {
	if (!pending() || !_cover) {
		return;
	}
	const auto window = _window.get();
	const auto foreground = window
		&& QGuiApplication::applicationState() == Qt::ApplicationActive
		&& window->widget()->isActive()
		&& Core::App().activeWindow() == window;
	const auto locked = Core::App().passcodeLocked() || Core::App().screenIsLocked();
	if (_presence.update(foreground, locked, WaitClockNow())) {
		quietCancel();
		return;
	}
	// Hidden behind a lock, so the cover cannot sit over the passcode
	// widget; shown again, with a fresh input guard, when the lock ends.
	const auto hide = _presence.locked();
	if (hide == _coverHidden) {
		return;
	}
	// The key filter passes everything through while _coverHidden is set,
	// so it is set before hiding and cleared only once the cover shows.
	if (hide) {
		_coverHidden = true;
		_cover->hide();
	} else {
		_gate.guardInput(WaitClockNow());
		_cover->show();
		_coverHidden = false;
		_cover->raise();
		if (foreground) {
			_cover->setFocus();
		}
		render(WaitClockNow());
	}
}

void PendingWait::focusChanged() {
	const auto applicationActive = QGuiApplication::applicationState() == Qt::ApplicationActive;
	const auto locked = Core::App().passcodeLocked() || Core::App().screenIsLocked();
	presenceChanged();
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
	presenceChanged();
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
	const auto focus = std::exchange(_previousFocus, nullptr);
	_window = nullptr;
	_presence.reset();
	_coverHidden = false;
	CloseWaitCover(cover, focus);
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
	const auto peekAction = action == WaitAction::StartPeek || action == WaitAction::ExtendPeek;
	const auto readInput = peekAction ? SettingsReadInputForOperation() : std::nullopt;
	const auto needsInput = WaitNeedsSettingsEdit(action, request.writesSettings);
	const auto input = !needsInput
		? std::optional<AccountSettingsWriteTarget>()
		: action == WaitAction::ChangePeekOption
		? request.editor ? request.editor->target : std::nullopt
		: SettingsInputForOperation(&inputError);
	const auto external = request.validate;
	const auto editor = request.editor;
	// Foreground is checked when the request is made and when it is
	// confirmed. While waiting, presence decides: a lock keeps the wait.
	const auto originCurrent = [=](bool foreground) -> QString {
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
			.foregroundUnlocked = !foreground
				|| (QGuiApplication::applicationState() == Qt::ApplicationActive
					&& host->widget()->isActive() && Core::App().activeWindow() == host
					&& !host->locked() && !Core::App().passcodeLocked()
					&& !Core::App().screenIsLocked()),
		});
		return current ? QString() : tr::lng_purple_wait_context_changed(tr::now);
	};
	const auto validateAt = [=](bool foreground) -> QString {
		const auto live = weak.get();
		const auto originError = originCurrent(foreground);
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
		if (peekAction && (!readInput || !ValidateSettingsReadInput(*readInput))) {
			return tr::lng_purple_wait_settings_changed(tr::now);
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
	const auto validate = [=] { return validateAt(false); };
	const auto before = validateAt(true);
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
		const auto refusal = CheckPeekRequest(action, Filtering(), CurrentState(),
			QDateTime::currentSecsSinceEpoch(), seconds);
		if (refusal != PeekRequestRefusal::None) {
			const auto result = WaitResult{ WaitOutcome::Refused, PeekRefusalText(refusal) };
			report(result, origin);
			return result;
		}
	}
	const auto apply = [=](const std::function<bool()> &current) -> QString {
		closeCover();
		const auto error = validateAt(true);
		if (!error.isEmpty()) {
			return error;
		}
		if (!current()) {
			return tr::lng_purple_wait_context_changed(tr::now);
		}
		if (action == WaitAction::StartPeek) {
			if (details::ActivatePeekFor(seconds).refused) {
				return Filtering() ? tr::lng_purple_wait_context_changed(tr::now)
					: PeekRefusalText(PeekRequestRefusal::WorkModeOff);
			}
		} else if (action == WaitAction::ExtendPeek) {
			if (!details::ActivatePeekExtension(seconds).extended) {
				const auto refusal = CheckPeekRequest(action, Filtering(), CurrentState(),
					QDateTime::currentSecsSinceEpoch(), seconds);
				return refusal == PeekRequestRefusal::None
					? tr::lng_purple_wait_context_changed(tr::now) : PeekRefusalText(refusal);
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
		.readyTimeoutSeconds = PeekReadyTimeoutSeconds(ActiveSettings()),
		.same = request.same,
		.onlyTightening = request.onlyTightening,
		.fromBypass = request.fromBypass,
		.toBypass = request.toBypass,
		.validate = validate,
		.originCurrent = [=] { return originCurrent(true).isEmpty(); },
		.apply = apply,
	}, WaitClockNow());
	if (result.outcome == WaitOutcome::Pending) {
		_window = origin;
		_previousFocus = QApplication::focusWidget();
		_presence.reset();
		_coverHidden = false;
		_cover = new WaitCover(
			window->widget()->body(),
			action,
			ReadyActionText(action, request.preset),
			[=] { quietCancel(); },
			[=] { confirm(); },
			[=](bool enter, bool autoRepeat) { key(enter, autoRepeat); });
		_keys.arm(window->widget(), _cover, {
			.abort = [=] { quietCancel(); },
			.presence = [=] { presenceChanged(); },
			.suspended = [=] { return _coverHidden; },
			.classify = [=](bool enter, bool autoRepeat) {
				return ClassifyWaitKey(
					_gate.phase(),
					_gate.inputGuarded(WaitClockNow()),
					enter,
					autoRepeat);
			},
			.confirm = [=] { confirm(); },
		});
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

void PendingWait::render(int64_t now) {
	if (!pending() || !_cover) {
		closeCover();
	} else if (_coverHidden) {
		return;
	} else if (_gate.phase() == WaitPhase::Ready) {
		_cover->showReady(_gate.inputGuarded(now));
	} else {
		_cover->setRemaining(_gate.remaining(now));
	}
}

void PendingWait::key(bool enter, bool autoRepeat) {
	const auto verdict = ClassifyWaitKey(
		_gate.phase(),
		_gate.inputGuarded(WaitClockNow()),
		enter,
		autoRepeat);
	if (verdict == WaitKeyVerdict::Abort) {
		quietCancel();
	} else if (verdict == WaitKeyVerdict::Confirm) {
		confirm();
	}
}

void PendingWait::confirm() {
	if (_coverHidden) {
		return;
	}
	const auto origin = _window;
	const auto result = _gate.confirm(WaitClockNow());
	if (!pending()) {
		closeCover();
	}
	report(result, origin);
}

void PendingWait::tick() {
	const auto origin = _window;
	// The activation and lock signals come first; this is the backstop, and
	// what ends a settle window or shows the cover again after a lock.
	presenceChanged();
	const auto quietGeneration = _quietFocusGeneration;
	const auto now = WaitClockNow();
	const auto result = _gate.tick(now);
	render(now);
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
	const auto openingTimeout = PeekReadyTimeoutSeconds(ActiveSettings());
	const auto nextDelay = change.startDelaySeconds.value_or(openingDelay);
	const auto nextExit = change.exitHiddenChats.value_or(openingExit);
	const auto nextFocus = change.endOnFocusLoss.value_or(openingFocus);
	const auto nextTimeout = change.readyTimeoutSeconds.value_or(openingTimeout);
	const auto writesSettings = change.startDelaySeconds.has_value()
		|| change.exitHiddenChats.has_value()
		|| change.readyTimeoutSeconds.has_value();
	const auto validateValues = [=](bool committed) -> QString {
		const auto expectedDelay = committed ? nextDelay : openingDelay;
		const auto expectedExit = committed ? nextExit : openingExit;
		const auto expectedTimeout = committed ? nextTimeout : openingTimeout;
		if (writesSettings && (!target || !editor || !editor->originCurrent || !editor->originCurrent())) {
			return tr::lng_purple_wait_editor_closed(tr::now);
		}
		if (nextDelay < 0) {
			return tr::lng_purple_wait_delay_invalid(tr::now);
		}
		if (nextTimeout < kPeekReadyTimeoutMinimumSeconds) {
			return tr::lng_purple_wait_ready_timeout_invalid(tr::now);
		}
		if ((editorCurrent && !editorCurrent())
			|| (change.startDelaySeconds && PeekWaitDelaySeconds(ActiveSettings()) != expectedDelay)
			|| (change.exitHiddenChats && PeekExitHiddenChats(ActiveSettings()) != expectedExit)
			|| (change.readyTimeoutSeconds && PeekReadyTimeoutSeconds(ActiveSettings()) != expectedTimeout)
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
			&& nextExit == openingExit && nextFocus == openingFocus
			&& nextTimeout == openingTimeout,
		// A shorter ready timeout only tightens; a longer one loosens.
		.onlyTightening = nextDelay >= openingDelay
			&& (!openingExit || nextExit) && (!openingFocus || nextFocus)
			&& nextTimeout <= openingTimeout,
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
			if (nextDelay != openingDelay || nextExit != openingExit
				|| nextTimeout != openingTimeout) {
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
						if (result.ok() && nextTimeout != openingTimeout) {
							const auto changed = result.changed;
							result = SetPeekReadyTimeout(result.text,
								target->destination.file.path, nextTimeout);
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

void RequestPeekReadyTimeoutChange(
		int seconds,
		const std::shared_ptr<AppWideSettingsEditorState> &editor,
		not_null<Main::Session*> session) {
	RequestPeekOptionChange({ .readyTimeoutSeconds = seconds }, editor, session);
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
