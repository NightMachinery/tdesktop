#define PURPLE_SYNC_CHAT_SETUP_FLOW_EMBEDDED
#include "test_sync_chat_setup_flow.cpp"

namespace {
void CancelUnlockControl() {
 for(const auto step:{Step::Resolve,Step::Create,Step::PostMark,Step::Pin}) {
  auto h=FlowHarness();h.Open();const auto command=h.At(step);const auto before=h.JournalBytes();
  {auto held=QLockFile(h.directory+u"/lock"_q);held.setStaleLockTime(0);CHECK(!held.tryLock(0));}
  h.admitted->store(false);h.flow->cancel();
  CHECK(h.last.phase==Phase::Cancelled&&!h.last.command);
  CHECK(h.JournalBytes()==before);
  auto other=QLockFile(h.directory+u"/lock"_q);other.setStaleLockTime(0);const auto released=other.tryLock(0);
  std::printf("CANCEL_UNLOCK step=%d released=%d flow_alive=1\n",int(command.step),released);
  CHECK(released);
  other.unlock();h.flow.reset();
  auto after=QLockFile(h.directory+u"/lock"_q);after.setStaleLockTime(0);CHECK(after.tryLock(0));after.unlock();
 }
}
void ResumeOnlyNoJournalControl(){
 auto h=FlowHarness();h.Open(true);CHECK(h.last.phase==Phase::Held&&h.last.reason==u"setup-no-journal"_q);
 auto lock=QLockFile(h.directory+u"/lock"_q);lock.setStaleLockTime(0);const auto released=lock.tryLock(0);
 std::printf("NO_JOURNAL released=%d executor_present=0\n",released);CHECK(released);lock.unlock();
 for(int n=0;n!=3;++n){h.flow->resume(1000);CHECK(h.last.reason==u"setup-no-journal"_q&&!h.last.command);}
 CHECK(SyncPathIdentity(h.directory+u"/sync-chat-setup.json"_q)=="missing");
}
void PartialFailureControls(){
 for(const auto step:{Step::Create,Step::PostMark,Step::Pin,Step::Mute,Step::Archive})for(const auto outcome:{Outcome::Rejected,Outcome::Ambiguous}){
  auto h=FlowHarness();h.Open();const auto command=h.At(step);auto reply=SetupReply(command,h.opening);reply.outcome=outcome;h.flow->complete(reply,1000);
  CHECK(h.last.phase==Phase::Held&&!h.last.command&&h.last.journal);
  CHECK(h.last.journal->randomId==command.journal.randomId);
  h.flow.reset();h.Open(true);CHECK(h.last.reason==u"setup-resume-required"_q);h.flow->resume(1000);h.Pump();
  CHECK(h.last.phase==(step==Step::Create&&outcome==Outcome::Ambiguous?Phase::Held:Phase::Complete));
  CHECK(h.last.journal&&h.last.journal->randomId==command.journal.randomId);
  if(step!=Step::Create){CHECK(std::count(h.effects.begin(),h.effects.end(),Step::Create)==1);}
 }
}

}

