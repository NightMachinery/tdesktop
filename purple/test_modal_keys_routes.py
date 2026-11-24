#!/usr/bin/env python3
"""Check the production hooks, UI architecture, EDN keymap, and guards around modal keys."""
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "Telegram/SourceFiles"


def read(path):
    return (SOURCE / path).read_text(encoding="utf-8")


def between(text, first, last):
    return text[text.index(first):text.index(last, text.index(first) + len(first))]


def ordered(text, *parts):
    position = 0
    for part in parts:
        position = text.index(part, position) + len(part)


app = read("core/application.cpp")
press = between(app, "case QEvent::KeyPress:", "case QEvent::MouseButtonPress:")
ordered(app, "bool Application::eventFilter", "Purple::FilterCustomizeSettingsInput", "HandlePossibleChatSwitch")
ordered(press, "IsToggleFullScreenKey", "HandlePossibleChatSwitch",
        "Purple::HandleModalKeys", "HandleKeysLegendHotkey")
mac_native = between(read("platform/mac/main_window_mac.mm"),
                     "bool MainWindow::nativeEvent", "void MainWindow::hideAndDeactivate")
ordered(mac_native, "PossiblyTextTypingEvent(event)",
        "!Purple::ModalKeysOwnChatListFocus(this)", "imeCompositionStartReceived()")
win_native = read("platform/win/main_window_win.cpp")
assert "ModalKeysOwnChatListFocus" not in win_native
assert press.count("Purple::HandleModalKeys") == 1

# MainWindow persistent modal status bar & docked which-key hooks.
mainwindow = read("mainwindow.cpp")
assert '#include "purple/purple_keys.h"' in mainwindow
ordered(mainwindow, "void MainWindow::finishFirstShow()", "Purple::EnsureModalKeysAttached(this);")
ordered(mainwindow, "void MainWindow::fixOrder()", "Purple::RaiseModalKeysChrome(this);")
ordered(mainwindow, "void MainWindow::updateControlsGeometry()",
        "Purple::ModalKeysStatusHeight(this)",
        "body.height() - modalStatusH",
        "Purple::UpdateModalKeysLayout(this, body);")

keys = read("purple/purple_keys.cpp")
keys_ui = read("purple/purple_keys_ui.cpp")
keys_mx = read("purple/purple_keys_mx.h")
resource = ROOT / "Telegram/Resources/purple"
defaults = (resource / "default-keys.edn").read_text(encoding="utf-8")
assert "<file>default-keys.edn</file>" in (resource / "keymaps.qrc").read_text()
purple_cmake = (ROOT / "Telegram/cmake/purple.cmake").read_text()
assert "purple/keymaps.qrc" in purple_cmake
for src in ("purple/purple_keys.cpp", "purple/purple_keys_ui.cpp", "purple/purple_keys_mx.h"):
    assert src in purple_cmake, src

commands = between(keys, "const Command kCommands[]", "using Chord")
command_ids = dict(re.findall(r'\{ Action::(\w+), "([a-z0-9-]+)"', commands))


def bound(action, chord, text=defaults):
    assert re.search(rf'"{re.escape(chord)}"\s+:{re.escape(command_ids[action])}(?:\s|\}}|$)', text), (action, chord)


# Customize occupies a four-stroke leaf and does not displace Calls.
bound("CustomizeSettings", "SPC f e d")
bound("OpenCalls", "SPC o p")
assert len(re.findall(r'"SPC f e d"\s+:', defaults)) == 1
assert re.search(r'"SPC f e"\s+"edit"', defaults)
assert command_ids["CustomizeSettings"] == "customize-settings"
ordered(keys, "case Action::CustomizeSettings:", "ShowCustomizeSettings")
ordered(keys, ".onPick = [guard", "Available(action, restoredTarget)", "guard->dispatch(action, restored, 1)")
customize = read("purple/purple_settings_customize.cpp")
ordered(customize, "sessionDataAboutToBeCleared()", "retire()", "PrivacyScreenChanges()")
ordered(customize, "void protectionChanged()", "setCurrentWidget(_covered)",
        "refreshFromDisk()", "_resumeRefused = _draftStale", "_coverClose->show()",
        "setCurrentWidget(_browser)")
