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
    'purple_sync_auto_transport.cpp', 'purple_sync_chat_write.cpp',
    'purple_sync_chat_carry_io.cpp', 'purple_sync_config_upload.cpp', 'purple_sync_config_post.cpp', 'purple_sync_config_publish.cpp', 'purple_sync_config_publish_flow.cpp', 'purple_account_config.cpp']}

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
    write = values['purple_sync_chat_write.cpp']
    upload = values['purple_sync_config_upload.cpp']
    post = values['purple_sync_config_post.cpp']
    publisher = values['purple_sync_config_publish.cpp']
    require('send-silent', 'MTP_flags(MTPmessages_SendMedia::Flag::f_silent), route.input' in write)
    require('edit-typed', 'route.input, MTP_int(id), MTP_string("#purplesync"), media' in write)
    require('receipt-random-map', 'uint64_t(data.vrandom_id().v) == randomId' in write)
    require('receipt-peer', 'DesktopSyncChatToken(data.vpeer_id(), route.peer.origin.account.userId)' in write)
    require('receipt-c0', 'GetSyncChatMessageOwnership(MapDesktopSyncChatMessage(' in write)
    require('parts-before-gate', '_offset == _content.size()' in upload and all(value in upload for value in ['ResumeEffect([self](bool ready)', 'if (ready) { self->DispatchMedia(); }', 'RenewBeforePost([self, ready = std::move(ready)]', 'CheckSyncChatBeforePost(']))
    require('final-route-compatibility', 'CompatibleDesktopSyncChatIo(*self->_route, lease->route)' in upload)
    require('final-lease-current', '!lease->current || !lease->current()' in upload)
    ordinary = upload.split('\n\tconst auto target = route();', 1)[1]
    require('updates-before-completion', ordinary.index('session->api().applyUpdates(result,') < ordinary.index('Finish(Current() && receipt'))
    carry_upload = upload.split('void SyncConfigUpload::OnMedia(', 1)[1].split('\n\tconst auto target = route();', 1)[0]
    carry_receipt = values['purple_sync_chat_carry_io.cpp'].split('std::optional<int32_t> DesktopSyncCarryUpdatesReceipt(', 1)[1].split('std::optional<MTPmessages_ForwardMessages>', 1)[0]
    require('carry-updates-before-completion', 'current->api().applyUpdates(updates, randomId);' in carry_upload
        and carry_upload.index('DesktopSyncCarryUpdatesReceipt(*_carry, result,') < carry_upload.index('Finish(Current() && receipt')
        and 'applyUpdates(updates, captured.randomId());' in carry_receipt
        and carry_receipt.index('applyUpdates(updates, captured.randomId());') < carry_receipt.index('return captured.current()'))
    require('upload-callback-lifetime', 'return !_done && session && !session->account().loggingOut()' in upload and 'SessionAccountKey(session) == _account' in upload)
    require('post-typed-readback', '_session, *_route, int32_t(_messageId.bare)' in post)
    require('post-exact-confirm', 'result.messageId == _messageId.bare && result.record == _staged' in post and 'result.fresh && result.owned' in post)
    require('post-cancel-stops-upload', 'void SyncConfigPost::Cancel()' in post and post.count('_upload->Cancel();') == 2)
    require('publisher-typed-ledger', 'DesktopSyncChatLedgerMatches(*state, _input.chat)' in values['purple_sync_config_publish_flow.cpp'])
    require('publisher-gui-no-files', all(marker not in publisher for marker in ['AccountSyncInspectionTarget(', 'AccountSyncTarget(', 'ValidateAccountSyncTarget(', 'ReadSettingsFileInput(', 'ReadSyncSettingsFile(', 'InspectSyncAccountRecovery(', 'SyncLocalStore(', '->StageConfig(', '->ConfirmConfigReadBack(']))
    confirmation = values['purple_sync_setup_box.cpp'].split('QString ConfirmText(', 1)[1].split('void RefreshButtons(', 1)[0]
    require('typed-settings-confirmation', 'Saved' not in confirmation and 'selected' in confirmation)
    finish = publisher.split('void SyncConfigPublish::Finish(', 1)[1]
    cancel = publisher.split('void SyncConfigPublish::Cancel()', 1)[1].split('void SyncConfigPublish::Finish(', 1)[0]
    require('publisher-terminal-before-child', '_post->Cancel()' not in cancel
        and finish.index('_done = true;') < finish.index('_post->Cancel();')
        < finish.index('_worker.reset();') < finish.index('std::move(_finished)'))
    router = values['purple_account_config.cpp']
    require('publisher-source-epoch', 'a.sourceGeneration == b.sourceGeneration' in router
        and '_routingGeneration, permissionsReady(), _generation' in router
        and 'SettingsChanges() | rpl::on_next(changed, _lifetime)' in publisher
        and 'AccountSyncRoutingCurrent(session, *_routing)' in publisher)
    require('publisher-worker-renewal', '_operation.authorize([self, ready = std::move(ready)]' in upload and 'self->Renew(std::move(ready));' in publisher)
    require('download-metadata-seal', 'if (self->_downloaded)' in read and 'documentId != self->_documentId || editDate != self->_editDate' in read and 'size != self->_expectedBytes' in read)
    require('auto-effect-self', '&& _store->local().state()->version == 1' in auto and '&& _store->local().state()->chat.kind == PeerTokenKind::Self' in auto)
    operation = auto.split('DesktopSyncChatOperation AutoSession::OperationFor(', 1)[1].split('void AutoSession::Send(', 1)[0]
    require('borrowed-proof-run', 'self->_store.get() == store && self->_sessionId == owner' in operation and 'store->Current()' in operation)
    require('borrowed-proof-observer-free', 'SyncChatSelfResolved' not in operation and 'Accept(' not in operation and 'CheckCompletion(' not in operation)
    require('auto-typed-self-inventory', 'lease->route, nullptr' in auto and 'DesktopSyncChatInventoryMatches(inventory, origin, PeerToken{})' in auto)
    return failures

