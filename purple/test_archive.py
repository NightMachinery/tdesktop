#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import re
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--output-dir', type=Path)
parser.add_argument('--use-build-objects', action='store_true')
parser.add_argument('--capture-only', action='store_true')
parser.add_argument('--reuse-from', type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
build = args.build_dir.resolve()
owned = tempfile.TemporaryDirectory(prefix='purple-archive-test.') if not args.output_dir else None
work = args.output_dir.resolve() if args.output_dir else Path(owned.name)
work.mkdir(parents=True, exist_ok=True)
service = (repo / 'Telegram/SourceFiles/purple/purple_archive_service.cpp').read_text()
start = service.index('if (!weak || Retired.contains(weak.get()))')
end = start + re.search(r'\n\s*\}\);', service[start:]).start()
(work / 'archive-page-callback.inc').write_text(service[start:end] + '\n')
base = 'Telegram/CMakeFiles/Telegram.dir/'
recipe_source = repo / 'Telegram/SourceFiles/purple/purple_config_file.cpp'
recipes = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', base + 'SourceFiles/purple/purple_config_file.cpp.o'], text=True)
recipe = next(shlex.split(line) for line in recipes.splitlines() if ' -c ' in line and shlex.split(line)[shlex.split(line).index('-c') + 1] == str(recipe_source))
objects = []
commands = []
with (work / 'build.log').open('w') as log:
    def compile(source, name):
        target = work / (name + '.o')
        if args.reuse_from:
            previous = args.reuse_from / (name + '.o')
            deps = previous.with_suffix('.d')
            if previous.exists() and deps.exists():
                dependencies = shlex.split(deps.read_text().replace('\\\n', ' '))[1:]
                if dependencies and all(Path(x).exists() and Path(x).stat().st_mtime_ns <= previous.stat().st_mtime_ns for x in dependencies):
                    return previous
        command = recipe[:]
        for flag, value in [('-c', str(source)), ('-o', str(target)), ('-MF', str(target.with_suffix('.d'))), ('-MT', str(target))]:
            command[command.index(flag) + 1] = value
        command += ['-I', str(work), '-I', str(repo / 'Telegram/ThirdParty/purple_core'), '-I', str(repo / 'Telegram/ThirdParty/purple_sqlite')]
        commands.append(command)
        subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
        return target
    objects += [compile(repo / 'purple/test_archive.cpp', 'test')]
    for name in ['purple_desktop_archive_store', 'purple_archive_sqlite', 'purple_archive_crypto', 'purple_archive_files', 'purple_archive_media', 'purple_archive_journal', 'purple_archive_capture_snapshot', 'purple_archive_attention']:
        objects.append(build / (base + 'SourceFiles/purple/' + name + '.cpp.o') if args.use_build_objects else compile(repo / ('Telegram/SourceFiles/purple/' + name + '.cpp'), name))
    objects += [build / (base + 'SourceFiles/purple/purple_archive_backup_mac.mm.o')]
    objects += [build / (base + 'ThirdParty/purple_core/purple/' + source.name + '.o') for source in sorted((repo / 'Telegram/ThirdParty/purple_core/purple').glob('*.cpp'))]
    objects += [build / (base + 'SourceFiles/data/data_peer_id.cpp.o'), build / (base + 'SourceFiles/data/data_types.cpp.o'), build / 'Telegram/CMakeFiles/td_scheme.dir/gen/scheme.cpp.o']
    objects += [build / 'Telegram/lib_tl/CMakeFiles/lib_tl.dir/tl/tl_basic_types.cpp.o']
    objects += [build / 'Telegram/libpurple_sqlite.a', build / 'Telegram/lib_base/liblib_base.a']
    link = [recipe[0]]
    for flag in ['-arch', '-isysroot']:
        if flag in recipe:
            link += [flag, recipe[recipe.index(flag) + 1]]
    for value in recipe:
        if value.startswith('-F'):
            link += [value, '-Wl,-rpath,' + value[2:]]
    link += ['-Wl,-dead_strip', *map(str, objects), '-L/opt/homebrew/opt/openssl@3/lib', '-lcrypto', '-framework', 'QtCore', '-framework', 'QtGui', '-framework', 'QtWidgets', '-framework', 'AppKit', '-o', str(work / 'test_archive')]
    subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
(work / 'inputs.json').write_text(json.dumps({'compile': commands, 'link': link, 'page_callback_boundary': 'unchanged production delivery lambda body with real weak lifetime and controlled retirement, ticket and UI endpoints'}, indent=2) + '\n')
with (work / 'run.log').open('w') as log:
    result = subprocess.run([str(work / 'test_archive')] + (['capture-only'] if args.capture_only else []), stdout=log, stderr=subprocess.STDOUT)
(work / 'run.exit').write_text(str(result.returncode) + '\n')
print((work / 'run.log').read_text())
raise SystemExit(result.returncode)
