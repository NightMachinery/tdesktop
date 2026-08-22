/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_focus.h"

#include "base/timer.h"
#include "purple/purple_config.h"
#include "purple/purple_engine.h"

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
	void enter();
	void leave();

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
	const auto &sync = ActiveSettings().focusSync;
	const auto &state = CurrentState();
	const auto imposed = (state.activeSource == PresetSource::Focus);
	if (!sync.enabled) {
		// Switching focus sync off while it is holding a preset has to hand
		// that preset back. Leaving it in force would be a preset nothing on
		// screen explains and nothing left running would ever lift.
		if (imposed) {
			leave();
		} else if (state.focusSeen) {
			UpdateState([](State &state) {
				state.focusSeen = false;
			});
		}
		return;
	} else if (state.focusActive == state.focusSeen) {
		// No edge, so nothing happens - which is exactly what makes a preset
		// chosen by hand mid-session stand until focus itself changes.
		return;
	} else if (state.focusActive) {
		enter();
	} else {
		leave();
	}
}

void Runner::enter() {
	const auto &state = CurrentState();
	const auto from = state.activePreset;

	// Focus cannot be what we remember returning to, or a hand-edited state
	// file could leave the two pointing at each other.
	const auto fromSource = (state.activeSource == PresetSource::Focus)
		? PresetSource::Manual
		: state.activeSource;
	const auto preset = ActiveSettings().focusSync.enterPreset;
	UpdateState([&](State &state) {
		state.focusSeen = true;
		state.previousPreset = from;
		state.previousSource = fromSource;
		state.activePreset = preset;
		state.activeSource = PresetSource::Focus;
	});
	LOG(("Purple: focus on, '%1' -> '%2'.").arg(from, preset));
}

void Runner::leave() {
	const auto &sync = ActiveSettings().focusSync;
	const auto &state = CurrentState();
	if (state.activeSource != PresetSource::Focus) {
		// The preset in force is not the one focus imposed: it was chosen
		// while focus was on, and that choice outlives the focus session.
		UpdateState([](State &state) {
			state.focusSeen = false;
		});
		LOG(("Purple: focus off, keeping '%1' (%2)."
			).arg(state.activePreset
			).arg(PresetSourceName(state.activeSource)));
		return;
	}
	const auto restore = IsPreviousPresetName(sync.exitPreset);
	const auto previous = state.previousPreset;
	const auto wanted = restore ? previous : sync.exitPreset;

	// Restoring puts back the reason as well as the preset, so a window the
	// schedule had opened still closes at its own boundary afterwards. A preset
	// named outright was not put there by either, so it is the user's until
	// something moves it.
	const auto source = restore ? state.previousSource : PresetSource::Manual;
	const auto preset = wanted.isEmpty() ? NormalPreset() : wanted;
	UpdateState([&](State &state) {
		state.focusSeen = false;
		state.activePreset = preset;
		state.activeSource = source;
		state.previousPreset = QString();
		state.previousSource = PresetSource::Manual;
	});
	LOG(("Purple: focus off, -> '%1' (%2).").arg(preset, PresetSourceName(source)));
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
