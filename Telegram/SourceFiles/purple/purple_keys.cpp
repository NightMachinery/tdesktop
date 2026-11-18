#include "purple/purple_keys.h"

#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/shortcuts.h"
#include "dialogs/dialogs_inner_widget.h"
#include "dialogs/dialogs_widget.h"
#include "history/history_widget.h"
#include "history/view/history_view_chat_section.h"
#include "history/view/history_view_pinned_section.h"
#include "history/view/history_view_scheduled_section.h"
#include "lang/lang_keys.h"
#include "mainwindow.h"
#include "purple/purple_chat_background.h"
#include "purple/purple_message_hints.h"
#include "settings/settings_builder.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/layer_widget.h"
#include "ui/toast/toast.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/checkbox.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QTextEdit>

#include <initializer_list>

namespace Purple {
namespace {

constexpr auto kEnabledPref = "purple-modal-keys-enabled";
constexpr auto kWhichKeyDelay = 300;
#ifdef Q_OS_MAC
constexpr auto kCtrl = Qt::MetaModifier;
#else
constexpr auto kCtrl = Qt::ControlModifier;
#endif // Q_OS_MAC

rpl::event_stream<bool> EnabledChanges;

bool Enabled() {
	return Core::App().settings().readPref<bool>(kEnabledPref);
}

enum class Action {
	Backgrounds,
	PreviousChat,
	NextChat,
	OpenChat,
	PreviousFolder,
	NextFolder,
	SelectMessage,
	CopyMessage,
	Help,
	Run,
	DescribeChord,
	DescribeCommand,
};

struct Command {
	Action action;
	const char *id;
	QString (*label)();
};

const Command kCommands[] = {
	{ Action::Backgrounds, "chat-backgrounds-visibility-toggle",
		[] { return tr::lng_purple_keys_backgrounds(tr::now); } },
	{ Action::PreviousChat, "chat-cursor-previous",
		[] { return tr::lng_purple_keys_chat_previous(tr::now); } },
	{ Action::NextChat, "chat-cursor-next",
		[] { return tr::lng_purple_keys_chat_next(tr::now); } },
	{ Action::OpenChat, "chat-cursor-open",
		[] { return tr::lng_purple_keys_chat_open(tr::now); } },
	{ Action::PreviousFolder, "folder-previous",
		[] { return tr::lng_purple_keys_folder_previous(tr::now); } },
	{ Action::NextFolder, "folder-next",
		[] { return tr::lng_purple_keys_folder_next(tr::now); } },
	{ Action::SelectMessage, "message-hint-select",
		[] { return tr::lng_purple_keys_message_select(tr::now); } },
	{ Action::CopyMessage, "message-hint-copy-text",
		[] { return tr::lng_purple_keys_message_copy(tr::now); } },
	{ Action::Help, "keys-help",
		[] { return tr::lng_purple_keys_help(tr::now); } },
	{ Action::Run, "command-search",
		[] { return tr::lng_purple_keys_run(tr::now); } },
	{ Action::DescribeChord, "key-describe",
		[] { return tr::lng_purple_keys_describe_key(tr::now); } },
	{ Action::DescribeCommand, "command-describe",
		[] { return tr::lng_purple_keys_describe_command(tr::now); } },
};

using Chord = std::vector<int>;

struct Binding {
	Action action;
	std::initializer_list<int> keys;
};

const Binding kBindings[] = {
	{ Action::Backgrounds, { Qt::Key_Space, Qt::Key_C, Qt::Key_B } },
	{ Action::PreviousChat, { Qt::Key_BracketLeft, Qt::Key_C } },
	{ Action::PreviousChat, { Qt::Key_K } },
	{ Action::NextChat, { Qt::Key_BracketRight, Qt::Key_C } },
	{ Action::NextChat, { Qt::Key_J } },
	{ Action::OpenChat, { Qt::Key_Return } },
	{ Action::PreviousFolder, { Qt::Key_BracketLeft, Qt::Key_F } },
	{ Action::NextFolder, { Qt::Key_BracketRight, Qt::Key_F } },
	{ Action::SelectMessage, { Qt::Key_G, Qt::Key_S, Qt::Key_S } },
	{ Action::CopyMessage, { Qt::Key_G, Qt::Key_S, Qt::Key_Y } },
	{ Action::Help, { Qt::Key_Space, Qt::Key_Question } },
	{ Action::Run, { Qt::Key_Space, Qt::Key_Space } },
	{ Action::DescribeChord, { Qt::Key_Space, Qt::Key_H, Qt::Key_K } },
	{ Action::DescribeCommand, { Qt::Key_Space, Qt::Key_H, Qt::Key_F } },
};

QString KeyLabel(int key) {
	if (key == Qt::Key_Space) {
		return u"SPC"_q;
	} else if (key == Qt::Key_Return) {
		return u"Enter"_q;
	} else if (key >= Qt::Key_A && key <= Qt::Key_Z) {
		return QString(QChar(key - Qt::Key_A + 'a'));
	}
	return QKeySequence(key).toString(QKeySequence::NativeText);
}

QString ChordLabel(std::initializer_list<int> keys) {
	auto result = QStringList();
	for (const auto key : keys) {
		result.push_back(KeyLabel(key));
	}
	return result.join(' ');
}

QString BindingsLabel(Action action) {
	auto result = QStringList();
	for (const auto &binding : kBindings) {
		if (binding.action == action) {
			result.push_back(ChordLabel(binding.keys));
		}
	}
	return result.join(u" / "_q);
}

const Command &FindCommand(Action action) {
	const auto command = ranges::find(kCommands, action, &Command::action);
	Assert(command != std::end(kCommands));
	return *command;
}

bool StartsWith(const Binding &binding, const Chord &prefix) {
	return binding.keys.size() >= prefix.size()
		&& std::equal(prefix.begin(), prefix.end(), binding.keys.begin());
}

struct Target {
	QPointer<QWidget> focus;
	Dialogs::InnerWidget *list = nullptr;
	MessageHints::Target messages;
	bool folders = false;
};

Target FindTarget(Window::SessionController *owner, QWidget *focus) {
	const auto forSection = [=](auto *section, bool scheduled = false) {
		const auto list = (section->controller() == owner)
			? section->purpleMessageList(focus)
			: nullptr;
		return list
			? Target{
				.focus = focus,
				.messages = { .section = list, .scheduled = scheduled },
			}
			: Target();
	};
	for (auto widget = focus; widget; widget = widget->parentWidget()) {
		if (const auto dialogs = dynamic_cast<Dialogs::Widget*>(widget)) {
			const auto list = dialogs->purpleKeysList(focus);
			if (!list || dialogs->controller() != owner) {
				return {};
			}
			return {
				.focus = focus,
				.list = list,
				.folders = owner->isPrimary()
					&& !owner->openedFolder().current()
					&& !owner->shownForum().current()
					&& !owner->openedCommunity().current(),
			};
		}
		if (const auto history = dynamic_cast<HistoryWidget*>(widget)) {
			return (history->controller() == owner
					&& history->allowsPurpleKeys(focus))
				? Target{
					.focus = focus,
					.messages = { .main = history->purpleMessageList() },
				}
				: Target();
		}
		if (const auto chat = dynamic_cast<HistoryView::ChatWidget*>(widget)) {
			return forSection(chat);
		}
		if (const auto pinned = dynamic_cast<HistoryView::PinnedWidget*>(widget)) {
			return forSection(pinned);
		}
		if (const auto scheduled = dynamic_cast<HistoryView::ScheduledWidget*>(widget)) {
			return forSection(scheduled, true);
		}
	}
	return {};
}

bool Available(Action action, const Target &target) {
	switch (action) {
	case Action::PreviousChat:
	case Action::NextChat:
	case Action::OpenChat: return target.list != nullptr;
	case Action::PreviousFolder:
	case Action::NextFolder: return target.folders;
	case Action::SelectMessage:
	case Action::CopyMessage: return bool(target.messages);
	default: return true;
	}
}

bool IsSearchableAction(Action action) {
	return action == Action::Backgrounds
		|| action == Action::PreviousChat
		|| action == Action::NextChat
		|| action == Action::OpenChat
		|| action == Action::PreviousFolder
		|| action == Action::NextFolder
		|| action == Action::SelectMessage
		|| action == Action::CopyMessage;
}

QString CommandDetail(const Command &command) {
	return command.label() + u"\n"_q + QString::fromLatin1(command.id)
		+ u"\n"_q + BindingsLabel(command.action);
}

void TextBox(
		not_null<Ui::GenericBox*> box,
		QString title,
		QString text) {
	box->setTitle(rpl::single(std::move(title)));
	box->addRow(object_ptr<Ui::FlatLabel>(box, text, st::boxLabel));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

class PaletteKeys final : public QObject {
public:
	PaletteKeys(QObject *editor, Fn<void(int)> move)
	: QObject(editor)
	, _move(std::move(move)) {
		editor->installEventFilter(this);
	}

private:
	bool eventFilter(QObject *, QEvent *event) override {
		if (event->type() != QEvent::KeyPress) {
			return false;
		}
		const auto key = static_cast<QKeyEvent*>(event);
		if (key->modifiers() != Qt::NoModifier
			|| (key->key() != Qt::Key_Up && key->key() != Qt::Key_Down)) {
			return false;
		}
		_move(key->key() == Qt::Key_Up ? -1 : 1);
		return true;
	}

	Fn<void(int)> _move;
};

void PaletteBox(
		not_null<Ui::GenericBox*> box,
		bool describe,
		std::vector<const Command*> commands,
		Fn<void(Action, QPointer<Ui::GenericBox>)> execute) {
	box->setTitle(describe
		? tr::lng_purple_keys_describe_command()
		: tr::lng_purple_keys_run());
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::SingleLine,
		tr::lng_purple_keys_search()));
	const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		QString(),
		st::boxLabel));
	const auto index = box->lifetime().make_state<int>(0);
	const auto matches = box->lifetime().make_state<
		std::vector<const Command*>>();
	const auto render = [=] {
		auto lines = QStringList();
		for (auto i = 0; i != int(matches->size()); ++i) {
			const auto command = (*matches)[i];
			lines.push_back((i == *index ? u"› "_q : u"  "_q)
				+ command->label() + u"    "_q + BindingsLabel(command->action));
		}
		if (!matches->empty()) {
			lines.push_back(u"\n"_q
				+ QString::fromLatin1((*matches)[*index]->id));
		}
		label->setText(lines.isEmpty()
			? tr::lng_purple_keys_no_matches(tr::now)
			: lines.join('\n'));
	};
	const auto filter = [=] {
		const auto query = field->getLastText().trimmed();
		matches->clear();
		for (const auto command : commands) {
			if (command->label().contains(query, Qt::CaseInsensitive)
				|| QString::fromLatin1(command->id).contains(
					query, Qt::CaseInsensitive)
				|| BindingsLabel(command->action).contains(
					query, Qt::CaseInsensitive)) {
				matches->push_back(command);
			}
		}
		*index = 0;
		render();
	};
	const auto submit = [=] {
		if (matches->empty()) {
			return;
		}
		const auto action = (*matches)[*index]->action;
		if (describe) {
			label->setText(CommandDetail(FindCommand(action)));
		} else {
			execute(action, QPointer<Ui::GenericBox>(box.get()));
		}
	};
	filter();
	field->changes() | rpl::on_next(filter, box->lifetime());
	field->submits() | rpl::on_next([=](Qt::KeyboardModifiers) {
		submit();
	}, box->lifetime());
	new PaletteKeys(field->rawTextEdit().get(), [=](int offset) {
		if (!matches->empty()) {
			*index = std::clamp(*index + offset, 0, int(matches->size()) - 1);
			render();
		}
	});
	box->addButton(describe
		? tr::lng_purple_keys_describe()
		: tr::lng_purple_keys_execute(), submit);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	box->setFocusCallback([=] { field->setFocusFast(); });
}

