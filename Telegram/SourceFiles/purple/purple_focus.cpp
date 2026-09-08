/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_focus.h"

#include "base/timer.h"
#include "purple/purple_config.h"
#include "purple/purple_device.h"
#include "purple/purple_engine.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace Purple {
namespace {

// One save can produce several filesystem events, and the app has no reason to
// re-read the file three times for one change of focus.
constexpr auto kDebounce = crl::time(250);

// A backstop under the watch. A file watch that quietly stops working would
// take focus sync with it and nothing would say so; re-reading two kilobytes a
// minute costs nothing beside that.
constexpr auto kPoll = crl::time(60 * 1000);

// Where macOS records the focus modes that are held right now. There is no
// public API for this - the file is undocumented, Apple owns its shape, and a
// point release may change it - so everything below is written to fail loudly
// and to change nothing when it cannot make sense of what it read. Reporting
// "focus is off" on a parse error would end a session that is still running.
[[nodiscard]] QString AssertionsPath() {
#ifdef Q_OS_MAC
	return QDir::homePath() + u"/Library/DoNotDisturb/DB/Assertions.json"_q;
#else // Q_OS_MAC
	return QString();
#endif // !Q_OS_MAC
}

class Runner final {
public:
	Runner();

private:
	void tick();

	// What the schedule wanted at the moment focus took over, or nothing when
	// no session is running - and nothing again after a restart, which is the
	// whole reason it is here rather than in state.toml. It is not a decision,
	// only a note the core reads on the way out of a session, and state.toml's
	// schema is the core's and shared with Android; a key for a note would be a
	// schema change for something that may legitimately be forgotten.
	//
	// Forgetting it costs one restore: the core then puts the pre-focus preset
	// back exactly as this fork did before the rule existed. Android keeps the
	// same note in a preference file of its own, for the same reasons.
	std::optional<QString> _enterTarget;

	rpl::lifetime _lifetime;

};

Runner::Runner() {
	// No clock of its own: the flag is the whole input, and it arrives through
	// state.toml whether an in-app detector wrote it or something outside did.
	rpl::merge(
		SettingsChanges(),
		StateChanges()
	) | rpl::on_next([=] {
		tick();
	}, _lifetime);

	tick();
}

void Runner::tick() {
	// Both halves of the policy are FocusStep() in the core - entering, the
	// four ways of leaving, and the rule that a preset chosen by hand
	// mid-session outlives the session. They were written here and in the
	// Android bridge, and the two had already parted company; see below.
	//
	// The flag goes in as it stands, because on this client the Detector below
	// is what writes it and this pass only acts on it. Android has one caller
	// for both halves and hands in what it just read.
	const auto step = FocusStep(
		ActiveSettings(),
		CurrentState(),
		CurrentState().focusActive,
		_enterTarget,
		QDateTime::currentDateTime(),
		ThisDevice());
	if (!step) {
		return;
	}
	const auto session = (step->state.activeSource == PresetSource::Focus);
	if (step->enterTarget) {
		_enterTarget = step->enterTarget;
	} else if (!session) {
		// The session is over, so the note is not about anything any more.
		// Keeping it would leave a value from one session to be read as the
		// next one's.
		_enterTarget = std::nullopt;
	}

	// The whole state, not field by field: what came back is a copy of the very
	// CurrentState() handed in a line ago, and both run on the UI thread, so
	// there is nothing in between for this to overwrite.
	const auto preset = step->state.activePreset;
	const auto source = step->state.activeSource;
	UpdateState([&](State &state) {
		state = step->state;
	});
	if (step->change == FocusChange::None) {
		// Only the flag moved, which is every change of focus that is not an
		// edge for the policy - focus coming on while sync is off, say. Nothing
		// about the view changed, so there is nothing to say about it, and the
		// write above is what makes the next edge an edge.
		return;
	}
	LOG(("Purple: focus %1 -> '%2' (%3)."
		).arg(FocusChangeName(step->change)
		).arg(preset.isEmpty() ? NormalPreset() : preset
		).arg(PresetSourceName(source)));
}

class Detector final {
public:
	Detector();

private:
	void check();
	[[nodiscard]] std::optional<bool> read(QString &problem) const;

