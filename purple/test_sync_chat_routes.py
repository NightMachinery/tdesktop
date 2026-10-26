#!/usr/bin/env python3
import json
from pathlib import Path

root = Path(__file__).resolve().parent.parent
purple = root / 'Telegram/SourceFiles/purple'
files = {name: (purple / name).read_text() for name in [
    'purple_app_services.cpp', 'purple_sync.cpp', 'purple_sync_setup_box.cpp',
    'purple_sync_auto_service.cpp', 'purple_sync_chat_service.cpp',
    'purple_sync_history_scanner.cpp', 'purple_sync_candidate_reader.cpp',
    'purple_sync_account_inventory.cpp', 'purple_sync_local_store.cpp',
    'purple_sync_auto_transport.cpp']}

RouteNames = set()

def inspect(values):
    failures = []
    def require(name, condition):
        RouteNames.add(name)
        if not condition:
            failures.append(name)
    app = values['purple_app_services.cpp']
    require('startup-order', app.index('StartSyncChatDiscovery();') < app.index('StartAutomaticSettingsSync();'))
    require('startup-origin', 'EnsureSyncChatResolved(session);' in values['purple_sync.cpp'])
    require('sync-screen-origin', 'EnsureSyncChatResolved(session, true);' in values['purple_sync_setup_box.cpp'])
    auto = values['purple_sync_auto_service.cpp']
    require('auto-self-facts', 'facts.chat.kind == PeerTokenKind::Self && SyncChatSelfResolved(_session.get())' in auto)
    require('disable-exempt', 'event != SyncAutoEvent::Disable && !SyncChatSelfResolved(session)' in auto)
    require('auto-owned-hold', 'Hold(SyncAutoReason::SyncChatNotResolved);' in auto)
    require('auto-visible', 'if (!SyncChatSelfResolved(session)) { return tr::lng_purple_auto_chat(tr::now); }' in auto)
    service = values['purple_sync_chat_service.cpp']
    require('typed-inventory-route', 'ValidDesktopSyncChatIo(route)' in service and 'command.origin, command.ticket, command.chat, result' in service)
    require('tri-state-records', all('return GetSyncChatMessageOwnership(' in values[name] for name in ['purple_sync_history_scanner.cpp','purple_sync_candidate_reader.cpp']))
    require('owned-retirement', 'Cancel(u"origin-retired"_q);' in service)
    require('run-generation', 'command.run != _run' in service)
    require('retry-snapshot', 'disk->stateBytes != self->_expectedStateBytes' in service)
    require('request-timeout', '_requestTimeout.callOnce(30 * 1000);' in service)
    require('no-posting', all(value not in service for value in ['MTPmessages_Send', 'MTPmessages_Edit', 'MTPchannels_Create', 'MTPmessages_Delete']))
    require('no-active-fallback', all(value not in service for value in ['activeSession(', 'activeAccount(', 'selectedAccount']))
    require('before-post-lock-proof', '_operation.directory' in service and '_operation.account' in service and '_operation.current()' in service)
    require('inventory-current-pure', 'self->Current() && self->_model' in service and 'self->_run == command.run' in service)
    scan = values['purple_sync_history_scanner.cpp']
    read = values['purple_sync_candidate_reader.cpp']
    inventory = values['purple_sync_account_inventory.cpp']
    require('scan-typed-route', 'DesktopSyncChatHistoryRequest(*_route, _pages.offset())' in scan)
    require('scan-flood-refusal', 'handleAllErrors().send()' in scan)
    require('reader-exact-route', 'DesktopSyncChatRecordRequest(*_route, id)' in read)
    require('download-typed-origin', 'DesktopSyncChatFileOrigin(*_route, _candidateIds[_next])' in read)
    require('download-exact-size', 'loader->bytes().size() != _expectedBytes' in read and '_downloadFailed ||' in read)
    require('inventory-route-preserved', '_session, *_route, progress, finished, _current' in inventory and '_session, *_route, _result.scan.candidateIds, finished, _current' in inventory)
    require('ledger-chat-fence', values['purple_sync_local_store.cpp'].count('DesktopSyncChatLedgerMatches(*_state, chat)') == 2)
    require('readback-typed-route', 'DesktopSyncChatRecordRequest(*_route, _id)' in values['purple_sync_auto_transport.cpp'])
    return failures

assert not inspect(files), inspect(files)
negative = [
    ('purple_sync_chat_service.cpp', 'disk->stateBytes != self->_expectedStateBytes', 'false', 'retry-snapshot'),
    ('purple_app_services.cpp', 'StartSyncChatDiscovery();', 'StartSyncChatDiscoveryLater();', 'startup-order'),
    ('purple_sync.cpp', 'EnsureSyncChatResolved(session);', 'Other(session);', 'startup-origin'),
    ('purple_sync_setup_box.cpp', 'EnsureSyncChatResolved(session, true);', 'Other(session);', 'sync-screen-origin'),
    ('purple_sync_auto_service.cpp', 'event != SyncAutoEvent::Disable', 'event != SyncAutoEvent::Enable', 'disable-exempt'),
    ('purple_sync_auto_service.cpp', 'Hold(SyncAutoReason::SyncChatNotResolved);', 'Stop();', 'auto-owned-hold'),
    ('purple_sync_chat_service.cpp', 'ValidDesktopSyncChatIo(route)', 'true', 'typed-inventory-route'),
    ('purple_sync_history_scanner.cpp', 'return GetSyncChatMessageOwnership(', 'return IsOwnSyncChatMessage(', 'tri-state-records'),
    ('purple_sync_chat_service.cpp', 'command.run != _run', 'false', 'run-generation'),
    ('purple_sync_chat_service.cpp', '_requestTimeout.callOnce(30 * 1000);', '', 'request-timeout'),
    ('purple_sync_chat_service.cpp', 'self->Current() && self->_model', 'true && self->_model', 'inventory-current-pure'),
    ('purple_sync_history_scanner.cpp', 'DesktopSyncChatHistoryRequest(*_route, _pages.offset())', 'Other()', 'scan-typed-route'),
    ('purple_sync_history_scanner.cpp', 'handleAllErrors().send()', 'send()', 'scan-flood-refusal'),
    ('purple_sync_candidate_reader.cpp', 'DesktopSyncChatRecordRequest(*_route, id)', 'Other()', 'reader-exact-route'),
    ('purple_sync_candidate_reader.cpp', 'DesktopSyncChatFileOrigin(*_route, _candidateIds[_next])', 'Other()', 'download-typed-origin'),
    ('purple_sync_candidate_reader.cpp', 'loader->bytes().size() != _expectedBytes', 'false', 'download-exact-size'),
    ('purple_sync_account_inventory.cpp', '_session, *_route, _result.scan.candidateIds, finished, _current', '_session, _result.scan.candidateIds, finished', 'inventory-route-preserved'),
    ('purple_sync_local_store.cpp', 'DesktopSyncChatLedgerMatches(*_state, chat)', 'true', 'ledger-chat-fence'),
    ('purple_sync_auto_transport.cpp', 'DesktopSyncChatRecordRequest(*_route, _id)', 'Other()', 'readback-typed-route'),
]
for name, before, after, expected in negative:
    mutated = dict(files)
    assert before in mutated[name]
    mutated[name] = mutated[name].replace(before, after)
    try:
        rejected = inspect(mutated)
    except ValueError:
        rejected = ['startup-order']
    assert expected in rejected, (expected, rejected)
print(json.dumps({'routes': len(RouteNames), 'negative_controls': len(negative), 'failures': 0}))
