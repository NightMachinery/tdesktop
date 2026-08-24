/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_peek.h"

#include "base/unique_qptr.h"
#include "core/application.h"
#include "purple/purple_config.h"
#include "purple/purple_gate.h"
#include "purple/purple_state.h"
#include "ui/layers/show.h"
#include "ui/rp_widget.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtGui/QAction>

namespace Purple {
namespace {

[[nodiscard]] QString Length(int seconds) {
	return (seconds >= 60 && !(seconds % 60))
		? u"%1 min"_q.arg(seconds / 60)
		: u"%1s"_q.arg(seconds);
}

// PortableText, so "Ctrl+Shift+P" in the file means what it says. On macOS Qt
// reads "Ctrl" as Command and "Meta" as the physical Control key, which is the
// same convention tdesktop's own shortcuts file documents.
[[nodiscard]] QKeySequence Parse(const QString &keys, const QString &what) {
	const auto sequence = QKeySequence(keys, QKeySequence::PortableText);
	if (sequence.isEmpty() && !keys.isEmpty()) {
		LOG(("Purple Error: %1 hotkey '%2' is not a key sequence."
			).arg(what, keys));
	}
	return sequence;
}

// One action per binding per application, as Shortcuts::Manager does: two
// actions holding the same sequence make it ambiguous, and Qt then fires
// neither. The parser already refuses a duplicate for the same reason; this is
// the half that would break if it did not.
class Hotkeys final {
public:
	Hotkeys();

	void listen(not_null<Ui::RpWidget*> widget);

private:
	struct Binding {
		base::unique_qptr<QAction> action;
		QString preset; // Empty for the peek key.
	};

	void apply();
	[[nodiscard]] Binding make(const QString &keys, const QString &preset);
	void triggerPeek();
	void triggerPreset(const QString &preset);

	std::vector<Binding> _bindings;
	std::vector<not_null<Ui::RpWidget*>> _widgets;
	QString _signature;
	rpl::lifetime _lifetime;

};

Hotkeys::Hotkeys() {
	SettingsChanges(
	) | rpl::on_next([=] {
		apply();
	}, _lifetime);

	apply();
}

Hotkeys::Binding Hotkeys::make(const QString &keys, const QString &preset) {
	auto result = Binding{
		.action = base::make_unique_q<QAction>(),
		.preset = preset,
	};
	const auto action = result.action.get();
	action->setShortcutContext(Qt::ApplicationShortcut);

	// Holding the key down would otherwise strobe the chat list, or flip the
	// preset back and forth once per repeat.
	action->setAutoRepeat(false);
	action->setShortcut(Parse(
		keys,
		preset.isEmpty() ? u"peek"_q : u"preset '%1'"_q.arg(preset)));

	QObject::connect(action, &QAction::triggered, [=] {
		if (preset.isEmpty()) {
			triggerPeek();
		} else {
			triggerPreset(preset);
		}
	});
	return result;
}

void Hotkeys::apply() {
	const auto &settings = ActiveSettings();

	// Rebuilt wholesale rather than diffed, because a preset can be renamed,
	// removed or reordered and the bindings have to follow. The signature is
	// what keeps that from happening on every unrelated save - which matters
	// here more than elsewhere, since re-adding an action to a live window is
	// the part Qt is fussy about.
	auto signature = settings.peek.hotkey;
	for (const auto &preset : settings.presets) {
		if (!preset.hotkey.isEmpty()) {
			signature += '\n' + preset.name + '\t' + preset.hotkey;
		}
	}
	if (signature == _signature && !_bindings.empty()) {
		return;
	}
	_signature = signature;

	_bindings.clear();
	if (!settings.peek.hotkey.isEmpty()) {
		_bindings.push_back(make(settings.peek.hotkey, QString()));
	}
	for (const auto &preset : settings.presets) {
		if (!preset.hotkey.isEmpty()) {
			_bindings.push_back(make(preset.hotkey, preset.name));
		}
	}
	for (const auto &widget : _widgets) {
		for (const auto &binding : _bindings) {
			widget->addAction(binding.action.get());
		}
	}
}

// Purple: Window::Controller::showToast() builds a throwaway Ui::Show for every
// call, and it is the Show that remembers the last toast in order to hide it -
// so toasts raised that way pile up on screen instead of replacing each other.
// A SessionController caches its Show, so going through one gets the
// replacement behaviour that was always intended. There is not always a session
// to go through, hence the fallback.
void ShowToast(not_null<Window::Controller*> window, const QString &text) {
	if (const auto session = window->sessionController()) {
		session->uiShow()->showToast(text);
	} else {
		window->showToast(text);
	}
}

void Hotkeys::triggerPeek() {
	const auto change = TogglePeek();
	const auto window = Core::App().activeWindow();
	if (!window) {
		return;
	}
	ShowToast(window, change.refused
		? u"Work Mode is off - nothing is hidden to peek at."_q
		: !change.peeking
		? u"Peek over."_q
		: change.seconds
		? u"Peeking for %1 - everything the preset hides is showing."_q.arg(
			Length(change.seconds))
		: u"Peeking until you press it again."_q);
}

void Hotkeys::triggerPreset(const QString &preset) {
	// Pressing the key of the preset already running turns it off rather than
	// doing nothing. One key then means both "get to work" and "come back",
	// which is what a single key wants to mean; a key that is a no-op half the
	// time reads as broken.
	const auto active = CurrentState().activePreset;
	const auto wanted = active.compare(preset, Qt::CaseInsensitive)
		? preset
		: NormalPreset();
	UpdateState([&](State &state) {
		state.activePreset = wanted;
		state.activeSource = PresetSource::Manual;
	});
	if (const auto window = Core::App().activeWindow()) {
		const auto settings = &ActiveSettings();
		const auto found = settings->preset(wanted);
		ShowToast(window, found
			? u"Work Mode: %1."_q.arg(PresetTitle(*found))
			: u"Work Mode off - stock Telegram Desktop."_q);
	}
}

void Hotkeys::listen(not_null<Ui::RpWidget*> widget) {
	// The actions outlive every window, so there is nothing to unregister when
	// one closes - but the widget list has to drop it, or a rebuild would add
	// actions to a window that is gone.
	if (!ranges::contains(_widgets, widget)) {
		_widgets.push_back(widget);
		widget->lifetime().add([=] {
			_widgets.erase(
				ranges::remove(_widgets, widget),
				_widgets.end());
		});
	}
	for (const auto &binding : _bindings) {
		widget->addAction(binding.action.get());
	}
}

// Never destroyed, for the same reason as the config singleton: it holds an
// rpl subscription to something with static storage duration.
[[nodiscard]] Hotkeys &Instance() {
	static const auto result = new Hotkeys();
	return *result;
}

} // namespace

void ListenHotkeys(not_null<Ui::RpWidget*> widget) {
	Instance().listen(widget);
}

} // namespace Purple
