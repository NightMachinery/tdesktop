#include "purple/purple_keys.h"

#include "base/event_filter.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/shortcuts.h"
#include "data/data_document.h"
#include "data/data_user.h"
#include "dialogs/dialogs_inner_widget.h"
#include "dialogs/dialogs_widget.h"
#include "history/history_widget.h"
#include "history/view/history_view_chat_section.h"
#include "history/view/history_view_pinned_section.h"
#include "history/view/history_view_scheduled_section.h"
#include "lang/lang_keys.h"
#include "logs.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mainwidget.h"
#include "media/player/media_player_instance.h"
#include "mainwindow.h"
#include "purple/purple_chat_background.h"
#include "purple/purple_config.h"
#include "purple/purple_message_hints.h"
#include "settings/settings_builder.h"
#include "settings/settings_key_navigation.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/layer_widget.h"
#include "ui/screen_reader_mode.h"
#include "ui/toast/toast.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/elastic_scroll.h"
#include "ui/widgets/checkbox.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QTextEdit>

#include <optional>

namespace Purple {
namespace {

constexpr auto kEnabledPref = "purple-modal-keys-enabled";
constexpr auto kWhichKeyDelay = 300;
constexpr auto kPaletteRows = 9;
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
	MessageMenu,
	CopyMessageLink,
	CopyMessageUrl,
	OpenMessageUrl,
	ReplyToMessage,
	EditMessage,
	Search,
	OpenMainMenu,
	OpenSettings,
	OpenSavedMessages,
	OpenArchive,
	OpenContacts,
	OpenScheduled,
	OpenChatMenu,
	PreviewChat,
	TogglePlayback,
	StopPlayback,
	PreviousTrack,
	NextTrack,
	SeekBackward,
	SeekForward,
	Account1,
	Account2,
	Account3,
	Account4,
	Account5,
	Account6,
	FocusChats,
	FocusConversation,
	OlderMessagesPage,
	NewerMessagesPage,
	LatestMessages,
	UnreadMessages,
	OpenHistoryCalendar,
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
	{ Action::Search, "search-messages",
		[] { return tr::lng_shortcuts_search(tr::now); } },
	{ Action::OpenMainMenu, "open-main-menu",
		[] { return tr::lng_main_menu(tr::now); } },
	{ Action::OpenSettings, "open-settings",
		[] { return tr::lng_menu_settings(tr::now); } },
	{ Action::OpenSavedMessages, "open-saved-messages",
		[] { return tr::lng_saved_messages(tr::now); } },
	{ Action::OpenArchive, "open-archived-chats",
		[] { return tr::lng_shortcuts_archive(tr::now); } },
	{ Action::OpenContacts, "open-contacts",
		[] { return tr::lng_shortcuts_contacts(tr::now); } },
	{ Action::OpenScheduled, "open-scheduled-messages",
		[] { return tr::lng_scheduled_messages(tr::now); } },
	{ Action::OpenChatMenu, "open-chat-menu",
		[] { return tr::lng_shortcuts_show_chat_menu(tr::now); } },
	{ Action::PreviewChat, "preview-chat",
		[] { return tr::lng_shortcuts_show_chat_preview(tr::now); } },
	{ Action::MessageMenu, "message-hint-menu",
		[] { return tr::lng_purple_keys_message_menu(tr::now); } },
	{ Action::TogglePlayback, "playback-toggle",
		[] { return tr::lng_shortcuts_media_play_pause(tr::now); } },
	{ Action::StopPlayback, "playback-stop",
		[] { return tr::lng_shortcuts_media_stop(tr::now); } },
	{ Action::PreviousTrack, "playback-previous-track",
		[] { return tr::lng_shortcuts_media_previous(tr::now); } },
	{ Action::NextTrack, "playback-next-track",
		[] { return tr::lng_shortcuts_media_next(tr::now); } },
	{ Action::SeekBackward, "playback-seek-backward",
		[] { return tr::lng_purple_shortcuts_seek_backward(tr::now); } },
	{ Action::SeekForward, "playback-seek-forward",
		[] { return tr::lng_purple_shortcuts_seek_forward(tr::now); } },
	{ Action::Account1, "switch-account-1",
		[] { return tr::lng_shortcuts_show_account_n(
			tr::now, lt_index, QString::number(1)); } },
	{ Action::Account2, "switch-account-2",
		[] { return tr::lng_shortcuts_show_account_n(
			tr::now, lt_index, QString::number(2)); } },
	{ Action::Account3, "switch-account-3",
		[] { return tr::lng_shortcuts_show_account_n(
			tr::now, lt_index, QString::number(3)); } },
	{ Action::Account4, "switch-account-4",
		[] { return tr::lng_shortcuts_show_account_n(
			tr::now, lt_index, QString::number(4)); } },
	{ Action::Account5, "switch-account-5",
		[] { return tr::lng_shortcuts_show_account_n(
			tr::now, lt_index, QString::number(5)); } },
	{ Action::Account6, "switch-account-6",
		[] { return tr::lng_shortcuts_show_account_n(
			tr::now, lt_index, QString::number(6)); } },
	{ Action::FocusChats, "focus-chat-list",
		[] { return tr::lng_purple_keys_focus_chats(tr::now); } },
	{ Action::FocusConversation, "focus-conversation",
		[] { return tr::lng_purple_keys_focus_conversation(tr::now); } },
	{ Action::OlderMessagesPage, "history-page-older",
		[] { return tr::lng_purple_keys_history_page_older(tr::now); } },
	{ Action::NewerMessagesPage, "history-page-newer",
		[] { return tr::lng_purple_keys_history_page_newer(tr::now); } },
	{ Action::LatestMessages, "history-jump-to-latest",
		[] { return tr::lng_jump_to_bottom(tr::now); } },
	{ Action::UnreadMessages, "history-jump-to-unread",
		[] { return tr::lng_unread_bar_some(tr::now); } },
	{ Action::OpenHistoryCalendar, "history-open-calendar",
		[] { return tr::lng_calendar(tr::now); } },
	{ Action::CopyMessageLink, "message-hint-copy-link",
		[] { return tr::lng_purple_keys_message_link(tr::now); } },
	{ Action::CopyMessageUrl, "message-hint-copy-url",
		[] { return tr::lng_context_copy_link(tr::now); } },
	{ Action::OpenMessageUrl, "message-hint-open-url",
		[] { return tr::lng_polls_option_open_link_title(tr::now); } },
	{ Action::ReplyToMessage, "message-hint-reply",
		[] { return tr::lng_context_reply_msg(tr::now); } },
	{ Action::EditMessage, "message-hint-edit",
		[] { return tr::lng_context_edit_msg(tr::now); } },
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
	Chord keys;
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
	{ Action::MessageMenu, { Qt::Key_G, Qt::Key_S, Qt::Key_M } },
	{ Action::CopyMessageLink, { Qt::Key_G, Qt::Key_S, Qt::Key_L } },
	{ Action::CopyMessageUrl, { Qt::Key_G, Qt::Key_S, Qt::Key_U } },
	{ Action::OpenMessageUrl, { Qt::Key_G, Qt::Key_S, Qt::Key_O } },
	{ Action::ReplyToMessage, { Qt::Key_G, Qt::Key_S, Qt::Key_R } },
	{ Action::EditMessage, { Qt::Key_G, Qt::Key_S, Qt::Key_E } },
	{ Action::Search, { Qt::Key_Slash } },
	{ Action::Search, { Qt::Key_Space, Qt::Key_S, Qt::Key_S } },
	{ Action::OpenMainMenu, { Qt::Key_Space, Qt::Key_O, Qt::Key_M } },
	{ Action::OpenSettings, { Qt::Key_Space, Qt::Key_O, Qt::Key_T } },
	{ Action::OpenSavedMessages, { Qt::Key_Space, Qt::Key_O, Qt::Key_S } },
	{ Action::OpenArchive, { Qt::Key_Space, Qt::Key_O, Qt::Key_A } },
	{ Action::OpenContacts, { Qt::Key_Space, Qt::Key_O, Qt::Key_C } },
	{ Action::OpenScheduled, { Qt::Key_Space, Qt::Key_O, Qt::Key_D } },
	{ Action::OpenChatMenu, { Qt::Key_Space, Qt::Key_C, Qt::Key_M } },
	{ Action::PreviewChat, { Qt::Key_Space, Qt::Key_C, Qt::Key_P } },
	{ Action::TogglePlayback, { Qt::Key_Space, Qt::Key_M, Qt::Key_P } },
	{ Action::StopPlayback, { Qt::Key_Space, Qt::Key_M, Qt::Key_S } },
	{ Action::PreviousTrack, { Qt::Key_BracketLeft, Qt::Key_T } },
	{ Action::NextTrack, { Qt::Key_BracketRight, Qt::Key_T } },
	{ Action::SeekBackward, { Qt::Key_Space, Qt::Key_M, Qt::Key_B } },
	{ Action::SeekForward, { Qt::Key_Space, Qt::Key_M, Qt::Key_F } },
	{ Action::Account1, { Qt::Key_Space, Qt::Key_A, Qt::Key_1 } },
	{ Action::Account2, { Qt::Key_Space, Qt::Key_A, Qt::Key_2 } },
	{ Action::Account3, { Qt::Key_Space, Qt::Key_A, Qt::Key_3 } },
	{ Action::Account4, { Qt::Key_Space, Qt::Key_A, Qt::Key_4 } },
	{ Action::Account5, { Qt::Key_Space, Qt::Key_A, Qt::Key_5 } },
	{ Action::Account6, { Qt::Key_Space, Qt::Key_A, Qt::Key_6 } },
	{ Action::FocusChats, { Qt::Key_Space, Qt::Key_W, Qt::Key_D } },
	{ Action::FocusConversation, { Qt::Key_Space, Qt::Key_W, Qt::Key_C } },
	{ Action::OlderMessagesPage, { Qt::Key_BracketLeft, Qt::Key_M } },
	{ Action::NewerMessagesPage, { Qt::Key_BracketRight, Qt::Key_M } },
	{ Action::LatestMessages, { Qt::Key_BracketRight, Qt::Key_BracketRight } },
	{ Action::UnreadMessages, { Qt::Key_BracketRight, Qt::Key_U } },
	{ Action::OpenHistoryCalendar, { Qt::Key_Space, Qt::Key_C, Qt::Key_D } },
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

QString ChordLabel(const Chord &keys) {
	auto result = QStringList();
	for (const auto key : keys) {
		result.push_back(KeyLabel(key));
	}
	return result.join(' ');
}

const std::vector<Binding> &Bindings();

QString BindingsLabel(Action action) {
	auto result = QStringList();
	for (const auto &binding : Bindings()) {
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

int ParseKey(const QString &token) {
	if (token.compare(u"SPC"_q, Qt::CaseInsensitive) == 0) {
		return Qt::Key_Space;
	} else if (token.compare(u"Enter"_q, Qt::CaseInsensitive) == 0) {
		return Qt::Key_Return;
	} else if (token.size() != 1) {
		return 0;
	}
	const auto key = token.front().unicode();
	if (key >= 'a' && key <= 'z') {
		return Qt::Key_A + (key - 'a');
	} else if ((key >= 'A' && key <= 'Z')
		|| (key >= '0' && key <= '9')
		|| key == '[' || key == ']' || key == '/' || key == '?') {
		return key;
	}
	return 0;
}

Chord ParseChord(const QString &text) {
	const auto tokens = text.simplified().split(' ', Qt::SkipEmptyParts);
	if (tokens.empty() || tokens.size() > 4) {
		return {};
	}
	auto result = Chord();
	for (const auto &token : tokens) {
		const auto key = ParseKey(token);
		if (!key) {
			return {};
		}
		result.push_back(key);
	}
	if ((result.front() == Qt::Key_I)
		|| result.front() == Qt::Key_Question) {
		return {};
	}
	return result;
}

bool IsHelpBinding(Action action) {
	return action == Action::Help || action == Action::Run
		|| action == Action::DescribeChord || action == Action::DescribeCommand;
}

std::optional<std::vector<Binding>> ParseBindings(
		const QByteArray &bytes,
		std::vector<Binding> result) {
	const auto document = QJsonDocument::fromJson(bytes);
	constexpr auto kEntryLimit = 128;
	const auto array = document.array();
	if (!document.isArray() || array.size() > kEntryLimit) {
		return std::nullopt;
	}
	auto edited = std::vector<Chord>();
	for (const auto value : array) {
		const auto entry = value.toObject();
		const auto text = entry.value(u"keys"_q);
		const auto id = entry.value(u"command"_q);
		if (entry.size() != 2 || !text.isString()
			|| (!id.isNull() && !id.isString())) {
			return std::nullopt;
		}
		auto keys = ParseChord(text.toString());
		if (keys.empty() || ranges::contains(edited, keys)) {
			return std::nullopt;
		}
		edited.push_back(keys);
		const auto current = ranges::find(result, keys, &Binding::keys);
		if (current != end(result) && IsHelpBinding(current->action)) {
			return std::nullopt;
		}
		if (id.isNull()) {
			if (current != end(result)) {
				result.erase(current);
			}
			continue;
		}
		const auto name = id.toString();
		const auto command = ranges::find_if(kCommands, [&](const Command &item) {
			return name == QString::fromLatin1(item.id);
		});
		if (command == std::end(kCommands)) {
			return std::nullopt;
		} else if (current != end(result)) {
			current->action = command->action;
		} else {
			result.push_back({ command->action, std::move(keys) });
		}
	}
	for (const auto &binding : result) {
		if (ranges::any_of(result, [&](const Binding &other) {
			return binding.keys.size() < other.keys.size()
				&& StartsWith(other, binding.keys);
		})) {
			return std::nullopt;
		}
	}
	return result;
}

const std::vector<Binding> &Bindings() {
	static const auto result = [] {
		auto defaults = std::vector<Binding>(
			std::begin(kBindings),
			std::end(kBindings));
		const auto path = ConfigDirectory() + u"/keys.json"_q;
		const auto info = QFileInfo(path);
		if (!info.exists()) {
			return defaults;
		}
		auto file = QFile(path);
		constexpr auto kByteLimit = 16 * 1024;
		if (info.isFile() && file.open(QIODevice::ReadOnly)) {
			const auto bytes = file.read(kByteLimit + 1);
			if (file.error() == QFileDevice::NoError
				&& bytes.size() <= kByteLimit) {
				if (auto parsed = ParseBindings(bytes, defaults)) {
					return std::move(*parsed);
				}
			}
		}
		LOG(("Purple modal keys: invalid keys.json, using default bindings."));
		return defaults;
	}();
	return result;
}

struct Target {
	QPointer<QWidget> focus;
	Dialogs::InnerWidget *list = nullptr;
	HistoryWidget *history = nullptr;
	MessageHints::Target messages;
	Fn<void(Data::MessagePosition)> jumpInSection;
	Dialogs::Key calendarChat;
	bool folders = false;
	bool chatActions = false;
	Window::SessionController *controller = nullptr;
};

Target FindTarget(Window::SessionController *owner, QWidget *focus) {
	const auto forSection = [=](
			auto *section,
			bool scheduled = false,
			bool chatActions = false) {
		const auto list = (section->controller() == owner)
			? section->purpleMessageList(focus)
			: nullptr;
		return list
			? Target{
				.focus = focus,
				.messages = { .section = list, .scheduled = scheduled },
				.jumpInSection = [section](Data::MessagePosition position) {
					section->cornerButtonsShowAtPosition(position);
				},
				.chatActions = chatActions,
				.controller = owner,
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
				.controller = owner,
			};
		}
		if (const auto history = dynamic_cast<HistoryWidget*>(widget)) {
			return (history->controller() == owner
					&& history->allowsPurpleKeys(focus))
				? Target{
					.focus = focus,
					.history = history,
					.messages = { .main = history->purpleMessageList() },
					.calendarChat = Dialogs::Key(history->history()),
					.chatActions = true,
					.controller = owner,
				}
				: Target();
		}
		if (const auto chat = dynamic_cast<HistoryView::ChatWidget*>(widget)) {
			auto target = forSection(chat, false, true);
			if (target.focus) {
				const auto key = chat->activeChat().key;
				const auto id = chat->id();
				if (!id.sublist && (!id.repliesRootId || key.topic())) {
					target.calendarChat = key;
				}
			}
			return target;
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

bool HasPlaybackInSession(Main::Session *session) {
	if (!session) {
		return false;
	}
	const auto player = Media::Player::instance();
	const auto document = player->current(player->getActiveType()).audio();
	return document && (&document->session() == session);
}

bool CanSwitchAccount(Window::SessionController *controller, int index) {
	if (!controller) {
		return false;
	}
	const auto accounts = Core::App().domain().orderedAccounts();
	return index < int(accounts.size())
		&& accounts[index]->sessionExists()
		&& accounts[index].get() != &controller->session().account();
}

Dialogs::Widget *ChatsPane(Window::SessionController *controller) {
	if (!controller) {
		return nullptr;
	}
	for (const auto child : controller->content()->children()) {
		const auto dialogs = dynamic_cast<Dialogs::Widget*>(child);
		if (dialogs && dialogs->controller() == controller
			&& dialogs->isEnabled() && dialogs->purpleKeysList(dialogs)) {
			return dialogs;
		}
	}
	return nullptr;
}

Ui::ElasticScroll *HistoryScroll(const Target &target) {
	const auto list = target.messages.widget();
	const auto scroll = list
		? dynamic_cast<Ui::ElasticScroll*>(list->parentWidget())
		: nullptr;
	return (scroll && scroll->widget() == list
		&& scroll->isVisible() && scroll->isEnabled())
		? scroll
		: nullptr;
}

bool Available(Action action, const Target &target) {
	switch (action) {
	case Action::PreviousChat:
	case Action::NextChat:
	case Action::PreviewChat:
	case Action::OpenChat: return target.list != nullptr;
	case Action::PreviousFolder:
	case Action::NextFolder: return target.folders;
	case Action::SelectMessage:
	case Action::CopyMessage:
	case Action::CopyMessageUrl:
	case Action::OpenMessageUrl:
	case Action::MessageMenu: return bool(target.messages);
	case Action::CopyMessageLink:
		return target.messages && !target.messages.scheduled;
	case Action::ReplyToMessage:
		return target.messages.supportsCompose(MessageHints::Action::Reply);
	case Action::EditMessage:
		return target.messages.supportsCompose(MessageHints::Action::Edit);
	case Action::OlderMessagesPage:
	case Action::NewerMessagesPage:
	case Action::LatestMessages: return HistoryScroll(target) != nullptr;
	case Action::UnreadMessages: return target.chatActions && HistoryScroll(target);
	case Action::OpenHistoryCalendar: return bool(target.calendarChat);
	case Action::Search: return !target.messages.scheduled;
	case Action::OpenScheduled: return target.chatActions;
	case Action::OpenChatMenu: return target.list || target.chatActions;
	case Action::TogglePlayback:
	case Action::StopPlayback:
	case Action::PreviousTrack:
	case Action::NextTrack:
	case Action::SeekBackward:
	case Action::SeekForward: return target.controller
		&& HasPlaybackInSession(&target.controller->session());
	case Action::Account1:
	case Action::Account2:
	case Action::Account3:
	case Action::Account4:
	case Action::Account5:
	case Action::Account6: return CanSwitchAccount(
		target.controller, int(action) - int(Action::Account1));
	case Action::FocusChats: return !target.list && ChatsPane(target.controller);
	case Action::FocusConversation:
		return target.list && target.controller
			&& !target.controller->adaptive().isOneColumn()
			&& target.controller->mainSectionShown()
			&& target.controller->activeChatCurrent().thread();
	default: return true;
	}
}

bool IsSearchableAction(Action action) {
	return action != Action::Help
		&& action != Action::Run
		&& action != Action::DescribeChord
		&& action != Action::DescribeCommand;
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
		const auto count = int(matches->size());
		const auto first = std::clamp(
			*index - kPaletteRows / 2, 0, std::max(count - kPaletteRows, 0));
		const auto till = std::min(first + kPaletteRows, count);
		if (first > 0) {
			lines.push_back(u"↑"_q);
		}
		for (auto i = first; i != till; ++i) {
			const auto command = (*matches)[i];
			lines.push_back((i == *index ? u"› "_q : u"  "_q)
				+ command->label() + u"    "_q + BindingsLabel(command->action));
		}
		if (till < count) {
			lines.push_back(u"↓"_q);
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
	[[nodiscard]] bool ownsChatListFocus() const;

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

Keys *FindKeys(MainWindow *window) {
	return static_cast<Keys*>(window->findChild<QObject*>(
		QString::fromLatin1(kKeysObjectName),
		Qt::FindDirectChildrenOnly));
}

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

bool Keys::ownsChatListFocus() const {
	return _normal
		&& Enabled()
		&& currentTarget(QApplication::focusWidget()).list;
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
	} else if (!mods || ((key == Qt::Key_Question || key == Qt::Key_Slash)
			&& mods == Qt::ShiftModifier)) {
		const auto nextBinding = ranges::any_of(
			Bindings(),
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
	case KeyUse::Hint: {
		const auto guard = QPointer(this);
		const auto result = _hints.type(key, target.messages);
		if (!guard) {
			break;
		} else if (result == MessageHints::Input::LeaveNormal) {
			reset();
		} else if (result == MessageHints::Input::Unknown) {
			Ui::Toast::Show(_window, tr::lng_purple_keys_undefined(tr::now));
		}
	} break;
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
	for (const auto &binding : Bindings()) {
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
			: (next == Qt::Key_O)
			? tr::lng_purple_keys_group_open(tr::now)
			: (next == Qt::Key_A)
			? tr::lng_purple_keys_group_accounts(tr::now)
			: (next == Qt::Key_W)
			? tr::lng_mac_menu_window(tr::now)
			: (next == Qt::Key_M)
			? tr::lng_media_type_media(tr::now)
			: (next == Qt::Key_S && _prefix == Chord{ Qt::Key_Space })
			? tr::lng_shortcuts_search(tr::now)
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
	for (const auto &binding : Bindings()) {
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
		} else {
			Ui::Toast::Show(_window,
				tr::lng_purple_keys_action_unavailable(tr::now));
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
	if (!current || !target.focus || !Available(action, target)) {
		return;
	}
	using Native = Shortcuts::Command;
	const auto launch = [window = QPointer(_window.get())](Native native) {
		if (!Shortcuts::Launch(std::vector<Native>{ native }) && window) {
			Ui::Toast::Show(window.data(),
				tr::lng_purple_keys_action_unavailable(tr::now));
		}
	};
	using Hint = MessageHints::Action;
	const auto startHint = [&](Hint hint, QString empty) {
		if (_hints.start(target.messages, hint)) {
			_normal = true;
		} else {
			Ui::Toast::Show(_window, std::move(empty));
		}
	};
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
	case Action::Search:
		launch(Native::Search);
		break;
	case Action::OpenMainMenu:
		_window->showMainMenu();
		break;
	case Action::OpenSettings:
		current->showSettings();
		break;
	case Action::OpenSavedMessages:
		current->showPeerHistory(current->session().user());
		break;
	case Action::OpenArchive:
		launch(Native::ShowArchive);
		break;
	case Action::OpenContacts:
		launch(Native::ShowContacts);
		break;
	case Action::OpenScheduled:
		launch(Native::ShowScheduled);
		break;
	case Action::OpenChatMenu:
		launch(Native::ShowChatMenu);
		break;
	case Action::PreviewChat:
		launch(Native::ShowChatPreview);
		break;
	case Action::TogglePlayback:
		launch(Native::MediaPlayPause);
		break;
	case Action::StopPlayback:
		launch(Native::MediaStop);
		break;
	case Action::PreviousTrack:
		launch(Native::MediaPrevious);
		break;
	case Action::NextTrack:
		launch(Native::MediaNext);
		break;
	case Action::SeekBackward:
		launch(Native::MediaSeekBackward);
		break;
	case Action::SeekForward:
		launch(Native::MediaSeekForward);
		break;
	case Action::Account1:
		launch(Native::ShowAccount1);
		break;
	case Action::Account2:
		launch(Native::ShowAccount2);
		break;
	case Action::Account3:
		launch(Native::ShowAccount3);
		break;
	case Action::Account4:
		launch(Native::ShowAccount4);
		break;
	case Action::Account5:
		launch(Native::ShowAccount5);
		break;
	case Action::Account6:
		launch(Native::ShowAccount6);
		break;
	case Action::FocusChats:
		if (const auto dialogs = ChatsPane(current)) {
			dialogs->setInnerFocus(true);
		}
		break;
	case Action::FocusConversation:
		current->content()->activate();
		break;
	case Action::OlderMessagesPage:
	case Action::NewerMessagesPage:
		if (const auto scroll = HistoryScroll(target)) {
			const auto delta = scroll->height();
			const auto top = scroll->scrollTop();
			scroll->scrollToY((action == Action::OlderMessagesPage)
				? (top - delta)
				: (top + delta));
		}
		break;
	case Action::LatestMessages:
	case Action::UnreadMessages: {
		const auto unread = (action == Action::UnreadMessages);
		if (const auto history = target.history) {
			if (const auto peer = history->peer()) {
				history->showHistory(peer->id, unread
					? ShowAtUnreadMsgId
					: ShowAtTheEndMsgId);
			}
		} else if (target.jumpInSection) {
			target.jumpInSection(unread
				? Data::UnreadMessagePosition
				: Data::MaxMessagePosition);
		}
	} break;
	case Action::OpenHistoryCalendar:
		reset();
		current->showCalendar({ .chat = target.calendarChat });
		break;
	case Action::SelectMessage:
		startHint(Hint::Select, tr::lng_purple_keys_message_select_empty(tr::now));
		break;
	case Action::CopyMessage:
		startHint(Hint::Copy, tr::lng_purple_keys_message_copy_empty(tr::now));
		break;
	case Action::MessageMenu:
		startHint(Hint::Menu, tr::lng_purple_keys_message_menu_empty(tr::now));
		break;
	case Action::CopyMessageLink:
		startHint(Hint::Link, tr::lng_purple_keys_message_link_empty(tr::now));
		break;
	case Action::CopyMessageUrl:
		startHint(Hint::CopyUrl, tr::lng_purple_keys_message_link_empty(tr::now));
		break;
	case Action::OpenMessageUrl:
		startHint(Hint::OpenUrl, tr::lng_purple_keys_message_link_empty(tr::now));
		break;
	case Action::ReplyToMessage:
		startHint(Hint::Reply, tr::lng_purple_keys_message_menu_empty(tr::now));
		break;
	case Action::EditMessage:
		startHint(Hint::Edit, tr::lng_purple_keys_message_menu_empty(tr::now));
		break;
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
	if (const auto existing = FindKeys(window)) {
		return existing->handle(receiver, event);
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

bool ModalKeysOwnChatListFocus(QWidget *widget) {
	const auto window = dynamic_cast<MainWindow*>(widget);
	const auto keys = window ? FindKeys(window) : nullptr;
	return keys && keys->ownsChatListFocus();
}

bool CalendarDayKeysEnabled() {
	return Enabled();
}

int CalendarDayKeyOffset(const QKeyEvent &event) {
	if ((event.modifiers() & ~Qt::KeypadModifier) != Qt::ShiftModifier) {
		return 0;
	}
	switch (event.key()) {
	case Qt::Key_Left: return style::RightToLeft() ? 1 : -1;
	case Qt::Key_Right: return style::RightToLeft() ? -1 : 1;
	case Qt::Key_Up: return -7;
	case Qt::Key_Down: return 7;
	default: return 0;
	}
}

void InstallMainMenuKeys(
		not_null<Ui::LayerWidget*> menu,
		not_null<Ui::RpWidget*> contents) {
	using Result = base::EventFilterResult;
	if (!Enabled()) {
		return;
	}
	const auto navigation = menu->lifetime().make_state<
		::Settings::KeyNavigation>(contents);
	base::install_event_filter(menu, [=](not_null<QEvent*> event) {
		if (event->type() != QEvent::KeyPress
			|| Ui::ScreenReaderModeActive()
			|| QApplication::focusWidget() != menu.get()
			|| QApplication::activeWindow() != menu->window()
			|| QApplication::activePopupWidget()
			|| QApplication::activeModalWidget()) {
			return Result::Continue;
		}
		const auto stack = dynamic_cast<Ui::LayerStackWidget*>(
			menu->parentWidget());
		if (!stack || stack->topShownLayer() != menu.get()) {
			return Result::Continue;
		}
		return navigation->handle(static_cast<QKeyEvent*>(event.get()))
			? Result::Cancel
			: Result::Continue;
	});
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
