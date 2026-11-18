#!/usr/bin/env python3
"""Check the production hooks and the guards around modal key dispatch."""
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
ordered(press, "IsToggleFullScreenKey", "HandlePossibleChatSwitch",
        "Purple::HandleModalKeys", "HandleKeysLegendHotkey")
assert press.count("Purple::HandleModalKeys") == 1

keys = read("purple/purple_keys.cpp")
gate = between(keys, "Window::SessionController *Keys::owner()", "KeyUse Keys::classify")
for protected in ("passcodeLocked()", "window.locked()", "activePopupWidget()",
                  "activeModalWidget()", "isLayerShown()", "receiver == focus"):
    assert protected in gate, protected
history = between(read("history/history_widget.cpp"),
                  "bool HistoryWidget::allowsPurpleKeys", "void HistoryWidget::setInnerFocus")
for protected in ("isSearching()", "isRecording()", "_purpleScreenTimeCover->visible()",
                  "preeditAreaText().isEmpty()", "_autocomplete->isHidden()"):
    assert protected in history, protected

routing = between(keys, "bool Keys::handle", "void Keys::clearPrefix")
ordered(routing, "QEvent::KeyRelease", "isAutoRepeat() && _held.contains(key)",
        "!target.focus || !Enabled()", "QEvent::ShortcutOverride",
        "event->accept()", "return true;", "_held.insert(key)",
        "keyEvent->isAutoRepeat()", "stroke(key, target)")
ordered(keys, "!Enabled()", "event->type() != QEvent::ShortcutOverride",
        "return (new Keys(window))")

stroke = between(keys, "void Keys::stroke", "void Keys::dispatch")
ordered(stroke, "const auto inspect = _inspect", "clearPrefix()", "if (inspect)",
        "CommandDetail", "else if (Available", "dispatch(")
palette = between(keys, "void Keys::showPalette", "} // namespace")
ordered(palette, "revision = _revision", "base::make_weak(current)", "stack->topShownLayer() != layer",
        "hideLayer(",
        "!Enabled()", "current != original.get()", "revision != _revision",
        "FindTarget(current, origin)", "currentTarget(origin)", "dispatch(action, restored)")
ordered(keys, "if (describe)", "label->setText(CommandDetail", "} else {", "execute(action")

dispatch = between(keys, "void Keys::dispatch", "void Keys::showPalette")
assert "chooseRow" not in between(dispatch, "case Action::PreviousChat:", "case Action::OpenChat:")
ordered(dispatch, "Qt::Key_Down : Qt::Key_Up", "processKeyDispatch",
        "case Action::OpenChat:", "chooseRow()")
assert re.search(r'\{ Action::Backgrounds, "chat-backgrounds-visibility-toggle"', keys)
assert "{ Action::Backgrounds, { Qt::Key_Space, Qt::Key_C, Qt::Key_B } }" in keys
stock_launch = between(read("core/shortcuts.cpp"),
                       "bool Launch(std::vector<Command>", "rpl::producer<not_null<Request*>>")
ordered(stock_launch, "if (Paused)", "return false;", "RequestHandler", "handler()")
ordered(dispatch, "!current || !target.focus || !Available",
        "Shortcuts::Launch(std::vector<Native>{ native })",
        "lng_purple_keys_action_unavailable", "switch (action)")
for action, native in (
        ("Search", "Search"), ("OpenArchive", "ShowArchive"),
        ("OpenContacts", "ShowContacts"), ("OpenScheduled", "ShowScheduled"),
        ("OpenChatMenu", "ShowChatMenu"), ("PreviewChat", "ShowChatPreview"),
        ("TogglePlayback", "MediaPlayPause"),
        ("StopPlayback", "MediaStop"),
        ("PreviousTrack", "MediaPrevious"),
        ("NextTrack", "MediaNext"),
        ("SeekBackward", "MediaSeekBackward"),
        ("SeekForward", "MediaSeekForward")):
    assert re.search(
        rf"case Action::{action}:\s+launch\(Native::{native}\);", dispatch)
playback_owner = between(keys, "bool HasPlaybackInSession", "bool Available")
ordered(playback_owner, "if (!session)", "player->current(player->getActiveType()).audio()",
        "document && (&document->session() == session)")
available = between(keys, "bool Available", "bool IsSearchableAction")
ordered(available, "case Action::TogglePlayback:", "case Action::StopPlayback:",
        "case Action::PreviousTrack:", "case Action::NextTrack:",
        "case Action::SeekBackward:", "case Action::SeekForward:",
        "HasPlaybackInSession(&target.controller->session())")
assert keys.count(".controller = owner") == 3
accounts = between(keys, "bool CanSwitchAccount", "Dialogs::Widget *ChatsPane")
ordered(accounts, "if (!controller)", "domain().orderedAccounts()", "index < int(accounts.size())",
        "accounts[index]->sessionExists()", "!= &controller->session().account()")
