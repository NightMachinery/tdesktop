#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--author-unit-dir', type=Path, required=True)
parser.add_argument('--output-dir', type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
work = args.output_dir.resolve()
work.mkdir(parents=True, exist_ok=True)
author = args.author_unit_dir.resolve()
inputs = json.loads((author / 'inputs.json').read_text())
for name in ['sources_sha256', 'headers_sha256', 'production_ports_sha256']:
    for path, sha in inputs[name].items():
        assert hashlib.sha256(Path(path).read_bytes()).hexdigest() == sha, path

def block(text, marker):
    start = text.index('{', text.index(marker))
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

service = (repo / 'Telegram/SourceFiles/purple/purple_sync_chat_service.cpp').read_text()
scanner = (repo / 'Telegram/SourceFiles/purple/purple_sync_history_scanner.cpp').read_text()
(work / 'hint-callback.inc').write_text(block(service, 'const auto completed = [=](const MTPmessages_Messages &response)'))
(work / 'scanner-callback.inc').write_text(block(scanner, ').done([weak](const MTPmessages_Messages &result)'))
source = repo / 'purple/test_sync_chat_review_t1.cpp'
reviewer = source.read_text()
layout = 'SyncHistoryPages _pages;'
extra = layout + '\n DesktopSyncChatHistoryGuard _modernHistory{123};\n bool Current() const { return true; }'
assert reviewer.count(layout) == 1
adapted = reviewer.replace(layout, extra)
(work / 'reviewer-layout-adapted.cpp').write_text(adapted)
prefix = adapted[adapted.index('#undef main') + len('#undef main'):adapted.index('void ExcludedHint()')]
(work / 'sync-chat-completeness-probes.inc').write_text(prefix)
if not (work / 'tree').exists():
    (work / 'tree').symlink_to(repo, target_is_directory=True)
commands = []
base = inputs['compile'][0]
with (work / 'build.log').open('w') as log:
    for label, fixture in [('complete', repo / 'purple/test_sync_chat_completeness.cpp'), ('reviewer', work / 'reviewer-layout-adapted.cpp')]:
        target = work / (label + '.o')
        compile = base[:]
        compile[1:1] = ['-I' + str(work), '-I' + str(author)]
        for flag, value in [('-c', str(fixture)), ('-o', str(target)), ('-MF', str(work / (label + '.d'))), ('-MT', str(target))]:
            compile[compile.index(flag) + 1] = value
        subprocess.run(compile, stdout=log, stderr=subprocess.STDOUT, check=True)
        link = [str(target) if value == str(author / 'test_sync_chat_discovery.o') else value for value in inputs['link']]
        link[link.index('-o') + 1] = str(work / label)
        if label == 'complete':
            core = args.build_dir / 'Telegram/CMakeFiles/Telegram.dir/ThirdParty/purple_core/purple'
            link[1:1] = [str(core / (name + '.cpp.o')) for name in ['purple_config_payload', 'purple_settings', 'purple_state']]
        subprocess.run(link, stdout=log, stderr=subprocess.STDOUT, check=True)
        commands.append({'label': label, 'compile': compile, 'link': link})
(work / 'inputs.json').write_text(json.dumps({'commands': commands,
    'reviewer_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'author_inputs_sha256': hashlib.sha256((author / 'inputs.json').read_bytes()).hexdigest(),
    'sources_sha256': {str(repo / 'Telegram/SourceFiles/purple' / name): hashlib.sha256((repo / 'Telegram/SourceFiles/purple' / name).read_bytes()).hexdigest() for name in ['purple_sync_chat_service.cpp','purple_sync_history_scanner.cpp']},
    'linked_inputs_sha256': {value: hashlib.sha256(Path(value).read_bytes()).hexdigest() for item in commands for value in item['link'] if Path(value).is_file() and value != str(work / item['label'])},
    'driver_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    'fixture_sha256': hashlib.sha256((repo / 'purple/test_sync_chat_completeness.cpp').read_bytes()).hexdigest(),
    'boundary': 'Unchanged reviewer source and assertions. Derived translation unit adds production guard state and a current-authority endpoint to the transport stub; full callbacks extracted from current production. Reviewer expected two dropped-status failures, independently explained. Corrected complete fixture forwards real service reply mapping.'}, indent=2) + '\n')
print('compiled completeness and preserved-reviewer fixtures')