ordered(customize, "QEvent::InputMethod", "preeditString()", "QEvent::KeyPress", "handleKey(")
ordered(customize, "bool handleKey", "_preedit.contains(object)", "return false", "ShouldHandleEditorKey")
ordered(customize, "new QPlainTextEdit", "setPlainText", "stageForm()", "PrepareWindowSettingsEditorEdit")
assert "RequestSettingsEditorApply" not in customize
# Verify new native UI surfaces reuse stock Ui::KeysLegend & Ui::LayerWidget, not Toasts/ASCII TextBox.
assert "Ui::Toast::Show" not in keys, "Modal UI must not fall back to plain Toast popups"
for required_ui in (
    "Ui::KeysLegend::MakeCap",
    "Ui::KeysLegend::PaintCap",
    "class ModalStatusBar",
    "class WhichKeyOverlay",
    "class ModalHelpLayer",
    "class DescribeLayer",
    "class CommandPaletteLayer",
    "ModalMx::RankAndFilter",
    "ModalMx::LabelMatchSpans",
    "ModalMx::StepInputHistory",
):
    assert required_ui in (keys_ui + read("purple/purple_keys_ui.h")), required_ui

# Verify IME safety in palette filter input.
ordered(keys_ui, "preeditAreaText().isEmpty()", "Qt::Key_Escape", "Qt::Key_Down", "Qt::Key_Up",
        "Qt::AltModifier", "stepHistory(")

# Verify amx + orderless helpers exist in purple_keys_mx.h (including title-first tiering and subtitle highlights).
for sym in ("kRecentLimit = 7", "WordInitials", "ParseTokens", "MatchToken",
            "TextMatchSpans", "LabelMatchSpans", "SubtitleMatchSpans",
            "CandidateMatchTier", "RankAndFilter", "StepInputHistory"):
    assert sym in keys_mx, sym
assert "SubtitleMatchSpans" in keys_ui

# Verify Normal-mode non-leakage guard: printable, Option/Alt text, Return/Enter, Backspace/Delete return Capture or Ignore, never Pass.
classify = between(keys, "KeyUse Keys::classify", "bool Keys::handle")
ordered(classify, "if (!_normal)", "return KeyUse::Pass;",
        "Qt::AltModifier", "!event.text().isEmpty()",
        "Qt::Key_Return", "Qt::Key_Enter", "KeyUse::Ignore", "KeyUse::Pass")

# Verify which-key merges duplicate actions and keeps groups/leaves separated cleanly.
wk = between(keys, "void Keys::showWhichKey", "void Keys::stroke")
ordered(wk, "leafIndexByAction", ".groups = std::move(groups)", ".leaves = std::move(leaves)")

# Verify active-layer and prefix/which-key status bar cues replace Normal-mode cues.
status_fn = between(keys, "void Keys::refreshStatusBar", "KeyUse Keys::classify")
for layer_case in ("ActiveLayer::Cheatsheet", "ActiveLayer::AllBindings",
                   "ActiveLayer::Palette", "ActiveLayer::DescribeKey",
                   "ActiveLayer::StockShortcuts", "state.rightCue",
                   'u"DEL back  ·  ESC cancel"_q'):
    assert layer_case in status_fn, layer_case

# Verify cheatsheet preserves key casing, derives live-available SPC prefixes, and tags localleader rows.
assert "card.title.toUpper()" not in keys_ui
assert "group.title.toUpper()" not in keys_ui
assert "QApplication::isRightToLeft" not in keys_ui
assert "return style::RightToLeft();" in keys_ui
assert "MeasureTextWidth(" in keys_ui
assert "ElideText(" in keys_ui
assert "_colCapWidths" in keys_ui
help_fn = between(keys, "void Keys::showHelpLayer", "void Keys::showDescribeKeyResult")
ordered(help_fn, "b.viaLocalleader ? mapContextTag(b.sourceMap)",
        "Qt::Key_S, Qt::Key_T", "Available(b.action, target)",
        ".contextTag = ctx", "requireAvailable && !Available(b.action, target)", "onTabChanged")

# Security & focus gates.
gate = between(keys, "Window::SessionController *Keys::owner()", "bool Keys::chromeVisible")
for protected in ("passcodeLocked()", "window.locked()", "activePopupWidget()",
                  "activeModalWidget()"):
    assert protected in gate, protected
