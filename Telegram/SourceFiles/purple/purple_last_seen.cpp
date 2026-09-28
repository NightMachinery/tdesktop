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
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_changes.h"
#include "data/data_lastseen_status.h"
#include "data/data_peer_id.h"
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
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QSaveFile>

#include <map>

namespace Purple {
namespace {

// The words, and the mark that stands in for them where a sentence does not
// fit. Both come after the separator every other status suffix in the app uses,
// so the fork's addition reads as part of the same line rather than as a second
// one that ran into the first.
const auto kSeparator = QString::fromUtf8(" \xC2\xB7 ");
const auto kShortReason = QString::fromUtf8("\xF0\x9F\x91\x80");
constexpr auto kSkipPeekConfirmationPref = "purple-skip-last-seen-peek-confirmation";
constexpr auto kRestoreRetry = crl::time(5000);
constexpr auto kRestoreWatchdog = crl::time(30000);

[[nodiscard]] QString JournalPath(not_null<Main::Session*> session) {
	const auto environment = int(session->mtp().environment());
	return cWorkingDir() + u"tdata/purple-last-seen-peek-%1-%2.json"_q
		.arg(environment).arg(session->userId().bare);
}

[[nodiscard]] QJsonArray WritePeers(
		const Api::UserPrivacy::Exceptions &exceptions) {
	auto result = QJsonArray();
	for (const auto peer : exceptions.peers) {
		auto value = QJsonObject{
			{ u"id"_q, QString::number(peer->id.value) },
		};
		if (const auto user = peer->asUser()) {
			value.insert(u"hash"_q, QString::number(user->accessHash()));
		}
		result.append(value);
	}
	return result;
}

[[nodiscard]] QJsonObject WriteRule(const Api::UserPrivacy::Rule &rule) {
	return {
		{ u"option"_q, int(rule.option) },
		{ u"always"_q, WritePeers(rule.always) },
		{ u"never"_q, WritePeers(rule.never) },
		{ u"premiums"_q, rule.always.premiums },
		{ u"allowBots"_q, rule.always.miniapps },
		{ u"disallowBots"_q, rule.never.miniapps },
		{ u"ignoreAlways"_q, rule.ignoreAlways },
		{ u"ignoreNever"_q, rule.ignoreNever },
	};
}

[[nodiscard]] bool ReadPeers(
		const QJsonValue &value,
		not_null<Main::Session*> session,
		Api::UserPrivacy::Exceptions &exceptions) {
	if (!value.isArray()) {
		return false;
	}
	for (const auto &entry : value.toArray()) {
		const auto object = entry.toObject();
		auto ok = false;
		const auto raw = object.value(u"id"_q).toString().toULongLong(&ok);
		if (!ok || !raw) {
			return false;
		}
		const auto id = PeerId(raw);
		if (!peerIsUser(id) && !peerIsChat(id) && !peerIsChannel(id)) {
			return false;
		}
		if (peerIsUser(id)) {
			const auto hash = object.value(u"hash"_q).toString().toULongLong(&ok);
			if (!ok || !hash) {
				return false;
			}
			const auto user = session->data().user(peerToUser(id));
			if (!user->accessHash()) {
				user->setAccessHash(hash);
			}
		}
		exceptions.peers.push_back(session->data().peer(id));
	}
	return true;
}

[[nodiscard]] std::optional<Api::UserPrivacy::Rule> ReadRule(
		const QJsonValue &value,
		not_null<Main::Session*> session) {
	if (!value.isObject()) {
		return std::nullopt;
	}
	const auto object = value.toObject();
	const auto option = object.value(u"option"_q).toInt(-1);
	if (option < int(Api::UserPrivacy::Option::Everyone)
		|| option > int(Api::UserPrivacy::Option::Nobody)
		|| !object.value(u"premiums"_q).isBool()
		|| !object.value(u"allowBots"_q).isBool()
		|| !object.value(u"disallowBots"_q).isBool()
		|| !object.value(u"ignoreAlways"_q).isBool()
		|| !object.value(u"ignoreNever"_q).isBool()) {
		return std::nullopt;
	}
	auto rule = Api::UserPrivacy::Rule();
	rule.option = Api::UserPrivacy::Option(option);
	if (!ReadPeers(object.value(u"always"_q), session, rule.always)
		|| !ReadPeers(object.value(u"never"_q), session, rule.never)) {
		return std::nullopt;
	}
	rule.always.premiums = object.value(u"premiums"_q).toBool();
	rule.always.miniapps = object.value(u"allowBots"_q).toBool();
	rule.never.miniapps = object.value(u"disallowBots"_q).toBool();
	rule.ignoreAlways = object.value(u"ignoreAlways"_q).toBool();
	rule.ignoreNever = object.value(u"ignoreNever"_q).toBool();
	return rule;
}

[[nodiscard]] bool SameRule(
		const Api::UserPrivacy::Rule &a,
		const Api::UserPrivacy::Rule &b) {
	const auto samePeers = [](const auto &left, const auto &right) {
		auto ids = std::vector<PeerId>();
		for (const auto peer : left.peers) {
			ids.push_back(peer->id);
		}
		for (const auto peer : right.peers) {
			const auto i = ranges::find(ids, peer->id);
			if (i == end(ids)) {
				return false;
			}
			ids.erase(i);
		}
		return ids.empty();
	};
	return a.option == b.option
		&& a.always.premiums == b.always.premiums
		&& a.always.miniapps == b.always.miniapps
		&& a.never.miniapps == b.never.miniapps
		&& a.ignoreAlways == b.ignoreAlways
		&& a.ignoreNever == b.ignoreNever
		&& samePeers(a.always, b.always)
		&& samePeers(a.never, b.never);
}

[[nodiscard]] bool WriteJournal(
		not_null<Main::Session*> session,
		const Api::UserPrivacy::Rule &rule) {
	const auto validPeers = [](const auto &exceptions) {
		return ranges::all_of(exceptions.peers, [](const auto peer) {
			const auto id = peer->id;
			return (peerIsUser(id) && peer->asUser()->accessHash())
				|| peerIsChat(id)
				|| peerIsChannel(id);
		});
	};
	if (!validPeers(rule.always) || !validPeers(rule.never)) {
		return false;
	}
	const auto path = JournalPath(session);
	if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
		return false;
	}
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	if (!file.setPermissions(
		QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
		file.cancelWriting();
		return false;
	}
	const auto document = QJsonDocument(QJsonObject{
		{ u"version"_q, 1 },
		{ u"environment"_q, int(session->mtp().environment()) },
		{ u"self"_q, QString::number(session->userId().bare) },
		{ u"rule"_q, WriteRule(rule) },
	});
	const auto bytes = document.toJson(QJsonDocument::Compact);
	if (file.write(bytes) != bytes.size() || !file.commit()) {
		return false;
	}
	return true;
}

[[nodiscard]] std::optional<Api::UserPrivacy::Rule> ReadJournal(
		not_null<Main::Session*> session) {
	auto file = QFile(JournalPath(session));
	if (!file.open(QIODevice::ReadOnly)) {
		return std::nullopt;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	if (object.value(u"version"_q).toInt() != 1
		|| object.value(u"environment"_q).toInt(-1)
			!= int(session->mtp().environment())
		|| object.value(u"self"_q).toString()
			!= QString::number(session->userId().bare)) {
		return std::nullopt;
	}
	return ReadRule(object.value(u"rule"_q), session);
}

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

class PeekRestoration;
std::map<QString, std::shared_ptr<PeekRestoration>> PendingRestores;

class PeekRestoration final : public std::enable_shared_from_this<PeekRestoration> {
public:
	PeekRestoration(
		not_null<Main::Session*> session,
		Api::UserPrivacy::Rule original)
	: _session(session)
	, _path(JournalPath(session))
	, _original(std::move(original))
	, _retry([=] { onTimer(); })
	, _verify([=] { verifyAgain(); }) {
	}

	void restore() {
		const auto session = _session.get();
		if (!session || _checking) {
			return;
		}
		_checking = true;
		_inFlight = true;
		_verifiedOnce = false;
		const auto serial = ++_serial;
		_retry.callOnce(kRestoreWatchdog);
		const auto weak = weak_from_this();
		session->api().userPrivacy().save(
			Api::UserPrivacy::Key::LastSeen,
			_original,
			[weak, serial](bool saved) {
				if (const auto strong = weak.lock()) {
					strong->saved(serial, saved);
				}
			},
			_afterRequest);
	}

	void orderAfter(mtpRequestId requestId) {
		_afterRequest = requestId;
	}

	[[nodiscard]] Main::Session *session() const {
		return _session.get();
	}

private:
	void onTimer() {
		if (_inFlight) {
			LOG(("Purple: Last Seen Peek restore still pending."));
			_retry.callOnce(kRestoreWatchdog);
			return;
		}
		_checking = false;
		_verify.cancel();
		restore();
	}

	void saved(int serial, bool saved) {
		if (serial != _serial) {
			return;
		}
		_inFlight = false;
		_retry.cancel();
		if (!saved) {
			_retry.callOnce(kRestoreRetry);
			return;
		}
		_retry.callOnce(kRestoreWatchdog);
		const auto session = _session.get();
		if (!session) {
			return;
		}
		const auto weak = weak_from_this();
		session->api().userPrivacy().reloadFresh(
			Api::UserPrivacy::Key::LastSeen,
			[weak, serial](std::optional<Api::UserPrivacy::Rule> rule) {
				if (const auto strong = weak.lock()) {
					strong->verified(serial, std::move(rule));
				}
			});
	}

	void verified(
			int serial,
			std::optional<Api::UserPrivacy::Rule> rule) {
		if (serial != _serial) {
			return;
		}
		if (!rule || !SameRule(*rule, _original)) {
			LOG(("Purple Error: Last Seen Peek restore not yet verified."));
			_retry.callOnce(kRestoreRetry);
			return;
		}
		if (!_verifiedOnce) {
			_verifiedOnce = true;
			_verify.callOnce(crl::time(1000));
			return;
		}
		_retry.cancel();
		if (!QFile::remove(_path)) {
			LOG(("Purple Error: Last Seen Peek journal not removed."));
			_retry.callOnce(kRestoreRetry);
			return;
		}
		LOG(("Purple: Last Seen Peek original rules verified and restored."));
		crl::on_main([path = _path, weak = weak_from_this()] {
			const auto i = PendingRestores.find(path);
			if (i != end(PendingRestores)
				&& i->second == weak.lock()) {
				PendingRestores.erase(i);
			}
		});
	}

	void verifyAgain() {
		const auto session = _session.get();
		if (!session) {
			return;
		}
		const auto serial = _serial;
		const auto weak = weak_from_this();
		session->api().userPrivacy().reloadFresh(
			Api::UserPrivacy::Key::LastSeen,
			[weak, serial](std::optional<Api::UserPrivacy::Rule> rule) {
				if (const auto strong = weak.lock()) {
					strong->verified(serial, std::move(rule));
				}
			});
	}

	const base::weak_ptr<Main::Session> _session;
	const QString _path;
	const Api::UserPrivacy::Rule _original;
	base::Timer _retry;
	base::Timer _verify;
	int _serial = 0;
	bool _checking = false;
	bool _inFlight = false;
	bool _verifiedOnce = false;
	mtpRequestId _afterRequest = 0;

};

void RestoreJournal(not_null<Main::Session*> session) {
	const auto path = JournalPath(session);
	if (const auto i = PendingRestores.find(path); i != end(PendingRestores)) {
		if (i->second->session() == session.get()) {
			i->second->restore();
			return;
		}
		PendingRestores.erase(i);
	}
	if (!QFileInfo::exists(path)) {
		return;
	}
	const auto original = ReadJournal(session);
	if (!original) {
		LOG(("Purple Error: Last Seen Peek journal unreadable: %1."
			).arg(path));
		return;
	}
	auto restore = std::make_shared<PeekRestoration>(session, *original);
	PendingRestores.emplace(path, restore);
	restore->restore();
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
class LastSeenPeek final : public base::has_weak_ptr {
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

	const base::weak_ptr<Main::Session> _session;
	const PeerIdValue _peer = 0;
	const UserId _userId = 0;
	const std::shared_ptr<Ui::Show> _show;

	base::Timer _hold;
	bool _journaled = false;
	bool _opened = false;
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

	_hold.callOnce(crl::time(1000) * std::max(Config().tradeHoldSeconds, 1));
	const auto weak = base::make_weak(this);
	session->api().userPrivacy().reloadFresh(
		Api::UserPrivacy::Key::LastSeen,
		[weak](std::optional<Api::UserPrivacy::Rule> rule) {
			if (const auto strong = weak.get(); strong && !strong->_done) {
				if (rule) {
					strong->showOurs(*rule);
				} else {
					strong->finish(u"privacy fetch failed"_q, false);
				}
			}
		});
}

void LastSeenPeek::showOurs(const Api::UserPrivacy::Rule &rule) {
	const auto session = _session.get();
	if (!session) {
		return;
	}
	if (QFileInfo::exists(JournalPath(session))
		|| !WriteJournal(session, rule)) {
		LOG(("Purple Error: Last Seen Peek journal could not be written."));
		finish(u"journal unavailable"_q, false);
		return;
	}
	_journaled = true;
	auto restoration = std::make_shared<PeekRestoration>(session, rule);
	PendingRestores.emplace(JournalPath(session), restoration);
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
	const auto weak = base::make_weak(this);
	const auto requestId = session->api().userPrivacy().save(
		Api::UserPrivacy::Key::LastSeen,
		modified,
		[weak](bool opened) {
			if (const auto strong = weak.get(); strong && !strong->_done) {
				if (opened) {
					strong->_opened = true;
					strong->requestStatus();
				} else {
					strong->finish(u"temporary rule failed"_q, false);
				}
			}
		});
	restoration->orderAfter(requestId);
}

void LastSeenPeek::requestStatus() {
	const auto session = _session.get();
	if (!session) {
		return;
	}
	const auto user = session->data().user(_userId);
	const auto weak = base::make_weak(this);
	session->api().request(MTPusers_GetUsers(
		MTP_vector<MTPInputUser>(1, user->inputUser())
	)).done([weak](const MTPVector<MTPUser> &result) {
		if (const auto strong = weak.get(); strong && !strong->_done) {
			if (const auto session = strong->_session.get()) {
				auto moment = TimeId(0);
				auto online = false;
				for (const auto &entry : result.v) {
					if (entry.type() != mtpc_user) {
						continue;
					}
					const auto &data = entry.c_user();
					if (data.vid().v != strong->_userId.bare) {
						continue;
					}
					if (const auto status = data.vstatus()) {
						if (status->type() == mtpc_userStatusOnline) {
							online = true;
						} else if (status->type() == mtpc_userStatusOffline) {
							moment = status->c_userStatusOffline()
								.vwas_online().v;
						}
					}
				}
				session->data().processUsers(result);
				if (online || moment > 0) {
					const auto now = base::unixtime::now();
					if (online) {
						moment = now;
					}
					UpdateState([&](State &state) {
						RememberTrade(
							state,
							strong->_peer,
							int64(now),
							int64(moment));
					});
					LOG(("Purple: Last Seen Peek with %1 - read %2."
						).arg(QString::number(strong->_peer),
							QString::number(moment)));
					strong->finish(u"read"_q, true);
				}
			}
		}
	}).fail([weak](const MTP::Error &error) {
		if (const auto strong = weak.get(); strong && !strong->_done) {
			LOG(("Purple Error: Last Seen Peek could not ask about %1, %2."
				).arg(QString::number(strong->_peer), error.type()));
			strong->finish(u"status request failed"_q, false);
		}
	}).send();
}

void LastSeenPeek::finish(const QString &reason, bool read) {
	if (_done) {
		return;
	}
	_done = true;
	_hold.cancel();
	if (!read && _opened) {
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
	if (_journaled) {
		if (const auto session = _session.get()) {
			const auto path = JournalPath(session);
			if (const auto i = PendingRestores.find(path);
				i != end(PendingRestores)) {
				i->second->restore();
			}
		}
	}

	crl::on_main([] { RunningPeek = nullptr; });
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

void StartPeek(
		not_null<Main::Session*> session,
		not_null<UserData*> user,
		std::shared_ptr<Ui::Show> show) {
	if (RunningPeek || QFileInfo::exists(JournalPath(session))
		|| PendingRestores.contains(JournalPath(session))) {
		show->showToast(tr::lng_lastseen_peek_running(tr::now));
		return;
	}
	RunningPeek = std::make_unique<LastSeenPeek>(session, user, show);
	RunningPeek->start();
}

} // namespace

void RecoverLastSeenPeek(not_null<Main::Session*> session) {
	RestoreJournal(session);
}

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

bool SkipLastSeenPeekConfirmation() {
	return Core::App().settings().readPref<bool>(kSkipPeekConfirmationPref);
}

void SetSkipLastSeenPeekConfirmation(bool skip) {
	Core::App().settings().writePref<bool>(kSkipPeekConfirmationPref, skip);
	Core::App().saveSettingsDelayed();
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
	if ((left <= 0) && SkipLastSeenPeekConfirmation()) {
		StartPeek(session, user, show);
		return;
	}
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

		const auto skipConfirmation = container->add(
			object_ptr<Ui::Checkbox>(
				container,
				tr::lng_lastseen_peek_skip_confirmation(tr::now),
				SkipLastSeenPeekConfirmation(),
				st::defaultCheckbox),
			st::boxRowPadding);
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				tr::lng_lastseen_peek_skip_confirmation_about(),
				st::boxDividerLabel),
			st::boxRowPadding);

		const auto apply = [=] {
			const auto skip = skipConfirmation->checked();
			if (skip != SkipLastSeenPeekConfirmation()) {
				SetSkipLastSeenPeekConfirmation(skip);
			}
		};

		const auto share = box->addButton(
			(again
				? tr::lng_lastseen_peek_again()
				: tr::lng_lastseen_peek_now()),
			[=] {
				apply();
				box->closeBox();
				StartPeek(session, user, show);
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
