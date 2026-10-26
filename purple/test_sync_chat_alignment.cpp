#define PURPLE_SYNC_CHAT_WRITE_EMBEDDED
#include "test_sync_chat_write.cpp"
#include <QtCore/QJsonArray>

namespace {

MTPMessage AlignmentMark(const DesktopSyncChatPeer &peer, int id=1, const char *text="#purplechatsync") {
	auto value=NativeMessage();value.id=MTP_int(id);value.date=MTP_int(100);
	value.flags=MTP_flags(MTPDmessage::Flag::f_out|MTPDmessage::Flag::f_from_id);
	value.from_id=Target({});value.peer_id=Target(peer.chat);value.message=MTP_string(text);return value.Build();
}

DesktopSyncChatPeer GroupMembers(std::vector<int64_t> members, bool creator=true) {
	auto input=DesktopSyncChatPeerInput{Origin,{PeerTokenKind::Chat,789}};
	input.canSendFiles=true;input.chosenSendAs=PeerToken{};
	auto chat=NativeChat();chat.id=MTP_long(789);chat.flags=MTP_flags(creator?MTPDchat::Flag::f_creator:MTPDchat::Flag());
	chat.participants_count=MTP_int(1+members.size());
	auto values=QVector<MTPChatParticipant>();
	values.push_back(creator?MTPChatParticipant(MTP_chatParticipantCreator(MTP_flags(MTPDchatParticipantCreator::Flags()),MTP_long(123),MTP_string()))
		:MTPChatParticipant(MTP_chatParticipantAdmin(MTP_flags(MTPDchatParticipantAdmin::Flags()),MTP_long(123),MTP_long(456),MTP_int(100),MTP_string())));
	for(const auto id:members)values.push_back(MTP_chatParticipant(MTP_flags(MTPDchatParticipant::Flags()),MTP_long(id),MTP_long(123),MTP_int(100),MTP_string()));
	auto full=NativeChatFull();full.id=MTP_long(789);full.participants=MTP_chatParticipants(MTP_long(789),MTP_vector<MTPChatParticipant>(values),MTP_int(1));
	input.group=chat.Build();input.groupFull=full.Build();return MapDesktopSyncChatPeer(Origin,input);
}

DesktopSyncChatCacheRecord Consented(const DesktopSyncChatPeer &peer) {
	auto value=Cache(peer.chat);value.readerConsent=true;value.readerCreator=peer.creator;
	value.readers=DesktopSyncChatEligibility(peer)->readers;value.readerMembers=peer.readerMembers;return value;
}

struct ScopeProbe {
	DesktopSyncChatOrigin _origin=Origin;
	QString _root=u"synthetic"_q;
	QByteArray _binding="binding-a";
	QString _loginBinding=QString(64,u'a');
	std::optional<AccountKey> _owner=Origin.account;
	AccountWorkMode _mode=AccountWorkMode::Own;
	DesktopSyncChatDiscoveryScope ScopeFor(const DesktopSyncChatDisk &disk,
		const std::optional<DesktopSyncChatCacheRecord> &cache) const
#include "service-scope.inc"
};

namespace AlignmentClock { namespace unixtime {
int64_t value=1000;
int64_t now() {return value;}
} }

struct ReceiptProbe : ScopeProbe {
	DesktopSyncChatStatus _status=Usable({});
	std::optional<DesktopSyncChatDiscoveryReceipt> _recent;
	bool _directBeforePost=false;
	int restarts=0;
	int delivered=0;
	void Begin(bool retry) {CHECK(!retry);++restarts;}
	void Complete(bool saved,std::shared_ptr<DesktopSyncChatDisk> disk,
		std::optional<DesktopSyncChatCacheRecord> cache) {
		const auto self=this;
#define base AlignmentClock
#include "service-completion.inc"
#undef base
		++delivered;
	}
};

struct SelectionProbe : ScopeProbe {
	std::optional<DesktopSyncChatDiscoveryReceipt> _recent;
	bool _beforePost=true;
	bool _directBeforePost=false;
	std::unique_ptr<DesktopSyncChatDiscovery> _model;
	void Start(const DesktopSyncChatDisk &disk,DesktopSyncChatCacheRecord cache,int64_t now) {
		_model=std::make_unique<DesktopSyncChatDiscovery>(_origin,cache,disk.settingsBound,disk.settingsChat,std::vector<PeerToken>(),_loginBinding);
		const auto self=this;const auto scope=ScopeFor(disk,cache);
#include "service-selection.inc"
	}
};

void ServiceReceiptTransitions() {
	auto disk=std::make_shared<DesktopSyncChatDisk>();disk->settingsBound=true;disk->settingsSpace=u"space-a"_q;
	const auto cache=Cache();auto scope=ScopeProbe();const auto opening=scope.ScopeFor(*disk,cache);
	const auto receipt=DesktopSyncChatDiscoveryReceipt{opening,1000};
	for(auto mode=0;mode<9;++mode) {
		auto service=ReceiptProbe();service._recent=receipt;service._directBeforePost=mode!=1;
		AlignmentClock::unixtime::value=mode==2?1900:1100;
		if(mode==3)service._status.complete=false;
		if(mode==4)service._status.postingHeld=true;
		if(mode==5)service._status.phase=DesktopSyncChatPhase::Lost;
		if(mode==8)service._binding="other";
		service.Complete(mode!=6,disk,mode==7?std::nullopt:std::optional(cache));
		if(mode==2 || mode==8) {CHECK(service.restarts==1 && service.delivered==0 && !service._recent);continue;}
		CHECK(service.restarts==0 && service.delivered==1);
		if(mode>=3 && mode<=7)CHECK(!service._recent);
		else CHECK(service._recent && service._recent->completedAt==(mode==1?1100:1000));
	}
	for(auto mode=0;mode<6;++mode) {
		auto service=SelectionProbe();service._recent=receipt;
		if(mode==1)service._recent.reset();
		if(mode==3)service._binding="other";
		if(mode==4)service._beforePost=false;
		if(mode==5)service._origin.login=u"other"_q;
		service.Start(*disk,cache,mode==2?1900:1100);
		CHECK(service._model->command().step==(mode==0?DesktopSyncChatStep::ExactMark:DesktopSyncChatStep::Search));
		CHECK(service._directBeforePost==(mode==0));
		CHECK(bool(service._recent)==(mode==0));
	}
}

void RecentScopes() {
	auto session=ScopeProbe();auto disk=DesktopSyncChatDisk();
	disk.settingsSpace=u"space-a"_q;disk.settingsBound=true;disk.settingsChat={PeerTokenKind::Channel,456};
	const auto opening=session.ScopeFor(disk,Cache(disk.settingsChat));
	const auto receipt=DesktopSyncChatDiscoveryReceipt{opening,1000};
	CHECK(ReuseDesktopSyncChatDiscovery(receipt,opening,1000));
	CHECK(ReuseDesktopSyncChatDiscovery(receipt,opening,1899));
	CHECK(!ReuseDesktopSyncChatDiscovery(receipt,opening,1900));
	CHECK(!ReuseDesktopSyncChatDiscovery(receipt,opening,999));
	CHECK(!ReuseDesktopSyncChatDiscovery({opening,0},opening,1000));
	for(auto mode=0;mode<15;++mode) {
		auto current=opening;
		if(mode==0)current.origin.account.userId=124;
		if(mode==1)current.origin.account.environment=1;
		if(mode==2)current.origin.login=u"other"_q;
		if(mode==3)current.root=u"other"_q;
		if(mode==4)current.binding="other";
		if(mode==5)current.loginBinding=QString(64,u'b');
		if(mode==6)current.owner=AccountKey{1,123};
		if(mode==7)current.mode=AccountWorkMode::Follow;
		if(mode==8)current.space=u"space-b"_q;
		if(mode==9)current.settingsBound=false;
		if(mode==10)current.settingsChat={PeerTokenKind::Channel,789};
		if(mode==11)current.mark->markId=2;
		if(mode==12)current.mark.reset();
		if(mode==13)current.carry=true;
		if(mode==14)current.mark->markDate=101;
		CHECK(!ReuseDesktopSyncChatDiscovery(receipt,current,1100));
	}
	auto invalid=opening;invalid.origin.login.clear();CHECK(!ReuseDesktopSyncChatDiscovery({invalid,1000},invalid,1100));
	invalid=opening;invalid.loginBinding=QString(64,u'g');CHECK(!ReuseDesktopSyncChatDiscovery({invalid,1000},invalid,1100));
	for(auto now=1100;now<1900;now+=100)CHECK(ReuseDesktopSyncChatDiscovery(receipt,opening,now));
	CHECK(receipt.completedAt==1000 && !ReuseDesktopSyncChatDiscovery(receipt,opening,1900));
}

void RealStoreScope() {
	auto directory=QTemporaryDir();directory.setAutoRemove(false);CHECK(directory.isValid());
	const auto path=directory.path()+u"/sync"_q;
	auto state=SyncLocalState();state.install=u"in-"_q+QString(26,u'a');state.space=u"sp-"_q+QString(26,u'a');
	state.createdDevice=u"Synthetic"_q;state.bindingToken=QString(32,u'a');
	auto store=SyncLocalStore(path);CHECK(store.Open(true).status==SyncStoreStatus::Uninitialized);CHECK(store.Initialize(state));
	const auto current=std::make_shared<std::atomic<bool>>(true);
	auto disk=OpenDesktopSyncChatDisk(directory.path(),path,path+u"/state.json"_q,Origin.account,state.bindingToken.toLatin1(),QString(64,u'a'),current,true);
	CHECK(disk->error.isEmpty() && disk->settingsBound && disk->settingsSpace==state.space);
	const auto cache=Cache();CHECK(SaveDesktopSyncChatDisk(disk,cache,current));
	auto service=ScopeProbe();const auto receipt=DesktopSyncChatDiscoveryReceipt{service.ScopeFor(*disk,cache),1000};
	auto next=state.configData;next.pending=InspectConfigPayload(ParseSyncEnvelope(RecordBytes())).version.key;
	CHECK(store.StageConfig(RecordBytes(),next,state.bindingToken.toLatin1()));
	auto refreshed=OpenDesktopSyncChatDisk(directory.path(),path,path+u"/state.json"_q,Origin.account,state.bindingToken.toLatin1(),QString(64,u'a'),current,true);
	CHECK(refreshed->error.isEmpty() && refreshed->stateBytes!=disk->stateBytes);
	CHECK(ReuseDesktopSyncChatDiscovery(receipt,service.ScopeFor(*refreshed,refreshed->cache),1100));
	auto warning=refreshed->cache;warning->newReaders=true;
	CHECK(ReuseDesktopSyncChatDiscovery(receipt,service.ScopeFor(*refreshed,warning),1100));
}

void FreshBeforePost() {
	for(const auto &peer:{Peer({}),Peer({PeerTokenKind::Channel,456}),GroupMembers({456,7890})}) {
		for(auto mode=0;mode<7;++mode) {
			auto cache=Consented(peer);
			if(mode==6) {cache=Consented(Peer({}));cache.mark={PeerToken{},0,0};}
			auto model=DesktopSyncChatDiscovery(Origin,cache,true,cache.mark.chat,{},QString(64,u'a'));
			model.StartBeforePost(1000);const auto command=model.command();
			CHECK(command.step==DesktopSyncChatStep::ExactMark && command.mark==cache.mark);
			model.StartBeforePost(1000);CHECK(model.command().ticket==command.ticket);
			auto reply=DesktopSyncChatReply{Origin,command.ticket,true};reply.peer=mode==6?Peer({}):peer;reply.chatReadable=true;
			if(command.mark->markId>0)reply.messages={AlignmentMark(*reply.peer)};
			if(mode==1)reply.success=false;
			if(mode==2)reply.peer->facts.reset();
			if(mode==3)reply.messages={MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flag::f_peer_id),MTP_int(1),Target(peer.chat))};
			if(mode==4)reply.messages={AlignmentMark(peer,1,"#purplechatsync v2")};
			if(mode==5) {
				auto retired=reply;retired.origin.login=u"other"_q;CHECK(!model.Complete(retired,1001));
				retired=reply;++retired.ticket;CHECK(!model.Complete(retired,1001));
			}
			CHECK(model.Complete(reply,1001));
			CHECK(model.command().step==DesktopSyncChatStep::Idle);
			CHECK(model.status().postingHeld==(mode>=1 && mode<=4));
			if(mode==0 || mode==5 || mode==6)CHECK(model.status().complete && model.status().phase==DesktopSyncChatPhase::Use);
			CHECK(model.cache() && model.cache()->mark==cache.mark);
		}
	}
	auto carry=Cache();carry.carry=QJsonObject();auto refused=DesktopSyncChatDiscovery(Origin,carry,true,{}, {},QString(64,u'a'));
	refused.StartBeforePost(1000);CHECK(refused.command().step==DesktopSyncChatStep::Idle && refused.status().postingHeld);
}