target_gate = between(keys, "Target Keys::currentTarget", "Target Keys::activeTarget")
for protected in ("isLayerShown()", "receiver == focus"):
    assert protected in target_gate, protected

history = between(read("history/history_widget.cpp"),
                  "bool HistoryWidget::allowsPurpleKeys", "void HistoryWidget::setInnerFocus")
for protected in ("isSearching()", "isRecording()", "_purpleScreenTimeCover->visible()",
                  "preeditAreaText().isEmpty()", "_autocomplete->isHidden()"):
    assert protected in history, protected

# Key routing, physical keys (non-ASCII + macOS Option M-), counts, DEL step-back, localleader.
assert "modifiers.testFlag(Qt::AltModifier)" in between(keys, "int ModalKey(", "enum class Action")
routing = between(keys, "bool Keys::handle", "void Keys::showWhichKey")
ordered(routing, "ModalKey(*keyEvent)", "QEvent::KeyRelease",
        "isAutoRepeat() && _held.contains(key)", "!target.focus || !Enabled()",
        "QEvent::ShortcutOverride", "event->accept()", "return true;",
        "_held.insert(key)", "keyEvent->isAutoRepeat()",
        "case KeyUse::CountDigit:", "case KeyUse::PrefixBackspace:",
        "stroke(KeyStroke(*keyEvent, key), target)")
ordered(routing, "const auto result = _hints.type", "if (!guard)",
        "result == MessageHints::Input::LeaveNormal", "enterInsert(true)")

stroke = between(keys, "void Keys::stroke", "void Keys::dispatch")
ordered(stroke, "_frozenEffective = EffectiveBindings", "IsExactLocalleader(_prefix)",
        "_swallowNext = true", "const auto inspect = _inspect", "clearPrefix()",
        "if (inspect)", "showDescribeKeyResult", "else if (Available", "dispatch(")

palette = between(keys, "void Keys::showPalette", "} // namespace")
ordered(palette, "revision = _revision", "base::make_weak(current)",
        "guard = QPointer(this)", "closePalettePosted",
        "stack->topShownLayer() == raw", "anim::type::instant",
        "ModalUi::CreateCommandPaletteLayer",
        "if (!guard)", "MxHistory().record", "QTimer::singleShot(0, guard.data()",
        "closePalettePosted();",
        "!Enabled()", "live != original.get()", "revision != guard->_revision",
        "FindTarget(live, origin)", "guard->currentTarget(origin)",
        "guard->dispatch(action, restored", "onCancel = [guard,")

describe_key = between(keys, "void Keys::showDescribeKeyResult", "void Keys::showDescribeCommandResult")
ordered(describe_key, "guard = QPointer(this)", "origin = target.focus",
        "original = base::make_weak(target.controller)",
        "onDescribeCommand = [guard, targetAction, origin, original]",
        "FindTarget(live, origin)")

keys_ctor = between(keys, "Keys::Keys(not_null<MainWindow*> window)", "Keys::~Keys()")
ordered(keys_ctor, "QApplication::focusChanged", "inThisWindow", "clearPrefix()")

# Status bar reflects the window's focused widget even while inactive, while dispatch stays active-only.
win_focus = between(keys, "QWidget *Keys::windowFocusWidget", "Target Keys::activeTarget")
ordered(win_focus, "QApplication::focusWidget()", "_window->focusWidget()", "isVisibleTo(_window)")
active_tgt = between(keys, "Target Keys::activeTarget", "bool Keys::ownsChatListFocus")
ordered(active_tgt, "windowFocusWidget()")
status_refresh = between(keys, "void Keys::refreshStatusBar", "KeyUse Keys::classify")
ordered(status_refresh, "activeTarget()", "windowFocusWidget()", "WA_InputMethodEnabled")
current_tgt = between(keys, "Target Keys::currentTarget", "QWidget *Keys::windowFocusWidget")
ordered(current_tgt, "QApplication::focusWidget()", "owner()")
owner_fn = between(keys, "Window::SessionController *Keys::owner", "bool Keys::chromeVisible")
ordered(owner_fn, "QApplication::activeWindow() == _window")

