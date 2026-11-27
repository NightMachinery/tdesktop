#include "purple/purple_sleep_timer_ui.h"
#include "purple/purple_hint_role.h"

#include "base/timer.h"
#include "chat_helpers/compose/compose_show.h"
#include "lang/lang_keys.h"
#include "media/player/media_player_dropdown.h"
#include "purple/purple_sleep_timer.h"
#include "ui/boxes/choose_time.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/widgets/fields/time_part_input.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/dropdown_menu.h"
#include "ui/widgets/labels.h"

#include "styles/style_layers.h"
#include "styles/style_media_player.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QPointer>
#include <QtGui/QAction>

#include <algorithm>

namespace Purple {
namespace {

namespace Rules = SleepTimerRules;
constexpr auto kName = "purple-sleep-timer-button";

class MenuController final : public Media::Player::WithDropdownController {
public:
	MenuController(
		not_null<Ui::IconButton*> button,
		not_null<QWidget*> parent,
		std::shared_ptr<Ui::Show> show,
		Fn<void(bool)> menuOver);

private:
	void fillMenu(not_null<Ui::DropdownMenu*> menu) override;
	std::shared_ptr<Ui::Show> _show;

};

class Button final : public Ui::IconButton {
public:
	Button(
		QWidget *parent,
		not_null<QWidget*> menuParent,
		std::shared_ptr<Ui::Show> show,
		Fn<void()> relayout,
		Fn<void(bool)> menuOver,
		Fn<bool(QPoint)> otherDropdown);
	~Button();
	void visibility(bool secondary);
	[[nodiscard]] MenuController &dropdown();

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	std::unique_ptr<MenuController> _dropdown;
	SleepTimerSnapshot _snapshot;
	bool _secondary = true;
	rpl::lifetime _subscription;

};

QString Duration(crl::time value) {
	const auto minutes = (value + Rules::kMinute - 1) / Rules::kMinute;
	return minutes % 60 == 0
		? tr::lng_hours(tr::now, lt_count, minutes / 60)
		: tr::lng_minutes(tr::now, lt_count, minutes);
}

QString EndText(std::optional<Rules::Media> media) {
	return media && media->item.kind == Rules::Kind::Voice
		? tr::lng_purple_sleep_end_message(tr::now)
		: media && media->item.kind == Rules::Kind::Video
		? tr::lng_purple_sleep_end_video(tr::now)
		: tr::lng_purple_sleep_end_track(tr::now);
}

QString Status(const SleepTimerSnapshot &value) {
	if (value.state.mode == Rules::Mode::Off) return tr::lng_purple_sleep_access_off(tr::now);
	if (value.state.mode == Rules::Mode::Timed) {
		const auto seconds = (value.remaining + 999) / 1000;
		const auto time = QString::number(qlonglong(seconds / 60)) + u':'
			+ QString::number(qlonglong(seconds % 60)).rightJustified(2, u'0');
		return tr::lng_purple_sleep_status(tr::now, lt_time, time);
	}
	return value.state.bound && value.state.bound->kind == Rules::Kind::Voice
		? tr::lng_purple_sleep_after_message(tr::now)
		: value.state.bound && value.state.bound->kind == Rules::Kind::Video
		? tr::lng_purple_sleep_after_video(tr::now)
		: tr::lng_purple_sleep_after_track(tr::now);
}

void CustomBox(not_null<Ui::GenericBox*> box) {
	struct State { crl::time duration = 0; };
	const auto state = box->lifetime().make_state<State>();
	const auto current = CurrentSleepTimer();
	box->setTitle(tr::lng_purple_sleep_title());
	box->setWidth(st::boxWidth);
	auto chooser = Ui::ChooseTimeWidget(box,
		Rules::CustomPrefill(current.state, crl::now(), current.lastCustom) / 1000, true);
	for (const auto object : chooser.widget->children()) {
		if (const auto part = dynamic_cast<Ui::TimePart*>(object)) part->setWheelStep(1);
	}
	box->addRow(std::move(chooser.widget));
	box->addRow(object_ptr<Ui::FlatLabel>(box, tr::lng_purple_sleep_bounds(), st::boxLabel));
	const auto start = box->addButton(tr::lng_purple_sleep_start(), [=] {
		if (Rules::ValidDuration(state->duration) && StartSleepTimer(state->duration, true)) {
			box->closeBox();
		}
	});
	std::move(chooser.secondsValue) | rpl::on_next([=](TimeId seconds) {
		state->duration = crl::time(seconds) * 1000;
		start->setDisabled(!Rules::ValidDuration(state->duration));
	}, box->lifetime());
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void ShowCustom(const std::shared_ptr<Ui::Show> &show) {
	if (show && show->valid()) show->showBox(Box(CustomBox));
}

MenuController::MenuController(
		not_null<Ui::IconButton*> button,
		not_null<QWidget*> parent,
		std::shared_ptr<Ui::Show> show,
		Fn<void(bool)> menuOver)
: WithDropdownController(button, parent, st::mediaPlayerMenu,
	style::al_topright, st::mediaPlayerMenuPosition, std::move(menuOver))
, _show(std::move(show)) {
	button->setClickedCallback([=] { showMenu(); });
}

void MenuController::fillMenu(not_null<Ui::DropdownMenu*> menu) {
	const auto value = CurrentSleepTimer();
	const auto active = value.state.mode != Rules::Mode::Off;
	SleepTimerValue() | rpl::on_next([=](const auto &now) {
		if (now.state.mode != value.state.mode) menu->hideFast();
	}, menu->lifetime());
	if (active) {
		const auto status = menu->addAction(Status(value), [] {});
		status->setEnabled(false);
		const auto guard = QPointer<QAction>(status.get());
		const auto refresh = menu->lifetime().make_state<base::Timer>([=] {
			if (guard) guard->setText(Status(CurrentSleepTimer()));
		});
		refresh->callEach(1000);
		SleepTimerValue() | rpl::on_next([=](const auto &now) {
			if (guard) guard->setText(Status(now));
		}, menu->lifetime());
	}
	if (value.state.mode == Rules::Mode::Timed) {
		menu->addAction(tr::lng_purple_sleep_add5(tr::now), [] { AddSleepTimerTime(5 * Rules::kMinute); });
		menu->addAction(tr::lng_purple_sleep_add15(tr::now), [] { AddSleepTimerTime(15 * Rules::kMinute); });
	} else {
		auto durations = std::vector<crl::time>{ 15 * Rules::kMinute, 30 * Rules::kMinute, 60 * Rules::kMinute };
		if (!active) durations.push_back(value.lastCustom);
		std::ranges::sort(durations);
		durations.erase(std::unique(durations.begin(), durations.end()), durations.end());
		for (const auto duration : durations) {
			menu->addAction(Duration(duration), [=] { StartSleepTimer(duration); });
		}
	}
	const auto end = SleepTimerEndItem();
	menu->addAction(EndText(end), [] { StartSleepTimerEnd(); })->setEnabled(end.has_value());
	const auto show = _show;
	menu->addAction(tr::lng_purple_sleep_custom(tr::now), [=] { ShowCustom(show); });
	if (active) menu->addAction(tr::lng_purple_sleep_cancel(tr::now), [] { CancelSleepTimer(); });
}

Button::Button(
		QWidget *parent,
		not_null<QWidget*> menuParent,
		std::shared_ptr<Ui::Show> show,
		Fn<void()> relayout,
		Fn<void(bool)> menuOver,
		Fn<bool(QPoint)> otherDropdown)
: IconButton(parent, st::mediaPlayerRepeatButton)
, _dropdown(std::make_unique<MenuController>(this, menuParent, std::move(show), std::move(menuOver))) {
	setObjectName(QString::fromLatin1(kName));
	_dropdown->setOtherDropdownCheck(std::move(otherDropdown));
	SleepTimerValue() | rpl::on_next([=](SleepTimerSnapshot value) {
		_snapshot = std::move(value);
		visibility(_secondary);
		setAccessibleName(_snapshot.state.mode == Rules::Mode::Off
			? tr::lng_purple_sleep_access_off(tr::now)
			: tr::lng_purple_sleep_access_on(tr::now, lt_time, Status(_snapshot)));
		setToolTip(accessibleName());
		update();
		relayout();
	}, _subscription);
}

Button::~Button() {
	_subscription.destroy();
}

void Button::visibility(bool secondary) {
	_secondary = secondary;
	setVisible(_secondary || _snapshot.state.mode != Rules::Mode::Off);
	if (isHidden() && _dropdown->menu()) _dropdown->menu()->hideFast();
}

MenuController &Button::dropdown() {
	return *_dropdown;
}

void Button::paintEvent(QPaintEvent*) {
	auto p = Painter(this);
	paintRipple(p, 0, 0);
	const auto active = _snapshot.state.mode != Rules::Mode::Off;
	const auto color = active ? st::mediaPlayerActiveFg->c
		: isOver() ? st::menuIconFgOver->c : st::menuIconFg->c;
	if (_snapshot.state.mode == Rules::Mode::Timed) {
		const auto remaining = _snapshot.remaining;
		const auto text = remaining <= Rules::kMinute
			? tr::lng_purple_sleep_badge_seconds(tr::now, lt_count, (remaining + 999) / 1000)
			: remaining >= 60 * Rules::kMinute
			? tr::lng_purple_sleep_badge_hours(tr::now, lt_count, (remaining + 60 * Rules::kMinute - 1) / (60 * Rules::kMinute))
			: tr::lng_purple_sleep_badge_minutes(tr::now, lt_count, (remaining + Rules::kMinute - 1) / Rules::kMinute);
		p.setFont(st::mediaPlayerSpeedButton.font);
		p.setPen(color);
		p.drawText(rect(), style::al_center, text);
	} else {
		st::menuIconNightMode.paint(
			p, st::mediaPlayerRepeatButton.iconPosition, width(), color);
	}
}

Button *Find(not_null<const QWidget*> bar) {
	return dynamic_cast<Button*>(bar->findChild<QWidget*>(QString::fromLatin1(kName)));
}

} // namespace

void DecorateSleepTimer(
		not_null<Ui::RpWidget*> bar,
		not_null<QWidget*> menuParent,
		std::shared_ptr<ChatHelpers::Show> show,
		Fn<void()> relayout,
		Fn<void(bool)> menuOver,
		Fn<bool(QPoint)> otherDropdown) {
	const auto button = Ui::CreateChild<Button>(bar, menuParent, std::move(show), std::move(relayout),
		std::move(menuOver), std::move(otherDropdown));
	SetWindowHintRole(button, HintRole::Playback);
}

not_null<Ui::RpWidget*> SleepTimerControl(not_null<QWidget*> bar) {
	const auto button = Find(bar);
	Expects(button != nullptr);
	return button;
}

void UpdateSleepTimerVisibility(not_null<QWidget*> bar, bool secondaryVisible) {
	if (const auto button = Find(bar)) button->visibility(secondaryVisible);
}

void PlaceSleepTimer(not_null<QWidget*> bar, int right, int top) {
	if (const auto button = Find(bar)) button->moveToRight(right, top);
}

int SleepTimerButtonWidth(not_null<const QWidget*> bar) {
	const auto button = Find(bar);
	return button && !button->isHidden() ? button->width() : 0;
}

bool OverSleepTimerDropdown(not_null<const QWidget*> bar, QPoint global) {
	const auto button = Find(bar);
	return button && !button->isHidden() && button->rect().contains(button->mapFromGlobal(global));
}

void HideSleepTimerDropdown(not_null<QWidget*> bar, not_null<Ui::RpWidget*> except) {
	const auto button = Find(bar);
	if (button && button != except.get()) {
		if (const auto menu = button->dropdown().menu()) menu->hideFast();
	}
}

void UpdateSleepTimerDropdown(not_null<QWidget*> bar) {
	if (const auto button = Find(bar)) button->dropdown().updateDropdownGeometry();
}

void SuspendSleepTimerDropdown(not_null<QWidget*> bar, bool hidden) {
	if (const auto button = Find(bar)) {
		if (hidden) button->dropdown().hideTemporarily();
		else button->dropdown().showBack();
	}
}

void AddSleepTimerViewerActions(
		const Ui::Menu::MenuCallback &add,
		std::shared_ptr<ChatHelpers::Show> show,
		const void *surface) {
	if (!SleepTimerVideoLoaded(surface)) return;
	const auto value = CurrentSleepTimer();
	add(value.state.mode == Rules::Mode::Off ? tr::lng_purple_sleep_open(tr::now)
		: value.state.mode == Rules::Mode::Timed
		? tr::lng_purple_sleep_view_left(tr::now, lt_time, Duration(value.remaining))
		: Status(value),
		[=] { ShowCustom(show); }, &st::menuIconNightMode);
	if (value.state.mode != Rules::Mode::Off) {
		add(tr::lng_purple_sleep_cancel(tr::now), [] { CancelSleepTimer(); }, nullptr);
	} else if (SleepTimerEndItem(Rules::kViewerOwner)) {
		add(tr::lng_purple_sleep_end_video(tr::now), [] { StartSleepTimerEnd(Rules::kViewerOwner); }, nullptr);
	}
}

} // namespace Purple
