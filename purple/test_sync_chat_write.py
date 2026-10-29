#!/usr/bin/env python3
from sync_carry_units import add_carry_units
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
parser.add_argument('--test-source', choices=['test_sync_chat_write.cpp', 'test_sync_chat_carry_io.cpp', 'test_sync_chat_carry.cpp', 'test_sync_chat_carry_review.cpp', 'test_sync_chat_consent.cpp', 'test_sync_chat_marks.cpp', 'test_sync_chat_consent_review.cpp'], default='test_sync_chat_write.cpp')
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
service = (repo / 'Telegram/SourceFiles/purple/purple_sync_chat_service.cpp').read_text()
scanner = (repo / 'Telegram/SourceFiles/purple/purple_sync_history_scanner.cpp').read_text()
reader = (repo / 'Telegram/SourceFiles/purple/purple_sync_candidate_reader.cpp').read_text()
def block(text, marker):
    start = text.index('{', text.index(marker) + len(marker))
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]
accept = block(service, 'bool SyncChatSession::Accept(')
cancel = block(service, 'void SyncChatSession::Cancel(')
publish = block(service, 'void SyncChatSession::Publish(')
scan = block(scanner, 'for (const auto &message : *messages)')
owned = block(reader, 'if (self->_owned)')
meta = 'namespace Purple { SyncHistoryMessageMeta SyncHistoryMessageMetaOf(const MTPMessage &message) ' + block(scanner, 'SyncHistoryMessageMeta SyncHistoryMessageMetaOf(') + '\nbool IsSyncHistoryCandidate(const MTPMessage &message) ' + block(scanner, 'bool IsSyncHistoryCandidate(') + ' }\n'
(work / 'sync-chat-native-meta.inc').write_text(meta)

(work / 'sync-chat-production-ports.inc').write_text(
    'bool ProbeSession::Accept(const DesktopSyncChatCommand &command) ' + accept + '\n'
    + 'void ProbeSession::ProductionCancel(QString reason) ' + cancel + '\n'
    + 'void ProbeSession::Publish(bool final) ' + publish + '\n'
    + 'void ProbeScan(ProbeOwnership *self, const QVector<MTPMessage> *messages, std::vector<SyncHistoryPageItem> &page) { for (const auto &message : *messages) ' + scan + ' }\n'
    + 'void ProbeReadOwnership(ProbeOwnership *self, const MTPMessage &message, int id) { if (self->_owned) ' + owned + ' self->accepted = true; }\n')
(work / 'typed-scan-failure.inc').write_text(block(scanner, ').fail([weak](const MTP::Error &error)'))
(work / 'typed-reader-failure.inc').write_text(block(reader, 'const auto failed = [weak, id](const MTP::Error &)'))
(work / 'typed-scan-reply.inc').write_text(block(scanner, ').done([weak](const MTPmessages_Messages &result)'))
(work / 'typed-reader-reply.inc').write_text(block(reader, 'const auto completed = [weak, id](const MTPmessages_Messages &result)'))
transport = (repo / 'Telegram/SourceFiles/purple/purple_sync_auto_transport.cpp').read_text()
(work / 'typed-exact-reply.inc').write_text(block(transport, 'const auto completed = [weak, readback](const MTPmessages_Messages &result)'))
(work / 'typed-download.inc').write_text(block(reader, 'void SyncCandidateReader::OnDownload()'))

