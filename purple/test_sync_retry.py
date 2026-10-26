#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--output-dir', required=True, type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
work = args.output_dir.resolve()
work.mkdir(parents=True, exist_ok=True)
source = (repo / 'Telegram/SourceFiles/purple/purple_sync_request_sender.cpp').read_text()

def method(name):
    start = source.index('void SyncRequestSender::' + name + '(')
    brace = source.index('{', start)
    end = brace + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

(work / 'sync-retry-production.inc').write_text('\n'.join(
    method(name) for name in ['dispatch', 'failed', 'tick', 'resume', 'cancel']))
binary = work / 'test-sync-retry'
subprocess.run(['xcrun', 'clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
    '-I' + str(repo / 'Telegram/SourceFiles/purple'), '-I' + str(work),
    str(repo / 'purple/test_sync_retry.cpp'), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)


def member(text, marker):
    start = text.index(marker)
    brace = text.index('{', start)
    end = brace + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

root = repo / 'Telegram/SourceFiles/purple'
header = (root / 'purple_sync_request_sender.h').read_text()
wire = member(header, '\ttemplate <typename Request>\n\tmtpRequestId send(')
(work / 'retry-wire.inc').write_text(wire)
controller = '\n'.join(member(source, marker) for marker in [
    'std::shared_ptr<SyncRequestSender::Pending> SyncRequestSender::create(',
    'mtpRequestId SyncRequestSender::watch(',
    *['void SyncRequestSender::' + name + '(' for name in ['cancelAll', 'dispatch', 'failed', 'tick', 'resume', 'cancel', 'arm']]])
(work / 'retry-controller.inc').write_text(controller)
upload = (root / 'purple_sync_config_upload.cpp').read_text()
(work / 'retry-upload.inc').write_text('\n'.join(member(upload, signature) for signature in [
    'bool SyncConfigUpload::Current() const',
    'void SyncConfigUpload::SendMedia()',
    'void SyncConfigUpload::RenewBeforePost(',
    'void SyncConfigUpload::ResumeEffect(']))
service = (root / 'purple_sync_chat_service.cpp').read_text()
(work / 'retry-local-open.inc').write_text('\n'.join(member(service, 'void SyncChatSession::' + name + '(')
    .replace('SyncChatSession::', 'LocalOpenProbe::') for name in ['RetryLocalOpen', 'Publish']))
(work / 'retry-ensure.inc').write_text(member(service, 'void SyncChatSession::Ensure(').replace('SyncChatSession::', 'LocalOpenProbe::'))
reader = (root / 'purple_sync_candidate_reader.cpp').read_text()
start = reader.index('\tconst auto weak = base::make_weak(this);', reader.index('void SyncCandidateReader::StartDownload('))
end = reader.index('\n\tconst auto stopped =', start)
(work / 'retry-download-watch.inc').write_text(reader[start:end])
(work / 'retry-download.inc').write_text('\n'.join(member(reader, 'void SyncCandidateReader::' + name + '(')
    .replace('SyncCandidateReader::', 'DownloadProbe::') for name in ['RetireDownload', 'RetryDownload']))
automatic = (root / 'purple_sync_auto_service.cpp').read_text()
(work / 'retry-delete-gate.inc').write_text(member(automatic, 'void AutoSession::RenewEffect('))
integration = work / 'test-sync-retry-integration'
subprocess.run(['xcrun', 'clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
    '-I' + str(root), '-I' + str(work),
    str(repo / 'purple/test_sync_retry_integration.cpp'), '-o', str(integration)], check=True)
subprocess.run([str(integration)], check=True)

queue_header = (root / 'purple_emoji_export_queue.h').read_text().replace('#include <QtCore/QtGlobal>', 'using qint64 = int64_t; using quint64 = uint64_t;')
(work / 'emoji-queue-header.inc').write_text(queue_header)
(work / 'emoji-queue.inc').write_text((root / 'purple_emoji_export_queue.cpp').read_text().replace('#include "purple/purple_emoji_export_queue.h"', ''))
emoji = (root / 'purple_emoji_export.cpp').read_text()
(work / 'emoji-deferred.inc').write_text(member(emoji, 'void EmojiExporter::deferred(').replace('EmojiExporter::', 'EmojiExporterProbe::'))
resolve = member(emoji, 'void EmojiExporter::resolve()')
marker = '}).fail([=](const MTP::Error &) '
start = resolve.index(marker) + len(marker)
end = resolve.index(').handleAllErrors().send();', start)
(work / 'emoji-permanent-fail.inc').write_text(resolve[start:end])
publish = member(emoji, 'void EmojiExporter::publish()')
marker = '}, [=](quint64 generation, bool ok) '
start = publish.index(marker) + len(marker)
end = start + 1
depth = 1
while depth:
    depth += (publish[end] == '{') - (publish[end] == '}')
    end += 1
(work / 'emoji-publication-completion.inc').write_text(publish[start:end])
constant = next(line for line in emoji.splitlines() if line.startswith('constexpr auto kDebounceMs = '))
(work / 'emoji-publication-constant.inc').write_text(constant + '\n')

emoji_binary = work / 'test-sync-retry-emoji'
subprocess.run(['xcrun', 'clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
    '-I' + str(root), '-I' + str(work),
    str(repo / 'purple/test_sync_retry_emoji.cpp'), '-o', str(emoji_binary)], check=True)
subprocess.run([str(emoji_binary)], check=True)

expected = {
    'purple_emoji_export.cpp': 1,
    'purple_sync_chat_peer_request.cpp': 3,
    'purple_sync_chat_service.cpp': 5,
    'purple_sync_history_scanner.cpp': 1,
    'purple_sync_candidate_reader.cpp': 1,
    'purple_sync_auto_transport.cpp': 1,
    'purple_sync_config_upload.cpp': 3,
    'purple_sync_auto_service.cpp': 1,
}
root = repo / 'Telegram/SourceFiles/purple'
for name, count in expected.items():
    text = (root / name).read_text()
    assert text.count('.handleAllErrors()') == count, name
    header = root / name.replace('.cpp', '.h')
    combined = text + (header.read_text() if header.exists() else '')
    assert 'SyncRequestSender' in combined, name
    assert 'MTP::Sender' not in combined, name
assert 'session->api().request(' not in (root / 'purple_sync_config_upload.cpp').read_text()
peer = (root / 'purple_sync_chat_peer_request.cpp').read_text()
assert '_timeout' not in peer
service = (root / 'purple_sync_chat_service.cpp').read_text()
run = service[service.index('void SyncChatSession::Run()'):service.index('void SyncChatSession::Complete(')]
assert '_requestTimeout.callOnce' not in run and '_requestTimeout.cancel()' in run
assert '_current.reset();' in service, 'late local-open reply must not cancel the scheduled timeout retry'
assert 'PendingSyncRequest' in service
assert 'SyncRequestChanges()' in (root / 'purple_sync_chat_setup_box.cpp').read_text()
assert 'SyncRequestChanges()' not in service
sender = (root / 'purple_sync_request_sender.h').read_text()
assert 'request = std::move(request)' in sender and 'auto copy = request;' in sender
assert 'self->_current && self->_current()' in sender
assert 'pending->active' in sender and 'std::weak_ptr<Pending>' in sender
assert '.handleAllErrors().send()' in sender
for path in root.glob('*.cpp'):
    if '.handleAllErrors()' in path.read_text():
        assert path.name in expected, 'Uninventoried bypass: ' + path.name
guards = {
    'purple_emoji_export.cpp': ['std::make_unique<SyncRequestSender>', '_queue.transient(transient.retryAt,', '_queue.retryAt(_clock.elapsed())'],
    'purple_sync_request_sender.cpp': ['if (_deferred && !transient->shortRetry)', 'callback(id, *transient);'],
    'purple_sync_config_upload.cpp': ['}).resumeEffect([self]', 'self->ResumeEffect(std::move(ready));',
        'CheckSyncChatBeforePost(', '_authorizeEdit([self, ready = std::move(ready)]', '_lease.reset();'],
    'purple_sync_auto_service.cpp': ['}).resumeEffect([weak, plan]', 'self->RenewEffect(plan, std::move(ready));',
        'self->_store->AuthorizeEffect(plan, &read)', 'void AutoSession::RenewEffect(',
        'CheckSyncChatBeforePost(_session.get(), [weak, plan, ready = std::move(ready)]'],
    'purple_sync_chat_service.cpp': ['Publish(false);', 'final ? base::take(_callbacks)', 'if (_retryLocalOpen) {',
        'self->_openContinuation && error == u"account-lock-busy"_q',
        'if (_retryLocalOpen && !ContextMatches())', 'self->_current != current'],
    'purple_sync_candidate_reader.cpp': ['_downloadWatch = _api.watch(', 'self->_downloadGeneration == generation',
        'self->RetireDownload();', 'self->RetryDownload();', 'void SyncCandidateReader::RetryDownload()',
        'if (_downloadWatch) { _api.request(base::take(_downloadWatch)).cancel(); }'],
}
negative_count = 0
for name, sentinels in guards.items():
    text = (root / name).read_text()
    for sentinel in sentinels:
        assert sentinel in text, (name, sentinel)
        assert sentinel not in text.replace(sentinel, 'REMOVED_GUARD'), (name, sentinel)
        negative_count += 1
assert upload.count('}).resumeEffect([self]') == 2, 'both sends and edits require renewed effect authority'
assert 'pending->resume && transient->kind != SyncTransientKind::Flood' in source
assert '_request && _taskTime.elapsed()' not in emoji, 'controller owns resolution timeout across short floods'
assert 'std::unique_ptr<MTP::Sender>' not in emoji
handoff = member(source, 'void SyncRequestSender::failed(')
assert handoff.index('cancel(id);') < handoff.index('callback(id, *transient);')
print(f'{negative_count} effect/local-open/download source guards and negative mutations PASS')
print('16 request sites inventoried, controller/current/timeout/immutable-wire gates PASS')
(work / 'receipt.json').write_text(json.dumps({'siteCounts': expected,
    'productionMethods': ['dispatch', 'failed', 'tick', 'resume', 'cancel'],
    'literalIntegration': ['send-template', 'watch', 'upload-Current', 'upload-SendMedia', 'upload-RenewBeforePost', 'upload-ResumeEffect', 'service-RetryLocalOpen', 'service-Publish', 'service-Ensure', 'download-watch', 'download-retire', 'download-retry', 'delete-RenewEffect'],
    'negativeGuards': negative_count,
    'emojiDeferredGuards': True,
    'sourcesSha256': {str(path.relative_to(repo)): hashlib.sha256(path.read_bytes()).hexdigest() for path in [root / 'purple_sync_request_sender.h', root / 'purple_sync_request_sender.cpp', root / 'purple_sync_retry.h', root / 'purple_emoji_export.cpp', root / 'purple_emoji_export_queue.cpp', root / 'purple_emoji_export_queue.h', repo / 'purple/test_sync_retry_emoji.cpp', root / 'purple_sync_config_upload.cpp', root / 'purple_sync_auto_service.cpp', root / 'purple_sync_chat_service.cpp', root / 'purple_sync_candidate_reader.cpp', repo / 'purple/test_sync_retry.cpp', repo / 'purple/test_sync_retry_integration.cpp', Path(__file__)]},
    'literalPortsSha256': {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in work.glob('*.inc')},
    'binariesSha256': {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in [binary, integration, emoji_binary]},
    'transport': 'injected host only', 'nativeCalls': 0}, indent=2) + '\n')
