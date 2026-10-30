#define PURPLE_SYNC_CHAT_CARRY_EXECUTOR_EMBEDDED
#include "test_sync_chat_carry.cpp"
#include "purple/purple_sync_chat_recovery.h"
#include "purple/purple_sync_chat_setup_flow.h"
#include <deque>

namespace {

DesktopSyncChatSetupSnapshot RecoveryFrame(ExecutorFiles &f) {
	auto frame = DesktopSyncChatSetupSnapshot();
	frame.scope.origin = f.fresh.target.peer.origin; frame.scope.root = f.temporary.path();
	frame.scope.binding = f.scope.binding; frame.scope.loginBinding = f.scope.loginAuthority;
	frame.scope.settingsBound = true; frame.scope.settingsChat = f.initial.chat; frame.scope.space = f.initial.space;
	frame.scope.mark = f.fresh.resolutionInput.cache;
	frame.input = f.fresh.resolutionInput;
	frame.status.complete = true; frame.status.resolution = ResolveSyncChat(frame.input);
	frame.status.phase = frame.status.resolution.action == SyncChatAction::Lost ? DesktopSyncChatPhase::Lost : DesktopSyncChatPhase::Searching;
	frame.run = 1; frame.stateImage = CaptureSyncPrivateFile(f.root+u"/state.json"_q,kSyncCarryJournalStorageLimit).bytes;
	auto cache = DesktopSyncChatCacheRecord();
	cache.account = f.scope.account; cache.mark = *frame.scope.mark; cache.loginBinding = f.scope.loginAuthority;
	cache.readers = DesktopSyncChatEligibility(f.fresh.source.peer)->readers;
	cache.readerConsent = true; cache.disclosedStreams = {u"config"_q};
	cache.readerCreator = f.fresh.source.peer.facts->creator; cache.readerAdmin = f.fresh.source.peer.facts->admin;
	cache.readerKind = f.fresh.source.peer.kind; cache.readerMembers = f.fresh.source.peer.readerMembers;
	frame.cache = cache;
	const auto bytes = SerializeDesktopSyncChatCache(cache); Must(bytes); frame.cacheImage = *bytes;
	Must(WriteSyncPrivateFile(f.scope.cacheFile, *bytes) == SyncStoreStatus::Ready);
	return frame;
}

struct RecoveryBox {
	int rows = 0;
	int continueRows = 0;
	int closeRows = 0;
	template <typename Text, typename Callback> void addButton(Text, Callback) { ++rows; }
	template <typename Callback> void addButton(int text, Callback) { ++rows; if(text==2){++continueRows;} if(text==1){++closeRows;} }
	template <typename Text> void setTitle(Text) {}
	void addRow(int) {}
	void closeBox() {}
};
struct RecoverySession { Main::Session *get() const { return nullptr; } };
struct RecoverySetup { std::optional<DesktopSyncChatSetupSnapshot> opening; RecoverySession session; };
QString RecoveryTitle(DesktopSyncChatRecoveryChoice) { return {}; }
bool Current(const RecoverySetup &) { return true; }
bool MoveContextCurrent(const RecoverySetup &) { return true; }
void ConfirmRecovery(RecoveryBox *, RecoverySetup *, DesktopSyncChatRecoveryChoice) {}
void EnsureSyncChatResolved(Main::Session *, bool) {}
namespace tr { int lng_purple_chat_recovery_retry() { return 0; } int lng_purple_chat_recovery_repair(){return 0;} int lng_close(){return 1;} int lng_continue(){return 2;} int lng_cancel(){return 3;} }
void LiteralRecoveryRows(RecoveryBox *box, RecoverySetup *state)
#include "recovery-ui-rows.inc"
void LiteralRecoveryConfirmation(RecoveryBox *confirmation, QString disclosure, Fn<void()> proceed)
#include "recovery-ui-confirmation.inc"

void MainControls(const DesktopSyncChatSetupSnapshot &frame, const DesktopSyncChatPeer &peer) {
	CHECK(!SyncStoreWorkerThread());
	CHECK(!DesktopSyncChatRecoveryActivated());
	auto box = RecoveryBox(); auto state = RecoverySetup{frame}; LiteralRecoveryRows(&box, &state);
	CHECK(box.rows == 0);
	auto dispatched=0;auto nested=RecoveryBox();LiteralRecoveryConfirmation(&nested,{},[&]{++dispatched;});
	CHECK(nested.continueRows==0 && nested.closeRows==1 && dispatched==0);
	for (const auto choice : {DesktopSyncChatRecoveryChoice::Create, DesktopSyncChatRecoveryChoice::Choose, DesktopSyncChatRecoveryChoice::RepairMark}) {
		CHECK(DesktopSyncChatRecoveryChoices(frame).empty());
		const auto selected = choice == DesktopSyncChatRecoveryChoice::Create ? std::nullopt
			: std::optional(choice == DesktopSyncChatRecoveryChoice::Choose ? Route({PeerTokenKind::Channel,456}).peer : peer);
		CHECK(!SelectDesktopSyncChatRecovery(frame, choice, selected, u"window"_q, true));
	}
}

QByteArray CreatedUpdates() {
	auto channel=NativeChannel();channel.id=MTP_long(456);channel.flags=MTP_flags(MTPDchannel::Flag::f_access_hash|MTPDchannel::Flag::f_creator|MTPDchannel::Flag::f_broadcast);
	channel.access_hash=MTP_long(789);channel.title=MTP_string("Purple sync");channel.date=MTP_int(100);channel.photo=MTP_chatPhotoEmpty();
	auto updates=NativeUpdates();updates.users=MTP_vector<MTPUser>();updates.updates=MTP_vector<MTPUpdate>();updates.chats=MTP_vector<MTPChat>({channel.Build()});updates.date=MTP_int(100);updates.seq=MTP_int(1);
	auto words=mtpBuffer();MTPUpdates(updates.Build()).write(words);
	return QByteArray(reinterpret_cast<const char*>(words.constData()),words.size()*sizeof(mtpPrime));
}

struct RecoveryHarness {
	ExecutorFiles files;
	DesktopSyncChatSetupSnapshot frame;
	DesktopSyncChatPeer peer;
	QString window = u"window-A"_q;
	QString login;
	DesktopSyncChatOrigin origin;
	bool live = true;
	bool foreground = true;
	bool activated = true;
	bool holdResolve = false;
	bool holdInspect = false;
	bool throwStatus = false;
	bool throwCancel = false;
	Fn<void()> onStatus;
	int resolves = 0;
	int inspections = 0;
	int starts = 0;
	int creates = 0;
	int createRequests = 0;
	int provisionResumes = 0;
	bool realProvision = false;
	bool gone = false;
	SyncCarrySendIntent pendingIntent = SyncCarrySendIntent::NotIssued;
	bool pendingUnknown = false;
	std::unique_ptr<DesktopSyncChatSetupFlow> setup;
	int cancels = 0;
	int sends = 0;
	QString reason;
	std::deque<Fn<void()>> queue;
	Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)> resolution;
	Fn<void(std::optional<DesktopSyncChatPeer>)> inspection;
	Fn<void(std::optional<DesktopSyncChatCreateJournal>, std::optional<DesktopSyncChatPeer>)> provision;
	std::unique_ptr<DesktopSyncChatRecovery> flow;
	DesktopSyncChatRecoveryChoice choice;
	explicit RecoveryHarness(DesktopSyncChatRecoveryChoice choice = DesktopSyncChatRecoveryChoice::RepairMark, bool group = false, bool pending = false)
	: files(group, choice == DesktopSyncChatRecoveryChoice::RepairMark ? SyncCarryMode::RepairMark : SyncCarryMode::Switch, pending), choice(choice) {
		files.engine.reset();
		files.fresh.source.peer.readerMembers = std::vector<QString>{};
		if (group) { files.own=false; files.fresh.source.peer.facts->protectedContent=false; files.fresh.target=choice==DesktopSyncChatRecoveryChoice::RepairMark?files.fresh.source:files.fresh.target; }
		if (choice==DesktopSyncChatRecoveryChoice::RepairMark) { files.fresh.target.peer.readerMembers=files.fresh.source.peer.readerMembers; }
		files.fresh.resolutionInput.cacheRead = SyncChatDirectRead::MarkGone;
		files.start.oldRead = SyncChatDirectRead::MarkGone;
		files.start.resolution = ResolveSyncChat(files.fresh.resolutionInput);
		frame = RecoveryFrame(files); peer = files.fresh.target.peer;
		origin = frame.scope.origin; login = frame.scope.loginBinding;
	}
	void drain() {
		for (auto n=0; !queue.empty() && n!=600; ++n) { auto task=std::move(queue.front()); queue.pop_front(); task(); }
		CHECK(queue.empty());
	}
	void one() { Must(!queue.empty()); auto task=std::move(queue.front()); queue.pop_front(); task(); }
	void start() {
		auto selection = DesktopSyncChatRecoverySelection{frame, choice, choice == DesktopSyncChatRecoveryChoice::Create ? std::nullopt : std::optional(peer), window, DesktopSyncChatMoveFingerprint(frame)};
		auto port = DesktopSyncChatRecoveryPort();
		port.activated=[&] { return activated; }; port.current=[&] { return live; }; port.foreground=[&] { return foreground; };
		port.origin=[&] { return origin; }; port.login=[&] { return login; }; port.window=[&] { return window; };
		port.worker=[&](Fn<void()> task) { queue.push_back(std::move(task)); };
		port.resolve=[&](Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)> done) {
			++resolves; if (holdResolve) { resolution=std::move(done); } else { done(frame); }
		};
		port.inspect=[&](PeerToken chat, Fn<void(std::optional<DesktopSyncChatPeer>)> done) {
			CHECK(chat==peer.chat); ++inspections; if (holdInspect) { inspection=std::move(done); } else { done(peer); }
		};
		port.provision=[&](DesktopSyncChatMoveDraft draft, Fn<void(std::optional<DesktopSyncChatCreateJournal>,std::optional<DesktopSyncChatPeer>)> done) {
			CHECK(draft.selection.provisionOnly && draft.selection.readerConsent && !draft.cleanup && !draft.removeOldMark);
			CHECK(BeginDesktopSyncChatProvision(draft.opening,draft.selection)); ++creates; provision=std::move(done);
			if (realProvision) {
				setup=std::make_unique<DesktopSyncChatSetupFlow>(draft.opening,draft.selection,files.root+u"/provision"_q,
					files.temporary.path()+u"/state.toml"_q,files.admitted,[&](DesktopSyncChatSetupProgress progress) { setupProgress(std::move(progress)); });
				setup->open(1000);
			}
		};
		port.resumeProvision=[&](Fn<void(std::optional<DesktopSyncChatCreateJournal>,std::optional<DesktopSyncChatPeer>)> done) {
			++provisionResumes; provision=std::move(done); if (setup) { setup->resume(1001); }
		};
		port.carry=[&](std::shared_ptr<std::atomic<bool>> admitted, Fn<void(DesktopSyncCarryProgress)> progress) {
			++starts; files.admitted=std::move(admitted); const auto lease=files.gate->carry(); Must(lease);
			return std::make_unique<DesktopSyncChatCarry>(files.root,files.scope,files.fresh.target.peer.origin,files.admitted,lease,std::move(progress));
		};
		port.send=[&](DesktopSyncCarryCommand command, Fn<void(DesktopSyncCarryReply)> done) {
			++sends; files.progress.command=command;
			auto reply=files.reply();
			if (gone) { reply.access=SyncCarrySourceAccess::ChatGone;reply.oldRead=SyncChatDirectRead::ChatGone;reply.source.reset(); }
			if(command.action==SyncCarryJournalAction::ResolvePending && !command.dispatch) {
				reply.sendIntent=pendingIntent;
				if(pendingUnknown && reply.source){reply.source->inventory.scan.status=SyncHistoryScanStatus::InvalidResponse;}
			}
			done(std::move(reply));
		};
		port.status=[&](DesktopSyncCarryProgress progress) {
			if (throwStatus) { throw std::runtime_error("synthetic observer"); }
			if (onStatus) { auto callback=std::move(onStatus); callback(); }
			if (!progress.reason.isEmpty()) { reason=progress.reason; }
			files.progress=std::move(progress);
		};
		port.cancel=[&] { ++cancels; if(setup){setup->cancel();setup.reset();} if(throwCancel){throw std::runtime_error("synthetic cancellation");} };
		flow=std::make_unique<DesktopSyncChatRecovery>(std::move(selection),std::move(port)); flow->start();
	}
	void setupProgress(DesktopSyncChatSetupProgress progress) {
		if (progress.provisioned && progress.journal) { provision(progress.journal,peer); return; }
		if (!progress.command) { return; }
		const auto command=*progress.command;
		queue.push_back([&,command] {
			if(!setup || !live){return;}
			auto reply=DesktopSyncChatCreateReply{command.origin,command.operation,command.ticket};reply.outcome=DesktopSyncChatCreateOutcome::Okay;
			if(command.step==DesktopSyncChatCreateStep::Resolve){reply.resolution=frame;}
			if(command.step==DesktopSyncChatCreateStep::Create){++createRequests;reply.updates=CreatedUpdates();reply.creationProof=DesktopSyncChatCreationProof::MatchedRequest;}
			if(command.step==DesktopSyncChatCreateStep::PeerInfo){reply.peer=peer;}
			if(command.step==DesktopSyncChatCreateStep::Inventory){reply.inventory=InventoryRows(files.fresh.target,{});}
			setup->complete(reply,1000);
		});
	}
	void close() { if (flow) { flow->close(); } drain(); CHECK(!files.gate->postingHeld()); }
};