enum class KeyUse {
	Pass,
	EnterNormal,
	ExitNormal,
	Cancel,
	Capture,
	Hint,
	Ignore,
};

class Keys final : public QObject {
public:
	explicit Keys(not_null<MainWindow*> window);
	bool handle(QObject *receiver, QEvent *event);

private:
	[[nodiscard]] Window::SessionController *owner() const;
	[[nodiscard]] Target currentTarget(QObject *receiver) const;
	[[nodiscard]] KeyUse classify(const QKeyEvent &event, int key) const;
	void reset();
	void invalidate();
	void clearPrefix();
	void showWhichKey();
	void stroke(int key, const Target &target);
	void dispatch(Action action, const Target &target);
	void showPalette(bool describe, const Target &target);

	const not_null<MainWindow*> _window;
	QTimer _timer;
	MessageHints _hints;
	base::weak_ptr<Ui::Toast::Instance> _whichKey;
	Chord _prefix;
	QSet<int> _held;
	bool _normal = false;
	bool _inspect = false;
	uint64 _revision = 0;
	rpl::lifetime _sessionLifetime;
	rpl::lifetime _lifetime;
};

constexpr auto kKeysObjectName = "purple-modal-keys";

Keys::Keys(not_null<MainWindow*> window)
: QObject(window)
, _window(window) {
	setObjectName(QString::fromLatin1(kKeysObjectName));
	_timer.setSingleShot(true);
	QObject::connect(&_timer, &QTimer::timeout, this, [=] { showWhichKey(); });
	QObject::connect(qApp, &QApplication::focusChanged, this, [=] {
		reset();
	});
	QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged,
		this, [=](Qt::ApplicationState state) {
			if (state != Qt::ApplicationActive) {
				_held.clear();
				invalidate();
			}
		});
	Core::App().passcodeLockValue() | rpl::on_next([=](bool) {
		invalidate();
	}, _lifetime);
	EnabledChanges.events() | rpl::on_next([=](bool) {
		invalidate();
	}, _lifetime);
	window->controller().boxShownValue() | rpl::on_next([=](bool) {
		reset();
	}, _lifetime);
	window->controller().sessionControllerValue(
	) | rpl::on_next([=](Window::SessionController *owner) {
		invalidate();
		_sessionLifetime.destroy();
		if (!owner) {
			return;
		}
		rpl::merge(
			owner->activeChatEntryChanges() | rpl::to_empty,
			owner->activeChatsFilter() | rpl::to_empty,
			owner->openedFolder().changes() | rpl::to_empty,
			owner->shownForum().changes() | rpl::to_empty,
			owner->openedCommunity().changes() | rpl::to_empty
		) | rpl::on_next([=] { invalidate(); }, _sessionLifetime);
	}, _lifetime);
}

