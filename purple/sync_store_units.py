import shlex
import subprocess
from pathlib import Path


def add_sync_store_units(repo, build, work, objects, compiled, log):
    add_current_units(repo, build, work, objects, compiled, log, ['purple_sync_local_store'], [])


def add_current_core_units(repo, build, work, objects, compiled, log):
    prefix = 'Telegram/CMakeFiles/Telegram.dir/ThirdParty/purple_core/purple/'
    targets = {
        str(Path(path).relative_to(build)): Path(path).name.removesuffix('.cpp.o')
        for path in objects
        if Path(path).is_relative_to(build)
        and str(Path(path).relative_to(build)).startswith(prefix)
    }
    known = set(line.split(': ', 1)[0] for line in subprocess.check_output(
        ['ninja', '-C', str(build), '-t', 'targets', 'all'], text=True).splitlines())
    present = {Path(path).name.removesuffix('.o').removesuffix('.cpp')
        .removeprefix('core-').removeprefix('storedeps-') for path in objects}
    current_sources = {path.stem for path in (repo / 'Telegram/ThirdParty/purple_core/purple').glob('*.cpp')}
    changed = {'purple_sync_auto', 'purple_sync_auto_state'} | (current_sources - present)
    changed.update(name for target, name in targets.items() if target not in known)
    targets = {target: name for target, name in targets.items() if target in known}
    if targets:
        deps = subprocess.check_output(
            ['ninja', '-C', str(build), '-t', 'deps', *targets], text=True)
        current = None
        seen = set()
        marker = '/Telegram/ThirdParty/purple_core/'
        for line in deps.splitlines():
            if line and not line.startswith(' '):
                current = targets.get(line.split(': ', 1)[0])
            elif current and marker in line:
                dependency = Path(line.strip())
                local = repo / 'Telegram/ThirdParty/purple_core' / str(dependency).split(marker, 1)[1]
                seen.add(current)
                if not dependency.is_file() or not local.is_file() or dependency.read_bytes() != local.read_bytes():
                    changed.add(current)
        changed.update(set(targets.values()) - seen)
    add_current_units(repo, build, work, objects, compiled, log, [], sorted(changed))


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
        known = set(line.split(': ', 1)[0] for line in subprocess.check_output(
            ['ninja', '-C', str(build), '-t', 'targets', 'all'], text=True).splitlines())
        recipe_name = name
        if name in core and target not in known:
            target = 'Telegram/CMakeFiles/Telegram.dir/ThirdParty/purple_core/purple/purple_settings.cpp.o'
            recipe_name = 'purple_settings'
        lines = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', target], text=True)
        recipe = next(shlex.split(line) for line in lines.splitlines() if ' -c ' in line
                      and shlex.split(line)[shlex.split(line).index('-c') + 1].endswith('/' + recipe_name + '.cpp'))
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
