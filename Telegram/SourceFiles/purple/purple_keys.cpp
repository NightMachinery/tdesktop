#include "purple/purple_keys.h"

#include "base/event_filter.h"
#include "base/weak_ptr.h"
#include "boxes/keys_legend.h"
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
#include "mainwindow.h"
#include "media/player/media_player_instance.h"
#include "purple/purple_chat_background.h"
#include "purple/purple_chat_palette.h"
#include "purple/purple_config.h"
#include "purple/purple_exact_search.h"
#include "purple/purple_keys_mx.h"
#include "purple/purple_keys_ui.h"
#include "purple/purple_message_hints.h"
#include "purple/purple_music_section.h"
#include "purple/purple_playlists_box.h"
#include "settings/settings_builder.h"
#include "settings/settings_key_navigation.h"
#include "storage/storage_shared_media.h"
#include "ui/abstract_button.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/layer_widget.h"
#include "ui/screen_reader_mode.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/elastic_scroll.h"
#include "ui/wrap/slide_wrap.h"
#include "window/themes/window_theme.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include "styles/style_settings.h"

#include <edn.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QTextEdit>

#include <algorithm>
#include <array>
#include <map>
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
constexpr auto kWhichKeyDelay = 280;
constexpr auto kFeedbackDurationMs = 3500;
constexpr auto kMaxCount = 200;
constexpr auto kLineScrollPx = 84;
#ifdef Q_OS_MAC
constexpr auto kCtrl = Qt::MetaModifier;
constexpr auto kSuper = Qt::ControlModifier;
#else
constexpr auto kCtrl = Qt::ControlModifier;
constexpr auto kSuper = Qt::MetaModifier;
#endif // Q_OS_MAC

rpl::event_stream<bool> EnabledChanges;
rpl::event_stream<> KeymapChanges;

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
	} else if (native == kVK_ANSI_Comma) {
		physical = modifiers.testFlag(Qt::ShiftModifier)
			? Qt::Key_Less
			: Qt::Key_Comma;
	} else if (native == kVK_ANSI_Semicolon) {
		physical = modifiers.testFlag(Qt::ShiftModifier)
			? Qt::Key_Colon
			: Qt::Key_Semicolon;
	} else if (native == kVK_ANSI_Slash) {
		physical = modifiers.testFlag(Qt::ShiftModifier)
			? Qt::Key_Question
			: Qt::Key_Slash;
	}
	if (physical != logical) {
		// macOS Option mutates letters into symbols (Option-x -> ≈) even on US layouts.
		if (modifiers.testFlag(Qt::AltModifier)) {
			return physical;
		}
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
	ToggleNightMode,
	MoveUp,
	MoveDown,
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
	FocusOtherPane,
	HistoryHalfUp,
	HistoryHalfDown,
	OlderMessagesPage,
	NewerMessagesPage,
	LatestMessages,
	UnreadMessages,
	OpenHistoryCalendar,
	Cheatsheet,
	Help,
	Run,
	DescribeChord,
	DescribeCommand,
	StockShortcuts,
	ReloadKeymap,
};

using ChatMediaType = Storage::SharedMediaType;

struct Command {
	Action action;
	const char *id;
	const char *category;
	const char *need;
	QString (*label)();
	bool repeatable = false;
	ChatMediaType mediaType = ChatMediaType::kCount;
};

const Command kCommands[] = {
	{ Action::EnterNormalMode, "enter-normal-mode", "mode", "",
		[] { return tr::lng_purple_keys_normal(tr::now); } },
	{ Action::EnterInsertMode, "enter-insert-mode", "mode", "",
		[] { return tr::lng_purple_keys_insert(tr::now); } },
	{ Action::Backgrounds, "chat-backgrounds-visibility-toggle", "toggle", "",
		[] { return tr::lng_purple_keys_backgrounds(tr::now); } },
	{ Action::ToggleNightMode, "toggle-night-mode", "toggle", "",
		[] { return tr::lng_menu_night_mode(tr::now); } },
	{ Action::MoveUp, "move-up", "navigate", "Needs chat list or scrollable conversation",
		[] { return u"Move or scroll up"_q; }, true },
	{ Action::MoveDown, "move-down", "navigate", "Needs chat list or scrollable conversation",
		[] { return u"Move or scroll down"_q; }, true },
	{ Action::PreviousChat, "chat-cursor-previous", "chats", "Needs chat list visible",
		[] { return tr::lng_purple_keys_chat_previous(tr::now); }, true },
	{ Action::NextChat, "chat-cursor-next", "chats", "Needs chat list visible",
		[] { return tr::lng_purple_keys_chat_next(tr::now); }, true },
	{ Action::OpenChat, "chat-cursor-open", "chats", "Needs chat list focus",
		[] { return tr::lng_purple_keys_chat_open(tr::now); } },
	{ Action::ChatPalette, "chat-palette", "chats", "",
		[] { return tr::lng_purple_chat_palette(tr::now); } },
	{ Action::PreviousFolder, "folder-previous", "chats", "Needs main chat list focus",
		[] { return tr::lng_purple_keys_folder_previous(tr::now); }, true },
	{ Action::NextFolder, "folder-next", "chats", "Needs main chat list focus",
		[] { return tr::lng_purple_keys_folder_next(tr::now); }, true },
	{ Action::SelectMessage, "message-hint-select", "messages", "Needs visible messages",
		[] { return tr::lng_purple_keys_message_select(tr::now); } },
	{ Action::CopyMessage, "message-hint-copy-text", "yank", "Needs visible messages",
		[] { return tr::lng_purple_keys_message_copy(tr::now); } },
	{ Action::Search, "search-messages", "search", "Unavailable in scheduled messages",
		[] { return tr::lng_shortcuts_search(tr::now); } },
	{ Action::OpenMainMenu, "open-main-menu", "open", "",
		[] { return tr::lng_main_menu(tr::now); } },
	{ Action::OpenSettings, "open-settings", "open", "",
		[] { return tr::lng_menu_settings(tr::now); } },
	{ Action::OpenSavedMessages, "open-saved-messages", "open", "",
		[] { return tr::lng_saved_messages(tr::now); } },
	{ Action::OpenArchive, "open-archived-chats", "open", "",
		[] { return tr::lng_shortcuts_archive(tr::now); } },
	{ Action::OpenContacts, "open-contacts", "open", "",
		[] { return tr::lng_shortcuts_contacts(tr::now); } },
	{ Action::OpenCalls, "open-calls", "open", "Unavailable in support mode",
		[] { return tr::lng_menu_calls(tr::now); } },
	{ Action::OpenDownloads, "open-downloads", "open", "",
		[] { return tr::lng_downloads_section(tr::now); } },
	{ Action::NewGroup, "new-group", "new", "Unavailable in support mode",
		[] { return tr::lng_create_group_title(tr::now); } },
	{ Action::NewChannel, "new-channel", "new", "Unavailable in support mode",
		[] { return tr::lng_create_channel_title(tr::now); } },
	{ Action::AddContact, "add-contact", "new", "",
		[] { return tr::lng_profile_add_contact(tr::now); } },
	{ Action::OpenScheduled, "open-scheduled-messages", "chat", "Needs an open chat",
		[] { return tr::lng_scheduled_messages(tr::now); } },
	{ Action::OpenMusic, "open-music", "playback", "",
		[] { return tr::lng_purple_music_title(tr::now); } },
	{ Action::OpenPlaylists, "open-playlists", "playback", "",
		[] { return tr::lng_purple_playlists_title(tr::now); } },
	{ Action::OpenChatInfo, "open-chat-info", "chat", "Needs an open chat",
		[] { return tr::lng_purple_keys_chat_info(tr::now); } },
	{ Action::OpenChatMedia, "open-chat-media", "chat media", "Needs an open chat",
		[] { return tr::lng_media_type_media(tr::now); }, false,
		ChatMediaType::PhotoVideo },
	{ Action::OpenChatMusic, "open-chat-music", "chat media", "Needs an open chat",
		[] { return tr::lng_purple_keys_chat_music(tr::now); }, false,
		ChatMediaType::MusicFile },
	{ Action::OpenChatFiles, "open-chat-files", "chat media", "Needs an open chat",
		[] { return tr::lng_media_type_files(tr::now); }, false,
		ChatMediaType::File },
	{ Action::OpenChatLinks, "open-chat-links", "chat media", "Needs an open chat",
		[] { return tr::lng_media_type_links(tr::now); }, false,
		ChatMediaType::Link },
	{ Action::OpenChatVoice, "open-chat-voice", "chat media", "Needs an open chat",
		[] { return tr::lng_media_type_audios(tr::now); }, false,
		ChatMediaType::RoundVoiceFile },
	{ Action::OpenChatGifs, "open-chat-gifs", "chat media", "Needs an open chat",
		[] { return tr::lng_media_type_gifs(tr::now); }, false,
		ChatMediaType::GIF },
	{ Action::OpenChatPolls, "open-chat-polls", "chat media", "Needs an open chat",
		[] { return tr::lng_media_type_polls(tr::now); }, false,
		ChatMediaType::Poll },
	{ Action::OpenPinnedMessages, "open-pinned-messages", "chat", "Needs an open chat",
		[] { return tr::lng_settings_events_pinned(tr::now); } },
	{ Action::AttachFiles, "attach-files", "chat", "Needs a chat you can send files to",
		[] { return tr::lng_attach(tr::now); } },
	{ Action::OpenChatMenu, "open-chat-menu", "chat", "Needs chat list or open chat",
		[] { return tr::lng_shortcuts_show_chat_menu(tr::now); } },
	{ Action::PreviewChat, "preview-chat", "chats", "Needs chat list focus",
		[] { return tr::lng_shortcuts_show_chat_preview(tr::now); } },
	{ Action::MessageMenu, "message-hint-menu", "messages", "Needs visible messages",
		[] { return tr::lng_purple_keys_message_menu(tr::now); } },
	{ Action::TogglePlayback, "playback-toggle", "playback", "Needs active audio in this account",
		[] { return tr::lng_shortcuts_media_play_pause(tr::now); } },
	{ Action::StopPlayback, "playback-stop", "playback", "Needs active audio in this account",
		[] { return tr::lng_shortcuts_media_stop(tr::now); } },
	{ Action::PreviousTrack, "playback-previous-track", "playback", "Needs active audio in this account",
		[] { return tr::lng_shortcuts_media_previous(tr::now); } },
	{ Action::NextTrack, "playback-next-track", "playback", "Needs active audio in this account",
		[] { return tr::lng_shortcuts_media_next(tr::now); } },
	{ Action::SeekBackward, "playback-seek-backward", "playback", "Needs active audio in this account",
		[] { return tr::lng_purple_shortcuts_seek_backward(tr::now); }, true },
	{ Action::SeekForward, "playback-seek-forward", "playback", "Needs active audio in this account",
		[] { return tr::lng_purple_shortcuts_seek_forward(tr::now); }, true },
	{ Action::Account1, "switch-account-1", "accounts", "Needs a signed-in 1st account",
		[] { return tr::lng_shortcuts_show_account_n(
			tr::now, lt_index, QString::number(1)); } },
	{ Action::Account2, "switch-account-2", "accounts", "Needs a signed-in 2nd account",
		[] { return tr::lng_shortcuts_show_account_n(
			tr::now, lt_index, QString::number(2)); } },
	{ Action::Account3, "switch-account-3", "accounts", "Needs a signed-in 3rd account",
		[] { return tr::lng_shortcuts_show_account_n(
			tr::now, lt_index, QString::number(3)); } },
	{ Action::Account4, "switch-account-4", "accounts", "Needs a signed-in 4th account",
		[] { return tr::lng_shortcuts_show_account_n(
			tr::now, lt_index, QString::number(4)); } },
	{ Action::Account5, "switch-account-5", "accounts", "Needs a signed-in 5th account",
		[] { return tr::lng_shortcuts_show_account_n(
			tr::now, lt_index, QString::number(5)); } },
	{ Action::Account6, "switch-account-6", "accounts", "Needs a signed-in 6th account",
		[] { return tr::lng_shortcuts_show_account_n(
			tr::now, lt_index, QString::number(6)); } },
	{ Action::FocusChats, "focus-chat-list", "window", "Needs chat list visible",
		[] { return tr::lng_purple_keys_focus_chats(tr::now); } },
	{ Action::FocusConversation, "focus-conversation", "window", "Needs an open conversation",
		[] { return tr::lng_purple_keys_focus_conversation(tr::now); } },
	{ Action::FocusOtherPane, "focus-other-pane", "window", "Needs two panes open",
		[] { return u"Switch between chat list and conversation"_q; } },
	{ Action::HistoryHalfUp, "history-half-up", "navigate", "Needs a scrollable conversation",
		[] { return u"Scroll conversation half-page up"_q; }, true },
	{ Action::HistoryHalfDown, "history-half-down", "navigate", "Needs a scrollable conversation",
		[] { return u"Scroll conversation half-page down"_q; }, true },
	{ Action::OlderMessagesPage, "history-page-older", "navigate", "Needs a scrollable conversation",
		[] { return tr::lng_purple_keys_history_page_older(tr::now); }, true },
	{ Action::NewerMessagesPage, "history-page-newer", "navigate", "Needs a scrollable conversation",
		[] { return tr::lng_purple_keys_history_page_newer(tr::now); }, true },
	{ Action::LatestMessages, "history-jump-to-latest", "navigate", "Needs a scrollable conversation",
		[] { return tr::lng_jump_to_bottom(tr::now); } },
	{ Action::UnreadMessages, "history-jump-to-unread", "navigate", "Needs an open chat",
		[] { return tr::lng_unread_bar_some(tr::now); } },
	{ Action::OpenHistoryCalendar, "history-open-calendar", "chat", "Needs an open chat",
		[] { return tr::lng_calendar(tr::now); } },
	{ Action::CopyMessageLink, "message-hint-copy-link", "yank", "Needs visible non-scheduled messages",
		[] { return tr::lng_purple_keys_message_link(tr::now); } },
	{ Action::CopyMessageUrl, "message-hint-copy-url", "yank", "Needs visible messages",
		[] { return tr::lng_context_copy_link(tr::now); } },
	{ Action::OpenMessageUrl, "message-hint-open-url", "messages", "Needs visible messages",
		[] { return tr::lng_polls_option_open_link_title(tr::now); } },
	{ Action::ReplyToMessage, "message-hint-reply", "messages", "Needs a chat you can reply in",
		[] { return tr::lng_context_reply_msg(tr::now); } },
	{ Action::EditMessage, "message-hint-edit", "messages", "Needs editable messages",
		[] { return tr::lng_context_edit_msg(tr::now); } },
	{ Action::Cheatsheet, "cheatsheet", "help", "",
		[] { return u"Keyboard cheatsheet"_q; } },
	{ Action::Help, "keys-help", "help", "",
		[] { return u"All active bindings"_q; } },
	{ Action::Run, "command-search", "help", "",
		[] { return tr::lng_purple_keys_run(tr::now); } },
	{ Action::DescribeChord, "key-describe", "help", "",
		[] { return tr::lng_purple_keys_describe_key(tr::now); } },
	{ Action::DescribeCommand, "command-describe", "help", "",
		[] { return tr::lng_purple_keys_describe_command(tr::now); } },
	{ Action::StockShortcuts, "stock-shortcuts", "help", "",
		[] { return u"Telegram stock shortcuts"_q; } },
	{ Action::ReloadKeymap, "reload-keymap", "help", "",
		[] { return u"Reload keys.edn"_q; } },
};

