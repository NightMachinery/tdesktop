#define PURPLE_SYNC_CHAT_CARRY_EMBEDDED
#include "test_sync_chat_carry_io.cpp"
#include "purple/purple_sync_chat_carry_executor.h"
#include <barrier>
#include <stdexcept>
#include <type_traits>

namespace {

DesktopSyncChatInventory InventoryRows(const DesktopSyncChatIo &route,
		const std::vector<DesktopSyncChatRecordReceipt> &receipts) {
	auto read = SyncCandidateReadResult(); read.status = SyncCandidateReadStatus::Complete;
	auto scan = SyncHistoryScanResult(); scan.status = SyncHistoryScanStatus::Complete;
	for (const auto &receipt : receipts) { read.records.push_back(receipt.record); scan.candidateIds.push_back(receipt.record.id); ++scan.scannedCount; }
	if (!read.records.empty()) { read.status = AggregateSyncCandidateRead(read.records); }
	return {route, FinishSyncAccountInventory(route.peer.origin.account.userId, scan, read), receipts, DesktopSyncChatReadStamp::Begin()};
}

struct ExecutorFiles {
	QTemporaryDir temporary;
	QString root;
	SyncChatCarryStoreScope scope;
	DesktopSyncCarryFresh fresh;
	SyncCarryJournalStart start;
	SyncLocalState initial;
	QByteArray original = RecordBytes();
	QByteArray dirty = "version = 1\n# local desired edit\n";
	std::vector<DesktopSyncChatRecordReceipt> cloud;
	std::unique_ptr<DesktopSyncChatCarry> engine;
	std::shared_ptr<std::atomic<bool>> admitted = std::make_shared<std::atomic<bool>>(true);
	std::shared_ptr<DesktopSyncCarryGate> gate;
	DesktopSyncCarryProgress progress;
	std::vector<SyncCarryJournalAction> actions;
	int outward = 0;
	int markWrites = 0;
	int writes = 0;
	int failWrite = -1;
	SyncDurablePoint failPoint = SyncDurablePoint::BeforeWrite;
	bool armed = false;
	bool failing = false;
	bool own;
	bool pending;
	bool pendingPresent = true;
	int pendingPosts = 0;
	bool unknownDelivery = false;
	bool unknownMark = false;
	SyncCarryMode mode;

