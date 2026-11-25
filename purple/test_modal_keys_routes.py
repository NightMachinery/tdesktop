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
ordered(mac_native, "[event isARepeat]", "Purple::SwallowModalNativeRepeat", "return true;",
        "PossiblyTextTypingEvent(event)")
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
mainwidget = read("mainwidget.cpp")
ordered(mainwidget, "void MainWidget::updateControlsGeometry()", "_a_dialogsWidth.value",
        "floatPlayerUpdatePositions()", "Purple::RefreshModalKeysColumns(_controller->widget())")

keys = read("purple/purple_keys.cpp")
keys_ui = read("purple/purple_keys_ui.cpp")
ordered(keys, "class ModalStatusLayout", "widget->mapTo(body", "main->_dialogs.get()",
        "main->_thirdSection.data()", "KeyboardAttachment::Geometry(section)",
        "fullTextMargins().left()", "composer.placeholderInset", "composer.iconWidth",
        "st::forumTopicRow : st::defaultDialogRow", "main->_sideShadow.get()", "main->_thirdShadow.data()")
ordered(keys, "bool Keys::eventFilter", "type == QEvent::Move", "_statusLayout.tracks", "refreshColumns()")
ordered(keys_ui, "void ModalStatusBar::paintEvent", "PaintModalStatusBar(",
        "st::sideBarBg", "st::dialogsBg", "st::historyComposeAreaBg", "st::shadowFg",
        "st::windowFiltersButton.style.font->f")
chrome = between(keys, "void Keys::raiseChrome", "Target Keys::currentTarget")
ordered(chrome, "topShownLayer()", "geometry().bottom()", "_statusBar->stackUnder(stack)",
        "_whichKey->stackUnder(stack)", "_statusBar->raise()")
assert keys.count("_statusBar->raise()") == 1
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
section_builder = read("settings/settings_builder.cpp")
ordered(section_builder, "SectionBuilder::controller() const",
        "const SearchContext &) -> Window::SessionController*", "return nullptr")
purple_section = read("purple/purple_settings_section.cpp")
ordered(purple_section, 'u"advanced/purple_customize_settings"_q',
        "if (const auto own = current())", "ShowCustomizeSettings(&own->window(), customize)")
assert not any("builder.controller()->" in source.read_text(encoding="utf-8")
               for source in (SOURCE / "purple").glob("*.cpp"))
bound("OpenCalls", "SPC o p")
assert len(re.findall(r'"SPC f e d"\s+:', defaults)) == 1
assert re.search(r'"SPC f e"\s+"edit settings"', defaults)
assert 'pushRow(leader.rows, rowFor(Action::CustomizeSettings))' in keys
assert command_ids["CustomizeSettings"] == "customize-settings"
ordered(keys, "case Action::CustomizeSettings:", "ShowCustomizeSettings")
customize = read("purple/purple_settings_customize.cpp")
ordered(customize, "class CustomizeLayer final", "passcodeLockChanges()", "PrivacyScreenChanges()",
        "void protectionChanged()", "_protectedForm.hide", "Core::App().passcodeLocked()",
        "setParent(_window->bodyWidget())", "_presentation.clear()", "refreshFromDisk()",
        "_resumeRefused = _draftStale", "present(anim::type::instant)", "_coverClose->show()",
        "_protectedForm.restore", "setCurrentWidget(_browser)")
ordered(customize, "bool privacyKey", "qobject_cast<QLineEdit*>", "Qt::Key_F8", "TogglePrivacyScreen")
ordered(customize, "FilterCustomizeSettingsInput", "editor->privacyKey")
ordered(keys, ".onPick = [guard", "CommandAvailable(action, restored)", "guard->dispatch(action, restored, 1)")
customize_look = read("purple/purple_settings_customize_look.h")
ordered(customize_look, "class Canvas", "paintEvent", "customizeBackground")
ordered(customize, "new CustomizeLook::Canvas(_pages)", "new CustomizeLook::Canvas(_sheetPage)",
        "setViewport(new CustomizeLook::Canvas(_sheetPage))")
ordered(customize, "void addSheetWidget", "CustomizeLook::AppendAction(_sheetLayout, button)")
ordered(customize_look, "class Chip", "setToolTip(details)", "setAccessibleDescription(details)", "Qt::ElideMiddle")
ordered(customize_look, "class Tree", "setAlternatingRowColors(false)", "drawBranches")
assert "new QPushButton" not in customize
ordered(customize, "void closeForm()", "_exitFocus.restore(_sheet)", "SettingsFocus(_lastField")
ordered(customize, "void returnToTree()", "SettingsFocus(_browserFocus)", "previous.restore(_browser)")
ordered(customize, "void showConflict(", "SummarizeConfigChange(preview.before, disk->bytes)",
        "SettingsReviewDisplay::Conflict", "raw->hide()", "Show raw files")
ordered(customize, "QTextEdit *sheetText", "WrapAtWordBoundaryOrAnywhere", "ScrollBarAlwaysOff")
ordered(customize, "sessionDataAboutToBeCleared()", "retire()", "PrivacyScreenChanges()")
ordered(customize, "QEvent::InputMethod", "preeditString()", "QEvent::KeyPress", "handleKey(")
ordered(customize, "bool handleKey", "_preedit.contains(object)", "return false", "ShouldHandleEditorKey")
ordered(customize, "new QPlainTextEdit", "setPlainText", "stageForm()", "PrepareWindowSettingsEditorEdit")
ordered(customize, "void submitReview", "_applyInFlight = true", "QPointer(this)",
        "RequestSettingsEditorApply", "weak->_applySerial == serial", "applyCompleted")
