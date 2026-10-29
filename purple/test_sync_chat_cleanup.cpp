#define PURPLE_SYNC_CHAT_CARRY_EXECUTOR_EMBEDDED
#include "test_sync_chat_carry.cpp"
#include <deque>
#include "purple/purple_sync_chat_setup.h"

namespace {

struct CleanupSenderProbe {
	Fn<void(const MTPmessages_AffectedMessages &)> done;
	Fn<void(const MTP::Error &)> fail;
	Fn<void(Fn<void(bool)>)> gate;
	mtpBuffer wire;
	int sends=0;
	template <typename Request> struct Builder {
		CleanupSenderProbe *owner;
		Builder &done(Fn<void(const MTPmessages_AffectedMessages &)> v) { owner->done=std::move(v); return *this; }
		Builder &fail(Fn<void(const MTP::Error &)> v) { owner->fail=std::move(v); return *this; }
		Builder &resumeEffect(Fn<void(Fn<void(bool)>)> v) { owner->gate=std::move(v); return *this; }
		void send() { ++owner->sends; }
	};
	template <typename Request> Builder<Request> request(Request value) { wire.clear(); value.write(wire); return {this}; }
};
#include "cleanup-transport.inc"

struct CleanupChoiceSink;
struct CleanupChoiceCheckbox {
	CleanupChoiceCheckbox(CleanupChoiceSink*, QString, bool checked, int)
	: value(checked) {}
	void setDisabled(bool disabled) { enabled = !disabled; }
	bool checked() const { return value; }
	bool value = false;
	bool enabled = true;
};
struct CleanupChoiceSink {
	std::vector<std::unique_ptr<CleanupChoiceCheckbox>> rows;
	CleanupChoiceCheckbox *addRow(std::unique_ptr<CleanupChoiceCheckbox> row) {
		const auto result = row.get(); rows.push_back(std::move(row)); return result;
	}
};
void CleanupChoiceOff() {
	CHECK(!DesktopSyncChatCleanupActivated() && !SyncStoreWorkerThread());
	struct State { std::optional<DesktopSyncChatSetupSnapshot> opening; };
	auto state = std::make_unique<State>();
	state->opening.emplace();
	state->opening->scope.settingsBound = true;
	state->opening->scope.mark = SyncChatCache{PeerToken{}, 9, 100};
	auto sink = CleanupChoiceSink();
	const auto confirmation = &sink;
#include "cleanup-choice.inc"
	auto enabled = 0;
	for (const auto &row : sink.rows) { enabled += row->enabled; }
	std::printf("OFF_CLEANUP_CHOICES rows=%zu enabled=%d\n", sink.rows.size(), enabled);
	CHECK(sink.rows.empty() && !removeMark && enabled == 0 && !oldMark);
}

MTPMessage CleanupEmpty(int id,std::optional<PeerToken> peer) {
	return MTP_messageEmpty(MTP_flags(peer?MTPDmessageEmpty::Flag::f_peer_id:MTPDmessageEmpty::Flags()),MTP_int(id),peer?Target(*peer):MTPPeer());
}

struct CleanupHarness {
	ExecutorFiles f;
	DesktopSyncCarryOpening opening;
	bool absent=false;
	std::set<int> removed;
	bool gone=false;
	int deletes=0;
	int applied=0;
	int sourceDate=100;
	explicit CleanupHarness(bool authorized=true,bool oldMark=false,SyncCarryMode mode=SyncCarryMode::Switch,bool typedOld=false,int date=100) : f(typedOld,mode),sourceDate(date) {
		f.own=false; f.fresh.source.peer.facts->protectedContent=false;
		if(mode!=SyncCarryMode::Fill) { f.fresh.resolutionInput.cache=SyncChatCache{f.initial.chat,9,100}; }
		f.start.resolution=ResolveSyncChat(f.fresh.resolutionInput);
		f.start.removeOldMarkAuthorized=oldMark;
		opening={CaptureSyncPrivateFile(f.root+u"/state.json"_q,kSyncCarryJournalStorageLimit).bytes,
			CaptureSyncPrivateFile(f.scope.cacheFile,65536).bytes,authorized};
		f.engine->open(f.start,{},u"config"_q,opening);
		for(auto i=0;f.progress.command && f.progress.command->action!=SyncCarryJournalAction::Cleanup && i!=100;++i) { f.engine->complete(readReply()); }
		Must(f.progress.journal && f.progress.journal->stateCommitted && f.progress.journal->cacheCompleted);
	}
	DesktopSyncCarryReply readReply() {
		auto reply=f.reply();
		reply.source=InventoryRows(f.fresh.source,{Receipt(f.fresh.source,f.original,10,sourceDate)});
		if(gone) { reply.access=SyncCarrySourceAccess::ChatGone; reply.oldRead=SyncChatDirectRead::ChatGone; }
		if(reply.command.cleanupRead) {
			const auto &item=reply.command.cleanupRead->item;
			if(absent || removed.contains(item.reference.messageId)) { reply.cleanupResponse=Response(f.fresh.source,{CleanupEmpty(item.reference.messageId,item.reference.chat)}); }
			else if(item.mark) {
				auto native=NativeMessage(); native.flags=MTP_flags(MTPDmessage::Flag::f_out|MTPDmessage::Flag::f_from_id);
				native.id=MTP_int(item.reference.messageId); native.date=MTP_int(item.date); native.peer_id=Target(item.reference.chat);
				native.from_id=MTP_peerUser(MTP_long(Origin.account.userId)); native.message=MTP_string(DesktopSyncChatMarkText());
				reply.cleanupResponse=Response(f.fresh.source,{native.Build()});
			} else {
				auto record=Receipt(f.fresh.source,f.original,item.reference.messageId,item.date);
				reply.cleanupResponse=record.response; reply.fresh->sourceRecord=record;
			}
		}
		return reply;
	}
	void step() {
		Must(f.progress.command);
		if(f.progress.command->dispatch && f.progress.command->cleanupPermit) {
			const auto permit=*f.progress.command->cleanupPermit; Must(permit.current()); const auto request=permit.request(); Must(request);
			CHECK(!permit.request()); ++deletes;
			auto reply=DesktopSyncCarryReply(); reply.command=*f.progress.command; reply.outcome=SyncCarryOutcome::Success;
			reply.cleanupResult=MTP_messages_affectedMessages(MTP_int(7),MTP_int(1)); ++applied; removed.emplace(permit.item().reference.messageId);
			f.engine->complete(reply);
		} else { f.engine->complete(readReply()); }
	}
	void finish() { for(auto n=0;f.progress.command && n!=30;++n) { step(); } }
	void toRead() { if(f.progress.command && !f.progress.command->cleanupRead && !f.progress.command->dispatch) { step(); } Must(f.progress.command && f.progress.command->cleanupRead); }
	void toPermit() { toRead(); step(); Must(f.progress.command && f.progress.command->cleanupPermit); }
	void restart() { f.engine.reset(); f.actualResolution(); f.newEngine(true); }
};

void CleanupFlows() {
	CHECK(!DesktopSyncChatCleanupActivated());
	{
		auto h=CleanupHarness(false); h.finish(); CHECK(h.f.progress.terminal && h.deletes==0);
	}
	for(const auto oldMark:{false,true}) {
		auto h=CleanupHarness(true,oldMark); h.finish();
		CHECK(h.f.progress.terminal && h.f.progress.journal->phase==SyncCarryPhase::Done && h.deletes==1+int(oldMark));
		CHECK(h.applied==h.deletes && !h.f.gate->postingHeld());
	}
	for(auto shape=0;shape!=15;++shape) {
		auto h=CleanupHarness(true,false,SyncCarryMode::Switch,shape==12); h.toRead(); auto reply=h.readReply(); const auto item=reply.command.cleanupRead->item;
		switch(shape) {
		case 0: reply.cleanupResponse=Response(h.f.fresh.source,{}); break;
		case 1: reply.cleanupResponse=Response(h.f.fresh.source,{CleanupEmpty(item.reference.messageId,{})}); break;
		case 2: reply.cleanupResponse=Response(h.f.fresh.source,{CleanupEmpty(item.reference.messageId,PeerToken{PeerTokenKind::Chat,999})}); break;
		case 3: reply.cleanupResponse=Response(h.f.fresh.source,{CleanupEmpty(item.reference.messageId+1,item.reference.chat)}); break;
		case 4: reply.cleanupResponse=Response(h.f.fresh.source,{CleanupEmpty(item.reference.messageId,item.reference.chat),CleanupEmpty(item.reference.messageId,item.reference.chat)}); break;
		case 5: reply.cleanupResponse=Response(h.f.fresh.source,{CarryDocument(h.f.fresh.source,h.f.original,item.reference.messageId,int(item.date)+1)}); break;
		case 6: reply.fresh->sourceRecord=Receipt(h.f.fresh.source,RecordBytes(2),item.reference.messageId,item.date); reply.cleanupResponse=reply.fresh->sourceRecord->response; break;
		case 7: reply.fresh->source.peer.origin.login+=u'x'; break;
		case 8: reply.destination->inventory.scan.status=SyncHistoryScanStatus::InvalidResponse; break;
		case 9: reply.fresh->target.peer.facts->otherMembers.reset(); break;
		case 10: reply.fresh->source.peer.facts->protectedContent=true; break;
		case 11: reply.destination=InventoryRows(h.f.fresh.target,{}); break;
		case 12: case 13: case 14: {
			auto native=NativeMessage(); native.id=MTP_int(item.reference.messageId); native.date=MTP_int(item.date);
			native.peer_id=Target(item.reference.chat); native.from_id=MTP_peerUser(MTP_long(shape==12?999:Origin.account.userId));
			native.flags=MTP_flags(MTPDmessage::Flag::f_from_id); native.message=MTP_string("ordinary sentinel");
			if(shape!=14) {
				auto doc=NativeDocument(); doc.id=MTP_long(99); doc.access_hash=MTP_long(789); doc.dc_id=MTP_int(2); doc.size=MTP_long(h.f.original.size());
				doc.attributes=MTP_vector<MTPDocumentAttribute>({MTP_documentAttributeFilename(MTP_string(shape==13?"settings.toml":SyncSettingsRecordFileName()))});
				auto media=NativeMessageMediaDocument(); media.flags=MTP_flags(MTPDmessageMediaDocument::Flag::f_document);media.document=doc.Build();
				native.flags=MTP_flags(MTPDmessage::Flag::f_from_id|MTPDmessage::Flag::f_media);native.media=media.Build();
			}
			reply.cleanupResponse=Response(h.f.fresh.source,{native.Build()});reply.fresh->sourceRecord->response=*reply.cleanupResponse;
		} break;
		}
		h.f.engine->complete(reply); CHECK(!h.f.progress.command && !h.f.progress.terminal && h.deletes==0);
		CHECK(h.f.progress.cleanup.pending && h.f.progress.cleanup.canAct && h.f.gate->postingHeld());
		h.f.engine->skipCleanup(true); CHECK(h.f.progress.terminal && h.f.progress.cleanup.skipped && !h.f.gate->postingHeld());
	}
	for(const auto skip:{false,true}) {
		auto h=CleanupHarness(); h.toRead(); auto reply=h.readReply(); reply.cleanupResponse=Response(h.f.fresh.source,{}); h.f.engine->complete(reply);
		CHECK(h.f.progress.cleanup.canAct); h.restart();
		if(skip) { h.f.engine->skipCleanup(true); }
		else { h.finish(); }
		CHECK(h.f.progress.terminal && h.f.progress.cleanup.skipped==skip && !h.f.gate->postingHeld());
	}
	{
		auto h=CleanupHarness(); h.toPermit(); const auto old=*h.f.progress.command;
		auto reply=DesktopSyncCarryReply(); reply.command=old; reply.outcome=SyncCarryOutcome::Unknown; h.absent=true; ++h.deletes;
		h.f.engine->complete(reply); CHECK(h.f.progress.cleanup.canAct && !h.f.progress.terminal);
		h.restart(); h.finish(); CHECK(h.deletes==1 && h.f.progress.terminal);
		h.f.engine->complete(reply); CHECK(h.deletes==1 && h.f.progress.terminal);
	}
	{
		auto h=CleanupHarness(); h.gone=true; h.finish(); CHECK(h.deletes==0 && h.f.progress.terminal && h.f.progress.journal->cleanupChatGone);
	}
	for(auto mutation=0;mutation!=3;++mutation) {
		auto h=CleanupHarness(); h.toRead(); auto reply=h.readReply(); reply.cleanupResponse.reset(); h.f.engine->complete(reply); h.f.engine.reset();
		const auto path=mutation==0?h.f.root+u"/state.json"_q:mutation==1?h.f.scope.cacheFile:h.f.root+u"/carry.json"_q;
		Must(WriteSyncPrivateFile(path,"invalid") == SyncStoreStatus::Ready);
		h.f.newEngine(true); h.f.engine->skipCleanup(true); CHECK(!h.f.progress.terminal && h.deletes==0);
	}
}

void CleanupPoliciesAndSavedResult() {
	for(const auto date:{200,201}) {
		auto h=CleanupHarness(false,false,SyncCarryMode::Follow,true,date);
		CHECK(h.f.progress.journal->plan->cleanup==SyncCarryCleanup::OwnLatePosts);
		h.finish(); CHECK(h.f.progress.terminal && h.deletes==int(date>200));
	}
	for(const auto mode:{SyncCarryMode::Follow,SyncCarryMode::Fill}) {
		auto h=CleanupHarness(false,false,mode,false);
		CHECK(h.f.progress.journal->plan->cleanup==SyncCarryCleanup::None); h.finish(); CHECK(h.f.progress.terminal && h.deletes==0);
	}
	{
		auto h=CleanupHarness(true,true); h.toPermit(); h.step(); h.toRead(); h.step(); h.toRead(); auto reply=h.readReply();
		reply.cleanupResponse=Response(h.f.fresh.source,{});h.f.engine->complete(reply);
		CHECK(h.f.progress.cleanup.pending && h.f.progress.cleanup.confirmed==1 && h.deletes==1);
		h.f.engine->skipCleanup(true); CHECK(h.f.progress.terminal && h.f.progress.cleanup.skipped && h.deletes==1);
		CHECK(h.f.progress.journal->abandonedCleanup.size()==1 && h.f.progress.journal->abandonedCleanup.front().messageId==9);
	}
	{
		auto h=CleanupHarness(); h.toRead(); h.f.engine.reset();
		{
			auto store=SyncChatCarryStore(h.f.root,h.f.scope); Must(store.Open());
			auto flow=DesktopSyncChatCleanup(store,Origin,h.f.admitted);auto destination=InventoryRows(h.f.fresh.target,h.f.cloud);
			Must(flow.prepare(h.f.fresh,destination,{})); const auto read=flow.next();Must(read);
			destination=InventoryRows(h.f.fresh.target,h.f.cloud);
			Must(flow.inspect(*read,h.f.fresh,destination,{},Response(h.f.fresh.source,{CleanupEmpty(read->item.reference.messageId,read->item.reference.chat)}),{}));
			CHECK(flow.deleted() && flow.deleted()->size()==1 && !flow.permit());
		}
		h.f.newEngine(true);h.finish();CHECK(h.f.progress.terminal && h.deletes==0);
	}
}

void CleanupLedgerControls() {
	for(auto mutation=0;mutation!=3;++mutation) {
		auto h=CleanupHarness(); h.toPermit(); h.f.engine.reset();
		const auto path=h.f.root+u"/carry.json"_q;
		auto document=QJsonDocument::fromJson(CaptureSyncPrivateFile(path,kSyncCarryJournalStorageLimit).bytes).object();
		const auto journal=ParseSyncCarryJournal(QJsonDocument(document.value(u"journal"_q).toObject()).toJson());Must(journal);
		auto transport=document.value(u"transport"_q).toObject();auto cleanup=transport.value(u"cleanup"_q).toObject();
		if(mutation==0) { cleanup.insert(u"version"_q,2); }
		if(mutation==1) {
			const auto entries=cleanup.value(u"entries"_q).toObject();Must(!entries.isEmpty());
			const auto hash=entries.begin().value().toObject().value(u"body_sha256"_q).toString();
			Must(WriteSyncPrivateFile(h.f.root+u"/carry-objects/record-"_q+hash+u".json"_q,"corrupt")==SyncStoreStatus::Ready);
		}
		if(mutation==2) { cleanup.insert(u"extension"_q,7); }
		transport.insert(u"cleanup"_q,cleanup);const auto hash=DesktopSyncCarryTransportHash(transport,document,journal.journal);Must(hash);
		document.insert(u"transport"_q,transport);document.insert(u"transport_sha256"_q,*hash);
		Must(WriteSyncPrivateFile(path,QJsonDocument(document).toJson())==SyncStoreStatus::Ready);
		h.f.newEngine(true);
		if(mutation!=2) { CHECK(!h.f.progress.command && !h.f.progress.terminal && h.deletes==0); }
		else {
			h.finish();CHECK(h.f.progress.terminal && h.deletes==1);
			auto preserved=false;
			for(const auto &image:h.f.progress.retainedImages) {
				const auto saved=QJsonDocument::fromJson(image).object();
				preserved|=saved.value(u"transport"_q).toObject().value(u"cleanup"_q).toObject().value(u"extension"_q)==QJsonValue(7);
			}
			CHECK(preserved);
		}
	}
}

void CleanupDurability() {
	for(const auto point:{SyncDurablePoint::BeforeWrite,SyncDurablePoint::BeforeRename,SyncDurablePoint::DirectoryBarrier}) {
		for(const auto absent:{false,true}) {
			auto h=CleanupHarness(); h.toRead(); h.absent=absent;
			h.f.failPoint=point; h.f.failWrite=h.f.writes+1; h.f.failing=false; h.f.armed=true;
			h.step(); CHECK(!h.f.progress.terminal && !h.f.progress.command && h.deletes==0);
			CHECK(h.f.progress.cleanup.pending && h.f.progress.cleanup.canAct && h.f.progress.storeStatus!=SyncStoreStatus::Ready);
			h.f.armed=false; h.f.failWrite=-1; h.f.failing=false;
			h.f.engine->skipCleanup(true); CHECK(h.f.progress.terminal && h.f.progress.cleanup.skipped && !h.f.gate->postingHeld());
		}
	}
}

void CleanupDirectoryReplacement() {
	auto h = CleanupHarness();
	h.toRead();
	auto reply = h.readReply();
	const auto moved = h.f.root + u"-original"_q;
	Must(QDir().rename(h.f.root, moved));
	Must(QDir().mkdir(h.f.root));
	h.f.engine->complete(reply);
	CHECK(h.f.progress.storeStatus == SyncStoreStatus::Changed);
	CHECK(h.f.progress.cleanup.pending && h.f.progress.cleanup.canAct);
	CHECK(!h.f.progress.command && !h.f.progress.terminal && h.deletes == 0);
	Must(QDir().rmdir(h.f.root));
	Must(QDir().rename(moved, h.f.root));
	h.f.engine->cancel();
	CHECK(!h.f.gate->postingHeld() && h.deletes == 0);
}

void CleanupRenewal() {
	for(auto control=0;control!=5;++control) {
		auto h=CleanupHarness(); h.toPermit(); const auto command=*h.f.progress.command; const auto permit=*command.cleanupPermit;
		auto sender=CleanupSenderProbe(); auto timers=std::deque<Fn<void()>>(); Fn<void(std::optional<DesktopSyncCleanupPermit>)> grant;
		int completed=0,ready=0; bool allowed=false;
		Must(ProbeCleanupTransport(sender,permit,[&](auto done){grant=std::move(done);},[](auto task){task();},
			[&](int millis,auto task){CHECK(millis==60000);timers.push_back(std::move(task));},[&](auto response){++completed; auto reply=DesktopSyncCarryReply(); reply.command=command; reply.outcome=response?SyncCarryOutcome::Success:SyncCarryOutcome::Unknown; reply.cleanupResult=response; h.f.engine->complete(reply);}));
		const auto read=h.f.engine->beginCleanupRenew(command,permit); Must(read);
		CHECK(!permit.current() && permit.operationCurrent());
		auto reply=h.f.reply(); reply.command.cleanupRead=read;
		sender.gate([&](bool value){++ready;allowed=value;}); Must(grant); Must(!timers.empty());
		if(control==0) { timers.front()(); CHECK(completed==1 && ready==1 && !allowed && sender.sends==1 && h.f.progress.cleanup.canAct); }
		if(control==1) { h.f.engine->cancel(); }
		if(control==2) { h.f.admitted->store(false); }
		if(control==3) { sender.fail(MTP::Error::Local(u"PURPLE_EFFECT_OUTCOME_UNKNOWN"_q,{})); }
		const auto record=Receipt(h.f.fresh.source,h.f.original,read->item.reference.messageId,read->item.date);
		reply.cleanupResponse=record.response; reply.fresh->sourceRecord=record;
		const auto renewed=h.f.engine->renewCleanup(command,permit,*read,reply);
		if(renewed) { permit.revoke(); CHECK(renewed->current()); }
		grant(renewed);
		if(control==4) { CHECK(allowed && ready==1 && completed==0); sender.done(MTP_messages_affectedMessages(MTP_int(1),MTP_int(1))); CHECK(completed==1); }
		else { CHECK(!allowed && completed==1); }
		timers.front()(); CHECK(completed==1 && ready<=1);
	}
}

void CleanupCompletionRelease() {
	for (const auto throws : {false, true}) {
		auto h = CleanupHarness();
		h.toPermit();
		const auto permit = *h.f.progress.command->cleanupPermit;
		auto sender = CleanupSenderProbe();
		auto completions = 0;
		Must(ProbeCleanupTransport(sender, permit, [](auto done) { done({}); },
			[](auto task) { task(); }, [](int, auto) {}, [&](auto) {
				++completions;
				if (throws) { throw 17; }
				h.f.engine->cancel();
				h.restart();
				h.finish();
			}));
		auto caught = false;
		try { sender.done(MTP_messages_affectedMessages(MTP_int(1), MTP_int(1))); }
		catch (int value) { caught = value == 17; }
		CHECK(caught == throws && completions == 1 && !permit.current());
		sender.done(MTP_messages_affectedMessages(MTP_int(1), MTP_int(1)));
		CHECK(completions == 1);
		if (throws) { h.f.engine->cancel(); }
		CHECK(!h.f.gate->postingHeld());
		if (!throws) { CHECK(h.f.progress.terminal && h.deletes == 1); }
	}
}

}
int main(int argc,char **argv) {
	auto app=QCoreApplication(argc,argv); std::setbuf(stdout,nullptr); CHECK(!SyncStoreWorkerThread()); CleanupChoiceOff();
	auto worker=std::thread([]{CleanupFlows();CleanupPoliciesAndSavedResult();CleanupLedgerControls();CleanupDurability();CleanupDirectoryReplacement();CleanupRenewal();CleanupCompletionRelease();});worker.join();
	std::printf("T5G CLEANUP %d checks, %d failures\n",Checks,Failures);return Failures?1:0;
}
