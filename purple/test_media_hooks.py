#!/usr/bin/env python3
# Pins the media playback and recording hooks: each upstream function named
# below must still contain its one Purple call, and each file its one facade
# include, so a merge that silently drops a hook fails here. The seek shortcut
# lines are pinned verbatim. See docs/purple/media_playback.md and
# docs/purple/media_recording.md.
from pathlib import Path
import sys

FACADE = '#include "purple/hooks/media_playback.h"'
HOOKS = [
    ('history/history_inner_widget.cpp', 'void HistoryInner::mouseDoubleClickEvent(', 'Purple::RestartOnDoubleClick('),
    ('history/view/history_view_list_widget.cpp', 'void ListWidget::mouseDoubleClickEvent(', 'Purple::RestartOnDoubleClick('),
    ('history/view/media/history_view_gif.cpp', 'QSize Gif::countThumbSize(', 'Purple::VideoMessageThumbSize('),
    ('history/view/media/history_view_gif.cpp', 'int Gif::bubbleWidthLimit(', 'Purple::VideoMessageBubbleWidthLimit('),
    ('media/player/media_player_float.cpp', 'Float::Float(', 'Purple::FloatPlayerSize('),
    ('media/player/media_player_float.cpp', 'Float::Float(', 'Purple::DecorateFloat('),
    ('media/player/media_player_float.cpp', 'void Float::mouseDoubleClickEvent(', 'Purple::RestartFloatOnDoubleClick('),
    ('media/player/media_player_instance.cpp', 'bool Instance::moveInPlaylist(', 'Purple::ConfinedVoiceNext('),
]
RECORDING_FACADE = '#include "purple/hooks/media_recording.h"'
RECORDING_HOOKS = [
    ('media/player/media_player_widget.cpp', 'Widget::Widget(', 'Purple::DecoratePlayerSeek('),
    ('media/player/media_player_widget.cpp', 'void Widget::updateControlsGeometry(', 'Purple::PlacePlayerSeek('),
    ('media/player/media_player_widget.cpp', 'int Widget::getTimeRight(', 'Purple::PlayerSeekWidth('),
    ('media/system_media_controls_manager.cpp', 'SystemMediaControlsManager::SystemMediaControlsManager(', 'Purple::HandleSystemMediaCommand('),
    ('core/shortcuts.cpp', 'void Manager::fillDefaults(', 'set(Purple::SeekShortcutKeys(-1)  , Command::MediaSeekBackward);'),
    ('core/shortcuts.cpp', 'void Manager::fillDefaults(', 'set(Purple::SeekShortcutKeys(1)   , Command::MediaSeekForward);'),
    ('core/shortcuts.cpp', 'const auto AutoRepeatCommands = ', 'Command::MediaSeekBackward,'),
    ('core/shortcuts.cpp', 'const auto AutoRepeatCommands = ', 'Command::MediaSeekForward,'),
    ('core/shortcuts.cpp', 'const auto MediaCommands = ', 'Command::MediaSeekBackward,'),
    ('core/shortcuts.cpp', 'const auto MediaCommands = ', 'Command::MediaSeekForward,'),
]
CAPTURE_FACADE = '#include "purple/hooks/media_recording_capture.h"'
CAPTURE_HOOKS = [
    ('media/audio/media_audio_capture.cpp', 'static int WriteData(void *opaque, uint8_t *buf, int buf_size) {', 'Purple::MirrorRecordingWrite(l, l->dataPos, buf, buf_size);'),
    ('history/view/controls/history_view_voice_record_bar.cpp', 'void VoiceRecordBar::stopRecording(', 'Purple::NoteRecordingStop(&_show->session(), int(type), isListenState(), ttlBeforeHide);'),
    ('ui/controls/round_video_recorder.cpp', 'void RoundVideoRecorder::Private::initEncoding() {', 'Purple::OnRoundEncodingStarted(this, _format.get());'),
    ('ui/controls/round_video_recorder.cpp', 'bool RoundVideoRecorder::Private::writeFrame(', 'Purple::OnRoundPacket(this, pkt);'),
]
# Purple::NoteRecordingStop reads the StopType ordinal, so its order is pinned.
BLOCKS = [
    ('history/view/controls/history_view_voice_record_bar.h', 'enum class StopType {\n\t\tCancel,\n\t\tSend,\n\t\tListen,\n\t};'),
]
SHORTCUT_LINES = [
    ('core/shortcuts.h', 'MediaSeekBackward,'),
    ('core/shortcuts.h', 'MediaSeekForward,'),
    ('core/shortcuts.cpp', '{ u"media_seek_backward"_q           , Command::MediaSeekBackward },'),
    ('core/shortcuts.cpp', '{ u"media_seek_forward"_q            , Command::MediaSeekForward },'),
    ('settings/sections/settings_shortcuts.cpp', '{ C::MediaSeekBackward, tr::lng_purple_shortcuts_seek_backward() },'),
    ('settings/sections/settings_shortcuts.cpp', '{ C::MediaSeekForward, tr::lng_purple_shortcuts_seek_forward() },'),
]


def body(text, signature):
    start = text.index(signature)
    end = text.index('{\n', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    sources = Path(__file__).resolve().parent.parent / 'Telegram/SourceFiles'
    failures = []
    checks = 0
    for facade, hooks in ((FACADE, HOOKS), (RECORDING_FACADE, RECORDING_HOOKS), (CAPTURE_FACADE, CAPTURE_HOOKS)):
        for relative in sorted({hook[0] for hook in hooks}):
            text = (sources / relative).read_text()
            lines = text.splitlines()
            own = '#include "' + relative[:-len('.cpp')] + '.h"'
            checks += 1
            if own not in lines or lines.index(own) + 1 >= len(lines) or lines[lines.index(own) + 1] != facade:
                failures.append(relative + ': the facade include must follow the file\'s own header')
            checks += 1
            if text.count(facade) != 1:
                failures.append(relative + ': expected exactly one facade include')
    for relative, line in SHORTCUT_LINES:
        checks += 1
        if [row.strip() for row in (sources / relative).read_text().splitlines()].count(line) != 1:
            failures.append(relative + ': expected the line ' + line + ' exactly once')
    for relative, block in BLOCKS:
        checks += 1
        if (sources / relative).read_text().count(block) != 1:
            failures.append(relative + ': expected the block ' + block.split('\n')[0] + ' exactly once, in this order')
    for relative, signature, call in HOOKS + RECORDING_HOOKS + CAPTURE_HOOKS:
        text = (sources / relative).read_text()
        checks += 1
        if signature not in text:
            failures.append(relative + ': ' + signature + ' is gone')
            continue
        if body(text, signature).count(call) != 1:
            failures.append(relative + ': ' + signature + ' must contain ' + call + ' exactly once')
    for failure in failures:
        print('FAIL ' + failure)
    print(f'{checks - len(failures)}/{checks} media hook checks passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
