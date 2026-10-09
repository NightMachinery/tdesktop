/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_last_seen_ui.h"

#include "api/api_user_privacy.h"
#include "apiwrap.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_peer.h"
#include "data/data_peer_values.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/fade_wrap.h"
#include "ui/basic_click_handlers.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include "styles/style_dialogs.h"
#include "styles/style_info.h"

namespace Purple {
namespace {

constexpr auto kLastSeenRefresh = crl::time(60000);

void OpenProfileLastSeen(
		not_null<Ui::FlatLabel*> label,
		const base::weak_ptr<Main::Session> &weak,
		UserId userId) {
	const auto session = weak.get();
	if (!session) {
		return;
	}
	const auto window = Core::App().findWindow(label);
	const auto controller = window ? window->sessionController() : nullptr;
	if (!controller || &controller->session() != session) {
		return;
	}
	if (const auto user = session->data().userLoaded(userId)) {
		ShowLastSeenPeekBox(controller, user);
	}
}

} // namespace

void OnLastSeenInputsChanged(Fn<void()> refresh, rpl::lifetime &lifetime) {
	rpl::merge(StateChanges(), SettingsChanges()) | rpl::on_next(
		std::move(refresh),
		lifetime);
}

QString TopBarOnlineText(
		not_null<UserData*> user,
		TimeId now,
		int available,
		LastSeenTail &tail) {
	auto note = LastSeenNoteFor(user, now, false, false);
	if (!note.tail.isEmpty()
		&& st::dialogsTextStyle.font->width(note.text) > available) {
		note = LastSeenNoteFor(user, now, false, true);
	}
	tail = {};
	if (!note.link.isEmpty()) {
		tail.shown = true;
		tail.from = st::dialogsTextStyle.font->width(note.base);
	}
	return note.text;
}

QRect LastSeenTailRect(
		const LastSeenTail &tail,
		float64 narrowRatio,
		int leftTaken,
		int rightTaken,
		int outerWidth,
		int fullWidth) {
	if (!tail.shown || narrowRatio == 1.) {
		return QRect();
	}
	const auto top = st::topBarHeight
		- st::topBarArrowPadding.bottom()
		- st::dialogsTextFont->height;
	const auto available = outerWidth
		- rightTaken
		- leftTaken
		- st::topBarNameRightPadding;
	if (fullWidth > available || fullWidth <= tail.from) {
		return QRect();
	}
	return style::rtlrect(
		leftTaken + tail.from,
		top,
		fullWidth - tail.from,
		st::topBarHeight - top,
		outerWidth);
}

bool HandleLastSeenTailClick(
		not_null<Window::SessionController*> controller,
		PeerData *peer,
		QPoint position,
		const QRect &rect) {
	const auto user = peer ? peer->asUser() : nullptr;
	if (!user || !rect.contains(position)) {
		return false;
	}
	ShowLastSeenPeekBox(controller, user);
	return true;
}

void AttachProfileStatus(
		not_null<Ui::FlatLabel*> label,
		not_null<PeerData*> peer,
		Fn<void()> refresh,
		rpl::lifetime &lifetime) {
	if (peer->asUser()) {
		OnLastSeenInputsChanged(crl::guard(label, std::move(refresh)), lifetime);
	}
}

void RefreshProfileStatus(not_null<PeerData*> peer, Fn<void()> refresh) {
	if (peer->asUser()) {
		refresh();
	}
}

ProfileStatusText ProfileStatusBody(
		not_null<UserData*> user,
		TimeId now,
		bool colorized) {
	const auto note = LastSeenNoteFor(user, now, true, false);
	auto body = (Data::OnlineTextActive(user, now) && colorized)
		? Ui::Text::Colorized(note.base)
		: tr::marked(note.base);
	if (!note.tail.isEmpty()) {
		body.append(u" \u00b7 "_q);
		body.append(note.tail);
	}
	if (note.tappable) {
		body = Ui::Text::Link(std::move(body), 3);
	}
	return { .text = std::move(body), .hasLink = note.tappable };
}

void ApplyLastSeenLink(
		not_null<Ui::FlatLabel*> label,
		UserData *user,
		bool hasLink) {
	if (!user) {
		return;
	}
	label->setAttribute(Qt::WA_TransparentForMouseEvents, !hasLink);
	if (hasLink) {
		const auto weak = base::make_weak(&user->session());
		const auto userId = peerToUser(user->id);
		label->setLink(3, std::make_shared<LambdaClickHandler>(crl::guard(
			label,
			[=] { OpenProfileLastSeen(label, weak, userId); })));
	}
}

bool SetupShowLastSeen(
		not_null<Window::SessionController*> controller,
		not_null<UserData*> user,
		not_null<Ui::FadeWrap<Ui::RoundButton>*> button) {
	button->entity()->setText(tr::lng_lastseen_peek_now());
	rpl::combine(
		user->session().changes().peerFlagsValue(
			user,
			Data::PeerUpdate::Flag::OnlineStatus),
		Data::AmPremiumValue(&user->session()),
		LocalPremiumValue(),
		rpl::single(rpl::empty) | rpl::then(SettingsChanges())
	) | rpl::on_next([=](auto, bool amPremium, bool localPremium, auto) {
		const auto premium = amPremium || localPremium;
		const auto wasShown = button->toggled();
		const auto hiddenByMe = user->lastseen().isHiddenByMe();
		const auto now = base::unixtime::now();
		const auto shown = !user->lastseen().isOnline(now)
			&& CanPeekLastSeen(user);
		button->toggle(shown, anim::type::instant);
		if (wasShown && premium && hiddenByMe) {
			user->updateFullForced();
		}
	}, button->lifetime());

	controller->session().api().userPrivacy().value(
		Api::UserPrivacy::Key::LastSeen
	) | rpl::filter([=](Api::UserPrivacy::Rule rule) {
		return (rule.option == Api::UserPrivacy::Option::Everyone);
	}) | rpl::on_next([=] {
		if (user->lastseen().isHiddenByMe()) {
			user->updateFullForced();
		}
	}, button->lifetime());

	button->setOpacity(0.);
	button->entity()->setFullRadius(true);
	button->entity()->setClickedCallback(crl::guard(controller, [=] {
		ShowLastSeenPeekBox(controller, user);
	}));
	return true;
}

QString RowOnlineText(not_null<UserData*> user, TimeId now) {
	return LastSeenNoteFor(user, now, false, true).text;
}

crl::time RowOnlineChangeTimeout(not_null<UserData*> user, TimeId now) {
	const auto note = LastSeenNoteFor(user, now, false, true);
	const auto timeout = Data::OnlineChangeTimeout(user, now);
	return note.tail.isEmpty() ? timeout : std::min(timeout, kLastSeenRefresh);
}

} // namespace Purple
