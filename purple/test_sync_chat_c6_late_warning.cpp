#include "create-fixture.inc"
int main() {
 for(const auto resume : {Step::Pin,Step::Mute,Step::Archive}) {
  auto h=Injected();const auto stopped=h.At(resume);
  auto cache=DesktopSyncChatSetupCache(h.saved,Fresh().scope,u"Synthetic"_q);
  CHECK(cache && !cache->newReaders);
  h.Restart();h.executor->Start(1000);
  auto laterCommits=0;
  for(auto n=0;n!=20;++n) {
   const auto command=h.executor->Next();if(!command)break;
   auto reply=h.Reply(*command);
   if(command->step==Step::PeerInfo)reply.peer->facts->otherMembers=1;
   if(command->step==Step::Commit) {
    ++laterCommits;cache=DesktopSyncChatSetupCache(h.saved,Fresh().scope,u"Synthetic"_q);
   }
   CHECK(h.executor->Complete(reply,1000));
  }
  std::printf("resume_step=%d phase=%d journal_warning=%d later_commits=%d cached_warning=%d\n",int(resume),int(h.executor->phase()),h.saved.newReaders,laterCommits,cache?cache->newReaders:false);
  CHECK(h.executor->phase()==Phase::Complete);
  CHECK(h.saved.newReaders);
  CHECK(cache && cache->newReaders);
  auto model=DesktopSyncChatDiscovery(Origin,cache,false,{}, {},QString(64,u'a'));
  model.StartBeforePost(1100);
  const auto command=model.command();CHECK(command.step==DesktopSyncChatStep::ExactMark);
  auto reply=DesktopSyncChatReply{Origin,command.ticket,true};
  reply.peer=Route(*h.saved.chat).peer;reply.chatReadable=true;reply.messages={CreatedMark(*h.saved.chat)};
  CHECK(model.Complete(reply,1100));
  CHECK(model.status().newReaders);
 }
 std::printf("C6 LATE WARNING %d checks, %d failures\n",Checks,Failures);
 return Failures?1:0;
}