	const QString _path;
	QFileSystemWatcher _watcher;
	base::Timer _debounce;
	base::Timer _poll;
	bool _complained = false;

};

Detector::Detector() : _path(AssertionsPath()) {
	if (_path.isEmpty()) {
		// Nothing to watch anywhere but macOS. focus_active stays whatever it
		// is, which lets something outside the app still drive it.
		return;
	}
	_debounce.setCallback([=] { check(); });
	_poll.setCallback([=] { check(); });

	// The directory, not the file: it is replaced rather than rewritten, and a
	// watch on the file itself would be left pointing at an inode nobody will
	// ever write to again - the same trap settings.toml has.
	_watcher.addPath(QFileInfo(_path).absolutePath());
	QObject::connect(&_watcher, &QFileSystemWatcher::directoryChanged, [=] {
		_debounce.callOnce(kDebounce);
	});

	_poll.callEach(kPoll);
	check();
}

std::optional<bool> Detector::read(QString &problem) const {
	auto file = QFile(_path);
	if (!file.exists()) {
		// No focus mode has ever been set on this machine.
		return false;
	} else if (!file.open(QIODevice::ReadOnly)) {
		// The file mode is ordinary, so this is macOS refusing rather than the
		// filesystem: the focus database is behind Full Disk Access, and a
		// denied read is indistinguishable from any other one from here.
		problem = u"cannot open it (%1) - Full Disk Access for Purple "
			"Telegram is what this usually wants"_q.arg(file.errorString());
		return std::nullopt;
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(file.readAll(), &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		problem = u"it is not a JSON object (%1)"_q.arg(error.errorString());
		return std::nullopt;
	}
	const auto data = document.object().value(u"data"_q).toArray();
	if (data.isEmpty()) {
		return false;
	} else if (!data.first().isObject()) {
		problem = u"'data' no longer holds objects"_q;
		return std::nullopt;
	}
	const auto records = data.first().toObject().value(
		u"storeAssertionRecords"_q);
	if (records.isUndefined() || records.isNull()) {
		// The key is absent until something holds an assertion, which is the
		// ordinary shape of the file with no focus mode on.
		return false;
	} else if (!records.isArray()) {
		problem = u"'storeAssertionRecords' is no longer an array"_q;
		return std::nullopt;
	}
	// Live assertions only: a mode that has ended moves to the invalidation
	// records beside this key, so anything left here is a focus mode that is on.
	return !records.toArray().isEmpty();
}

void Detector::check() {
	auto problem = QString();
	const auto active = read(problem);
	if (!active) {
		if (!_complained) {
			// Once per spell of not understanding it, not once a minute.
			_complained = true;
			LOG(("Purple Error: Focus state unreadable, %1. Holding whatever "
				"it last saw. (%2)"_q).arg(problem, _path));
		}
		return;
	}
	_complained = false;
	if (*active != FocusActive()) {
		LOG(("Purple: focus mode %1.").arg(*active ? u"on"_q : u"off"_q));
		SetFocusActive(*active);
	}
}

// Never destroyed, for the same reason as the config singleton: they hold rpl
// subscriptions and a file watch on things with static storage duration.
[[nodiscard]] Runner &Instance() {
	static const auto result = new Runner();
	return *result;
}

} // namespace

void StartFocusSync() {
	// The policy first, so the detector's opening read arrives at something
	// already listening rather than only landing in state.
	Instance();

	static const auto detector = new Detector();
	(void)detector;
}

bool FocusActive() {
	return CurrentState().focusActive;
}

void SetFocusActive(bool active) {
	UpdateState([&](State &state) {
		state.focusActive = active;
	});
}

} // namespace Purple
