#include "embedded_consent.inc"

namespace {
int ReviewChecks = 0;
int ReviewFailures = 0;
void Review(bool ok, const char *name) {
 ++ReviewChecks;
 if (!ok) { ++ReviewFailures; std::fprintf(stderr, "REVIEW FAIL %s\n", name); }
}
void ReviewerActions(const DesktopSyncChatSetupSnapshot &frame, const DesktopSyncChatPeer &peer, bool fill) {
 auto action = DesktopSyncChatConsentAction(frame, u"review-window"_q, [] { return true; }, [] { return true; });
 const auto offer = action.offer(peer, u"config"_q); Must(offer);
 Review(!offer->askReaders && offer->firstPost == fill, "no-reader disclosure shape");
 const auto noDisclosure = action.accept(*offer, frame, peer, DesktopSyncChatConsentChoice::Proceed, false);
 Review(noDisclosure.has_value() == !fill, "Fill requires first-post disclosure even without other readers");
 if (fill) { Review(action.accept(*offer, frame, peer, DesktopSyncChatConsentChoice::Proceed, true).has_value(), "disclosed Fill accepted"); }
}
void BasicTarget(ExecutorFiles &f) {
 f.fresh.target=Route({PeerTokenKind::Chat,456});
 for(auto &mark:f.fresh.resolutionInput.marks){mark.location.chat=f.fresh.target.peer.chat;}
 f.start.target=f.fresh.target.peer.chat;
 Readers(f,1,std::vector<QString>{QString(64,u'a')});
}
void BasicHarness(FlowHarness &h) {
 BasicTarget(h.files);h.frame=Frame(h.files);h.peer=h.files.fresh.target.peer;
}
void ReviewerWorker() {
 for (const auto mode : {SyncCarryMode::Follow, SyncCarryMode::Fill}) {
  for (auto mutation = 0; mutation != 4; ++mutation) {
   auto h = FlowHarness(mode); h.start();
   for (auto n=0; h.inspections < 1 && !h.queue.empty() && n!=20; ++n) {
    auto task=std::move(h.queue.front());h.queue.pop_front();task();
   }
   Must(h.inspections == 1);
   if (mutation==0) { h.peer.readerMembers=std::vector<QString>{QString(64,u'b')}; }
   if (mutation==1) { h.peer.readerMembers.reset(); }
   if (mutation==2) { h.peer.creator=false;h.peer.facts->creator=false;h.peer.soleAdmin=false;h.peer.facts->soleAdmin=false;h.peer.facts->otherAdmins=1; }
   if (mutation==3) { h.frame.cacheImage += ' '; }
   h.drain();
   Review(h.starts==0 && h.sends==0 && h.files.outward==0, "late final admission change refuses all work");
   Review(CaptureSyncPrivateFile(h.files.root+u"/carry.json"_q,kSyncCarryJournalStorageLimit).absent(), "late refusal creates no journal");
  }
  {
   auto h=FlowHarness(mode);h.holdReply=true;h.start();h.drain();Must(h.reply);
   const auto old=h.reply;const auto reply=h.pending;
   old(reply);h.drain();const auto sent=h.sends;const auto writes=h.files.writes;
   old(reply);h.drain();
   Review(h.sends==sent && h.files.writes==writes, "duplicate old callback admits nothing");
   h.flow->close();h.drain();Review(!h.files.gate->postingHeld(), "duplicate callback leaves lease releasable");
  }
  {
   auto h=FlowHarness(mode);BasicHarness(h);
   auto writer=h.files.gate->writer();Must(writer);
   h.start();h.drain();
   Review(h.starts==1 && h.sends==0 && h.reason==u"publishers-not-quiescent"_q,"active publisher defers open");
   writer.reset();
   const auto changed=DesktopSyncChatPauseImage({},Origin.account,h.login,false,true);Must(changed);
   Must(WriteSyncPrivateFile(h.files.scope.cacheFile,*changed)==SyncStoreStatus::Ready);
   h.flow->retry();h.drain();
   std::printf("RETRY_CHANGED_CACHE mode=%d outward=%d terminal=%d reason=%s\n",int(mode),h.files.outward,int(h.files.progress.terminal),qPrintable(h.reason));
   Review(h.files.outward==0 && !h.files.progress.terminal,"retry retains accepted opening cache witness");
  }
  {
   auto h=FlowHarness(mode);BasicHarness(h);auto writer=h.files.gate->writer();Must(writer);
   h.start();h.drain();Must(h.reason==u"publishers-not-quiescent"_q);writer.reset();
   Readers(h.files,1,std::vector<QString>{QString(64,u'b')});
   h.flow->retry();h.drain();
   const auto cache=ParseDesktopSyncChatCache(CaptureSyncPrivateFile(h.files.scope.cacheFile,65536).bytes,Origin.account);
   std::printf("RETRY_CHANGED_READERS mode=%d outward=%d terminal=%d baseline_new=%d warning=%d\n",int(mode),h.files.outward,int(h.files.progress.terminal),int(cache.record && cache.record->readerMembers==std::optional(std::vector<QString>{QString(64,u'b')})),int(cache.record && cache.record->newReaders));
   Review(h.files.outward==0 && !h.files.progress.terminal,"retry preserves originally accepted roster before first intent");
  }
  for (auto mutation=0;mutation!=3;++mutation) {
   auto f=ExecutorFiles(false,mode);BasicTarget(f);
   f.engine->open(f.start,f.fresh.target.peer,u"library"_q);
   const auto baseline=TransportReaders(f);
   for(auto n=0;f.progress.command && !f.progress.command->dispatch && n!=30;++n){f.step();}
   Must(f.progress.command && f.progress.command->dispatch);f.step();
   const auto effects=f.outward;
   if(mutation==0){f.fresh.target.peer.readerMembers.reset();f.fresh.target.peer.facts->otherMembers=200;}
   if(mutation==1){f.fresh.target.peer.creator=false;f.fresh.target.peer.facts->creator=false;f.fresh.target.peer.soleAdmin=false;f.fresh.target.peer.facts->soleAdmin=false;f.fresh.target.peer.facts->otherAdmins=1;}
   if(mutation==2){f.fresh.target.peer.facts->otherMembers.reset();}
   f.run();
   std::printf("POST_INTENT_LOSS mode=%d mutation=%d outward=%d original=%d terminal=%d reason=%s\n",int(mode),mutation,f.outward,effects,int(f.progress.terminal),qPrintable(f.progress.reason));
   Review(!f.progress.terminal && !f.progress.command && f.outward==effects, "post-intent loss of reader authority holds");
   Review(TransportReaders(f)==baseline, "held reader loss preserves disclosed baseline");
   if(f.progress.terminal){ continue; }
   f.engine.reset();f.newEngine(true);f.run();
   Review(!f.progress.terminal && !f.progress.command && f.outward==effects, "restart cannot refresh away lost reader authority");
   Review(TransportReaders(f)==baseline, "restart preserves exact original baseline");
  }
 }
 {
  auto f=ExecutorFiles();f.engine.reset();
  const auto image=DesktopSyncChatPauseImage({},Origin.account,f.scope.loginAuthority,true,true);Must(image);
  Must(WriteSyncPrivateFile(f.scope.cacheFile,*image)==SyncStoreStatus::Ready);
  ForgetDesktopSyncChatLogin(f.scope.cacheFile,Origin.account,QString(64,u'b'));
  const auto after=CaptureSyncPrivateFile(f.scope.cacheFile,65536).bytes;
  Review(after==*image && ParseDesktopSyncChatCache(after,Origin.account).paused,"unrelated login cleanup preserves negative Pause");
 }
}
}
int main(int argc,char **argv) {
 auto app=QCoreApplication(argc,argv);std::setbuf(stdout,nullptr);
 for(const auto mode:{SyncCarryMode::Follow,SyncCarryMode::Fill}) {
  DesktopSyncChatSetupSnapshot frame;DesktopSyncChatPeer peer;
  auto prepare=std::thread([&]{auto f=ExecutorFiles(false,mode);Readers(f,0,std::vector<QString>{});frame=Frame(f);peer=f.fresh.target.peer;});prepare.join();
  ReviewerActions(frame,peer,mode==SyncCarryMode::Fill);
 }
 auto worker=std::thread([]{ReviewerWorker();});worker.join();
 std::printf("REVIEWER CONSENT %d checks, %d failures; fixture %d checks, %d failures\n",ReviewChecks,ReviewFailures,Checks,Failures);
 return ReviewFailures || Failures ? 1 : 0;
}
