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
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "purple/purple_config.h"
#include "purple/purple_device.h"
#include "purple/purple_gate.h"
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
#include <QtCore/QPointer>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>

#include "styles/style_layers.h"
#include "styles/style_purple_peek_controls.h"

namespace Purple {
namespace {

constexpr auto kCountdownTick = crl::time(250);

class PeekCover final : public Ui::RpWidget {
public:
	PeekCover(
		not_null<Ui::RpWidget*> parent,
		bool setting,
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

class PendingPeek final {
public:
	PendingPeek();

	void watch(not_null<Ui::RpWidget*> widget);
	[[nodiscard]] bool pending() const;
	[[nodiscard]] bool starting() const;
	[[nodiscard]] rpl::producer<> changes() const;
	PeekChange start(int seconds);
	void changeDelay(int seconds);
	void cancel();
	void closeHiddenChats();

private:
	bool begin(
		int delay,
		std::optional<int> setting,
		int duration,
		SettingsFileInput input);
	[[nodiscard]] bool valid() const;
	void tick();
	void applyDelay(
		int seconds,
		const SettingsFileInput &input,
		Fn<bool()> contextValid);

	std::vector<not_null<Ui::RpWidget*>> _widgets;
	base::weak_ptr<Window::Controller> _window;
	Main::Session *_session = nullptr;
	Main::Account *_account = nullptr;
	Main::Account *_domainAccount = nullptr;
	QString _preset;
	QString _resolvedPreset;
	std::optional<int> _setting;
	std::optional<int> _originalDelay;
	SettingsFileInput _input;
	uint64 _generation = 0;
	int _duration = 0;
	crl::time _until = 0;
	bool _pending = false;
	QPointer<PeekCover> _cover;
	base::Timer _timer;
	rpl::event_stream<> _changes;
	rpl::lifetime _pendingLifetime;

};

PeekCover::PeekCover(
		not_null<Ui::RpWidget*> parent,
		bool setting,
		Fn<void()> abort)
: RpWidget(parent)
, _abortCallback(std::move(abort))
, _title(
	this,
	setting ? tr::lng_purple_peek_delay_changing() : tr::lng_purple_peek_starting(),
	st::purplePeekHeading)
, _number(this, QString(), st::purplePeekCountdown)
, _abort(this, tr::lng_purple_peek_abort(), st::purplePeekAbort) {
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

void PeekCover::setRemaining(int seconds) {
	_number->setText(QString::number(seconds));
	layout();
	raise();
}

void PeekCover::paintEvent(QPaintEvent *e) {
	QPainter(this).fillRect(e->rect(), st::boxBg);
}

void PeekCover::resizeEvent(QResizeEvent *e) {
	RpWidget::resizeEvent(e);
	layout();
}

void PeekCover::layout() {
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

void PeekCover::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape) {
		_abortCallback();
	}
	e->accept();
}

void PeekCover::mousePressEvent(QMouseEvent *e) {
	e->accept();
}

PendingPeek::PendingPeek()
: _timer([=] { tick(); }) {
	QObject::connect(
		QCoreApplication::instance(),
		&QCoreApplication::aboutToQuit,
		QCoreApplication::instance(),
		[=] { cancel(); });
}

void PendingPeek::watch(not_null<Ui::RpWidget*> widget) {
	if (ranges::contains(_widgets, widget)) {
		return;
	}
	_widgets.push_back(widget);
	widget->lifetime().add([=] {
		cancel();
		_widgets.erase(ranges::remove(_widgets, widget), _widgets.end());
	});
}

bool PendingPeek::pending() const {
	return _pending;
}

bool PendingPeek::starting() const {
	return _pending && !_setting.has_value();
}

rpl::producer<> PendingPeek::changes() const {
	return _changes.events();
}

void PendingPeek::cancel() {
	++_generation;
	const auto wasPending = std::exchange(_pending, false);
	_timer.cancel();
	_pendingLifetime.destroy();
	if (_cover) {
		_cover->hide();
		_cover->deleteLater();
		_cover = nullptr;
	}
	_window = nullptr;
	_session = nullptr;
	_account = nullptr;
	_domainAccount = nullptr;
	_setting.reset();
	if (wasPending) {
		_changes.fire({});
	}
}

bool PendingPeek::valid() const {
	const auto window = _window.get();
	return _pending
		&& window
		&& _cover
		&& window->widget()->isActive()
		&& Core::App().activeWindow() == window
		&& &window->account() == _account
		&& &Core::App().activeAccount() == _domainAccount
		&& window->maybeSession() == _session
		&& !window->locked()
		&& !Core::App().passcodeLocked()
		&& !Core::App().screenIsLocked()
		&& CurrentState().activePreset == _preset
		&& ActiveResolved().preset == _resolvedPreset
		&& ActiveSettings().peek.startDelaySeconds == _originalDelay
		&& (_setting.has_value() || Filtering());
}

bool PendingPeek::begin(
		int delay,
		std::optional<int> setting,
		int duration,
		SettingsFileInput input) {
	const auto window = Core::App().activeWindow();
	if (!window || !window->maybeSession() || window->locked()
		|| Core::App().screenIsLocked() || Core::App().passcodeLocked()) {
		return false;
	}
	_input = std::move(input);
	_window = base::make_weak(window);
	_session = window->maybeSession();
	_account = &window->account();
	_domainAccount = &Core::App().activeAccount();
	_preset = CurrentState().activePreset;
	_resolvedPreset = ActiveResolved().preset;
	_setting = setting;
	_originalDelay = ActiveSettings().peek.startDelaySeconds;
	_duration = duration;
	_until = crl::now() + delay * crl::time(1000);
	_pending = true;
	_cover = new PeekCover(window->widget()->body(), setting.has_value(), [=] {
		cancel();
	});
	window->sessionControllerChanges() | rpl::on_next([=] {
		cancel();
	}, _pendingLifetime);
	Core::App().domain().activeChanges() | rpl::on_next([=] {
		cancel();
	}, _pendingLifetime);
	SettingsChanges() | rpl::on_next([=] {
		cancel();
	}, _pendingLifetime);
	StateChanges() | rpl::on_next([=] {
		if (!valid()) {
			cancel();
		}
	}, _pendingLifetime);
	if (!valid()) {
		cancel();
		return false;
	}
	_timer.callEach(kCountdownTick);
	_changes.fire({});
	tick();
	return true;
}

void PendingPeek::applyDelay(
		int seconds,
		const SettingsFileInput &input,
		Fn<bool()> contextValid) {
	const auto result = WriteSettingsGuarded(input, [=](const QString &text) {
		if (!contextValid()) {
			return SpliceResult{ .error = u"The Peek context changed. Try again."_q };
		}
		return SetPeekStartDelay(text, SettingsFilePath(), seconds);
	}, u"Peek start delay"_q);
	if (!result.ok) {
		if (const auto window = Core::App().activeWindow()) {
			window->showToast(result.error);
		}
	}
}

void PendingPeek::tick() {
	if (!valid()) {
		cancel();
		return;
	}
	const auto left = std::max(_until - crl::now(), crl::time(0));
	_cover->setRemaining(int((left + 999) / 1000));
	if (left) {
		return;
	}
	const auto generation = _generation;
	const auto input = _input;
	if (!ValidateSettingsInput(input) || generation != _generation || !valid()) {
		cancel();
		return;
	}
	const auto setting = _setting;
	const auto seconds = _duration;
	const auto origin = _window;
	const auto session = _session;
	const auto account = _account;
	const auto domain = _domainAccount;
	const auto preset = _preset;
	const auto resolved = _resolvedPreset;
	const auto completionGeneration = generation + 1;
	cancel();
	const auto contextValid = [=] {
		const auto window = origin.get();
		return _generation == completionGeneration && window
			&& window->widget()->isActive()
			&& Core::App().activeWindow() == window
			&& window->maybeSession() == session && &window->account() == account
			&& &Core::App().activeAccount() == domain
			&& !window->locked() && !Core::App().passcodeLocked()
			&& !Core::App().screenIsLocked()
			&& CurrentState().activePreset == preset
			&& ActiveResolved().preset == resolved;
	};
	if (!ValidateSettingsInput(input) || !contextValid()) {
		return;
	}
	if (setting) {
		applyDelay(*setting, input, contextValid);
	} else {
		details::ActivatePeekFor(seconds);
	}
}

PeekChange PendingPeek::start(int seconds) {
	if (pending()) {
		cancel();
		return {};
	}
	const auto input = SettingsInputForOperation();
	if (!input || !Filtering()) {
		return { .refused = true };
	}
	const auto delay = PeekStartDelaySeconds(ActiveSettings(), ThisDevice());
	if (!delay) {
		return ValidateSettingsInput(*input)
			? details::ActivatePeekFor(seconds)
			: PeekChange{ .refused = true };
	}
	const auto origin = base::make_weak(Core::App().activeWindow());
	if (Peeking()) {
		EndPeek();
	}
	if (!origin || Core::App().activeWindow() != origin.get()) {
		return { .refused = true };
	}
	return begin(delay, std::nullopt, seconds, *input)
		? PeekChange{ .pending = true, .seconds = seconds }
		: PeekChange{ .refused = true };
}

void PendingPeek::changeDelay(int seconds) {
	cancel();
	const auto input = SettingsInputForOperation();
	if (!input || seconds < 0
		|| ActiveSettings().peek.startDelaySeconds == seconds) {
		return;
	}
	const auto delay = std::max(
		PeekStartDelaySeconds(ActiveSettings(), ThisDevice()),
		seconds);
	begin(delay, seconds, 0, *input);
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

void PendingPeek::closeHiddenChats() {
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

PendingPeek &Pending() {
	static const auto result = new PendingPeek();
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

void RequestPeekDelayChange(int seconds) {
	Pending().changeDelay(seconds);
}

void CloseHiddenPeekChats() {
	Pending().closeHiddenChats();
}

PeekChange StartPeekFor(int seconds) {
	return Pending().start(seconds);
}

} // namespace Purple
