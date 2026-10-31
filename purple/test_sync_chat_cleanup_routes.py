#!/usr/bin/env python3
from pathlib import Path
repo=Path(__file__).resolve().parent.parent
root=repo/'Telegram/SourceFiles/purple'
paths={'cleanup':root/'purple_sync_chat_cleanup.cpp','worker':root/'purple_sync_chat_carry_executor.cpp','store':root/'purple_sync_chat_carry_store.cpp','ui':root/'purple_sync_chat_setup_box.cpp','decoration':root/'purple_sync_chat_setup_requests.cpp','service':root/'purple_sync_chat_carry_owner.cpp','consent':root/'purple_sync_chat_consent.cpp'}
sources={k:p.read_text() for k,p in paths.items()}
guards=[
 ('cleanup','bool DesktopSyncChatCleanupActivated() { return false; }'),
 ('cleanup','SyncCarryCleanupCandidates(*j.plan,inventory,playlist)'),
 ('cleanup','CheckSyncCarryDeletion(j,*message,inventory,playlist)'),
 ('cleanup','CheckSyncCarryOldMarkDeletion(j,readback,inventory,playlist)'),
 ('cleanup','|| !_store.WitnessCleanup()'),
 ('cleanup','readerGuard.CheckReaders(fresh)'),
 ('cleanup','read.owner != _identity'),
 ('cleanup','read.generation != _slot->load()'),
 ('cleanup','destination.readStarted.after(read.after)'),
 ('cleanup','!native->c_messageEmpty().vpeer_id()'),
 ('cleanup','if (!save(item,u"issued"_q,body))'),
 ('cleanup','_claimed->exchange(true)'),
 ('cleanup','_slot->compare_exchange_strong(expected,_generation+1)'),
 ('cleanup','!active->operationCurrent()'),
 ('cleanup','if (retired->exchange(true)) { return; }'),
 ('cleanup','delay(60000,'),
 ('cleanup','if (!completed->exchange(true))'),
 ('cleanup','next->request()'),
 ('cleanup','actual!=wire'),
 ('store','CleanupShape(result.value(u"cleanup"_q),journal)'),
 ('store','value.insert(u"version"_q,1); value.insert(u"entries"_q,entries)'),
 ('store','StateHash(_local->_state)!=_journal->nextStateHash'),
 ('store','Same(_cacheFile,CaptureSyncPrivateFile(_scope.cacheFile,64*1024))'),
 ('store','_envelope.insert(u"transport"_q,*transport); _envelope.insert(u"transport_sha256"_q,*hash);'),
 ('worker','if (start && opening &&'),
 ('worker','_rememberedJournal = progress.journal'),
 ('worker','if (!progress.journal) { progress.journal = _rememberedJournal; }'),
 ('worker','_skipCleanupRequested=true; open();'),
 ('worker','if (!_store->journal() || !_store->WitnessCleanup())'),
 ('worker','SyncCarryEventKind::SkipCleanup'),
 ('worker','accepted.deleted=*deleted; accepted.mark=_cleanup->oldMarkAbsent()'),
 ('worker','input.cleanup=SyncCarryCleanup::OwnLatePosts'),
 ('worker','journal.mode==SyncCarryMode::Switch && authority->value(u"cleanup_settings_authorized"_q)==QJsonValue(true)'),
 # A Follow's late-post cleanup needs the Cleanup switch as the owner admitted it, recorded by Begin.
 ('worker','journal.source!=PeerToken{} && authority->value(u"cleanup_settings_authorized"_q)==QJsonValue(true)'),
 ('service','port.cleanup = _host.activated(DesktopSyncChatCarrySwitch::Cleanup);'),
 # A run without a cleanup stage reports no pending cleanup, and the texts name removal only where it can happen.
 ('worker','progress.journal->plan->cleanup != SyncCarryCleanup::None) { progress.cleanup = DesktopSyncChatCleanupProgressOf('),
 ('ui','offer.action == SyncChatAction::Follow && offer.source != PeerToken{} && DesktopSyncChatCleanupActivated()'),
 ('ui','(OffersCleanup(*state) ? u" "_q + tr::lng_purple_chat_move_cleanup_disclosure(tr::now) : QString())'),
 ('ui','carry->journal->plan->cleanup != SyncCarryCleanup::None\n\t\t\t? tr::lng_purple_chat_move_complete(tr::now)'),
 ('consent','const auto cleanup = port.cleanup && input.mode == SyncCarryMode::Follow;'),
 ('consent','DesktopSyncCarryOpening{selection.opening.stateImage, selection.opening.cacheImage, cleanup}'),
 ('store','(input.mode!=SyncCarryMode::Switch && input.mode!=SyncCarryMode::Follow) || !input.operationConfirmed'),
 ('service','|| !DesktopSyncChatCarryCommandAllowed(command, activated)) { return false; }'),
 ('service','return !(command.cleanupPermit || command.cleanupRead) || (activated && activated(DesktopSyncChatCarrySwitch::Cleanup));'),
 ('service','!transport.applyCleanup(command.cleanupPermit->route(), *reply.cleanupResult)'),
 ('ui','if (!DesktopSyncChatCleanupActivated() || !opening.journal'),
 ('ui','if (!DesktopSyncChatCleanupActivated() || !owner'),
 ('ui','if (DesktopSyncChatCleanupActivated()) {\n\t\tstate->cleanup = box->addRow('),
 ('ui','tr::lng_purple_chat_cleanup_remove_mark(tr::now), false'),
 ('ui','fresh->journal->revision==opening.journal->revision'),
 ('ui','fresh->journal->loginAuthority==opening.journal->loginAuthority'),
 ('ui','if (allowed() && skip) { skip(true); }'),
 ('decoration','DesktopSyncChatMoveDecorationAction::cleanupProgress'),
 ('decoration','return receipt == _receipt'),
 ('store','_lastFailure = SyncStoreStatus::Ready;\n\treturn { SyncStoreStatus::Ready, next.status };'),
]
def valid(text):
 return all(fragment in text[name] for name,fragment in guards) and all(x not in text['cleanup'] for x in ['MTPchannels_DeleteChannel','MTPmessages_DeleteHistory','MTPchannels_DeleteHistory'])
assert valid(sources)
for name,fragment in guards:
 changed=dict(sources);changed[name]=changed[name].replace(fragment,'REMOVED_GUARD',1);assert not valid(changed),(name,fragment)
for method in ['MTPchannels_DeleteChannel','MTPmessages_DeleteHistory','MTPchannels_DeleteHistory']:
 changed=dict(sources);changed['cleanup']+=method;assert not valid(changed),method
print(f'T5g cleanup routes: {len(guards)} guards, {len(guards)} negative mutations, 3 whole-chat negatives PASS; activation OFF')
