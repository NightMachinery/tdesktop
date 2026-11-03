#!/usr/bin/env python3
import json
import re
from pathlib import Path

root = Path(__file__).resolve().parent.parent
purple = root / 'Telegram/SourceFiles/purple'
files = {name: (purple / name).read_text() for name in [
    'purple_app_services.cpp', 'purple_sync.cpp', 'purple_sync_setup_box.cpp',
    'purple_sync_auto_service.cpp', 'purple_sync_chat_service.cpp',
    'purple_sync_history_scanner.cpp', 'purple_sync_candidate_reader.cpp',
    'purple_sync_account_inventory.cpp', 'purple_sync_local_store.cpp',
    'purple_sync_auto_transport.cpp', 'purple_sync_chat_write.cpp',
    'purple_sync_chat_carry_io.cpp', 'purple_sync_config_upload.cpp', 'purple_sync_config_post.cpp', 'purple_sync_config_publish.cpp', 'purple_sync_config_publish_flow.cpp', 'purple_account_config.cpp',
    'purple_sync_magic_service.cpp', 'purple_sync_magic_box.cpp', 'purple_sync_magic_telegram.cpp', 'purple_archive_service.cpp', 'purple_sync_config_review.cpp']}

RouteNames = set()

def inspect(values):
    failures = []
    def require(name, condition):
        RouteNames.add(name)
        if not condition:
            failures.append(name)
    app = values['purple_app_services.cpp']
    require('startup-order', app.index('StartSyncChatDiscovery();') < app.index('StartAutomaticSettingsSync();'))
    require('startup-origin', 'SyncMagicLookup(session, SyncMagicTrigger::Startup, false);' in values['purple_sync_chat_service.cpp'])
    discovery = values['purple_sync_chat_service.cpp'].split('void StartSyncChatDiscovery() {', 1)[-1]
    require('startup-stored-sessions', 'domain().activeSessionChanges() | rpl::on_next([=] { refresh(); }, lifetime);' in discovery)
    autostart = values['purple_sync_auto_service.cpp'].split('void StartAutomaticSettingsSync() {', 1)[-1].split('void TriggerAutomaticSettingsSync(', 1)[0]
    require('auto-stored-sessions', 'domain().activeSessionChanges() | rpl::on_next([=] { refresh(); }, lifetime);' in autostart)
    archive = values['purple_archive_service.cpp'].split('void StartArchiveServices() {', 1)[-1].split('void RemoveArchiveAtLogout(', 1)[0]
    require('archive-stored-sessions', 'domain().activeSessionChanges() | rpl::on_next([=] { refresh(); }, lifetime);' in archive
        and archive.index('accountsChanges()') < archive.index('activeSessionChanges()') < archive.index('\trefresh();'))
    require('sync-screen-origin', 'EnsureSyncChatResolved(session, true);' in values['purple_sync_setup_box.cpp'])
    magic = values['purple_sync_magic_service.cpp']
    toast = values['purple_sync_magic_box.cpp'].split('void ToastSyncMagicProblem(', 1)[1].split('void SyncMagicChatBox(', 1)[0]
    require('magic-problem-toast', 'last.kind == SyncMagicResult::Kind::Problem && last.show' in magic
        and 'ToastSyncMagicProblem(session);' in magic
        and 'chat->shown();' in toast and toast.index('chat->shown();') < toast.index('showToast(')
        and 'SyncMagicChatBox' in toast)
    telegram = values['purple_sync_magic_telegram.cpp']
    lists = telegram.split('void loadLists(std::optional<PeerToken> bound,', 1)[-1].split('void fullInfo(', 1)[0]
    require('magic-lists-early', 'bound && data.channelLoaded(ChannelId(bound->id)) != nullptr' in lists
        and 'session->data().chatsListChanges(),' in lists and 'SyncMagicListsStepOf(' in lists
        and 'kListsTimeoutMs' in lists)
    # Every request goes through upstream's generated layer: nothing Purple serializes by hand.
    require('no-hand-rolled-tl', all('mtpTypeId(' not in text and 'getAllChats' not in text and '875f74be' not in text
        for text in values.values()))
    local = telegram.split('std::function<void(std::optional<SyncMagicLocal>)> done) override {', 1)[-1].split('void loadLists(', 1)[0]
    require('magic-lists-unfiltered', 'data->enumerateGroups(' in telegram and 'AddJoined(&data, result.visible);' in local
        and 'chatsList(' not in local.split('result.mainLoaded', 1)[0]
        and 'result.archiveLoaded = archive ? data.chatsListLoaded(archive) : result.mainLoaded;' in local)
    box = values['purple_sync_magic_box.cpp']
    require('magic-wait-wording', 'return frame.telegram' in box and 'tr::lng_purple_magic_retrying(' in box
        and 'case SyncChatProblem::RequestRefused:' in box)
    auto = values['purple_sync_auto_service.cpp']
    require('auto-bound-facts', 'facts.chatResolved = Resolved();' in auto and 'facts.chatProblem = SyncMagicPaused(' in auto
        and 'bound && *bound == StateChat()' in auto)
    require('disable-exempt', 'event != SyncAutoEvent::Disable && !SyncMagicBound(session) && !SyncMagicPaused(session)' in auto)
    require('auto-owned-hold', 'Hold(SyncAutoReason::SyncChatNotResolved);' in auto)
    require('auto-visible', 'if (!SyncMagicBound(session)) { return tr::lng_purple_auto_chat(tr::now); }' in auto)
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
    started = inventory.split('void SyncAccountInventory::Start()', 1)[1].split('void SyncAccountInventory::Cancel()', 1)[0]
    require('inventory-read-start', '_readStarted = DesktopSyncChatReadStamp::Begin();' in started
        and started.index('_readStarted = DesktopSyncChatReadStamp::Begin();') < started.index('_scanner->Start();'))
    require('inventory-read-stamp', 'std::move(_receipts), _readStarted' in inventory)
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
    flow = values['purple_sync_config_publish_flow.cpp']
    one_space = flow.split('bool OneSyncSpace(', 1)[1].split('bool CreatedBySetup(', 1)[0]
    created = flow.split('bool CreatedBySetup(', 1)[1].split('SyncConfigPublishFlow::SyncConfigPublishFlow(', 1)[0]
    require('unbound-join-admission', 'record.status == SyncCandidateStatus::Valid' in one_space
        and 'record.header->space == *space' in one_space and '!inventory.read->records.empty()' in one_space
        and 'if (identity == "missing") { return false; }' in created and 'return !journal || journal->created' in created
        and '(journal->choice == DesktopSyncChatSetupChoice::Create && !journal->adopted)' in created
        and 'const auto joinable = empty || (complete && OneSyncSpace(inventory)' in flow
        and '&& !CreatedBySetup(_setupIdentity, _setupBytes, _input.setup, _input.account));' in flow
        and '|| (_input.chat != PeerToken{} && !joinable && !magicJoinable)' in flow and '(_input.chat != PeerToken{} && !empty)' not in flow)
    magic_bound = flow.split('bool MagicBound(', 1)[1].split('SyncConfigPublishFlow::SyncConfigPublishFlow(', 1)[0]
    require('magic-publish-authority', 'binding->chat == chat && binding->problem == SyncChatProblem::None' in magic_bound
        and '(chat == PeerToken{} || binding->verifiedAt > 0)' in magic_bound
        and 'if (!setup && MagicBound(_target->files.syncDirectory, _input.chat)) {' in flow
        and 'const auto magicJoinable = _magic && complete && SelectedSyncSpace(inventory);' in flow
        and '} else if (_magic && !MagicBound(_target->files.syncDirectory, _input.chat)) {' in flow)
    selected = flow.split('bool SelectedSyncSpace(', 1)[-1].split('bool CreatedBySetup(', 1)[0]
    require('magic-join-selected-space', 'return record.status == SyncCandidateStatus::Valid && record.header;' in selected
        and 'record.header && record.header->space == *space' in selected and 'inventory.directory.selectedSpace' in selected)
    check = values['purple_sync_setup_box.cpp'].split('void StartCheck(', 1)[1]
    require('magic-manual-check', 'MoveSyncStateToBoundChat(session);' in check
        and '!lease->current() || !setup' not in check and 'state->setup = SyncChatSetupSnapshotOf(session);' in check)
    require('magic-move-unbound-auto', 'if (session && SyncMagicBound(session)) {\n\t\t\tFor(session).MoveToBound();' in auto and 'MoveStore();' in auto)
    require('adopted-setup-join', '|| (!journal->adopted && (journal->markId != setup->cache->mark.markId' in flow
        and '|| journal->markDate != setup->cache->mark.markDate))' in flow and '|| setup->cache->mark.chat != _input.chat) {' in flow)
    box = values['purple_sync_setup_box.cpp']
    joins = box.split('bool JoinsTypedChat(', 1)[1].split('bool UnboundTypedChoice(', 1)[0]
    require('typed-join-route', 'state.route->peer.chat != PeerToken{}' in joins and '!state.review->bound' in joins
        and 'ConfigSyncVerdict::Empty' in joins and 'if (JoinsTypedChat(state)) {' in box
        and '|| JoinsTypedChat(*state))) {' in box and '(action == Action::Join && JoinsTypedChat(*state))' in box)
    def guarded(start, guard):
        # The guard sits in the function, before its call that applies settings.
        body = box.split(start, 1)[1] if start in box else ''
        return guard in body and 'ApplySyncConfigChoice(' in body and body.find(guard) < body.find('ApplySyncConfigChoice(')
    require('unbound-typed-choice-guard', box.count('ApplySyncConfigChoice(') == 3
        and guarded('void RunAction(', 'if (UnboundTypedChoice(*state, *state->review)) {')
        and guarded('void ApplyChoice(', 'if (UnboundTypedChoice(*state, review)) {'))
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
    require('auto-effect-bound', '_store && _session && Current() && Resolved()' in auto)
    operation = auto.split('DesktopSyncChatOperation AutoSession::OperationFor(', 1)[1].split('void AutoSession::Send(', 1)[0]
    require('borrowed-proof-run', 'self->_store.get() == store && self->_sessionId == owner' in operation and 'store->Current()' in operation)
    require('borrowed-proof-observer-free', 'SyncChatSelfResolved' not in operation and 'Accept(' not in operation and 'CheckCompletion(' not in operation)
    require('auto-typed-inventory', 'lease->route, nullptr' in auto and 'DesktopSyncChatInventoryMatches(inventory, origin, chat)' in auto)
    pause = auto.split('bool AutoSession::PauseOnForeignSpace(', 1)[-1].split('void AutoSession::RetireCarry(', 1)[0]
    require('auto-space-first-use', 'if (self->PauseOnForeignSpace(chat)) { return; }' in auto
        and 'ReviewSyncFirstUse(_store->files(), _inventory)' in pause
        and 'if ((first.need != SyncFirstUseNeed::OtherSpace && first.need != SyncFirstUseNeed::Unjoined) || !first.action) {' in pause
        and 'self->FirstUse(std::move(inventory), SyncFirstUseChoice::None, nullptr, false);' in pause
        and 'raise(SyncChatProblem::SpaceMismatch)' not in auto)
    opened = auto.split('bool AutoSession::Open() {', 1)[-1].split('void AutoSession::MoveStore()', 1)[0]
    retire = auto.split('void AutoSession::RetireCarry() {', 1)[-1].split('bool AutoSession::NoStateEvent(', 1)[0]
    require('first-use-carry-retire', 'RetireCarry();\n\tconst auto recovery = InspectSyncAccountRecovery(*session);' in opened
        and '!SyncMagicBound(session) || DesktopSyncCarryGateFor(_account)->postingHeld()' in retire
        and 'RetireSyncCarryLeftovers(target->files.syncDirectory, date)' in retire)
    unbound = auto.split('bool AutoSession::NoStateEvent(', 1)[-1].split('void AutoSession::FirstUse(', 1)[0]
    require('auto-first-use-unbound', 'if (!Open()) { Stop(); return NoStateEvent(event); }' in auto
        and 'recovery->journalPresent || recovery->plan.status != SyncAccountRecoveryStatus::Setup' in unbound
        and 'FirstUseDue(target->files, event)) {\n\t\tFirstUse(std::nullopt, SyncFirstUseChoice::None, nullptr, false);' in unbound)
    due = auto.split('bool AutoSession::FirstUseDue(', 1)[-1].split('void AutoSession::FirstUse(', 1)[0]
    require('first-use-lifecycle-limit', '} else if (event != SyncAutoEvent::Disable && FirstUseDue(target->files, event)) {' in unbound
        and 'now - mark->checkedAt >= kSyncAutoCheckSeconds' in due
        and 'if (event == SyncAutoEvent::Enable || _firstBypass) {' in due
        and 'event == SyncAutoEvent::LocalSaved && _save.isActive()' in due
        and 'mark->fingerprint != ReadSyncSettingsFile(files).fingerprint' in due
        and 'For(session).ChatChanged();' in auto
        and 'mark->checkedAt = int64_t(base::unixtime::now());' in auto)
    run = auto.split('void AutoSession::FirstUseRun(', 1)[-1].split('void AutoSession::FirstUseJoined(', 1)[0]
    require('first-use-journal-guard', 'if (!recovery || recovery->journalPresent) {\n\t\tFirstUseFail(recovery ? u"recovery_journal"_q : u"unreadable"_q);' in run
        and run.index('recovery->journalPresent') < run.index('ReviewSyncFirstUse(')
        and 'fresh && !fresh->journalPresent' in run and run.index('Stop();') < run.index('StartAgainSyncAccount(')
        and 'StartAgainSyncAccount(*session, *fresh, true, false, true)' in run)
    finish = auto.split('void AutoSession::FirstUseFinish(', 1)[-1].split('void AutoSession::ReadNext(', 1)[0]
    require('first-use-mark', 'if (!WriteSyncFirstUseMark(target->files, mark)) {' in run
        and run.index('WriteSyncFirstUseMark(') < run.index('StartAgainSyncAccount(')
        and run.index('WriteSyncFirstUseMark(') < run.index('std::make_unique<SyncConfigPublish>(')
        and 'mark.choice = review.input.choice;' in run
        and '} else if (review.need == SyncFirstUseNeed::Unjoined) {\n\t\tFirstUseJoined(inventory, review);' in run
        and 'ClearSyncFirstUseMark(target->files);' in finish
        and 'if (first.settled) {\n\t\tClearSyncFirstUseMark(_store->files());' in pause)
    failures_text = auto.split('QString FirstUseFailureText(', 1)[-1].split('QString FirstUsePublishReason(', 1)[0]
    reasons = set(word for line in auto.split('\n') if 'FirstUseFail(' in line and 'void ' not in line
        for word in re.findall(r'u"(\w+)"_q', line.split('FirstUseFail(', 1)[1])) | set(re.findall(r'return u"(\w+)"_q;', auto.split('QString FirstUsePublishReason(', 1)[-1].split('class AutoSession', 1)[0]))
    require('first-use-named-failures', auto.count('FirstUseFinish(SyncFirstUseOutcome::Failed') == 2
        and 'SyncDiagReason(reason) });' in auto and 'code.isEmpty() ? u"unknown"_q : code' in run
        and len(reasons) >= 12 and all(f'u"{reason}"_q' in failures_text for reason in reasons if reason != 'apply_refused'))
    first_review = values['purple_sync_config_review.cpp'].split('SyncFirstUseReview ReviewSyncFirstUse(', 1)[-1].split('SyncConfigReview ReviewSyncConfig(', 1)[0]
    require('first-use-core-head', 'result.headKey = input->headKey;' in first_review
        and '? std::vector<QString>{ *input->headKey }' in first_review
        and 'ExtractSyncConfigHeads(' not in first_review and 'heads.size()' not in first_review)
    require('first-use-actions', 'publish ? review.request : SyncConfigPublishRequest()' in run
        and '(action == SyncFirstUseAction::JoinSilently && !review.sameBytes)' in run
        and 'self->FirstUseJoined(inventory, review);' in run
        and all(marker not in run for marker in ['ConfirmAction(', 'showBox(', 'Ui::show(', 'uiShow(']))
    box = values['purple_sync_setup_box.cpp']
    first = box.split('void RunFirstUse(', 1)[-1].split('void OnInventory(', 1)[0]
    require('first-use-box', 'if (followup == CheckFollowup::None && FirstUse(box, state, result)) {' in box
        and box.index('FirstUse(box, state, result)') < box.index('auto recovery = InspectSyncAccountRecovery(*session, &result);')
        and 'RunFirstUse(box, state, SyncFirstUseChoice::Synced);' in box
        and 'RunFirstUse(box, state, SyncFirstUseChoice::ThisDevice);' in box
        and 'if (*first.action == SyncFirstUseAction::Ask) {\n\t\tstate->asking = true;' in first
        and all(marker not in first for marker in ['ConfirmAction(', 'showBox(', 'Ui::show(', 'uiShow(']))
    require('first-use-auto-status', 'first->outcome == SyncFirstUseOutcome::Asking) { return tr::lng_purple_auto_first_ask(tr::now); }' in auto
        and 'first->outcome == SyncFirstUseOutcome::Waiting) { return tr::lng_purple_auto_first_wait(tr::now); }' in auto)
    return failures

