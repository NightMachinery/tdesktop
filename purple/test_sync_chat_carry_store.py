#!/usr/bin/env python3
import argparse, hashlib, json, shlex, subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--output-dir', type=Path, required=True)
parser.add_argument('--build-only', action='store_true')
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
build = args.build_dir.resolve()
work = args.output_dir.resolve()
work.mkdir(parents=True, exist_ok=True)
base = build / 'Telegram/CMakeFiles/Telegram.dir'
commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands',
    'Telegram/CMakeFiles/Telegram.dir/SourceFiles/purple/purple_config_file.cpp.o'], text=True)
recipe = next(shlex.split(line) for line in commands.splitlines() if ' -c ' in line
    and shlex.split(line)[shlex.split(line).index('-c') + 1].endswith('/purple_config_file.cpp'))
objects, compiled, sources = [], [], []
cache = (build / 'CMakeCache.txt').read_text()
native_repo = Path(next(line.split('=', 1)[1] for line in cache.splitlines() if line.startswith('CMAKE_HOME_DIRECTORY:INTERNAL=')))
for header in (repo / 'Telegram/ThirdParty/purple_core/purple').glob('*.h'):
    assert header.read_bytes() == (native_repo / 'Telegram/ThirdParty/purple_core/purple' / header.name).read_bytes(), header
with (work / 'build.log').open('w') as log:
    for source in [repo / 'purple/test_sync_chat_carry_store.cpp', *[repo / ('Telegram/SourceFiles/purple/' + name + '.cpp')
            for name in ['purple_sync_local_store', 'purple_sync_chat_carry_store', 'purple_sync_chat_cache']]]:
        target = work / (source.stem + '.o')
        command = recipe[:]
        for flag, value in [('-c', str(source)), ('-o', str(target)), ('-MF', str(target.with_suffix('.d'))), ('-MT', str(target))]:
            command[command.index(flag) + 1] = value
        command[1:1] = ['-I' + str(repo / 'Telegram/SourceFiles'), '-I' + str(repo / 'Telegram/ThirdParty/purple_core')]
        compiled.append(command)
        subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
        objects.append(target)
    for source in sorted((repo / 'Telegram/ThirdParty/purple_core/purple').glob('*.cpp')):
        native = native_repo / 'Telegram/ThirdParty/purple_core/purple' / source.name
        assert source.read_bytes() == native.read_bytes(), source
        obj = base / 'ThirdParty/purple_core/purple' / (source.name + '.o')
        if obj.exists(): objects.append(obj); sources.append(source)
    objects += [base / 'SourceFiles/purple/purple_sync_chat_facts.cpp.o',
        base / 'SourceFiles/data/data_peer_id.cpp.o', base / 'SourceFiles/data/data_types.cpp.o',
        build / 'Telegram/CMakeFiles/td_scheme.dir/gen/scheme.cpp.o', build / 'Telegram/lib_base/liblib_base.a']
    link = [recipe[0]]
    for flag in ['-arch', '-isysroot']:
        if flag in recipe: link += [flag, recipe[recipe.index(flag) + 1]]
    for value in recipe:
        if value.startswith('-F'): link += [value, '-Wl,-rpath,' + value[2:]]
    link += ['-Wl,-dead_strip', *map(str, objects), '-framework', 'QtCore', '-framework', 'QtGui',
        '-framework', 'QtWidgets', '-framework', 'AppKit', '-o', str(work / 'test_sync_chat_carry_store')]
    subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
(work / 'inputs.json').write_text(json.dumps({'compile': compiled, 'link': link,
    'sources_sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources + [Path(c[c.index('-c') + 1]) for c in compiled]},
    'objects_sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in objects},
    'boundary': 'Production storage adapters and private synthetic files; unchanged shared core objects from existing Debug; no app build, network, GUI or physical power-loss proof.'}, indent=2) + '\n')
if not args.build_only:
    result = subprocess.run([str(work / 'test_sync_chat_carry_store')], capture_output=True)
    (work / 'run.log').write_bytes(result.stdout + result.stderr)
    (work / 'run.exit').write_text(str(result.returncode) + '\n')
    print(result.stdout.decode())
    raise SystemExit(result.returncode)