assert customize.count("RequestSettingsEditorApply(") == 1
ordered(customize, "void chooseAccount", "SessionAccountKey(session) != key", "ensureSeparateWindowFor",
        "Review and apply", "Discard and switch")
ordered(customize, "void showCollection", "LiveSessionFor(target.account)", "PeerCatalogue(session, *field)",
        "CreateSettingsCollection", "stageIntent(std::move(intent), key)")
collections = read("purple/purple_settings_collection_ui.cpp")
ordered(collections, "new CustomizeLook::Actions(_main)", "button(footer,", "CustomizeLook::AppendAction(layout, result)")
collection_edit = collections[collections.index("void edit(Op op)"):collections.index("bool run(std::vector<SettingsEdit>")]
ordered(collection_edit, "FindSettingDescriptor(_config.address)", "if (!descriptor)", "return;", "descriptor->id")
ordered(collection_edit, "FindSettingDescriptor(item.address)", "if (_config.named && !insert && !objectDescriptor)", "return;")
assert not re.search(r"FindSettingDescriptor\([^\n]+\)->", collections)
ordered(collections, "void showOtherReferences", "_config.otherReferences(object)", "Other accounts’ files are not checked")
ordered(collection_edit, "showOtherReferences(item.address)", "_accept = button")
ordered(collections, 'u"Add"_q, [this] { add()', 'u"Enter ID"_q, [this] { edit(Op::Insert)',
        'u"Duplicate"_q, [this] { edit(Op::Duplicate)',
        'u"Delete"_q, [this] { edit(Op::Erase)', 'u"Move up"_q, [this] { move(-1)',
        "&QAbstractItemModel::rowsMoved", "moveTo(from, to)",
        "code == Qt::Key_D", "edit(Op::Duplicate)", "code == Qt::Key_A", "add()", "edit(Op::Erase)")
ordered(collections, "void add()", "_config.peerField", "addPeer()", "edit(Op::Insert)")
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
palette_ui = between(keys_ui, "class CommandPaletteLayer", "class UnreadBufferRowsWidget")
ordered(keys_ui, "void InstallListFilter", "ListFilter::Mark(edit)", "preeditAreaText().isEmpty()",
        "ListFilter::ProtectPreedit", "FilterResult::Cancel", "FilterResult::Continue", "handler(key)")
ordered(palette_ui, "InstallListFilter(this, _input", "ListFilter::Submit(key)", "AllowModalLayerKey",
        "ListFilter::Exit", "cancel()", "Qt::Key_F6", "setFocus()",
        "ListStep(key, mods, false)",
        "Qt::AltModifier", "stepHistory(")
ordered(palette_ui, "void keyPressEvent", "ListStep(key, mods, ModalKeysEnabled())",
        "AllowModalLayerKey(*event, this, true)", "moveSelection(step)",
        "ListFilter::Submit(key)", "activateRow(_selected)", "ListFilter::Exit", "cancel()")
ordered(palette_ui, "void doSetInnerFocus()", "_input->setFocusFast()")
assert "_input->setText(QString())" not in palette_ui
for layer, last in (("UnreadBufferLayer", "TopicRowsWidget"), ("TopicPickerLayer", "NoticeBodyWidget")):
    body = between(keys_ui, f"class {layer}", f"class {last}")
    input_keys = between(body, "InstallListFilter(this, _filter", "\n\t\t});")
    ordered(input_keys, "ListFilter::Exit", "AllowModalLayerKey", "setFocus()")
    assert "setText(" not in input_keys
    ordered(body, "void keyPressEvent", "ClearListFilterOrClose(this, _filter)", "ListFilter::Enter(*e, key)")
help_layer = between(keys_ui, "class ModalHelpLayer", "class DescribeLayer")
ordered(help_layer, "InstallListFilter(this, _filter", "ListFilter::Exit", "AllowModalLayerKey",
        "setFocus()", "closeLayer()", "FilterResult::Cancel")
assert "_filter->setText(QString())" not in help_layer
ordered(help_layer, "void keyPressEvent", "ClearListFilterOrClose(this, _filter)", "ListFilter::Enter(*e, key)")
ordered(keys, "void Keys::showPalette", "EffectiveBindings(true, target.contextMap)",
        "ChordsForAction(cmd.action, effective)", "fromInsert", "origin->setFocus",
        ".describeMode = describe", ".onCancel", "restoreOrigin()")
unread = between(keys_ui, "class UnreadBufferLayer", "class TopicRowsWidget")
ordered(unread, 'u"i or / to filter"_q', "_filter->submits()", "openRow(_selected)", "InstallListFilter")
ordered(unread, "void keyPressEvent", "key == Qt::Key_Q", "closeLayer()", "_pendingConfirmTargets.empty()")
ordered(unread, "_filter->hasFocus()", 'u"RET open · C-n/C-p move · Esc list"_q',
        "ModalUnread::SelectedPeer", "_selection.refilter")
notice_history = keys_ui[keys_ui.index("class NoticeHistoryLayer final"):]
ordered(notice_history, "InstallListFilter(this, _filter", "ListFilter::Exit", "setFocus()",
        "void keyPressEvent", "ClearListFilterOrClose(this, _filter)", "ListFilter::Enter(*e, key)")
for layer, end in (("UnreadBufferLayer", "TopicRowsWidget"),
                   ("TopicPickerLayer", "NoticeBodyWidget"),
                   ("NoticeHistoryLayer", "CreateNoticeHistoryLayer")):
    body = between(keys_ui, f"class {layer}", end)
    ordered(body, "_selection.keepCurrentOnClear()", "ClearListFilterOrClose(this, _filter)",
            "_selection.refilter(")
