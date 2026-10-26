#include "create-fixture.inc"
int main() {
 for (const auto &route : {Route({}),Route({PeerTokenKind::User,456}),Route({PeerTokenKind::Chat,456}),Route({PeerTokenKind::Channel,456}),Route({PeerTokenKind::Channel,456},false)}) {
  const auto peerless=MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flags()),MTP_int(10),MTPPeer());
  const auto response=Response(route,{peerless});
  const auto chat=PeerTokenText(route.peer.chat).value_or(QString());
  const auto parsed=DesktopSyncChatRecordMessage(route,response,10);
  const auto metadata=InspectDesktopSyncExactMessage(response,10,route);
  auto exact=ExactProbe(route);exact.Reply(response,false);
  std::printf("route=%s selected=%d presence=%d callback_fresh=%d callback_presence=%d\n",qPrintable(chat),bool(parsed),int(metadata.presence),exact.result?exact.result->fresh:false,exact.result?int(exact.result->presence):-1);
  CHECK(!parsed);
  CHECK(metadata.presence==SyncAutoMessagePresence::Unknown);
  CHECK(exact.result && !exact.result->fresh && exact.result->presence==SyncAutoMessagePresence::Unknown);
  CHECK(!DesktopSyncChatRecordMessage(route,Response(route,{}),10));
  CHECK(InspectDesktopSyncExactMessage(Response(route,{}),10,route).presence==SyncAutoMessagePresence::Unknown);
  const auto explicitPeer=MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flag::f_peer_id),MTP_int(10),Target(route.peer.chat));
  CHECK(InspectDesktopSyncExactMessage(Response(route,{explicitPeer}),10,route).presence==SyncAutoMessagePresence::Absent);
 }
 const auto self=Route({});
 const auto peerless=MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flags()),MTP_int(10),MTPPeer());
 CHECK(InspectDesktopSyncExactMessage(Response(self,{peerless}),10,uint64_t(123)).presence==SyncAutoMessagePresence::Unknown);
 CHECK(InspectDesktopSyncExactMessage(Response(self,{}),10,uint64_t(123)).presence==SyncAutoMessagePresence::Unknown);
 std::printf("D8 RECORD EXACT %d checks, %d failures\n",Checks,Failures);
 return Failures?1:0;
}
