#define main AuthorMain
#include "tree/purple/test_sync_chat_discovery.cpp"
#undef main

struct ProbeHint : base::has_weak_ptr {
 DesktopSyncChatReply reply;
 bool Accept(const DesktopSyncChatCommand &) { return true; }
 void Peers(const MTPmessages_Messages &) {}
 std::vector<MTPMessage> Messages(const MTPmessages_Messages &response) {
  auto result = std::vector<MTPMessage>();
  response.match([](const MTPDmessages_messagesNotModified &) {}, [&](const auto &data) { result.assign(data.vmessages().v.begin(),data.vmessages().v.end()); });
  return result;
 }
 void Peer(const DesktopSyncChatCommand &, Fn<void(std::optional<DesktopSyncChatPeer>)> done) { done(::Peer({})); }
 void Complete(DesktopSyncChatReply value) { reply=std::move(value); }
 void Response(const DesktopSyncChatCommand &command, const MTPmessages_Messages &response) {
  const auto weak = base::make_weak(this);
  const auto completed = [=](const MTPmessages_Messages &response)
#include "hint-callback.inc"
  ; completed(response);
 }
};

struct ProbeScanReply : base::has_weak_ptr {
 bool _done=false;
 int _requestId=1;
 SyncHistoryPages _pages;
 Fn<void(uint64_t)> _progress;
 Fn<SyncChatMessageOwnership(const MTPMessage &)> _owned;
 SyncHistoryScanStatus status=SyncHistoryScanStatus::Cancelled;
 void Finish(SyncHistoryScanStatus value) { status=value; _done=true; }
 void RequestNext() {}
 void Response(const MTPmessages_Messages &result) {
  const auto weak=base::make_weak(this);
  const auto completed=[weak](const MTPmessages_Messages &result)
#include "scanner-callback.inc"
  ; completed(result);
 }
};

MTPMessage SettingsHit(bool forwarded, int id=20) {
 auto native=NativeMessage(); native.id=MTP_int(id);native.date=MTP_int(200);
 native.peer_id=MTP_peerUser(MTP_long(123));native.message=MTP_string("#purplesync");
 native.flags=MTP_flags(MTPDmessage::Flag::f_media|(forwarded?MTPDmessage::Flag::f_fwd_from:MTPDmessage::Flag()));
 auto media=NativeMessageMediaDocument();media.flags=MTP_flags(MTPDmessageMediaDocument::Flag::f_document);
 media.document=MTP_document(MTP_flags(MTPDdocument::Flags()),MTP_long(10),MTP_long(20),MTP_bytes(QByteArray()),MTP_int(100),MTP_string("application/json"),MTP_long(10),MTP_vector<MTPPhotoSize>(),MTP_vector<MTPVideoSize>(),MTP_int(1),MTP_vector<MTPDocumentAttribute>(1,MTP_documentAttributeFilename(MTP_string("purple.json"))));
 native.media=media.Build();return native.Build();
}

void ExcludedHint() {
 auto discovery=DesktopSyncChatDiscovery(Origin,{},false,{},{},QString(64,u'a'));discovery.Start(1000);
 auto inventory=false;
 for(auto n=0;n<15 && discovery.command().step!=DesktopSyncChatStep::Idle;++n) {
  const auto command=discovery.command();auto reply=DesktopSyncChatReply{Origin,command.ticket,true};
  if(command.step==DesktopSyncChatStep::Search) {auto empty=NativeMessages();reply.page=MapDesktopSyncChatSearchPage(Origin,empty.Build(),command.search,command.search==DesktopSyncChatSearch::Global?std::optional<PeerToken>{}:std::optional(PeerToken{}));}
  else if(command.step==DesktopSyncChatStep::PeerInfo) reply.peer=Peer(command.chat);
  else if(command.step==DesktopSyncChatStep::SettingsHint) {
   auto response=NativeMessagesSlice();response.count=MTP_int(2);response.messages=MTP_vector<MTPMessage>({SettingsHit(true)});
   auto service=ProbeHint();service.Response(command,response.Build());reply=service.reply;
   std::printf("Q_EXCLUDED total=2 returned=1 success=%d absent=%d\n",reply.success,reply.hint==SyncChatSettingsHint::Absent);
  } else if(command.step==DesktopSyncChatStep::Inventory) {inventory=true;reply.records=SyncChatRecordPresence::Present;reply.chatReadable=true;}
  CHECK(discovery.Complete(reply,1000));
 }
 std::printf("Q_EXCLUDED_RESULT unset=%d complete=%d inventory=%d held=%d\n",discovery.status().phase==DesktopSyncChatPhase::Unset,discovery.status().complete,inventory,discovery.status().postingHeld);
 CHECK(discovery.status().phase!=DesktopSyncChatPhase::Unset || inventory);
}