using Chord = std::vector<int>;

enum class MapKind {
	Normal,
	Insert,
	Chat,
	Chats,
	Music,
};

constexpr auto kCtrlG = int(kCtrl) | Qt::Key_G;

struct Binding {
	Action action;
	Chord keys;
	MapKind map = MapKind::Normal;
};

struct KeymapData {
	std::vector<Binding> bindings;
	std::vector<Chord> localleaders;
	std::map<std::pair<MapKind, Chord>, QString> labels;
	QSet<Action> keyless;
	QString userPath;
	QString error;
};

QString MapName(MapKind map) {
	switch (map) {
	case MapKind::Normal: return u"everywhere"_q;
	case MapKind::Insert: return u"in Insert mode"_q;
	case MapKind::Chat: return u"in a chat (after localleader)"_q;
	case MapKind::Chats: return u"in the chat list (after localleader)"_q;
	case MapKind::Music: return u"in Music (after localleader)"_q;
	}
	return u"everywhere"_q;
}

QString BareKeyLabel(int key, bool shifted = false) {
	if (key == Qt::Key_Space) {
		return u"SPC"_q;
	} else if (key == Qt::Key_Return) {
		return u"RET"_q;
	} else if (key == Qt::Key_Backspace) {
		return u"DEL"_q;
	} else if (key == Qt::Key_Escape) {
		return u"ESC"_q;
	} else if (key == Qt::Key_Tab) {
		return u"TAB"_q;
	} else if (key >= Qt::Key_A && key <= Qt::Key_Z) {
		return QString(QChar(key - Qt::Key_A + (shifted ? 'A' : 'a')));
	}
	return QKeySequence(key).toString(QKeySequence::NativeText);
}

QString KeyLabel(int stroke) {
	const auto combination = QKeyCombination::fromCombined(stroke);
	const auto mods = combination.keyboardModifiers();
	const auto key = combination.key();
	const auto isLetter = (key >= Qt::Key_A && key <= Qt::Key_Z);
	auto label = QString();
	if (mods.testFlag(kCtrl)) {
		label += u"C-"_q;
	}
	if (mods.testFlag(Qt::AltModifier)) {
		label += u"M-"_q;
	}
	if (mods.testFlag(kSuper)) {
		label += u"s-"_q;
	}
	const auto shift = mods.testFlag(Qt::ShiftModifier);
	if (shift && !isLetter) {
		label += u"S-"_q;
	}
	return label + BareKeyLabel(key, shift && isLetter);
}

QStringList ChordCaps(const Chord &keys) {
	auto caps = QStringList();
	caps.reserve(keys.size());
	for (const auto stroke : keys) {
		caps.push_back(KeyLabel(stroke));
	}
	return caps;
}