dispatch = between(keys, "void Keys::dispatch", "void Keys::showHelpLayer")
assert "chooseRow" not in between(dispatch, "case Action::PreviousChat:", "case Action::OpenChat:")
ordered(dispatch, "case Action::ToggleNightMode:", "editingTheme()",
        "ToggleNightModeWithConfirmation", "Window::Theme::ToggleNightMode();",
        "Window::Theme::KeepApplied();")
ordered(dispatch, "case Action::OpenChat:", "chooseRow()")

# Verify Doom-aligned SPC leader + localleader (, / SPC m) bindings and zero 4-stroke chords.
assert ':localleader ["," "SPC m"]' in defaults
for action, chord in (
    ("Cheatsheet", "?"),
    ("Help", "SPC ?"),
    ("Help", "SPC h b"),
    ("DescribeChord", "SPC h k"),
    ("DescribeCommand", "SPC h f"),
    ("StockShortcuts", "SPC h s"),
    ("ReloadKeymap", "SPC h r"),
    ("Run", "M-x"),
    ("Run", ":"),
    ("Run", "SPC SPC"),
    ("ChatPalette", "SPC ,"),
    ("ChatPalette", "SPC RET"),
    ("ChatPalette", "SPC b b"),
    ("Backgrounds", "SPC t b"),
    ("ToggleNightMode", "SPC t n"),
    ("OpenChatInfo", "SPC b i"),
    ("OpenChatMenu", "SPC b m"),
    ("OpenPinnedMessages", "SPC b p"),
    ("OpenScheduled", "SPC b s"),
    ("OpenHistoryCalendar", "SPC b d"),
    ("AttachFiles", "SPC b u"),
    ("OpenSettings", "SPC o s"),
    ("OpenSavedMessages", "SPC o S"),
    ("OpenArchive", "SPC o a"),
    ("OpenContacts", "SPC o c"),
    ("OpenCalls", "SPC o p"),
    ("OpenDownloads", "SPC o d"),
    ("OpenMainMenu", "SPC o m"),
    ("NewGroup", "SPC n g"),
    ("NewChannel", "SPC n c"),
    ("AddContact", "SPC n p"),
    ("TogglePlayback", "SPC p p"),
    ("StopPlayback", "SPC p s"),
    ("NextTrack", "SPC p n"),
    ("PreviousTrack", "SPC p N"),
    ("SeekForward", "SPC p f"),
    ("SeekBackward", "SPC p b"),
    ("OpenMusic", "SPC p m"),
    ("OpenPlaylists", "SPC p l"),
    ("FocusChats", "SPC w h"),
    ("FocusConversation", "SPC w l"),
    ("FocusOtherPane", "SPC w w"),
    ("SelectMessage", "g s"),
    ("MessageMenu", "g m"),
    ("ReplyToMessage", "g r"),
    ("EditMessage", "g e"),
    ("OpenMessageUrl", "f"),
    ("CopyMessage", "y y"),
    ("CopyMessageLink", "y l"),
    ("CopyMessageUrl", "y u"),
    ("UnreadBuffer", "SPC u"),
    ("UnreadBuffer", "SPC b I"),
    ("ToggleZenMode", "SPC t z"),
    ("ToggleZenMode", "SPC w m"),
    ("TogglePrivacyScreen", "SPC t p"),
    ("TogglePrivacyScreen", "SPC z"),
    ("TopicPalette", "SPC b t"),
    ("TopicUnreadPalette", "SPC b T"),
    ("TopicNext", "SPC b n"),
    ("TopicPrevious", "SPC b N"),
    ("TopicNext", "] T"),
    ("TopicPrevious", "[ T"),
    ("TopicNextUnread", "] U"),
    ("TopicLast", "SPC b l"),
    ("TopicCreate", "SPC b c"),
    ("TopicParent", "SPC b h"),
    ("TopicParent", "g ^"),
    ("NoticeHistory", "SPC h e"),
):
    bound(action, chord)
for index in range(1, 7):
    assert re.search(rf"case Action::Account{index}:.*launch\(Native::ShowAccount{index}\);", dispatch)
    bound(f"Account{index}", f"SPC a {index}")