ordered(keys, "void Keys::showNoticeLayer", "if (isHistory)", "rememberFilterFocus(raw)")
ordered(customize, "if (object == _filter)", "ListFilter::Exit", "_tree->setFocus()", "ListFilter::Enter(*event, key)")
assert "_filterBeforeEdit" not in customize
ordered(collections, "object == _search && ListFilter::Exit", "_peerList->setFocus()", "ListFilter::Enter(*key, code, false)")
ordered(read("boxes/peer_list_box.cpp"), "Purple::InstallPickerFilter", "Purple::IsChatPaletteController",
        "void PeerListBox::keyPressEvent", "Purple::HandlePickerFilter", "Purple::IsChatPaletteController")
ordered(read("purple/purple_picker.cpp"), "void InstallPickerFilter", "preeditAreaText().isEmpty()",
        "ListFilter::ProtectPreedit", "ListFilter::Exit", "if (closeOnEscape)", "box->closeBox()",
        "list->setFocus()", "bool HandlePickerFilter", "if (closeOnEscape", "box->closeBox()",
        "ListFilter::Enter", "filter->clearQuery()")
ordered(keys_ui, "void ClearListFilterOrClose", "getLastText().isEmpty()", "CloseModalFilterLayer(layer)", "setText(QString())")
ordered(keys, "void Keys::rememberFilterFocus", "windowFocusWidget()", "revision == guard->_revision",
        "origin->setFocus", "void Keys::closeFilterLayer", "&QObject::destroyed", "QTimer::singleShot", "layer->closeLayer()")
ordered(keys, "void Keys::refreshStatusBar", "ListFilter::IsMarked(focus)", "case ActiveLayer::UnreadBuffer",
        "if (filterFocused)", "ModalUi::BadgeKind::Insert", 'u"INSERT"_q')

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
assert 'u"Unread unmuted chats"_q' in wk
assert 'u"Unread unmuted chats (iBuffer)"_q' in commands
which_key_paint = between(keys_ui, "void WhichKeyOverlay::paintEvent", "class CheatsheetGrid")
ordered(which_key_paint, "MeasureWhichKeyCapsWidth(entry.keys", "std::max(28, capsW)", "arrowX")
assert "_colCapWidths" not in keys_ui

# Verify active-layer and prefix/which-key status bar cues replace Normal-mode cues.
status_fn = between(keys, "void Keys::refreshStatusBar", "KeyUse Keys::classify")
for layer_case in ("ActiveLayer::Cheatsheet", "ActiveLayer::AllBindings",
                   "ActiveLayer::Palette", "ActiveLayer::DescribeKey",
                   "ActiveLayer::StockShortcuts", "state.hints",
                   '{ u"DEL"_q, u"back"_q }', '{ u"ESC"_q, u"cancel"_q }'):
    assert layer_case in status_fn, layer_case

# Verify cheatsheet preserves key casing, derives live-available SPC prefixes, and tags localleader rows.
assert "card.title.toUpper()" not in keys_ui
assert "group.title.toUpper()" not in keys_ui
assert "QApplication::isRightToLeft" not in keys_ui
assert "return style::RightToLeft();" in keys_ui
assert "MeasureTextWidth(" in keys_ui
assert "ElideText(" in keys_ui
help_fn = between(keys, "void Keys::showHelpLayer", "void Keys::showDescribeKeyResult")
ordered(help_fn, ".contextTag = mapContextTag(b.sourceMap)",
        "Qt::Key_S, Qt::Key_T", "Available(b.action, target)",
        ".contextTag = ctx", "requireAvailable && !Available(b.action, target)", "onTabChanged")

# Security & focus gates.
gate = between(keys, "Window::SessionController *Keys::owner()", "bool Keys::chromeVisible")
for protected in ("passcodeLocked()", "screenIsLocked()", "window.locked()",
                  "PrivacyScreenActiveAny()", "PeekCountdownPending()", "activePopupWidget()",
                  "activeModalWidget()", "account.loggingOut()", "account.maybeSession()"):
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
ordered(routing, "ModalKey(*keyEvent)", "PhysicalIdentity(*keyEvent)", "QEvent::KeyRelease",
        "_motion.release(physical)", "ClaimedKeys.erase(physical)",
        "if (keyEvent->isAutoRepeat())", "ClaimedKeys.find(physical)",
        "_held.contains(physical)", "repeat(*keyEvent, receiver)", "if (!Enabled())",
        "StockModeShortcut", "MxShortcutInStockMode()", "receiver == _preeditInput",
        "currentTarget(receiver, true)", "event->accept()", "QEvent::KeyPress",
        "!keyEvent->isAutoRepeat()", "showPalette(false, target, false)",
        "const auto target = currentTarget(receiver)", "if (!target.focus)",
        "QEvent::ShortcutOverride", "event->accept()", "return true;",
        "keyEvent->isAutoRepeat()", "_held.insert(physical)",
        "case KeyUse::CountDigit:", "case KeyUse::PrefixBackspace:",
        "stroke(HoldStroke(*keyEvent), target)")
ordered(routing, "const auto result = _hints.type", "if (!guard)",
        "result == MessageHints::Input::LeaveNormal", "enterInsert(true)")

stroke = between(keys, "void Keys::stroke", "void Keys::dispatch")
ordered(stroke, "_frozenEffective = EffectiveBindings", "IsExactLocalleader(_prefix)",
        "_swallowNext = true", "const auto action = b.action", "const auto inspect = _inspect",
        "KeyRepeat::CanArm", "clearPrefix(RepeatPolicy::Never, opensCursor)", "if (inspect)", "showDescribeKeyResult",
        "CommandAvailable(action, target)", "_motion.start", "dispatch(action, target, count,")