QString ChordLabel(const Chord &keys) {
	return ChordCaps(keys).join(u' ');
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

const Command &FindCommand(Action action) {
	const auto command = ranges::find(kCommands, action, &Command::action);
	Assert(command != std::end(kCommands));
	return *command;
}

std::optional<Action> FindActionById(const QString &name) {
	const auto command = ranges::find_if(kCommands, [&](const Command &item) {
		return name == QString::fromLatin1(item.id);
	});
	return (command != std::end(kCommands))
		? std::optional(command->action)
		: std::nullopt;
}

bool StartsWith(const Chord &full, const Chord &prefix) {
	return full.size() >= prefix.size()
		&& std::equal(prefix.begin(), prefix.end(), full.begin());
}

bool StartsWith(const Binding &binding, const Chord &prefix) {
	return StartsWith(binding.keys, prefix);
}

int ParseKey(const QString &token, bool &impliedShift) {
	impliedShift = false;
	if (token.compare(u"SPC"_q, Qt::CaseInsensitive) == 0) {
		return Qt::Key_Space;
	} else if (token.compare(u"Enter"_q, Qt::CaseInsensitive) == 0
			|| token.compare(u"RET"_q, Qt::CaseInsensitive) == 0) {
		return Qt::Key_Return;
	} else if (token.compare(u"DEL"_q, Qt::CaseInsensitive) == 0
			|| token.compare(u"Backspace"_q, Qt::CaseInsensitive) == 0) {
		return Qt::Key_Backspace;
	} else if (token.compare(u"<up>"_q, Qt::CaseInsensitive) == 0) {
		return Qt::Key_Up;
	} else if (token.compare(u"<down>"_q, Qt::CaseInsensitive) == 0) {
		return Qt::Key_Down;
	} else if (token.compare(u"<left>"_q, Qt::CaseInsensitive) == 0) {
		return Qt::Key_Left;
	} else if (token.compare(u"<right>"_q, Qt::CaseInsensitive) == 0) {
		return Qt::Key_Right;
	} else if (token.compare(u"<prior>"_q, Qt::CaseInsensitive) == 0
			|| token.compare(u"PgUp"_q, Qt::CaseInsensitive) == 0) {
		return Qt::Key_PageUp;
	} else if (token.compare(u"<next>"_q, Qt::CaseInsensitive) == 0
			|| token.compare(u"PgDown"_q, Qt::CaseInsensitive) == 0) {
		return Qt::Key_PageDown;
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
	} else if (key >= 'A' && key <= 'Z') {
		impliedShift = true;
		return Qt::Key_A + (key - 'A');
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
	auto impliedShift = false;
	const auto key = ParseKey(text, impliedShift);
	if (!key || (ShiftedSymbol(key) && mods.testFlag(Qt::ShiftModifier))) {
		return 0;
	}
	if (impliedShift) {
		mods |= Qt::ShiftModifier;
	}
	return QKeyCombination(mods, Qt::Key(key)).toCombined();
}

Chord ParseChord(const QString &text, MapKind map) {
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
	if (map == MapKind::Insert && !HasShortcutModifier(result.front())) {
		return {};
	}
	// Bare leading digits are reserved for numeric repeat counts in Normal mode.
	if (map == MapKind::Normal) {
		const auto first = QKeyCombination::fromCombined(result.front());
		if (first.keyboardModifiers() == Qt::NoModifier
			&& first.key() >= Qt::Key_0 && first.key() <= Qt::Key_9) {
			return {};
		}
	}
	return result;
}

bool IsProtectedHelpAction(Action action) {
	return action == Action::Help || action == Action::Run
		|| action == Action::DescribeChord || action == Action::DescribeCommand;
}

std::optional<MapKind> ParseMapKind(const QString &name) {
	if (name == u"normal"_q) return MapKind::Normal;
	if (name == u"insert"_q) return MapKind::Insert;
	if (name == u"chat"_q) return MapKind::Chat;
	if (name == u"chats"_q) return MapKind::Chats;
	if (name == u"music"_q) return MapKind::Music;
	return std::nullopt;
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

std::optional<QString> EdnString(const edn_value_t *value) {
	auto size = size_t(0);
	const auto ptr = edn_string_get(value, &size);
	return ptr ? std::optional(QString::fromUtf8(ptr, qsizetype(size))) : std::nullopt;
}

bool ParseEdnOverlay(
		const QByteArray &bytes,
		KeymapData &data,
		bool isUserOverlay,
		QString &errorOut) {
	if (bytes.isEmpty() || bytes.contains('\0')) {
		errorOut = u"empty or binary file"_q;
		return false;
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
		errorOut = u"expected one top-level EDN map"_q;
		return false;
	}
	auto end = size_t();
	if (!edn_source_position(root.get(), nullptr, &end)
		|| end > size_t(bytes.size())) {
		errorOut = u"invalid EDN source bounds"_q;
		return false;
	}
	if (end < size_t(bytes.size())) {
		auto tailOptions = options;
		tailOptions.eof_value = root.get();
		const auto tail = edn_read_with_options(
			bytes.constData() + end, size_t(bytes.size()) - end, &tailOptions);
		if (tail.value != root.get()) {
			edn_free(tail.value);
			errorOut = u"unexpected trailing form after top-level map"_q;
			return false;
		}
		if (tail.error != EDN_OK) {
			errorOut = u"syntax error after top-level map"_q;
			return false;
		}
	}

	constexpr auto kEntryLimit = 256;
	auto editedCount = 0;
	auto seen = std::vector<std::pair<MapKind, Chord>>();

	for (auto i = size_t(0); i < edn_map_count(root.get()); ++i) {
		const auto topKey = EdnKeyword(edn_map_get_key(root.get(), i));
		const auto topVal = edn_map_get_value(root.get(), i);
		if (topKey.isEmpty()) {
			errorOut = u"top-level key must be an unqualified keyword"_q;
			return false;
		}
		if (topKey == u"localleader"_q) {
			if (edn_type(topVal) != EDN_TYPE_VECTOR) {
				errorOut = u":localleader must be a vector of chord strings"_q;
				return false;
			}
			auto leaders = std::vector<Chord>();
			for (auto j = size_t(0); j < edn_vector_count(topVal); ++j) {
				const auto s = EdnString(edn_vector_get(topVal, j));
				if (!s) {
					errorOut = u":localleader entries must be strings"_q;
					return false;
				}
				const auto chord = ParseChord(*s, MapKind::Normal);
				if (chord.empty() || ranges::contains(leaders, chord)) {
					errorOut = u"invalid or duplicate :localleader chord \""_q + *s + u"\""_q;
					return false;
				}
				leaders.push_back(chord);
			}
			if (leaders.empty()) {
				errorOut = u":localleader cannot be empty"_q;
				return false;
			}
			data.localleaders = std::move(leaders);
			continue;
		}
		if (topKey == u"keyless"_q) {
			if (edn_type(topVal) != EDN_TYPE_VECTOR) {
				errorOut = u":keyless must be a vector of command keywords"_q;
				return false;
			}
			for (auto j = size_t(0); j < edn_vector_count(topVal); ++j) {
				const auto id = EdnKeyword(edn_vector_get(topVal, j));
				const auto act = FindActionById(id);
				if (!act) {
					errorOut = u":keyless unknown command :"_q + id;
					return false;
				}
				data.keyless.insert(*act);
			}
			continue;
		}
		const auto mapKind = ParseMapKind(topKey);
		if (!mapKind || edn_type(topVal) != EDN_TYPE_MAP) {
			errorOut = u"unknown map :"_q + topKey + u" (expected :normal :insert :chat :chats :music)"_q;
			return false;
		}
		for (auto j = size_t(0); j < edn_map_count(topVal); ++j) {
			const auto chordStr = EdnString(edn_map_get_key(topVal, j));
			if (!chordStr) {
				errorOut = u"chord key in :"_q + topKey + u" must be a string"_q;
				return false;
			}
			auto chord = ParseChord(*chordStr, *mapKind);
			if (chord.empty()) {
				errorOut = u"invalid chord \""_q + *chordStr + u"\" in :"_q + topKey;
				return false;
			}
			if (++editedCount > kEntryLimit) {
				errorOut = u"too many entries in keymap file"_q;
				return false;
			}
			const auto val = edn_map_get_value(topVal, j);
			if (const auto labelStr = EdnString(val)) {
				data.labels[{ *mapKind, chord }] = *labelStr;
				continue;
			}
			if (ranges::contains(seen, std::pair(*mapKind, chord))) {
				errorOut = u"duplicate chord \""_q + *chordStr + u"\" in :"_q + topKey;
				return false;
			}
			seen.emplace_back(*mapKind, chord);

			const auto current = ranges::find_if(data.bindings, [&](const Binding &b) {
				return b.map == *mapKind && b.keys == chord;
			});
			if (isUserOverlay && current != data.bindings.end()
					&& IsProtectedHelpAction(current->action)) {
				errorOut = u"cannot override protected help chord \""_q + *chordStr + u"\""_q;
				return false;
			}
			if (edn_type(val) == EDN_TYPE_NIL) {
				if (current != data.bindings.end()) {
					data.bindings.erase(current);
				}
				continue;
			}
			const auto cmdName = EdnKeyword(val);
			const auto action = FindActionById(cmdName);
			if (!action) {
				errorOut = u"unknown command :"_q + cmdName + u" on \""_q + *chordStr + u"\""_q;
				return false;
			}
			if (current != data.bindings.end()) {
				current->action = *action;
			} else {
				data.bindings.push_back({ *action, std::move(chord), *mapKind });
			}
		}
	}

	// Validate no command/prefix collision within any single map.
	for (const auto &a : data.bindings) {
		for (const auto &b : data.bindings) {
			if (a.map == b.map && a.keys.size() < b.keys.size() && StartsWith(b, a.keys)) {
				errorOut = u"prefix collision between \""_q + ChordLabel(a.keys)
					+ u"\" and \""_q + ChordLabel(b.keys) + u"\""_q;
				return false;
			}
		}
	}
	// Validate localleader chords do not collide with :normal commands or prefixes.
	for (const auto &ll : data.localleaders) {
		for (const auto &b : data.bindings) {
			if (b.map == MapKind::Normal
					&& (StartsWith(b.keys, ll) || StartsWith(ll, b.keys))) {
				errorOut = u":localleader \""_q + ChordLabel(ll)
					+ u"\" collides with :normal \""_q + ChordLabel(b.keys) + u"\""_q;
				return false;
			}
		}
	}
	// Require at least one :insert binding that enters Normal mode.
	const auto canEnterNormal = ranges::any_of(data.bindings, [](const Binding &b) {
		return b.map == MapKind::Insert && b.action == Action::EnterNormalMode;
	});
	if (!canEnterNormal) {
		errorOut = u":insert must keep at least one binding for :enter-normal-mode"_q;
		return false;
	}
	return true;
}

constexpr auto kStarterKeysTemplate =
"; Purple Telegram modal keymap overlay (shared evilify EDN format).\n"
"; Entries here overlay the built-in defaults and reload live on save.\n"
"; Remove leading \";;\" to enable an example. If any entry is invalid,\n"
"; built-in defaults stay active and the status line names the error.\n"
";\n"
"; Each map binds a Doom chord string to:\n"
";   :command-id   bind chord to command (SPC h f lists every command ID)\n"
";   nil           unbind built-in chord in that map\n"
";   \"label\"       rename a prefix group in which-key\n"
";\n"
"; Maps: :normal (global), :insert (typing: C-/M-/s- first stroke only),\n"
"; and localleader context maps after \",\" or \"SPC m\": :chat, :chats, :music.\n"
"{\n"
" ;; :localleader [\",\" \"SPC m\"]\n"
" ;; :normal {\"SPC o a\" nil               ; unbind Archived Chats\n"
" ;;          \"C-c m\"   :open-music       ; add a custom shortcut\n"
" ;;          \"SPC b\"   \"chat\"}           ; rename a which-key prefix\n"
" ;; :chat   {\"k\"       :open-pinned-messages}\n"
"}\n";

void EnsureStarterKeysFile(const QString &path) {
	if (QFileInfo::exists(path)) {
		return;
	}
	const auto dir = QFileInfo(path).dir();
	if (!dir.exists()) {
		dir.mkpath(u"."_q);
	}
	auto file = QFile(path);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(kStarterKeysTemplate);
	}
}

KeymapData LoadKeymapData() {
	auto defaults = KeymapData();
	auto bundled = QFile(u":/purple/default-keys.edn"_q);
	const auto loaded = bundled.open(QIODevice::ReadOnly);
	Expects(loaded);
	auto defaultErr = QString();
	const auto okDefault = ParseEdnOverlay(bundled.readAll(), defaults, false, defaultErr);
	Expects(okDefault);

	const auto directory = ConfigDirectory();
	const auto userPath = directory + u"/keys.edn"_q;
	defaults.userPath = userPath;
	if (Enabled() && !QFileInfo::exists(directory + u"/keys.json"_q)) {
		EnsureStarterKeysFile(userPath);
	}
	const auto info = QFileInfo(userPath);
	if (!info.exists() || !info.isFile()) {
		return defaults;
	}
	auto file = QFile(userPath);
	constexpr auto kByteLimit = 32 * 1024;
	if (!file.open(QIODevice::ReadOnly)) {
		defaults.error = u"could not open keys.edn"_q;
		return defaults;
	}
	const auto bytes = file.read(kByteLimit + 1);
	if (file.error() != QFileDevice::NoError || bytes.size() > kByteLimit) {
		defaults.error = u"keys.edn exceeds 32 KiB limit"_q;
		return defaults;
	}
	auto candidate = defaults;
	auto userErr = QString();
	if (ParseEdnOverlay(bytes, candidate, true, userErr)) {
		return candidate;
	}
	defaults.error = userErr;
	LOG(("Purple modal keys: invalid keys.edn (%1), using built-in defaults.").arg(userErr));
	return defaults;
}

KeymapData &KeymapStore() {
	static auto store = LoadKeymapData();
	return store;
}

const KeymapData &Keymap() {
	return KeymapStore();
}

void ReloadKeymapStore() {
	KeymapStore() = LoadKeymapData();
	KeymapChanges.fire({});
}

QString MxHistoryPath() {
	return ConfigDirectory() + u"/mx-history.json"_q;
}

ModalMx::History &MxHistory() {
	static auto history = [] {
		auto f = QFile(MxHistoryPath());
		if (f.open(QIODevice::ReadOnly)) {
			return ModalMx::History::Deserialize(f.read(64 * 1024));
		}
		return ModalMx::History();
	}();
	return history;
}

void SaveMxHistory() {
	const auto path = MxHistoryPath();
	const auto dir = QFileInfo(path).dir();
	if (!dir.exists()) {
		dir.mkpath(u"."_q);
	}
	auto f = QFile(path);
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		f.write(MxHistory().serialize());
	}
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
	QString paneLabel;
	std::optional<MapKind> contextMap;
	QString contextName;
	Window::SessionController *controller = nullptr;
};

Target FindTarget(Window::SessionController *owner, QWidget *focus) {
	const auto forSection = [=](
			auto *section,
			QString paneName,
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
				.paneLabel = std::move(paneName),
				.contextMap = chatActions ? std::optional(MapKind::Chat) : std::nullopt,
				.contextName = chatActions ? u"chat"_q : QString(),
				.controller = owner,
			}
			: Target();
	};
	if (MusicAllowsModalKeys(owner, focus)) {
		return {
			.focus = focus,
			.paneLabel = u"Music"_q,
			.contextMap = MapKind::Music,
			.contextName = u"music"_q,
			.controller = owner,
		};
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
				.paneLabel = u"Chats"_q,
				.contextMap = MapKind::Chats,
				.contextName = u"chats"_q,
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
					.paneLabel = u"Chat"_q,
					.contextMap = MapKind::Chat,
					.contextName = u"chat"_q,
					.controller = owner,
				}
				: Target();
		}
		if (const auto chat = dynamic_cast<HistoryView::ChatWidget*>(widget)) {
			auto target = forSection(chat, u"Topic"_q, false, true);
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
			return forSection(pinned, u"Pinned"_q);
		}
		if (const auto scheduled = dynamic_cast<HistoryView::ScheduledWidget*>(widget)) {
			auto target = forSection(scheduled, u"Scheduled"_q, true);
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

Dialogs::InnerWidget *ChatsInnerList(Window::SessionController *controller) {
	const auto dialogs = ChatsPane(controller);
	return dialogs ? dialogs->purpleKeysList(dialogs) : nullptr;
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
	case Action::MoveUp:
	case Action::MoveDown:
		return target.list != nullptr || HistoryScroll(target) != nullptr;
	case Action::PreviousChat:
	case Action::NextChat:
		return target.list != nullptr || ChatsInnerList(target.controller) != nullptr;
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
	case Action::HistoryHalfUp:
	case Action::HistoryHalfDown:
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
	case Action::FocusOtherPane:
		return Available(Action::FocusChats, target)
			|| Available(Action::FocusConversation, target);
	default: return true;
	}
}

bool IsSearchableAction(Action action) {
	return action != Action::Help
		&& action != Action::Cheatsheet
		&& action != Action::Run
		&& action != Action::DescribeChord
		&& action != Action::DescribeCommand;
}

// Builds the effective binding list for the given mode & active context:
// localleader chords expand the active context map into effective Normal chords.
struct EffectiveBinding {
	Action action;
	Chord keys;
	MapKind sourceMap = MapKind::Normal;
	bool viaLocalleader = false;
};

std::vector<EffectiveBinding> EffectiveBindings(
		bool normalMode,
		std::optional<MapKind> contextMap) {
	const auto &km = Keymap();
	auto result = std::vector<EffectiveBinding>();
	if (!normalMode) {
		for (const auto &b : km.bindings) {
			if (b.map == MapKind::Insert) {
				result.push_back({ b.action, b.keys, MapKind::Insert, false });
			}
		}
		return result;
	}
	if (contextMap) {
		for (const auto &ll : km.localleaders) {
			for (const auto &b : km.bindings) {
				if (b.map != *contextMap) {
					continue;
				}
				auto combined = ll;
				combined.insert(combined.end(), b.keys.begin(), b.keys.end());
				result.push_back({ b.action, std::move(combined), *contextMap, true });
			}
		}
	}
	for (const auto &b : km.bindings) {
		if (b.map == MapKind::Normal) {
			result.push_back({ b.action, b.keys, MapKind::Normal, false });
		}
	}
	return result;
}

bool IsLocalleaderPrefix(const Chord &prefix) {
	for (const auto &ll : Keymap().localleaders) {
		if (StartsWith(prefix, ll)) {
			return true;
		}
	}
	return false;
}

bool IsExactLocalleader(const Chord &prefix) {
	return ranges::contains(Keymap().localleaders, prefix);
}

QString PrefixLabel(MapKind map, const Chord &prefix) {
	const auto &km = Keymap();
	if (const auto it = km.labels.find({ map, prefix }); it != km.labels.end()) {
		return it->second;
	}
	if (IsExactLocalleader(prefix)) {
		return u"localleader"_q;
	}
	return QString();
}

