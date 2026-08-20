/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_focus.h"

#include "purple/purple_config.h"
#include "purple/purple_engine.h"

namespace Purple {
namespace {

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

// Never destroyed, for the same reason as the config singleton: it holds an
// rpl subscription to something with static storage duration.
[[nodiscard]] Runner &Instance() {
	static const auto result = new Runner();
	return *result;
}

} // namespace

void StartFocusSync() {
	Instance();
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