for index in range(1, 7):
    assert re.search(rf"case Action::Account{index}:\s+launch\(Native::ShowAccount{index}\);", dispatch)
    assert f"{{ Action::Account{index}, {{ Qt::Key_Space, Qt::Key_A, Qt::Key_{index} }} }}" in keys
panes = between(keys, "Dialogs::Widget *ChatsPane", "bool Available")
ordered(panes, "controller->content()->children()", "dynamic_cast<Dialogs::Widget*>",
        "dialogs->controller() == controller", "dialogs->isEnabled()", "dialogs->purpleKeysList(dialogs)")
ordered(available, "case Action::FocusChats:", "ChatsPane(target.controller)",
        "case Action::FocusConversation:", "!target.controller->adaptive().isOneColumn()",
        "target.controller->mainSectionShown()", "target.controller->activeChatCurrent().thread()")
ordered(dispatch, "case Action::FocusChats:", "ChatsPane(current)", "dialogs->setInnerFocus(true)",
        "case Action::FocusConversation:", "current->content()->activate()")

history_scroll = between(keys, "Ui::ScrollArea *HistoryScroll", "bool Available")
ordered(history_scroll, "target.messages.widget()", "list->parentWidget()",
        "dynamic_cast<Ui::ScrollArea*>(viewport->parentWidget())",
        "scroll->widget() == list", "scroll->isVisible()", "scroll->isEnabled()")
for action, bracket in (("OlderMessagesPage", "Left"), ("NewerMessagesPage", "Right")):
    assert f"{{ Action::{action}, {{ Qt::Key_Bracket{bracket}, Qt::Key_M }} }}" in keys
paging = between(dispatch, "case Action::OlderMessagesPage:", "case Action::SelectMessage:")
ordered(paging, "case Action::NewerMessagesPage:", "HistoryScroll(target)",
        "Qt::Key_PageDown", "Qt::Key_PageUp", "scroll->keyPressEvent(&page)")
for view, assignment in (
        ("history/history_widget.cpp", "_list ="),
        ("history/view/history_view_chat_section.cpp", "_inner ="),
        ("history/view/history_view_pinned_section.cpp", "_inner ="),
        ("history/view/history_view_scheduled_section.cpp", "_inner =")):
    assert f"{assignment} _scroll->setOwnedWidget(" in read(view), view
bindings = re.findall(r"\{ Action::(\w+), \{ ([^}]+) \} \},", keys)
chords = [tuple(keys.split(", ")) for _, keys in bindings]
assert len(set(chords)) == len(chords)
assert not any(a != b and b[:len(a)] == a for a in chords for b in chords)

painter = read("window/section_widget.cpp")
ordered(painter, "Purple::PaintHiddenChatBackground", "theme->background().giftId",
        "theme->backgroundState(fill)")
background = read("purple/purple_chat_background.cpp")
ordered(background, "readPref<bool>(kVisiblePref, true)", "widget->update()",
        "color.setAlpha(255)", "writePref<bool>(kVisiblePref, visible)",
        "VisibilityChanges.fire")
assert "{ Action::SelectMessage, { Qt::Key_G, Qt::Key_S, Qt::Key_S } }" in keys
assert "{ Action::CopyMessage, { Qt::Key_G, Qt::Key_S, Qt::Key_Y } }" in keys
ordered(routing, "QEvent::ShortcutOverride", "case KeyUse::Hint:",
        "_hints.type(key, target.messages)")
ordered(keys, "void Keys::clearPrefix()", "_hints.clear()")

hints = read("purple/purple_message_hints.cpp")
ordered(hints, "setFocusPolicy(Qt::NoFocus)", "Qt::WA_TransparentForMouseEvents")
ordered(hints, "intersected(visible)", "contentOverlapped", "element->data()->fullId()",
        "main->purpleEnumerateVisible(collect)", "section->purpleEnumerateVisible(collect)",
        "HintLabel(i, digits)")
ordered(hints, "bool MessageHints::eligible", "hasCopyRestriction", "goodForSelection",
        "groupIds.push_back", "MaxSelectedItems", "hasTextForCopy()")
ordered(hints, "if (source.scheduled)", "IsScheduledMsgId(item->id)", "item->isLocal()",
        "item->isSending()", "item->hasFailed()", "scheduledMessages().lookupItem",
        "else if (!IsServerMsgId(item->id)")
assert "select && (!section->_selectEnabled || section->hasSelectRestriction())" in hints
assert "section->isGoodForSelection(section->_selected, candidate, total)" in hints
assert "section->hasCopyRestriction(candidate)" in hints
watch = between(hints, "void MessageHints::watch", "MessageHints::Input MessageHints::type")
for cancellation in ("_purpleHintGeometryChanges", "shownValue()", "scrollTopChanges()",
                     "itemRemoved()", "viewRemoved()", "viewLayoutChanged()"):
    assert cancellation in watch, cancellation