std::vector<QStringList> ChordsForAction(
		Action action,
		const std::vector<EffectiveBinding> &effective) {
	auto out = std::vector<QStringList>();
	for (const auto &b : effective) {
		if (b.action == action) {
			auto caps = ChordCaps(b.keys);
			if (!ranges::contains(out, caps)) {
				out.push_back(std::move(caps));
			}
		}
	}
	return out;
}

QString FirstLocalleaderLabel() {
	const auto &ll = Keymap().localleaders;
	return ll.empty() ? u","_q : ChordLabel(ll.front());
}

enum class KeyUse {
	Pass,
	ExitInsertOrBackOut,
	Cancel,
	CountDigit,
	PrefixBackspace,
	SwallowEmptyLocal,
	Capture,
	Hint,
	Ignore,
};

class Keys final : public QObject {
public:
	explicit Keys(not_null<MainWindow*> window);
	~Keys() override;

	bool handle(QObject *receiver, QEvent *event);
	[[nodiscard]] bool ownsChatListFocus() const;
	[[nodiscard]] int statusBarHeight() const;
	void updateLayout(QRect body);
	void raiseChrome();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	[[nodiscard]] Window::SessionController *owner() const;
	[[nodiscard]] Target currentTarget(QObject *receiver) const;
	[[nodiscard]] Target activeTarget() const;
	[[nodiscard]] bool chromeVisible() const;
	[[nodiscard]] KeyUse classify(
		const QKeyEvent &event,
		int key,
		const Target &target) const;
	void clearPrefix();
	void enterNormal(bool silent = true);
	void enterInsert(bool silent = true);
	void showFeedback(QString text, bool isError = false);
	void refreshStatusBar();
	void showWhichKey();
	void hideWhichKey();
	void stroke(int key, const Target &target);
	void dispatch(Action action, const Target &target, int count);
	void showHelpLayer(bool startOnAllBindings, const Target &target);
	void showDescribeKeyResult(const Chord &chord, const Target &target);
	void showDescribeCommandResult(Action action, const Target &target);
	void showPalette(bool describe, const Target &target, bool fromInsert);
	void watchUserKeymap();

	const not_null<MainWindow*> _window;
	QPointer<ModalUi::ModalStatusBar> _statusBar;
	QPointer<ModalUi::WhichKeyOverlay> _whichKey;
	QTimer _whichKeyTimer;
	QTimer _feedbackTimer;
	QFileSystemWatcher _keymapWatcher;
	MessageHints _hints;

	Chord _prefix;
	std::vector<EffectiveBinding> _frozenEffective;
	int _count = 0;
	QSet<int> _held;
	bool _normal = true; // Starts in Normal mode when modal keys are enabled.
	bool _inspect = false;
	bool _swallowNext = false;
	enum class ActiveLayer {
		None,
		Cheatsheet,
		AllBindings,
		Palette,
		DescribeCommandPicker,
		DescribeKey,
		DescribeCommand,
		StockShortcuts,
	};
	ActiveLayer _activeLayer = ActiveLayer::None;
	bool _describeKeyHasDetails = false;
	QString _feedbackText;
	bool _feedbackError = false;
	QRect _lastBody;
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

Keys *EnsureKeys(not_null<MainWindow*> window) {
	if (const auto existing = FindKeys(window.get())) {
		return existing;
	}
	return new Keys(window);
}

Keys::Keys(not_null<MainWindow*> window)
: QObject(window)
, _window(window) {
	setObjectName(QString::fromLatin1(kKeysObjectName));
	_statusBar = Ui::CreateChild<ModalUi::ModalStatusBar>(_window->bodyWidget());
	_whichKey = Ui::CreateChild<ModalUi::WhichKeyOverlay>(_window->bodyWidget());
	_whichKey->hide();

	_whichKeyTimer.setSingleShot(true);
	QObject::connect(&_whichKeyTimer, &QTimer::timeout, this, [=] { showWhichKey(); });
	_feedbackTimer.setSingleShot(true);
	QObject::connect(&_feedbackTimer, &QTimer::timeout, this, [=] {
		_feedbackText.clear();
		_feedbackError = false;
		refreshStatusBar();
	});

	qApp->installEventFilter(this);

	QObject::connect(qApp, &QApplication::focusChanged, this, [=](QWidget *old, QWidget *now) {
		const auto inThisWindow = (old && old->window() == _window)
			|| (now && now->window() == _window);
		if (!inThisWindow) {
			return;
		}
		clearPrefix();
		// Stay in Normal mode across eligible surfaces; reflect Insert if focus moved to an input outside Normal.
		if (now && now->window() == _window && _activeLayer == ActiveLayer::None) {
			refreshStatusBar();
		}
	});
	QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged,
		this, [=](Qt::ApplicationState state) {
			if (state != Qt::ApplicationActive) {
				_held.clear();
				clearPrefix();
			}
		});

	Core::App().passcodeLockValue() | rpl::on_next([=](bool) {
		clearPrefix();
		_window->updateControlsGeometry();
		refreshStatusBar();
	}, _lifetime);

	EnabledChanges.events() | rpl::on_next([=](bool enabled) {
		if (enabled) {
			ReloadKeymapStore();
			watchUserKeymap();
			_normal = true;
		}
		clearPrefix();
		_window->updateControlsGeometry();
		refreshStatusBar();
	}, _lifetime);

	KeymapChanges.events() | rpl::on_next([=] {
		clearPrefix();
		if (!Keymap().error.isEmpty()) {
			showFeedback(u"keys.edn error: "_q + Keymap().error, true);
		}
		refreshStatusBar();
	}, _lifetime);

	window->controller().boxShownValue() | rpl::on_next([=](bool shown) {
		if (!shown) {
			_activeLayer = ActiveLayer::None;
			_describeKeyHasDetails = false;
		}
		clearPrefix();
		refreshStatusBar();
	}, _lifetime);

	window->controller().sessionControllerValue(
	) | rpl::on_next([=](Window::SessionController *owner) {
		++_revision;
		clearPrefix();
		_sessionLifetime.destroy();
		_window->updateControlsGeometry();
		refreshStatusBar();
		if (!owner) {
			return;
		}
		rpl::merge(
			owner->activeChatEntryChanges() | rpl::to_empty,
			owner->activeChatsFilter() | rpl::to_empty,
			owner->openedFolder().changes() | rpl::to_empty,
			owner->shownForum().changes() | rpl::to_empty,
			owner->openedCommunity().changes() | rpl::to_empty
		) | rpl::on_next([=] {
			// Keep Normal mode across chat/folder switches; only clear in-flight sequences.
			++_revision;
			clearPrefix();
			refreshStatusBar();
		}, _sessionLifetime);
	}, _lifetime);

	watchUserKeymap();
	QObject::connect(&_keymapWatcher, &QFileSystemWatcher::fileChanged, this, [=](const QString &) {
		QTimer::singleShot(80, this, [=] {
			ReloadKeymapStore();
			watchUserKeymap();
			if (Keymap().error.isEmpty()) {
				showFeedback(u"Reloaded keys.edn"_q);
			}
		});
	});
	QObject::connect(&_keymapWatcher, &QFileSystemWatcher::directoryChanged, this, [=](const QString &) {
		if (QFileInfo::exists(Keymap().userPath)
				&& !_keymapWatcher.files().contains(Keymap().userPath)) {
			watchUserKeymap();
			ReloadKeymapStore();
		}
	});

	if (Enabled() && !Keymap().error.isEmpty()) {
		showFeedback(u"keys.edn error: "_q + Keymap().error, true);
	}
	_window->updateControlsGeometry();
	refreshStatusBar();
}

Keys::~Keys() {
	if (qApp) {
		qApp->removeEventFilter(this);
	}
}

void Keys::watchUserKeymap() {
	const auto path = Keymap().userPath;
	if (path.isEmpty()) {
		return;
	}
	const auto dir = QFileInfo(path).absolutePath();
	if (QFileInfo::exists(dir) && !_keymapWatcher.directories().contains(dir)) {
		_keymapWatcher.addPath(dir);
	}
	if (QFileInfo::exists(path) && !_keymapWatcher.files().contains(path)) {
		_keymapWatcher.addPath(path);
	}
}

