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
// of a peeked read is that it is exact, and rounding it back into a phrase
// would throw away the only thing the peek found.
[[nodiscard]] QString ExactMoment(TimeId till, TimeId now) {
	const auto moment = base::unixtime::parse(till);
	const auto today = base::unixtime::parse(now);
	const auto locale = QLocale();
	const auto time = locale.toString(moment.time(), QLocale::ShortFormat);
	if (moment.date() == today.date()) {
		return time;
	} else if (moment.date().addDays(1) == today.date()) {
		return tr::lng_lastseen_peek_moment_yesterday(
			tr::now,
			lt_time,
			time);
	}
	return tr::lng_lastseen_peek_moment_date(
		tr::now,
		lt_date,
		locale.toString(moment.date(), QLocale::ShortFormat),
		lt_time,
		time);
}

// How old the read is. Coarse on purpose - the age is a caveat on the exact
// time beside it, and a caveat counted to the second would read as the more
// precise half of the sentence.
[[nodiscard]] QString AgeText(int seconds) {
	if (seconds < 60) {
		return tr::lng_lastseen_peek_age_now(tr::now);
	}
	const auto minutes = seconds / 60;
	if (minutes < 60) {
		return tr::lng_lastseen_peek_age_minutes(
			tr::now,
			lt_count,
			minutes);
	}
	const auto hours = seconds / 3600;
	return tr::lng_lastseen_peek_age_hours(
		tr::now,
		lt_count,
		hours);
}

[[nodiscard]] QString DurationText(int seconds) {
	if (seconds < 60) {
		return tr::lng_seconds(tr::now, lt_count, seconds);
	}
	const auto minutes = seconds / 60;
	if (minutes < 60) {
		return tr::lng_minutes(tr::now, lt_count, minutes);
	}
	const auto hours = seconds / 3600;
	return tr::lng_hours(tr::now, lt_count, hours);
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
	return tr::lng_lastseen_peek_remembered(
		tr::now,
		lt_last_seen,
		ExactMoment(TimeId(note.wasOnlineUnix), now),
		lt_age,
		AgeText(std::max(now - TimeId(note.readAtUnix), 0)));
}

// The one fact about a status the core cannot read for itself: which of the
// three kinds it is. Everything after this is a rule about data, and those
// live in the core.
//
// The split is on `isLongAgo' rather than on `isHidden', which is the near
// miss worth naming: `isHidden' is every status the server did not hand a
// `was_online' with, so it covers the three vague spellings AND the locally
// guessed online moment - the one `madeAction' writes when we watched somebody
// act in a chat, and the one `LastseenFromMTP' keeps when a coarse status
// arrives over the top of it. That moment is real and upstream already prints
// it as a time, so it is Exact: the app has the truth and a read from some
// hours ago put over it would be older news dressed as newer. `isLongAgo' is
// exactly `userStatusEmpty' - and the offline status with nothing usable in it,
// which folds into the same value - so the three cases stay exhaustive.
[[nodiscard]] LastSeenShape ShapeOf(Data::LastseenStatus status) {
	const auto coarse = status.isRecently()
		|| status.isWithinWeek()
		|| status.isWithinMonth();
	return coarse
		? LastSeenShape::Coarse
		: status.isLongAgo()
		? LastSeenShape::LongAgo
		: LastSeenShape::Exact;
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
		ShapeOf(user->lastseen()),
		int64(now));
}