# Verify context maps exist and contain shared media and local commands.
all_commands = set(command_ids.values())
map_sections = re.findall(r':(normal|chat|chats|music|insert)\s*\{([^}]*)\}', defaults, re.S)
assert {m for m, _ in map_sections} == {"normal", "chat", "chats", "music", "insert"}
for map_name, body in map_sections:
    bindings = re.findall(r'"([^"]+)"\s+:([a-z0-9-]+)', body)
    chords = [tuple(c.split()) for c, _ in bindings]
    assert set(cmd for _, cmd in bindings) <= all_commands, map_name
    assert len(set(chords)) == len(chords), f"duplicate chord in :{map_name}"
    assert all(len(c) <= 4 for c in chords), f"chord > 4 strokes in :{map_name}"
    assert not any(a != b and b[:len(a)] == a for a in chords for b in chords), f"prefix collision in :{map_name}"
    if map_name == "insert":
        assert ('C-[',) in chords and ('M-x',) in chords

# Shared media & chat actions under :chat localleader.
chat_body = dict(map_sections)["chat"]
for action, key in (
    ("OpenChatInfo", "i"), ("OpenChatMenu", "m"), ("OpenPinnedMessages", "p"),
    ("OpenScheduled", "s"), ("OpenHistoryCalendar", "d"), ("AttachFiles", "a"),
    ("OpenChatMedia", "v"), ("OpenChatMusic", "u"), ("OpenChatFiles", "f"),
    ("OpenChatLinks", "l"), ("OpenChatVoice", "o"), ("OpenChatGifs", "g"),
    ("OpenChatPolls", "q"),
    ("TopicPalette", "t"), ("TopicUnreadPalette", "T"),
    ("TopicNext", "n"), ("TopicPrevious", "N"),
    ("TopicLast", "b"), ("TopicCreate", "c"), ("TopicParent", "h"),
):
    bound(action, key, chat_body)

# Phase 2 routes: unread status counts, avatars, iBuffer read-receipt confirm,
# real fullscreen Zen mode with transient status bar, app-wide privacy screen, and topics.
ordered(keys, "ComputeUnreadSummary", "HasUnmutedUnreadNotification", "BucketForPeer")
ordered(keys, "void Keys::setZenMode", "setPurpleZenHidden(true)", "showFullScreen()",
        "setPurpleZenHidden(false)", "PlanWindowRestore", "showNormal()")
ordered(keys, "bool Keys::zenNeedsTransientBar", "int Keys::statusBarHeight", "!_zenMode")
ordered(keys, "void Keys::showUnreadBuffer", "CreateUnreadBufferLayer", "MarkAsReadMenu::MarkAsReadThread")
ordered(keys, "void Keys::showTopicPicker", "CreateTopicPickerLayer", "onRefresh",
        "onLoadMore", "requestTopics()", "onReload", "reloadTopics()", "pageUpdates",
        "reserveNewBotTopic", "NewForumTopicBox")
ordered(keys, "avatarSession", "avatarPeerId")

keys_ui = read("purple/purple_keys_ui.cpp")
ordered(keys_ui, "sends read receipts", "ModalUnread::NeedsMarkReadConfirmation", "executeMarkRead")
ordered(keys_ui, "paintUserpic")
ordered(keys_ui, "class TopicPickerLayer final", "QTimer::timeout", "_config.pageUpdates",
        "_rootsBeforeLoad.contains", "QTimer::singleShot(0, this, [=] { maybeRequestMore(); })",
        "Qt::Key_R && !mods", "finishLoading()", "_config.onReload()")
ordered(keys_ui, "class NoticeLayer final", "class NoticeHistoryLayer final",
        "CreateNoticeLayer", "CreateNoticeHistoryLayer")
ordered(keys, "void Keys::schedulePendingNoticeFlush", "_noticeLayers.empty()",
        "&QObject::destroyed", "_noticePresentation == presentation", "MaybeFlushPendingNotices")
ordered(keys, "PeekCountdownChanges()", "schedulePendingNoticeFlush()", "PeekCountdownPending()")
wait_cpp = read("purple/purple_peek_controls.cpp")
ordered(wait_cpp, "void PendingWait::report", ".session = session", ".background = background",
        "_session = weak", "void PendingWait::confirm", "report(result, origin, session.get(), true, onResult)")
