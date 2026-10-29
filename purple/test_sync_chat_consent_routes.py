#!/usr/bin/env python3
from pathlib import Path
import re

repo = Path(__file__).resolve().parent.parent
paths = {
    'policy': 'Telegram/SourceFiles/purple/purple_sync_chat_consent.cpp',
    'service': 'Telegram/SourceFiles/purple/purple_sync_chat_service.cpp',
    'io': 'Telegram/SourceFiles/purple/purple_sync_chat_carry_io.cpp',
    'worker': 'Telegram/SourceFiles/purple/purple_sync_chat_carry_executor.cpp',
    'store': 'Telegram/SourceFiles/purple/purple_sync_chat_store.cpp',
    'ui': 'Telegram/SourceFiles/purple/purple_sync_chat_setup_box.cpp',
    'cache': 'Telegram/SourceFiles/purple/purple_sync_chat_cache.cpp',
}
sources = {name: (repo / path).read_text() for name, path in paths.items()}
guards = [
    ('policy', 'bool DesktopSyncChatFollowFillActivated() { return false; }'),
    ('policy', 'DesktopSyncChatConsentFingerprint(fresh) == offer.fingerprint'),
    ('policy', 'offer.window != _window'),
    ('policy', 'port.foreground && port.foreground()'),
    ('policy', 'port.login() == selection.opening.scope.loginBinding'),
    ('policy', 'port.origin() == selection.opening.scope.origin'),
    ('policy', 'state.ticket != run'),
    ('policy', 'resolve(true,'),
    ('policy', 'engine->open(std::move(input), peer, selection.offer.stream,'),
    ('policy', 'self->engine.reset();'),
    ('policy', 'closed.exchange(true)'),
    ('policy', 'admitted->store(false)'),
    ('policy', 'port.carry(admitted,'),
    ('policy', 'port.pause(selection, admitted,'),
    ('service', 'if (!DesktopSyncChatFollowFillActivated()) { return false; }'),
    ('service', 'self->_session->account().loggingOut()'),
    ('service', 'crl::async([=] { ForgetDesktopSyncChatLogin(path, account, login); });'),
    ('worker', '_store->Begin(*start, consentPeer.has_value(), opening && opening->cleanupSettings)'),
    ('worker', '_io->AcceptReaders(*consentPeer, stream)'),
    ('worker', '_io && _io->CheckReaders(facts)'),
    ('worker', 'std::move(pending->consentPeer), std::move(pending->stream), std::move(pending->opening)'),
    ('worker', '!_io->CheckReaders(facts))'),
    ('worker', 'reader-authority-held'),
    ('io', 'DesktopSyncCarryReaderAuthority(previous, fresh.target.peer)'),
    ('io', 'known == QJsonValue(true)'),
    ('io', 'added && !_store._journal->deliveryAttempted && !_store._journal->markAttempted'),
    ('io', 'transport.insert(u"reader_warning"_q, true)'),
    ('io', 'u"settings_disclosed"_q, true'),
    ('cache', 'record.disclosedStreams'),
    ('cache', 'object.keys().size() != 5'),
    ('cache', 'parsed.record->loginBinding != login'),
    ('store', 'parsed.paused) { return fail(u"sync-paused-here"_q); }'),
    ('store', '!SyncStoreWorkerThread() || !current || !current->load()'),
    ('store', 'before.bytes == expected'),
    ('store', 'WriteSyncDurableFile(path, *bytes, before)'),
    ('ui', 'lng_purple_chat_fill_disclosure'),
    ('ui', 'lng_purple_chat_library_reader_warning'),
]

def validate(values):
    for name, token in guards:
        assert token in values[name], (name, token)
    for forbidden in ['MTPmessages_SendMessage', 'MTPchannels_CreateChannel', 'std::thread', 'QFile(']:
        assert forbidden not in values['policy'], forbidden
    assert not re.search(r'addButton\([^;]*lng_purple_chat_(?:follow_action|pause_here_action)', values['ui'], re.S)
    assert 'DesktopSyncChatConsentFlow' not in values['ui']
    assert 'DesktopSyncChatConsentFlow' not in values['service']
    assert 'Core::App' not in values['policy']

validate(sources)
for name, token in guards:
    changed = dict(sources)
    changed[name] = changed[name].replace(token, '')
    try:
        validate(changed)
    except AssertionError:
        continue
    raise AssertionError(('negative mutation escaped', name, token))
print(f'T5e consent routes: {len(guards)} production guards and {len(guards)} negative mutations PASS; activationOFF/no actionable UI')