bool Keys::eventFilter(QObject *watched, QEvent *event) {
	// Clicking directly into a message composer enters Insert mode so mouse typing works immediately.
	if (event->type() == QEvent::MouseButtonPress
			&& Enabled()
			&& _normal
			&& !_window->controller().isLayerShown()
			&& watched->isWidgetType()) {
		const auto widget = static_cast<QWidget*>(watched);
		if (widget->window() == _window
				&& (qobject_cast<QTextEdit*>(widget)
					|| (widget->parentWidget()
						&& qobject_cast<QTextEdit*>(widget->parentWidget())))) {
			const auto target = FindTarget(owner(), qobject_cast<QTextEdit*>(widget)
				? widget
				: widget->parentWidget());
			if (target.focus && target.history) {
				enterInsert(true);
			}
		}
	}
	return QObject::eventFilter(watched, event);
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

bool Keys::chromeVisible() const {
	return Enabled()
		&& !Core::App().passcodeLocked()
		&& !_window->controller().locked()
		&& _window->controller().sessionController() != nullptr;
}

int Keys::statusBarHeight() const {
	return chromeVisible() ? ModalUi::ModalStatusBar::kHeight : 0;
}

void Keys::updateLayout(QRect body) {
	_lastBody = body;
	if (!_statusBar || !_whichKey) {
		return;
	}
	if (!chromeVisible()) {
		_statusBar->hide();
		_whichKey->hide();
		return;
	}
	const auto h = ModalUi::ModalStatusBar::kHeight;
	_statusBar->setGeometry(body.x(), body.y() + body.height() - h, body.width(), h);
	_statusBar->show();
	_statusBar->raise();
	if (_whichKey->isVisible()) {
		const auto above = QRect(body.x(), body.y(), body.width(), std::max(0, body.height() - h));
		_whichKey->updateBounds(above);
	}
}

void Keys::raiseChrome() {
	if (_statusBar && _statusBar->isVisible()) {
		_statusBar->raise();
	}
	if (_whichKey && _whichKey->isVisible()) {
		_whichKey->raise();
	}
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

Target Keys::activeTarget() const {
	const auto focus = QApplication::focusWidget();
	const auto current = _window->controller().sessionController();
	return (focus && focus->window() == _window && current)
		? FindTarget(current, focus)
		: Target();
}

bool Keys::ownsChatListFocus() const {
	return _normal
		&& Enabled()
		&& currentTarget(QApplication::focusWidget()).list;
}

void Keys::showFeedback(QString text, bool isError) {
	_feedbackText = std::move(text);
	_feedbackError = isError;
	_feedbackTimer.start(isError ? 7000 : kFeedbackDurationMs);
	refreshStatusBar();
}

void Keys::enterNormal(bool silent) {
	clearPrefix();
	_normal = true;
	if (!silent) {
		showFeedback(tr::lng_purple_keys_normal(tr::now));
	} else {
		refreshStatusBar();
	}
}

void Keys::enterInsert(bool silent) {
	clearPrefix();
	_normal = false;
	if (!silent) {
		showFeedback(tr::lng_purple_keys_insert(tr::now));
	} else {
		refreshStatusBar();
	}
}

void Keys::hideWhichKey() {
	_whichKeyTimer.stop();
	if (_whichKey) {
		_whichKey->hide();
	}
}

void Keys::clearPrefix() {
	_hints.clear();
	_prefix.clear();
	_frozenEffective.clear();
	_count = 0;
	_inspect = false;
	_swallowNext = false;
	hideWhichKey();
	refreshStatusBar();
}

void Keys::refreshStatusBar() {
	if (!_statusBar) {
		return;
	}
	if (!chromeVisible()) {
		_statusBar->hide();
		hideWhichKey();
		return;
	}
	const auto target = activeTarget();
	auto state = ModalUi::StatusState();
	if (_activeLayer != ActiveLayer::None) {
		switch (_activeLayer) {
		case ActiveLayer::Cheatsheet:
			state.badge = ModalUi::BadgeKind::Help;
			state.badgeText = u"HELP"_q;
			state.paneLabel = u"Keyboard Cheatsheet"_q;
			state.rightCue = u"TAB all bindings  ·  j/k scroll  ·  / filter  ·  q/ESC close"_q;
			break;
		case ActiveLayer::AllBindings:
			state.badge = ModalUi::BadgeKind::Insert;
			state.badgeText = u"FILTER"_q;
			state.paneLabel = u"All Active Bindings"_q;
			state.rightCue = u"TAB cheatsheet  ·  type to filter  ·  ESC close"_q;
			break;
		case ActiveLayer::Palette:
			state.badge = ModalUi::BadgeKind::Palette;
			state.badgeText = u"M-x"_q;
			state.paneLabel = u"Command Palette"_q;
			state.rightCue = u"C-n/p move  ·  RET run  ·  M-p/n history  ·  ESC close"_q;
			break;
		case ActiveLayer::DescribeCommandPicker:
			state.badge = ModalUi::BadgeKind::Palette;
			state.badgeText = u"SPC h f"_q;
			state.paneLabel = u"Describe Command"_q;
			state.rightCue = u"C-n/p move  ·  RET inspect  ·  ESC close"_q;
			break;
		case ActiveLayer::DescribeKey:
			state.badge = ModalUi::BadgeKind::Help;
			state.badgeText = u"HELP"_q;
			state.paneLabel = u"Describe Key"_q;
			state.rightCue = _describeKeyHasDetails
				? u"RET command details  ·  q/ESC close"_q
				: u"q/ESC close"_q;
			break;
		case ActiveLayer::DescribeCommand:
			state.badge = ModalUi::BadgeKind::Help;
			state.badgeText = u"HELP"_q;
			state.paneLabel = u"Describe Command"_q;
			state.rightCue = u"q/ESC close"_q;
			break;
		case ActiveLayer::StockShortcuts:
			state.badge = ModalUi::BadgeKind::Help;
			state.badgeText = u"HELP"_q;
			state.paneLabel = u"Shortcuts & Gestures"_q;
			state.rightCue = u"ESC close"_q;
			break;
		case ActiveLayer::None:
			break;
		}
		_statusBar->setState(std::move(state));
		return;
	}
	if (_inspect) {
		state.badge = ModalUi::BadgeKind::DescribeKey;
		state.badgeText = u"KEY?"_q;
		state.rightCue = u"ESC cancel"_q;
	} else if (_hints.active()) {
		state.badge = ModalUi::BadgeKind::Hints;
		state.badgeText = u"HINTS"_q;
		state.rightCue = u"DEL back  ·  ESC cancel"_q;
	} else if (!_prefix.empty() && IsLocalleaderPrefix(_prefix)) {
		state.badge = ModalUi::BadgeKind::Local;
		state.badgeText = target.contextName.isEmpty()
			? u"LOCAL"_q
			: (u"LOCAL · "_q + target.contextName.toUpper());
		state.rightCue = u"DEL back  ·  ESC cancel"_q;
	} else if (!_prefix.empty() && _prefix.front() == Qt::Key_Space) {
		state.badge = ModalUi::BadgeKind::Leader;
		state.badgeText = u"LEADER"_q;
		state.rightCue = u"DEL back  ·  ESC cancel"_q;
	} else {
		const auto focus = QApplication::focusWidget();
		const auto inForeignInput = focus
			&& !target.focus
			&& focus->testAttribute(Qt::WA_InputMethodEnabled);
		if (_normal && !inForeignInput) {
			state.badge = ModalUi::BadgeKind::Normal;
			state.badgeText = u"NORMAL"_q;
		} else {
			state.badge = ModalUi::BadgeKind::Insert;
			state.badgeText = u"INSERT"_q;
		}
		if (!_prefix.empty()) {
			state.rightCue = u"DEL back  ·  ESC cancel"_q;
		}
	}

	state.paneLabel = target.paneLabel;
	if (!target.contextName.isEmpty()) {
		state.contextChip = FirstLocalleaderLabel() + u" "_q + target.contextName;
	}

	auto pending = QString();
	if (_count > 0) {
		pending += QString::number(_count);
	}
	if (!_prefix.empty()) {
		if (!pending.isEmpty()) {
			pending += u' ';
		}
		pending += ChordLabel(_prefix) + u" -"_q;
		state.pendingGroup = IsLocalleaderPrefix(_prefix)
			? (target.contextName.isEmpty() ? u"localleader"_q : target.contextName)
			: PrefixLabel(MapKind::Normal, _prefix);
	}
	state.pendingText = pending;

	if (!_feedbackText.isEmpty()) {
		state.message = _feedbackText;
		state.messageIsError = _feedbackError;
	} else if (_inspect) {
		state.message = tr::lng_purple_keys_capture(tr::now);
	} else if (!Keymap().error.isEmpty()) {
		state.message = u"keys.edn invalid (defaults active): "_q + Keymap().error;
		state.messageIsError = true;
	}

	state.localleaderHint = u"SPC leader  ·  "_q + FirstLocalleaderLabel() + u" local"_q;
	_statusBar->setState(std::move(state));
}

KeyUse Keys::classify(
		const QKeyEvent &event,
		int key,
		const Target &target) const {
	const auto mods = event.modifiers() & ~Qt::KeypadModifier;
	const auto stroke = KeyStroke(event, key);
	const auto pending = !_prefix.empty() || _count > 0 || _inspect || _hints.active() || _swallowNext;

	if (_swallowNext) {
		return KeyUse::SwallowEmptyLocal;
	}
	if (((key == Qt::Key_Escape && !mods) || stroke == kCtrlG)
		&& (_normal || pending || target.focus)) {
		if (pending) {
			return KeyUse::Cancel;
		}
		// In Insert mode on an eligible surface, Escape returns to Normal mode!
		return (!_normal && target.focus) ? KeyUse::ExitInsertOrBackOut : KeyUse::Pass;
	}
	if (_hints.active()) {
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
	}
	if (!_prefix.empty() && key == Qt::Key_Backspace && !mods) {
		return KeyUse::PrefixBackspace;
	}
	// Numeric repeat count digits in Normal mode (1-9 start count, 0 continues it).
	if (_normal && !_inspect && _prefix.empty() && !mods
			&& ((key >= Qt::Key_1 && key <= Qt::Key_9)
				|| (_count > 0 && key == Qt::Key_0))) {
		return KeyUse::CountDigit;
	}
	if (HasShortcutModifier(stroke)) {
		// Allow our explicit modal modifier chords (C-[, M-x, C-d, C-u, C-w, C-j, C-k, C-f, C-b)
		// before falling through to Telegram stock shortcuts.
		const auto effective = !_frozenEffective.empty()
			? _frozenEffective
			: EffectiveBindings(_normal, target.contextMap);
		const auto matchesModal = ranges::any_of(effective, [&](const EffectiveBinding &b) {
			return b.keys.size() > _prefix.size()
				&& b.keys[_prefix.size()] == stroke
				&& StartsWith(b.keys, _prefix);
		});
		if (matchesModal) {
			return KeyUse::Capture;
		}
		const auto &shortcuts = Shortcuts::KeysCurrents();
		if (shortcuts.contains(QKeySequence(event.keyCombination()))
			|| shortcuts.contains(QKeySequence(QKeyCombination::fromCombined(stroke)))) {
			return KeyUse::Pass;
		}
	}
	if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
		return KeyUse::Pass;
	}

	const auto effective = !_frozenEffective.empty()
		? _frozenEffective
		: EffectiveBindings(_normal, target.contextMap);
	const auto matchesEffective = ranges::any_of(effective, [&](const EffectiveBinding &b) {
		return b.keys.size() > _prefix.size()
			&& b.keys[_prefix.size()] == stroke
			&& StartsWith(b.keys, _prefix);
	});
	const auto matchesLocalleader = _normal && ranges::any_of(Keymap().localleaders, [&](const Chord &ll) {
		return ll.size() > _prefix.size()
			&& ll[_prefix.size()] == stroke
			&& StartsWith(ll, _prefix);
	});
	const auto printable = key >= Qt::Key_Space && key < Qt::Key_Escape;
	const auto plainOrShift = (mods == Qt::NoModifier || mods == Qt::ShiftModifier);
	if (matchesEffective || matchesLocalleader
			|| ((_inspect || (_normal && pending)) && plainOrShift && printable)) {
		return KeyUse::Capture;
	}
	if (!_normal) {
		return KeyUse::Pass;
	}
	const auto normalTextMods = (mods == Qt::NoModifier
		|| mods == Qt::ShiftModifier
		|| mods == Qt::AltModifier
		|| mods == (Qt::AltModifier | Qt::ShiftModifier));
	return (normalTextMods
			&& (!event.text().isEmpty()
				|| (key >= Qt::Key_Dead_Grave && key <= Qt::Key_Dead_Greek)
				|| key == Qt::Key_Return
				|| key == Qt::Key_Enter
				|| key == Qt::Key_Backspace
				|| key == Qt::Key_Delete))
		? KeyUse::Ignore
		: KeyUse::Pass;
}

bool Keys::handle(QObject *receiver, QEvent *event) {
	if (event->type() == QEvent::WindowDeactivate) {
		if (receiver == _window) {
			_held.clear();
			clearPrefix();
		}
		return false;
	} else if (event->type() == QEvent::InputMethod) {
		clearPrefix();
		return false;
	}
	const auto keyEvent = static_cast<QKeyEvent*>(event);
	const auto key = ModalKey(*keyEvent);
	if (event->type() == QEvent::KeyRelease) {
		return keyEvent->isAutoRepeat() ? _held.contains(key) : _held.remove(key);
	}
	if (keyEvent->isAutoRepeat() && _held.contains(key)) {
		// Allow repeat for single-key repeatable motions in Normal mode? Keep non-repeat guard safe.
		event->accept();
		return true;
	}
	const auto target = currentTarget(receiver);
	if (!target.focus || !Enabled()) {
		if (receiver == QApplication::focusWidget()) {
			clearPrefix();
		}
		return false;
	}
	const auto use = classify(*keyEvent, key, target);
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
	case KeyUse::ExitInsertOrBackOut:
		enterNormal(true);
		break;
	case KeyUse::Cancel:
		clearPrefix();
		showFeedback(u"Canceled"_q);
		break;
	case KeyUse::SwallowEmptyLocal:
		_swallowNext = false;
		clearPrefix();
		break;
	case KeyUse::CountDigit:
		_count = std::min(kMaxCount, _count * 10 + (key - Qt::Key_0));
		refreshStatusBar();
		break;
	case KeyUse::PrefixBackspace:
		if (!_prefix.empty()) {
			_prefix.pop_back();
		}
		if (_prefix.empty()) {
			clearPrefix();
		} else {
			refreshStatusBar();
			showWhichKey();
		}
		break;
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
			enterInsert(true);
		} else if (result == MessageHints::Input::Unknown) {
			showFeedback(tr::lng_purple_keys_undefined(tr::now));
		} else if (result == MessageHints::Input::Finished) {
			clearPrefix();
		}
		refreshStatusBar();
	} break;
	case KeyUse::Pass:
		break;
	}
	return true;
}

