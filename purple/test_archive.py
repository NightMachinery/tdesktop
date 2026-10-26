#!/usr/bin/env python3
import argparse
import hashlib
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
parser.add_argument('--views', action='store_true')
parser.add_argument('--purge', action='store_true')
parser.add_argument('--lifecycle', action='store_true')
parser.add_argument('--access', action='store_true')
parser.add_argument('--pagination-only', action='store_true')
parser.add_argument('--handoff', action='store_true')
parser.add_argument('--passive', action='store_true')
parser.add_argument('--build-only', action='store_true')
parser.add_argument('--reuse-from', type=Path)
args = parser.parse_args()
args.access = args.access or args.pagination_only
repo = Path(__file__).resolve().parent.parent
build = args.build_dir.resolve()
owned = tempfile.TemporaryDirectory(prefix='purple-archive-test.') if not args.output_dir else None
work = args.output_dir.resolve() if args.output_dir else Path(owned.name)
work.mkdir(parents=True, exist_ok=True)
service = (repo / 'Telegram/SourceFiles/purple/purple_archive_service.cpp').read_text()
start = service.index('if (!weak || Retired.contains(weak.get()))')
end = start + re.search(r'\n\s*\}\);', service[start:]).start()
(work / 'archive-page-callback.inc').write_text(service[start:end] + '\n')
if args.access:
    begin = service.index('void OpenArchiveMedia(')
    position = service.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (service[position] == '{') - (service[position] == '}')
        position += 1
    callback = service[begin:position]
    assert callback.count('not_null<Main::Session*>') == 1
    (work / 'archive-media-callback.inc').write_text(callback.replace(
        'not_null<Main::Session*>', 'not_null<CallbackSession*>').replace(
        'crl::on_main', 'CallbackOnMain') + '\n')
    history = (repo / 'Telegram/SourceFiles/purple/purple_archive_history_box.cpp').read_text()
    begin = history.index('[=](ArchiveLeaseRevisionChange change)')
    position = history.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (history[position] == '{') - (history[position] == '}')
        position += 1
    history_callback = history[begin:position]
    (work / 'archive-history-lease.inc').write_text('return ' + history_callback + ';\n')
    reader = (repo / 'Telegram/SourceFiles/media/streaming/media_streaming_reader.cpp').read_text()
    (work / 'archive-reader-slices.inc').write_text(reader[
        reader.index('namespace Media {'):reader.index('Reader::Reader(')]
        + reader[reader.index('QByteArray SerializeComplexPartsMap('):])
def extract(text, signature):
    begin = text.index(signature)
    pos = text.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[begin:pos] + '\n'
if args.views:
    menu = (repo / 'Telegram/SourceFiles/purple/purple_archive_menu.cpp').read_text()
    deleted = (repo / 'Telegram/SourceFiles/purple/purple_archive_deleted_box.cpp').read_text()
    (work / 'archive-view-route.inc').write_text(extract(deleted, 'const auto current = [=]') + ';\n')
    (work / 'archive-scope-match.inc').write_text(extract(menu, 'bool SameArchiveViewScope('))
    worker = extract(service, 'class ArchiveWorker final') + ';\n'
    (work / 'archive-view-worker.inc').write_text(worker.replace('ArchiveWorker', 'ProbeViewWorker'))
    delivery = extract(service, 'void DeliverDeletedPage(')
    assert delivery.count('not_null<Main::Session*>') == 1
    (work / 'archive-deleted-delivery.inc').write_text(delivery.replace(
        'not_null<Main::Session*>', 'not_null<Session*>').replace('crl::on_main', 'OnMain'))
    history_action = extract(deleted, '[=] {\n\t\t\t\t\t\tif (current() && active->load())')
    (work / 'archive-deleted-history-action.inc').write_text('return ' + history_action + ';\n')
    history = (repo / 'Telegram/SourceFiles/purple/purple_archive_history_box.cpp').read_text()
    (work / 'archive-history-current.inc').write_text(extract(history, 'const auto current = [=]') + ';\n')
    (work / 'archive-history-notice.inc').write_text(extract(history, 'const auto check = [=]') + ';\n')
if args.purge:
    worker = extract(service, 'class ArchiveWorker final') + ';\n'
    (work / 'archive-purge-worker.inc').write_text(worker.replace('ArchiveWorker', 'ProbePurgeWorker'))
    execute = extract(service, 'void ExecuteArchivePurge(')
    execute = execute.replace('ExecuteArchivePurge(', 'ExecuteProbePurge(', 1).replace('Main::Session', 'ProbePurgeSession').replace('Retired', 'PurgeRetired').replace('For(session)', 'PurgeFor(session)').replace('crl::on_main', 'HarnessOnMain')
    (work / 'archive-purge-execute.inc').write_text(execute)
    bind = extract((repo / 'Telegram/SourceFiles/purple/purple_archive_purge_box.cpp').read_text(), 'void BindArchivePurgeScopeChanges(')
    (work / 'archive-purge-migration-bind.inc').write_text(bind.replace('Main::Session', 'ProbeMigrationSession'))
    native_changes = (repo / 'Telegram/SourceFiles/data/data_changes.cpp').read_text()
    (work / 'archive-native-peer-manager.inc').write_text(native_changes[native_changes.index('namespace Data {'):native_changes.index('Changes::Changes(')] + '}\n')
    media = extract(service, 'void OpenArchiveMedia(').replace('Main::Session', 'RetirementSession').replace('crl::on_main', 'RetirementOnMain')
    (work / 'archive-retirement-open.inc').write_text(media)
    deleted = (repo / 'Telegram/SourceFiles/purple/purple_archive_purge_box.cpp').read_text()
    menu = (repo / 'Telegram/SourceFiles/purple/purple_archive_menu.cpp').read_text()
    (work / 'archive-purge-route.inc').write_text(extract(deleted, 'const auto current = [=]') + ';\n')
    (work / 'archive-purge-scope.inc').write_text(extract(menu, 'bool SameArchiveViewScope('))
