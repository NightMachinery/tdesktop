#!/usr/bin/env python3
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
p = repo / 'Telegram/SourceFiles/purple'
rules = {
 'purple_sync_chat_setup_flow.cpp': [
  'std::make_unique<DesktopSyncChatCreateStore>',
  'if (!existing && _resumeOnly)',
  '!existing && !_store->Save(*journal)',
  'std::make_unique<DesktopSyncChatSetupExecutor>',
  'OpenDesktopSyncChatDisk(scope.root, _directory,',
  '_admitted, true)',
  '!disk->settingsBound', 'disk->settingsSpace.isEmpty()',
  'disk->cache->mark == record->mark',
  'SaveDesktopSyncChatDisk(disk, record, _admitted)',
  '_executor->Renew(command, proof, now,',
  '_admitted->store(false)',
  'status.provisioned = !_executor->journal().provisionSource.isEmpty()',
  '_store.reset();',
  'existing->provisionSource.isEmpty() != _selection.provisionOnly',
 ],
 'purple_sync_chat_setup_service.cpp': [
  'crl::object_on_queue<DesktopSyncChatSetupFlow>',
  'flow.renew(command, proof,',
  'ready(allowed && self && self->current())',
  'session->account().sessionChanges()',
  'session->lifetime().add(',
  '_admitted->exchange(false)',
  '_transport->cancel()',
  'flow.cancel()',
  'if (_last.resumeAbsent) { finishResumeLookup(); }',
  'void DesktopSyncChatSetupService::Impl::finishResumeLookup()',
  '_worker.reset()',
  '_last.phase != DesktopSyncChatCreatePhase::Limit',
 ],
 'purple_sync_chat_setup_transport.cpp': [
  'SyncChatSetupContextCurrent(session, _scope)',
  'generation == _generation',
  '++_generation',
  '_resumeReady = nullptr',
  'CheckSyncChatBeforePost(_session.get(),',
  'SyncChatSetupSnapshotOf(self->_session.get())',
  'self->_resumeProof.peers.push_back(*peer)',
  'self->renewPeer(generation, true)',
  'self->_resumeProof.inventory = std::move(inventory)',
  'self->_resumeProof.readback = response',
  '_renew(*_command, _resumeProof,',
  '_createdReceipt->first == _command->operation',
  'DesktopSyncChatCreationProof::MatchedRequest',
  'DecodeDesktopSyncChatSetupUpdates(_command->journal.updates)',
  'applyUpdates(*updates)',
  'DesktopSyncChatUploadReceipt(*target, updates, randomId)',
  'readMark(*id, true, generation)',
 ],
 'purple_sync_chat_setup_requests.cpp': [
  'MTPmessages_SendMessage::Flag::f_silent',
  'MTPmessages_SendMessage::Flag::f_no_webpage',
  'MTP_string(DesktopSyncChatMarkText())',
  '!journal.created',
  '*journal.chat != route.peer.chat',
  'route.peer.origin != command.origin',
  'MTPmessages_UpdatePinnedMessage::Flag::f_silent',
  'std::numeric_limits<int32_t>::max()',
  'MTP_inputFolderPeer(route.input, MTP_int(1))',
  'cursor == begin + words.size()',
  'u"PURPLE_EFFECT_RESUME_REFUSED"_q',
 ],
 'purple_sync_chat_create.cpp': [
  'SerializeDesktopSyncChatCreateJournal(command.journal) != bytes',
  'candidate.createAttempts - 1',
  'verifier.journal().chat != _journal.chat',
  'verifier.journal().randomId != _journal.randomId',
  'verifier.journal().markId != _journal.markId',
  'if (!commitCache || !commitCache(verifier.journal()))',
  'if (!Save(std::move(renewed)))',
 ],
}
checks = 0
for name, sentinels in rules.items():
 source = (p / name).read_text()
 for sentinel in sentinels:
  def validate(value):
   assert sentinel in value, name + ': ' + sentinel
  validate(source)
  try:
   validate(source.replace(sentinel, 'REMOVED'))
  except AssertionError:
   checks += 1
  else:
   raise AssertionError('negative source fixture failed')
transport = (p / 'purple_sync_chat_setup_transport.cpp').read_text()
assert transport.count('.resumeEffect(') == 3, 'create, mark and decoration variants need renewal'
assert '.handleAllErrors()' not in transport, 'all setup network calls use common managed sender'
assert 'SyncRequestSender _api;' in (p / 'purple_sync_chat_setup_transport.h').read_text()
for name in ['purple_sync_chat_setup_box.cpp', 'purple_sync_chat_setup_service.cpp']:
 source = (p / name).read_text()
 for forbidden in ['ReadSyncPrivateFile(', 'WriteSyncPrivateFile(', 'OpenDesktopSyncChatDisk(',
  'QLockFile', 'std::future', '.get_future(', '.wait(', 'SyncConfigPublish',
  'SyncConfigPost', 'InitializeSyncAccountLocally', 'DesktopSyncChatSetupExecutor']:
  assert forbidden not in source, name + ': GUI disk wait or first-post activation: ' + forbidden