create = (repo / 'Telegram/SourceFiles/purple/purple_sync_chat_create.cpp').read_text()
(work / 'carry-mark-text.inc').write_text('namespace Purple { QString DesktopSyncChatMarkText() ' + block(create, 'QString DesktopSyncChatMarkText(') + ' }\n')
upload = (repo / 'Telegram/SourceFiles/purple/purple_sync_config_upload.cpp').read_text()
carry_constructor_at = upload.index('\t\tDesktopSyncCarryPermit permit,')
carry_constructor_start = upload.rfind('SyncConfigUpload::SyncConfigUpload(', 0, carry_constructor_at)
carry_constructor_body = block(upload, '\t\tDesktopSyncCarryPermit permit,')
carry_constructor_end = upload.index(carry_constructor_body, carry_constructor_at) + len(carry_constructor_body)
carry_constructor = upload[carry_constructor_start:carry_constructor_end]
(work / 'upload-CarryConstructor.inc').write_text(carry_constructor.replace(
    'SyncConfigUpload::SyncConfigUpload(', 'UploadProbe::UploadProbe(').replace(
    'Main::Session', 'UploadSession').replace(': SyncConfigUpload(', ': UploadProbe(') + '\n')

post = (repo / 'Telegram/SourceFiles/purple/purple_sync_config_post.cpp').read_text()
automatic = (repo / 'Telegram/SourceFiles/purple/purple_sync_auto_service.cpp').read_text()
(work / 'auto-operation.inc').write_text(block(automatic, 'DesktopSyncChatOperation AutoSession::OperationFor('))
(work / 'upload-Current.inc').write_text(block(upload, 'bool SyncConfigUpload::Current('))
(work / 'post-counters.inc').write_text(block(post, 'std::shared_ptr<PostCounters> PostsFor(not_null<Main::Session*> session, PeerToken chat = {})'))
(work / 'post-network.inc').write_text(block(post, 'std::function<void()> BeginSyncConfigNetworkOperation(\n\t\tnot_null<Main::Session*> session,\n\t\tPeerToken chat)'))
for method in ['SendNextPart', 'SendMedia', 'RenewBeforePost', 'ResumeEffect', 'DispatchMedia', 'OnMedia', 'Finish', 'Cancel']:
    (work / ('upload-' + method + '.inc')).write_text(block(upload, 'void SyncConfigUpload::' + method + '('))
(work / 'post-current.inc').write_text(block(post, 'bool SyncConfigPost::ContextCurrent('))
for method in ['OnReceipt', 'OnReadback', 'Finish', 'Cancel']:
    (work / ('post-' + method + '.inc')).write_text(block(post, 'void SyncConfigPost::' + method + '('))

assert 'return entry;' in (work / 'post-counters.inc').read_text()
recipe = [value for value in recipe if not value.startswith('-DLANG_KEYS_SUBSET=')]
objects = []
compiled = []
consent_sources = [repo / ('Telegram/SourceFiles/purple/' + name + '.cpp')
    for name in ['purple_sync_chat_consent', 'purple_sync_chat_setup']] if args.test_source in ['test_sync_chat_consent.cpp', 'test_sync_chat_consent_review.cpp'] else []
if args.test_source == 'test_sync_chat_consent_review.cpp':
    source = (repo / 'purple/test_sync_chat_consent.cpp').read_text()
    assert source.count('int main(') == 1
    (work / 'embedded_consent.inc').write_text(source.replace('int main(', 'int EmbeddedConsentMain(', 1))
    recipe[1:1] = ['-I' + str(repo / 'purple')]
if args.test_source == 'test_sync_chat_marks.cpp':
    consent_sources = [repo / 'Telegram/SourceFiles/purple/purple_sync_chat_mark_actions.cpp']
    mark_transport = (repo / 'Telegram/SourceFiles/purple/purple_sync_chat_mark_transport.cpp').read_text()
    (work / 'mark-transport.inc').write_text('bool ProbeMarkTransport(MarkSenderProbe &sender, DesktopSyncChatMarkPermit permit, DesktopSyncChatMarkRenewal renew, Fn<void(Fn<void()>)> onSender, Fn<void(std::optional<MTPmessages_AffectedMessages>)> done) ' + block(mark_transport, 'bool SendDesktopSyncChatMarkDelete('))
