#!/usr/bin/env python3
from sync_store_units import add_current_core_units
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
parser.add_argument('--test-source', choices=['test_sync_chat_io.cpp', 'test_sync_chat_write.cpp', 'test_sync_config_publish.cpp'], default='test_sync_chat_io.cpp')
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
build = args.build_dir.resolve()
work = args.output_dir.resolve()
work.mkdir(parents=True, exist_ok=True)
binary = work / Path(args.test_source).stem
header = build / 'Telegram/gen/scheme.h'
schema = header.read_text()
constructors = []
for name, factory in [('Message', 'message'), ('MessageService', 'messageService'),
                      ('User', 'user'), ('UserFull', 'userFull'), ('Chat', 'chat'),
                      ('ChatFull', 'chatFull'), ('Channel', 'channel'),
                      ('ChannelFull', 'channelFull'), ('MessageMediaDocument', 'messageMediaDocument'), ('Messages', 'messages_messages'),
                      ('MessagesSlice', 'messages_messagesSlice'), ('ChannelMessages', 'messages_channelMessages'), ('Document', 'document'), ('Updates', 'updates'), ('UpdatesCombined', 'updatesCombined'), ('ShortSent', 'updateShortSentMessage')]:
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
overlay = work / 'include'
overlay.mkdir(exist_ok=True)
if not (overlay / 'purple').exists():
    (overlay / 'purple').symlink_to(repo / 'Telegram/SourceFiles/purple', target_is_directory=True)
source = repo / 'Telegram/SourceFiles/purple'
scanner = (source / 'purple_sync_history_scanner.cpp').read_text()
reader = (source / 'purple_sync_candidate_reader.cpp').read_text()
transport = (source / 'purple_sync_auto_transport.cpp').read_text()
def block(text, marker):
    start = text.index('{', text.index(marker) + len(marker))
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]
meta = 'namespace Purple { SyncHistoryMessageMeta SyncHistoryMessageMetaOf(const MTPMessage &message) ' + block(scanner, 'SyncHistoryMessageMeta SyncHistoryMessageMetaOf(') + '\nbool IsSyncHistoryCandidate(const MTPMessage &message) ' + block(scanner, 'bool IsSyncHistoryCandidate(') + ' }\n'
(work / 'sync-chat-native-meta.inc').write_text(meta)
for name, text, marker in [
    ('typed-scan-failure.inc', scanner, ').fail([weak](const MTP::Error &error)'),
    ('typed-reader-failure.inc', reader, 'const auto failed = [weak, id](const MTP::Error &)'),
    ('typed-scan-reply.inc', scanner, ').done([weak](const MTPmessages_Messages &result)'),
    ('typed-reader-reply.inc', reader, 'const auto completed = [weak, id](const MTPmessages_Messages &result)'),
    ('typed-exact-reply.inc', transport, 'const auto completed = [weak, readback](const MTPmessages_Messages &result)'),
    ('typed-download.inc', reader, 'void SyncCandidateReader::OnDownload()')]:
    (work / name).write_text(block(text, marker))
if args.test_source != 'test_sync_chat_io.cpp':
    upload = (source / 'purple_sync_config_upload.cpp').read_text()
    post = (source / 'purple_sync_config_post.cpp').read_text()
    automatic = (source / 'purple_sync_auto_service.cpp').read_text()
    (work / 'auto-operation.inc').write_text(block(automatic, 'DesktopSyncChatOperation AutoSession::OperationFor('))
    (work / 'post-counters.inc').write_text(block(post, 'std::shared_ptr<PostCounters> PostsFor(not_null<Main::Session*> session, PeerToken chat = {})'))
    (work / 'post-network.inc').write_text(block(post, 'std::function<void()> BeginSyncConfigNetworkOperation(\n\t\tnot_null<Main::Session*> session,\n\t\tPeerToken chat)'))
    for method in ['RequestCurrent', 'Current']:
        (work / ('upload-' + method + '.inc')).write_text(block(upload, 'bool SyncConfigUpload::' + method + '('))
    for method in ['SendNextPart', 'SendMedia', 'RenewBeforePost', 'ResumeEffect', 'DispatchMedia', 'OnMedia', 'Finish', 'Cancel']:
        (work / ('upload-' + method + '.inc')).write_text(block(upload, 'void SyncConfigUpload::' + method + '('))
    (work / 'post-current.inc').write_text(block(post, 'bool SyncConfigPost::ContextCurrent('))
    for method in ['OnReceipt', 'OnReadback', 'Finish', 'Cancel']:
        (work / ('post-' + method + '.inc')).write_text(block(post, 'void SyncConfigPost::' + method + '('))