assert not inspect(files), inspect(files)
negative = [
    ('purple_sync_config_upload.cpp', 'current->api().applyUpdates(updates, randomId);', 'IgnoreUpdates(updates);', 'carry-updates-before-completion'),
    ('purple_sync_chat_carry_io.cpp', 'applyUpdates(updates, captured.randomId());', 'IgnoreUpdates(updates);', 'carry-updates-before-completion'),
    ('purple_sync_config_publish.cpp', '_done = true;\n\t_admitted->store(false);\n\tif (_post) { _post->Cancel(); }', '_admitted->store(false);\n\tif (_post) { _post->Cancel(); }\n\t_done = true;', 'publisher-terminal-before-child'),
    ('purple_account_config.cpp', 'a.sourceGeneration == b.sourceGeneration', 'true', 'publisher-source-epoch'),
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
    ('purple_sync_chat_write.cpp', 'MTP_flags(MTPmessages_SendMedia::Flag::f_silent), route.input', 'MTP_flags(0), route.input', 'send-silent'),
    ('purple_sync_chat_write.cpp', 'route.input, MTP_int(id), MTP_string("#purplesync"), media', 'MTP_inputPeerSelf(), MTP_int(id), MTP_string("#purplesync"), media', 'edit-typed'),
    ('purple_sync_chat_write.cpp', 'uint64_t(data.vrandom_id().v) == randomId', 'true', 'receipt-random-map'),
    ('purple_sync_chat_write.cpp', 'DesktopSyncChatToken(data.vpeer_id(), route.peer.origin.account.userId)', 'PeerToken{}', 'receipt-peer'),
    ('purple_sync_chat_write.cpp', 'GetSyncChatMessageOwnership(MapDesktopSyncChatMessage(', 'Other(MapDesktopSyncChatMessage(', 'receipt-c0'),
    ('purple_sync_config_upload.cpp', '_offset == _content.size()', 'true', 'parts-before-gate'),
    ('purple_sync_config_upload.cpp', 'ResumeEffect([self](bool ready)', 'DispatchMediaWithoutGate([self](bool ready)', 'parts-before-gate'),
    ('purple_sync_config_upload.cpp', 'RenewBeforePost([self, ready = std::move(ready)]', 'SkipFacts([self, ready = std::move(ready)]', 'parts-before-gate'),
    ('purple_sync_config_upload.cpp', 'CompatibleDesktopSyncChatIo(*self->_route, lease->route)', 'true', 'final-route-compatibility'),
    ('purple_sync_config_upload.cpp', '!lease->current || !lease->current()', 'false', 'final-lease-current'),
    ('purple_sync_config_upload.cpp', 'session->api().applyUpdates(result,', 'Other(result,', 'updates-before-completion'),
    ('purple_sync_config_upload.cpp', 'SessionAccountKey(session) == _account', 'true', 'upload-callback-lifetime'),
    ('purple_sync_config_post.cpp', '_session, *_route, int32_t(_messageId.bare)', '_session, int32_t(_messageId.bare)', 'post-typed-readback'),
    ('purple_sync_config_post.cpp', 'result.messageId == _messageId.bare && result.record == _staged', 'true', 'post-exact-confirm'),
    ('purple_sync_config_post.cpp', '_upload->Cancel();', 'Other();', 'post-cancel-stops-upload'),
    ('purple_sync_config_publish_flow.cpp', 'DesktopSyncChatLedgerMatches(*state, _input.chat)', 'true', 'publisher-typed-ledger'),
    ('purple_sync_setup_box.cpp', "this account's selected ", "this account's Saved ", 'typed-settings-confirmation'),
    ('purple_sync_config_publish.cpp', 'void SyncConfigPublish::Start()', 'void SyncConfigPublish::Start() /* ReadSettingsFileInput( */', 'publisher-gui-no-files'),
    ('purple_sync_config_upload.cpp', '_operation.authorize([self, ready = std::move(ready)]', 'SkipDisk([self, ready = std::move(ready)]', 'publisher-worker-renewal'),
    ('purple_sync_candidate_reader.cpp', 'documentId != self->_documentId || editDate != self->_editDate', 'false', 'download-metadata-seal'),
    ('purple_sync_auto_service.cpp', '&& _store->local().state()->version == 1', '&& true', 'auto-effect-self'),
    ('purple_sync_auto_service.cpp', 'self->_store.get() == store && self->_sessionId == owner', 'true', 'borrowed-proof-run'),
    ('purple_sync_auto_service.cpp', 'store->Current() && store->local().state()', 'store->Current() && SyncChatSelfResolved(self->_session.get()) && store->local().state()', 'borrowed-proof-observer-free'),
    ('purple_sync_auto_service.cpp', 'DesktopSyncChatInventoryMatches(inventory, origin, PeerToken{})', 'true', 'auto-typed-self-inventory'),
]
for name, before, after, expected in negative:
    mutated = dict(files)
    assert before in mutated[name]
    mutated[name] = mutated[name].replace(before, after)
    try:
        rejected = inspect(mutated)
    except ValueError:
        rejected = ['startup-order', 'updates-before-completion']
    assert expected in rejected, (expected, rejected)
print(json.dumps({'routes': len(RouteNames), 'negative_controls': len(negative), 'failures': 0}))