repeat_path = between(keys, "void Keys::repeat", "bool Keys::layerKey")
ordered(repeat_path, "Enabled()", "_normal", "_preeditInput", "_prefix.empty()", "_count",
        "currentTarget(receiver)", "Available(*action, target)", "_motion.next",
        "dispatch(*action, target, 1, { PhysicalIdentity(event), true })")
scroll_key = between(keys, "void Keys::scrollKey", "bool Keys::advanceScroll")
ordered(scroll_key, "readScroll(view, layer)", "anim::Disabled()", "ScrollTo(view, current->top + delta)",
        "_pixel.expectedTop", "press.repeat && style == ScrollStyle::Continuous",
        "_pixel.motion.startHold", "_pixel.motion.step", "_scrollFrame.start()")
ordered(keys, "Keys::readScroll", "owner()", "HistoryScroll(target)",
        "scroll->movement()", "KeyboardAttachment::ScrollAnimating", "animatedScrolling()")
ordered(keys, "void ScrollTo(", "elastic->scrollToY", "scroll->scrollToY",
        "QAbstractScrollArea*>(view)->verticalScrollBar()->setValue(top)")
ordered(keys, "bool Keys::advanceScroll", "current->top != _pixel.expectedTop",
        "_pixel.motion.advance", "_pixel.expectedTop = next", "ScrollTo(_pixel.view, next)")
ordered(routing, "_motion.release(physical)", "_pixel.motion.release", "_pixel.physical = 0")
ordered(keys, "type == QEvent::Wheel", "cancelScroll()")
for surface in ("ModalHelpLayer", "NoticeLayer"):
    assert "ScrollModalLayer" in keys_ui.split(f"class {surface} final", 1)[1].split("\nclass ", 1)[0]
ordered(customize, "if (_reviewBody && object == _reviewBody", "ScrollModalLayer", "singleStep()")
ordered(keys, "QEvent::InputMethodQuery", "watched == _normalComposer",
        "Enabled() && _normal && owner()", "setValue(Qt::ImEnabled, false)")
ordered(keys, "void Keys::clearPrefix", "_motion.cancel(keepMotion)", "_layerMotion.cancel()")
ordered(keys, "owner->activeChatEntryChanges()", "RepeatPolicy::ChatNavigation",
        "owner->activeChatsFilter()", "contextChanged(folderMotion())", "contextChanged(RepeatPolicy::Never)")
actions = between(keys, "const Command kCommands[]", "using Chord")
armed = re.findall(r"\{ Action::(\w+),[^\n]*\n[^\n]*RepeatPolicy::(SameTarget|ChatNavigation|FolderNavigation)", actions)
assert set(armed) == {
    ("MoveUp", "SameTarget"), ("MoveDown", "SameTarget"),
    ("PreviousChat", "ChatNavigation"), ("NextChat", "ChatNavigation"),
    ("HistoryHalfUp", "SameTarget"), ("HistoryHalfDown", "SameTarget"),
    ("OlderMessagesPage", "SameTarget"), ("NewerMessagesPage", "SameTarget"),
    ("SeekBackward", "SameTarget"), ("SeekForward", "SameTarget"),
    ("PreviousFolder", "FolderNavigation"), ("NextFolder", "FolderNavigation"),
}

palette = between(keys, "void Keys::showPalette", "} // namespace")
ordered(palette, "revision = _revision", "base::make_weak(current)",
        "guard = QPointer(this)", "closePalettePosted",
        "stack->topShownLayer() == raw", "anim::type::instant",
        "ModalUi::CreateCommandPaletteLayer",
        "if (!guard)", "QTimer::singleShot(0, guard.data()",
        "revision != guard->_revision", "closePalettePosted();",
        "live != original.get()", "revision != guard->_revision", "origin->setFocus",
        "guard->owner() != original.get()", "guard->currentTarget(origin, !Enabled())",
        "CommandAvailable(action, restored)", "MxHistory().record", "guard->dispatch(action, restored",
        "onCancel = [guard,")

target_gate = between(keys, "Target Keys::currentTarget", "QWidget *Keys::windowFocusWidget")
ordered(target_gate, "QApplication::focusWidget()", "owner()", "focus->isVisible()",
        "focus->isEnabled()", "focus->window() == _window", "receiver == focus",
        "isLayerShown()", "FindTarget(current, focus, allowUnmapped)")
ordered(keys, "Target FindTarget(", "history->allowsPurpleKeys(focus)", ": Target();",
        "return allowUnmapped ? Target")
ordered(keys, "void SetEnabled(bool enabled)", "writePref<bool>(kEnabledPref", "EnabledChanges.fire_copy")
mode_change = between(keys, "EnabledChanges.events() | rpl::on_next", "KeymapChanges.events() | rpl::on_next")
ordered(mode_change, "++_revision", "_held.clear()", "enterNormal(true)", "closeKeyLayers()",
        "clearPrefix()", "_window->updateControlsGeometry()")
assert "hideLayer" not in mode_change and "Customize" not in mode_change
ordered(keys, "case Action::ModalInputToggle: {", "SetEnabled(enabled)", "ShowPurpleAck",
        "lng_purple_keys_modal_input_on", "lng_purple_keys_modal_input_off")
assert ":modal-input-toggle" not in defaults
ordered(keys, "bool Keys::chromeVisible() const", "return Enabled()")
ordered(keys, "void BuildModalKeysSetting", "on_next(SetEnabled", "MxShortcutInStockModeValue()",
        "SetMxShortcutInStockMode")
ordered(customize, "new QCheckBox(_browser)", "MxShortcutInStockModeValue()",
        "QSignalBlocker", "if (visibleContent())", "SetMxShortcutInStockMode")
