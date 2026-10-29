#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', required=True, type=Path)
parser.add_argument('--output-dir', required=True, type=Path)
parser.add_argument('--compile-wrapper', default='')
parser.add_argument('--go', default=shutil.which('go'))
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
source = repo / 'Telegram/SourceFiles'
build = args.build_dir.resolve()
work = args.output_dir.resolve()
work.mkdir(parents=True, exist_ok=True)
records = []


def run(argv, compile=False, **kwargs):
    command = list(map(str, argv))
    prefix = shlex.split(args.compile_wrapper) if compile else []
    result = subprocess.run(prefix + command, **kwargs)
    records.append({'command': command, 'wrapper': prefix, 'exit': result.returncode})
    (work / 'commands.json').write_text(json.dumps(records, indent=2) + '\n')
    result.check_returncode()
    return result


def definition(text, marker):
    start = text.index(marker)
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


target = 'Telegram/CMakeFiles/Telegram.dir/SourceFiles/purple/purple_config_file.cpp.o'
lines = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', target], text=True).splitlines()
recipe = next(shlex.split(line) for line in lines if ' -c ' in line
    and shlex.split(line)[shlex.split(line).index('-c') + 1].endswith('/purple_config_file.cpp'))
flags, i = [], 1
while i < len(recipe):
    flag = recipe[i]
    if flag == '-Xclang' and recipe[i + 1] in ('-include-pch', '-include'):
        i += 4
    elif flag in ('-c', '-o', '-MF', '-MT', '-include', '-include-pch'):
        i += 2
    elif flag in ('-MD', '-MMD') or flag.startswith(('-g', '-DLANG_KEYS_SUBSET=')):
        i += 1
    else:
        flags.append(flag)
        i += 1
(work / 'gen').mkdir(exist_ok=True)
(work / 'lang.strings').write_bytes((repo / 'Telegram/Resources/langs/lang.strings').read_bytes()
    + b'\n' + (repo / 'Telegram/Resources/langs/purple.strings').read_bytes())
run([build / 'Telegram/codegen/codegen/lang/codegen_lang', '-o' + str(work / 'gen'), work / 'lang.strings'])
(work / 'stdafx.hxx').write_text('#include "' + str(source / 'stdafx.h') + '"\n')
flags = ['-I' + str(work), '-I' + str(work / 'gen'), '-I' + str(source), *flags,
    '-g0', '-include', str(work / 'stdafx.hxx')]
header = (source / 'core/core_settings.h').read_text()
settings = (source / 'purple/purple_control.cpp').read_text()
(work / 'control-preference-api.inc').write_text('\n'.join(definition(header, marker) for marker in [
    '\ttemplate <typename Type, typename Other>\n\tvoid writePref(',
    '\ttemplate <typename Type, typename Other = Type>\n\t[[nodiscard]] Type readPref(']))
(work / 'control-preference-storage.inc').write_text('\n'.join(definition(settings, marker) for marker in [
    'template <>\nstd::optional<QByteArray> Settings::readPrefImpl<QByteArray>(',
    'template <>\nvoid Settings::writePrefImpl<QByteArray>(']))


def compile(relative):
    path = source / relative
    obj = work / (path.stem + '.o')
    run([recipe[0], *flags, '-c', path, '-o', obj, '-MD', '-MF', obj.with_suffix('.d')], compile=True)
    records[-1].update(sourceSha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        objectSha256=hashlib.sha256(obj.read_bytes()).hexdigest())
    (work / 'commands.json').write_text(json.dumps(records, indent=2) + '\n')
    return obj


base = ['purple_control_protocol', 'purple_control_commands', 'purple_control_endpoint', 'purple_control_server']
objects = [compile('purple/' + name + '.cpp') for name in base]
for name in ['purple_control', 'purple_control_settings', 'purple_app_services', 'purple_settings_section',
        'purple_emoji_export_store', 'purple_emoji_export']:
    compile('purple/' + name + '.cpp')
