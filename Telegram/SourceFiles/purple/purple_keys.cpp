#include "purple/purple_keys.h"

#include "base/event_filter.h"
#include "base/weak_ptr.h"
#include "calls/calls_box_controller.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/shortcuts.h"
#include "data/data_document.h"
#include "data/data_user.h"
#include "dialogs/dialogs_inner_widget.h"
#include "dialogs/dialogs_widget.h"
#include "history/history_widget.h"
#include "history/view/controls/history_view_compose_controls.h"
#include "history/view/history_view_chat_section.h"
#include "history/view/history_view_pinned_section.h"
#include "history/view/history_view_scheduled_section.h"
#include "info/downloads/info_downloads_widget.h"
#include "info/info_controller.h"
#include "info/info_memento.h"
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
#include "purple/purple_music_section.h"
#include "purple/purple_chat_palette.h"
#include "purple/purple_exact_search.h"
#include "purple/purple_playlists_box.h"
#include "settings/settings_builder.h"
#include "settings/settings_key_navigation.h"
#include "storage/storage_shared_media.h"
#include "ui/abstract_button.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/layer_widget.h"
#include "ui/screen_reader_mode.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/elastic_scroll.h"
#include "ui/widgets/checkbox.h"
#include "ui/wrap/slide_wrap.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <edn.h>

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtGui/QTextBlock>
#include <QtGui/QTextLayout>
#include <QtWidgets/QApplication>
#include <QtWidgets/QTextEdit>

#include <array>
#include <memory>
#include <optional>
#include <type_traits>

#ifdef Q_OS_MAC
#include <Carbon/Carbon.h>
#endif // Q_OS_MAC

namespace Purple {

class KeyboardAttachment final {
public:
	template <typename Widget>
	static Fn<void()> From(Widget *widget) {
		if (!CanAttach(widget)) return {};
		return [widget = QPointer(widget)] {
			if (!widget || !CanAttach(widget.data())) return;
			if constexpr (std::is_same_v<Widget, HistoryView::ScheduledWidget>) {
				widget->chooseAttach();
			} else {
				widget->chooseAttach(std::nullopt);
			}
		};
	}

private:
	static bool ButtonAvailable(Ui::IconButton *button) {
		return button
			&& button->isVisible()
			&& button->isEnabled()
			&& !button->isDisabled();
	}

	static bool CanAttach(HistoryWidget *widget) {
		return widget->_canSendMessages && ButtonAvailable(widget->_attachToggle.data());
	}

	static bool CanAttach(HistoryView::ChatWidget *widget) {
		return widget->_canSendMessages && SectionAvailable(widget);
	}

	static bool CanAttach(HistoryView::ScheduledWidget *widget) {
		return SectionAvailable(widget);
	}

