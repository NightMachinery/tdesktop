#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--output-dir', type=Path, required=True)
parser.add_argument('--build-only', action='store_true')
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
build = args.build_dir.resolve()
work = args.output_dir.resolve()
work.mkdir(parents=True, exist_ok=True)
header = (repo / 'Telegram/SourceFiles/purple/purple_picker.h').read_text()
source = (repo / 'Telegram/SourceFiles/purple/purple_picker.cpp').read_text()
hooks = {
    'boxes/share_box.cpp': {'Purple::PickerVisible(history)': 3, 'Purple::PickerVisible(selfHistory)': 1,
        'SharePickerBridge<Inner>::Watch(this)': 1, 'SharePickerBridge<Inner>::Filter(this, id)': 1,
        'Purple::WatchPickerChild(box.data(),': 3},
    'boxes/peer_list_controllers.cpp': {'RecipientPickerBridge::Rebuild(this)': 1,
        'RecipientPickerBridge::Allows(this, history)': 1, 'RecipientPickerBridge::SearchAllows(delegate(), peer)': 1,
        'RecipientPickerBridge::Watch(this)': 1, 'Purple::PickerVisible(history)': 1,
        'Purple::WatchPickerChild(owned.data(), peer)': 3},
    'window/window_peer_menu.cpp': {'Purple::PickerVisible(row->peer())': 2,
        'RecipientPickerBridge::Filter(box, id)': 1, 'RecipientPickerBridge::SelectionChanges(': 2},
    'boxes/peer_list_box.cpp': {'RecipientPickerBridge::BeforeRemove(this)': 1},
    'boxes/peers/choose_peer_box.cpp': {'RecipientPickerBridge::Watch(this)': 1, 'Purple::PickerVisible(history)': 1,
        'Purple::PickerDeselected(_selected, _selectedCount, peer)': 1},
    'boxes/peers/add_bot_to_chat_box.cpp': {'RecipientPickerBridge::Watch(this)': 1, 'Purple::PickerVisible(history)': 1},
}
for name, calls in hooks.items():
    text = (repo / 'Telegram/SourceFiles' / name).read_text()
    for call, expected in calls.items():
        if text.count(call) != expected:
            raise RuntimeError(f'Picker hook changed: {name}: {call}')


def function(signature):
    begin = source.index(signature)
    position = source.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (source[position] == '{') - (source[position] == '}')
        position += 1
    return source[begin:position] + '\n'


(work / 'picker-projection.inc').write_text(header[header.index('template <typename T, typename Visible>'):header.index('class RecipientPickerBridge')])
(work / 'picker-share.inc').write_text(header[header.index('template <typename Inner>'):header.rindex('\n}')])
(work / 'picker-visible.inc').write_text(function('bool PickerVisible(not_null<History*>'))
(work / 'picker-recipient.inc').write_text(''.join(function(signature) for signature in (
    'bool RecipientPickerBridge::Allows(', 'bool RecipientPickerBridge::SearchAllows(',
    'void RecipientPickerBridge::Filter(', 'void RecipientPickerBridge::BeforeRemove(',
    'rpl::producer<> RecipientPickerBridge::SelectionChanges(',
    'void WatchPickerChild(', 'bool RecipientPickerBridge::Rebuild(')))
target = 'Telegram/CMakeFiles/Telegram.dir/SourceFiles/purple/purple_config_file.cpp.o'
recipes = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', target], text=True)
recipe = next(shlex.split(line) for line in recipes.splitlines() if ' -c ' in line and 'purple_config_file.cpp' in line)
flags = ['-std=c++20', '-I', str(work), '-I', str(repo / 'Telegram/ThirdParty/purple_core')]
for flag in ('-arch', '-isysroot', '-isystem'):
    for index, value in enumerate(recipe):
        if value == flag:
            flags += [flag, recipe[index + 1]]
for value in recipe:
    if value.startswith(('-I', '-D', '-F', '-mmacosx-version-min=')):
        flags.append(value)
objects = [str(path) for path in sorted((build / 'Telegram/CMakeFiles/Telegram.dir/ThirdParty/purple_core/purple').glob('*.cpp.o'))]
frameworks = next(Path(value[2:]) for value in flags if value.startswith('-F') and (Path(value[2:]) / 'QtCore.framework').exists())
with (work / 'build.log').open('w') as log:
    subprocess.run([recipe[0], *flags, '-c', str(repo / 'purple/test_picker.cpp'), '-o', str(work / 'test.o')], check=True, stdout=log, stderr=subprocess.STDOUT)
    subprocess.run([recipe[0], *flags, '-Wl,-dead_strip', str(work / 'test.o'), *objects,
        str(build / 'Telegram/lib_base/liblib_base.a'), '-framework', 'QtCore', '-framework', 'QtGui', '-framework', 'QtWidgets',
        '-framework', 'Foundation', '-lobjc', '-Wl,-rpath,' + str(frameworks), '-o', str(work / 'test_picker')], check=True, stdout=log, stderr=subprocess.STDOUT)
(work / 'inputs.json').write_text(json.dumps({
    'source_sha256': {str(path.relative_to(repo)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in (repo / 'Telegram/SourceFiles/purple/purple_picker.h', repo / 'Telegram/SourceFiles/purple/purple_picker.cpp', repo / 'purple/test_picker.cpp')},
    'boundary': 'Exact production visibility function, projection, recipient reconciliation and share bridge. Controlled synthetic History, session/index/delegate and eligibility providers; real core resolution, RPL and Qt queued refresh.'}, indent=2) + '\n')
if args.build_only:
    raise SystemExit(0)
with (work / 'run.log').open('w') as log:
    result = subprocess.run([str(work / 'test_picker')], env=dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_PLUGIN_PATH=str(frameworks.parent / 'plugins')), stdout=log, stderr=subprocess.STDOUT)
(work / 'run.exit').write_text(str(result.returncode) + '\n')
print((work / 'run.log').read_text())
raise SystemExit(result.returncode)