with (work / 'build.log').open('w') as log:
    for source in [repo / 'purple' / args.test_source,
                   repo / 'Telegram/SourceFiles/purple/purple_sync_chat_write.cpp',
                   repo / 'Telegram/SourceFiles/purple/purple_sync_chat_facts.cpp',
                   repo / 'Telegram/SourceFiles/purple/purple_sync_chat_requests.cpp',
                   repo / 'Telegram/SourceFiles/purple/purple_sync_chat_io.cpp',
                   repo / 'Telegram/SourceFiles/purple/purple_sync_auto_transport.cpp',
                   repo / 'Telegram/SourceFiles/mtproto/mtproto_response.cpp',
                   repo / 'Telegram/SourceFiles/purple/purple_sync_local_store.cpp',
                   repo / 'Telegram/SourceFiles/purple/purple_sync_chat_carry_store.cpp',
                   repo / 'Telegram/SourceFiles/purple/purple_sync_chat_carry_io.cpp',
                   repo / 'Telegram/SourceFiles/purple/purple_sync_chat_carry_executor.cpp',

                   repo / 'Telegram/SourceFiles/purple/purple_sync_chat_cache.cpp',
                   repo / 'Telegram/SourceFiles/purple/purple_sync_chat_discovery.cpp',
                   repo / 'Telegram/SourceFiles/purple/purple_sync_chat_store.cpp',
                   repo / 'Telegram/ThirdParty/purple_core/purple/purple_sync_chat.cpp',
                   repo / 'Telegram/ThirdParty/purple_core/purple/purple_peer_token.cpp', *consent_sources]:
        target = work / (('core-' if 'purple_core' in source.parts and source.stem in ['purple_sync_chat_carry','purple_sync_chat_carry_codec','purple_sync_chat_journal'] else '') + source.stem + '.o')
        command = recipe[:]
        for flag, value in [('-c', str(source)), ('-o', str(target)),
                            ('-MF', str(target.with_suffix('.d'))), ('-MT', str(target))]:
            command[command.index(flag) + 1] = value
        command[1:1] = ['-I' + str(overlay),
                         '-I' + str(repo / 'Telegram/ThirdParty/purple_core'), '-I' + str(work)]
        compiled.append(command)
        subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
        objects.append(target)
    objects += [build / 'Telegram/CMakeFiles/Telegram.dir/ThirdParty/purple_core/purple/purple_sync_directory.cpp.o',
                build / 'Telegram/CMakeFiles/Telegram.dir/SourceFiles/data/data_types.cpp.o',
                build / 'Telegram/CMakeFiles/Telegram.dir/SourceFiles/data/data_peer_id.cpp.o',
                build / 'Telegram/CMakeFiles/Telegram.dir/ThirdParty/purple_core/purple/purple_sync_envelope.cpp.o',
                build / 'Telegram/CMakeFiles/Telegram.dir/ThirdParty/purple_core/purple/purple_config_sync.cpp.o',
                build / 'Telegram/CMakeFiles/Telegram.dir/ThirdParty/purple_core/purple/purple_sync_json.cpp.o',
                build / 'Telegram/CMakeFiles/Telegram.dir/ThirdParty/purple_core/purple/purple_sync_local_state.cpp.o',
                build / 'Telegram/CMakeFiles/Telegram.dir/ThirdParty/purple_core/purple/purple_sync_inventory.cpp.o',
                build / 'Telegram/CMakeFiles/td_scheme.dir/gen/scheme.cpp.o',
                build / 'Telegram/lib_tl/CMakeFiles/lib_tl.dir/tl/tl_basic_types.cpp.o',
                build / 'Telegram/lib_base/liblib_base.a']
    existing = set(objects)
    for path in (build / 'Telegram/CMakeFiles/Telegram.dir/ThirdParty/purple_core/purple').glob('*.cpp.o'):
        if path.stem.removesuffix('.cpp') not in ['purple_sync_chat', 'purple_peer_token'] and path not in existing:
            objects.append(path)
    add_carry_units(repo, build, work, objects, compiled, log)
    link = [recipe[0]]
    for flag in ['-arch', '-isysroot']:
        if flag in recipe:
            link += [flag, recipe[recipe.index(flag) + 1]]
    for value in recipe:
        if value.startswith('-F'):
            link += [value, '-Wl,-rpath,' + value[2:]]
    link += ['-Wl,-dead_strip', *map(str, objects), '-framework', 'QtCore',
             '-framework', 'QtGui', '-framework', 'QtWidgets', '-framework', 'AppKit',
             '-o', str(binary)]
    subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
