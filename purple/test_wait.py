#!/usr/bin/env python3
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--output-dir', type=Path)
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
objects = []
with (work / 'build.log').open('w') as log:
    for relative in ('purple/test_wait.cpp', 'Telegram/SourceFiles/purple/purple_wait_gate.cpp', 'Telegram/SourceFiles/purple/purple_wait_key_filter.cpp'):
        source = repo / relative
        target = work / (source.stem + '.o')
        subprocess.run([recipe[0], *flags, '-c', str(source), '-o', str(target)], check=True, stdout=log, stderr=subprocess.STDOUT)
        objects.append(str(target))
    objects += [str(path) for path in sorted((build / 'Telegram/CMakeFiles/Telegram.dir/ThirdParty/purple_core/purple').glob('*.cpp.o'))]
    subprocess.run([recipe[0], *flags, '-Wl,-dead_strip', *objects, '-framework', 'QtCore', '-framework', 'QtGui', '-framework', 'QtWidgets', '-Wl,-rpath,' + str(frameworks), '-o', str(work / 'test_wait')], check=True, stdout=log, stderr=subprocess.STDOUT)
env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_PLUGIN_PATH=str(frameworks.parent / 'plugins'))
with (work / 'run.log').open('w') as log:
    result = subprocess.run([str(work / 'test_wait')], env=env, stdout=log, stderr=subprocess.STDOUT)
(work / 'run.exit').write_text(str(result.returncode) + '\n')
print((work / 'run.log').read_text())
raise SystemExit(result.returncode)