if args.handoff:
    bodies = ''.join(extract(service, sign) for sign in ('void QueueArchiveRetentionGap(', 'void QueueArchiveJournalWrite(', 'void QueueArchiveLifecycleFact(', 'void QueueArchiveObservation('))
    (work / 'archive-capture-queue.inc').write_text(bodies.replace('Main::Session', 'ProbeSession'))
    post = extract(service, 'bool Post(Task task, bool cleanup = false)')
    (work / 'archive-worker-post.inc').write_text(post.replace('bool Post(', 'bool ProbeWorker::Post(', 1).replace(' = false', ''))
if args.passive:
    acquired = (repo / 'Telegram/SourceFiles/purple/purple_archive_acquired_source.cpp').read_text()
    def adapt(text):
        return text.replace('Main::Session', 'ProbeSession').replace('Media::Streaming::Reader', 'ProbeReader').replace('Media::Streaming::Loader', 'ProbeLoader').replace('Media::Streaming::Document', 'ProbeDocument').replace('For(session).streams', 'StreamRegistryFor(session).streams').replace('For(session).documents', 'StreamRegistryFor(session).documents')
    (work / 'archive-passive-registry.inc').write_text(adapt(acquired[acquired.index('struct StreamBindings'):acquired.index('std::map<Main::Session*, Registry>')]))
    (work / 'archive-passive-sink.inc').write_text(adapt(extract(service, 'Fn<void(uint64_t, QByteArray)> ArchiveStreamSink(')))
    bridge = acquired[acquired.index('class ArchiveStreamBridge final'):acquired.index('namespace {')]
    (work / 'archive-passive-bridge.inc').write_text(adapt(bridge))
    (work / 'archive-passive-source.inc').write_text(adapt(extract(acquired, 'void Stream(')))
    binder = extract(acquired, 'void BindArchiveStreamDocument(').replace('BindArchiveStreamDocument(', 'BindDocument(', 1)
    (work / 'archive-passive-document.inc').write_text(adapt(binder + extract(acquired, 'void ReusedDocument(')))
    native = (repo / 'Telegram/SourceFiles/data/data_streaming.cpp').read_text()
    a = native.index('template <typename Data>\n[[nodiscard]] std::shared_ptr<Streaming::Reader>')
    b = native.index('template <typename Data>\n[[nodiscard]] std::shared_ptr<Streaming::Document>', a)
    c = native.index('template <typename Data>\nvoid Streaming::keepAlive', b)
    def native_port(text):
        return text.replace('Streaming::', 'StreamingProbe::').replace('FileOrigin origin', '::Data::FileOrigin origin')
    (work / 'archive-passive-reader.inc').write_text(native_port(native[a:b]))
    (work / 'archive-passive-shared-document.inc').write_text(native_port(native[b:c]))
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
    objects += [compile(repo / ('purple/test_archive_purge.cpp' if args.purge else 'purple/test_archive_views.cpp' if args.views else 'purple/test_archive_handoff.cpp' if args.handoff else 'purple/test_archive_passive.cpp' if args.passive else 'purple/test_archive_access.cpp' if args.access else 'purple/test_archive_lifecycle.cpp' if args.lifecycle else 'purple/test_archive.cpp'), 'test')]
    for name in ['purple_desktop_archive_store', 'purple_archive_maintenance', 'purple_archive_queries', 'purple_archive_sqlite', 'purple_archive_crypto', 'purple_archive_files', 'purple_archive_media', 'purple_archive_journal', 'purple_archive_capture_snapshot', 'purple_archive_attention', 'purple_archive_stream', 'purple_archive_media_access', 'purple_archive_media_loader', 'purple_archive_markdown', 'purple_markdown_publication', 'purple_markdown_source']:
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
(work / 'inputs.json').write_text(json.dumps({
    'compile': commands, 'link': link,
    'objects_sha256': {str(path): hashlib.sha256(Path(path).read_bytes()).hexdigest() for path in objects},
    'page_callback_boundary': 'unchanged production delivery lambda body with real weak lifetime and controlled retirement, ticket and UI endpoints',
    'media_callback_boundary': 'Exact production OpenArchiveMedia body; only session type and main-queue dispatcher are substituted. Controlled weak-session lifetime, Work Mode/source authority, worker/journal scheduling; real encrypted store, reader endpoint and loader.' if args.access else None,
    'history_lease_callback_sha256': hashlib.sha256(history_callback.encode()).hexdigest() if args.access else None,
    'media_callback_sha256': hashlib.sha256(callback.encode()).hexdigest() if args.access else None,
}, indent=2) + '\n')
if args.build_only:
    raise SystemExit(0)
with (work / 'run.log').open('w') as log:
    result = subprocess.run([str(work / 'test_archive')] + (['pagination-only'] if args.pagination_only else ['capture-only'] if args.capture_only else []), stdout=log, stderr=subprocess.STDOUT)
(work / 'run.exit').write_text(str(result.returncode) + '\n')
print((work / 'run.log').read_text())
raise SystemExit(result.returncode)