void ReaderWarnings() {
	const auto original=GroupMembers({456,7890});CHECK(original.readerMembers && original.readerMembers->size()==2);
	for(auto mode=0;mode<10;++mode) {
		auto cache=Consented(original);
		auto fresh=mode==1?GroupMembers({456,9999}):mode==2?GroupMembers({456}):mode==3?GroupMembers({456,7890},false):GroupMembers({456,7890,9999});
		if(mode==4)fresh.facts->otherMembers.reset();
		if(mode==5)fresh.facts->canSendFiles=false;
		if(mode==6)cache.readerConsent=false;
		if(mode==7)cache.readerMembers.reset();
		if(mode==8)cache.newReaders=true;
		if(mode==9) {cache.readers.populationUnknown=true;cache.readers.otherMembers.reset();cache.readerMembers.reset();}
		auto model=DesktopSyncChatDiscovery(Origin,cache,true,cache.mark.chat,{},QString(64,u'a'));model.StartBeforePost(1000);
		const auto command=model.command();auto reply=DesktopSyncChatReply{Origin,command.ticket,true};reply.peer=fresh;reply.chatReadable=true;reply.messages={AlignmentMark(fresh)};
		CHECK(model.Complete(reply,1001));
		const auto held=mode==4 || mode==5 || mode==6;
		CHECK(model.status().postingHeld==held);
		if(held) {CHECK(!model.cache()->newReaders);continue;}
		CHECK(model.status().newReaders==(mode!=2 && mode!=9));
		CHECK(model.cache()->readerConsent && model.cache()->readers==(mode==9?DesktopSyncChatEligibility(fresh)->readers:cache.readers));
		CHECK(model.cache()->readerMembers==cache.readerMembers || !cache.readerMembers);
		const auto bytes=SerializeDesktopSyncChatCache(*model.cache());CHECK(bytes);
		const auto parsed=ParseDesktopSyncChatCache(*bytes,Origin.account);CHECK(parsed.record && parsed.record->newReaders==(mode!=2 && mode!=9));
		auto restored=DesktopSyncChatDiscovery(Origin,parsed.record,true,cache.mark.chat,{},QString(64,u'a'));restored.StartBeforePost(1100);
		auto again=DesktopSyncChatReply{Origin,restored.command().ticket,true};again.peer=original;again.chatReadable=true;again.messages={AlignmentMark(original)};
		CHECK(restored.Complete(again,1101));CHECK(!restored.status().postingHeld && restored.status().newReaders==(mode!=2 && mode!=9));
	}
	auto legacy=Consented(original);const auto bytes=SerializeDesktopSyncChatCache(legacy);CHECK(bytes);
	for(const auto &field:{u"new_readers"_q,u"reader_creator"_q,u"reader_members"_q}) {
		auto object=QJsonDocument::fromJson(*bytes).object();object.remove(field);CHECK(ParseDesktopSyncChatCache(QJsonDocument(object).toJson(),Origin.account).record);
		object.insert(field,42);CHECK(!ParseDesktopSyncChatCache(QJsonDocument(object).toJson(),Origin.account).record);
	}
	for(const auto &bad:{QString(64,u'g'),QString(64,u'A'),QString(63,u'a')}) {
		auto object=QJsonDocument::fromJson(*bytes).object();object.insert(u"reader_members"_q,QJsonArray{bad});
		CHECK(!ParseDesktopSyncChatCache(QJsonDocument(object).toJson(),Origin.account).record);
	}
	CHECK(!NewDesktopSyncChatReaders(Consented(original),GroupMembers({456})));
}