ui = (p / 'purple_sync_chat_setup_box.cpp').read_text()
assert '}), true);' in ui, 'Resume must not create an intent if the journal is absent'
print(f'Q2A transport {checks} source guards and {checks} negative fixtures PASS; worker ownership, shared resume gates and first-post OFF')

flow = (p / 'purple_sync_chat_setup_flow.cpp').read_text()
service = (p / 'purple_sync_chat_setup_service.cpp').read_text()
ui = (p / 'purple_sync_chat_setup_box.cpp').read_text()
def body(source, marker):
 start = source.index(marker)
 return source[start:source.index('\n}', start)]
def ordered(text, *parts):
 offset = 0
 for part in parts:
  index = text.find(part, offset)
  assert index >= 0, part
  offset = index + len(part)
cancel = body(flow, 'void DesktopSyncChatSetupFlow::cancel()')
lookup = body(service, 'void DesktopSyncChatSetupService::Impl::finishResumeLookup()')
restore = body(ui, 'bool RestoreFreshSetup(')
scenarios = [
 (cancel, ['_admitted->store(false)', '_executor->Cancel()', '_store.reset()', 'publish()']),
 (lookup, ['_admitted->store(false)', '_transport->cancel()', '_worker.reset()']),
 (restore, ['state->serviceGeneration != generation', '++state->serviceGeneration', 'state->service.reset()', 'state->accept->setDisabled(!Current(*state))']),
]
for text, parts in scenarios:
 ordered(text, *parts)
 for part in parts:
  try:
   ordered(text.replace(part, 'REMOVED'), *parts)
  except AssertionError:
   pass
  else:
   raise AssertionError('lifecycle negative guard did not refuse')
assert '_store.reset();\n\t\tpublish(u"setup-no-journal"_q, true);' in flow
assert 'crl::on_main(box, [=] {\n\t\t\tif (RestoreFreshSetup(state, generation, message))' in ui
assert 'state->loading || !Current(*state)' in ui
print('Q2A lifecycle release/order, deferred chooser reset and generation negative guards PASS')

requests = (p / 'purple_sync_chat_setup_requests.cpp').read_text()
move_decoration = body(requests, 'auto DesktopSyncChatMoveDecorationRequest(')
for guard in ['!SerializeSyncCarryJournal(carry)', '!carry.stateCommitted', '!carry.cacheCompleted',
              'carry.stateHash != SyncCarryRecordHash(opening.stateImage)',
              'provision.provisionSource != DesktopSyncChatMoveFingerprint(opening)',
              'carry.loginAuthority != DesktopSyncCarryLogin(opening.scope.loginBinding)',
              'route.peer.origin != opening.scope.origin']:
 assert guard in move_decoration
 assert guard not in move_decoration.replace(guard, '')
for name in ['purple_sync_chat_setup_box.cpp', 'purple_sync_chat_service.cpp', 'purple_sync_chat_setup_service.cpp']:
 assert 'DesktopSyncChatMoveDecorationRequest(' not in (p / name).read_text(), 'held move must not dispatch decorations'
print('T5d committed-only newly created destination decoration guards and held no-callsite PASS')

for marker in ['DesktopSyncChatMoveDecorationAction::observe(', 'DesktopSyncChatMoveDecorationAction::retry(',
               'DesktopSyncChatMoveDecorationAction::complete(']:
 assert marker in requests
observe = body(requests, 'auto DesktopSyncChatMoveDecorationAction::observe(')
assert 'DesktopSyncChatMoveDecorationPhase::NotStarted' in observe
assert 'DesktopSyncChatMoveDecorationPhase::Ready' in observe
assert 'DesktopSyncChatMoveDecorationPhase::Waiting' in observe
completion = body(requests, 'void DesktopSyncChatMoveDecorationAction::complete(')
assert 'if (!okay)' in completion and 'DesktopSyncChatMoveDecorationPhase::Held' in completion
for name in ['purple_sync_chat_setup_box.cpp', 'purple_sync_chat_service.cpp', 'purple_sync_chat_setup_service.cpp']:
 assert 'DesktopSyncChatMoveDecorationAction(' not in (p / name).read_text(), 'qualification-held action must stay disconnected'
print('T5d decoration failed/waiting state and explicit retry route inventory PASS')
