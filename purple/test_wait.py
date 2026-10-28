#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--output-dir', type=Path)
parser.add_argument('--build-only', action='store_true')
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
build = args.build_dir.resolve()
temporary = tempfile.TemporaryDirectory(prefix='purple-wait.') if not args.output_dir else None
work = args.output_dir.resolve() if args.output_dir else Path(temporary.name)
work.mkdir(parents=True, exist_ok=True)
target = 'Telegram/CMakeFiles/Telegram.dir/SourceFiles/purple/purple_config_file.cpp.o'
recipes = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', target], text=True)
recipe = next(shlex.split(line) for line in recipes.splitlines() if ' -c ' in line and 'purple_config_file.cpp' in line)
flags = ['-std=c++20', '-I', str(repo / 'Telegram/SourceFiles'), '-I', str(repo / 'Telegram/ThirdParty/purple_core')]
for option in ('-arch', '-isysroot'):
    if option in recipe:
        flags += [option, recipe[recipe.index(option) + 1]]
for option in recipe:
    if option.startswith(('-F', '-mmacosx-version-min=')):
        flags.append(option)
frameworks = next(Path(option[2:]) for option in flags if option.startswith('-F') and (Path(option[2:]) / 'QtCore.framework').exists())
gui_headers = frameworks / 'QtGui.framework/Headers'
private_versions = sorted(path for path in gui_headers.iterdir() if path.is_dir() and (path / 'QtGui/qpa/qwindowsysteminterface.h').exists())
if not private_versions:
    raise RuntimeError('Qt application-state test needs the matching installed QtGui private headers')
flags += ['-I', str(private_versions[-1])]
source = (repo / 'Telegram/SourceFiles/purple/purple_peek_controls.cpp').read_text()
read_start = source.index('bool PeekEndsOnFocusLoss()')
read_end = source.index('\nrpl::producer<bool> PeekFocusLossValue()', read_start)
def function(signature):
    begin = source.index(signature)
    position = source.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (source[position] == '{') - (source[position] == '}')
        position += 1
    return source[begin:position]
pref_key = next(line for line in source.splitlines() if line.startswith('constexpr auto kPeekFocusLossPref ='))
(work / 'peek-focus-preference.inc').write_text(pref_key + '\n' + source[read_start:read_end] + '\n' + '\n'.join(function(signature) for signature in (
    'void RequestPeekOptionChange(', 'void RequestPeekFocusLossChange(', 'void RequestPeekDelayChange(',
    'void RequestPeekReadyTimeoutChange(')))
config_source = (repo / 'Telegram/SourceFiles/purple/purple_config.cpp').read_text()
begin = config_source.index('WriteResult Config::write(')
end = config_source.index('std::optional<AccountSettingsWriteTarget> Config::checkedInput', begin)
(work / 'peek-config-write.inc').write_text(config_source[begin:end].replace('WriteResult Config::write(', 'WriteResult PortConfig::write(', 1))
flags += ['-I', str(work)]
inputs = ('purple/test_wait.cpp', 'purple/test_wait.py',
    'Telegram/SourceFiles/purple/purple_wait_gate.h', 'Telegram/SourceFiles/purple/purple_wait_gate.cpp',
    'Telegram/SourceFiles/purple/purple_wait_key_filter.h', 'Telegram/SourceFiles/purple/purple_wait_key_filter.cpp',
    'Telegram/SourceFiles/purple/purple_config.cpp', 'Telegram/SourceFiles/purple/purple_config.h',
    'Telegram/SourceFiles/purple/purple_peek_controls.h', 'Telegram/SourceFiles/purple/purple_peek_controls.cpp')
source_hashes = {path: hashlib.sha256((repo / path).read_bytes()).hexdigest() for path in inputs}
objects = []
with (work / 'build.log').open('w') as log:
    for relative in ('purple/test_wait.cpp', 'Telegram/SourceFiles/purple/purple_wait_gate.cpp', 'Telegram/SourceFiles/purple/purple_wait_key_filter.cpp'):
        source = repo / relative
        target = work / (source.stem + '.o')
        subprocess.run([recipe[0], *flags, '-c', str(source), '-o', str(target)], check=True, stdout=log, stderr=subprocess.STDOUT)
        objects.append(str(target))
    objects += [str(path) for path in sorted((build / 'Telegram/CMakeFiles/Telegram.dir/ThirdParty/purple_core/purple').glob('*.cpp.o'))]
    subprocess.run([recipe[0], *flags, '-Wl,-dead_strip', *objects, '-framework', 'QtCore', '-framework', 'QtGui', '-framework', 'QtWidgets', '-Wl,-rpath,' + str(frameworks), '-o', str(work / 'test_wait')], check=True, stdout=log, stderr=subprocess.STDOUT)
if any(hashlib.sha256((repo / path).read_bytes()).hexdigest() != digest for path, digest in source_hashes.items()):
    raise RuntimeError('Wait fixture source changed while compiling; retry from the stable snapshot')
(work / 'inputs.json').write_text(json.dumps({
    'source_sha256': source_hashes,
    'binary_sha256': hashlib.sha256((work / 'test_wait').read_bytes()).hexdigest(),
    'boundary': 'Exact production option request and wrappers, native WaitGate/PeekFocusGate/key filter and real Qt/RPL/core splices. Controlled account/editor/file and preference ports; no real settings files, visible GUI, live account or network.'}, indent=2) + '\n')
if args.build_only:
    raise SystemExit(0)
env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_PLUGIN_PATH=str(frameworks.parent / 'plugins'))
with (work / 'run.log').open('w') as log:
    result = subprocess.run([str(work / 'test_wait')], env=env, stdout=log, stderr=subprocess.STDOUT)
(work / 'run.exit').write_text(str(result.returncode) + '\n')
print((work / 'run.log').read_text())
raise SystemExit(result.returncode)