void Policy() {
	auto h=RecoveryHarness();
	CHECK(DesktopSyncChatRecoveryHeld(h.frame).isEmpty());
	std::printf("POLICY repair=%s type=%d cached=%d statepending=%llu data=%s\n",qPrintable(DesktopSyncChatRepairHeld(h.frame,h.peer)),int(h.peer.kind),int(h.frame.cache->readerKind.value_or(SyncChatKind::Unknown)),(unsigned long long)ParseSyncLocalState(h.frame.stateImage).state.config.pendingSeq,ParseSyncLocalState(h.frame.stateImage).state.configData.pending.isEmpty() ? "empty" : "present");
	CHECK(DesktopSyncChatRepairHeld(h.frame,h.peer).isEmpty());
	CHECK(DesktopSyncChatRepairBaseline(h.frame,h.peer));
	for (auto mutation=0; mutation!=17; ++mutation) {
		auto frame=h.frame; auto peer=h.peer;
		switch (mutation) {
		case 0: frame.input.cacheRead=SyncChatDirectRead::Unknown; break;
		case 1: frame.input.cacheRead=SyncChatDirectRead::ChatGone; break;
		case 2: frame.status.complete=false; break;
		case 3: frame.scope.origin.login+=u'x'; break;
		case 4: peer.origin.account.userId++; break;
		case 5: peer.chat={PeerTokenKind::Chat,11}; break;
		case 6: frame.cache->readerConsent=false; break;
		case 7: frame.cache->disclosedStreams.clear(); break;
		case 8: frame.cache->readerKind.reset(); break;
		case 9: frame.cache->readerAdmin=true; peer.facts->admin=false; break;
		case 10: frame.cache->readerMembers=std::vector<QString>{}; peer.readerMembers.reset(); break;
		case 11: frame.cache->readerCreator=true; peer.creator=false; break;
		case 12: frame.cache->readerAdmin=true; peer.facts->admin=false; break;
		case 13: peer.facts->protectedContent=true; break;
		case 14: peer.facts->canSendFiles=false; break;
		case 15: frame.cache->paused=true; break;
		case 16: frame.stateImage+=' '; break;
		}
		frame.status.resolution=ResolveSyncChat(frame.input);
		CHECK(!DesktopSyncChatRepairBaseline(frame,peer));
	}
	for (const auto read : {SyncChatDirectRead::Unknown,SyncChatDirectRead::Valid,SyncChatDirectRead::MarkGone,SyncChatDirectRead::ChatGone}) {
		auto frame=h.frame; frame.input.cacheRead=read; frame.status.resolution=ResolveSyncChat(frame.input);
		CHECK((read==SyncChatDirectRead::MarkGone)==DesktopSyncChatRepairHeld(frame,h.peer).isEmpty());
	}
}