// One peek, from the click to the rules going back. At most one runs at a time
// for the whole app, which is not a limitation worth working around: a peek is
// a window in which somebody can see our last seen, and two of them open at
// once would be two windows we did not separately agree to.
//
// Held in a file-static pointer with an explicit reset rather than in the
// session's lifetime, because "is one running" has to be answerable. Everything
// it touches goes through a weak session pointer, so an account logged out
// mid-peek leaves it with nothing to do rather than with a dangling one.
class LastSeenPeek final {
public:
	LastSeenPeek(
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

std::unique_ptr<LastSeenPeek> RunningPeek;

LastSeenPeek::LastSeenPeek(
	not_null<Main::Session*> session,
	not_null<UserData*> user,
	std::shared_ptr<Ui::Show> show)
: _session(session)
, _peer(IdOf(user))
, _userId(peerToUser(user->id))
, _show(std::move(show))
, _hold([=] { finish(u"the hold ran out"_q, false); }) {
}

void LastSeenPeek::start() {
	const auto session = _session.get();
	if (!session) {
		return;
	}
	LOG(("Purple: Last Seen Peek with %1 - reading our rules."
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

	// Covers the fetch as well as the read: a peek that cannot even learn our
	// own rules must not sit there forever, and the hold is the one number the
	// file gives for how long any of this may take.
	_hold.callOnce(crl::time(1000) * std::max(Config().tradeHoldSeconds, 1));
}

void LastSeenPeek::showOurs(const Api::UserPrivacy::Rule &rule) {
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
	LOG(("Purple: Last Seen Peek with %1 - showing ours."
		).arg(QString::number(_peer)));
	session->api().userPrivacy().save(
		Api::UserPrivacy::Key::LastSeen,
		modified);
	requestStatus();
}

void LastSeenPeek::requestStatus() {
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
		// anything this peek found.
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
		LOG(("Purple: Last Seen Peek with %1 - read %2."
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
		LOG(("Purple Error: Last Seen Peek could not ask about %1, %2."
			).arg(QString::number(_peer), error.type()));
	}).send();
}

void LastSeenPeek::finish(const QString &reason, bool read) {
	if (_done) {
		return;
	}
	_done = true;
	_hold.cancel();
	if (!read) {
		// Written down even though nothing was read, because this is what the
		// cooldown counts: a peek that answered nothing is still a window
		// somebody could have looked through, and repeating it every time the
		// chat opens is the thing the cooldown exists to stop.
		const auto now = base::unixtime::now();
		UpdateState([&](State &state) {
			RememberTrade(state, _peer, int64(now), 0);
		});
		LOG(("Purple: Last Seen Peek with %1 - nothing read, %2."
			).arg(QString::number(_peer), reason));
		if (_show) {
			_show->showToast(tr::lng_lastseen_peek_empty_result(tr::now));
		}
	}
	restore();

	// The subscriptions are dropped from inside one of their own handlers, so
	// the object goes a turn later rather than under the stack still walking
	// it.
	crl::on_main([] { RunningPeek = nullptr; });
}

void LastSeenPeek::restore() {
	if (_restored) {
		return;
	}
	_restored = true;
	const auto session = _session.get();
	if (!session || !_previous) {
		return;
	}
	LOG(("Purple: Last Seen Peek with %1 - rules back."
		).arg(QString::number(_peer)));
	session->api().userPrivacy().save(
		Api::UserPrivacy::Key::LastSeen,
		*_previous);
}

// What the sheet will not open for at all. The cooldown is deliberately not
// here: a wait is not a refusal, and refusing it in a toast is what made the
// remembered line a dead end - the sheet opens, counts the wait down and
// enables another peek when it is spent.
[[nodiscard]] QString Refusal(not_null<UserData*> user) {
	if (!Config().trade) {
		return tr::lng_lastseen_peek_disabled(tr::now);
	} else if (ReasonForUser(user) != LastSeenReason::ByMe) {
		return tr::lng_lastseen_peek_unavailable(
			tr::now,
			lt_user,
			user->shortName());
	} else if (RunningPeek) {
		return tr::lng_lastseen_peek_running(tr::now);
	}
	return QString();
}

} // namespace

LastSeenReason ReasonForUser(not_null<UserData*> user) {
	if (!Eligible(user)) {
		return LastSeenReason::None;
	}
	const auto status = user->lastseen();
	return ReasonFor(ShapeOf(status), status.isHiddenByMe());
}

bool CanPeekLastSeen(not_null<UserData*> user) {
	return Config().trade
		&& (ReasonForUser(user) == LastSeenReason::ByMe);
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
	result.tappable = CanPeekLastSeen(user);
	switch (note.line) {
	case LastSeenLine::Plain:
		return result;

	// A read that is still fresh replaces the phrase underneath rather than
	// hanging off it: "last seen recently, and also 14:32" would be the app
	// saying the vaguer half first, and over "a long time ago" - where the core
	// now also shows it - the two halves would contradict each other outright.
	// What hangs off it instead is the way back into the sheet, because the
	// peek took the tail that used to be the only door. Over "a long time ago"
	// nothing hangs off it at all: such a status has no reason, so the note is
	// not tappable and there is no peek to start.
	case LastSeenLine::Remembered:
		result.base = RememberedText(note, now);
		result.text = result.base;
		if (result.tappable) {
			result.tail = narrow
				? kShortReason
				: tr::lng_lastseen_peek_again_suffix(tr::now);
		}
		break;

	case LastSeenLine::ByMeTail:
		result.tail = narrow
			? kShortReason
			: tr::lng_lastseen_peek_suffix(tr::now);
		break;
	}
	if (!result.tail.isEmpty()) {
		result.text = result.base + kSeparator + result.tail;
		if (result.tappable) {
			result.link = result.tail;
		}
	}
	return result;
}

void ShowLastSeenPeekBox(
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
		box->setTitle(tr::lng_lastseen_peek_title());

		const auto container = box->verticalLayout();
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				tr::lng_lastseen_peek_about(
					tr::now,
					lt_user,
					user->shortName(),
					lt_duration,
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

		const auto never = container->add(
			object_ptr<Ui::Checkbox>(
				container,
				tr::lng_lastseen_peek_disable(tr::now),
				false,
				st::defaultCheckbox),
			st::boxRowPadding);
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				tr::lng_lastseen_peek_disable_about(),
				st::boxDividerLabel),
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
			}, u"Last Seen Peek"_q);
		};