Window::SessionController *Keys::owner() const {
	const auto &window = _window->controller();
	return (QApplication::activeWindow() == _window
		&& Core::App().activeWindow() == &window
		&& !Core::App().passcodeLocked()
		&& !window.locked()
		&& !QApplication::activePopupWidget()
		&& !QApplication::activeModalWidget())
		? window.sessionController()
		: nullptr;
}

Target Keys::currentTarget(QObject *receiver) const {
	const auto focus = QApplication::focusWidget();
	const auto current = owner();
	return (focus
			&& focus->isVisible()
			&& focus->isEnabled()
			&& focus->window() == _window
			&& receiver == focus
			&& current
			&& !_window->controller().isLayerShown())
		? FindTarget(current, focus)
		: Target();
}

KeyUse Keys::classify(const QKeyEvent &event, int key) const {
	const auto mods = event.modifiers() & ~Qt::KeypadModifier;
	if (key == Qt::Key_BracketLeft && mods == kCtrl) {
		return Shortcuts::KeysCurrents().contains(QKeySequence(event.keyCombination()))
			? KeyUse::Pass
			: KeyUse::EnterNormal;
	} else if (!_normal) {
		return KeyUse::Pass;
	} else if ((key == Qt::Key_Escape && !mods)
		|| (key == Qt::Key_G && mods == kCtrl)) {
		return (_prefix.empty() && !_inspect && !_hints.active())
			? KeyUse::ExitNormal
			: KeyUse::Cancel;
	} else if (_hints.active()) {
		const auto printable = key >= Qt::Key_Space && key < Qt::Key_Escape;
		if (!mods && (printable
				|| key == Qt::Key_Return
				|| key == Qt::Key_Backspace
				|| key == Qt::Key_Delete)) {
			return KeyUse::Hint;
		}
		return ((mods == Qt::NoModifier || mods == Qt::ShiftModifier)
				&& (!event.text().isEmpty()
					|| (key >= Qt::Key_Dead_Grave && key <= Qt::Key_Dead_Greek)))
			? KeyUse::Ignore
			: KeyUse::Pass;
	} else if (key == Qt::Key_I && !mods && _prefix.empty() && !_inspect) {
		return KeyUse::ExitNormal;
	} else if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
		return KeyUse::Pass;
	} else if (!mods || (key == Qt::Key_Question && mods == Qt::ShiftModifier)) {
		const auto nextBinding = ranges::any_of(
			kBindings,
			[&](const Binding &binding) {
				return binding.keys.size() > _prefix.size()
					&& binding.keys.begin()[_prefix.size()] == key
					&& StartsWith(binding, _prefix);
			});
		const auto printable = key >= Qt::Key_Space && key < Qt::Key_Escape;
		if (nextBinding || ((_inspect || !_prefix.empty()) && printable)) {
			return KeyUse::Capture;
		}
	}
	return ((mods == Qt::NoModifier || mods == Qt::ShiftModifier)
			&& (!event.text().isEmpty()
				|| (key >= Qt::Key_Dead_Grave && key <= Qt::Key_Dead_Greek)
				|| key == Qt::Key_Backspace
				|| key == Qt::Key_Delete))
		? KeyUse::Ignore
		: KeyUse::Pass;
}