assert not inspect(files), inspect(files)
negative = [
    ('purple_sync_account_inventory.cpp', '_readStarted = DesktopSyncChatReadStamp::Begin();', '', 'inventory-read-start'),
    ('purple_sync_account_inventory.cpp', 'std::move(_receipts), _readStarted', 'std::move(_receipts), {}', 'inventory-read-stamp'),
    ('purple_sync_config_upload.cpp', 'current->api().applyUpdates(updates, randomId);', 'IgnoreUpdates(updates);', 'carry-updates-before-completion'),
    ('purple_sync_chat_carry_io.cpp', 'applyUpdates(updates, captured.randomId());', 'IgnoreUpdates(updates);', 'carry-updates-before-completion'),
    ('purple_sync_config_publish.cpp', '_done = true;\n\t_admitted->store(false);\n\tif (_post) { _post->Cancel(); }', '_admitted->store(false);\n\tif (_post) { _post->Cancel(); }\n\t_done = true;', 'publisher-terminal-before-child'),
    ('purple_account_config.cpp', 'a.sourceGeneration == b.sourceGeneration', 'true', 'publisher-source-epoch'),
    ('purple_sync_chat_service.cpp', 'disk->stateBytes != self->_expectedStateBytes', 'false', 'retry-snapshot'),
    ('purple_app_services.cpp', 'StartSyncChatDiscovery();', 'StartSyncChatDiscoveryLater();', 'startup-order'),
    ('purple_sync_chat_service.cpp', 'SyncMagicLookup(session, SyncMagicTrigger::Startup, false);', 'Other(session);', 'startup-origin'),
    ('purple_sync_chat_service.cpp', '\tCore::App().domain().activeSessionChanges() | rpl::on_next([=] { refresh(); }, lifetime);\n', '', 'startup-stored-sessions'),
    ('purple_sync_auto_service.cpp', '\tCore::App().domain().activeSessionChanges() | rpl::on_next([=] { refresh(); }, lifetime);\n', '', 'auto-stored-sessions'),
    ('purple_archive_service.cpp', '\tCore::App().domain().activeSessionChanges() | rpl::on_next([=] { refresh(); }, lifetime);\n', '', 'archive-stored-sessions'),
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
    ('purple_sync_config_publish_flow.cpp', 'record.status == SyncCandidateStatus::Valid', 'true', 'unbound-join-admission'),
    ('purple_sync_config_publish_flow.cpp', 'record.header->space == *space', 'true', 'unbound-join-admission'),
    ('purple_sync_config_publish_flow.cpp', 'return !journal || journal->created', 'return !journal', 'unbound-join-admission'),
    ('purple_sync_config_publish_flow.cpp', '&& !CreatedBySetup(_setupIdentity, _setupBytes, _input.setup, _input.account));', ');', 'unbound-join-admission'),
    ('purple_sync_config_publish_flow.cpp', '(!journal->adopted && (', '(true && (', 'adopted-setup-join'),
    ('purple_sync_setup_box.cpp', '|| JoinsTypedChat(*state))) {', ')) {', 'typed-join-route'),
    ('purple_sync_setup_box.cpp', 'state.route->peer.chat != PeerToken{}\n', 'true\n', 'typed-join-route'),
    ('purple_sync_setup_box.cpp', 'if (UnboundTypedChoice(*state, *state->review)) {', 'if (false) {', 'unbound-typed-choice-guard'),
    ('purple_sync_setup_box.cpp', 'if (UnboundTypedChoice(*state, review)) {', 'if (false) {', 'unbound-typed-choice-guard'),
    ('purple_sync_config_publish.cpp', 'void SyncConfigPublish::Start()', 'void SyncConfigPublish::Start() /* ReadSettingsFileInput( */', 'publisher-gui-no-files'),
    ('purple_sync_config_upload.cpp', '_operation.authorize([self, ready = std::move(ready)]', 'SkipDisk([self, ready = std::move(ready)]', 'publisher-worker-renewal'),
    ('purple_sync_candidate_reader.cpp', 'documentId != self->_documentId || editDate != self->_editDate', 'false', 'download-metadata-seal'),
    ('purple_sync_auto_service.cpp', '_store && _session && Current() && Resolved()', '_store && _session && Current()', 'auto-effect-bound'),
    ('purple_sync_auto_service.cpp', 'facts.chatProblem = SyncMagicPaused(', 'facts.chatWritable = SyncMagicPaused(', 'auto-bound-facts'),
    ('purple_sync_auto_service.cpp', 'if (self->PauseOnForeignSpace(chat)) { return; }', '', 'auto-space-first-use'),
    ('purple_sync_auto_service.cpp', 'if ((first.need != SyncFirstUseNeed::OtherSpace && first.need != SyncFirstUseNeed::Unjoined) || !first.action) {', 'if (!SyncMagicSpaceMismatch(_inventory.directory, _store->local().state()->space)) {', 'auto-space-first-use'),
    ('purple_sync_auto_service.cpp', 'mark.choice = review.input.choice;', '', 'first-use-mark'),
    ('purple_sync_config_review.cpp', 'result.headKey = input->headKey;', 'const auto heads = ExtractSyncConfigHeads(inventory, *selected, QString());\n\tif (heads.heads.size() == 1) { result.headKey = heads.heads.front().head.key; }', 'first-use-core-head'),
    ('purple_sync_auto_service.cpp', 'FirstUseFail(u"archive_refused"_q);', 'FirstUseFinish(SyncFirstUseOutcome::Failed);', 'first-use-named-failures'),
    ('purple_sync_auto_service.cpp', ': (reason == u"archive_refused"_q) ? tr::lng_purple_sync_first_failed_archive(tr::now)\n', '', 'first-use-named-failures'),
    ('purple_sync_auto_service.cpp', '\t\tSyncDiagReason(reason) });', '\t\tSyncDiagCode(u"step"_q, u"failed"_q) });', 'first-use-named-failures'),
    ('purple_sync_auto_service.cpp', 'event != SyncAutoEvent::Disable && FirstUseDue(target->files, event)) {', 'event != SyncAutoEvent::Disable) {', 'first-use-lifecycle-limit'),
    ('purple_sync_auto_service.cpp', 'now - mark->checkedAt >= kSyncAutoCheckSeconds', 'true', 'first-use-lifecycle-limit'),
    ('purple_sync_auto_service.cpp', 'For(session).ChatChanged();', 'For(session).Event(SyncAutoEvent::SessionReady);', 'first-use-lifecycle-limit'),
    ('purple_sync_auto_service.cpp', 'mark->checkedAt = int64_t(base::unixtime::now());', 'mark->checkedAt = 0;', 'first-use-lifecycle-limit'),
    ('purple_sync_auto_service.cpp', '} else if (review.need == SyncFirstUseNeed::Unjoined) {\n\t\tFirstUseJoined(inventory, review);', '} else if (false) {\n\t\tFirstUseJoined(inventory, review);', 'first-use-mark'),
    ('purple_sync_auto_service.cpp', 'if (!WriteSyncFirstUseMark(target->files, mark)) {', 'if (false) {', 'first-use-mark'),
    ('purple_sync_auto_service.cpp', '\tRetireCarry();\n\tconst auto recovery = InspectSyncAccountRecovery(*session);', '\tconst auto recovery = InspectSyncAccountRecovery(*session);', 'first-use-carry-retire'),
    ('purple_sync_auto_service.cpp', '!SyncMagicBound(session) || DesktopSyncCarryGateFor(_account)->postingHeld()', '!SyncMagicBound(session)', 'first-use-carry-retire'),
    ('purple_sync_auto_service.cpp', 'if (!Open()) { Stop(); return NoStateEvent(event); }', 'if (!Open()) { Stop(); return false; }', 'auto-first-use-unbound'),
    ('purple_sync_auto_service.cpp', 'recovery->journalPresent || recovery->plan.status != SyncAccountRecoveryStatus::Setup', 'recovery->plan.status != SyncAccountRecoveryStatus::Setup', 'auto-first-use-unbound'),
    ('purple_sync_auto_service.cpp', 'if (!recovery || recovery->journalPresent) {\n\t\tFirstUseFail(', 'if (!recovery) {\n\t\tFirstUseFail(', 'first-use-journal-guard'),
    ('purple_sync_auto_service.cpp', 'fresh && !fresh->journalPresent', 'fresh', 'first-use-journal-guard'),
    ('purple_sync_auto_service.cpp', '(action == SyncFirstUseAction::JoinSilently && !review.sameBytes)', '(action == SyncFirstUseAction::JoinSilently)', 'first-use-actions'),
    ('purple_sync_auto_service.cpp', 'self->FirstUseJoined(inventory, review);', 'self->FirstUseFinish(SyncFirstUseOutcome::Joined);', 'first-use-actions'),
    ('purple_sync_setup_box.cpp', 'if (followup == CheckFollowup::None && FirstUse(box, state, result)) {', 'if (false) {', 'first-use-box'),
    ('purple_sync_setup_box.cpp', 'RunFirstUse(box, state, SyncFirstUseChoice::Synced);', 'RunFirstUse(box, state, SyncFirstUseChoice::None);', 'first-use-box'),
    ('purple_sync_setup_box.cpp', 'if (*first.action == SyncFirstUseAction::Ask) {\n\t\tstate->asking = true;', 'if (*first.action == SyncFirstUseAction::Ask) {\n\t\tConfirmAction(box, state, Action::Join);', 'first-use-box'),
    ('purple_sync_auto_service.cpp', 'first->outcome == SyncFirstUseOutcome::Asking) { return tr::lng_purple_auto_first_ask(tr::now); }', 'first->outcome == SyncFirstUseOutcome::Asking) { return tr::lng_purple_auto_review(tr::now); }', 'first-use-auto-status'),
    ('purple_sync_auto_service.cpp', 'self->_store.get() == store && self->_sessionId == owner', 'true', 'borrowed-proof-run'),
    ('purple_sync_auto_service.cpp', 'store->Current() && self->Resolved()', 'store->Current() && SyncChatSelfResolved(self->_session.get()) && self->Resolved()', 'borrowed-proof-observer-free'),
    ('purple_sync_auto_service.cpp', 'DesktopSyncChatInventoryMatches(inventory, origin, chat)', 'true', 'auto-typed-inventory'),
    ('purple_sync_magic_service.cpp', 'ToastSyncMagicProblem(session);', '', 'magic-problem-toast'),
    ('purple_sync_magic_telegram.cpp', '\t\t\tsession->data().chatsListChanges(),\n', '', 'magic-lists-early'),
    ('purple_sync_magic_telegram.cpp', 'bound && data.channelLoaded(ChannelId(bound->id)) != nullptr', 'false', 'magic-lists-early'),
    ('purple_sync_magic_telegram.cpp', 'namespace Purple {', 'namespace Purple {\nconstexpr auto kOld = mtpTypeId(0x875f74be);', 'no-hand-rolled-tl'),
    ('purple_sync_magic_telegram.cpp', 'AddJoined(&data, result.visible);', 'AddFromList(data.chatsList(nullptr), result.visible);', 'magic-lists-unfiltered'),
    ('purple_sync_magic_telegram.cpp', 'archive ? data.chatsListLoaded(archive) : result.mainLoaded;', 'archive && data.chatsListLoaded(archive);', 'magic-lists-unfiltered'),
    ('purple_sync_magic_box.cpp', 'return frame.telegram', 'return true', 'magic-wait-wording'),
    ('purple_sync_config_publish_flow.cpp', ' || binding->verifiedAt > 0)', ')', 'magic-publish-authority'),
    ('purple_sync_config_publish_flow.cpp', '} else if (_magic && !MagicBound(_target->files.syncDirectory, _input.chat)) {', '} else if (false) {', 'magic-publish-authority'),
    ('purple_sync_config_publish_flow.cpp', '_magic && complete && SelectedSyncSpace(inventory);', '_magic && complete;', 'magic-publish-authority'),
    ('purple_sync_config_publish_flow.cpp', 'return record.status == SyncCandidateStatus::Valid && record.header;', 'return true;', 'magic-join-selected-space'),
    ('purple_sync_config_publish_flow.cpp', 'record.header && record.header->space == *space;', 'record.header;', 'magic-join-selected-space'),
    ('purple_sync_setup_box.cpp', '\tMoveSyncStateToBoundChat(session);\n', '', 'magic-manual-check'),
    ('purple_sync_auto_service.cpp', '\t\t\tFor(session).MoveToBound();\n', '', 'magic-move-unbound-auto'),
    ('purple_sync_magic_box.cpp', '\tchat->shown();\n\tconst auto weak = base::make_weak(controller);', '\tconst auto weak = base::make_weak(controller);', 'magic-problem-toast'),
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
for path in sorted(purple.rglob('*')):
    if path.suffix in ('.cpp', '.h'):
        text = path.read_text()
        assert 'mtpTypeId(' not in text and '875f74be' not in text, ('hand-rolled TL', path.name)
print(json.dumps({'routes': len(RouteNames), 'negative_controls': len(negative), 'failures': 0}))