void WorkerSchedules() {
	{
		auto h=RecoveryHarness(); h.start(); h.drain(); std::printf("FLOW held=%s starts=%d\n",qPrintable(h.reason),h.starts); CHECK(h.files.progress.terminal && h.files.markWrites==1 && h.files.cloud.empty());
		CHECK(CaptureSyncPrivateFile(h.files.root+u"/state.json"_q,kSyncCarryJournalStorageLimit).bytes==h.frame.stateImage);
		CHECK(h.files.initial.bindingToken==ParseSyncLocalState(h.frame.stateImage).state.bindingToken); h.close();
	}
	for (auto late=0;late!=12;++late) {
		auto h=RecoveryHarness(); h.holdResolve=true; h.start(); h.drain(); Must(h.resolution);
		auto frame=h.frame;
		switch (late) {
		case 0: frame.input.cacheRead=SyncChatDirectRead::Valid; break;
		case 1: frame.input.cacheRead=SyncChatDirectRead::ChatGone; break;
		case 2: frame.input.cacheRead=SyncChatDirectRead::Unknown; break;
		case 3: frame.input.marks.push_back({{h.peer.chat,99,900},SyncChatMarkStatus::Supported,*DesktopSyncChatEligibility(h.peer),SyncChatRecordPresence::Present,true,true}); break;
		case 4: h.login=QString(64,u'b'); break;
		case 5: h.origin.account.userId++; break;
		case 6: h.origin.login+=u'x'; break;
		case 7: frame.cacheImage+=' '; break;
		case 8: frame.stateImage+=' '; break;
		case 9: h.foreground=false; break;
		case 10: h.window+=u'x'; break;
		case 11: h.activated=false; break;
		}
		frame.status.resolution=ResolveSyncChat(frame.input); h.resolution(frame); h.drain();
		CHECK(h.starts==0 && h.files.outward==0); h.close();
	}
	for (auto late=0;late!=7;++late) {
		auto h=RecoveryHarness(DesktopSyncChatRecoveryChoice::RepairMark,true); h.holdInspect=true; h.start(); h.drain(); Must(h.inspection);
		auto peer=h.peer;
		if (late==0) { peer.readerMembers.reset(); }
		if (late==1) { peer.facts->canSendFiles=false; }
		if (late==2) { h.live=false; }
		if (late==3) { h.flow->close(); }
		if (late==4) { peer.creator=false;peer.facts->creator=false; }
		if (late==5) { peer.facts->admin=false; }
		if (late==6) { peer.facts->otherMembers.reset(); }
		h.inspection(peer); h.drain(); CHECK(h.starts==0 && h.files.outward==0); h.close();
	}
	for (const auto choice : {DesktopSyncChatRecoveryChoice::Create,DesktopSyncChatRecoveryChoice::Choose,DesktopSyncChatRecoveryChoice::RepairMark}) {
		auto h=RecoveryHarness(choice); h.activated=false; h.start(); h.drain();
		CHECK(!h.creates && !h.starts && !h.sends); h.close();
	}
	{
		auto h=RecoveryHarness(DesktopSyncChatRecoveryChoice::Create); h.start(); h.drain(); Must(h.provision); CHECK(h.creates==1);
		h.flow->start(); h.flow->retry(); h.drain(); CHECK(h.creates==1 && h.starts==0);
		const auto done=h.provision; done({},{}); h.drain(); h.flow->retry(); h.drain(); CHECK(h.creates==1 && h.starts==0 && h.provisionResumes==1);
		h.close(); done({},{}); h.drain(); CHECK(h.creates==1 && h.starts==0);
	}
	{
		auto h=RecoveryHarness(); h.throwStatus=true; h.start(); h.drain(); h.close(); CHECK(h.cancels==1);
	}
	{
		auto h=RecoveryHarness();h.throwCancel=true;h.start();h.close();CHECK(h.cancels==1);
	}
	{
		auto h=RecoveryHarness();h.onStatus=[&]{h.flow->close();h.flow->retry();};h.start();h.drain();h.close();CHECK(h.cancels==1 && h.files.outward==0);
	}
	for (auto boundary=0;boundary!=6;++boundary) {
		auto h=RecoveryHarness(); h.start(); for(auto n=0;n!=boundary && !h.queue.empty();++n){h.one();}
		h.close(); CHECK(h.files.outward==0 && h.cancels==1);
	}
}

