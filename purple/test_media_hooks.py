#!/usr/bin/env python3
# Pins the media playback hooks: each upstream function named below must still
# contain its one Purple call, and each file its one facade include, so a merge
# that silently drops a hook fails here. See docs/purple/media_playback.md.
from pathlib import Path
import sys

FACADE = '#include "purple/hooks/media_playback.h"'
HOOKS = [
    ('history/history_inner_widget.cpp', 'void HistoryInner::mouseDoubleClickEvent(', 'Purple::RestartOnDoubleClick('),
    ('history/view/history_view_list_widget.cpp', 'void ListWidget::mouseDoubleClickEvent(', 'Purple::RestartOnDoubleClick('),
]


def body(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    sources = Path(__file__).resolve().parent.parent / 'Telegram/SourceFiles'
    failures = []
    checks = 0
    for relative in sorted({hook[0] for hook in HOOKS}):
        text = (sources / relative).read_text()
        lines = text.splitlines()
        own = '#include "' + relative[:-len('.cpp')] + '.h"'
        checks += 1
        if own not in lines or lines.index(own) + 1 >= len(lines) or lines[lines.index(own) + 1] != FACADE:
            failures.append(relative + ': the facade include must follow the file\'s own header')
        checks += 1
        if text.count(FACADE) != 1:
            failures.append(relative + ': expected exactly one facade include')
    for relative, signature, call in HOOKS:
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
