/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_last_seen.h"

#include "api/api_user_privacy.h"
#include "apiwrap.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "data/data_changes.h"
#include "data/data_lastseen_status.h"
#include "data/data_peer_values.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_gate.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtCore/QLocale>

namespace Purple {
namespace {

// The words, and the mark that stands in for them where a sentence does not
// fit. Both come after the separator every other status suffix in the app uses,
// so the fork's addition reads as part of the same line rather than as a second
// one that ran into the first.
const auto kSeparator = QString::fromUtf8(" \xC2\xB7 ");
const auto kLongReason = u"share yours to see"_q;
const auto kLongRefresh = u"refresh"_q;
const auto kShortReason = QString::fromUtf8("\xF0\x9F\x91\x80");

[[nodiscard]] const LastSeen &Config() {
	return ActiveSettings().lastSeen;
}

// Everybody the question is meaningless for. A bot has no last seen, a service
// user is not a person, and our own status is never coarsened to us.
[[nodiscard]] bool Eligible(not_null<UserData*> user) {
	return !user->isSelf()
		&& !user->isBot()
		&& !user->isServiceUser()
		&& !user->isInaccessible();
}

// The moment itself, in clock time rather than as "20 minutes ago": the point
// of a traded read is that it is exact, and rounding it back into a phrase
// would throw away the only thing the trade bought.
[[nodiscard]] QString ExactMoment(TimeId till, TimeId now) {
	const auto moment = base::unixtime::parse(till);
	const auto today = base::unixtime::parse(now);
	const auto locale = QLocale();
	const auto time = locale.toString(moment.time(), QLocale::ShortFormat);
	if (moment.date() == today.date()) {
		return time;
	} else if (moment.date().addDays(1) == today.date()) {
		return u"yesterday %1"_q.arg(time);
	}
	return u"%1 %2"_q.arg(
		locale.toString(moment.date(), QLocale::ShortFormat),
		time);
}

// How old the read is. Coarse on purpose - the age is a caveat on the exact
// time beside it, and a caveat counted to the second would read as the more
// precise half of the sentence.
[[nodiscard]] QString AgeText(int seconds) {
	if (seconds < 60) {
		return u"just now"_q;
	}
	const auto minutes = seconds / 60;
	if (minutes < 60) {
		return (minutes == 1) ? u"1 min ago"_q : u"%1 min ago"_q.arg(minutes);
	}
	const auto hours = seconds / 3600;
	return (hours == 1) ? u"1 hour ago"_q : u"%1 hours ago"_q.arg(hours);
}

[[nodiscard]] QString DurationText(int seconds) {
	if (seconds < 60) {
		return (seconds == 1)
			? u"1 second"_q
			: u"%1 seconds"_q.arg(seconds);
	}
	const auto minutes = seconds / 60;
	if (minutes < 60) {
		return (minutes == 1) ? u"a minute"_q : u"%1 minutes"_q.arg(minutes);
	}
	const auto hours = seconds / 3600;
	return (hours == 1) ? u"an hour"_q : u"%1 hours"_q.arg(hours);
}

// The wait the sheet counts down, as a clock rather than as a phrase: it is
// ticking on screen, and "about 3 minutes" that stands still for sixty seconds
// reads as a stuck box.
[[nodiscard]] QString CooldownText(int seconds) {
	const auto hours = seconds / 3600;
	const auto rest = seconds % 60;
	return hours
		? u"%1:%2:%3"_q.arg(hours)
			.arg((seconds / 60) % 60, 2, 10, QChar('0'))
			.arg(rest, 2, 10, QChar('0'))
		: u"%1:%2"_q.arg(seconds / 60).arg(rest, 2, 10, QChar('0'));
}

[[nodiscard]] QString RememberedText(const LastSeenNote &note, TimeId now) {
	return u"last seen %1%2as of %3"_q.arg(
		ExactMoment(TimeId(note.wasOnlineUnix), now),
		kSeparator,
		AgeText(std::max(now - TimeId(note.readAtUnix), 0)));
}

// The one fact about a status the core cannot read for itself: whether it is
// one of the three vague spellings. Everything after this is a rule about
// data, and those live in the core.
[[nodiscard]] bool CoarseStatus(Data::LastseenStatus status) {
	return status.isRecently()
		|| status.isWithinWeek()
		|| status.isWithinMonth();
}

[[nodiscard]] LastSeenNote NoteFor(not_null<UserData*> user, TimeId now) {
	if (!Eligible(user)) {
		return LastSeenNote();
	}
	return LastSeenNoteNow(
		ActiveSettings(),
		CurrentState(),
		IdOf(user),
		ReasonForUser(user),
		CoarseStatus(user->lastseen()),
		int64(now));
}

// One trade, from the click to the rules going back. At most one runs at a time
// for the whole app, which is not a limitation worth working around: a trade is
// a window in which somebody can see our last seen, and two of them open at
// once would be two windows we did not separately agree to.
//
// Held in a file-static pointer with an explicit reset rather than in the
// session's lifetime, because "is one running" has to be answerable. Everything
// it touches goes through a weak session pointer, so an account logged out
// mid-trade leaves it with nothing to do rather than with a dangling one.
class Trade final {
public:
	Trade(
		not_null<Main::Session*> session,
		not_null<UserData*> user,
		std::shared_ptr<Ui::Show> show);