void ExpandedSchedules() {
	for(const auto group:{false,true}) {
		auto h=RecoveryHarness(DesktopSyncChatRecoveryChoice::RepairMark,group);h.start();h.drain();
		CHECK(h.files.progress.terminal && h.files.markWrites==1 && h.files.outward==1);
		CHECK(CaptureSyncPrivateFile(h.files.root+u"/state.json"_q,kSyncCarryJournalStorageLimit).bytes==h.frame.stateImage);
		h.close();
	}
	{
		auto h=RecoveryHarness(DesktopSyncChatRecoveryChoice::Choose);h.start();h.drain();
		CHECK(h.files.progress.terminal && h.files.markWrites==1 && !h.files.cloud.empty());h.close();
	}
	{
		auto h=RecoveryHarness(DesktopSyncChatRecoveryChoice::Create);h.realProvision=true;h.start();h.drain();
		CHECK(h.createRequests==1 && h.creates==1 && h.files.progress.terminal);h.close();
	}
	for(const auto cancel:{false,true}) {
		auto h=RecoveryHarness(DesktopSyncChatRecoveryChoice::Create);h.realProvision=true;h.start();
		while(!h.setup && !h.queue.empty()){h.one();}
		Must(h.setup);if(cancel){h.close();}else{h.live=false;h.drain();h.close();}
		CHECK(h.createRequests==0 && h.files.outward==0);
	}
	{
		auto h=RecoveryHarness(DesktopSyncChatRecoveryChoice::RepairMark,true);
		h.peer.facts->otherMembers=1;h.peer.readerMembers=std::vector<QString>{QString(64,u'b')};h.files.fresh.target.peer=h.peer;
		h.start();h.drain();CHECK(h.files.progress.terminal && h.files.outward==1);
		const auto cached=ParseDesktopSyncChatCache(CaptureSyncPrivateFile(h.files.scope.cacheFile,65536).bytes,Origin.account);Must(cached.record);
		CHECK(cached.record->newReaders && cached.record->readerMembers==h.frame.cache->readerMembers && cached.record->readers==h.frame.cache->readers);h.close();
	}
	for(auto mutation=0;mutation!=3;++mutation) {
		auto h=RecoveryHarness(DesktopSyncChatRecoveryChoice::Choose,true);
		h.files.fresh.source.peer.facts->protectedContent=true;h.files.own=true;
		if(mutation==1 || mutation==2){auto state=h.files.initial;state.preserved.remove(u"retained_config_bases"_q);if(mutation==2){state.configData.base=InspectConfigPayload(ParseSyncEnvelope(RecordBytes(2))).version.key;}
		Must(WriteSyncPrivateFile(h.files.root+u"/state.json"_q,SerializeSyncLocalState(state).canonical)==SyncStoreStatus::Ready);h.frame=RecoveryFrame(h.files);}
		h.start();h.drain();
		if(mutation==0){CHECK(h.files.progress.terminal && !h.files.cloud.empty());
			for(const auto &receipt:h.files.cloud){CHECK(InspectConfigPayload(ParseSyncEnvelope(receipt.record.bytes)).text!=h.files.dirty);}
		}else{
			CHECK(h.files.outward==0 && !h.files.progress.terminal && h.files.progress.storeStatus==SyncStoreStatus::MissingLocalVersion && h.reason==u"missing-local-version"_q);
			const auto starts=h.starts;h.flow->retry();h.drain();CHECK(h.starts==starts+1 && h.files.outward==0 && h.reason==u"missing-local-version"_q);
		}
		CHECK(CaptureSyncPrivateFile(h.files.temporary.path()+u"/settings.toml"_q,256*1024).bytes==h.files.dirty);h.close();
	}
}