compile('core/sandbox.cpp')
symbols = run(['nm', '-gU', work / 'purple_control.o'], stdout=subprocess.PIPE, text=True).stdout
definitions = run(['c++filt'], input=symbols, stdout=subprocess.PIPE, text=True).stdout
for member in ['readPrefImpl', 'writePrefImpl']:
    assert any(' T ' in line and 'Core::Settings::' + member + '<QByteArray>' in line
        for line in definitions.splitlines()), 'Missing byte-array preference definition: ' + member
prefix = [recipe[0]]
for flag in ('-arch', '-isysroot'):
    if flag in recipe:
        prefix += [flag, recipe[recipe.index(flag) + 1]]
for flag in recipe:
    if flag.startswith('-F'):
        prefix += [flag, '-Wl,-rpath,' + flag[2:]]
frontend = (source / 'purple/purple_emoji_export.cpp').read_text()
(work / 'control-emoji-frontend.inc').write_text('\n'.join(definition(frontend, marker) for marker in [
    'void EmojiExporter::render(', 'bool EmojiExporter::serveRender(', 'void EmojiExporter::passcodeChanged(']))
(work / 'control-emoji-completion.inc').write_text(definition(
    definition(frontend, 'void EmojiExporter::checkMedia('), '[=](quint64 generation, bool ok)'))
emoji_objects = [compile('purple/' + name + '.cpp') for name in [
    'purple_emoji_render', 'purple_emoji_large', 'purple_emoji_large_store', 'purple_emoji_export_queue',
    'purple_emoji_export_store', 'purple_emoji_export_worker']]
for fixture in ['control_cases.cpp', 'control_preferences_cases.cpp', 'control_emoji_cases.cpp']:
    obj = compile('purple/tests/' + fixture)
    binary = work / Path(fixture).stem
    name = Path(fixture).stem
    run([*prefix, obj, *(objects if name != 'control_preferences_cases' else []),
        *(emoji_objects if name == 'control_emoji_cases' else []),
        '-framework', 'QtCore', '-framework', 'QtNetwork',
        '-framework', 'QtGui', '-o', binary], compile=True)
    run([binary], timeout=90)

sandbox = (source / 'core/sandbox.cpp').read_text()
assert 'CTRL:' not in sandbox and 'external_control' not in sandbox
assert all(marker in sandbox for marker in ['CMD:', 'OPEN:', 'XDG_ACTIVATION_TOKEN:'])
assert not (source / 'core/external_control.cpp').exists() and not (source / 'core/external_control.h').exists()
assert 'core/external_control' not in (repo / 'Telegram/CMakeLists.txt').read_text()
assert args.go, 'Local Go toolchain is required; do not install one from the test driver.'
environment = dict(os.environ, GOCACHE=str(work / 'go-cache'), CGO_ENABLED='0',
    GOTOOLCHAIN='local', GOPROXY='off', GOSUMDB='off', GOWORK='off', GOENV='off')
assert (repo / 'purple/cli/tdesktop-cli/main_test.go').is_file()
with tempfile.TemporaryDirectory(prefix='purple-control-', dir='/private/tmp' if os.sys.platform == 'darwin' else None) as temporary:
    environment['TMPDIR'] = temporary
    run([args.go, 'version'], env=environment)
    run([args.go, 'test', '-p', '1', '-count=1', '-v',
        '-toolexec=python3 ' + shlex.quote(str(repo / 'purple/control_go_tool.py')), './...'],
        cwd=repo / 'purple/cli/tdesktop-cli', env=environment, timeout=600)
    run([args.go, 'build', '-p', '1', '-toolexec=python3 '
        + shlex.quote(str(repo / 'purple/control_go_tool.py')), '-o', work / 'tdesktop-cli', '.'],
        cwd=repo / 'purple/cli/tdesktop-cli', env=environment, timeout=600)
    run([work / 'tdesktop-cli', '--help'], env=environment)
print('IPC native, emoji async/publication, preference link contract, CTRL removal and Go fake-server tests PASS')