ordered(palette, "for (const auto &cmd : kCommands)", "cmd.action == Action::Run",
        "CommandAvailable(cmd.action, target)", "CommandUnavailableReason(cmd)")
needs_normal = between(keys, "bool RequiresModalInput", "QString CommandUnavailableReason")
for action in ("SelectMessage", "DescribeChord", "UnreadBuffer", "TopicPalette"):
    assert f"Action::{action}" in needs_normal, action
for action in ("Help", "Cheatsheet", "DescribeCommand", "TogglePrivacyScreen",
               "CustomizeSettings", "ModalInputToggle"):
    assert f"Action::{action}" not in needs_normal, action

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
ordered(owner_fn, "QApplication::activeWindow() != _window", "Core::App().activeWindow() != &window")

dispatch = between(keys, "void Keys::dispatch", "void Keys::showHelpLayer")
assert "chooseRow" not in between(dispatch, "case Action::PreviousChat:", "case Action::OpenChat:")
ordered(dispatch, "case Action::ToggleNightMode:", "editingTheme()",
        "ToggleNightModeWithConfirmation", "Window::Theme::ToggleNightMode();",
        "Window::Theme::KeepApplied();")
ordered(dispatch, "case Action::OpenChat:", "chooseRow()")
cursor = between(keys, "Dialogs::InnerWidget *Keys::liveChatCursor", "QWidget *Keys::windowFocusWidget")
ordered(cursor, "controller != owner()", "isLayerShown()", "ChatsInnerList(controller)",
        "list != _chatCursor.list", "purpleKeyboardSelection()", "key != _chatCursor.row",
        "key.owningHistory()", "purpleHiddenFromView()")
ordered(between(keys, "bool Available(", "bool RequiresModalInput"),
        "case Action::PreviewChat:", "case Action::OpenChat:", "target.chatCursorAvailable")
ordered(keys, "const auto stepChatList", "ChatsInnerList(current)", "processKeyDispatch",
        "purpleKeyboardSelection()", "_chatCursor = row")
ordered(dispatch, "case Action::OpenChat:", "liveChatCursor(current)", "_chatCursor = {};",
        "list->showChatPreview()", "list->chooseRow()", "activeChatCurrent() == picked", "activate()")
ordered(stroke, "const auto opensCursor", "target.chatCursorAvailable",
        "Action::OpenChat || action == Action::PreviewChat", "clearPrefix(RepeatPolicy::Never, opensCursor)")
ordered(keys, "void Keys::clearPrefix", "if (!keepChatCursor)", "_chatCursor = {};")
ordered(keys, "bool Keys::eventFilter", "type == QEvent::MouseMove", "globalPosition().toPoint()",
        "type == QEvent::MouseButtonPress", "type == QEvent::Hide", "_chatCursor = {};")
ordered(status_fn, "target.focus && liveChatCursor(target.controller)", '{ u"RET"_q, u"open"_q }')
ordered(read("dialogs/dialogs_widget.cpp"), "InnerWidget *Widget::purpleVisibleKeysList",
        "_childList->purpleVisibleKeysList()", "purpleKeysList(this)")
ordered(read("dialogs/dialogs_inner_widget.cpp"), "InnerWidget::computeChosenRow",
        ".key = row->folder", ".key = _selected->key()", "Key InnerWidget::purpleKeyboardSelection",
        "!_mouseSelection", "computeChosenRow().key")
assert '"J/K to pick a chat first"' in keys

# Universal hints use the active layer, reuse the message engine, and validate again after capture.
universal = read("purple/purple_message_hints.cpp")
hint_targets = read("purple/purple_hint_targets.cpp")
ordered(keys, "Ui::RpWidget *Keys::hintRoot()", "if (!owner())", "stack->topShownLayer()")
ordered(keys, "bool Keys::handleWindowHints", "receiver == _preeditInput", "_hints.windowActive()",
        "event->isAutoRepeat()", "_hints.typeWindow(key, hintRoot(), &current->session(),")
ordered(universal, "bool MessageHints::startWindow", "CollectWindowHintTargets(root)",
        "appendWindowMessages", "HintLabels(targets.size())", "WindowHintExposed", "PlaceHintBadge")
ordered(universal, "MessageHints::Input MessageHints::typeWindow", "session != _session.get()",
        "Qt::Key_Backspace", "_prefix.chop(1)", "auto entry = std::move", "clear()",
        "entry.current()", "WindowHintExposed", "entry.activate()")
ordered(universal, "void MessageHints::appendWindowMessages", "purpleHiddenFromView()", "fullId()",
        "alive->data().message(id)", "CanSendReply(item)", "VisibleBodyLinks", "purpleEnumerateVisible")
ordered(hint_targets, "class HintDialogRows", "At(list, index)", "At(guard, index) == identity")
ordered(hint_targets, "class HintMediaRows", "item->fullId()", "live.layout->getItem()->fullId() == id")
ordered(hint_targets, "void AddItemView", "QPersistentModelIndex(view->indexAt", "index.isValid()",
        "identity.current()", "ClickWindowHint(viewport, local)")
ordered(read("purple/purple_hint_item_identity.h"), "bool current() const", "index.isValid()",
        "view->viewport() == viewport", "view->indexAt(point) == index")
ordered(customize, "FilterCustomizeSettingsInput", "owns(target)", "HandleModalWindowHints(target, event)")
ordered(hint_targets, "void AddButton", "SendButton::Type::Record", "SendButton::Type::Round",
        "send->state() == *sendState", "clicked(Qt::NoModifier, Qt::LeftButton)")
