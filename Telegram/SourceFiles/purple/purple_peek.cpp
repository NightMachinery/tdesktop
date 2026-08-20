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
#include "ui/rp_widget.h"
#include "window/window_controller.h"

#include <QtGui/QAction>

namespace Purple {
namespace {

[[nodiscard]] QString Length(int seconds) {
	return (seconds >= 60 && !(seconds % 60))
		? u"%1 min"_q.arg(seconds / 60)
		: u"%1s"_q.arg(seconds);
}

// One action for every window, as Shortcuts::Manager does: two actions holding
// the same sequence make it ambiguous, and Qt then fires neither.
class Hotkey final {
public:
	Hotkey();

	void listen(not_null<Ui::RpWidget*> widget);

private:
	void apply();
	void trigger();

	base::unique_qptr<QAction> _action;
	QString _keys;
	rpl::lifetime _lifetime;

};

Hotkey::Hotkey()
: _action(base::make_unique_q<QAction>()) {
	_action->setShortcutContext(Qt::ApplicationShortcut);

	// Holding the key down would otherwise strobe the chat list.
	_action->setAutoRepeat(false);

	QObject::connect(_action.get(), &QAction::triggered, [=] {
		trigger();
	});

	SettingsChanges(
	) | rpl::on_next([=] {
		apply();
	}, _lifetime);

	apply();
}

void Hotkey::apply() {
	const auto keys = ActiveSettings().peek.hotkey;
	if (keys == _keys) {
		return;
	}
	_keys = keys;

	// PortableText, so "Ctrl+Shift+P" in the file means what it says. On macOS
	// Qt reads "Ctrl" as Command and "Meta" as the physical Control key, which
	// is the same convention tdesktop's own shortcuts file documents.
	const auto sequence = QKeySequence(keys, QKeySequence::PortableText);
	if (sequence.isEmpty() && !keys.isEmpty()) {
		LOG(("Purple Error: peek hotkey '%1' is not a key sequence."
			).arg(keys));
	}
	_action->setShortcut(sequence);
}

void Hotkey::trigger() {
	const auto change = TogglePeek();
	const auto window = Core::App().activeWindow();
	if (!window) {
		return;
	}
	window->showToast(change.refused
		? u"Work Mode is off - nothing is hidden to peek at."_q
		: !change.peeking
		? u"Peek over."_q
		: change.seconds
		? u"Peeking for %1 - everything the preset hides is showing."_q.arg(
			Length(change.seconds))
		: u"Peeking until you press it again."_q);
}

void Hotkey::listen(not_null<Ui::RpWidget*> widget) {
	// The action outlives every window and is never rebuilt, only re-pointed
	// at a new sequence, so there is nothing to unregister when one closes.
	widget->addAction(_action.get());
}

// Never destroyed, for the same reason as the config singleton: it holds an
// rpl subscription to something with static storage duration.
[[nodiscard]] Hotkey &Instance() {
	static const auto result = new Hotkey();
	return *result;
}

} // namespace

void ListenPeekHotkey(not_null<Ui::RpWidget*> widget) {
	Instance().listen(widget);
}

} // namespace Purple