void InventoryPageLimits() {
	for(const auto &route:{Route({}),Route({PeerTokenKind::Channel,456})}) {
		for(const auto total:{1,20,21,999,1000,1001}) {
			auto scan=ScanProbe(route);
			for(auto page=0;page<std::min(total,1000);++page) {
				const auto message=AlignmentMark(route.peer,2000-page,"ordinary");
				if(route.peer.chat.kind==PeerTokenKind::Channel) {auto response=NativeChannelMessages();response.count=MTP_int(total);response.messages=MTP_vector<MTPMessage>({message});scan.Reply(response.Build());}
				else {auto response=NativeMessagesSlice();response.count=MTP_int(total);response.messages=MTP_vector<MTPMessage>({message});scan.Reply(response.Build());}
			}
			CHECK((scan.status==SyncHistoryScanStatus::Complete)==(total<1000));
			const auto result=FinishSyncAccountInventory(123,{scan.status,scan._pages.candidates(),scan._pages.count()},SyncCandidateReadResult{SyncCandidateReadStatus::Complete,{}});
			const auto mapped=MapDesktopSyncChatInventoryReply(Origin,1,route.peer.chat,{route,result});
			CHECK(mapped.success==(total<1000));
			if(mapped.success)CHECK(mapped.records==SyncChatRecordPresence::Empty);
		}
	}
	auto r1=DesktopSyncChatSearchPages(Origin,DesktopSyncChatSearch::Global);
	for(auto page=0;page<20;++page)CHECK(r1.Add(Page(10000-page*100,2001))==(page==19?DesktopSyncChatPageStatus::Capped:DesktopSyncChatPageStatus::Next));
}

}

#ifndef PURPLE_SYNC_CHAT_SETUP_EMBEDDED
int main() {
	AuthorMain();RoutesAndWire();FullReadPath();ExactReadback();CandidateSeal();LedgerScope();
	ReceiptMatrix();UploadSchedules();PostReadbackSchedules();BorrowedRunProof();CounterScopes();
	ServiceReceiptTransitions();RecentScopes();RealStoreScope();FreshBeforePost();ReaderWarnings();InventoryPageLimits();
	std::printf("ALIGNMENT %d checks, %d failures\n",Checks,Failures);return Failures?1:0;
}

#endif
