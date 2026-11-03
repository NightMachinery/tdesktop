/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_media_seek.h"

#include "purple/purple_config.h"
#include "purple/purple_seek_rules.h"

#include "core/shortcuts.h"
#include "lang/lang_keys.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_float.h"
#include "media/player/media_player_instance.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "styles/style_purple_media_playback.h"

#include <QtGui/QPainter>

namespace Purple {
namespace {

using Type = AudioMsgId::Type;
using SystemCommand = base::Platform::SystemMediaControls::Command;

constexpr auto kBackName = "purple-player-seek-back"_cs;
constexpr auto kForwardName = "purple-player-seek-forward"_cs;

rpl::lifetime Lifetime;

[[nodiscard]] rpl::producer<int> SeekStepValue() {
	return rpl::single(rpl::empty) | rpl::then(
		SettingsChanges()
	) | rpl::map([] {
		return SeekStepSeconds();
	}) | rpl::distinct_until_changed();
}

[[nodiscard]] QString SeekText(int direction, int step) {
	return QString(direction < 0 ? QChar(0x2212) : QChar('+'))
		+ QString::number(step);
}

[[nodiscard]] QString SeekName(int direction, int step) {
	const auto seconds = QString::number(step);
	return (direction < 0)
		? tr::lng_purple_seek_back(tr::now, lt_seconds, seconds)
		: tr::lng_purple_seek_forward(tr::now, lt_seconds, seconds);
}

not_null<Ui::AbstractButton*> AddSeekButton(
		not_null<QWidget*> parent,
		int direction,
		Fn<void(
			QPainter&,
			not_null<Ui::AbstractButton*>,
			const QString&)> paint,
		Fn<void()> clicked) {
	const auto button = Ui::CreateChild<Ui::AbstractButton>(parent.get());
	const auto step = button->lifetime().make_state<int>(SeekStepSeconds());
	button->setClickedCallback(std::move(clicked));
	button->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(button);
		paint(p, button, SeekText(direction, *step));
	}, button->lifetime());
	SeekStepValue() | rpl::on_next([=](int value) {
		*step = value;
		button->setAccessibleName(SeekName(direction, value));
		button->update();
	}, button->lifetime());
	return button;
}

void PaintPlayerSeek(
		QPainter &p,
		not_null<Ui::AbstractButton*> button,
		const QString &text) {
	const auto inner = button->rect().marginsRemoved(
		st::purpleSeekButtonPadding);
	if (button->isOver()) {
		const auto side = st::purpleSeekButtonOver;
		PainterHighQualityEnabler hq(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawEllipse(
			inner.x() + (inner.width() - side) / 2,
			inner.y() + (inner.height() - side) / 2,
			side,
			side);
	}
	p.setFont(st::purpleSeekButtonFont);
	p.setPen(button->isOver() ? st::menuIconFgOver : st::menuIconFg);
	p.drawText(inner, text, style::al_center);
}

void PaintFloatSeek(
		QPainter &p,
		not_null<Ui::AbstractButton*> button,
		const QString &text) {
	PainterHighQualityEnabler hq(p);
	p.setPen(Qt::NoPen);
	p.setBrush(button->isOver() ? st::msgDateImgBgOver : st::msgDateImgBg);
	p.drawEllipse(button->rect());
	p.setFont(st::purpleFloatSeekFont);
	p.setPen(st::msgDateImgFg);
	p.drawText(button->rect(), text, style::al_center);
}

[[nodiscard]] Ui::AbstractButton *FindPlayerSeek(
		not_null<const QWidget*> parent,
		const QString &name) {
	return parent->findChild<Ui::AbstractButton*>(name);
}

[[nodiscard]] bool SeekActive(int direction) {
	return SeekBy(Media::Player::instance()->getActiveType(), direction);
}

} // namespace

int SeekStepSeconds() {
	return SeekRules::StepFrom(ActiveSettings().playback.seekStepSeconds);
}

bool SeekBy(AudioMsgId::Type type, int direction) {
	const auto player = Media::Player::instance();
	const auto state = player->getState(type);
	if (!state.id
		|| state.id != player->current(type)
		|| Media::Player::IsStoppedOrStopping(state.state)
		|| (state.frequency <= 0)
		|| (state.length <= 0)) {
		return false;
	}
	const auto duration = (state.length * 1000LL) / state.frequency;
	const auto target = SeekRules::SeekTarget(
		(state.position * 1000LL) / state.frequency,
		duration,
		direction,
		SeekStepSeconds());
	if (target && duration > 0) {
		const auto progress = double(*target) / double(duration);
		if (Media::Player::IsPausedOrPausing(state.state)) {
			player->updateSeeking(type, progress);
		} else {
			player->finishSeeking(type, progress);
		}
	}
	return true;
}

void StartMediaSeek() {
	Lifetime.destroy();
	Shortcuts::Requests(
	) | rpl::on_next([](not_null<Shortcuts::Request*> request) {
		using Command = Shortcuts::Command;
		request->check(Command::MediaSeekBackward) && request->handle([] {
			return SeekActive(-1);
		});
		request->check(Command::MediaSeekForward) && request->handle([] {
			return SeekActive(1);
		});
	}, Lifetime);
}

void DecoratePlayerSeek(
		not_null<Ui::RpWidget*> controls,
		Fn<AudioMsgId::Type()> type) {
	const auto add = [&](int direction, const QString &name) {
		const auto seek = [=] {
			SeekBy(type(), direction);
		};
		const auto button = AddSeekButton(
			controls,
			direction,
			PaintPlayerSeek,
			seek);
		button->setObjectName(name);
		button->resize(st::purpleSeekButtonSize);
		button->show();
	};
	add(-1, kBackName.utf16());
	add(1, kForwardName.utf16());
}

int PlacePlayerSeek(not_null<Ui::RpWidget*> controls, int right) {
	auto result = 0;
	for (const auto &name : { kForwardName.utf16(), kBackName.utf16() }) {
		if (const auto button = FindPlayerSeek(controls.get(), name)) {
			button->moveToRight(right + result, 0);
			result += button->width();
		}
	}
	return result;
}

int PlayerSeekWidth(not_null<const QWidget*> player) {
	auto result = 0;
	for (const auto &name : { kBackName.utf16(), kForwardName.utf16() }) {
		if (const auto button = FindPlayerSeek(player, name)) {
			result += button->width();
		}
	}
	return result;
}

bool HandleSystemMediaCommand(SystemCommand command) {
	switch (command) {
	case SystemCommand::SkipBackward: return SeekActive(-1);
	case SystemCommand::SkipForward: return SeekActive(1);
	default: return false;
	}
}

auto AddFloatSeekButtons(not_null<Media::Player::Float*> floating)
-> std::array<not_null<Ui::AbstractButton*>, 2> {
	const auto add = [&](int direction) {
		const auto seek = [=] {
			SeekBy(Type::Voice, direction);
		};
		const auto button = AddSeekButton(
			floating,
			direction,
			PaintFloatSeek,
			seek);
		button->resize(st::purpleFloatButtonSize, st::purpleFloatButtonSize);
		button->hide();
		return button;
	};
	return { add(-1), add(1) };
}

} // namespace Purple
