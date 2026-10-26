#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', required=True, type=Path)
parser.add_argument('--output-dir', required=True, type=Path)
parser.add_argument('--build-only', action='store_true')
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
build = args.build_dir.resolve()
work = args.output_dir.resolve()
work.mkdir(parents=True, exist_ok=True)
header = build / 'Telegram/gen/scheme.h'
schema = header.read_text()
constructors = []
for name, factory in [('Message', 'message'), ('MessageService', 'messageService'),
                      ('User', 'user'), ('UserFull', 'userFull'), ('Chat', 'chat'),
                      ('ChatFull', 'chatFull'), ('Channel', 'channel'),
                      ('ChannelFull', 'channelFull'), ('MessageMediaDocument', 'messageMediaDocument')]:
    match = re.search(r'^([A-Za-z0-9_]+) MTP_' + factory + r'\((.*)\);$', schema, re.MULTILINE)
    if not match:
        raise RuntimeError('unsupported native factory: ' + factory)
    fields = []
    for argument in match[2].split(', '):
        kind, field = argument.rsplit(' ', 1)
        kind = kind.removeprefix('const ').removesuffix(' &')
        field = field.removeprefix('&').removesuffix('_')
        fields.append((kind, field))
    constructors.append('struct Native' + name + ' {\n'
        + ''.join('\t' + kind + ' ' + field + ' = {};\n' for kind, field in fields)
        + '\t' + match[1] + ' Build() const { return MTP_' + factory + '('
        + ', '.join(field for _, field in fields) + '); }\n};\n')
(work / 'sync-chat-native-cases.inc').write_text('\n'.join(constructors))
commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands',
    'Telegram/CMakeFiles/Telegram.dir/SourceFiles/purple/purple_config_file.cpp.o'], text=True)
recipe = next(shlex.split(line) for line in commands.splitlines() if ' -c ' in line
              and shlex.split(line)[shlex.split(line).index('-c') + 1].endswith('/purple_config_file.cpp'))
objects = []
compiled = []
with (work / 'build.log').open('w') as log:
    for source in [repo / 'purple/test_sync_chat_facts.cpp',
                   repo / 'Telegram/SourceFiles/purple/purple_sync_chat_facts.cpp',
                   repo / 'Telegram/ThirdParty/purple_core/purple/purple_sync_chat.cpp',
                   repo / 'Telegram/ThirdParty/purple_core/purple/purple_peer_token.cpp']:
        target = work / (source.stem + '.o')
        command = recipe[:]
        for flag, value in [('-c', str(source)), ('-o', str(target)),
                            ('-MF', str(target.with_suffix('.d'))), ('-MT', str(target))]:
            command[command.index(flag) + 1] = value
        command[1:1] = ['-I' + str(repo / 'Telegram/SourceFiles'),
                         '-I' + str(repo / 'Telegram/ThirdParty/purple_core'), '-I' + str(work)]
        compiled.append(command)
        subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
        objects.append(target)
    objects += [build / 'Telegram/CMakeFiles/td_scheme.dir/gen/scheme.cpp.o',
                build / 'Telegram/lib_tl/CMakeFiles/lib_tl.dir/tl/tl_basic_types.cpp.o',
                build / 'Telegram/lib_base/liblib_base.a']
    link = [recipe[0]]
    for flag in ['-arch', '-isysroot']:
        if flag in recipe:
            link += [flag, recipe[recipe.index(flag) + 1]]
    for value in recipe:
        if value.startswith('-F'):
            link += [value, '-Wl,-rpath,' + value[2:]]
    link += ['-Wl,-dead_strip', *map(str, objects), '-framework', 'QtCore',
             '-framework', 'QtGui', '-framework', 'QtWidgets', '-framework', 'AppKit',
             '-o', str(work / 'test_sync_chat_facts')]
    subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
(work / 'inputs.json').write_text(json.dumps({'compile': compiled, 'link': link,
    'schema_sha256': hashlib.sha256(header.read_bytes()).hexdigest(),
    'sources_sha256': {str(source): hashlib.sha256(Path(source).read_bytes()).hexdigest()
                       for source in [c[c.index('-c') + 1] for c in compiled]},
    'objects_sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in objects},
    'boundary': 'Actual mapper and shared C0 compiled unchanged; real generated TL constructors and reused schema/base objects. Unit-only, no app build/network/native acceptance.'}, indent=2) + '\n')
if not args.build_only:
    result = subprocess.run([str(work / 'test_sync_chat_facts')], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (work / 'run.log').write_bytes(result.stdout)
    (work / 'run.exit').write_text(str(result.returncode) + '\n')
    print(result.stdout.decode())
    raise SystemExit(result.returncode)
