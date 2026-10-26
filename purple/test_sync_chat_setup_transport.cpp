#define PURPLE_SYNC_CHAT_SETUP_FLOW_EMBEDDED
#include "test_sync_chat_setup_flow.cpp"

namespace {

auto ResumeSnapshot = std::optional<DesktopSyncChatSetupSnapshot>();
bool ResumeContext = true;
std::optional<DesktopSyncChatSetupSnapshot> SyncChatSetupSnapshotOf(UploadSession *) { return ResumeSnapshot; }
bool SyncChatSetupContextCurrent(UploadSession *, const DesktopSyncChatDiscoveryScope &) { return ResumeContext; }

struct SetupPeerEndpoint {
	DesktopSyncChatOrigin origin;
	PeerToken chat;
	Fn<bool()> current;
	Fn<void(std::optional<DesktopSyncChatPeer>)> done;
	SetupPeerEndpoint(UploadSession *, DesktopSyncChatOrigin origin, PeerToken chat,
		Fn<bool()> current, Fn<void(std::optional<DesktopSyncChatPeer>)> done)
	: origin(origin), chat(chat), current(std::move(current)), done(std::move(done)) {}
	void Reply(std::optional<DesktopSyncChatPeer> peer) {
		CHECK(current());const auto callback=done;callback(std::move(peer));
	}
};
struct SetupInventoryEndpoint {
	DesktopSyncChatIo route;
	Fn<void(DesktopSyncChatInventory)> done;
	Fn<bool()> current;
	bool started=false;
	SetupInventoryEndpoint(UploadSession *, DesktopSyncChatIo route, Fn<void(uint64_t)>,
		Fn<void(DesktopSyncChatInventory)> done, Fn<bool()> current)
	: route(std::move(route)), done(std::move(done)), current(std::move(current)) {}
	void Start() { started=true; }
	void Reply(DesktopSyncChatInventory value) { CHECK(current()&&started);const auto callback=done;callback(std::move(value)); }
};
struct SetupReadApi {
	struct Pending {
		mtpBuffer wire;
		Fn<void(const MTPmessages_Messages &)> done;
		Fn<void(const MTP::Error &)> fail;
	};
	std::vector<Pending> pending;
	template <typename Request> struct Builder {
		SetupReadApi *api;Pending call;
		Builder &done(Fn<void(const MTPmessages_Messages &)> done) {call.done=std::move(done);return *this;}
		Builder &fail(Fn<void(const MTP::Error &)> fail) {call.fail=std::move(fail);return *this;}
		void send(){api->pending.push_back(std::move(call));}
	};
	template <typename Request> Builder<Request> request(Request request){
		auto value=Builder<Request>{this,{}};request.write(value.call.wire);return value;
	}
	void cancelAll(){pending.clear();}
};

struct SetupTransportProbe : base::has_weak_ptr {
	base::weak_ptr<UploadSession> _session;
	DesktopSyncChatDiscoveryScope _scope;
	DesktopSyncChatOperation _operation;
	Fn<bool()> _current=[]{return true;};
	SetupReadApi _api;
	std::optional<DesktopSyncChatCreateCommand> _command;
	Fn<void(DesktopSyncChatCreateReply)> _done=[](DesktopSyncChatCreateReply){};
	std::optional<DesktopSyncChatPeer> _peer;
	std::optional<std::pair<QString,QByteArray>> _createdReceipt;
	std::unique_ptr<SetupPeerEndpoint> _peerRequest;
	std::unique_ptr<SetupInventoryEndpoint> _inventory;
	uint64_t _generation=1;
	DesktopSyncChatSetupResumeProof _resumeProof;
	Fn<void(bool)> _resumeReady;
	Fn<void(DesktopSyncChatCreateCommand,DesktopSyncChatSetupResumeProof,Fn<void(bool)>)> _renew;