bool Keys::handle(QObject *receiver, QEvent *event) {
	if (event->type() == QEvent::WindowDeactivate) {
		if (receiver == _window) {
			_held.clear();
			invalidate();
		}
		return false;
	} else if (event->type() == QEvent::InputMethod) {
		reset();
		return false;
	}
	const auto keyEvent = static_cast<QKeyEvent*>(event);
	const auto key = (keyEvent->key() == Qt::Key_Enter)
		? Qt::Key_Return
		: keyEvent->key();
	if (event->type() == QEvent::KeyRelease) {
		return keyEvent->isAutoRepeat() ? _held.contains(key) : _held.remove(key);
	}
	if (keyEvent->isAutoRepeat() && _held.contains(key)) {
		event->accept();
		return true;
	}
	const auto target = currentTarget(receiver);
	if (!target.focus || !Enabled()) {
		if (receiver == QApplication::focusWidget()) {
			reset();
		}
		return false;
	}
	const auto stockLegend = (key == Qt::Key_Question
		&& target.list && _prefix.empty() && !_inspect);
	const auto use = stockLegend ? KeyUse::Pass : classify(*keyEvent, key);
	if (event->type() == QEvent::ShortcutOverride) {
		if (use != KeyUse::Pass) {
			event->accept();
			return true;
		}
		return false;
	}
	if (use == KeyUse::Pass) {
		if (key != Qt::Key_Shift && key != Qt::Key_Control
			&& key != Qt::Key_Meta && key != Qt::Key_Alt
			&& key != Qt::Key_AltGr) {
			clearPrefix();
		}
		return false;
	}
	_held.insert(key);
	if (keyEvent->isAutoRepeat()) {
		return true;
	}
	switch (use) {
	case KeyUse::EnterNormal:
		clearPrefix();
		if (!_normal) {
			_normal = true;
			Ui::Toast::Show(_window, tr::lng_purple_keys_normal(tr::now));
		}
		break;
	case KeyUse::ExitNormal:
		reset();
		Ui::Toast::Show(_window, tr::lng_purple_keys_insert(tr::now));
		break;
	case KeyUse::Cancel:
	case KeyUse::Ignore:
		clearPrefix();
		break;
	case KeyUse::Capture:
		stroke(key, target);
		break;
	case KeyUse::Hint:
		if (_hints.type(key, target.messages) == MessageHints::Input::Unknown) {
			Ui::Toast::Show(_window, tr::lng_purple_keys_undefined(tr::now));
		}
		break;
	case KeyUse::Pass:
		break;
	}
	return true;
}

