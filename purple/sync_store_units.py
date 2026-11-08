import shlex
import subprocess
from pathlib import Path


def add_sync_store_units(repo, build, work, objects, compiled, log):
    add_current_units(repo, build, work, objects, compiled, log, ['purple_sync_local_store'], [])


def add_sync_policy_units(repo, build, work, objects, compiled, log):
    add_current_units(repo, build, work, objects, compiled, log, [], ['purple_sync_auto', 'purple_sync_auto_state'])


def add_current_units(repo, build, work, objects, compiled, log, client, core):
    def replaced(path):
        text = str(path)
        name = Path(path).name.removesuffix('.o').removesuffix('.cpp')
        if name.startswith('core-'):
            return name.removeprefix('core-') in core
        if name.startswith('storedeps-'):
            return name.removeprefix('storedeps-') in client + core
        return name in client or (name in core and '/ThirdParty/purple_core/' in text)
    objects[:] = [path for path in objects if not replaced(path)]
    for name in client + core:
        relative = ('ThirdParty/purple_core/purple/' if name in core else 'SourceFiles/purple/') + name + '.cpp'
        target = 'Telegram/CMakeFiles/Telegram.dir/' + relative + '.o'
        lines = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', target], text=True)
        recipe = next(shlex.split(line) for line in lines.splitlines() if ' -c ' in line
                      and shlex.split(line)[shlex.split(line).index('-c') + 1].endswith('/' + name + '.cpp'))
        source = repo / 'Telegram' / relative
        output = work / ('storedeps-' + name + '.o')
        for flag, value in [('-c', source), ('-o', output), ('-MF', output.with_suffix('.d')), ('-MT', output)]:
            recipe[recipe.index(flag) + 1] = str(value)
        recipe[1:1] = ['-I' + str(repo / 'Telegram/SourceFiles'), '-I' + str(repo / 'Telegram/ThirdParty/purple_core')]
        compiled.append(recipe)
        subprocess.run(recipe, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
        objects.append(output)


def write_upload_current_checks(work, upload, block):
    for method in ['RequestCurrent', 'Current']:
        (work / ('upload-' + method + '.inc')).write_text(
            block(upload, 'bool SyncConfigUpload::' + method + '('))
