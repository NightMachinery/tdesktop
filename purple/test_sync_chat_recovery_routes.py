#!/usr/bin/env python3
from pathlib import Path
import re
repo=Path(__file__).resolve().parent.parent
paths={'recovery':'Telegram/SourceFiles/purple/purple_sync_chat_recovery.cpp','ui':'Telegram/SourceFiles/purple/purple_sync_chat_setup_box.cpp','service':'Telegram/SourceFiles/purple/purple_sync_chat_service.cpp','store':'Telegram/SourceFiles/purple/purple_sync_chat_carry_store.cpp','io':'Telegram/SourceFiles/purple/purple_sync_chat_carry_io.cpp','discovery':'Telegram/SourceFiles/purple/purple_sync_chat_discovery.cpp'}
sources={name:(repo/path).read_text() for name,path in paths.items()}
guards=[
 ('recovery','bool DesktopSyncChatRecoveryActivated() { return false; }'),
 ('recovery','if (!DesktopSyncChatRecoveryActivated() || !DesktopSyncChatRecoveryHeld(snapshot).isEmpty())'),
 ('recovery','if (!DesktopSyncChatRecoveryActivated() || !disclosed || window.isEmpty()'),
 ('recovery','port.window() == selection.window'),
 ('recovery','port.login() == selection.opening.scope.loginBinding'),
 ('recovery','port.origin() == selection.opening.scope.origin'),
 ('recovery','state.ticket != run'),
 ('recovery','selection.opening.stateImage == fresh.stateImage'),
 ('recovery','selection.opening.cacheImage == fresh.cacheImage'),
 ('recovery','selection.opening.input.cacheRead == fresh.input.cacheRead'),
 ('recovery','state.resolve(true)'),
 ('recovery','journal->provisionSource != state.selection.fingerprint'),
 ('recovery','port.resumeProvision(std::move(done))'),
 ('recovery','SyncCarryMode::RepairMark : SyncCarryMode::Switch'),
 ('recovery','DesktopSyncCarryOpening{opening.stateImage, opening.cacheImage}'),
 ('recovery','admitted->store(false)'),
 ('recovery','self->engine.reset()'),
 ('recovery','sourceAccess = repair ? SyncCarrySourceAccess::Readable'),
 ('recovery','SyncChatDirectRead::ChatGone'),
 ('ui','if (!state->opening || !DesktopSyncChatRecoveryActivated()) { return; }'),
 ('ui','for (const auto choice : DesktopSyncChatRecoveryChoices(*state->opening))'),
 ('ui','ConfirmRecovery(box, state, choice)'),
 ('ui','BuildRecoveryConfirmation('),
 ('ui','state->opening->status.phase == DesktopSyncChatPhase::Use'),
 ('service','if (!DesktopSyncChatRecoveryActivated()) { return {}; }'),
 ('service','fresh->input.cacheRead != opening.input.cacheRead'),
 ('store','input.mode == SyncCarryMode::RepairMark'),
 ('store','object.objectHash != next.journal.stateHash'),
 ('store','*bytes != _stateFile.bytes || hash != _journal->stateHash'),
 ('store','const auto barrier = WitnessState(hash)'),
 ('io','fresh.resolutionInput.cacheRead == SyncChatDirectRead::MarkGone'),
 ('io','&& _store._journal->mode != SyncCarryMode::RepairMark'),
]
def validate(values):
 for name,guard in guards:assert guard in values[name],(name,guard)
 for forbidden in ['MTPmessages_SendMessage(', 'MTPchannels_CreateChannel(', 'QFile(', 'std::thread', '_api.request(']:assert forbidden not in values['recovery'],forbidden
 for control in ['recovery_create','recovery_choose','recovery_repair','recovery_retry']:
  assert 'lng_purple_chat_'+control in values['ui'],control
 assert 'if (!DesktopSyncChatRecoveryActivated())' in values['ui'][values['ui'].index('void BuildRecoveryConfirmation('):values['ui'].index('void ConfirmRecovery(')]
 assert 'ReadSyncPrivateFile(' not in values['recovery']
validate(sources)
for name,guard in guards:
 changed=dict(sources);changed[name]=changed[name].replace(guard,'')
 try:validate(changed)
 except AssertionError:continue
 raise AssertionError(('negative guard escaped',name,guard))
print(f'T5h recovery routes: {len(guards)} guards and {len(guards)} negative mutations PASS; four recovery rows and nested confirmation OFF; no direct transport or GUI IO')