	explicit ExecutorFiles(bool own = false, SyncCarryMode mode = SyncCarryMode::Switch, bool pending = false)
	: own(own), pending(pending), mode(mode) {
		Must(temporary.isValid()); temporary.setAutoRemove(false);
		root = temporary.path() + u"/sync"_q;
		Must(EnsureSyncPrivateDirectory(root) == SyncStoreStatus::Ready);
		Must(WriteSyncPrivateFile(temporary.path()+u"/settings.toml"_q,dirty)==SyncStoreStatus::Ready);
		initial.version = own ? 2 : 1; initial.chat = own ? PeerToken{PeerTokenKind::Chat,17} : PeerToken{};
		initial.install = u"in-"_q + QString(26,u'a'); initial.space = u"sp-"_q + QString(26,u'a');
		initial.createdDevice = u"Synthetic"_q; initial.bindingToken = QString(32,u'a');
		scope = {Origin.account,initial.bindingToken.toLatin1(),QString(64,u'a'),temporary.path()+u'/' + DesktopSyncChatCacheFileName(Origin.account)};
		if (mode != SyncCarryMode::Fill) {
			auto store = SyncLocalStore(root); CHECK(store.Open(true).status == SyncStoreStatus::Uninitialized); Must(store.Initialize(initial));
			auto data = initial.configData; data.pending = InspectConfigPayload(ParseSyncEnvelope(original)).version.key;
			Must(store.StageConfig(original,data,scope.binding)); Must(store.ConfirmConfigReadBack(original,initial.createdDevice,10,initial.chat));
			if (pending) {
				const auto next = RecordBytes(2); data = store.state()->configData;
				data.pending = InspectConfigPayload(ParseSyncEnvelope(next)).version.key;
				Must(store.StageConfig(next,data,scope.binding));
			}
			initial = *store.state();
		}
		fresh.source = Route(initial.chat); fresh.target = mode == SyncCarryMode::RepairMark ? Route(initial.chat) : Route({PeerTokenKind::Channel,456});
		fresh.source.peer.facts->protectedContent = own;
		fresh.loginAuthority = scope.loginAuthority; fresh.current = [alive=admitted] { return alive->load(); };
		fresh.readerConsentAccepted = true; fresh.consentPeer = fresh.target.peer;
		fresh.resolutionInput.selfUserId = Origin.account.userId;
		fresh.resolutionInput.cache = SyncChatCache{initial.chat,10,100};
		fresh.resolutionInput.cacheRead = mode == SyncCarryMode::RepairMark ? SyncChatDirectRead::MarkGone : SyncChatDirectRead::Valid;
		fresh.resolutionInput.settingsBound = mode != SyncCarryMode::Fill;
		fresh.resolutionInput.settingsChat = initial.chat;
		fresh.resolutionInput.readerWarningAccepted = true;
		fresh.resolutionInput.cachedEligibility = *DesktopSyncChatEligibility(Route(initial.chat).peer);
		if (mode == SyncCarryMode::Follow || mode == SyncCarryMode::Fill) {
			fresh.resolutionInput.marks.push_back({{fresh.target.peer.chat,20,200},SyncChatMarkStatus::Supported,
				*DesktopSyncChatEligibility(fresh.target.peer),mode == SyncCarryMode::Fill ? SyncChatRecordPresence::Empty : SyncChatRecordPresence::Present,true,true});
		}
		if (mode == SyncCarryMode::Fill) { fresh.resolutionInput.cache.reset(); fresh.resolutionInput.settingsHint = SyncChatSettingsHint::Found; }
		start.operationId = *FormatSyncBindingToken(QByteArray(16,'o')); start.account = scope.account;
		start.source = initial.chat; start.target = fresh.target.peer.chat; start.mode = mode;
		start.state = mode == SyncCarryMode::Fill ? std::nullopt : std::optional(initial);
		start.stateAbsentConfirmed = mode == SyncCarryMode::Fill; start.preferenceToken = scope.binding;
		start.targetEligibility = *DesktopSyncChatEligibility(fresh.target.peer);
		start.readerConsentAccepted = true; start.operationConfirmed = true;
		start.resolution = ResolveSyncChat(fresh.resolutionInput);
		start.markText = DesktopSyncChatMarkText(); start.markRequestId = (mode == SyncCarryMode::Switch || mode == SyncCarryMode::RepairMark) ? u"41"_q : QString();
		start.ownRequestId = u"42"_q; start.loginAuthority = DesktopSyncCarryLogin(scope.loginAuthority);
		start.oldRead = mode == SyncCarryMode::RepairMark ? SyncChatDirectRead::MarkGone : SyncChatDirectRead::Valid;
		start.sourceAccess = SyncCarrySourceAccess::Readable;
		gate = DesktopSyncCarryGateFor(scope.account);
		newEngine();
	}
	~ExecutorFiles() { engine.reset(); }
	void newEngine(bool resume = false) {
		engine.reset();
		admitted->store(true);
		const auto lease = gate->carry(); Must(lease);
		engine = std::make_unique<DesktopSyncChatCarry>(root,scope,fresh.target.peer.origin,admitted,lease,
			[this](DesktopSyncCarryProgress value) { progress = std::move(value); }, [this](SyncDurablePoint point) {
				if (point == SyncDurablePoint::BeforeWrite) { ++writes; if (armed && writes == failWrite) { failing = true; } }
				return !failing || point != failPoint;
			});
		if (resume) { engine->open(); }
	}
	void actualResolution() {
		if (markWrites) {
			const auto mark = SyncChatCache{fresh.target.peer.chat,90,500};
			if (std::none_of(fresh.resolutionInput.marks.begin(), fresh.resolutionInput.marks.end(),
					[&](const auto &value) { return value.location == mark; })) {
				fresh.resolutionInput.marks.push_back({mark,SyncChatMarkStatus::Supported,
					*DesktopSyncChatEligibility(fresh.target.peer),SyncChatRecordPresence::Present,true,true});
			}
		}
		const auto state = ParseSyncLocalState(CaptureSyncPrivateFile(root+u"/state.json"_q,4*1024*1024).bytes);
		if (state && progress.journal && progress.journal->stateCommitted
			&& state.state.chat == fresh.target.peer.chat) {
			fresh.resolutionInput.settingsChat = state.state.chat;
			fresh.resolutionInput.settingsBound = true;
			fresh.resolutionInput.cache = SyncChatCache{fresh.target.peer.chat,
				mode==SyncCarryMode::Follow || mode==SyncCarryMode::Fill ? 20 : 90,
				mode==SyncCarryMode::Follow || mode==SyncCarryMode::Fill ? 200 : 500};
			fresh.resolutionInput.cacheRead = SyncChatDirectRead::Valid;
			fresh.resolutionInput.cachedEligibility = *DesktopSyncChatEligibility(fresh.target.peer);
		}
	}