		const auto share = box->addButton(
			(again
				? tr::lng_lastseen_peek_again()
				: tr::lng_lastseen_peek_now()),
			[=] {
				apply();
				box->closeBox();
				if (RunningPeek) {
					show->showToast(
						tr::lng_lastseen_peek_running(tr::now));
					return;
				}
				RunningPeek = std::make_unique<LastSeenPeek>(session, user, show);
				RunningPeek->start();
			});

		// One peek per person per `trade_cooldown', counted from the read
		// that is already written down, so the wait is the same number the
		// line behind this box lets the user refresh. It is recomputed from
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
						tr::lng_lastseen_peek_wait(
							tr::now,
							lt_time,
							CooldownText(left)));
					timer->callOnce(crl::time(1000));
					return;
				}
				countdown->setText(
					tr::lng_lastseen_peek_ready(tr::now));
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

void LastSeenPeeksBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session) {
	box->setTitle(tr::lng_lastseen_peeks_title());
	box->setWidth(st::boxWideWidth);

	const auto container = box->verticalLayout();
	const auto now = base::unixtime::now();
	const auto title = TitleResolver(session);
	auto peeks = CurrentState().lastSeenTrades;
	ranges::sort(peeks, ranges::greater(), &LastSeenTrade::readAtUnix);

	auto lines = QStringList();
	for (const auto &peek : peeks) {
		const auto name = title(peek.peer);
		const auto who = name.isEmpty()
			? QString::number(peek.peer)
			: name;
		const auto age = AgeText(std::max(now - TimeId(peek.readAtUnix), 0));
		lines.push_back(peek.wasOnlineUnix
			? tr::lng_lastseen_peeks_read(
				tr::now,
				lt_user,
				who,
				lt_last_seen,
				ExactMoment(TimeId(peek.wasOnlineUnix), now),
				lt_age,
				age)
			: tr::lng_lastseen_peeks_no_read(
				tr::now,
				lt_user,
				who,
				lt_age,
				age));
	}
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			(lines.isEmpty()
				? tr::lng_lastseen_peeks_empty(tr::now)
				: lines.join('\n')),
			st::boxLabel),
		st::boxRowPadding);
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			tr::lng_lastseen_peeks_about(),
			st::boxDividerLabel),
		st::boxRowPadding);

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace Purple