	void start();

private:
	void showOurs(const Api::UserPrivacy::Rule &rule);
	void requestStatus();
	void finish(const QString &reason, bool read);
	void restore();

	const base::weak_ptr<Main::Session> _session;
	const PeerIdValue _peer = 0;
	const UserId _userId = 0;
	const std::shared_ptr<Ui::Show> _show;

	std::optional<Api::UserPrivacy::Rule> _previous;
	base::Timer _hold;
	rpl::lifetime _lifetime;
	bool _ignoreCached = false;
	bool _restored = false;
	bool _done = false;

};

std::unique_ptr<Trade> Running;

Trade::Trade(
	not_null<Main::Session*> session,
	not_null<UserData*> user,
	std::shared_ptr<Ui::Show> show)
: _session(session)
, _peer(IdOf(user))
, _userId(peerToUser(user->id))
, _show(std::move(show))
, _hold([=] { finish(u"the hold ran out"_q, false); }) {
}

void Trade::start() {
	const auto session = _session.get();
	if (!session) {
		return;
	}
	LOG(("Purple: last seen trade with %1 - reading our rules."
		).arg(QString::number(_peer)));

	// The cached rules fire the moment we subscribe, and they are whatever was
	// fetched last, which is not good enough to put back afterwards. So the
	// synchronous first emission is dropped and the reload's answer taken
	// instead; a reload already in flight answers the same way.
	_ignoreCached = true;
	session->api().userPrivacy().value(
		Api::UserPrivacy::Key::LastSeen
	) | rpl::on_next([=, this](Api::UserPrivacy::Rule rule) {
		if (_ignoreCached || _previous) {
			return;
		}
		_previous = rule;
		showOurs(rule);
	}, _lifetime);
	_ignoreCached = false;

	session->api().userPrivacy().reload(Api::UserPrivacy::Key::LastSeen);

	// Covers the fetch as well as the read: a trade that cannot even learn our
	// own rules must not sit there forever, and the hold is the one number the
	// file gives for how long any of this may take.
	_hold.callOnce(crl::time(1000) * std::max(Config().tradeHoldSeconds, 1));
}

void Trade::showOurs(const Api::UserPrivacy::Rule &rule) {
	const auto session = _session.get();
	if (!session) {
		return;
	}
	const auto peer = not_null<PeerData*>(session->data().user(_userId));
	auto modified = rule;
	auto &always = modified.always.peers;
	auto &never = modified.never.peers;
	never.erase(ranges::remove(never, peer), end(never));
	if (!ranges::contains(always, peer)) {
		always.push_back(peer);
	}
	LOG(("Purple: last seen trade with %1 - showing ours."
		).arg(QString::number(_peer)));
	session->api().userPrivacy().save(
		Api::UserPrivacy::Key::LastSeen,
		modified);
	requestStatus();
}

void Trade::requestStatus() {
	const auto session = _session.get();
	if (!session) {
		return;
	}
	const auto user = session->data().user(_userId);
	session->changes().peerUpdates(
		user,
		Data::PeerUpdate::Flag::OnlineStatus
	) | rpl::on_next([=, this] {
		// Only a status carrying a real moment ends the wait. A coarse one is
		// the server still saying no, and the local "online till" the client
		// keeps beside a coarse status is our own old knowledge rather than
		// anything this trade bought.
		const auto status = user->lastseen();
		const auto till = status.onlineTill();
		if (status.isHidden() || !till) {
			return;
		}
		const auto now = base::unixtime::now();
		const auto moment = status.isOnline(now) ? now : till;
		UpdateState([&](State &state) {
			RememberTrade(state, _peer, int64(now), int64(moment));
		});
		LOG(("Purple: last seen trade with %1 - read %2."
			).arg(QString::number(_peer), QString::number(moment)));
		finish(u"read"_q, true);
	}, _lifetime);

	session->api().request(MTPusers_GetUsers(
		MTP_vector<MTPInputUser>(1, user->inputUser())
	)).done([=](const MTPVector<MTPUser> &result) {
		if (const auto strong = _session.get()) {
			strong->data().processUsers(result);
		}
	}).fail([=](const MTP::Error &error) {
		LOG(("Purple Error: last seen trade could not ask about %1, %2."
			).arg(QString::number(_peer), error.type()));
	}).send();
}

void Trade::finish(const QString &reason, bool read) {
	if (_done) {
		return;
	}
	_done = true;
	_hold.cancel();
	if (!read) {
		// Written down even though nothing was read, because this is what the
		// cooldown counts: a trade that answered nothing is still a window
		// somebody could have looked through, and repeating it every time the
		// chat opens is the thing the cooldown exists to stop.
		const auto now = base::unixtime::now();
		UpdateState([&](State &state) {
			RememberTrade(state, _peer, int64(now), 0);
		});
		LOG(("Purple: last seen trade with %1 - nothing read, %2."
			).arg(QString::number(_peer), reason));
		if (_show) {
			_show->showToast(u"Nothing was read - they hide their last seen "
				"for their own reasons."_q);
		}
	}
	restore();

	// The subscriptions are dropped from inside one of their own handlers, so
	// the object goes a turn later rather than under the stack still walking
	// it.
	crl::on_main([] { Running = nullptr; });
}

void Trade::restore() {
	if (_restored) {
		return;
	}
	_restored = true;
	const auto session = _session.get();
	if (!session || !_previous) {
		return;
	}
	LOG(("Purple: last seen trade with %1 - rules back."
		).arg(QString::number(_peer)));
	session->api().userPrivacy().save(
		Api::UserPrivacy::Key::LastSeen,
		*_previous);
}

// What the sheet will not open for at all. The cooldown is deliberately not
// here: a wait is not a refusal, and refusing it in a toast is what made the
// remembered line a dead end - the sheet opens, counts the wait down and
// offers the re-trade when it is spent.
[[nodiscard]] QString Refusal(not_null<UserData*> user) {
	if (!LastSeenTradeOffered()) {
		return u"The last seen trade is switched off."_q;
	} else if (ReasonForUser(user) != LastSeenReason::ByMe) {
		return u"%1's last seen is not coarse because of your own privacy, "
			"so there is nothing to trade."_q.arg(user->shortName());
	} else if (Running) {
		return u"A trade is already running."_q;
	}
	return QString();
}

} // namespace

LastSeenReason ReasonForUser(not_null<UserData*> user) {
	if (!Eligible(user)) {
		return LastSeenReason::None;
	}
	const auto status = user->lastseen();
	const auto coarse = status.isRecently()
		|| status.isWithinWeek()
		|| status.isWithinMonth();
	return ReasonFor(!status.isHidden(), coarse, status.isHiddenByMe());
}

LastSeenText LastSeenNoteFor(
		not_null<UserData*> user,
		TimeId now,
		bool full,
		bool narrow) {
	auto result = LastSeenText();
	result.base = full
		? Data::OnlineTextFull(user, now)
		: Data::OnlineText(user, now);
	result.text = result.base;

	const auto note = NoteFor(user, now);
	result.tappable = note.tappable;
	switch (note.line) {
	case LastSeenLine::Plain:
		return result;

	// A read that is still fresh replaces the coarse phrase rather than hanging
	// off it: "last seen recently, and also 14:32" would be the app saying the
	// vaguer half first. What hangs off it instead is the way back into the
	// sheet, because the trade took the tail that used to be the only door.
	case LastSeenLine::Remembered:
		result.base = RememberedText(note, now);
		result.text = result.base;
		if (note.tappable) {
			result.tail = narrow ? kShortReason : kLongRefresh;
		}
		break;

	case LastSeenLine::ByMeTail:
		result.tail = narrow ? kShortReason : kLongReason;
		break;
	}
	if (!result.tail.isEmpty()) {
		result.text = result.base + kSeparator + result.tail;
		if (note.tappable) {
			result.link = result.tail;
		}
	}
	return result;
}

bool LastSeenTradeOffered() {
	return Config().trade;
}

void ShowLastSeenTradeBox(
		not_null<Window::SessionController*> controller,
		not_null<UserData*> user) {
	const auto now = base::unixtime::now();
	if (const auto refusal = Refusal(user); !refusal.isEmpty()) {
		controller->showToast(refusal);
		return;
	}
	const auto session = &controller->session();
	const auto show = controller->uiShow();
	const auto peer = IdOf(user);
	const auto seconds = Config().tradeHoldSeconds;
	const auto cooldown = Config().tradeCooldownSeconds;
	const auto left = TradeCooldownLeft(
		CurrentState(),
		peer,
		int64(now),
		cooldown);
	const auto again = (left > 0)
		|| RememberedTrade(
			CurrentState(),
			peer,
			int64(now),
			Config().tradeRememberSeconds).has_value();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Show mine to see theirs"_q));

		const auto container = box->verticalLayout();
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				u"Telegram hides %1's exact last seen from you because you "
				"hide yours from them.\n\nFor up to %2 this will let %1 see "
				"your last seen, read theirs once, and put your privacy back "
				"exactly as it was. Nobody is told, and nothing else about "
				"your privacy changes. If they hide their last seen for their "
				"own reasons, nothing will be read."_q.arg(
					user->shortName(),
					DurationText(seconds)),
				st::boxLabel),
			st::boxRowPadding);

		const auto countdown = (left > 0)
			? container->add(
				object_ptr<Ui::FlatLabel>(
					container,
					QString(),
					st::boxDividerLabel),
				st::boxRowPadding)
			: nullptr;

		// "Don't offer this again" is the same switch as Settings -> Advanced
		// -> Purple -> "Offer the last seen trade", written to settings.toml,
		// rather than a second flag meaning the same thing somewhere else. It
		// takes the offer away everywhere and leaves the explanation, which is
		// what somebody who never wants to be asked is asking for.
		const auto never = container->add(
			object_ptr<Ui::Checkbox>(
				container,
				u"Don't offer this again"_q,
				false,
				st::defaultCheckbox),
			st::boxRowPadding);

		const auto apply = [=] {
			if (!never->checked()) {
				return;
			}
			WriteSettings([=](const QString &text) {
				return SetTableBool(
					text,
					SettingsFilePath(),
					u"last_seen"_q,
					u"trade_p"_q,
					false);
			}, u"the last seen trade"_q);
		};

		const auto share = box->addButton(
			rpl::single(again ? u"Refresh now"_q : u"Share once"_q),
			[=] {
				apply();
				box->closeBox();
				if (Running) {
					show->showToast(u"A trade is already running."_q);
					return;
				}
				Running = std::make_unique<Trade>(session, user, show);
				Running->start();
			});

		// One trade per person per `trade_cooldown', counted from the read
		// that is already written down, so the wait is the same number the
		// line behind this box is offering to refresh. It is recomputed from
		// the clock on every tick rather than decremented, because a box left
		// open through a suspend would otherwise finish counting a wait that
		// wall-clock time had already spent.
		if (countdown) {
			share->setDisabled(true);
			share->setTextFgOverride(st::windowSubTextFg->c);
			const auto timer = box->lifetime().make_state<base::Timer>();
			const auto tick = [=] {
				const auto left = TradeCooldownLeft(
					CurrentState(),
					peer,
					int64(base::unixtime::now()),
					cooldown);
				if (left > 0) {
					countdown->setText(
						u"You can refresh in %1."_q.arg(CooldownText(left)));
					timer->callOnce(crl::time(1000));
					return;
				}
				countdown->setText(u"You can refresh now."_q);
				share->setDisabled(false);
				share->setTextFgOverride(std::nullopt);
			};
			timer->setCallback(tick);
			tick();
		}
		box->addButton(tr::lng_cancel(), [=] {
			apply();
			box->closeBox();
		});
	}));
}

void LastSeenTradesBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session) {
	box->setTitle(rpl::single(u"Last seen trades"_q));
	box->setWidth(st::boxWideWidth);

	const auto container = box->verticalLayout();
	const auto now = base::unixtime::now();
	const auto title = TitleResolver(session);
	auto trades = CurrentState().lastSeenTrades;
	ranges::sort(trades, ranges::greater(), &LastSeenTrade::readAtUnix);

	auto lines = QStringList();
	for (const auto &trade : trades) {
		const auto name = title(trade.peer);
		const auto who = name.isEmpty()
			? QString::number(trade.peer)
			: name;
		const auto age = AgeText(std::max(now - TimeId(trade.readAtUnix), 0));
		lines.push_back(trade.wasOnlineUnix
			? u"%1 - last seen %2, read %3"_q.arg(
				who,
				ExactMoment(TimeId(trade.wasOnlineUnix), now),
				age)
			: u"%1 - nothing was read, %2"_q.arg(who, age));
	}
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			(lines.isEmpty()
				? u"Nothing has been traded for yet."_q
				: lines.join('\n')),
			st::boxLabel),
		st::boxRowPadding);
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			u"Kept in state.toml on this machine only, and dropped once a "
			"read is older than [last_seen] trade_remember."_q,
			st::boxDividerLabel),
		st::boxRowPadding);

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace Purple