if args.test_source == 'test_sync_config_publish.cpp':
    publisher = (source / 'purple_sync_config_publish.cpp').read_text()
    for method in ['Cancel', 'Finish', 'OnPost']:
        (work / ('publisher-' + method + '.inc')).write_text(block(publisher, 'void SyncConfigPublish::' + method + '('))
    (work / 'publisher-source-change.inc').write_text(block(publisher, 'const auto changed = [weak]'))
    config = (source / 'purple_config.cpp').read_text()
    (work / 'publisher-routing-current.inc').write_text(block(config, 'bool AccountSyncRoutingCurrent('))
    retry = (source / 'purple_sync_request_sender.cpp').read_text()
    for method in ['cancelAll', 'cancel', 'dispatch', 'failed', 'arm', 'tick', 'resume']:
        (work / ('publisher-retry-' + method + '.inc')).write_text(block(retry, 'void SyncRequestSender::' + method + '(').replace('crl::now()', 'PublishMono').replace('base::unixtime::now()', 'PublishUnix'))
recipe = [value for value in recipe if not value.startswith('-DLANG_KEYS_SUBSET=')]
objects = []
compiled = []
names = ['purple_sync_chat_facts', 'purple_sync_chat_requests', 'purple_sync_chat_io', 'purple_sync_auto_transport', 'purple_sync_local_store']
if args.test_source != 'test_sync_chat_io.cpp': names += ['purple_sync_chat_write']
if args.test_source == 'test_sync_config_publish.cpp':
    names += ['purple_sync_config_publish_flow', 'purple_account_config', 'purple_config_file', 'purple_sync_config_review', 'purple_sync_account_recovery']
sources = [repo / 'purple' / args.test_source, *[source / (name + '.cpp') for name in names],
    repo / 'Telegram/SourceFiles/mtproto/mtproto_response.cpp',
    repo / 'Telegram/ThirdParty/purple_core/purple/purple_sync_chat.cpp',
    repo / 'Telegram/ThirdParty/purple_core/purple/purple_peer_token.cpp']
with (work / 'build.log').open('w') as log:
    for path in sources:
        target = work / (path.stem + '.o')
        command = recipe[:]
        for flag, value in [('-c', path), ('-o', target), ('-MF', target.with_suffix('.d')), ('-MT', target)]:
            command[command.index(flag) + 1] = str(value)
        command[1:1] = ['-I' + str(overlay), '-I' + str(repo / 'Telegram/ThirdParty/purple_core'), '-I' + str(work), '-I' + str(repo / 'purple')]
        compiled.append(command)
        subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
        objects.append(target)
    compiled_core = {'purple_sync_chat', 'purple_peer_token'}
    for path in sorted((build / 'Telegram/CMakeFiles/Telegram.dir/ThirdParty/purple_core/purple').glob('*.cpp.o')):
        if not (repo / 'Telegram/ThirdParty/purple_core/purple' / path.name.removesuffix('.o')).is_file():
            continue
        if path.stem.removesuffix('.cpp') not in compiled_core and (repo / 'Telegram/ThirdParty/purple_core/purple' / path.name.removesuffix('.o')).is_file():
            objects.append(path)
    objects += [build / 'Telegram/CMakeFiles/Telegram.dir/SourceFiles/data/data_types.cpp.o',
        build / 'Telegram/CMakeFiles/Telegram.dir/SourceFiles/data/data_peer_id.cpp.o',
        build / 'Telegram/CMakeFiles/td_scheme.dir/gen/scheme.cpp.o',
        build / 'Telegram/lib_tl/CMakeFiles/lib_tl.dir/tl/tl_basic_types.cpp.o',
        build / 'Telegram/lib_base/liblib_base.a']
    add_current_core_units(repo, build, work, objects, compiled, log)
    link = [recipe[0]]
    for flag in ['-arch', '-isysroot']:
        if flag in recipe: link += [flag, recipe[recipe.index(flag) + 1]]
    for value in recipe:
        if value.startswith('-F'): link += [value, '-Wl,-rpath,' + value[2:]]
    link += ['-Wl,-dead_strip', *map(str, objects), '-framework', 'QtCore', '-framework', 'QtGui', '-framework', 'QtWidgets', '-framework', 'AppKit', '-o', str(binary)]
    subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
(work / 'inputs.json').write_text(json.dumps({'compile': compiled, 'link': link,
    'schema_sha256': hashlib.sha256(header.read_bytes()).hexdigest(),
    'driver_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    'sources_sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources},
    'included_fixture_sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in [repo / 'purple/sync_chat_test_fixtures.h', repo / 'purple/test_sync_chat_io.cpp', repo / 'purple/test_sync_chat_write.cpp']},
    'literal_callback_sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in work.glob('*.inc')},
    'objects_sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in objects},
    'boundary': 'Current typed IO, record ownership, exact readback, ordinary upload, account/write fences and first-use publish. Literal production callbacks and real generated TL constructors. No legacy discovery/carry implementations, GUI or network.'}, indent=2) + '\n')
if not args.build_only:
    result = subprocess.run([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (work / 'run.log').write_bytes(result.stdout)
    (work / 'run.exit').write_text(str(result.returncode) + '\n')
    print(result.stdout.decode())
    raise SystemExit(result.returncode)
