/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_screentime_recorder.h"

#include "base/qt_signal_producer.h"
#include "base/timer.h"
#include "core/application.h"
#include "data/data_peer.h"
#include "dialogs/dialogs_key.h"
#include "history/history.h"
#include "purple/purple_config.h"
#include "purple/purple_gate.h"
#include "window/window_session_controller.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>

namespace Purple {
namespace {

// How long a line can sit in memory before it reaches the file. Buffered
// because the composer fires an event per burst and the chat list fires one
// per glance, and a file opened for each of those would be an open-append-close
// on the main thread for something nobody is waiting to read.
constexpr auto kFlushDelay = crl::time(5 * 1000);

// How often the watchdog looks at the clock. It is not the idle threshold -
// that is [screen_time] idle_after, and the event it writes is stamped back to
// the moment input actually stopped, so a coarse tick still produces an exact
// pause. See the Idle branch of check().
constexpr auto kWatchdogTick = crl::time(5 * 1000);

// The prune is "once a day" as the plan asks, and this is what makes it true
// for an app that is left running for a week rather than started each morning.
// A pass that finds nothing to drop reads the file and writes nothing.
constexpr auto kPruneEvery = crl::time(6 * 60 * 60 * 1000);

[[nodiscard]] const ScreenTime &Config() {
	return ActiveSettings().screenTime;
}

[[nodiscard]] int64 NowMs() {
	return QDateTime::currentMSecsSinceEpoch();
}

// Empty for Normal, which is how the log spells it and what makes a preset
// split possible without a lookup. Filtering() rather than the state's own
// string because a preset that no longer resolves runs as Normal, and the log
// should say what was in force rather than what was asked for.
[[nodiscard]] QString CurrentPreset() {
	return Filtering() ? CurrentState().activePreset : QString();
}

[[nodiscard]] QString SnoozeFilePath() {
	return ConfigDirectory() + u"/screentime_snoozes"_q;
}

// Today's snoozes, one line each: day, budget index, how many have been taken,
// and when the last one runs out.
//
// Beside the log rather than in state.toml on purpose: state.toml's schema is
// the core's, shared with Android, and a desktop cover's snooze count is not a
// fact about Work Mode - it is this client's bookkeeping about one afternoon.
struct Snooze {
	QDate day;
	int index = 0;
	int count = 0;
	int64 untilMs = 0;
};

[[nodiscard]] std::vector<Snooze> ReadSnoozes() {
	auto file = QFile(SnoozeFilePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	const auto text = QString::fromUtf8(file.readAll());
	const auto today = QDate::currentDate();
	auto result = std::vector<Snooze>();
	for (const auto &line : text.split('\n', Qt::SkipEmptyParts)) {
		const auto parts = line.split('\t');
		if (parts.size() != 4) {
			continue;
		}
		auto entry = Snooze();
		entry.day = QDate::fromString(parts[0], Qt::ISODate);
		// Yesterday's snoozes are not today's, and the file is rewritten
		// whole, so dropping them on the way in is also what clears them out.
		if (entry.day != today) {
			continue;
		}
		entry.index = parts[1].toInt();
		entry.count = parts[2].toInt();
		entry.untilMs = parts[3].toLongLong();
		result.push_back(entry);
	}
	return result;
}

void WriteSnoozes(const std::vector<Snooze> &list) {
	auto text = QString();
	for (const auto &entry : list) {
		text += entry.day.toString(Qt::ISODate)
			+ '\t' + QString::number(entry.index)
			+ '\t' + QString::number(entry.count)
			+ '\t' + QString::number(entry.untilMs)
			+ '\n';
	}
	if (!QDir().mkpath(ConfigDirectory())) {
		LOG(("Purple Error: Could not create %1.").arg(ConfigDirectory()));
		return;
	}
	WriteConfigFile(SnoozeFilePath(), text);
}

[[nodiscard]] QString NoticeFilePath() {
	return ConfigDirectory() + u"/screentime_notices"_q;
}

// Today's soft budget bulletins, one line each: day, budget index, and the
// peer the bulletin was said in. Kept beside the log for the same reason the
// snoozes above are, and for the reason spelled out there.
struct Notice {
	QDate day;
	int index = 0;
	uint64 peerId = 0;
};

[[nodiscard]] std::vector<Notice> ReadNotices() {
	auto file = QFile(NoticeFilePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	const auto text = QString::fromUtf8(file.readAll());
	const auto today = QDate::currentDate();
	auto result = std::vector<Notice>();
	for (const auto &line : text.split('\n', Qt::SkipEmptyParts)) {
		const auto parts = line.split('\t');
		if (parts.size() != 3) {
			continue;
		}
		auto entry = Notice();
		entry.day = QDate::fromString(parts[0], Qt::ISODate);
		// Yesterday's bulletins are not today's, and the file is rewritten
		// whole, so dropping them on the way in is also what clears them out.
		if (entry.day != today) {
			continue;
		}
		entry.index = parts[1].toInt();
		entry.peerId = parts[2].toULongLong();
		result.push_back(entry);
	}
	return result;
}

void WriteNotices(const std::vector<Notice> &list) {
	auto text = QString();
	for (const auto &entry : list) {
		text += entry.day.toString(Qt::ISODate)
			+ '\t' + QString::number(entry.index)
			+ '\t' + QString::number(entry.peerId)
			+ '\n';
	}
	if (!QDir().mkpath(ConfigDirectory())) {
		LOG(("Purple Error: Could not create %1.").arg(ConfigDirectory()));
		return;
	}
	WriteConfigFile(NoticeFilePath(), text);
}

class Recorder final {
public:
	Recorder();

	void watch(not_null<Window::SessionController*> controller);
	void noteAction(PeerData *peer, const QString &action);

	[[nodiscard]] std::vector<Event> events();

private:
	void ensureCache();
	void append(Event event);
	void append(EventKind kind);
	void flush();
	void check();
	void prune();

	void refreshEnabled();
	void checkPreset();
	void setForeground(bool value);
	void setActiveChat(Dialogs::Key key);

	void openSession();
	void closeSession();

	base::Timer _flushTimer;
	base::Timer _watchdogTimer;
	base::Timer _pruneTimer;

	std::vector<Event> _pending;

	// The file as it was last read, so the budget cover asking every half
	// minute does not re-parse ninety days of log to answer. Kept in step by
	// hand rather than dropped on every write: this process is the only thing
	// that appends to the file, so what it just wrote is exactly what the
	// cache is missing.
	std::vector<Event> _cache;
	bool _cacheValid = false;

	bool _enabled = false;
	bool _foreground = true;

	// Whether an Open is standing in the log without its Close. Everything
	// else here is what that Open said, kept so the Close and the Preset cut
	// can say the same.
	bool _open = false;
	PeerIdValue _dialogId = 0;
	ScreenTimeKind _kind = ScreenTimeKind::Elsewhere;
	bool _hidden = false;

	QString _preset;
	bool _idle = false;

	// When the last "typing" went in, for the throttle. Monotonic rather than
	// wall clock: it is a duration between two keystrokes and nothing else
	// reads it.
	crl::time _lastTyping = 0;

	rpl::lifetime _lifetime;

};

Recorder::Recorder() {
	_flushTimer.setCallback([=] { flush(); });
	_watchdogTimer.setCallback([=] { check(); });
	_pruneTimer.setCallback([=] { prune(); });

	// The file is where the switch lives, so a reload is how the feature is
	// turned on and off. Turning it off closes whatever was open first, so the
	// log never ends on a session that has no end.
	SettingsChanges(
	) | rpl::on_next([=] {
		refreshEnabled();
	}, _lifetime);

	// Fires with the current value, so this is also how _foreground starts out
	// right rather than assuming the app launched in front.
	Core::App().appDeactivatedValue(
	) | rpl::on_next([=](bool deactivated) {
		setForeground(!deactivated);
	}, _lifetime);

	// The preset cuts the session so every second of it has exactly one. Also
	// covers a preset changing under a schedule or a focus mode, which is the
	// case a hook on the picker would have missed.
	ActiveChanges(
	) | rpl::on_next([=] {
		checkPreset();
	}, _lifetime);

	base::qt_signal_producer(
		QCoreApplication::instance(),
		&QCoreApplication::aboutToQuit
	) | rpl::on_next([=] {
		closeSession();
		flush();
	}, _lifetime);

	refreshEnabled();
	_pruneTimer.callEach(kPruneEvery);
}

void Recorder::refreshEnabled() {
	const auto enabled = Config().enabled;
	if (enabled == _enabled) {
		if (_enabled) {
			checkPreset();
		}
		return;
	}
	_enabled = enabled;
	if (_enabled) {
		_preset = CurrentPreset();
		_idle = false;
		_watchdogTimer.callEach(kWatchdogTick);
		prune();
		if (_foreground) {
			// Switched on from the box, so the app is in front - but the same
			// path runs at launch and on every reload, and a log that opened
			// with a Foreground it did not see would count a minimised app.
			append(EventKind::Foreground);
			openSession();
		}
	} else {
		closeSession();
		_watchdogTimer.cancel();
		flush();
	}
}

void Recorder::append(Event event) {
	if (!_enabled) {
		return;
	}
	if (!event.unixMs) {
		event.unixMs = NowMs();
	}
	_pending.push_back(std::move(event));
	if (!_flushTimer.isActive()) {
		_flushTimer.callOnce(kFlushDelay);
	}
}

// The shorthand for the events that are about the app rather than about a
// chat: they carry the running preset and nothing else, because the core works
// the rest out from the Open they follow.
void Recorder::append(EventKind kind) {
	append(Event{ .kind = kind, .preset = _preset });
}

void Recorder::openSession() {
	if (!_enabled || !_foreground || _open) {
		return;
	}
	append(Event{
		.kind = EventKind::Open,
		.dialogId = _dialogId,
		.chatKind = _kind,
		.preset = _preset,
		.hidden = _hidden,
	});
	_open = true;
	_idle = false;
}

void Recorder::closeSession() {
	if (!_open) {
		return;
	}
	append(Event{
		.kind = EventKind::Close,
		.dialogId = _dialogId,
		.chatKind = _kind,
		.preset = _preset,
	});
	_open = false;
	_idle = false;
}

void Recorder::setForeground(bool value) {
	if (value == _foreground) {
		return;
	}
	_foreground = value;
	if (!_enabled) {
		return;
	}
	if (value) {
		append(EventKind::Foreground);
		openSession();
	} else {
		// Background ends the session by itself in the core - a chat you
		// cannot see is not screen time - so there is no Close to write here,
		// and writing one would only claim the session ended twice.
		_open = false;
		_idle = false;
		append(EventKind::Background);

		// The one moment the buffer is worth emptying early: the app going
		// away is also how it usually ends, and a machine that sleeps between
		// here and the next tick would lose the last five seconds of the day.
		flush();
	}
}

void Recorder::setActiveChat(Dialogs::Key key) {
	const auto history = key.owningHistory();
	const auto peer = key.peer();
	const auto id = peer ? IdOf(peer) : PeerIdValue(0);
	const auto kind = peer
		? ScreenTimeKindFor(KindOf(peer))
		: ScreenTimeKind::Elsewhere;

	// Whether this chat is one the running preset hides, which is what makes
	// "time in hidden chats while peeking" answerable. purpleHiddenByPreset()
	// is the preset's verdict with the peek taken out, which is exactly the
	// question - during a peek nothing is hidden, and the number wanted here
	// is what would have been.
	const auto hidden = history
		&& Filtering()
		&& history->purpleHiddenByPreset();

	if (id == _dialogId && kind == _kind && hidden == _hidden && _open) {
		return;
	}
	closeSession();
	_dialogId = id;
	_kind = kind;
	_hidden = hidden;
	openSession();
}

void Recorder::checkPreset() {
	const auto preset = CurrentPreset();
	if (preset == _preset) {
		return;
	}
	_preset = preset;
	if (!_enabled) {
		return;
	}
	append(EventKind::Preset);
}

void Recorder::noteAction(PeerData *peer, const QString &action) {
	if (!_enabled || !peer) {
		return;
	}
	if (action == u"typing"_q) {
		// One event per action_span. The field fires per keystroke and the
		// core already reads one Action as a span of activity, so anything
		// finer would be a hundred lines describing the three seconds the
		// first line already describes.
		const auto span = std::max(Config().actionSpanSeconds, 1);
		const auto now = crl::now();
		if (_lastTyping && (now - _lastTyping) < crl::time(span * 1000)) {
			return;
		}
		_lastTyping = now;
	}
	if (_idle) {
		// A send is input, and the watchdog would only notice on its next
		// tick. Saying so here keeps the pause ending where it really ended.
		append(EventKind::Resume);
		_idle = false;
	}
	append(Event{
		.kind = EventKind::Action,
		.dialogId = IdOf(peer),
		.chatKind = ScreenTimeKindFor(KindOf(peer)),
		.preset = _preset,
		.action = action,
	});
}

void Recorder::check() {
	if (!_enabled || !_open || !_foreground) {
		return;
	}
	const auto after = Config().idleAfterSeconds;
	if (after <= 0) {
		return;
	}
	const auto threshold = crl::time(after) * 1000;
	const auto quiet = crl::now() - Core::App().lastNonIdleTime();
	if (!_idle && quiet >= threshold) {
		// The core rewinds an Idle by idle_after to find where the pause
		// began, so the event is stamped at "input stopped, plus the
		// threshold" rather than at now. That is what makes a five-second
		// tick produce a pause that starts on the second it really started.
		append(Event{
			.unixMs = NowMs() - int64(quiet - threshold),
			.kind = EventKind::Idle,
			.dialogId = _dialogId,
			.chatKind = _kind,
			.preset = _preset,
		});
		_idle = true;
	} else if (_idle && quiet < threshold) {
		append(Event{
			.unixMs = NowMs() - int64(quiet),
			.kind = EventKind::Resume,
			.dialogId = _dialogId,
			.chatKind = _kind,
			.preset = _preset,
		});
		_idle = false;
	}
}

void Recorder::flush() {
	_flushTimer.cancel();
	if (_pending.empty()) {
		return;
	}
	const auto pending = base::take(_pending);
	if (!QDir().mkpath(ConfigDirectory())) {
		LOG(("Purple Error: Could not create %1.").arg(ConfigDirectory()));
		return;
	}
	auto file = QFile(ScreenTimeLogPath());
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
		LOG(("Purple Error: Could not append to %1."
			).arg(ScreenTimeLogPath()));
		return;
	}
	auto text = QString();
	for (const auto &event : pending) {
		text += FormatEvent(event) + '\n';
	}
	file.write(text.toUtf8());

	if (_cacheValid) {
		_cache.insert(_cache.end(), pending.begin(), pending.end());
	}
}

void Recorder::ensureCache() {
	if (_cacheValid) {
		return;
	}
	auto file = QFile(ScreenTimeLogPath());
	_cache = file.open(QIODevice::ReadOnly)
		? ParseEventLog(QString::fromUtf8(file.readAll()))
		: std::vector<Event>();
	_cacheValid = true;
}

void Recorder::prune() {
	if (!_enabled) {
		return;
	}
	const auto days = Config().retentionDays;
	if (days <= 0) {
		return;
	}
	ensureCache();
	const auto &events = _cache;
	const auto kept = Prune(events, NowMs(), days);
	if (kept.size() == events.size()) {
		// Which is the usual answer, and the reason a daily pass is cheap:
		// nothing is rewritten until something actually ages out.
		return;
	}
	auto text = QString();
	for (const auto &event : kept) {
		text += FormatEvent(event) + '\n';
	}

	// Through the same QSaveFile every other file in the directory is written
	// with: this is the one place the log is not append-only, and a crash in
	// the middle of it must not be able to leave half a history behind.
	if (WriteConfigFile(ScreenTimeLogPath(), text)) {
		_cache = kept;
		_cacheValid = true;
		LOG(("Purple: screen time log pruned to %1 days, %2 events left."
			).arg(days).arg(kept.size()));
	}
}

std::vector<Event> Recorder::events() {
	if (!_enabled) {
		return {};
	}
	ensureCache();
	auto result = _cache;

	// What has not reached disk yet belongs in the answer too: a screen opened
	// a second after a send has to show that send, and the flush is five
	// seconds away.
	result.insert(result.end(), _pending.begin(), _pending.end());
	return result;
}

void Recorder::watch(not_null<Window::SessionController*> controller) {
	controller->activeChatValue(
	) | rpl::on_next([=](Dialogs::Key key) {
		setActiveChat(key);
	}, controller->lifetime());
}

// Never destroyed, for the same reason as the config and schedule singletons:
// it holds rpl subscriptions to things with static storage duration.
[[nodiscard]] Recorder &Instance() {
	static const auto result = new Recorder();
	return *result;
}

} // namespace

QString ScreenTimeLogPath() {
	return ConfigDirectory() + u"/screentime.log"_q;
}

void StartScreenTime() {
	Instance();
}

void WatchScreenTime(not_null<Window::SessionController*> controller) {
	Instance().watch(controller);
}

void NoteScreenTimeAction(PeerData *peer, const QString &action) {
	Instance().noteAction(peer, action);
}

std::vector<Event> ScreenTimeEvents() {
	return Instance().events();
}

int ScreenTimeSnoozesUsed(int budgetIndex) {
	const auto list = ReadSnoozes();
	for (const auto &entry : list) {
		if (entry.index == budgetIndex) {
			return entry.count;
		}
	}
	return 0;
}

int64 ScreenTimeSnoozeUntil(int budgetIndex) {
	const auto list = ReadSnoozes();
	for (const auto &entry : list) {
		if (entry.index == budgetIndex) {
			return entry.untilMs;
		}
	}
	return 0;
}

void NoteScreenTimeSnooze(int budgetIndex) {
	const auto &budgets = Config().budgets;
	if (budgetIndex < 0 || budgetIndex >= int(budgets.size())) {
		return;
	}
	const auto seconds = budgets[budgetIndex].snoozeSeconds;
	auto list = ReadSnoozes();
	const auto until = NowMs() + int64(seconds) * 1000;
	for (auto &entry : list) {
		if (entry.index == budgetIndex) {
			++entry.count;
			entry.untilMs = until;
			WriteSnoozes(list);
			return;
		}
	}
	list.push_back({
		.day = QDate::currentDate(),
		.index = budgetIndex,
		.count = 1,
		.untilMs = until,
	});
	WriteSnoozes(list);
}

bool ScreenTimeNoticeShown(int budgetIndex, uint64 peerId) {
	const auto list = ReadNotices();
	for (const auto &entry : list) {
		if (entry.index == budgetIndex && entry.peerId == peerId) {
			return true;
		}
	}
	return false;
}

void NoteScreenTimeNotice(int budgetIndex, uint64 peerId) {
	auto list = ReadNotices();
	for (const auto &entry : list) {
		if (entry.index == budgetIndex && entry.peerId == peerId) {
			return;
		}
	}
	list.push_back({
		.day = QDate::currentDate(),
		.index = budgetIndex,
		.peerId = peerId,
	});
	WriteNotices(list);
}

} // namespace Purple