(work / 'inputs.json').write_text(json.dumps({'compile': compiled, 'link': link,
    'schema_sha256': hashlib.sha256(header.read_bytes()).hexdigest(),
    'production_ports_sha256': {str(repo / 'Telegram/SourceFiles/purple' / name): hashlib.sha256((repo / 'Telegram/SourceFiles/purple' / name).read_bytes()).hexdigest() for name in ['purple_sync_chat_create.cpp', 'purple_sync_chat_service.cpp', 'purple_sync_history_scanner.cpp', 'purple_sync_candidate_reader.cpp', 'purple_sync_config_upload.cpp', 'purple_sync_config_post.cpp', 'purple_sync_auto_service.cpp']},
    'included_fixture_sha256': hashlib.sha256((repo / 'purple/test_sync_chat_discovery.cpp').read_bytes()).hexdigest(),
    'included_io_fixture_sha256': hashlib.sha256((repo / 'purple/test_sync_chat_io.cpp').read_bytes()).hexdigest(),
    'driver_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    'review_consent_source_sha256': hashlib.sha256((repo / 'purple/test_sync_chat_consent.cpp').read_bytes()).hexdigest() if args.test_source == 'test_sync_chat_consent_review.cpp' else None,
    'review_consent_port_sha256': hashlib.sha256((work / 'embedded_consent.inc').read_bytes()).hexdigest() if args.test_source == 'test_sync_chat_consent_review.cpp' else None,
    'mark_transport_sha256': hashlib.sha256((repo / 'Telegram/SourceFiles/purple/purple_sync_chat_mark_transport.cpp').read_bytes()).hexdigest() if args.test_source == 'test_sync_chat_marks.cpp' else None,
    'mark_transport_port_sha256': hashlib.sha256((work / 'mark-transport.inc').read_bytes()).hexdigest() if args.test_source == 'test_sync_chat_marks.cpp' else None,
    'literal_callback_sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in [work / 'auto-operation.inc'] + list(work.glob('upload-*.inc')) + list(work.glob('post-*.inc')) + [work / 'typed-scan-reply.inc', work / 'typed-reader-reply.inc', work / 'typed-download.inc', work / 'typed-exact-reply.inc', work / 'typed-scan-failure.inc', work / 'typed-reader-failure.inc']},
    'headers_sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in (repo / 'Telegram/SourceFiles/purple').glob('purple_sync_chat_*.h')},
    'sources_sha256': {str(source): hashlib.sha256(Path(source).read_bytes()).hexdigest()
                       for source in [c[c.index('-c') + 1] for c in compiled]},
    'objects_sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in objects},
    'boundary': 'Upload parts/fresh-gate/dispatch/Updates/finish/cancel and post receipt/readback/finish/cancel bodies are literal production ports; session, API queue, before-post service and exact-reader transports are substituted without network. Entropy endpoint is deterministic in test; stage/ledger and TL objects are real. Actual mapper, C0, requests, cache, discovery and disk adapter compiled directly; literal Accept/Cancel/Publish and scanner/reader ownership bodies are ported with substituted session/notification/worker endpoints. Real generated TL constructors; reused schema/base/private-file/core-state objects. Unit-only, no app build/network/native acceptance.'}, indent=2) + '\n')
if not args.build_only:
    result = subprocess.run([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (work / 'run.log').write_bytes(result.stdout)
    (work / 'run.exit').write_text(str(result.returncode) + '\n')
    print(result.stdout.decode())
    raise SystemExit(result.returncode)