	template <typename Widget>
	static bool SectionAvailable(Widget *widget) {
		return !widget->_choosingAttach && widget->_composeControls
			&& ButtonAvailable(widget->_composeControls->_attachToggle);
	}
};

namespace {

constexpr auto kEnabledPref = "purple-modal-keys-enabled";
constexpr auto kWhichKeyDelay = 300;
constexpr auto kPaletteRows = 9;
#ifdef Q_OS_MAC
constexpr auto kCtrl = Qt::MetaModifier;
constexpr auto kSuper = Qt::ControlModifier;
#else
constexpr auto kCtrl = Qt::ControlModifier;
constexpr auto kSuper = Qt::MetaModifier;
#endif // Q_OS_MAC

rpl::event_stream<bool> EnabledChanges;

bool Enabled() {
	return Core::App().settings().readPref<bool>(kEnabledPref);
}

int ModalKey(const QKeyEvent &event) {
	const auto logical = (event.key() == Qt::Key_Enter)
		? Qt::Key_Return
		: event.key();
#ifdef Q_OS_MAC
	const auto modifiers = event.modifiers();
	if (!event.timestamp()) {
		return logical;
	}
	constexpr auto letters = std::to_array<quint32>({
		kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E,
		kVK_ANSI_F, kVK_ANSI_G, kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J,
		kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N, kVK_ANSI_O,
		kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R, kVK_ANSI_S, kVK_ANSI_T,
		kVK_ANSI_U, kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y,
		kVK_ANSI_Z,
	});
	constexpr auto digits = std::to_array<quint32>({
		kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4,
		kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9,
	});
	const auto native = event.nativeVirtualKey();
	auto physical = logical;
	if (const auto letter = ranges::find(letters, native);
		letter != end(letters)) {
		physical = Qt::Key_A + (letter - begin(letters));
	} else if (const auto digit = ranges::find(digits, native);
		digit != end(digits)) {
		physical = Qt::Key_0 + (digit - begin(digits));
	} else if (native == kVK_ANSI_LeftBracket) {
		physical = Qt::Key_BracketLeft;
	} else if (native == kVK_ANSI_RightBracket) {
		physical = Qt::Key_BracketRight;
	} else if (native == kVK_ANSI_Slash) {
		physical = modifiers.testFlag(Qt::ShiftModifier)
			? Qt::Key_Question
			: Qt::Key_Slash;
	}
	if (physical != logical) {
		if (const auto layout = TISCopyCurrentKeyboardLayoutInputSource()) {
			const auto nonAscii = (TISGetInputSourceProperty(
				layout,
				kTISPropertyInputSourceIsASCIICapable) == kCFBooleanFalse);
			CFRelease(layout);
			if (nonAscii) {
				return physical;
			}
		}
	}
#endif // Q_OS_MAC
	return logical;
}

enum class Action {
	EnterNormalMode,
	EnterInsertMode,
	Backgrounds,
	PreviousChat,
	NextChat,
	OpenChat,
	ChatPalette,
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
	OpenCalls,
	OpenDownloads,
	NewGroup,
	NewChannel,
	AddContact,
	OpenScheduled,
	OpenMusic,
	OpenPlaylists,
	OpenChatInfo,
	OpenChatMedia,
	OpenChatMusic,
	OpenChatFiles,
	OpenChatLinks,
	OpenChatVoice,
	OpenChatGifs,
	OpenChatPolls,
	OpenPinnedMessages,
	AttachFiles,
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

using ChatMediaType = Storage::SharedMediaType;

struct Command {
	Action action;
	const char *id;
	QString (*label)();
	ChatMediaType mediaType = ChatMediaType::kCount;
};

const Command kCommands[] = {
	{ Action::EnterNormalMode, "enter-normal-mode",
		[] { return tr::lng_purple_keys_normal(tr::now); } },
	{ Action::EnterInsertMode, "enter-insert-mode",
		[] { return tr::lng_purple_keys_insert(tr::now); } },
	{ Action::Backgrounds, "chat-backgrounds-visibility-toggle",
		[] { return tr::lng_purple_keys_backgrounds(tr::now); } },
	{ Action::PreviousChat, "chat-cursor-previous",
		[] { return tr::lng_purple_keys_chat_previous(tr::now); } },
	{ Action::NextChat, "chat-cursor-next",
		[] { return tr::lng_purple_keys_chat_next(tr::now); } },
	{ Action::OpenChat, "chat-cursor-open",
		[] { return tr::lng_purple_keys_chat_open(tr::now); } },
	{ Action::ChatPalette, "chat-palette",
		[] { return tr::lng_purple_chat_palette(tr::now); } },
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
	{ Action::OpenCalls, "open-calls",
		[] { return tr::lng_menu_calls(tr::now); } },
	{ Action::OpenDownloads, "open-downloads",
		[] { return tr::lng_downloads_section(tr::now); } },
	{ Action::NewGroup, "new-group",
		[] { return tr::lng_create_group_title(tr::now); } },
	{ Action::NewChannel, "new-channel",
		[] { return tr::lng_create_channel_title(tr::now); } },
	{ Action::AddContact, "add-contact",
		[] { return tr::lng_profile_add_contact(tr::now); } },
	{ Action::OpenScheduled, "open-scheduled-messages",
		[] { return tr::lng_scheduled_messages(tr::now); } },
	{ Action::OpenMusic, "open-music",
		[] { return tr::lng_purple_music_title(tr::now); } },
	{ Action::OpenPlaylists, "open-playlists",
		[] { return tr::lng_purple_playlists_title(tr::now); } },
	{ Action::OpenChatInfo, "open-chat-info",
		[] { return tr::lng_purple_keys_chat_info(tr::now); } },
	{ Action::OpenChatMedia, "open-chat-media",
		[] { return tr::lng_media_type_media(tr::now); },
		ChatMediaType::PhotoVideo },
	{ Action::OpenChatMusic, "open-chat-music",
		[] { return tr::lng_purple_keys_chat_music(tr::now); },
		ChatMediaType::MusicFile },
	{ Action::OpenChatFiles, "open-chat-files",
		[] { return tr::lng_media_type_files(tr::now); },
		ChatMediaType::File },
	{ Action::OpenChatLinks, "open-chat-links",
		[] { return tr::lng_media_type_links(tr::now); },
		ChatMediaType::Link },
	{ Action::OpenChatVoice, "open-chat-voice",
		[] { return tr::lng_media_type_audios(tr::now); },
		ChatMediaType::RoundVoiceFile },
	{ Action::OpenChatGifs, "open-chat-gifs",
		[] { return tr::lng_media_type_gifs(tr::now); },
		ChatMediaType::GIF },
	{ Action::OpenChatPolls, "open-chat-polls",
		[] { return tr::lng_media_type_polls(tr::now); },
		ChatMediaType::Poll },
	{ Action::OpenPinnedMessages, "open-pinned-messages",
		[] { return tr::lng_settings_events_pinned(tr::now); } },
	{ Action::AttachFiles, "attach-files",
		[] { return tr::lng_attach(tr::now); } },
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

enum class KeyMode {
	Normal,
	Insert,
};

constexpr auto kCtrlBracket = int(kCtrl) | Qt::Key_BracketLeft;
constexpr auto kCtrlG = int(kCtrl) | Qt::Key_G;

struct Binding {
	Action action;
	Chord keys;
	KeyMode mode = KeyMode::Normal;
};

const Binding kBindings[] = {
	{ Action::EnterNormalMode, { kCtrlBracket }, KeyMode::Insert },
	{ Action::EnterNormalMode, { kCtrlBracket } },
	{ Action::EnterInsertMode, { Qt::Key_I } },
	{ Action::Backgrounds, { Qt::Key_Space, Qt::Key_C, Qt::Key_B } },
	{ Action::PreviousChat, { Qt::Key_BracketLeft, Qt::Key_C } },
	{ Action::PreviousChat, { Qt::Key_K } },
	{ Action::NextChat, { Qt::Key_BracketRight, Qt::Key_C } },
	{ Action::NextChat, { Qt::Key_J } },
	{ Action::OpenChat, { Qt::Key_Return } },
	{ Action::ChatPalette, { Qt::Key_Space, Qt::Key_Return } },
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
	{ Action::OpenSettings, { Qt::Key_Space, Qt::Key_O, Qt::Key_S, Qt::Key_E } },
	{ Action::OpenSavedMessages, { Qt::Key_Space, Qt::Key_O, Qt::Key_S, Qt::Key_A } },
	{ Action::OpenArchive, { Qt::Key_Space, Qt::Key_O, Qt::Key_A } },
	{ Action::OpenContacts, { Qt::Key_Space, Qt::Key_O, Qt::Key_C } },
	{ Action::OpenCalls, { Qt::Key_Space, Qt::Key_O, Qt::Key_P } },
	{ Action::OpenDownloads, { Qt::Key_Space, Qt::Key_O, Qt::Key_D } },
	{ Action::NewGroup, { Qt::Key_Space, Qt::Key_N, Qt::Key_G } },
	{ Action::NewChannel, { Qt::Key_Space, Qt::Key_N, Qt::Key_C } },
	{ Action::AddContact, { Qt::Key_Space, Qt::Key_N, Qt::Key_P } },
	{ Action::OpenScheduled, { Qt::Key_Space, Qt::Key_C, Qt::Key_H, Qt::Key_S } },
	{ Action::OpenChatMenu, { Qt::Key_Space, Qt::Key_C, Qt::Key_M } },
	{ Action::OpenChatInfo, { Qt::Key_Space, Qt::Key_C, Qt::Key_I } },
	{ Action::OpenChatMedia, { Qt::Key_Space, Qt::Key_C, Qt::Key_S, Qt::Key_V } },
	{ Action::OpenChatMusic, { Qt::Key_Space, Qt::Key_C, Qt::Key_S, Qt::Key_A } },
	{ Action::OpenChatFiles, { Qt::Key_Space, Qt::Key_C, Qt::Key_S, Qt::Key_F } },
	{ Action::OpenChatLinks, { Qt::Key_Space, Qt::Key_C, Qt::Key_S, Qt::Key_L } },
	{ Action::OpenChatVoice, { Qt::Key_Space, Qt::Key_C, Qt::Key_S, Qt::Key_R } },
	{ Action::OpenChatGifs, { Qt::Key_Space, Qt::Key_C, Qt::Key_S, Qt::Key_G } },
	{ Action::OpenChatPolls, { Qt::Key_Space, Qt::Key_C, Qt::Key_S, Qt::Key_P } },
	{ Action::OpenPinnedMessages, { Qt::Key_Space, Qt::Key_C, Qt::Key_H, Qt::Key_P } },
	{ Action::AttachFiles, { Qt::Key_Space, Qt::Key_C, Qt::Key_U } },
	{ Action::PreviewChat, { Qt::Key_Space, Qt::Key_C, Qt::Key_P } },
	{ Action::OpenMusic, { Qt::Key_Space, Qt::Key_M, Qt::Key_M } },
	{ Action::OpenPlaylists, { Qt::Key_Space, Qt::Key_M, Qt::Key_L } },
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
	{ Action::FocusChats, { Qt::Key_Space, Qt::Key_W, Qt::Key_L } },
	{ Action::FocusConversation, { Qt::Key_Space, Qt::Key_W, Qt::Key_C } },
	{ Action::OlderMessagesPage, { Qt::Key_BracketLeft, Qt::Key_M } },
	{ Action::NewerMessagesPage, { Qt::Key_BracketRight, Qt::Key_M } },
	{ Action::LatestMessages, { Qt::Key_BracketRight, Qt::Key_BracketRight } },
	{ Action::UnreadMessages, { Qt::Key_BracketRight, Qt::Key_U } },
	{ Action::OpenHistoryCalendar, { Qt::Key_Space, Qt::Key_C, Qt::Key_H, Qt::Key_D } },
	{ Action::Help, { Qt::Key_Space, Qt::Key_Question } },
	{ Action::Run, { Qt::Key_Space, Qt::Key_Space } },
	{ Action::DescribeChord, { Qt::Key_Space, Qt::Key_H, Qt::Key_K } },
	{ Action::DescribeCommand, { Qt::Key_Space, Qt::Key_H, Qt::Key_F } },
};

const Binding kLegacyBindings[] = {
	{ Action::OpenSettings, { Qt::Key_Space, Qt::Key_O, Qt::Key_T } },
	{ Action::OpenSavedMessages, { Qt::Key_Space, Qt::Key_O, Qt::Key_S } },
	{ Action::OpenCalls, { Qt::Key_Space, Qt::Key_O, Qt::Key_V } },
	{ Action::OpenDownloads, { Qt::Key_Space, Qt::Key_O, Qt::Key_F } },
	{ Action::OpenScheduled, { Qt::Key_Space, Qt::Key_O, Qt::Key_D } },
	{ Action::OpenChatMedia, { Qt::Key_Space, Qt::Key_C, Qt::Key_V } },
	{ Action::OpenChatMusic, { Qt::Key_Space, Qt::Key_C, Qt::Key_A } },
	{ Action::OpenChatFiles, { Qt::Key_Space, Qt::Key_C, Qt::Key_F } },
	{ Action::OpenChatLinks, { Qt::Key_Space, Qt::Key_C, Qt::Key_L } },
	{ Action::OpenChatVoice, { Qt::Key_Space, Qt::Key_C, Qt::Key_R } },
	{ Action::OpenChatGifs, { Qt::Key_Space, Qt::Key_C, Qt::Key_G } },
	{ Action::OpenChatPolls, { Qt::Key_Space, Qt::Key_C, Qt::Key_O } },
	{ Action::OpenPinnedMessages, { Qt::Key_Space, Qt::Key_C, Qt::Key_N } },
	{ Action::FocusChats, { Qt::Key_Space, Qt::Key_W, Qt::Key_D } },
	{ Action::OpenHistoryCalendar, { Qt::Key_Space, Qt::Key_C, Qt::Key_D } },
};

QString BareKeyLabel(int key) {
	if (key == Qt::Key_Space) {
		return u"SPC"_q;
	} else if (key == Qt::Key_Return) {
		return u"Enter"_q;
	} else if (key >= Qt::Key_A && key <= Qt::Key_Z) {
		return QString(QChar(key - Qt::Key_A + 'a'));
	}
	return QKeySequence(key).toString(QKeySequence::NativeText);
}

QString KeyLabel(int stroke) {
	const auto combination = QKeyCombination::fromCombined(stroke);
	const auto mods = combination.keyboardModifiers();
	auto label = QString();
	if (mods.testFlag(kCtrl)) {
		label += u"Ctrl+"_q;
	}
	if (mods.testFlag(Qt::AltModifier)) {
		label += u"Alt+"_q;
	}
	if (mods.testFlag(kSuper)) {
		label += u"Super+"_q;
	}
	if (mods.testFlag(Qt::ShiftModifier)) {
		label += u"Shift+"_q;
	}
	return label + BareKeyLabel(combination.key());
}

bool ShiftedSymbol(int key) {
	return key >= '!' && key <= '~'
		&& QStringView(u"!@#$%^&*()_+{}|:\"<>?~").contains(QChar(ushort(key)));
}

int KeyStroke(const QKeyEvent &event, int key) {
	auto mods = event.modifiers() & ~Qt::KeypadModifier;
	if (ShiftedSymbol(key)) {
		mods &= ~Qt::ShiftModifier;
	}
	return QKeyCombination(mods, Qt::Key(key)).toCombined();
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
			const auto label = ChordLabel(binding.keys);
			result.push_back((binding.mode == KeyMode::Insert)
				? tr::lng_purple_keys_insert(tr::now) + u" ("_q + label + u")"_q
				: label);
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
	} else if (token.compare(u"Enter"_q, Qt::CaseInsensitive) == 0
			|| token.compare(u"RET"_q, Qt::CaseInsensitive) == 0) {
		return Qt::Key_Return;
	} else if (token.size() != 1) {
		const auto key = QKeySequence::fromString(
			token, QKeySequence::PortableText);
		if (key.count() == 1 && key[0].keyboardModifiers() == Qt::NoModifier) {
			const auto value = key[0].key();
			if ((value >= Qt::Key_F1 && value <= Qt::Key_F35)
				|| (value >= Qt::Key_Home && value <= Qt::Key_PageDown)) {
				return value;
			}
		}
		return 0;
	}
	const auto key = token.front().unicode();
	if (key >= 'a' && key <= 'z') {
		return Qt::Key_A + (key - 'a');
	} else if (key >= '!' && key <= '~') {
		return key;
	}
	return 0;
}

bool HasShortcutModifier(int stroke) {
	const auto mods = QKeyCombination::fromCombined(stroke).keyboardModifiers();
	return (mods & (kCtrl | kSuper | Qt::AltModifier)) != Qt::NoModifier;
}

int ParseStroke(const QString &token) {
	auto text = token;
	auto mods = Qt::KeyboardModifiers();
	while (text.size() > 2 && text[1] == '-') {
		const auto prefix = text[0];
		const auto mod = (prefix == 'C') ? kCtrl
			: (prefix == 'M') ? Qt::AltModifier
			: (prefix == 's') ? kSuper
			: (prefix == 'S') ? Qt::ShiftModifier
			: Qt::NoModifier;
		if (!mod || (mods & mod)) {
			return 0;
		}
		mods |= mod;
		text.remove(0, 2);
	}
	for (auto separator = text.indexOf('+'); separator > 0;
		separator = text.indexOf('+')) {
		const auto part = text.left(separator).toLower();
		const auto mod = (part == u"ctrl"_q) ? kCtrl
			: (part == u"alt"_q) ? Qt::AltModifier
			: (part == u"super"_q) ? kSuper
			: (part == u"shift"_q) ? Qt::ShiftModifier
			: Qt::NoModifier;
		if (!mod || (mods & mod)) {
			return 0;
		}
		mods |= mod;
		text.remove(0, separator + 1);
	}
	const auto key = ParseKey(text);
	if (!key || (ShiftedSymbol(key) && mods.testFlag(Qt::ShiftModifier))) {
		return 0;
	}
	return QKeyCombination(mods, Qt::Key(key)).toCombined();
}

Chord ParseChord(const QString &text, KeyMode mode) {
	const auto tokens = text.simplified().split(' ', Qt::SkipEmptyParts);
	if (tokens.empty() || tokens.size() > 4) {
		return {};
	}
	auto result = Chord();
	for (const auto &token : tokens) {
		const auto key = ParseStroke(token);
		if (!key || key == kCtrlG) {
			return {};
		}
		result.push_back(key);
	}
	if (result.front() == Qt::Key_Question
		|| (mode == KeyMode::Insert && !HasShortcutModifier(result.front()))) {
		return {};
	}
	return result;
}

bool IsHelpBinding(Action action) {
	return action == Action::Help || action == Action::Run
		|| action == Action::DescribeChord || action == Action::DescribeCommand;
}

QString EdnKeyword(const edn_value_t *value) {
	const char *name = nullptr;
	auto size = size_t(0);
	auto namespaceSize = size_t(0);
	return (edn_keyword_get(value, nullptr, &namespaceSize, &name, &size)
			&& !namespaceSize)
		? QString::fromUtf8(name, qsizetype(size))
		: QString();
}

QJsonDocument ReadEdnKeymap(const QByteArray &bytes) {
	if (bytes.isEmpty() || bytes.contains('\0')) {
		return {};
	}
	const auto options = edn_parse_options_t{
		.struct_size = sizeof(edn_parse_options_t),
		.default_reader_mode = EDN_DEFAULT_READER_ERROR,
		.max_depth = 16,
	};
	const auto parsed = edn_read_with_options(
		bytes.constData(), size_t(bytes.size()), &options);
	const auto root = std::unique_ptr<edn_value_t, decltype(&edn_free)>(
		parsed.value, &edn_free);
	if (parsed.error != EDN_OK || edn_type(root.get()) != EDN_TYPE_MAP) {
		return {};
	}
	auto end = size_t();
	if (!edn_source_position(root.get(), nullptr, &end)
		|| end > size_t(bytes.size())) {
		return {};
	}
	if (end < size_t(bytes.size())) {
		auto tailOptions = options;
		tailOptions.eof_value = root.get();
		const auto tail = edn_read_with_options(
			bytes.constData() + end, size_t(bytes.size()) - end, &tailOptions);
		if (tail.value != root.get()) {
			edn_free(tail.value);
			return {};
		}
		if (tail.error != EDN_OK) {
			return {};
		}
	}
	auto groups = QJsonObject();
	for (auto i = size_t(0); i < edn_map_count(root.get()); ++i) {
		const auto mode = EdnKeyword(edn_map_get_key(root.get(), i));
		const auto map = edn_map_get_value(root.get(), i);
		if ((mode != u"normal"_q && mode != u"insert"_q)
			|| edn_type(map) != EDN_TYPE_MAP) {
			return {};
		}
		auto entries = QJsonObject();
		for (auto j = size_t(0); j < edn_map_count(map); ++j) {
			auto size = size_t(0);
			const auto key = edn_string_get(edn_map_get_key(map, j), &size);
			if (!key) {
				return {};
			}
			const auto chord = QString::fromUtf8(key, qsizetype(size));
			const auto value = edn_map_get_value(map, j);
			if (edn_type(value) == EDN_TYPE_NIL) {
				entries.insert(chord, QJsonValue::Null);
			} else {
				const auto command = EdnKeyword(value);
				if (command.isEmpty()) {
					return {};
				}
				entries.insert(chord, command);
			}
		}
		groups.insert(mode, entries);
	}
	return QJsonDocument(groups);
}

std::optional<std::vector<Binding>> ParseBindings(
		const QByteArray &bytes,
		bool legacy,
		std::vector<Binding> result) {
	const auto document = legacy
		? QJsonDocument::fromJson(bytes)
		: ReadEdnKeymap(bytes);
	constexpr auto kEntryLimit = 128;
	auto edited = std::vector<std::pair<KeyMode, Chord>>();
	const auto apply = [&](KeyMode mode, const QString &text, QJsonValue id) {
		if (!id.isNull() && !id.isString()) {
			return false;
		}
		auto keys = ParseChord(text, mode);
		if (keys.empty()
			|| edited.size() >= kEntryLimit
			|| ranges::contains(edited, std::pair(mode, keys))) {
			return false;
		}
		edited.emplace_back(mode, keys);
		const auto current = ranges::find_if(result, [&](const Binding &item) {
			return item.mode == mode && item.keys == keys;
		});
		if (current != end(result) && IsHelpBinding(current->action)) {
			return false;
		}
		if (id.isNull()) {
			if (current != end(result)) {
				result.erase(current);
			}
			return true;
		}
		const auto name = id.toString();
		const auto command = ranges::find_if(kCommands, [&](const Command &item) {
			return name == QString::fromLatin1(item.id);
		});
		if (command == std::end(kCommands)) {
			return false;
		} else if (current != end(result)) {
			current->action = command->action;
		} else {
			result.push_back({ command->action, std::move(keys), mode });
		}
		return true;
	};
	if (legacy && document.isArray()) {
		for (auto &binding : result) {
			const auto previous = ranges::find(
				kLegacyBindings, binding.action, &Binding::action);
			if (binding.mode == KeyMode::Normal
				&& previous != std::end(kLegacyBindings)) {
				binding.keys = previous->keys;
			}
		}
		for (const auto value : document.array()) {
			const auto entry = value.toObject();
			const auto text = entry.value(u"keys"_q);
			if (entry.size() != 2 || !text.isString()
				|| !apply(KeyMode::Normal, text.toString(), entry.value(u"command"_q))) {
				return std::nullopt;
			}
		}
	} else if (!legacy && document.isObject()) {
		const auto groups = document.object();
		for (auto group = groups.begin(); group != groups.end(); ++group) {
			if ((group.key() != u"normal"_q && group.key() != u"insert"_q)
				|| !group.value().isObject()) {
				return std::nullopt;
			}
			const auto mode = (group.key() == u"normal"_q)
				? KeyMode::Normal
				: KeyMode::Insert;
			const auto entries = group.value().toObject();
			for (auto entry = entries.begin(); entry != entries.end(); ++entry) {
				if (!apply(mode, entry.key(), entry.value())) {
					return std::nullopt;
				}
			}
		}
	} else {
		return std::nullopt;
	}
	for (const auto &binding : result) {
		if (ranges::any_of(result, [&](const Binding &other) {
			return binding.mode == other.mode
				&& binding.keys.size() < other.keys.size()
				&& StartsWith(other, binding.keys);
		})) {
			return std::nullopt;
		}
	}
	const auto canEnterNormal = ranges::any_of(result, [](const Binding &binding) {
		return binding.mode == KeyMode::Insert
			&& binding.action == Action::EnterNormalMode;
	});
	return canEnterNormal ? std::optional(std::move(result)) : std::nullopt;
}

const std::vector<Binding> &Bindings() {
	static const auto result = [] {
		auto defaults = std::vector<Binding>(
			std::begin(kBindings),
			std::end(kBindings));
		const auto directory = ConfigDirectory();
		auto info = QFileInfo(directory + u"/keys.edn"_q);
		const auto legacy = !info.exists();
		if (legacy) {
			info.setFile(directory + u"/keys.json"_q);
		}
		if (!info.exists()) {
			return defaults;
		}
		auto file = QFile(info.filePath());
		constexpr auto kByteLimit = 16 * 1024;
		if (info.isFile() && file.open(QIODevice::ReadOnly)) {
			const auto bytes = file.read(kByteLimit + 1);
			if (file.error() == QFileDevice::NoError
				&& bytes.size() <= kByteLimit) {
				if (auto parsed = ParseBindings(bytes, legacy, defaults)) {
					return std::move(*parsed);
				}
			}
		}
		LOG(("Purple modal keys: invalid keymap, using default bindings."));
		return defaults;
	}();
	return result;
}

bool MatchesNext(KeyMode mode, const Chord &prefix, int stroke) {
	return ranges::any_of(Bindings(), [&](const Binding &binding) {
		return binding.mode == mode
			&& binding.keys.size() > prefix.size()
			&& binding.keys[prefix.size()] == stroke
			&& StartsWith(binding, prefix);
	});
}

QString WhichKeyGroup(Chord prefix, int next) {
	prefix.push_back(next);
	if (prefix == Chord{ Qt::Key_Space, Qt::Key_C }) {
		return tr::lng_purple_keys_group_chat(tr::now);
	} else if (prefix == Chord{ Qt::Key_Space, Qt::Key_O }) {
		return tr::lng_purple_keys_group_open(tr::now);
	} else if (prefix == Chord{ Qt::Key_Space, Qt::Key_A }) {
		return tr::lng_purple_keys_group_accounts(tr::now);
	} else if (prefix == Chord{ Qt::Key_Space, Qt::Key_W }) {
		return tr::lng_mac_menu_window(tr::now);
	} else if (prefix == Chord{ Qt::Key_Space, Qt::Key_M }) {
		return tr::lng_purple_music_title(tr::now);
	} else if (prefix == Chord{ Qt::Key_Space, Qt::Key_S }) {
		return tr::lng_shortcuts_search(tr::now);
	} else if (prefix == Chord{ Qt::Key_Space, Qt::Key_N }) {
		return tr::lng_create_group_create(tr::now);
	} else if (prefix == Chord{ Qt::Key_Space, Qt::Key_H }) {
		return tr::lng_purple_keys_help(tr::now);
	} else if (prefix == Chord{ Qt::Key_G, Qt::Key_S }
			|| prefix == Chord{ Qt::Key_Space, Qt::Key_C, Qt::Key_H }) {
		return tr::lng_purple_keys_group_messages(tr::now);
	} else if (prefix == Chord{ Qt::Key_Space, Qt::Key_C, Qt::Key_S }) {
		return tr::lng_profile_shared_media(tr::now);
	}
	return u"…"_q;
}

struct Target {
	QPointer<QWidget> focus;
	Dialogs::InnerWidget *list = nullptr;
	HistoryWidget *history = nullptr;
	MessageHints::Target messages;
	Fn<void(Data::MessagePosition)> jumpInSection;
	Fn<void()> attach;
	Dialogs::Key chat;
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
	if (MusicAllowsModalKeys(owner, focus)) {
		return { .focus = focus, .controller = owner };
	}
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
					.attach = KeyboardAttachment::From(history),
					.chat = Dialogs::Key(history->history()),
					.chatActions = true,
					.controller = owner,
				}
				: Target();
		}
		if (const auto chat = dynamic_cast<HistoryView::ChatWidget*>(widget)) {
			auto target = forSection(chat, false, true);
			if (target.focus) {
				target.attach = KeyboardAttachment::From(chat);
				const auto key = chat->activeChat().key;
				const auto id = chat->id();
				if (!id.sublist && (!id.repliesRootId || key.topic())) {
					target.chat = key;
				}
			}
			return target;
		}
		if (const auto pinned = dynamic_cast<HistoryView::PinnedWidget*>(widget)) {
			return forSection(pinned);
		}
		if (const auto scheduled = dynamic_cast<HistoryView::ScheduledWidget*>(widget)) {
			auto target = forSection(scheduled, true);
			if (target.focus) target.attach = KeyboardAttachment::From(scheduled);
			return target;
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
	case Action::OpenChatInfo:
	case Action::OpenPinnedMessages: return target.chat.thread() != nullptr;
	case Action::OpenChatMedia:
	case Action::OpenChatMusic:
	case Action::OpenChatFiles:
	case Action::OpenChatLinks:
	case Action::OpenChatVoice:
	case Action::OpenChatGifs:
	case Action::OpenChatPolls: return target.chat.peer() != nullptr;
	case Action::AttachFiles: return bool(target.attach);
	case Action::OpenHistoryCalendar: return bool(target.chat);
	case Action::Search: return !target.messages.scheduled;
	case Action::OpenCalls:
	case Action::NewGroup:
	case Action::NewChannel:
		return target.controller && !target.controller->session().supportMode();
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
	PaletteKeys(not_null<QTextEdit*> editor, Fn<void(int)> move)
	: QObject(editor.get())
	, _editor(editor)
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
		const auto layout = _editor->textCursor().block().layout();
		if (layout && !layout->preeditAreaText().isEmpty()) {
			return false;
		}
		_move(key->key() == Qt::Key_Up ? -1 : 1);
		return true;
	}

