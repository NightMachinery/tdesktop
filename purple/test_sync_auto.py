#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--output-dir', type=Path)
parser.add_argument('--use-build-objects', action='store_true')
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
build = args.build_dir.resolve()
owned = tempfile.TemporaryDirectory(prefix='purple-auto-test.') if not args.output_dir else None
work = args.output_dir.resolve() if args.output_dir else Path(owned.name)
work.mkdir(parents=True, exist_ok=True)
base = 'Telegram/CMakeFiles/Telegram.dir/'
def unit_recipe(name):
    relative = 'Telegram/SourceFiles/purple/' + name + '.cpp'
    target = base + 'SourceFiles/purple/' + name + '.cpp.o'
    recipes = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', target], text=True)
    matches = [shlex.split(line) for line in recipes.splitlines() if ' -c ' in line]
    return next(command for command in matches
        if Path(command[command.index('-c') + 1]).as_posix().endswith('/' + relative))

recipe = unit_recipe('purple_config_file')
(work / 'opaque-session.h').write_text('#pragma once\nnamespace Main { class Session {}; class Account {}; }\n')
source = (repo / 'Telegram/SourceFiles/purple/purple_sync_config_apply.cpp').read_text()
body = source.replace('#include "main/main_session.h"', '#include "' + str(work / 'opaque-session.h') + '"')
assert body != source
(work / 'apply-body.cpp').write_text(body)
service = (repo / 'Telegram/SourceFiles/purple/purple_sync_auto_service.cpp').read_text()
methods = service[service.index('std::optional<SyncAutoFacts> AutoSession::Facts('):service.index('void AutoSession::Saved(')].replace('AutoSession::Facts(', 'AutoSession::BuildFacts(')
methods += service[service.index('bool AutoSession::Accept('):service.index('void AutoSession::RetryLater(')]
methods += service[service.index('void AutoSession::Complete('):service.index('void AutoSession::Check(')]
check = service[service.index('void AutoSession::Check('):service.index('void AutoSession::ReadNext(')]
marker = '[weak, plan, origin = lease->route.peer.origin](DesktopSyncChatInventory inventory)'
start = check.index('{', check.index(marker)) + 1
end = start
depth = 1
while depth:
    depth += (check[end] == '{') - (check[end] == '}')
    end += 1
callback = check[start:end - 1]
methods += 'void AutoSession::InvokeInventory(const SyncAutoPlan &plan, SyncAccountInventoryResult reply) {\n auto weak = Weak{ this, alive };\n const auto route = ProbeSelfRoute(_account);\n const auto origin = route.peer.origin;\n auto inventory = DesktopSyncChatInventory{route, std::move(reply)};\n' + callback + '\n}\n'
(work / 'auto-service-boundary.inc').write_text(methods)

objects = []
commands = []
with (work / 'build.log').open('w') as log:
    def compile(source, name, opaque=False, own_recipe=None):
        target = work / (name + '.o')
        command = (own_recipe if own_recipe is not None else recipe)[:]
        for flag, value in [('-c', str(source)), ('-o', str(target)), ('-MF', str(target.with_suffix('.d'))), ('-MT', str(target))]:
            command[command.index(flag) + 1] = value
        if opaque:
            command += ['-include', str(work / 'opaque-session.h'), '-I', str(work)]
        commands.append(command)
        subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
        return target
    objects += [compile(repo / 'purple/test_sync_auto.cpp', 'test', True), compile(work / 'apply-body.cpp', 'apply')]
    adapters = ['purple_account_config', 'purple_config_file', 'purple_sync_local_store', 'purple_sync_config_history', 'purple_sync_config_review', 'purple_sync_config_text', 'purple_sync_auto_store', 'purple_sync_auto_transport', 'purple_sync_chat_facts', 'purple_sync_chat_io', 'purple_sync_chat_requests']
    for name in adapters:
        if name in ['purple_sync_config_text', 'purple_sync_config_review']:
            objects.append(compile(repo / ('Telegram/SourceFiles/purple/' + name + '.cpp'), name,
                own_recipe=unit_recipe(name)))
        elif not args.use_build_objects and name in ['purple_account_config', 'purple_sync_local_store', 'purple_sync_auto_store', 'purple_sync_auto_transport', 'purple_sync_chat_facts', 'purple_sync_chat_io', 'purple_sync_chat_requests']:
            objects.append(compile(repo / ('Telegram/SourceFiles/purple/' + name + '.cpp'), name))
        else:
            objects.append(build / (base + 'SourceFiles/purple/' + name + '.cpp.o'))
    for source in sorted((repo / 'Telegram/ThirdParty/purple_core/purple').glob('*.cpp')):
        if not args.use_build_objects and source.stem in ['purple_sync_auto', 'purple_sync_auto_state']:
            objects.append(compile(source, source.stem))
        else:
            objects.append(build / (base + 'ThirdParty/purple_core/purple/' + source.name + '.o'))
    objects += [build / (base + 'SourceFiles/data/data_peer_id.cpp.o'), build / (base + 'SourceFiles/data/data_types.cpp.o'), build / 'Telegram/CMakeFiles/td_scheme.dir/gen/scheme.cpp.o', build / 'Telegram/lib_base/liblib_base.a']
    link = [recipe[0]]
    for flag in ['-arch', '-isysroot']:
        if flag in recipe:
            link += [flag, recipe[recipe.index(flag) + 1]]
    for value in recipe:
        if value.startswith('-F'):
            link += [value, '-Wl,-rpath,' + value[2:]]
    link += ['-Wl,-dead_strip', *map(str, objects), '-framework', 'QtCore', '-framework', 'QtGui', '-framework', 'QtWidgets', '-framework', 'AppKit', '-o', str(work / 'test_sync_auto')]
    subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
(work / 'inputs.json').write_text(json.dumps({'compile': commands, 'link': link, 'apply_body': 'unchanged production source except opaque session include', 'service_boundary': 'production Facts (method name only changed), exact Accept/Hold/Stop/Complete and inventory callback bodies with controlled session/transport/UI endpoints'}, indent=2) + '\n')
exits = []
for name, args in [('run', []), ('worker-run', ['--worker'])]:
    with (work / (name + '.log')).open('w') as log:
        result = subprocess.run([str(work / 'test_sync_auto'), *args], stdout=log, stderr=subprocess.STDOUT)
    (work / (name + '.exit')).write_text(str(result.returncode) + '\n')
    print((work / (name + '.log')).read_text())
    exits.append(result.returncode)
raise SystemExit(next((code for code in exits if code), 0))