notices_cpp = read("purple/purple_notices.cpp")
ordered(notices_cpp, "sessionDataAboutToBeCleared", "clearAccount",
        "WindowMatchesAccount", "CanExposeNoticeInWindow",
        "ResolveNoticePeerLabel", "peerFromUser(bare)", "peerFromChat(bare)",
        "peerFromChannel(bare)", "PickerVisible", "ResolvePeerIdsInConfigValue",
        "void SubmitNotice", "Actions().erase(id)", "ShowPurpleNotice",
        "ShowPurpleAck", "MaybeFlushPendingNotices", "OpenPurpleNoticeHistory",
        "clearHistoryAndPending")
ordered(notices_cpp, "void OpenNoticeLayerForRecord", "Store().findById(accountKey, id)",
        "revision = rec.repeatCount", "live->repeatCount == revision", "run(&weakWin->controller())")
ordered(notices_cpp, "class PendingNoticeDelivery", "passcodeLockChanges()",
        "PrivacyScreenChanges()", "ModalKeysEnabledChanges()", "sessionControllerValue()",
        "QEvent::Hide", "QEvent::WindowActivate", "forgetExpiredAcks", "takeNextPending",
        "ShowDeferredToast", "&QObject::destroyed")
ordered(notices_cpp, "QWidget *ShowDeferredToast", "ToastCapture", "pending.fallbackToast()",
        "capture.visibleToast()", "CanShowDeferredToast", "Ui::Toast::Show")
config_cpp = read("purple/purple_config.cpp")
ordered(config_cpp, "void Config::reloadFromDisk", "takeEditorGlobalReload", "edited && !editorEcho")
ordered(config_cpp, "SettingsEditorOpening CaptureWindowSettingsEditor", "activeWindow() == host",
        "host->maybeSession() == live", "!host->locked()", "!PrivacyScreenActiveAny()", "!live->account().loggingOut()")
ordered(config_cpp, "void RequestSettingsEditorApply", "_opening.windowCurrent", "review->commit",
        "RequestWait({", "WaitAction::ChangePeekOption", "apply(current, true)", ".onResult = finish")
ordered(config_cpp, "PurpleNoticeWindow(session)", "hasConfigPeerIds", "ShowPurpleNotice",
        "config:reload:")
archive_cpp = read("purple/purple_archive_service.cpp")
ordered(archive_cpp, "void ShowArchiveRemovalPending()", ".appScoped = true",
        "kArchiveRemovalNoticeKey", "ShowArchiveRemovalPending();", "archive:gap:",
        "void RemoveArchiveAtLogout", "ShowArchiveRemovalPending();", "archive:discard_failed:")
st_cover_cpp = read("purple/purple_screentime_cover.cpp")
ordered(st_cover_cpp, "ShowPurpleAck", ".persistent = true")
last_seen_cpp = read("purple/purple_last_seen.cpp")
ordered(last_seen_cpp, "ShowPurpleNotice", "lastseen:empty:", ".background = true")

privacy = read("purple/purple_privacy_screen.cpp")
ordered(privacy, "ShouldFetchWeather", "Accept-Language")
ordered(privacy, "Lock with passcode", "hasLocalPasscode()", "lockByPasscode()", "DismissPrivacyScreen()")
ordered(privacy, "QEvent::WindowActivate", "QEvent::FocusIn", "QEvent::InputMethod", "QEvent::Drop")
ordered(privacy, "class PrivacyAppFilter final", "passcodeLockValue()", "QEvent::Show",
        "ClassifyTopLevelSurface", "SuppressTopLevelWidget")
ordered(privacy, "closeMediaViewAndPanelsForPrivacy()", "clearAllFast()",
        "HideSecondaryTopLevelSurfaces()", "enumerateWindows",
        "GlobalPrivacyChanges.fire(true)", "RefreshMenusAndTitles()")
ordered(privacy, "SurfaceState.onDismiss(", "RestoreSecondaryTopLevelSurfaces()",
        "GlobalPrivacyChanges.fire(false)", "checkStartUrls()")
mw_cpp = read("mainwindow.cpp")
fix_order = between(mw_cpp, "void MainWindow::fixOrder()", "void MainWindow::raiseMediaPreview()")
ordered(fix_order, "_setupEmailLock", "_passcodeLock", "_layer", "raiseMediaPreview()",
        "Purple::PrivacyScreenActive(this)", "Purple::RaisePrivacyScreen(this)")
