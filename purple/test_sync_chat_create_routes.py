#!/usr/bin/env python3
from pathlib import Path
repo = Path(__file__).resolve().parent.parent
p = repo / 'Telegram/SourceFiles/purple'
rules = {
 'purple_sync_chat_create.cpp': [
  'step == DesktopSyncChatCreateStep::Create && _journal.pending',
  'step = DesktopSyncChatCreateStep::ReconcileCreate;',
  'reply.creationProof != DesktopSyncChatCreationProof::MatchedRequest',
  'MapDesktopSyncChatInventoryReply(_origin, reply.ticket,',
  'mapped.records != SyncChatRecordPresence::Empty',
  'SameDesktopSyncChatDisclosure(*_peer, reply.inventory->route.peer)',
  'RecognizedDesktopSyncChatRead(response, *_journal.chat)',
  'messages->size() == 1',
  'DesktopSyncChatCreateStep::ReconcileMark ? reply.markId : _journal.markId',
  'reply.ticket != _issued->ticket',
  '_journal.createAttempts >= 3',
  'now < _journal.retryAt',
  'object.size() != 22',
  'o.size() != 10',
  'next.decorationAfterCache = next.step;',
  'next.decorationAfterCache.reset();',
  '_inventoryReady=true;',
  '_next=DesktopSyncChatCreateStep::PeerInfo;',
  'offer.ticket!=_ticket',
  'SameDesktopSyncChatDisclosure(offer.peer,_offer->peer)',
  'next.newReaders=next.newReaders || NewDesktopSyncChatReaders(baseline,*reply.peer)',
  'result.readers.otherMembers=o.value(u"others"_q)',
  'SerializeDesktopSyncChatCreateJournal(j) == bytes',
 ],
 'purple_sync_chat_create_store.cpp': [
  '!Worker()',
  '_journal->operation != journal.operation',
  '_identity != SyncPathIdentity(_path)',
  'before != _bytes',
 ],
 'purple_sync_chat_create_requests.cpp': [
  'Flag::f_silent',
  '!j.created || j.chat->kind != PeerTokenKind::Channel',
  'std::numeric_limits<int32_t>::max()',
  'MTP_inputFolderPeer(route->input, MTP_int(1))',
 ],
}
count = 0
for file, guards in rules.items():
 source = (p / file).read_text()
 for sentinel in guards:
  def check(text):
   assert sentinel in text, file + ': ' + sentinel
  check(source)
  try:
   check(source.replace(sentinel, 'REMOVED_GUARD'))
  except AssertionError:
   count += 1
  else:
   raise AssertionError('negative inventory did not fail')
ui = (p / 'purple_sync_chat_setup_box.cpp').read_text()
for forbidden in ['DesktopSyncChatSetupExecutor', 'DesktopSyncChatSetupRequest(', 'MTPchannels_CreateChannel', '.send()']:
 assert forbidden not in ui, 'write activation in qualification-held UI'
for file in p.glob('*.cpp'):
 if file.name in ['purple_sync_chat_create.cpp', 'purple_sync_chat_create_requests.cpp']:
  continue
 source = file.read_text()
 assert 'DesktopSyncChatSetupExecutor' not in source, 'production executor callsite: ' + file.name
print(f'T3B routes {count} guards + {count} negative, no production write transport/UI executor PASS')