	const not_null<QTextEdit*> _editor;
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
		const auto terms = ExactSearchTerms(field->getLastText());
		matches->clear();
		for (const auto command : commands) {
			const auto text = command->label() + u' '
				+ QString::fromLatin1(command->id) + u' '
				+ BindingsLabel(command->action);
			if (MatchExactSearch(terms, text)) {
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
	new PaletteKeys(field->rawTextEdit(), [=](int offset) {
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
	[[nodiscard]] KeyMode mode() const;
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

KeyMode Keys::mode() const {
	return _normal ? KeyMode::Normal : KeyMode::Insert;
}

KeyUse Keys::classify(const QKeyEvent &event, int key) const {
	const auto mods = event.modifiers() & ~Qt::KeypadModifier;
	const auto stroke = KeyStroke(event, key);
	const auto pending = !_prefix.empty() || _inspect || _hints.active();
	if (HasShortcutModifier(stroke)) {
		const auto &shortcuts = Shortcuts::KeysCurrents();
		if (shortcuts.contains(QKeySequence(event.keyCombination()))
			|| shortcuts.contains(QKeySequence(QKeyCombination::fromCombined(stroke)))) {
			return KeyUse::Pass;
		}
	}
	if (((key == Qt::Key_Escape && !mods) || stroke == kCtrlG)
		&& (_normal || pending)) {
		return pending ? KeyUse::Cancel : KeyUse::ExitNormal;
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
	} else if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
		return KeyUse::Pass;
	}
	const auto printable = key >= Qt::Key_Space && key < Qt::Key_Escape;
	if (MatchesNext(mode(), _prefix, stroke)
		|| ((_inspect || (_normal && pending)) && !mods && printable)) {
		return KeyUse::Capture;
	}
	if (!_normal) {
		return KeyUse::Pass;
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
	const auto key = ModalKey(*keyEvent);
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
	const auto stockLegend = (keyEvent->key() == Qt::Key_Question
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
	case KeyUse::ExitNormal:
		dispatch(Action::EnterInsertMode, target);
		break;
	case KeyUse::Cancel:
	case KeyUse::Ignore:
		clearPrefix();
		break;
	case KeyUse::Capture:
		stroke(KeyStroke(*keyEvent, key), target);
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
	if (!target.focus || !Enabled() || (_prefix.empty() && !_inspect)) {
		reset();
		return;
	}
	auto lines = QStringList();
	auto seen = QSet<int>();
	for (const auto &binding : Bindings()) {
		if (binding.mode != mode()
			|| !StartsWith(binding, _prefix)
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
			: WhichKeyGroup(_prefix, next);
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
		if (binding.mode != mode() || !StartsWith(binding, _prefix)) {
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
	case Action::EnterNormalMode:
		clearPrefix();
		if (!_normal) {
			_normal = true;
			Ui::Toast::Show(_window, tr::lng_purple_keys_normal(tr::now));
		}
		break;
	case Action::EnterInsertMode:
		reset();
		Ui::Toast::Show(_window, tr::lng_purple_keys_insert(tr::now));
		break;
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
	case Action::ChatPalette:
		reset();
		ShowChatPalette(current);
		break;
	case Action::OpenMusic:
		ToggleMusicSection(current);
		break;
	case Action::OpenPlaylists:
		reset();
		ShowPlaylistsBox(current);
		break;
	case Action::OpenChatInfo:
		reset();
		current->showPeerInfo(target.chat.thread());
		break;
	case Action::AttachFiles:
		reset();
		target.attach();
		break;
	case Action::OpenChatMedia:
	case Action::OpenChatMusic:
	case Action::OpenChatFiles:
	case Action::OpenChatLinks:
	case Action::OpenChatVoice:
	case Action::OpenChatGifs:
	case Action::OpenChatPolls: {
		reset();
		const auto media = Info::Section(FindCommand(action).mediaType);
		if (const auto topic = target.chat.topic()) {
			current->showSection(std::make_shared<Info::Memento>(topic, media));
		} else {
			current->showSection(std::make_shared<Info::Memento>(target.chat.peer(), media));
		}
	} break;
	case Action::OpenPinnedMessages:
		reset();
		current->showSection(std::make_shared<HistoryView::PinnedMemento>(
			target.chat.thread()));
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
	case Action::OpenCalls:
		reset();
		::Calls::ShowCallsBox(current);
		break;
	case Action::OpenDownloads:
		reset();
		current->showSection(Info::Downloads::Make(current->session().user()));
		break;
	case Action::NewGroup:
		reset();
		current->showNewGroup();
		break;
	case Action::NewChannel:
		reset();
		current->showNewChannel();
		break;
	case Action::AddContact:
		reset();
		current->showAddContact();
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
		current->showCalendar({ .chat = target.chat });
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
		auto lines = QStringList{
			tr::lng_purple_keys_settings_hint(tr::now),
			tr::lng_purple_keys_menu_accounts_help(tr::now),
		};
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
	const auto stroke = KeyStroke(*key, ModalKey(*key));
	if (!HasShortcutModifier(stroke) || !MatchesNext(KeyMode::Insert, {}, stroke)) {
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
		not_null<Ui::RpWidget*> contents,
		not_null<Ui::AbstractButton*> accountsToggle,
		not_null<Ui::SlideWrap<Ui::RpWidget>*> accounts) {
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
		const auto key = static_cast<QKeyEvent*>(event.get());
		if (key->key() == Qt::Key_A
			&& key->modifiers() == Qt::NoModifier
			&& accountsToggle->isVisibleTo(menu)
			&& !accountsToggle->isDisabled()) {
			if (!key->isAutoRepeat()) {
				accountsToggle->clicked(Qt::NoModifier, Qt::LeftButton);
				accounts->finishAnimating();
				if (accounts->toggled()) {
					navigation->selectFirstWithin(accounts);
				}
			}
			return Result::Cancel;
		}
		return navigation->handle(key)
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