void InexactInventory() {
 for(auto mode=0;mode<3;++mode) {
  auto response=NativeMessagesSlice();response.count=MTP_int(mode==0?0:2);
  if(mode==1) response.flags=MTP_flags(MTPDmessages_messagesSlice::Flag::f_inexact);
  if(mode==2) {response.flags=MTP_flags(MTPDmessages_messagesSlice::Flag::f_search_flood);response.search_flood=MTP_searchPostsFlood(MTP_flags(MTPDsearchPostsFlood::Flags()),MTP_int(1),MTP_int(2),MTP_int(3),MTP_long(4));}
  auto scanner=ProbeScanReply();scanner._owned=[](const MTPMessage &){return SyncChatMessageOwnership::Own;};scanner.Response(response.Build());
  auto scan=SyncHistoryScanResult();scan.status=scanner.status;scan.candidateIds=scanner._pages.candidates();
  auto read=SyncCandidateReadResult();read.status=AggregateSyncCandidateRead({});
  const auto result=FinishSyncAccountInventory(123,scan,read);
  std::printf("INVENTORY_SLICE mode=%d scannerComplete=%d inventoryComplete=%d records=%zu\n",mode,scan.complete(),result.status==SyncAccountInventoryStatus::Complete,result.read->records.size());
  auto model=DesktopSyncChatDiscovery(Origin,{},false,{},{},QString(64,u'a'));model.Start(1000);
  auto scenario=Scenario{{Mark({},10)}};scenario.inventory=result.read->records.empty()?SyncChatRecordPresence::Empty:SyncChatRecordPresence::Present;Drive(model,scenario,1000);
  std::printf("INVENTORY_RESOLUTION mode=%d use=%d complete=%d held=%d\n",mode,model.status().phase==DesktopSyncChatPhase::Use,model.status().complete,model.status().postingHeld);
  CHECK(mode==0?!model.status().postingHeld:model.status().postingHeld);
  CHECK(mode==0?result.status==SyncAccountInventoryStatus::Complete:result.status!=SyncAccountInventoryStatus::Complete);
 }
}

void Boundaries() {
 auto self=Peer({});
 for(auto invalid=0;invalid<4;++invalid) {
  auto value=NativeMessage();value.id=MTP_int(invalid==0?0:10);value.date=MTP_int(invalid==1?0:100);value.peer_id=MTP_peerUser(MTP_long(invalid==2?124:123));
  if(invalid==3) {value.flags=MTP_flags(MTPDmessage::Flag::f_media);value.media=MTP_messageMediaUnsupported();}
  CHECK(GetSyncChatMessageOwnership(MapDesktopSyncChatMessage(Origin,self,value.Build()))==SyncChatMessageOwnership::Unknown);
 }
 auto model=DesktopSyncChatDiscovery(Origin,Cache(),true,{},{},QString(64,u'a'));model.Start(1000);
 auto scenario=Scenario();scenario.failGlobal=true;Drive(model,scenario,1000);
 CHECK(model.status().postingHeld && model.cache()->mark==Cache().mark);
 CHECK(std::none_of(scenario.commands.begin(),scenario.commands.end(),[](auto command){return command.step==DesktopSyncChatStep::SettingsHint;}));
 for(auto delay : {30,120,600}) {auto input=SyncChatResolveInput();input.selfUserId=123;input.cache=Cache().mark;input.cacheRead=SyncChatDirectRead::MarkGone;input.searchRetriesCompleted=delay==30?0:delay==120?1:2;CHECK(ResolveSyncChat(input).retryAfterSeconds==delay);}
}