void Keys::clearPrefix() {
	_hints.clear();
	_prefix.clear();
	_inspect = false;
	_timer.stop();
	if (_whichKey) {
		_whichKey->hide();
		_whichKey = {};
	}
}

void Keys::reset() {
	_normal = false;
	clearPrefix();
}

void Keys::invalidate() {
	++_revision;
	reset();
}

void Keys::showWhichKey() {
	const auto target = currentTarget(QApplication::focusWidget());
	if (!target.focus || !_normal || !Enabled()) {
		reset();
		return;
	}
	auto lines = QStringList();
	auto seen = QSet<int>();
	for (const auto &binding : kBindings) {
		if (!StartsWith(binding, _prefix)
			|| binding.keys.size() == _prefix.size()
			|| (!_inspect && !Available(binding.action, target))) {
			continue;
		}
		const auto next = binding.keys.begin()[_prefix.size()];
		if (seen.contains(next)) {
			continue;
		}
		seen.insert(next);
		const auto label = (binding.keys.size() == _prefix.size() + 1)
			? FindCommand(binding.action).label()
			: (next == Qt::Key_C)
			? tr::lng_purple_keys_group_chat(tr::now)
			: (next == Qt::Key_S)
			? tr::lng_purple_keys_group_messages(tr::now)
			: tr::lng_purple_keys_help(tr::now);
		lines.push_back(KeyLabel(next) + u"    "_q + label);
	}
	if (_inspect && _prefix.empty()) {
		lines = { tr::lng_purple_keys_capture(tr::now) };
	}
	if (!lines.isEmpty()) {
		_whichKey = Ui::Toast::Show(_window, Ui::Toast::Config{
			.text = { lines.join('\n') },
			.attach = RectPart::Bottom,
			.infinite = true,
		});
	}
}