core_app = read("core/application.cpp")
ordered(core_app, "int Application::unreadBadge()", "Purple::PrivacyScreenActiveAny()")
ordered(core_app, "Purple::PrivacyScreenChanges()")
ordered(core_app, "SyncPrivacyScreenForWindow")
notif = read("window/notifications_manager.cpp")
ordered(notif, "getNotificationOptions", "Purple::PrivacyScreenActiveAny()")
ordered(notif, "openNotificationMessage", "Purple::PrivacyScreenActiveAny()")
media_overlay = read("media/view/media_view_overlay_widget.cpp")
ordered(media_overlay, "Purple::PrivacyScreenChanges(", "_pip = nullptr;")
media_ctrl = read("media/system_media_controls_manager.cpp")
ordered(media_ctrl, "Purple::PrivacyScreenActiveAny()", "clearMetadata()", "Purple::PrivacyScreenChanges(")
tray_acc = read("tray_accounts_menu.cpp")
ordered(tray_acc, "Purple::PrivacyScreenActiveAny()")
mac_menu = read("platform/mac/global_menu_mac.mm")
ordered(mac_menu, "Purple::PrivacyScreenActiveAny()")
main_w = read("mainwidget.cpp")
ordered(main_w, "syncZenHide", "_zenThirdSectionHide", "_zenThirdShadowHide")
ordered(main_w, "void MainWidget::destroyThirdSection()", "_zenThirdSectionHide.onWidgetDestroyed()")

# Existing attachment, playback, account, calendar, background, hints, menu, and music checks.
attachment = between(keys, "class KeyboardAttachment final", "constexpr auto kEnabledPref")
ordered(attachment, "if (!CanAttach(widget)) return {};", "QPointer(widget)",
        "HistoryView::ScheduledWidget", "widget->chooseAttach();",
        "widget->chooseAttach(std::nullopt);")

stock_launch = between(read("core/shortcuts.cpp"),
                       "bool Launch(std::vector<Command>", "rpl::producer<not_null<Request*>>")
ordered(stock_launch, "if (Paused)", "return false;", "RequestHandler", "handler()")

available = between(keys, "bool Available", "bool IsSearchableAction")
ordered(available, "case Action::OpenCalls:", "case Action::NewGroup:",
        "case Action::NewChannel:",
        "return target.controller && !target.controller->session().supportMode();")
assert "case Action::AttachFiles: return bool(target.attach);" in available
ordered(available, "case Action::TogglePlayback:", "case Action::StopPlayback:",
        "HasPlaybackInSession(&target.controller->session())")

calendar = read("ui/boxes/calendar_box.cpp")
ordered(calendar, "CalendarBox::keyPressEvent", "Purple::CalendarDayKeysEnabled()",
        "Purple::CalendarDayKeyOffset(*e)")

painter = read("window/section_widget.cpp")
ordered(painter, "Purple::PaintHiddenChatBackground", "theme->background().giftId")
background = read("purple/purple_chat_background.cpp")
ordered(background, "readPref<bool>(kVisiblePref, true)", "VisibilityChanges.fire")

hints = read("purple/purple_message_hints.cpp")
ordered(hints, "setFocusPolicy(Qt::NoFocus)", "Qt::WA_TransparentForMouseEvents")
ordered(hints, "0xff, 0xd8, 0x4a", "0xb9, 0x1c, 0x1c")
ordered(hints, "bool MessageHints::eligible", "hasCopyRestriction", "goodForSelection")

menu = read("window/window_main_menu.cpp")
ordered(menu, "MainMenu::MainMenu(", "setupMenu();",
        "Purple::InstallMainMenuKeys(", "_toggleAccounts.data(), _accounts)")

assert "MusicAllowsModalKeys(owner, focus)" in keys
assert "MusicHidesChats(_mainSection.data())" in read("mainwidget.cpp")
assert "Purple::IsChatPaletteController(_controller)" in read("boxes/peer_list_box.cpp")

print("Modal key, status line, which-key, cheatsheet, M-x, localleader, hints and background routes passed")