	DesktopSyncCarryReply reply() {
		actualResolution();
		Must(progress.command);
		const auto command = *progress.command;
		actions.push_back(command.action);
		auto result = DesktopSyncCarryReply(); result.command = command; result.outcome = SyncCarryOutcome::Success;
		result.fresh = fresh; result.access = own ? SyncCarrySourceAccess::Protected : SyncCarrySourceAccess::Readable;
		result.source = CarryInventory(fresh.source,original);
		if (!own && pending && (pendingPresent || pendingPosts)) {
			result.source=InventoryRows(fresh.source,{Receipt(fresh.source,original,10),Receipt(fresh.source,RecordBytes(2),11)});
		}
		result.destination = InventoryRows(fresh.target,cloud);
		if (command.action == SyncCarryJournalAction::ResolvePending) {
			result.retainedLocalText = InspectConfigPayload(ParseSyncEnvelope(command.pendingRecord)).text;
			result.sendIntent = SyncCarrySendIntent::NotIssued;
			if (!own && (pendingPresent || pendingPosts)) { result.source = CarryInventory(fresh.source,command.pendingRecord,11); }
		}
		if (command.dispatch) {
			Must(progress.journal);
			CHECK(command.action==SyncCarryJournalAction::ResolvePending || progress.journal->deliveryAttempted || progress.journal->markAttempted);
			for (const auto &permit : command.effects) {
				CHECK(permit.current()); ++outward;
				if (command.action == SyncCarryJournalAction::ResolvePending) {
					CHECK(permit.target().peer.chat==fresh.source.peer.chat && permit.uploadContent()==command.pendingRecord);
					const auto duplicate=permit; CHECK(permit.claimUpload() && !duplicate.claimUpload());
					CHECK(permit.uploadMatches(command.pendingRecord) && !permit.uploadMatches(command.pendingRecord+' '));
					++pendingPosts; pendingPresent=true;
					result.updates.push_back(Receipt(fresh.source,permit.randomId(),11));
				} else if (command.action == SyncCarryJournalAction::PostMark) {
					CHECK(DesktopSyncCarryMarkRequest(permit)); ++markWrites;
					result.updates.push_back(Receipt(fresh.target,permit.randomId(),90));
				} else {
					auto bytes = permit.uploadContent();
					if (bytes.isEmpty()) {
						const auto request=DesktopSyncCarryForwardRequest(permit); Must(request); auto wire=mtpBuffer(); request->write(wire);
						auto cursor=wire.constData(); const auto end=cursor+wire.size(); Read<MTPint>(cursor,end); Read<MTPint>(cursor,end); Read<MTPInputPeer>(cursor,end);
						const auto ids=Read<MTPVector<MTPint>>(cursor,end); Must(ids.v.size()==1);
						const auto found=std::find_if(result.source->receipts.begin(),result.source->receipts.end(),[&](const auto &receipt){return receipt.record.id==ids.v.front().v;});
						Must(found!=result.source->receipts.end()); bytes=found->record.bytes;
					} else { CHECK(permit.uploadMatches(bytes)); }
					const auto id = 30 + int(cloud.size()); cloud.push_back(Receipt(fresh.target,bytes,id,300));
					result.updates.push_back(Receipt(fresh.target,permit.randomId(),id));
				}
			}
			if ((unknownDelivery && command.action == SyncCarryJournalAction::Deliver) || (unknownMark && command.action == SyncCarryJournalAction::PostMark)) { result.outcome = SyncCarryOutcome::Unknown; result.updates.clear(); }
		}
		if (command.action == SyncCarryJournalAction::ReadMark) {
			if ((mode==SyncCarryMode::Switch || mode==SyncCarryMode::RepairMark) && !markWrites) {
				result.markResponse=Response(fresh.target,{}); return result;
			}
			result.markId = mode == SyncCarryMode::Follow || mode == SyncCarryMode::Fill ? 20 : 90;
			auto message = CarryMark(fresh.target,DesktopSyncChatMarkText(),result.markId);
			if (mode == SyncCarryMode::Follow || mode == SyncCarryMode::Fill) {
				auto native = NativeMessage();
				native.id = MTP_int(result.markId); native.date = MTP_int(200);
				native.flags = MTP_flags(MTPDmessage::Flag::f_out | MTPDmessage::Flag::f_from_id);
				native.peer_id = Target(fresh.target.peer.chat); native.from_id = MTP_peerUser(MTP_long(Origin.account.userId));
				native.message = MTP_string(DesktopSyncChatMarkText()); message = native.Build();
			}
			result.markResponse = Response(fresh.target,{message});
			if (unknownMark && (mode==SyncCarryMode::Switch || mode==SyncCarryMode::RepairMark)) { result.updates.push_back(Receipt(fresh.target,uint64_t(41),result.markId)); }
		}
		return result;
	}
	void step() { auto value = reply(); engine->complete(std::move(value)); }
	void run(int maximum = 100) {
		for (auto i=0; !progress.terminal && i!=maximum; ++i) {
			if (progress.command) { step(); }
			else if (progress.reason==u"carry-retry-required"_q) { engine->resume(); }
			else { break; }
		}
	}
	void finish() {
		run();
		if (!progress.terminal) { std::fprintf(stderr,"Executor hold: %s phase=%d store=%d journal=%d\n",qPrintable(progress.reason),progress.journal ? int(progress.journal->phase) : -1,int(progress.storeStatus),int(progress.journalStatus)); }
		Must(progress.terminal); Must(progress.journal); CHECK(progress.journal->phase == SyncCarryPhase::Done);
		CHECK(!gate->postingHeld());
		CHECK(SyncCarryStartupAdmission(root,scope.binding) == SyncStoreStatus::Ready);
		const auto cache = CaptureSyncPrivateFile(scope.cacheFile,64*1024);
		const auto parsed = ParseDesktopSyncChatCache(cache.bytes,scope.account); Must(parsed.record);
		CHECK(!parsed.record->carry && parsed.record->mark.chat == fresh.target.peer.chat);
		CHECK(CaptureSyncPrivateFile(temporary.path()+u"/settings.toml"_q,256*1024).bytes==dirty);
		if (mode == SyncCarryMode::Fill) { CHECK(CaptureSyncPrivateFile(root+u"/state.json"_q,4*1024*1024).absent()); }
		else { auto store=SyncLocalStore(root); Must(store.Open(true,scope.binding)); CHECK(store.state()->chat==fresh.target.peer.chat); }
	}
};

void Executors() {
	for (const auto own : {false,true}) {
		for (const auto mode : {SyncCarryMode::Switch,SyncCarryMode::Follow,SyncCarryMode::Fill}) {
			if (own && mode==SyncCarryMode::Fill) { continue; }
			std::printf("Executor mode=%d protected=%d\n",int(mode),own);
			auto f = ExecutorFiles(own,mode); f.engine->open(f.start); f.finish();
			CHECK(f.markWrites == (mode==SyncCarryMode::Switch ? 1 : 0));
		}
	}
	for (const auto own : {false,true}) { auto f=ExecutorFiles(own,SyncCarryMode::Switch,true); f.engine->open(f.start); f.finish(); }
	{
		auto f=ExecutorFiles(false,SyncCarryMode::RepairMark); f.engine->open(f.start); f.finish(); CHECK(f.cloud.empty() && f.outward==1);
		CHECK(CaptureSyncPrivateFile(f.root+u"/state.json"_q,4*1024*1024).bytes == SerializeSyncLocalState(f.initial).canonical);
	}
}

void RecoverySchedules() {
	for (auto crash=0;crash!=14;++crash) {
		std::printf("Recovery boundary=%d\n",crash);
		auto f=ExecutorFiles(true); f.engine->open(f.start);
		for (auto i=0;f.progress.command && !f.progress.terminal && i!=crash;++i) { f.step(); }
		if (f.progress.terminal) { continue; }
		const auto seq=f.progress.journal->plan ? ParseSyncEnvelope(f.progress.journal->plan->ownRecord).header->seq : 0;
		f.newEngine(true); f.run();
		if (f.progress.reason==u"matched-mark-receipt-required"_q && !f.markWrites) {
			CHECK(f.progress.journal && f.progress.journal->markAttempted && !f.progress.journal->stateCommitted);
			f.engine->cancel(); CHECK(!f.gate->postingHeld()); continue;
		}
		if (f.progress.reason==u"matched-mark-receipt-required"_q && f.markWrites) { f.unknownMark=true; f.engine->resume(); }
		f.finish();
		if (seq) { CHECK(f.progress.journal->plan && ParseSyncEnvelope(f.progress.journal->plan->ownRecord).header->seq==seq); }
		CHECK(f.markWrites<=1);
	}
	for (auto kind=0;kind!=3;++kind) {
		auto f=ExecutorFiles(); f.unknownDelivery=kind!=1; f.unknownMark=kind!=0;
		f.engine->open(f.start); f.finish(); CHECK(f.markWrites==1 && f.cloud.size()==1);
	}
}

void EffectBeforeCallbackControls() {
	for (const auto action : {SyncCarryJournalAction::Deliver,SyncCarryJournalAction::PostMark}) {
		auto f=ExecutorFiles(); f.engine->open(f.start);
		while(f.progress.command && !(f.progress.command->action==action && f.progress.command->dispatch)){f.step();}
		Must(f.progress.command); auto lost=f.reply(); const auto sent=f.outward;
		f.newEngine(true); f.run();
		if(action==SyncCarryJournalAction::PostMark){
			CHECK(f.progress.reason==u"matched-mark-receipt-required"_q && f.markWrites==1 && !f.progress.terminal);
			f.unknownMark=true; f.engine->resume();
		}
		f.finish(); CHECK(f.markWrites==1 && f.outward>=sent && f.cloud.size()==1);
	}
	for(auto damaged=0;damaged!=3;++damaged){
		auto f=ExecutorFiles(); f.engine->open(f.start);
		while(f.progress.command && f.progress.command->action!=SyncCarryJournalAction::VerifyDestination){f.step();}
		Must(f.progress.command); auto reply=f.reply();
		if(damaged==0){reply.destination->inventory.scan.status=SyncHistoryScanStatus::InvalidResponse;}
		if(damaged==1){reply.destination->receipts.front().record.bytes+=' ';}
		if(damaged==2){reply.destination->route.peer.origin.login=u"stale"_q;}
		f.engine->complete(reply); CHECK(!f.progress.terminal && !f.progress.command && f.markWrites==0);
		f.engine->resume(); f.finish(); CHECK(f.markWrites==1 && f.cloud.size()==1);
	}
	for(auto boundary=0;boundary!=15;++boundary){
		auto f=ExecutorFiles(true); f.engine->open(f.start);
		for(auto i=0;f.progress.command && !f.progress.terminal && i!=boundary;++i){f.step();}
		if(f.progress.terminal){continue;}
		const auto before=f.outward; auto late=f.reply();
		f.engine->cancel(); CHECK(!f.gate->postingHeld());
		f.engine->complete(late); CHECK(f.outward>=before && !f.gate->postingHeld());
		CHECK(CaptureSyncPrivateFile(f.temporary.path()+u"/settings.toml"_q,256*1024).bytes==f.dirty);
	}
}

void GateControls() {
	const auto gate=DesktopSyncCarryGateFor(Origin.account);
	auto writer=gate->writer(); Must(writer); CHECK(writer->current());
	auto carry=gate->carry(); Must(carry); CHECK(!writer->current() && !carry->quiescent() && !gate->writer() && !gate->carry());
	writer.reset(); CHECK(carry->quiescent()); carry.reset();
	auto next=gate->carry(); Must(next); CHECK(next->quiescent()); next.reset();
	CHECK(gate->writer());
}

void ThreadAndMergeControls() {
	{
		auto f=ExecutorFiles(); f.engine->open(f.start); f.finish(); f.engine.reset();
		auto store=SyncChatCarryStore(f.root,f.scope); Must(store.Open()); Must(store.state());
		auto next=f.start; next.state=*store.state(); next.source=next.state->chat; next.target={PeerTokenKind::Channel,789};
		next.targetEligibility=*DesktopSyncChatEligibility(Route(next.target).peer);
		auto input=SyncChatResolveInput(); input.selfUserId=Origin.account.userId; input.settingsBound=true;
		input.settingsChat=next.source; input.cache=f.progress.journal->targetMark; input.cacheRead=SyncChatDirectRead::Valid;
		input.cachedEligibility=*DesktopSyncChatEligibility(Route(next.source).peer); input.readerWarningAccepted=true;
		next.resolution=ResolveSyncChat(input); CHECK(next.resolution.action==SyncChatAction::Use);
		CHECK(!store.Begin(next)); next.operationId=*FormatSyncBindingToken(QByteArray(16,'p'));
		CHECK(!store.Begin(next)); next.markRequestId=u"91"_q; next.ownRequestId=u"92"_q;
		Must(store.Begin(next));
	}
	{
		auto f=ExecutorFiles(); f.initial.library.seq=1; f.initial.library.confirmedSeq=1; f.initial.library.ownHash=QString(64,u'a');
		const auto image=SerializeSyncLocalState(f.initial); Must(image); f.start.state=f.initial;
		Must(WriteSyncPrivateFile(f.root+u"/state.json"_q,image.canonical)==SyncStoreStatus::Ready);
		f.engine->open(f.start); f.run(); CHECK(f.progress.reason==u"playlist-required"_q && !f.progress.terminal && f.markWrites==0);
		f.engine->cancel(); CHECK(!f.gate->postingHeld());
	}
	{
		auto gate=DesktopSyncCarryGateFor(Origin.account); auto admitted=std::make_shared<std::atomic<bool>>(true);
		auto closed=false;
		auto flow=DesktopSyncChatCarry(u"/unused"_q,{},Origin,admitted,gate->carry(),[&](auto){closed=true;throw std::runtime_error("injected-cleanup");});
		try { flow.cancel(); } catch(const std::runtime_error &) {}
		CHECK(closed && !gate->postingHeld() && gate->writer());
	}
	{
		auto gate=DesktopSyncCarryGateFor(Origin.account); auto admitted=std::make_shared<std::atomic<bool>>(true);
		auto pointer=static_cast<DesktopSyncChatCarry*>(nullptr); auto calls=0;
		auto flow=DesktopSyncChatCarry(u"/unused"_q,{},Origin,admitted,gate->carry(),[&](auto){++calls;pointer->cancel();});
		pointer=&flow; flow.cancel(); CHECK(calls==1 && !gate->postingHeld());
	}
	for (const auto mode : {SyncCarryMode::Switch, SyncCarryMode::RepairMark}) {
		auto f=ExecutorFiles(false, mode);
		auto record=DesktopSyncChatCacheRecord(); record.account=f.scope.account;
		record.mark=*f.start.resolution.cache; record.readers=DesktopSyncChatEligibility(f.fresh.source.peer)->readers;
		record.readerConsent=true; record.newReaders=true; record.loginBinding=f.scope.loginAuthority;
		record.preserved.insert(u"future-consent"_q,QJsonObject{{u"accepted"_q,true}});
		const auto bytes=SerializeDesktopSyncChatCache(record); Must(bytes);
		Must(WriteSyncPrivateFile(f.scope.cacheFile,*bytes)==SyncStoreStatus::Ready);
		f.engine->open(f.start); f.finish();
		const auto saved=ParseDesktopSyncChatCache(CaptureSyncPrivateFile(f.scope.cacheFile,64*1024).bytes,f.scope.account);
		Must(saved.record); CHECK(saved.record->newReaders == (mode == SyncCarryMode::RepairMark)); CHECK(saved.record->readerConsent);
		CHECK(saved.record->preserved.value(u"future-consent"_q).toObject().value(u"accepted"_q).toBool());
	}
	{
		auto f=ExecutorFiles(); f.engine->open(f.start); f.finish(); f.engine.reset();
		auto store=SyncChatCarryStore(f.root,f.scope); Must(store.Open());
		const auto effects=store.RetainedEffects(); Must(effects); CHECK(effects->size()==1);
		const auto images=store.RetainedImages(); Must(images); CHECK(images->size()==1);
		const auto transport=QJsonDocument::fromJson(images->front()).object().value(u"transport"_q).toObject();
		CHECK(!transport.value(u"entries"_q).toObject().isEmpty());
		auto history=QDir(f.root+u"/carry-history"_q).entryList({u"*.json"_q},QDir::Files); Must(history.size()==1);
		const auto path=f.root+u"/carry-history/"_q+history.front();
		const auto snapshot=CaptureSyncPrivateFile(path,4*1024*1024);
		auto envelope=QJsonDocument::fromJson(snapshot.bytes).object(); envelope.remove(u"login_authority"_q);
		CHECK(!InspectSyncCarryJournal(QJsonDocument(envelope).toJson(),f.scope.binding));
		envelope=QJsonDocument::fromJson(snapshot.bytes).object(); envelope.insert(u"login_authority"_q,QString(64,u'b'));
		CHECK(!InspectSyncCarryJournal(QJsonDocument(envelope).toJson(),f.scope.binding));
	}
}

void MainThreadAndConcurrentGateControls() {
	auto gate=DesktopSyncCarryGateFor(Origin.account); auto admitted=std::make_shared<std::atomic<bool>>(true);
	auto progress=DesktopSyncCarryProgress();
	auto flow=DesktopSyncChatCarry(u"/unused"_q,{},Origin,admitted,gate->carry(),[&](auto value){progress=std::move(value);});
	flow.open(); CHECK(progress.storeStatus==SyncStoreStatus::WrongThread && !gate->postingHeld());
	auto sync=std::barrier(2); auto counts=std::atomic<int>(0);
	auto a=std::thread([&]{ for(auto i=0;i!=1000;++i){sync.arrive_and_wait();auto lease=gate->writer(); if(lease){++counts;}sync.arrive_and_wait();} });
	auto b=std::thread([&]{ for(auto i=0;i!=1000;++i){sync.arrive_and_wait();auto lease=gate->carry(); if(lease){++counts;}sync.arrive_and_wait();} });
	a.join(); b.join(); CHECK(counts.load()>=1000 && !gate->postingHeld());
}

void RefusalAndLoginControls() {
	{
		auto f=ExecutorFiles(); f.engine->open(f.start); Must(f.progress.command);
		auto reply=f.reply(); reply.fresh->resolutionInput.marks.push_back({{f.fresh.target.peer.chat,21,1000},SyncChatMarkStatus::Supported,*DesktopSyncChatEligibility(f.fresh.target.peer),SyncChatRecordPresence::Present,true,true});
		f.engine->complete(reply); CHECK(f.progress.terminal && f.progress.journal && f.progress.journal->phase==SyncCarryPhase::Aborted && f.outward==0 && !f.gate->postingHeld());
	}
	for (auto change=0;change!=7;++change) {
		auto f=ExecutorFiles(); f.engine->open(f.start); Must(f.progress.command);
		auto reply=f.reply();
		if (change==0) { ++reply.command.ticket; }
		if (change==1) { ++reply.command.run; }
		if (change==2) { reply.command.origin.login=u"foreign"_q; }
		if (change==3) { reply.fresh->target.peer.origin.login=u"foreign"_q; }
		if (change==4) { reply.fresh->target.peer.facts->otherMembers.reset(); }
		if (change==5) { reply.fresh->loginAuthority=QString(64,u'b'); }
		if (change==6) { reply.fresh->target.peer.facts->canSendFiles=false; }
		f.engine->complete(reply); CHECK(f.outward==0 && !f.progress.terminal);
		f.engine->cancel(); CHECK(!f.gate->postingHeld());
	}
	for (auto step=0;step!=9;++step) {
		auto f=ExecutorFiles(true); f.engine->open(f.start);
		for (auto i=0;f.progress.command && !f.progress.terminal && i!=step;++i) { f.step(); }
		if (f.progress.terminal) { continue; }
		const auto before=CaptureSyncPrivateFile(f.root+u"/state.json"_q,4*1024*1024).bytes;
		f.engine.reset(); f.scope.loginAuthority=QString(64,u'b'); f.fresh.loginAuthority=f.scope.loginAuthority;
		f.fresh.source.peer.origin.login=u"relogin"_q; f.fresh.target.peer.origin.login=u"relogin"_q;
		f.newEngine(true); CHECK(f.progress.reason==u"interrupted-login-discard-only"_q && !f.progress.command);
		f.engine->discardInterrupted(false); CHECK(!f.progress.terminal);
		f.engine->discardInterrupted(true); Must(f.progress.terminal); Must(f.progress.journal);
		CHECK(f.progress.journal->phase==SyncCarryPhase::InterruptedAbandoned);
		CHECK(CaptureSyncPrivateFile(f.root+u"/state.json"_q,4*1024*1024).bytes==before);
		CHECK(!f.gate->postingHeld() && !f.progress.retainedEffects.empty());
		CHECK(SyncCarryStartupAdmission(f.root,f.scope.binding)==SyncStoreStatus::Ready);
	}
	{
		auto f=ExecutorFiles(); f.engine->open(); CHECK(f.progress.absent && !f.gate->postingHeld() && !f.progress.command);
	}
	{
		auto f=ExecutorFiles(); Must(WriteSyncPrivateFile(f.root+u"/carry.json"_q,"{bad") == SyncStoreStatus::Ready);
		f.engine->open(); CHECK(!f.progress.command && !f.gate->postingHeld());
		CHECK(CaptureSyncPrivateFile(f.root+u"/carry.json"_q,4*1024*1024).bytes=="{bad");
	}
	{
		auto f=ExecutorFiles(); f.unknownMark=true; f.engine->open(f.start);
		while (f.progress.command && f.progress.command->action!=SyncCarryJournalAction::ReadMark) { f.step(); }
		Must(f.progress.command); auto reply=f.reply(); reply.updates.clear(); f.engine->complete(reply);
		CHECK(f.progress.reason==u"matched-mark-receipt-required"_q && f.markWrites==1);
		f.engine->resume(); f.finish(); CHECK(f.markWrites==1);
	}
}

void LogoutWithoutPreferences() {
	auto f=ExecutorFiles(true,SyncCarryMode::Switch,true); f.engine->open(f.start);
	f.step(); f.engine.reset(); f.scope.loginAuthority=QString(64,u'b'); f.scope.binding.clear();
	f.fresh.loginAuthority=f.scope.loginAuthority;
	f.fresh.source.peer.origin.login=u"relogin"_q; f.fresh.target.peer.origin.login=u"relogin"_q;
	f.newEngine(true); CHECK(f.progress.reason==u"interrupted-login-discard-only"_q);
	f.engine->discardInterrupted(true); Must(f.progress.terminal);
	CHECK(f.progress.bindingRecoveryState && !f.progress.bindingRecoveryState->bindingToken.isEmpty());
	CHECK(!f.progress.retainedEffects.empty() && !f.progress.retainedImages.empty());
}

void DurableFailureControls() {
	auto count=0;
	{
		auto f=ExecutorFiles(true); f.engine->open(f.start); f.finish(); count=f.writes;
	}
	for (const auto point : {SyncDurablePoint::BeforeWrite,SyncDurablePoint::PartialWrite,SyncDurablePoint::BeforeRename,SyncDurablePoint::AfterRename,SyncDurablePoint::FileBarrier,SyncDurablePoint::DirectoryBarrier}) {
		for (auto write=1;write<=count;++write) {
			auto f=ExecutorFiles(true); f.failPoint=point; f.failWrite=write; f.armed=true;
			f.engine->open(f.start); f.run();
			if (!f.failing) { continue; }
			CHECK(!f.progress.terminal);
			const auto sends=f.outward;
			f.failing=false; f.armed=false; f.newEngine();
			const auto journal=CaptureSyncPrivateFile(f.root+u"/carry.json"_q,4*1024*1024);
			const auto history=QDir(f.root+u"/carry-history"_q).entryList({u"*.json"_q},QDir::Files);
			if (journal.absent() && !history.isEmpty()) {
				f.engine->open(); CHECK(f.progress.absent && !f.gate->postingHeld());
				auto local=SyncLocalStore(f.root); Must(local.Open(true,f.scope.binding));
				CHECK(local.state()->chat==f.fresh.target.peer.chat && f.outward==sends);
				continue;
			}
			if (journal.absent()) { f.engine->open(f.start); }
			else { f.engine->open(); }
			f.run();
			if (!f.progress.terminal && f.progress.reason==u"matched-mark-receipt-required"_q) {
				Must(f.progress.journal); CHECK(f.progress.journal->markAttempted && f.markWrites<=1);
				CHECK(!CaptureSyncPrivateFile(f.root+u"/carry.json"_q,4*1024*1024).absent());
				CHECK(f.outward==sends); f.engine->cancel(); CHECK(!f.gate->postingHeld());
			} else {
				if (!f.progress.terminal) { std::fprintf(stderr,"Durable point=%d write=%d\n",int(point),write); }
				f.finish();
			}
			CHECK(f.markWrites<=1 && f.outward>=sends);
		}
	}
}

template <typename Io>
concept PublicPendingIssuer = requires(Io &io, DesktopSyncCarryFresh fresh, DesktopSyncChatInventory inventory) {
	io.PendingPermit(fresh,inventory,1,2);
};

void PendingAndRetentionControls() {
	static_assert(!PublicPendingIssuer<DesktopSyncChatCarryIo>);
	static_assert(!std::is_default_constructible_v<DesktopSyncCarryPermit>);
	for (auto negative=0;negative!=5;++negative) {
		std::printf("Pending negative=%d\n",negative);
		auto f=ExecutorFiles(false,SyncCarryMode::Switch,true); f.pendingPresent=false;
		auto cache=DesktopSyncChatCacheRecord(); cache.account=f.scope.account; cache.mark=*f.start.resolution.cache;
		cache.readers=DesktopSyncChatEligibility(f.fresh.source.peer)->readers; cache.readerConsent=true; cache.loginBinding=f.scope.loginAuthority;
		const auto bytes=SerializeDesktopSyncChatCache(cache); Must(bytes); Must(WriteSyncPrivateFile(f.scope.cacheFile,*bytes)==SyncStoreStatus::Ready);
		f.engine->open(f.start); if (!f.progress.command) { std::cerr << "Pending opening held: " << f.progress.reason.toStdString() << " store=" << int(f.progress.storeStatus) << " journal=" << int(f.progress.journalStatus) << std::endl; } Must(f.progress.command); auto reply=f.reply();
		if (negative==1) { reply.fresh->source.peer.facts->protectedContent=true; }
		if (negative==2) { reply.source->inventory.scan.status=SyncHistoryScanStatus::InvalidResponse; }
		if (negative==3) { reply.fresh->loginAuthority=QString(64,u'b'); }
		if (negative==4) { Must(WriteSyncPrivateFile(f.root+u"/pending/config-2.json"_q,RecordBytes(2)+' ')==SyncStoreStatus::Ready); }
		f.engine->complete(reply);
		if (negative) { CHECK(!f.progress.command && f.pendingPosts==0); f.engine->cancel(); continue; }
		if (!f.progress.command) { std::cerr << "Pending admission held: " << f.progress.reason.toStdString() << " store=" << int(f.progress.storeStatus) << " journal=" << int(f.progress.journalStatus) << std::endl; } Must(f.progress.command); CHECK(f.progress.command->dispatch && f.progress.command->action==SyncCarryJournalAction::ResolvePending);
		const auto captured=*f.progress.command; Must(captured.effects.size()==1);
		auto session=UploadSession(); auto ordinary=OrdinaryPostContext(session); CHECK(!ordinary.ContextCurrent());
		const auto before=PostsFor(&session,f.fresh.source.peer.chat)->started;
		auto done=BeginNetwork(&session,f.fresh.source.peer.chat); CHECK(PostsFor(&session,f.fresh.source.peer.chat)->started==before); done();
		f.step(); CHECK(!captured.effects.front().current() && !captured.effects.front().claimUpload());
		f.finish(); CHECK(f.pendingPosts==1);
		CHECK(!f.engine->renew(captured,captured.effects.front(),f.fresh));
	}

	{
		std::printf("Pending scenario 1\n");
		auto f=ExecutorFiles(false,SyncCarryMode::Switch,true); f.engine->open(f.start); Must(f.progress.command);
		f.engine.reset();
		const auto path=f.root+u"/carry.json"_q;
		auto envelope=QJsonDocument::fromJson(CaptureSyncPrivateFile(path,4*1024*1024).bytes).object();
		envelope.remove(u"opening_pending_sha256"_q);
		const auto bytes=QJsonDocument(envelope).toJson(QJsonDocument::Compact);
		Must(WriteSyncPrivateFile(path,bytes)==SyncStoreStatus::Ready);
		f.newEngine(true); CHECK(!f.progress.command && !f.gate->postingHeld() && !f.pendingPosts);
		CHECK(CaptureSyncPrivateFile(path,4*1024*1024).bytes==bytes);
	}

	{
		std::printf("Pending scenario 2\n");
		auto f=ExecutorFiles(true,SyncCarryMode::Switch,true); f.engine->open(f.start); Must(f.progress.command);
		auto reply=f.reply(); reply.sendIntent=SyncCarrySendIntent::Issued;
		reply.source=CarryInventory(f.fresh.source,{},10);
		f.engine->complete(reply); f.finish();
		CHECK(!f.progress.journal->deferredRecord.isEmpty() && !f.progress.journal->targetPendingRecord.isEmpty());
		auto local=SyncLocalStore(f.root); Must(local.Open(true,f.scope.binding));
		CHECK(local.ReadPendingConfig().staged==f.progress.journal->targetPendingRecord);
	}
	{
		std::printf("Pending scenario 3\n");
		auto f=ExecutorFiles(true,SyncCarryMode::Switch,true); f.engine->open(f.start);
		auto reply=f.reply(); reply.sendIntent=SyncCarrySendIntent::Issued; reply.source=CarryInventory(f.fresh.source,reply.command.pendingRecord,11);
		f.engine->complete(reply); f.finish(); CHECK(f.progress.journal->deferredRecord.isEmpty());
	}
	{
		std::printf("Pending scenario 4\n");
		auto f=ExecutorFiles(true,SyncCarryMode::Switch,true); f.engine->open(f.start);
		auto reply=f.reply(); reply.sendIntent=SyncCarrySendIntent::Issued; reply.source->inventory.scan.status=SyncHistoryScanStatus::InvalidResponse;
		f.engine->complete(reply); CHECK(!f.progress.command && f.outward==0); f.engine->cancel();
	}
	{
		std::printf("Pending scenario 5\n");
		auto f=ExecutorFiles(true,SyncCarryMode::Switch,true); f.engine->open(f.start);
		auto reply=f.reply(); reply.access=SyncCarrySourceAccess::ChatGone; reply.oldRead=SyncChatDirectRead::ChatGone; reply.source.reset();
		f.engine->complete(reply); f.finish();
	}
	{
		std::printf("Pending scenario 6\n");
		auto f=ExecutorFiles(); f.engine->open(f.start);
		while (f.progress.command && !f.progress.command->dispatch) { f.step(); }
		Must(f.progress.command); auto old=*f.progress.command;
		f.engine->cancel(); CHECK(!f.gate->postingHeld());
		f.newEngine(true); Must(f.progress.command); const auto ticket=f.progress.command->ticket;
		auto late=DesktopSyncCarryReply(); late.command=old; late.outcome=SyncCarryOutcome::Unknown; f.engine->complete(late);
		CHECK(f.progress.command && f.progress.command->ticket==ticket); f.finish();
	}
	{
		std::printf("Pending scenario 7\n");
		auto f=ExecutorFiles(); f.engine->open(f.start); f.finish(); f.engine.reset();
		auto remove=SyncChatCarryStore(f.root,f.scope); CHECK(!remove.RemoveAccount(false)); Must(remove.RemoveAccount(true));
		CHECK(CaptureSyncPrivateFile(f.root+u"/carry.json"_q,4*1024*1024).absent());
		CHECK(!CaptureSyncPrivateFile(f.root+u"/state.json"_q,4*1024*1024).absent());
	}
}

}

#ifndef PURPLE_SYNC_CHAT_CARRY_EXECUTOR_EMBEDDED
int main(int argc,char **argv) {
	auto app=QCoreApplication(argc,argv); std::setbuf(stdout,nullptr);
	GateControls(); MainThreadAndConcurrentGateControls();
	auto worker=std::thread([] { Executors(); RecoverySchedules(); EffectBeforeCallbackControls(); RefusalAndLoginControls(); std::puts("Pending controls"); PendingAndRetentionControls(); std::puts("Logout controls"); LogoutWithoutPreferences(); std::puts("Thread controls"); ThreadAndMergeControls(); std::puts("Durable controls"); DurableFailureControls(); }); worker.join();
	std::printf("CARRY EXECUTOR %d checks, %d failures\n",Checks,Failures);
	return Failures ? 1 : 0;
}

#endif