void CappedFallback() {
 auto dialogs=std::vector<PeerToken>();for(auto id=0;id<61;++id) dialogs.push_back({PeerTokenKind::Channel,uint64_t(1000+id)});
 auto model=DesktopSyncChatDiscovery(Origin,{},false,{},dialogs,QString(64,u'a'));model.Start(1000);
 auto global=0,self=0,broadcasts=0,fallback=0,hints=0;
 for(auto step=0;step<100;++step) {
  const auto command=model.command();if(command.step==DesktopSyncChatStep::Idle) break;
  auto reply=DesktopSyncChatReply{Origin,command.ticket,true};
  if(command.step==DesktopSyncChatStep::Search) {
   if(command.search==DesktopSyncChatSearch::Global) {reply.page=Page(10000-global*100,2001);++global;}
   else {if(command.search==DesktopSyncChatSearch::Self)++self;else if(command.search==DesktopSyncChatSearch::Broadcasts)++broadcasts;else ++fallback;auto empty=DesktopSyncChatSearchPage();empty.recognized=empty.full=true;reply.page=empty;}
  } else if(command.step==DesktopSyncChatStep::PeerInfo) reply.peer=Peer(command.chat);
  else if(command.step==DesktopSyncChatStep::SettingsHint) {++hints;reply.hint=SyncChatSettingsHint::Absent;}
  CHECK(model.Complete(reply,1000));
 }
 CHECK(global==20 && self==1 && broadcasts==1 && fallback==50);
 CHECK(model.status().phase==DesktopSyncChatPhase::Paused && !model.status().complete && model.status().postingHeld && !model.cache());
 std::printf("CAP_CONTROL R1=%d R2=%d broadcasts=%d F1=%d incomplete=%d\n",global,self,broadcasts,fallback,!model.status().complete);
}

void HintControls() {
 for(auto mode=0;mode<3;++mode) {
  auto probe=ProbeHint();auto command=DesktopSyncChatCommand{Origin,9,DesktopSyncChatStep::SettingsHint};
  auto response=NativeMessages();if(mode>0)response.messages=MTP_vector<MTPMessage>({SettingsHit(false)});
  auto native=response.Build();
  if(mode==2) {auto slice=NativeMessagesSlice();slice.flags=MTP_flags(MTPDmessages_messagesSlice::Flag::f_inexact);slice.count=MTP_int(2);slice.messages=MTP_vector<MTPMessage>({SettingsHit(false)});native=slice.Build();}
  probe.Response(command,native);
  CHECK(mode==2?!probe.reply.success&&probe.reply.hint==SyncChatSettingsHint::Unknown:probe.reply.success&&probe.reply.hint==(mode==0?SyncChatSettingsHint::Absent:SyncChatSettingsHint::Found));
 }
}

void IncompleteNewWinner() {
 auto cache=Cache();auto model=DesktopSyncChatDiscovery(Origin,cache,true,{},{},QString(64,u'a'));model.Start(1000);
 auto scenario=Scenario{{Mark({},1),Mark({PeerTokenKind::Channel,456},2,200)}};scenario.failGlobal=true;Drive(model,scenario,1000);
 CHECK(model.status().postingHeld&&!model.status().complete&&model.status().phase==DesktopSyncChatPhase::Paused);
 CHECK(SerializeDesktopSyncChatCache(*model.cache())==SerializeDesktopSyncChatCache(cache));
}

int main() {Boundaries();CappedFallback();HintControls();IncompleteNewWinner();ExcludedHint();InexactInventory();std::printf("INDEPENDENT %d checks, %d failures\n",Checks,Failures);return Failures?1:0;}