void Keys::stroke(int key, const Target &target) {
	_prefix.push_back(key);
	if (_whichKey) {
		_whichKey->hide();
		_whichKey = {};
	}
	auto more = false;
	for (const auto &binding : kBindings) {
		if (!StartsWith(binding, _prefix)) {
			continue;
		} else if (binding.keys.size() != _prefix.size()) {
			more = true;
			continue;
		}
		const auto inspect = _inspect;
		clearPrefix();
		if (inspect) {
			if (const auto current = owner()) {
				current->show(Box(TextBox,
					tr::lng_purple_keys_describe_key(tr::now),
					CommandDetail(FindCommand(binding.action))));
			}
		} else if (Available(binding.action, target)) {
			dispatch(binding.action, target);
		}
		return;
	}
	if (more) {
		_timer.start(kWhichKeyDelay);
	} else {
		clearPrefix();
		Ui::Toast::Show(_window, tr::lng_purple_keys_undefined(tr::now));
	}
}

void Keys::dispatch(Action action, const Target &target) {
	const auto current = owner();
	if (!current || !Available(action, target)) {
		return;
	}
	switch (action) {
	case Action::Backgrounds:
		ToggleChatBackgroundVisibility(_window);
		break;
	case Action::PreviousChat:
	case Action::NextChat: {
		auto event = QKeyEvent(QEvent::KeyPress,
			action == Action::NextChat ? Qt::Key_Down : Qt::Key_Up,
			Qt::NoModifier);
		target.list->processKeyDispatch(&event);
	} break;
	case Action::OpenChat:
		target.list->chooseRow();
		break;
	case Action::PreviousFolder:
	case Action::NextFolder:
		Window::CheckAndJumpToNearChatsFilter(
			current, action == Action::NextFolder, true);
		break;
	case Action::SelectMessage:
	case Action::CopyMessage: {
		const auto copy = (action == Action::CopyMessage);
		if (_hints.start(target.messages, copy
				? MessageHints::Action::Copy
				: MessageHints::Action::Select)) {
			_normal = true;
		} else {
			Ui::Toast::Show(_window, copy
				? tr::lng_purple_keys_message_copy_empty(tr::now)
				: tr::lng_purple_keys_message_select_empty(tr::now));
		}
	} break;
	case Action::Help: {
		auto lines = QStringList{ tr::lng_purple_keys_settings_hint(tr::now) };
		for (const auto &command : kCommands) {
			lines.push_back(BindingsLabel(command.action)
				+ u"    "_q + command.label());
		}
		current->show(Box(TextBox,
			tr::lng_purple_keys_help(tr::now),
			lines.join('\n')));
	} break;
	case Action::Run:
	case Action::DescribeCommand:
		showPalette(action == Action::DescribeCommand, target);
		break;
	case Action::DescribeChord:
		_inspect = true;
		_timer.start(kWhichKeyDelay);
		break;
	}
}