void PendingAndRestartSchedules() {
	for (const auto gone : {false,true}) {
		for (auto pending=0;pending!=3;++pending) {
			auto h=RecoveryHarness(DesktopSyncChatRecoveryChoice::Choose,true,pending!=0);
			h.files.own=true;h.files.fresh.source.peer.facts->protectedContent=!gone;h.gone=gone;
			if(gone){h.files.fresh.resolutionInput.cacheRead=SyncChatDirectRead::ChatGone;}
			if(pending==2){h.pendingIntent=SyncCarrySendIntent::Issued;}
			h.frame=RecoveryFrame(h.files);h.start();h.drain();
			CHECK(h.files.progress.terminal && h.files.markWrites==1 && !h.files.cloud.empty());
			CHECK(h.files.pendingPosts==0 && h.files.progress.journal && h.files.progress.journal->source==h.frame.scope.settingsChat);
			h.close();
		}
	}
	{
		auto h=RecoveryHarness(DesktopSyncChatRecoveryChoice::Choose,true,true);h.files.own=true;h.files.fresh.source.peer.facts->protectedContent=true;
		h.pendingIntent=SyncCarrySendIntent::Issued;h.pendingUnknown=true;h.frame=RecoveryFrame(h.files);
		h.start();h.drain();CHECK(!h.files.progress.terminal && h.files.outward==0 && h.files.progress.journal->phase==SyncCarryPhase::ResolvePending);
		h.pendingIntent=SyncCarrySendIntent::NotIssued;h.flow->retry();h.drain();
		CHECK(!h.files.progress.terminal && h.files.outward==0 && h.files.progress.journal->phase==SyncCarryPhase::ResolvePending);h.close();
	}
	{
		auto f=ExecutorFiles(false,SyncCarryMode::RepairMark,true);const auto image=CaptureSyncPrivateFile(f.root+u"/state.json"_q,kSyncCarryJournalStorageLimit).bytes;
		f.engine->open(f.start);CHECK(!f.progress.command && f.outward==0);f.engine->cancel();
		CHECK(CaptureSyncPrivateFile(f.root+u"/state.json"_q,kSyncCarryJournalStorageLimit).bytes==image);
		CHECK(!CaptureSyncPrivateFile(f.root+u"/pending/config-2.json"_q,kSyncSettingsRecordStorageLimit).absent());
	}
	for (auto boundary=0;boundary!=5;++boundary) {
		auto f=ExecutorFiles(false,SyncCarryMode::RepairMark);f.engine->open(f.start);
		for(auto n=0;n!=boundary && f.progress.command;++n){f.step();}
		if(f.progress.terminal){continue;}
		f.actualResolution();f.newEngine(true);f.run();
		CHECK(f.markWrites<=1);
		if(f.markWrites){CHECK(f.progress.terminal);}
		else{CHECK(f.outward==0);f.engine->cancel();}
		CHECK(CaptureSyncPrivateFile(f.root+u"/state.json"_q,kSyncCarryJournalStorageLimit).bytes==SerializeSyncLocalState(f.initial).canonical);
	}
}