void Keys::showWhichKey() {
	const auto target = currentTarget(QApplication::focusWidget());
	if (!target.focus || !Enabled() || (_prefix.empty() && !_inspect) || !_whichKey) {
		hideWhichKey();
		return;
	}
	const auto &effective = !_frozenEffective.empty()
		? _frozenEffective
		: EffectiveBindings(_normal, target.contextMap);

	auto groups = std::vector<ModalUi::WhichKeyEntry>();
	auto leaves = std::vector<ModalUi::WhichKeyEntry>();
	auto leafIndexByAction = std::map<Action, size_t>();
	auto seen = QSet<int>();

	const auto keySortKey = [](const QString &k) {
		// Group: 0 = single letters, 1 = digits, 2 = punctuation, 3 = multi-char (SPC, RET, etc.)
		if (k.size() == 1) {
			const auto ch = k.front();
			if (ch.isLetter()) {
				return std::tuple(0, k.toLower(), k.front().isUpper() ? 1 : 0);
			}
			if (ch.isDigit()) {
				return std::tuple(1, k, 0);
			}
			return std::tuple(2, k, 0);
		}
		return std::tuple(3, k.toLower(), 0);
	};

	for (const auto &b : effective) {
		if (!StartsWith(b.keys, _prefix)
			|| b.keys.size() == _prefix.size()
			|| (!_inspect && !Available(b.action, target))) {
			continue;
		}
		const auto next = b.keys[_prefix.size()];
		if (seen.contains(next)) {
			continue;
		}
		seen.insert(next);
		const auto keyLbl = KeyLabel(next);
		if (b.keys.size() == _prefix.size() + 1) {
			if (const auto it = leafIndexByAction.find(b.action);
				it != leafIndexByAction.end()) {
				auto &existing = leaves[it->second].keys;
				if (!existing.contains(keyLbl)) {
					existing.push_back(keyLbl);
				}
			} else {
				leafIndexByAction.emplace(b.action, leaves.size());
				leaves.push_back({
					.keys = { keyLbl },
					.label = FindCommand(b.action).label(),
					.isGroup = false,
				});
			}
		} else {
			auto extended = _prefix;
			extended.push_back(next);
			auto lbl = PrefixLabel(MapKind::Normal, extended);
			if (lbl.isEmpty()) {
				lbl = IsExactLocalleader(extended)
					? (target.contextName.isEmpty() ? u"local"_q : target.contextName)
					: u"prefix"_q;
			}
			groups.push_back({
				.keys = { keyLbl },
				.label = lbl,
				.isGroup = true,
			});
		}
	}

	// Keep alias keys inside merged leaf entries in consistent canonical order (e.g. "SPC" / ":", "," / "RET").
	for (auto &leaf : leaves) {
		std::sort(leaf.keys.begin(), leaf.keys.end(), [&](const QString &a, const QString &b) {
			if (a == u"SPC"_q) return true;
			if (b == u"SPC"_q) return false;
			if (a == u","_q) return true;
			if (b == u","_q) return false;
			return keySortKey(a) < keySortKey(b);
		});
	}

	const auto sortEntry = [&](const ModalUi::WhichKeyEntry &a, const ModalUi::WhichKeyEntry &b) {
		const auto ka = a.keys.isEmpty() ? QString() : a.keys.front();
		const auto kb = b.keys.isEmpty() ? QString() : b.keys.front();
		return keySortKey(ka) < keySortKey(kb);
	};
	std::sort(groups.begin(), groups.end(), sortEntry);
	std::sort(leaves.begin(), leaves.end(), sortEntry);

	if (groups.empty() && leaves.empty()) {
		hideWhichKey();
		return;
	}

	const auto isLocal = IsLocalleaderPrefix(_prefix);
	auto wk = ModalUi::WhichKeyState{
		.prefixCaps = ChordCaps(_prefix),
		.title = isLocal
			? u"localleader"_q
			: PrefixLabel(MapKind::Normal, _prefix),
		.contextBadge = isLocal ? target.contextName : QString(),
		.isLocal = isLocal,
		.groups = std::move(groups),
		.leaves = std::move(leaves),
	};
	_whichKey->setState(std::move(wk));
	const auto above = QRect(
		_lastBody.x(),
		_lastBody.y(),
		_lastBody.width(),
		std::max(0, _lastBody.height() - statusBarHeight()));
	_whichKey->updateBounds(above);
	_window->fixOrder();
}

void Keys::stroke(int key, const Target &target) {
	if (_prefix.empty()) {
		// Freeze effective bindings at stroke 1 so mid-chord focus/state shifts cannot change meaning.
		_frozenEffective = EffectiveBindings(_normal, target.contextMap);
		if (!_feedbackError && !_feedbackText.isEmpty()) {
			_feedbackText.clear();
			_feedbackTimer.stop();
		}
	}
	_prefix.push_back(key);
	hideWhichKey();

	// If the user hit an exact localleader chord where no context bindings exist,
	// warn cleanly and swallow the next keystroke so no stray Normal command fires.
	if (_normal && !_inspect && IsExactLocalleader(_prefix)) {
		const auto hasLocal = ranges::any_of(_frozenEffective, [&](const EffectiveBinding &b) {
			return b.viaLocalleader && StartsWith(b.keys, _prefix)
				&& Available(b.action, target);
		});
		if (!hasLocal) {
			const auto label = ChordLabel(_prefix);
			clearPrefix();
			_swallowNext = true;
			showFeedback(u"No local keys here ("_q + label + u")"_q);
			return;
		}
	}

	auto more = false;
	for (const auto &b : _frozenEffective) {
		if (!StartsWith(b.keys, _prefix)) {
			continue;
		} else if (b.keys.size() != _prefix.size()) {
			more = true;
			continue;
		}
		const auto inspect = _inspect;
		const auto captured = _prefix;
		const auto count = std::max(1, _count);
		clearPrefix();
		if (inspect) {
			showDescribeKeyResult(captured, target);
		} else if (Available(b.action, target)) {
			dispatch(b.action, target, count);
		} else {
			const auto &cmd = FindCommand(b.action);
			showFeedback((cmd.need && *cmd.need)
				? QString::fromLatin1(cmd.need)
				: tr::lng_purple_keys_action_unavailable(tr::now));
		}
		return;
	}
	if (!more && _normal) {
		more = ranges::any_of(Keymap().localleaders, [&](const Chord &ll) {
			return ll.size() > _prefix.size() && StartsWith(ll, _prefix);
		});
	}
	if (more) {
		refreshStatusBar();
		_whichKeyTimer.start(kWhichKeyDelay);
	} else {
		const auto unknown = ChordLabel(_prefix);
		const auto inspect = _inspect;
		const auto captured = _prefix;
		clearPrefix();
		if (inspect) {
			showDescribeKeyResult(captured, target);
		} else {
			showFeedback(unknown + u" is undefined"_q);
		}
	}
}