hydra = read("purple/purple_hint_hydra.cpp")
ordered(hydra, "void keyPressEvent", "AllowModalLayerKey(*e, this)", "live()", "Qt::Key_Backspace", "Qt::Key_Return")
ordered(hydra, "void load(", "menu->actions()", "action->isEnabled()", "menu->ensureSubmenu", "HintMnemonics")
ordered(hydra, "void choose(", "action.child", "action.native->trigger()", "const auto menu = std::move(_menu)",
        "closeLayer();", "if (allowed() && current())", "trigger()")
ordered(keys, "void Keys::showHintHydra", "guard->owner() == original.get()", "target.nativeMenu()", "target.current()",
        "CreateHintHydra", "data.itemRemoved()", "data.itemDataChanges()", "ranges::contains(watched, id)")
ordered(universal, "WindowHintTarget::Menu MessageHints::buildMenu", "view.session().frozen()",
        "MessageHintMenuEvent", "main->showContextMenu(&event)", "request.passive = true", "FillContextMenu")
ordered(read("history/history_inner_widget.cpp"), "Purple::AddSelfDestructSaveAction(_menu.get(), leaderOrSelf)",
        "Purple::CaptureHintMenu(e, _menu)", "AttachSelectorToMenu", "_menu->popupPrepared()")
assert "->popup(" not in hydra

# Bare J/K select folders only in the list. The other chat-cursor chords
# remain in Normal, and explicit overlays take precedence over the defaults.
chat_map = between(defaults, "\n :chats", "\n :music")
normal_map = between(defaults, "\n :normal", "\n :chat")
for action, chord in (("NextFolder", "J"), ("PreviousFolder", "K"),
                      ("NextFolder", "f"), ("PreviousFolder", "F")):
    bound(action, chord, chat_map)
    assert len(re.findall(rf'"{chord}"\s+:', chat_map)) == 1
for action, chord in (("NextChat", "J"), ("PreviousChat", "K"),
                      ("NextChat", "C-j"), ("PreviousChat", "C-k"),
                      ("NextChat", "] c"), ("PreviousChat", "[ c"),
                      ("NextFolder", "] f"), ("PreviousFolder", "[ f")):
    bound(action, chord, normal_map)
ordered(keys, "bool IsActiveDirectChatListBinding", "IsDirectChatListChord",
        "binding.user", "normalChatListOverrides")
ordered(keys, "std::vector<EffectiveBinding> EffectiveBindings", "contextMap == MapKind::Chats",
        "IsActiveDirectChatListBinding", "directKeys.insert",
        "b.map == MapKind::Normal && !directKeys.contains")
ordered(keys, "void Keys::showWhichKey()", "EffectiveBindings(_normal, target.contextMap)",
        "other.sourceMap == MapKind::Chats", "!other.viaLocalleader", 'u" (%1 from list)"')
ordered(status_fn, "_normal && target.folders", "StepModalChatsFilter(target.controller, 1, false)",
        "HasFolderKeys()", '{ u"J/K"_q, u"folders"_q }')
ordered(keys, "Target FindTarget", ".folders = owner->isPrimary()",
        "!owner->openedFolder().current()", "!owner->shownForum().current()")
ordered(dispatch, "case Action::PreviousFolder:", "StepModalChatsFilter(current, delta, true)",
        'u"No other folders"', "setInnerFocus(true)", "purpleKeyboardSelection()", "selectSkip(1)")
ordered(read("purple/purple_folder_strip.cpp"), "bool StepModalChatsFilter", "ShownList(session)",
        "dialogFiltersCurrent()", "count < 2", "(start + delta % count + count) % count",
        "setActiveChatsFilter(list[changed].id())")
ordered(read("window/window_filters_menu.cpp"), "_session->activeChatsFilter(", "scrollToButton(j->second)")

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
    ("WindowHint", "f"),
    ("WindowHintMenu", "F"),
    ("OpenMessageUrl", "g o"),
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
        "onCreateTopic", "guard->owner() != ctrl", "CreateTopic(ctrl, liveForum)")
step_topic = between(keys, "void Keys::stepTopic", "void Keys::enterNormal")
ordered(step_topic, "preloadTopics()", "if (unreadOnly)", "topicsLoadFailed()", "topicsList()->loaded()",
        "FindNextUnreadTopic", "showThread(topics[*index])", "remaining == Pages::Complete",
        "No unread topics", "ChordsForAction(", "Action::TopicUnreadPalette", "EffectiveBindings",
        "M-x topic-unread-palette", "Could not load more topics", "No unread topic among loaded topics")
forum = read("data/data_forum.cpp")
ordered(forum, "void Forum::reloadTopics", "_topicsLoadFailed = false", "_topicsList.setLoaded(false)")
ordered(forum, "void Forum::requestTopics", ".fail", "_topicsLoadFailed = true", "_topicsList.setLoaded()")
ordered(keys, "case Action::TopicCreate:", "CreateTopic(current, forum)")
create_topic = between(keys, "void CreateTopic(", "ModalUnread::ChatBucket BucketForPeer")
ordered(create_topic, "Data::IsBotUserCreatesTopics", "showPeerHistory", "return;",
        "reserveNewBotTopic", "showSection", "HistoryView::ChatMemento",
        ".repliesRootId = topic->rootId()", "SectionShow::Way::ClearStack", "NewForumTopicBox")
assert "->showThread(" not in create_topic
ordered(keys, "QString TopicActionLabel", "Data::IsBotUserCreatesTopics",
        "New bot thread", "Message bot off-topic")
ordered(keys, "void Keys::showWhichKey", "CommandLabel(b.action, target)")
ordered(keys, "void Keys::showPalette", "CommandLabel(cmd.action, target)")
stock_create = read("boxes/peers/edit_forum_topic_box.cpp")
ordered(stock_create, "const auto create = ", "showSection", "ChatMemento", "reserveCreatingId")
histories = read("data/data_histories.cpp")
ordered(between(histories, "void Histories::sendCreateTopicRequest", "bool Histories::isCreatingTopic"),
        "unregisterMessageRandomId(randomId)", "base::take(i->second)", "_creatingTopics.erase(i)",
        "_creatingTopicRequests.erase(entry.requestId)", "entry.fail(error, response)")