void CacheAndMainThreadControls() {
	auto h=RecoveryHarness(DesktopSyncChatRecoveryChoice::RepairMark,true);
	for(const auto raw:{QJsonValue(-1),QJsonValue(99),QJsonValue(1.5),QJsonValue(u"1"_q)}) {
		auto object=QJsonDocument::fromJson(h.frame.cacheImage).object();object.insert(u"reader_kind"_q,raw);
		CHECK(!ParseDesktopSyncChatCache(QJsonDocument(object).toJson(),Origin.account).record);
	}
	for(const auto raw:{QJsonValue(1),QJsonValue(u"true"_q),QJsonValue(QJsonObject())}) {
		auto object=QJsonDocument::fromJson(h.frame.cacheImage).object();object.insert(u"reader_admin"_q,raw);
		CHECK(!ParseDesktopSyncChatCache(QJsonDocument(object).toJson(),Origin.account).record);
	}
	auto legacy=QJsonDocument::fromJson(h.frame.cacheImage).object();legacy.remove(u"reader_kind"_q);legacy.remove(u"reader_admin"_q);
	const auto parsed=ParseDesktopSyncChatCache(QJsonDocument(legacy).toJson(),Origin.account);Must(parsed.record);
	auto frame=h.frame;frame.cache=parsed.record;CHECK(!DesktopSyncChatRepairBaseline(frame,h.peer));
}

