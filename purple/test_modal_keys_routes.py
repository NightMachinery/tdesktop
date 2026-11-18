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

painter = read("window/section_widget.cpp")
ordered(painter, "Purple::PaintHiddenChatBackground", "theme->background().giftId",
        "theme->backgroundState(fill)")
background = read("purple/purple_chat_background.cpp")
ordered(background, "readPref<bool>(kVisiblePref, true)", "widget->update()",
        "color.setAlpha(255)", "writePref<bool>(kVisiblePref, visible)",
        "VisibilityChanges.fire")
print("Modal key and chat background routes passed")