stock_replies = read("window/window_session_controller.cpp")
ordered(stock_replies, "void SessionNavigation::showRepliesForMessage", "unreadCountKnown()",
        "!history->peer->asChannel()", "return;")
ordered(keys, "avatarSession", "avatarPeerId")

keys_ui = read("purple/purple_keys_ui.cpp")
ordered(keys_ui, "class TopicPickerLayer final", "_config.actionLabel",
        "_topicAction->setClickedCallback", "Qt::Key_C && !mods && _topicAction", "openTopicAction()")
ordered(keys_ui, "sends read receipts", "ModalUnread::NeedsMarkReadConfirmation", "executeMarkRead")
unread_layer = between(keys_ui, "class UnreadBufferLayer final", "class TopicRowsWidget final")
ordered(unread_layer, "void refilter(uint64_t selectedPeer)", "_selection.refilter(",
        "_rows->setData", "syncSelection()", "void syncSelection()", "scrollToY(rowTop,")
ordered(unread_layer, "void openRow(int row)",
        "_config.items[_visible[row]].peerId", "cb(peerId)")
notice_history = between(keys_ui, "class NoticeHistoryLayer final",
                         "std::unique_ptr<Ui::LayerWidget> CreateNoticeHistoryLayer")
ordered(notice_history, "void refilter()", "Identity(item.accountKey, item.id)", "_selection.refilter(",
        "_rows->setData", "ensureSelectionVisible()", "void ensureSelectionVisible()", "scrollToY(rowTop,",
        "void inspectRow(int row)", "_config.items[_visible[row]]", "cb(std::move(rec))")
ordered(keys_ui, "paintUserpic")
ordered(keys_ui, "class TopicPickerLayer final", "QTimer::timeout", "_config.pageUpdates",
        "_rootsBeforeLoad.contains", "QTimer::singleShot(0, this, [=] { maybeRequestMore(); })",
        "Qt::Key_R && !mods", "finishLoading()", "_config.onReload()")
ordered(keys_ui, "class NoticeLayer final", "class NoticeHistoryLayer final",
        "CreateNoticeLayer", "CreateNoticeHistoryLayer")
notice = between(keys_ui, "class NoticeLayer final", "class NoticeHistoryRowsWidget final")
ordered(notice, "AllowModalLayerKey(*e, this", "_keys.escape()", "NoticeEscape::CancelPrefix",
        "NoticeEscape::HideHelp", "NoticeEscape::Close", "e->isAutoRepeat() || elapsed < 180",
        "if (_keys.pendingG)", "_scroll->scrollToY(0)", "return;", "activateButton(_focusedButton)")
ordered(keys, "void Keys::schedulePendingNoticeFlush", "_noticeLayers.empty()",
        "&QObject::destroyed", "_noticePresentation == presentation", "MaybeFlushPendingNotices")
ordered(keys, "PeekCountdownChanges()", "schedulePendingNoticeFlush()", "PeekCountdownPending()")
wait_cpp = read("purple/purple_peek_controls.cpp")
ordered(wait_cpp, "PendingWait::PendingWait()", "aboutToQuit", "_shuttingDown = true", "cancel()")
ordered(wait_cpp, "void PendingWait::watch", "widget->lifetime().add", "_originWidget == widget.get()", "cancel()")
ordered(wait_cpp, "void PendingWait::quietCancel", "cancel()", "void PendingWait::cancel",
        "onResult = _onResult", "_gate.cancel()", "closeCover()", "onResult.deliver({})")
ordered(wait_cpp, "WaitResult PendingWait::request", "if (pending())", "cancel()", "receiver.deliver({})",
        "_onResult = receiver", "sessionDataAboutToBeCleared()", "cancel()")
ordered(wait_cpp, "void PendingWait::tick", "presenceChanged()", "generation != _coverGeneration", "_gate.tick(now)")
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
diagnostics_ui = read("purple/purple_diagnostics_ui.cpp")
ordered(keys, '"log-export"', '"log-clear"', "case Action::LogExport:",
        "Diagnostics::CanManage", "Diagnostics::ShowExport", "Diagnostics::ShowClear")
ordered(diagnostics_ui, "void ShowExport", "CanManage(window)", "FileDialog::GetWritePath",
        "CurrentWindow(window)", "IsAppDataPath(path)", "QSaveFile(path)", "file.commit()")
ordered(diagnostics_ui, "void ShowClear", "CanManage(window)", "Ui::MakeConfirmBox",
        "CurrentWindow(window)", "close()", "current()", "Clear()")
ordered(read("purple/purple_settings_section.cpp"), "void BuildPurpleDiagnosticsSettings", "CurrentSettingsWindow(builder)",
        "advanced/purple_log_export", "Diagnostics::ShowExport",
        "advanced/purple_log_clear", "Diagnostics::ShowClear")
ordered(config_cpp, "void Config::reloadFromDisk", "takeEditorGlobalReload", "edited && !editorEcho")
ordered(config_cpp, "SettingsEditorOpening CaptureWindowSettingsEditor", "activeWindow() == host",
        "host->maybeSession() == live", "!host->locked()", "!PrivacyScreenActiveAny()", "!live->account().loggingOut()")
ordered(config_cpp, "void RequestSettingsEditorApply", "_opening.windowCurrent", "review->commit",
        "RequestWait({", "WaitAction::ChangePeekOption", "apply(current, true)", ".onResult = finish")