pick = between(hints, "MessageHints::Input MessageHints::type", "void MessageHints::apply")
ordered(pick, "view.widget() != _view.data()", "view.scheduled != _scheduled",
        "&view.session() != _session.get()", "snapshot() != _entries",
        "QPointer(view.widget())", "const auto id = i->id", "const auto element = i->element",
        "const auto point = i->rect.center()", "clear();", "if (!widget)",
        "view.session().data().message(id)", "item && (view.main", "view.main->viewByItem(item)",
        "view.section->viewForItem(item)) == element", "apply(view, item, action, point)")
apply = between(hints, "void MessageHints::apply", "void MessageHints::clear")
ordered(apply, "if (action == Action::Menu)", "visibleRegion().contains(point)",
        "MessageHintMenuEvent(item, point", "main->showContextMenu", "view.section->showContextMenu",
        "if (action == Action::Copy)", "copyContextText", "return;",
        "clearTextSelection()", "changeSelectionAsGroup", "updateTopBarSelection(true)",
        "view.section->showCopyRestriction(item)", "HistoryGroupText(group)",
        "Iv::SetRichBlocksClipboard", "section->_purpleKeepSelectionFocus, true",
        "gsl::finally", "section->selectItemAsGroup(item)")

scope = between(keys, "Target FindTarget", "bool Available")
ordered(scope, "section->controller() == owner", "section->purpleMessageList(focus)",
        "HistoryView::ChatWidget", "HistoryView::PinnedWidget", "HistoryView::ScheduledWidget",
        "forSection(scheduled, true)")
for section, widget, guards in (
        ("chat", "ChatWidget", ("Mode::Replies", "!_composeSearch",
                                "!_chooseForReport", "allowsPurpleMessageKeys")),
        ("pinned", "PinnedWidget", ("!_composeSearch", "focus == _inner.data()")),
        ("scheduled", "ScheduledWidget", ("allowsPurpleMessageKeys",))):
    source = read(f"history/view/history_view_{section}_section.cpp")
    admission = between(source, f"{widget}::purpleMessageList", f"{widget}::listSelectionChanged")
    ordered(admission, "_inner->isVisible()", "!animatingShow()", *guards)

compose = read("history/view/controls/history_view_compose_controls.cpp")
admission = between(compose, "ComposeControls::allowsPurpleMessageKeys", "ComposeControls::focusedValue")
ordered(admission, "isEditingMessage()", "isRecording()", "_autocomplete->isVisible()",
        "_emojiSuggestions->shown()", "_inlineResults->isVisible()", "_tabbedPanel->isVisible()",
        "if (listFocused)", "_field->rawTextEdit().get()", "preeditAreaText().isEmpty()")

section_list = read("history/view/history_view_list_widget.cpp")
for name in ("refreshRows", "visibleTopBottomUpdated", "resizeGetHeight",
             "refreshAttachmentsAtIndex", "viewReplaced"):
    assert "_purpleHintGeometryChanges.fire" in section_list.split(f"ListWidget::{name}(", 1)[1].split(chr(10) + "}", 1)[0], name
chat = read("history/view/history_view_chat_section.cpp")
ordered(chat, "ChatWidget::listSelectionChanged", "!_inner->keepingPurpleSelectionFocus()",
        "doSetInnerFocus()")

assert "{ Action::MessageMenu, { Qt::Key_G, Qt::Key_S, Qt::Key_M } }" in keys
ordered(hints, "class MessageHintMenuEvent", "QContextMenuEvent::Keyboard",
        "dynamic_cast<const MessageHintMenuEvent*>", "hint->item.get() : focused")
for file, cls in (("history/history_inner_widget.cpp", "HistoryInner"),
                  ("history/view/history_view_list_widget.cpp", "ListWidget")):
    context = between(read(file), f"void {cls}::showContextMenu", "\n}\n")
    ordered(context, "Purple::MessageHintMenuItem", "e, _accessibilityFocusedItem",
            "QContextMenuEvent::Keyboard", "PrepareImportSettingsKeyboardHit", "keyboardItem",
            "showFrozenError()")
    assert context.count("_accessibilityFocusedItem") == 1

menu = read("window/window_main_menu.cpp")
ordered(menu, "MainMenu::MainMenu(", "setupMenu();",
        "Purple::InstallMainMenuKeys(this, _inner)")
menu_keys = between(keys, "void InstallMainMenuKeys", "void BuildModalKeysSetting")
ordered(menu_keys, "if (!Enabled())", "Settings::KeyNavigation", "QEvent::KeyPress",
        "Ui::ScreenReaderModeActive()", "QApplication::focusWidget() != menu.get()",
        "QApplication::activePopupWidget()", "QApplication::activeModalWidget()",
        "topShownLayer() != menu.get()", "navigation->handle(")

print("Modal key, menu, visible message, nested section and chat background routes passed")