void StoreControls() {
	for (auto kind=0;kind!=5;++kind) {
		auto h=RecoveryHarness();
		if (kind==0) {
			auto state=h.files.initial; state.preserved.remove(u"retained_config_bases"_q);
			CHECK(WriteSyncPrivateFile(h.files.root+u"/state.json"_q,SerializeSyncLocalState(state).canonical)==SyncStoreStatus::Ready);
			h.frame=RecoveryFrame(h.files);
		}
		if (kind==1) { Must(EnsureSyncPrivateDirectory(h.files.root+u"/pending"_q)==SyncStoreStatus::Ready); Must(WriteSyncPrivateFile(h.files.root+u"/pending/config-999.json"_q,h.files.original)==SyncStoreStatus::Ready); }
		if (kind==2) { h.files.start.oldRead=SyncChatDirectRead::ChatGone; }
		if (kind==3) { h.files.start.sourceAccess=SyncCarrySourceAccess::Unknown; }
		if (kind==4) { h.files.start.target=h.files.fresh.target.peer.chat={PeerTokenKind::Channel,777}; }
		if (kind==0) { h.start(); h.drain(); CHECK(h.files.progress.terminal && h.files.markWrites==1); h.close(); }
		else { h.files.newEngine(); h.files.engine->open(h.files.start); CHECK(h.files.outward==0 && !h.files.progress.command); h.files.engine->cancel(); }
		CHECK(CaptureSyncPrivateFile(h.files.root+u"/state.json"_q,kSyncCarryJournalStorageLimit).bytes==h.frame.stateImage);
	}
	for (const auto read : {SyncChatDirectRead::Valid,SyncChatDirectRead::Unknown,SyncChatDirectRead::ChatGone}) {
		auto f=ExecutorFiles(false,SyncCarryMode::RepairMark); f.engine->open(f.start); Must(f.progress.command);
		auto reply=f.reply(); reply.fresh->resolutionInput.cacheRead=read;
		f.engine->complete(reply); CHECK(f.outward==0 && !f.progress.command && !f.progress.terminal); f.engine->cancel();
	}
}

}

int main(int argc,char **argv) {
	auto app=QCoreApplication(argc,argv); std::setbuf(stdout,nullptr);
	DesktopSyncChatSetupSnapshot frame; DesktopSyncChatPeer peer;
	auto prepare=std::thread([&] { auto h=RecoveryHarness(); frame=h.frame; peer=h.peer; }); prepare.join();
	MainControls(frame,peer);
	std::unique_ptr<RecoveryHarness> mainFlow;
	auto create=std::thread([&]{mainFlow=std::make_unique<RecoveryHarness>();});create.join();
	CHECK(!SyncStoreWorkerThread());mainFlow->start();CHECK(!mainFlow->queue.empty() && mainFlow->starts==0);
	auto execute=std::thread([&]{mainFlow->drain();CHECK(mainFlow->files.progress.terminal);mainFlow->close();});execute.join();mainFlow.reset();
	auto worker=std::thread([] { Policy(); WorkerSchedules(); ExpandedSchedules(); PendingAndRestartSchedules(); CacheAndMainThreadControls(); StoreControls(); }); worker.join();
	std::printf("RECOVERY %d checks, %d failures\n",Checks,Failures);
	return Failures ? 1 : 0;
}