void Keys::showPalette(bool describe, const Target &target) {
	const auto current = owner();
	if (!current) {
		return;
	}
	auto commands = std::vector<const Command*>();
	for (const auto &command : kCommands) {
		if (describe || (IsSearchableAction(command.action)
				&& Available(command.action, target))) {
			commands.push_back(&command);
		}
	}
	const auto revision = _revision;
	const auto origin = target.focus;
	const auto original = base::make_weak(current);
	current->show(Box(PaletteBox, describe, std::move(commands),
		Fn<void(Action, QPointer<Ui::GenericBox>)>([=](
				Action action,
				QPointer<Ui::GenericBox> box) {
			QTimer::singleShot(0, this, [=] {
				const auto focus = QApplication::focusWidget();
				const auto layer = box ? box->parentWidget() : nullptr;
				const auto stack = layer
					? dynamic_cast<Ui::LayerStackWidget*>(layer->parentWidget())
					: nullptr;
				if (!stack || stack->topShownLayer() != layer
					|| !box->isVisible() || !focus
					|| (focus != layer && !layer->isAncestorOf(focus))
					|| owner() != original.get()) {
					return;
				}
				_window->controller().hideLayer(anim::type::instant);
				const auto current = owner();
				if (!Enabled() || !current || current != original.get()
					|| revision != _revision || !origin
					|| _window->controller().isLayerShown()) {
					return;
				}
				const auto target = FindTarget(current, origin);
				if (target.focus && Available(action, target)) {
					origin->setFocus(Qt::OtherFocusReason);
					const auto restored = currentTarget(origin);
					if (restored.focus && revision == _revision) {
						dispatch(action, restored);
					}
				}
			});
		})));
}

} // namespace

bool HandleModalKeys(QObject *receiver, QEvent *event) {
	if (!receiver->isWidgetType()) {
		return false;
	}
	const auto widget = static_cast<QWidget*>(receiver);
	const auto window = dynamic_cast<MainWindow*>(widget->window());
	if (!window) {
		return false;
	}
	const auto existing = window->findChild<QObject*>(
		QString::fromLatin1(kKeysObjectName),
		Qt::FindDirectChildrenOnly);
	if (existing) {
		return static_cast<Keys*>(existing)->handle(receiver, event);
	}
	if (!Enabled()
		|| (event->type() != QEvent::ShortcutOverride
			&& event->type() != QEvent::KeyPress)) {
		return false;
	}
	const auto key = static_cast<QKeyEvent*>(event);
	if (key->key() != Qt::Key_BracketLeft || key->modifiers() != kCtrl) {
		return false;
	}
	return (new Keys(window))->handle(receiver, event);
}

void BuildModalKeysSetting(::Settings::Builder::SectionBuilder &builder) {
	const auto toggle = builder.addButton({
		.id = u"advanced/purple_modal_keys"_q,
		.title = tr::lng_purple_keys_settings(),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(Enabled()) | rpl::then(EnabledChanges.events()),
		.keywords = { u"keyboard"_q, u"modal"_q, u"evil"_q, u"leader"_q },
	});
	if (toggle) {
		toggle->toggledValue() | rpl::on_next([](bool enabled) {
			if (enabled != Enabled()) {
				Core::App().settings().writePref<bool>(kEnabledPref, enabled);
				EnabledChanges.fire_copy(enabled);
			}
		}, toggle->lifetime());
	}
	builder.addDividerText(tr::lng_purple_keys_settings_hint());
	builder.addSkip();
}

} // namespace Purple