ordered(config_cpp, "PurpleNoticeWindow(session)", "hasConfigPeerIds", "ShowPurpleNotice",
        "config:reload:")
archive_cpp = read("purple/purple_archive_service.cpp")
ordered(archive_cpp, "void ShowArchiveRemovalPending()", ".appScoped = true",
        "kArchiveRemovalNoticeKey")
ordered(archive_cpp, "void ShowArchiveDiscardFailure", "event=discard_failed reason=%1 count=1",
        ".arg(reason)", "ShowPurpleNotice", "archive:discard_failed:")
ordered(archive_cpp, "void ReportArchiveCaptureLoss", "AccumulateArchiveCaptureIncidents",
        "event=%1 reason=%2 count=%3", "if (warn)", "ShowPurpleNotice", "archive:gap:")
ordered(archive_cpp, "void RemoveArchiveAtLogout", "ShowArchiveRemovalPending();",
        "Fn<void()> ArchiveDiscardAction", "ShowArchiveDiscardFailure")
topic_edit = read("boxes/peers/edit_forum_topic_box.cpp")
ordered(topic_edit, "const auto refreshBotTopic", "bot ? peer->forum() : nullptr",
        "liveForum->requestTopic(rootId)", "MTPmessages_EditForumTopic",
        "api->applyUpdates(result)", "refreshBotTopic();", "TOPIC_NOT_MODIFIED",
        "if (unchanged)", "refreshBotTopic();")
ordered(keys, "using TopicFlag = Data::TopicUpdate::Flag",
        "topicUpdates(TopicFlag::Title | TopicFlag::IconId)",
        "update.topic->peer()->id == peerId", "CreateTopicPickerLayer")
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

available = between(keys, "bool Available", "bool RequiresModalInput")
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
ordered(read("purple/purple_hint_paint.h"), "0xff, 0xd8, 0x4a", "0xb9, 0x1c, 0x1c")
ordered(hints, "bool MessageHints::eligible", "hasCopyRestriction", "goodForSelection")

menu = read("window/window_main_menu.cpp")
ordered(menu, "MainMenu::MainMenu(", "setupMenu();",
        "Purple::InstallMainMenuKeys(", "_toggleAccounts.data(), _accounts)")

assert "MusicAllowsModalKeys(owner, focus)" in keys
composer = between(keys, "static QTextEdit *Composer", "static Fn<void()> From")
for widget in ("HistoryWidget", "HistoryView::ChatWidget", "HistoryView::ScheduledWidget"):
    assert widget in composer
ordered(keys, "QEvent::MouseButtonPress", "KeyboardAttachment::Composer(widget)",
        "FindTarget(owner(), edit).focus", "enterInsert(true)")
ordered(keys, "void Keys::setNormalComposer", "setCursorWidth(_composerCursorWidth)", "setCursorWidth(0)")
ordered(keys, "void Keys::refreshStatusBar", "activeTarget()", "chromeVisible() && _normal && target.focus")
music = read("purple/purple_music_section.cpp")
ordered(music, "void MusicSection::doSetInnerFocus", "ModalKeysInNormal", "setFocus()", "_search->setFocusFast()")
ordered(keys, "&QApplication::focusChanged", "MusicSearchContains(current, now)", "enterInsert(true)")
ordered(keys, "void Keys::enterNormal", "_normal = true", "FocusMusicSearch(current, windowFocusWidget(), false)")
dispatch = between(keys, "void Keys::dispatch", "void Keys::showHelpLayer")
ordered(dispatch, "case Action::EnterInsertMode:", "enterInsert(false)", "FocusMusicSearch(current, target.focus, true)")
ordered(dispatch, "case Action::Search:", "MapKind::Music", "enterInsert(true)",
        "FocusMusicSearch(current, target.focus, true)", "launch(Native::Search)")
assert "MusicHidesChats(_mainSection.data())" in read("mainwidget.cpp")
notice_probe = read("test/test_purple_notices.cpp")
assert re.search(r"option\(PURPLE_BUILD_NOTICE_TEST_PROBES[^)]*OFF\)", purple_cmake)
assert "$<$<NOT:$<CONFIG:Release>>:PURPLE_BUILD_NOTICE_TEST_PROBES>" in purple_cmake
ordered(keys, "bool Keys::eventFilter", "Test::TryPurpleNoticeProbe(_window, watched, event)")
ordered(notice_probe, "PURPLE_TEST_NOTICE_PROBES", "QApplication::activeWindow() != window",
        "key->isAutoRepeat()", "controller.maybeSession()", "QTimer::singleShot(6000",
        "live.maybeSession() == session", "SubmitProbe(live, session, dialog, true)")
ordered(notice_probe, "void SubmitProbe", "ShowPurpleNotice", "ShowPurpleAck")
ordered(notices_cpp, "class PendingNoticeDelivery", "QEvent::FocusIn", "schedule()", "void flush()")
ordered(notices_cpp, "void SubmitNotice", "Store().submit", "CanPresentNotice(window, accountKey)")
ordered(keys, "Core::App().passcodeLockValue()", "_held.clear()", "if (!locked)", "schedulePendingNoticeFlush()")
privacy_input = read("purple/purple_privacy_screen.cpp")
ordered(privacy_input, "bool filterWindowEvent", "QEvent::ShortcutOverride", "QEvent::KeyRelease",
        "Swallow every keystroke", "return true")
assert all(word not in notice_probe for word in ("writePref", "WriteSettings", "sync(", "api()."))
assert "Purple::IsChatPaletteController(_controller)" in read("boxes/peer_list_box.cpp")

print("Modal key, status line, which-key, cheatsheet, M-x, localleader, hints and background routes passed")