	SetupTransportProbe(UploadSession &session,FlowHarness &flow,DesktopSyncChatCreateCommand command)
	: _session(base::make_weak(&session)), _scope(flow.opening.scope), _command(command) {
		_operation={flow.directory,Origin.account,[this]{return current(_generation);}};
		_renew=[&flow](DesktopSyncChatCreateCommand command,DesktopSyncChatSetupResumeProof proof,Fn<void(bool)> done){
			done(flow.flow->renew(command,proof,1000));
		};
	}
	bool current(uint64_t generation)const;
	void cancel();
	std::optional<DesktopSyncChatIo> route()const {
		if (!_peer) { return std::nullopt; }
		auto value = Route(_peer->chat);
		value.peer = *_peer;
		return ValidDesktopSyncChatIo(value) ? std::optional(value) : std::nullopt;
	}
	void renewEffect(Fn<void(bool)> ready,uint64_t generation);
	void renewPeer(uint64_t generation,bool afterInventory);
	void renewMark(uint64_t generation);
	void approveRenewal(uint64_t generation);
	void finishRenewal(bool allowed,uint64_t generation);
};

#define DesktopSyncChatPeerRequest SetupPeerEndpoint
#define SyncAccountInventory SetupInventoryEndpoint
#include "setup-transport-current.inc"
#include "setup-transport-cancel.inc"
#include "setup-transport-renewEffect.inc"
#include "setup-transport-renewPeer.inc"
#include "setup-transport-renewMark.inc"
#include "setup-transport-approveRenewal.inc"
#include "setup-transport-finishRenewal.inc"
#undef DesktopSyncChatPeerRequest
#undef SyncAccountInventory

void SetupCallbackChains() {
	for(const auto step:{Step::Create,Step::PostMark,Step::Pin,Step::Mute,Step::Archive}) {
		for(auto mode=0;mode!=5;++mode) {
			auto h=FlowHarness();h.Open();const auto command=h.At(step);auto session=UploadSession();
			auto probe=SetupTransportProbe(session,h,command);ResumeContext=true;
			ResumeSnapshot=SetupProof(command,h.opening).resolution;
			int completions=0;bool allowed=false;
			probe.renewEffect([&](bool value){++completions;allowed=value;},probe._generation);
			CHECK(BeforePost && BorrowedProof);
			if(mode==1){ResumeSnapshot->scope.binding+='x';}
			if(mode==2){ResumeSnapshot->status.complete=false;}
			auto resolution=std::move(BeforePost);resolution(ResumeSnapshot->status);
			if(step!=Step::Create) {
				CHECK(probe._peerRequest && probe._peerRequest->origin==command.origin
					&& probe._peerRequest->chat==command.journal.chat);
				auto peer=Route(*command.journal.chat).peer;
				if(mode==3){peer.facts->canSendFiles=false;}
				probe._peerRequest->Reply(peer);
				if (mode == 3) {
					CHECK(completions == 1 && !allowed);
				} else if(step==Step::PostMark) {
					CHECK(probe._inventory && probe._inventory->route.peer.chat==command.journal.chat);
					auto inventory=*SetupProof(command,h.opening).inventory;
					if(mode==4){inventory.inventory.status=SyncAccountInventoryStatus::Incomplete;}
					probe._inventory->Reply(inventory);
					CHECK(probe._peerRequest);probe._peerRequest->Reply(peer);
				} else {
					CHECK(probe._api.pending.size()==1);
					const auto request=probe._api.pending.front();
					auto from=request.wire.constData();const auto end=from+request.wire.size();
					CHECK(Read<MTPint>(from,end).v==int(mtpc_channels_getMessages));
					CHECK(Read<MTPInputChannel>(from,end).c_inputChannel().vchannel_id().v==command.journal.chat->id);
					request.done(Response(Route(*command.journal.chat),{CreatedMark(*command.journal.chat,mode==4?5:0)}));
				}
			}
			const auto expected=step==Step::Create?(mode==0||mode==3||mode==4):mode==0;
			CHECK(completions==1 && allowed==expected);
			CHECK(h.last.command && h.last.command->ticket==command.ticket);
		}
	}
	{
		auto h=FlowHarness();h.Open();const auto command=h.At(Step::PostMark);auto session=UploadSession();
		auto probe=SetupTransportProbe(session,h,command);ResumeSnapshot=SetupProof(command,h.opening).resolution;
		int completions=0;probe.renewEffect([&](bool){++completions;},probe._generation);
		auto old=std::move(BeforePost);probe.cancel();old(ResumeSnapshot->status);
		CHECK(completions==0 && !probe._peerRequest && !probe._command);
	}
	{
		auto h=FlowHarness();h.Open();const auto command=h.At(Step::PostMark);auto session=UploadSession();
		auto probe=SetupTransportProbe(session,h,command);ResumeSnapshot=SetupProof(command,h.opening).resolution;
		int completions=0;probe.renewEffect([&](bool){++completions;},probe._generation);
		auto done=std::move(BeforePost);done(ResumeSnapshot->status);auto late=probe._peerRequest->done;
		probe.cancel();probe._command=command;probe._done=[](DesktopSyncChatCreateReply){};
		probe._resumeReady=[&](bool){++completions;};late(Route(*command.journal.chat).peer);
		CHECK(completions==0 && probe._resumeReady && probe._resumeProof.peers.empty());
	}
}

}

int main(int argc,char **argv) {
	auto app=QCoreApplication(argc,argv);
	auto worker=std::thread([]{SetupCallbackChains();});worker.join();
	std::printf("Q2A TRANSPORT %d checks, %d failures\n",Checks,Failures);
	return Failures?1:0;
}