void Keys::dispatch(Action action, const Target &target, int count) {
	const auto current = owner();
	if (!current || !target.focus || !Available(action, target)) {
		return;
	}
	const auto repeat = FindCommand(action).repeatable ? std::clamp(count, 1, kMaxCount) : 1;
	using Native = Shortcuts::Command;
	const auto launch = [this, window = QPointer(_window.get())](Native native) {
		if (!Shortcuts::Launch(std::vector<Native>{ native }) && window) {
			showFeedback(tr::lng_purple_keys_action_unavailable(tr::now));
		}
	};
	using Hint = MessageHints::Action;
	const auto startHint = [&](Hint hint, QString empty) {
		if (_hints.start(target.messages, hint)) {
			_normal = true;
			refreshStatusBar();
		} else {
			showFeedback(std::move(empty));
		}
	};
	const auto stepChatList = [&](bool down) {
		const auto list = target.list ? target.list : ChatsInnerList(current);
		if (!list) {
			return;
		}
		for (auto i = 0; i < repeat; ++i) {
			auto ev = QKeyEvent(QEvent::KeyPress,
				down ? Qt::Key_Down : Qt::Key_Up,
				Qt::NoModifier);
			list->processKeyDispatch(&ev);
		}
	};
	switch (action) {
	case Action::EnterNormalMode:
		enterNormal(false);
		break;
	case Action::EnterInsertMode:
		enterInsert(false);
		break;
	case Action::Backgrounds:
		ToggleChatBackgroundVisibility(_window);
		break;
	case Action::ToggleNightMode:
		if (Window::Theme::Background()->editingTheme()) {
			current->show(Ui::MakeInformBox(
				tr::lng_theme_editor_cant_change_theme()));
			break;
		}
		Window::Theme::ToggleNightModeWithConfirmation(
			&_window->controller(),
			[] {
				Window::Theme::ToggleNightMode();
				Window::Theme::KeepApplied();
			});
		break;
	case Action::MoveUp:
	case Action::MoveDown:
		if (target.list) {
			stepChatList(action == Action::MoveDown);
		} else if (const auto scroll = HistoryScroll(target)) {
			const auto delta = kLineScrollPx * repeat;
			const auto top = scroll->scrollTop();
			scroll->scrollToY((action == Action::MoveUp)
				? std::max(0, top - delta)
				: (top + delta));
		}
		break;
	case Action::PreviousChat:
	case Action::NextChat:
		stepChatList(action == Action::NextChat);
		break;
	case Action::OpenChat:
		target.list->chooseRow();
		break;
	case Action::PreviousFolder:
	case Action::NextFolder:
		for (auto i = 0; i < repeat; ++i) {
			Window::CheckAndJumpToNearChatsFilter(
				current, action == Action::NextFolder, true);
		}
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
		clearPrefix();
		ShowChatPalette(current);
		break;
	case Action::OpenMusic:
		ToggleMusicSection(current);
		break;
	case Action::OpenPlaylists:
		clearPrefix();
		ShowPlaylistsBox(current);
		break;
	case Action::OpenChatInfo:
		clearPrefix();
		current->showPeerInfo(target.chat.thread());
		break;
	case Action::AttachFiles:
		clearPrefix();
		target.attach();
		break;
	case Action::OpenChatMedia:
	case Action::OpenChatMusic:
	case Action::OpenChatFiles:
	case Action::OpenChatLinks:
	case Action::OpenChatVoice:
	case Action::OpenChatGifs:
	case Action::OpenChatPolls: {
		clearPrefix();
		const auto media = Info::Section(FindCommand(action).mediaType);
		if (const auto topic = target.chat.topic()) {
			current->showSection(std::make_shared<Info::Memento>(topic, media));
		} else {
			current->showSection(std::make_shared<Info::Memento>(target.chat.peer(), media));
		}
	} break;
	case Action::OpenPinnedMessages:
		clearPrefix();
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
		clearPrefix();
		::Calls::ShowCallsBox(current);
		break;
	case Action::OpenDownloads:
		clearPrefix();
		current->showSection(Info::Downloads::Make(current->session().user()));
		break;
	case Action::NewGroup:
		clearPrefix();
		current->showNewGroup();
		break;
	case Action::NewChannel:
		clearPrefix();
		current->showNewChannel();
		break;
	case Action::AddContact:
		clearPrefix();
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
		for (auto i = 0; i < repeat; ++i) launch(Native::MediaSeekBackward);
		break;
	case Action::SeekForward:
		for (auto i = 0; i < repeat; ++i) launch(Native::MediaSeekForward);
		break;
	case Action::Account1: launch(Native::ShowAccount1); break;
	case Action::Account2: launch(Native::ShowAccount2); break;
	case Action::Account3: launch(Native::ShowAccount3); break;
	case Action::Account4: launch(Native::ShowAccount4); break;
	case Action::Account5: launch(Native::ShowAccount5); break;
	case Action::Account6: launch(Native::ShowAccount6); break;
	case Action::FocusChats:
		if (const auto dialogs = ChatsPane(current)) {
			dialogs->setInnerFocus(true);
		}
		break;
	case Action::FocusConversation:
		current->content()->activate();
		break;
	case Action::FocusOtherPane:
		if (Available(Action::FocusChats, target)) {
			if (const auto dialogs = ChatsPane(current)) {
				dialogs->setInnerFocus(true);
			}
		} else if (Available(Action::FocusConversation, target)) {
			current->content()->activate();
		}
		break;
	case Action::HistoryHalfUp:
	case Action::HistoryHalfDown:
		if (const auto scroll = HistoryScroll(target)) {
			const auto delta = std::max(80, scroll->height() / 2) * repeat;
			const auto top = scroll->scrollTop();
			scroll->scrollToY((action == Action::HistoryHalfUp)
				? std::max(0, top - delta)
				: (top + delta));
		}
		break;
	case Action::OlderMessagesPage:
	case Action::NewerMessagesPage:
		if (const auto scroll = HistoryScroll(target)) {
			const auto delta = scroll->height() * repeat;
			const auto top = scroll->scrollTop();
			scroll->scrollToY((action == Action::OlderMessagesPage)
				? std::max(0, top - delta)
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
		clearPrefix();
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
	case Action::Cheatsheet:
		showHelpLayer(false, target);
		break;
	case Action::Help:
		showHelpLayer(true, target);
		break;
	case Action::Run:
	case Action::DescribeCommand:
		showPalette(action == Action::DescribeCommand, target, !_normal);
		break;
	case Action::DescribeChord:
		_inspect = true;
		refreshStatusBar();
		break;
	case Action::StockShortcuts:
		clearPrefix();
		_activeLayer = ActiveLayer::StockShortcuts;
		ShowKeysLegend(&_window->controller());
		refreshStatusBar();
		break;
	case Action::ReloadKeymap:
		ReloadKeymapStore();
		showFeedback(Keymap().error.isEmpty()
			? u"Reloaded keys.edn"_q
			: (u"keys.edn error: "_q + Keymap().error),
			!Keymap().error.isEmpty());
		break;
	}
}

void Keys::showHelpLayer(bool startOnAllBindings, const Target &target) {
	if (!_feedbackError) {
		_feedbackText.clear();
		_feedbackTimer.stop();
	}
	const auto effective = EffectiveBindings(true, target.contextMap);
	const auto insertEff = EffectiveBindings(false, std::nullopt);
	const auto primaryLl = Keymap().localleaders.empty()
		? Chord{ Qt::Key_Comma }
		: Keymap().localleaders.front();
	const auto mapContextTag = [](MapKind map) -> QString {
		switch (map) {
		case MapKind::Chat: return u"chat"_q;
		case MapKind::Chats: return u"chats"_q;
		case MapKind::Music: return u"music"_q;
		default: return {};
		}
	};
	const auto rowFor = [&](
			Action action,
			QString overrideLabel = {},
			int maxChords = 2,
			bool skipSecondaryLocalleader = false) -> std::optional<ModalUi::HelpRow> {
		struct TaggedChord {
			QStringList caps;
			QString contextTag;
		};
		auto candidates = std::vector<TaggedChord>();
		const auto secondaryCaps = (skipSecondaryLocalleader && Keymap().localleaders.size() > 1)
			? ChordCaps(Keymap().localleaders[1])
			: QStringList();
		for (const auto &b : effective) {
			if (b.action != action) {
				continue;
			}
			auto caps = ChordCaps(b.keys);
			if (!secondaryCaps.isEmpty()
				&& caps.size() > secondaryCaps.size()
				&& std::equal(secondaryCaps.begin(), secondaryCaps.end(), caps.begin())) {
				continue;
			}
			const auto dup = ranges::any_of(candidates, [&](const TaggedChord &c) {
				return c.caps == caps;
			});
			if (!dup) {
				candidates.push_back({
					.caps = std::move(caps),
					.contextTag = b.viaLocalleader ? mapContextTag(b.sourceMap) : QString(),
				});
			}
		}
		if (candidates.empty()) {
			return std::nullopt;
		}
		// Keep cheatsheet cards cleanly aligned: prefer shortest/primary chords and cap total keycaps.
		std::stable_sort(candidates.begin(), candidates.end(), [](const TaggedChord &a, const TaggedChord &b) {
			return a.caps.size() < b.caps.size();
		});
		auto kept = std::vector<QStringList>();
		auto keptTag = QString();
		auto allLocalSameTag = true;
		auto totalCaps = 0;
		for (auto &c : candidates) {
			if (int(kept.size()) >= maxChords) {
				break;
			}
			if (!kept.empty() && totalCaps + c.caps.size() > 3) {
				break;
			}
			if (kept.empty()) {
				keptTag = c.contextTag;
			} else if (c.contextTag != keptTag) {
				allLocalSameTag = false;
			}
			totalCaps += int(c.caps.size());
			kept.push_back(std::move(c.caps));
		}
		const auto &cmd = FindCommand(action);
		return ModalUi::HelpRow{
			.chords = std::move(kept),
			.label = overrideLabel.isEmpty() ? cmd.label() : std::move(overrideLabel),
			.commandId = QString::fromLatin1(cmd.id),
			.contextTag = allLocalSameTag ? std::move(keptTag) : QString(),
		};
	};
	const auto pushRow = [](std::vector<ModalUi::HelpRow> &rows, std::optional<ModalUi::HelpRow> r) {
		if (r) rows.push_back(std::move(*r));
	};

	// 1. Curated task-oriented cheatsheet cards (live-resolved from active EDN keymap).
	auto cards = std::vector<ModalUi::HelpSection>();
	{
		auto nav = ModalUi::HelpSection{ .title = u"NAVIGATE"_q };
		pushRow(nav.rows, rowFor(Action::MoveDown, u"Down (chat list or scroll)"_q));
		pushRow(nav.rows, rowFor(Action::MoveUp, u"Up (chat list or scroll)"_q));
		pushRow(nav.rows, rowFor(Action::NextChat, u"Next chat from any pane"_q));
		pushRow(nav.rows, rowFor(Action::OpenChat, u"Open highlighted chat"_q));
		pushRow(nav.rows, rowFor(Action::HistoryHalfDown, u"Half-page scroll down"_q));
		pushRow(nav.rows, rowFor(Action::HistoryHalfUp, u"Half-page scroll up"_q));
		pushRow(nav.rows, rowFor(Action::LatestMessages, u"Jump to newest"_q));
		pushRow(nav.rows, rowFor(Action::UnreadMessages, u"Jump to unread"_q, 1));
		nav.rows.push_back({ .chords = { { u"5"_q, u"j"_q } }, .label = u"Count repeats motions"_q });
		cards.push_back(std::move(nav));
	}
	{
		auto leader = ModalUi::HelpSection{ .title = u"LEADER (SPC)"_q };
		pushRow(leader.rows, rowFor(Action::Run, u"M-x command palette"_q));
		pushRow(leader.rows, rowFor(Action::ChatPalette, u"Switch chat (palette)"_q, 1));
		// Dynamically derive all SPC prefix groups from the live keymap (including SPC s and SPC t).
		auto seenPrefixKeys = QSet<int>();
		auto livePrefixes = std::vector<int>();
		const std::array<int, 9> preferredOrder = {
			Qt::Key_B, Qt::Key_O, Qt::Key_N, Qt::Key_P,
			Qt::Key_S, Qt::Key_T, Qt::Key_W, Qt::Key_A, Qt::Key_H,
		};
		for (const auto &b : effective) {
			if (!b.viaLocalleader && b.keys.size() >= 3 && b.keys[0] == Qt::Key_Space
					&& Available(b.action, target)) {
				const auto k = b.keys[1];
				if (!seenPrefixKeys.contains(k)) {
					seenPrefixKeys.insert(k);
					livePrefixes.push_back(k);
				}
			}
		}
		std::sort(livePrefixes.begin(), livePrefixes.end(), [&](int a, int b) {
			const auto ia = ranges::find(preferredOrder, a) - begin(preferredOrder);
			const auto ib = ranges::find(preferredOrder, b) - begin(preferredOrder);
			if (ia != ib) {
				return ia < ib;
			}
			return QString::compare(KeyLabel(a), KeyLabel(b), Qt::CaseInsensitive) < 0;
		});
		for (const auto k : livePrefixes) {
			const auto pfx = Chord{ Qt::Key_Space, k };
			auto lbl = PrefixLabel(MapKind::Normal, pfx);
			if (lbl.isEmpty()) {
				lbl = u"prefix"_q;
			}
			leader.rows.push_back({
				.chords = { { u"SPC"_q, KeyLabel(k) } },
				.label = u"+"_q + lbl,
			});
		}
		cards.push_back(std::move(leader));
	}
	{
		auto localTitle = u"LOCALLEADER ("_q + FirstLocalleaderLabel();
		if (Keymap().localleaders.size() > 1) {
			localTitle += u" / "_q + ChordLabel(Keymap().localleaders[1]);
		}
		localTitle += u")"_q;
		auto local = ModalUi::HelpSection{ .title = std::move(localTitle) };
		const auto addLocalRow = [&](MapKind map, Action action, QString shortLabel = {}) {
			for (const auto &b : Keymap().bindings) {
				if (b.map == map && b.action == action) {
					auto full = primaryLl;
					full.insert(full.end(), b.keys.begin(), b.keys.end());
					const auto ctx = (map == MapKind::Chat) ? u"chat"_q
						: (map == MapKind::Chats) ? u"chats"_q
						: u"music"_q;
					local.rows.push_back({
						.chords = { ChordCaps(full) },
						.label = shortLabel.isEmpty() ? FindCommand(action).label() : std::move(shortLabel),
						.commandId = QString::fromLatin1(FindCommand(action).id),
						.contextTag = ctx,
					});
					return;
				}
			}
		};
		addLocalRow(MapKind::Chat, Action::OpenChatInfo, u"Chat info"_q);
		addLocalRow(MapKind::Chat, Action::OpenPinnedMessages, u"Pinned messages"_q);
		addLocalRow(MapKind::Chat, Action::OpenScheduled, u"Scheduled messages"_q);
		addLocalRow(MapKind::Chat, Action::OpenHistoryCalendar, u"History calendar"_q);
		addLocalRow(MapKind::Chat, Action::AttachFiles, u"Add attachment"_q);
		addLocalRow(MapKind::Chat, Action::OpenChatMedia, u"Shared media"_q);
		addLocalRow(MapKind::Chats, Action::PreviewChat, u"Chat preview"_q);
		addLocalRow(MapKind::Chats, Action::NextFolder, u"Next folder"_q);
		addLocalRow(MapKind::Music, Action::TogglePlayback, u"Play / pause"_q);
		cards.push_back(std::move(local));
	}
	{
		auto hints = ModalUi::HelpSection{ .title = u"COPY & MESSAGE HINTS"_q };
		pushRow(hints.rows, rowFor(Action::CopyMessage, u"Copy message text"_q, 1));
		pushRow(hints.rows, rowFor(Action::CopyMessageLink, u"Copy message link"_q, 1));
		pushRow(hints.rows, rowFor(Action::CopyMessageUrl, u"Copy URL in message"_q, 1));
		pushRow(hints.rows, rowFor(Action::OpenMessageUrl, u"Open URL in message"_q, 2));
		pushRow(hints.rows, rowFor(Action::ReplyToMessage, u"Reply to message"_q, 1, true));
		pushRow(hints.rows, rowFor(Action::EditMessage, u"Edit message"_q, 1, true));
		pushRow(hints.rows, rowFor(Action::SelectMessage, u"Select message"_q, 1));
		pushRow(hints.rows, rowFor(Action::MessageMenu, u"Message menu"_q, 1));
		cards.push_back(std::move(hints));
	}
	{
		auto panes = ModalUi::HelpSection{ .title = u"PANES, FOLDERS & TOGGLES"_q };
		pushRow(panes.rows, rowFor(Action::FocusChats, u"Focus chat list"_q, 1));
		pushRow(panes.rows, rowFor(Action::FocusConversation, u"Focus conversation"_q, 1));
		pushRow(panes.rows, rowFor(Action::FocusOtherPane, u"Toggle pane"_q, 1));
		pushRow(panes.rows, rowFor(Action::NextFolder, u"Next folder"_q, 1, true));
		pushRow(panes.rows, rowFor(Action::PreviousFolder, u"Previous folder"_q, 1, true));
		pushRow(panes.rows, rowFor(Action::Search, u"Search messages"_q, 1));
		pushRow(panes.rows, rowFor(Action::ToggleNightMode, u"Toggle night mode"_q, 1));
		pushRow(panes.rows, rowFor(Action::Backgrounds, u"Toggle chat wallpaper"_q, 1));
		cards.push_back(std::move(panes));
	}
	{
		auto modes = ModalUi::HelpSection{ .title = u"MODES & HELP"_q };
		pushRow(modes.rows, rowFor(Action::EnterInsertMode, u"Enter Insert mode (type)"_q, 2));
		auto exitChords = std::vector<QStringList>{ { u"ESC"_q } };
		for (const auto &c : ChordsForAction(Action::EnterNormalMode, insertEff)) {
			if (exitChords.size() < 2) {
				exitChords.push_back(c);
			}
		}
		modes.rows.push_back({ .chords = std::move(exitChords), .label = u"Return to Normal / cancel"_q });
		modes.rows.push_back({ .chords = { { u"DEL"_q } }, .label = u"Back up one key in prefix"_q });
		pushRow(modes.rows, rowFor(Action::DescribeChord, u"Describe a key sequence"_q, 1));
		pushRow(modes.rows, rowFor(Action::DescribeCommand, u"Describe any command"_q, 1));
		pushRow(modes.rows, rowFor(Action::Help, u"All active bindings"_q, 1));
		pushRow(modes.rows, rowFor(Action::StockShortcuts, u"Stock Telegram shortcuts"_q, 1));
		pushRow(modes.rows, rowFor(Action::ReloadKeymap, u"Reload keys.edn"_q, 1));
		cards.push_back(std::move(modes));
	}

	// 2. Exhaustive active bindings grouped from most specific to most general.
	auto groups = std::vector<ModalUi::HelpSection>();
	const auto addGrouped = [&](
			const QString &title,
			const auto &pred,
			const std::vector<EffectiveBinding> &src,
			bool requireAvailable = true) {
		auto sec = ModalUi::HelpSection{ .title = title };
		auto byAction = std::map<Action, std::vector<QStringList>>();
		auto order = std::vector<Action>();
		for (const auto &b : src) {
			if (!pred(b) || (requireAvailable && !Available(b.action, target))) {
				continue;
			}
			if (!byAction.contains(b.action)) {
				order.push_back(b.action);
			}
			auto caps = ChordCaps(b.keys);
			if (!ranges::contains(byAction[b.action], caps)) {
				byAction[b.action].push_back(std::move(caps));
			}
		}
		for (const auto action : order) {
			const auto &cmd = FindCommand(action);
			sec.rows.push_back({
				.chords = std::move(byAction[action]),
				.label = cmd.label(),
				.commandId = QString::fromLatin1(cmd.id),
			});
		}
		if (!sec.rows.empty()) {
			groups.push_back(std::move(sec));
		}
	};

	if (target.contextMap) {
		addGrouped(
			u"LOCALLEADER ("_q + target.contextName + u")"_q,
			[](const EffectiveBinding &b) { return b.viaLocalleader; },
			effective);
	}
	const std::pair<int, QString> leaderGroups[] = {
		{ Qt::Key_B, u"LEADER: SPC b (buffer / chat)"_q },
		{ Qt::Key_O, u"LEADER: SPC o (open)"_q },
		{ Qt::Key_N, u"LEADER: SPC n (new)"_q },
		{ Qt::Key_P, u"LEADER: SPC p (playback)"_q },
		{ Qt::Key_S, u"LEADER: SPC s (search)"_q },
		{ Qt::Key_T, u"LEADER: SPC t (toggle)"_q },
		{ Qt::Key_W, u"LEADER: SPC w (window)"_q },
		{ Qt::Key_A, u"LEADER: SPC a (accounts)"_q },
		{ Qt::Key_H, u"LEADER: SPC h (help)"_q },
	};
	for (const auto &[k, title] : leaderGroups) {
		addGrouped(
			title,
			[k](const EffectiveBinding &b) {
				return !b.viaLocalleader && b.keys.size() >= 2
					&& b.keys[0] == Qt::Key_Space && b.keys[1] == k;
			},
			effective);
	}
	addGrouped(
		u"LEADER: SPC top-level"_q,
		[](const EffectiveBinding &b) {
			return !b.viaLocalleader && b.keys.size() == 2 && b.keys[0] == Qt::Key_Space;
		},
		effective);
	addGrouped(
		u"VIM PREFIXES (g, y, [, ], C-w)"_q,
		[](const EffectiveBinding &b) {
			return !b.viaLocalleader && b.keys.size() >= 2 && b.keys[0] != Qt::Key_Space;
		},
		effective);
	addGrouped(
		u"SINGLE KEYS (Normal)"_q,
		[](const EffectiveBinding &b) {
			return !b.viaLocalleader && b.keys.size() == 1;
		},
		effective);
	addGrouped(
		u"INSERT MODE"_q,
		[](const EffectiveBinding &) { return true; },
		insertEff,
		false);

	const auto guard = QPointer(this);
	_activeLayer = startOnAllBindings
		? ActiveLayer::AllBindings
		: ActiveLayer::Cheatsheet;
	auto layer = ModalUi::CreateModalHelpLayer({
		.startOnAllBindings = startOnAllBindings,
		.keymapPath = Keymap().userPath,
		.keymapError = Keymap().error,
		.cheatsheetCards = std::move(cards),
		.activeBindingGroups = std::move(groups),
		.onTabChanged = [guard](bool showingAll) {
			if (!guard) return;
			guard->_activeLayer = showingAll
				? ActiveLayer::AllBindings
				: ActiveLayer::Cheatsheet;
			guard->refreshStatusBar();
		},
	});
	layer->lifetime().add([guard] {
		if (!guard) return;
		if (guard->_activeLayer == ActiveLayer::Cheatsheet
			|| guard->_activeLayer == ActiveLayer::AllBindings) {
			guard->_activeLayer = ActiveLayer::None;
			guard->refreshStatusBar();
		}
	});
	_window->showOrHideBoxOrLayer(
		std::move(layer),
		Ui::LayerOption::KeepOther,
		anim::type::normal);
	refreshStatusBar();
}

namespace {

[[nodiscard]] std::vector<ModalUi::DescribeBinding> CollectCommandBindings(Action action) {
	auto out = std::vector<ModalUi::DescribeBinding>();
	for (const auto &b : Keymap().bindings) {
		if (b.action != action) {
			continue;
		}
		if (b.map == MapKind::Chat || b.map == MapKind::Chats || b.map == MapKind::Music) {
			for (const auto &ll : Keymap().localleaders) {
				auto full = ll;
				full.insert(full.end(), b.keys.begin(), b.keys.end());
				out.push_back({
					.caps = ChordCaps(full),
					.where = MapName(b.map),
				});
			}
		} else {
			out.push_back({
				.caps = ChordCaps(b.keys),
				.where = MapName(b.map),
			});
		}
	}
	return out;
}

} // namespace

void Keys::showDescribeKeyResult(const Chord &chord, const Target &target) {
	if (!_feedbackError) {
		_feedbackText.clear();
		_feedbackTimer.stop();
	}
	const auto effective = EffectiveBindings(_normal, target.contextMap);
	auto data = ModalUi::DescribeData{
		.isKeyMode = true,
		.queriedChord = ChordCaps(chord),
	};
	const auto it = ranges::find_if(effective, [&](const EffectiveBinding &b) {
		return b.keys == chord;
	});
	const auto guard = QPointer(this);
	const auto origin = target.focus;
	const auto original = base::make_weak(target.controller);
	if (it != end(effective)) {
		const auto &cmd = FindCommand(it->action);
		data.found = true;
		data.title = cmd.label();
		data.commandId = QString::fromLatin1(cmd.id);
		data.availableHere = Available(it->action, target);
		data.keylessOnPurpose = Keymap().keyless.contains(it->action);
		data.unavailableReason = QString::fromLatin1(cmd.need);
		data.bindings = CollectCommandBindings(it->action);
		const auto targetAction = it->action;
		data.onDescribeCommand = [guard, targetAction, origin, original] {
			if (!guard) {
				return;
			}
			const auto live = guard->_window->controller().sessionController();
			const auto liveTarget = (live && live == original.get() && origin)
				? FindTarget(live, origin)
				: Target();
			guard->showDescribeCommandResult(targetAction, liveTarget);
		};
	}
	// List anywhere else this chord is bound to a different command across other maps.
	for (const auto &b : Keymap().bindings) {
		if (b.keys == chord) {
			if (data.found && it != end(effective) && b.action == it->action) {
				continue;
			}
			data.elsewhere.push_back({
				.caps = ChordCaps(b.keys),
				.where = FindCommand(b.action).label() + u" ("_q + MapName(b.map) + u")"_q,
			});
		}
	}
	_activeLayer = ActiveLayer::DescribeKey;
	_describeKeyHasDetails = data.found && bool(data.onDescribeCommand);
	auto layer = ModalUi::CreateDescribeLayer(std::move(data));
	layer->lifetime().add([guard] {
		if (!guard) return;
		if (guard->_activeLayer == ActiveLayer::DescribeKey) {
			guard->_activeLayer = ActiveLayer::None;
			guard->_describeKeyHasDetails = false;
			guard->refreshStatusBar();
		}
	});
	_window->showOrHideBoxOrLayer(
		std::move(layer),
		Ui::LayerOption::KeepOther,
		anim::type::normal);
	refreshStatusBar();
}

void Keys::showDescribeCommandResult(Action action, const Target &target) {
	if (!_feedbackError) {
		_feedbackText.clear();
		_feedbackTimer.stop();
	}
	const auto &cmd = FindCommand(action);
	auto data = ModalUi::DescribeData{
		.isKeyMode = false,
		.title = cmd.label(),
		.commandId = QString::fromLatin1(cmd.id),
		.found = true,
		.availableHere = Available(action, target),
		.keylessOnPurpose = Keymap().keyless.contains(action),
		.unavailableReason = QString::fromLatin1(cmd.need),
		.bindings = CollectCommandBindings(action),
	};
	const auto guard = QPointer(this);
	_activeLayer = ActiveLayer::DescribeCommand;
	_describeKeyHasDetails = false;
	auto layer = ModalUi::CreateDescribeLayer(std::move(data));
	layer->lifetime().add([guard] {
		if (!guard) return;
		if (guard->_activeLayer == ActiveLayer::DescribeCommand) {
			guard->_activeLayer = ActiveLayer::None;
			guard->refreshStatusBar();
		}
	});
	_window->showOrHideBoxOrLayer(
		std::move(layer),
		Ui::LayerOption::KeepOther,
		anim::type::normal);
	refreshStatusBar();
}

void Keys::showPalette(bool describe, const Target &target, bool fromInsert) {
	if (!_feedbackError) {
		_feedbackText.clear();
		_feedbackTimer.stop();
	}
	const auto current = owner();
	if (!current) {
		return;
	}
	const auto effective = EffectiveBindings(true, target.contextMap);
	auto items = std::vector<ModalUi::PaletteItem>();
	for (const auto &cmd : kCommands) {
		if (!describe && !IsSearchableAction(cmd.action)) {
			continue;
		}
		const auto avail = Available(cmd.action, target);
		items.push_back({
			.actionId = int(cmd.action),
			.id = QString::fromLatin1(cmd.id),
			.label = cmd.label(),
			.category = QString::fromLatin1(cmd.category),
			.unavailableReason = QString::fromLatin1(cmd.need),
			.chords = ChordsForAction(cmd.action, effective),
			.available = describe ? true : avail,
		});
	}

	const auto revision = _revision;
	const auto origin = target.focus;
	const auto original = base::make_weak(current);
	const auto guard = QPointer(this);
	const auto kind = describe
		? ActiveLayer::DescribeCommandPicker
		: ActiveLayer::Palette;
	_activeLayer = kind;
	refreshStatusBar();

	auto layer = ModalUi::CreateCommandPaletteLayer({
		.promptBadge = describe ? u"SPC h f"_q : u"M-x"_q,
		.placeholder = describe
			? u"Describe any command (orderless, e.g. \"pin\")..."_q
			: u"Execute command (orderless, e.g. \"opm\", M-p/n history)..."_q,
		.describeMode = describe,
		.items = std::move(items),
		.history = MxHistory(),
		.onPick = [guard, describe, revision, origin, original](int actionId, QString query) {
			if (!guard) {
				return;
			}
			guard->_activeLayer = ActiveLayer::None;
			const auto action = Action(actionId);
			MxHistory().record(QString::fromLatin1(FindCommand(action).id), query);
			SaveMxHistory();
			QTimer::singleShot(0, guard.data(), [guard, describe, action, revision, origin, original] {
				if (!guard) {
					return;
				}
				const auto live = guard->owner();
				if (!Enabled() || !live || live != original.get()
					|| revision != guard->_revision || !origin
					|| guard->_window->controller().isLayerShown()) {
					guard->refreshStatusBar();
					return;
				}
				if (describe) {
					guard->showDescribeCommandResult(action, FindTarget(live, origin));
					return;
				}
				const auto restoredTarget = FindTarget(live, origin);
				if (restoredTarget.focus && Available(action, restoredTarget)) {
					origin->setFocus(Qt::OtherFocusReason);
					const auto restored = guard->currentTarget(origin);
					if (restored.focus && revision == guard->_revision) {
						guard->dispatch(action, restored, 1);
					}
				}
				guard->refreshStatusBar();
			});
		},
		.onCancel = [guard, revision, origin, original, fromInsert] {
			if (!guard) {
				return;
			}
			guard->_activeLayer = ActiveLayer::None;
			QTimer::singleShot(0, guard.data(), [guard, revision, origin, original, fromInsert] {
				if (!guard) {
					return;
				}
				const auto live = guard->owner();
				if (Enabled() && live && live == original.get()
					&& revision == guard->_revision && origin && fromInsert
					&& !guard->_window->controller().isLayerShown()) {
					origin->setFocus(Qt::OtherFocusReason);
					guard->_normal = false;
				}
				guard->refreshStatusBar();
			});
		},
	});
	layer->lifetime().add([guard, kind] {
		if (!guard) return;
		if (guard->_activeLayer == kind) {
			guard->_activeLayer = ActiveLayer::None;
			guard->refreshStatusBar();
		}
	});
	_window->showOrHideBoxOrLayer(
		std::move(layer),
		Ui::LayerOption::KeepOther,
		anim::type::normal);
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
	return EnsureKeys(window)->handle(receiver, event);
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

void EnsureModalKeysAttached(not_null<MainWindow*> window) {
	EnsureKeys(window);
}

int ModalKeysStatusHeight(not_null<MainWindow*> window) {
	const auto keys = FindKeys(window.get());
	return keys ? keys->statusBarHeight() : 0;
}

void UpdateModalKeysLayout(not_null<MainWindow*> window, QRect body) {
	if (const auto keys = FindKeys(window.get())) {
		keys->updateLayout(body);
	}
}

void RaiseModalKeysChrome(not_null<MainWindow*> window) {
	if (const auto keys = FindKeys(window.get())) {
		keys->raiseChrome();
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