namespace {
struct ProbeButton {
	bool disabled = true;
	bool hidden = false;
	void setDisabled(bool value) { disabled = value; }
	void hide() { hidden = true; }
};
struct ProbeLabel {
	QString text;
	void setText(const QString &value) { text = value; }
};
struct ProbeService {
	bool *destroyed = nullptr;
	explicit ProbeService(bool *destroyed) : destroyed(destroyed) {}
	~ProbeService() { *destroyed = true; }
};
struct ChooserProbe {
	std::unique_ptr<ProbeService> service;
	DesktopSyncChatSetupProgress progress;
	uint64_t serviceGeneration = 1;
	bool busy = true;
	bool current = true;
	bool *destroyed = nullptr;
	ProbeLabel *status = nullptr;
	ProbeButton *accept = nullptr;
	ProbeButton *check = nullptr;
	ProbeButton *retry = nullptr;
	ProbeButton *self = nullptr;
	ProbeButton *consent = nullptr;
};
bool Current(const ChooserProbe &state) {
	CHECK(!state.service && *state.destroyed);
	return state.current;
}
#include "setup-restore-fresh.inc"

struct ServiceTransportProbe {
	bool fenced = false;
	void cancel() { fenced = true; }
};
struct ServiceWorkerProbe {
	std::shared_ptr<std::atomic<bool>> admitted;
	ServiceTransportProbe *transport = nullptr;
	bool *released = nullptr;
	~ServiceWorkerProbe() {
		CHECK(!admitted->load() && transport->fenced);
		*released = true;
	}
};
struct ServiceResumeProbe {
	std::shared_ptr<std::atomic<bool>> _admitted = std::make_shared<std::atomic<bool>>(true);
	std::unique_ptr<ServiceTransportProbe> _transport = std::make_unique<ServiceTransportProbe>();
	std::unique_ptr<ServiceWorkerProbe> _worker;
	void finishResumeLookup();
};
#include "setup-finish-resume.inc"

void EmptyResumeChooser() {
	for (const auto current : {false, true}) {
		auto h = FlowHarness(); h.Open(true);
		CHECK(h.last.resumeAbsent && h.last.reason == u"setup-no-journal"_q);
		CHECK(!h.last.command && !h.last.journal);
		CHECK(SyncPathIdentity(h.directory + u"/sync-chat-setup.json"_q) == "missing");
		const auto empty = h.last;
		auto released = false;
		auto owner = ServiceResumeProbe();
		owner._admitted = h.admitted;
		owner._worker = std::make_unique<ServiceWorkerProbe>();
		owner._worker->admitted = owner._admitted;
		owner._worker->transport = owner._transport.get();
		owner._worker->released = &released;
		owner.finishResumeLookup();
		CHECK(released && !owner._worker && !owner._admitted->load() && owner._transport->fenced);
		h.flow->resume(1000);
		CHECK(!h.last.command && SyncPathIdentity(h.directory + u"/sync-chat-setup.json"_q) == "missing");
		auto destroyed = false;
		auto label = ProbeLabel();
		auto accept = ProbeButton(), check = ProbeButton(), retry = ProbeButton();
		auto self = ProbeButton(), consent = ProbeButton();
		auto state = ChooserProbe();
		state.service = std::make_unique<ProbeService>(&destroyed);
		state.progress = empty;
		state.current = current;
		state.destroyed = &destroyed;
		state.status = &label; state.accept = &accept; state.check = &check;
		state.retry = &retry; state.self = &self; state.consent = &consent;
		CHECK(!RestoreFreshSetup(&state, 0, u"none pending"_q));
		CHECK(state.service && !destroyed && accept.disabled && check.disabled);
		CHECK(RestoreFreshSetup(&state, 1, u"none pending"_q));
		CHECK(!state.service && destroyed && !state.busy && !state.progress.resumeAbsent);
		CHECK(accept.disabled == !current && retry.disabled == !current && !check.disabled);
		CHECK(self.hidden && consent.hidden && label.text == u"none pending"_q);
		CHECK(!RestoreFreshSetup(&state, 1, u"stale"_q));
		if (current) {
			h.flow.reset(); h.admitted = std::make_shared<std::atomic<bool>>(true); h.Open();
			CHECK(h.last.command && h.last.command->step == Step::Resolve);
			CHECK(SyncPathIdentity(h.directory + u"/sync-chat-setup.json"_q) != "missing");
			h.Pump(); CHECK(h.last.phase == Phase::Complete);
		}
	}
	{
		auto h = FlowHarness(); h.Open(); const auto command = h.At(Step::Pin);
		h.flow.reset(); h.Open(true);
		CHECK(!h.last.resumeAbsent && h.last.journal && h.last.reason == u"setup-resume-required"_q);
		auto destroyed = false;
		auto state = ChooserProbe();
		state.service = std::make_unique<ProbeService>(&destroyed);
		state.progress = h.last;
		CHECK(!RestoreFreshSetup(&state, state.serviceGeneration, u"not absent"_q));
		CHECK(state.service && !destroyed);
		h.flow->resume(1000); h.Pump();
		CHECK(h.last.phase == Phase::Complete && h.last.journal->randomId == command.journal.randomId);
		CHECK(std::count(h.effects.begin(), h.effects.end(), Step::Create) == 1);
	}

	{
		auto destroyed = false;
		auto state = ChooserProbe();
		state.service = std::make_unique<ProbeService>(&destroyed);
		state.progress.resumeAbsent = true;
		state.serviceGeneration = 3;
		CHECK(!RestoreFreshSetup(&state, 1, u"old completion"_q));
		CHECK(state.service && !destroyed);
		state.progress.resumeAbsent = false;
		CHECK(!RestoreFreshSetup(&state, 3, u"not empty"_q));
		CHECK(state.service && !destroyed);
	}
}
}
int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	auto worker = std::thread([] {
		CancelUnlockControl(); ResumeOnlyNoJournalControl(); PartialFailureControls(); EmptyResumeChooser();
	});
	worker.join();
	std::printf("Q2A LIFECYCLE %d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
