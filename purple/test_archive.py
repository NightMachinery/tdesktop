#!/usr/bin/env python3
from sync_store_units import add_current_core_units
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--output-dir', type=Path)
parser.add_argument('--use-build-objects', action='store_true')
parser.add_argument('--music-index', action='store_true')
parser.add_argument('--build-only', action='store_true')
parser.add_argument('--reuse-from', type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
build = args.build_dir.resolve()
owned = tempfile.TemporaryDirectory(prefix='purple-archive-test.') if not args.output_dir else None
work = args.output_dir.resolve() if args.output_dir else Path(owned.name)
work.mkdir(parents=True, exist_ok=True)
base = 'Telegram/CMakeFiles/Telegram.dir/'
recipe_source = repo / 'Telegram/SourceFiles/purple/purple_config_file.cpp'
recipes = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', base + 'SourceFiles/purple/purple_config_file.cpp.o'], text=True)
recipe = next(shlex.split(line) for line in recipes.splitlines() if ' -c ' in line and shlex.split(line)[shlex.split(line).index('-c') + 1] == str(recipe_source))
objects = []
commands = []
with (work / 'build.log').open('w') as log:
    def compile(source, name):
        target = work / (name + '.o')
        if args.reuse_from and name != 'test':
            previous = args.reuse_from / (name + '.o')
            deps = previous.with_suffix('.d')
            if previous.exists() and deps.exists():
                dependencies = shlex.split(deps.read_text().replace('\\\n', ' '))[1:]
                def unchanged(name):
                    dependency = Path(name)
                    if not dependency.exists() or dependency.stat().st_mtime_ns > previous.stat().st_mtime_ns:
                        return False
                    if dependency.is_relative_to(args.reuse_from.resolve()):
                        replacement = work / dependency.relative_to(args.reuse_from.resolve())
                        return replacement.exists() and replacement.read_bytes() == dependency.read_bytes()
                    return True
                if dependencies and all(unchanged(name) for name in dependencies):
                    return previous
        command = recipe[:]
        for flag, value in [('-c', str(source)), ('-o', str(target)), ('-MF', str(target.with_suffix('.d'))), ('-MT', str(target))]:
            command[command.index(flag) + 1] = value
        command += ['-I', str(work), '-I', str(repo / 'Telegram/ThirdParty/purple_core'), '-I', str(repo / 'Telegram/ThirdParty/purple_sqlite')]
        commands.append(command)
        subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
        return target
    fixture = 'purple/test_music_index.cpp' if args.music_index else 'purple/test_archive.cpp'
    objects += [compile(repo / fixture, 'test')]
    units = ['purple_archive_sqlite']
    if not args.music_index:
        units += ['purple_archive_previous', 'purple_archive_crypto', 'purple_archive_files']
        units += ['purple_archive_v2_' + unit for unit in ('schema', 'storage', 'store', 'writer', 'acquisition', 'quota', 'views')]
    for name in units:
        target = build / (base + 'SourceFiles/purple/' + name + '.cpp.o')
        source = repo / ('Telegram/SourceFiles/purple/' + name + '.cpp')
        objects.append(target if args.use_build_objects else compile(source, name))
    if not args.music_index:
        objects += [build / (base + 'SourceFiles/purple/purple_archive_backup_mac.mm.o')]
    objects += sorted((build / (base + 'ThirdParty/purple_core/purple')).glob('*.cpp.o'))
    objects += [build / (base + 'SourceFiles/data/data_peer_id.cpp.o'), build / (base + 'SourceFiles/data/data_types.cpp.o'), build / 'Telegram/CMakeFiles/td_scheme.dir/gen/scheme.cpp.o']
    objects += [build / 'Telegram/lib_tl/CMakeFiles/lib_tl.dir/tl/tl_basic_types.cpp.o']
    objects += [build / 'Telegram/libpurple_sqlite.a', build / 'Telegram/lib_base/liblib_base.a']
    add_current_core_units(repo, build, work, objects, commands, log)
    link = [recipe[0]]
    for flag in ['-arch', '-isysroot']:
        if flag in recipe:
            link += [flag, recipe[recipe.index(flag) + 1]]
    for value in recipe:
        if value.startswith('-F'):
            link += [value, '-Wl,-rpath,' + value[2:]]
    link += ['-Wl,-dead_strip', *map(str, objects), '-L/opt/homebrew/opt/openssl@3/lib', '-lcrypto', '-framework', 'QtCore', '-framework', 'QtGui', '-framework', 'QtWidgets', '-framework', 'AppKit', '-o', str(work / 'test_archive')]
    subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
(work / 'inputs.json').write_text(json.dumps({
    'compile': commands, 'link': link,
    'objects_sha256': {str(path): hashlib.sha256(Path(path).read_bytes()).hexdigest() for path in objects},
}, indent=2) + '\n')
if args.build_only:
    raise SystemExit(0)
with (work / 'run.log').open('w') as log:
    result = subprocess.run([str(work / 'test_archive')], stdout=log, stderr=subprocess.STDOUT)
(work / 'run.exit').write_text(str(result.returncode) + '\n')
output = (work / 'run.log').read_text()
print(output)
raise SystemExit(result.returncode)
