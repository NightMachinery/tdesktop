#!/usr/bin/env python3
import json
from pathlib import Path

root = Path(__file__).resolve().parent.parent
purple = root / 'Telegram/SourceFiles/purple'
files = {name: (purple / name).read_text() for name in [
    'purple_app_services.cpp', 'purple_sync.cpp', 'purple_sync_setup_box.cpp',
    'purple_sync_auto_service.cpp', 'purple_sync_chat_service.cpp']}

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
    require('typed-inventory-hold', 'if (command.chat != PeerToken{})' in service)
    require('tri-state-records', 'return GetSyncChatMessageOwnership(' in service)
    require('owned-retirement', 'Cancel(u"origin-retired"_q);' in service)
    require('run-generation', 'command.run != _run' in service)
    require('retry-snapshot', 'disk->stateBytes != self->_expectedStateBytes' in service)
    require('request-timeout', '_requestTimeout.callOnce(30 * 1000);' in service)
    require('no-posting', all(value not in service for value in ['MTPmessages_Send', 'MTPmessages_Edit', 'MTPchannels_Create', 'MTPmessages_Delete']))
    require('no-active-fallback', all(value not in service for value in ['activeSession(', 'activeAccount(', 'selectedAccount']))
    require('before-post-lock-proof', '_operation.directory' in service and '_operation.account' in service and '_operation.current()' in service)
    return failures

assert not inspect(files), inspect(files)
negative = [
    ('purple_sync_chat_service.cpp', 'disk->stateBytes != self->_expectedStateBytes', 'false', 'retry-snapshot'),
    ('purple_app_services.cpp', 'StartSyncChatDiscovery();', 'StartSyncChatDiscoveryLater();', 'startup-order'),
    ('purple_sync.cpp', 'EnsureSyncChatResolved(session);', 'Other(session);', 'startup-origin'),
    ('purple_sync_setup_box.cpp', 'EnsureSyncChatResolved(session, true);', 'Other(session);', 'sync-screen-origin'),
    ('purple_sync_auto_service.cpp', 'event != SyncAutoEvent::Disable', 'event != SyncAutoEvent::Enable', 'disable-exempt'),
    ('purple_sync_auto_service.cpp', 'Hold(SyncAutoReason::SyncChatNotResolved);', 'Stop();', 'auto-owned-hold'),
    ('purple_sync_chat_service.cpp', 'if (command.chat != PeerToken{})', 'if (false)', 'typed-inventory-hold'),
    ('purple_sync_chat_service.cpp', 'return GetSyncChatMessageOwnership(', 'return IsOwnSyncChatMessage(', 'tri-state-records'),
    ('purple_sync_chat_service.cpp', 'command.run != _run', 'false', 'run-generation'),
    ('purple_sync_chat_service.cpp', '_requestTimeout.callOnce(30 * 1000);', '', 'request-timeout'),
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
