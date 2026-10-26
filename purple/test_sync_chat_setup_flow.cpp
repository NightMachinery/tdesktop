#define PURPLE_SYNC_CHAT_CREATE_EMBEDDED
#include "test_sync_chat_create.cpp"
#include "purple/purple_sync_chat_setup_flow.h"
#include "purple/purple_sync_chat_setup_requests.h"

namespace {

DesktopSyncChatCreateReply SetupReply(const DesktopSyncChatCreateCommand &command,
		DesktopSyncChatSetupSnapshot opening) {
	auto reply = DesktopSyncChatCreateReply{command.origin, command.operation,
		command.ticket, Outcome::Okay};
	const auto &journal = command.journal;
	switch (command.step) {
	case Step::Resolve:
		if (journal.markId) {
			opening.status.phase = DesktopSyncChatPhase::Use;
			opening.status.postingHeld = false;
			opening.status.resolution.action = SyncChatAction::Use;
			opening.status.resolution.readChat = journal.chat;
			opening.status.resolution.cache = SyncChatCache{
				*journal.chat, journal.markId, journal.markDate ? journal.markDate : 100};
		}
		reply.resolution = opening;
		break;
	case Step::Create:
		reply.creationProof = DesktopSyncChatCreationProof::MatchedRequest;
		reply.updates = CreatedUpdates();
		break;
	case Step::ReconcileCreate:
		reply.outcome = Outcome::Ambiguous;
		break;
	case Step::PeerInfo: reply.peer = Route(*journal.chat).peer; break;
	case Step::Inventory:
		reply.inventory = DesktopSyncChatInventory{Route(*journal.chat),
			FinishSyncAccountInventory(123, {SyncHistoryScanStatus::Complete, {}, 0},
				SyncCandidateReadResult{SyncCandidateReadStatus::Complete,{}})};
		break;
	case Step::PostMark:
		reply.markId = 1; reply.randomId = journal.randomId;
		break;
	case Step::ReadMark:
	case Step::ReconcileMark:
		reply.markId = journal.markId ? journal.markId : 1;
		reply.randomId = journal.randomId;
		reply.readback = Response(Route(*journal.chat), {CreatedMark(*journal.chat)});
		break;
	default: break;
	}
	return reply;
}

DesktopSyncChatSetupResumeProof SetupProof(const DesktopSyncChatCreateCommand &command,
		const DesktopSyncChatSetupSnapshot &opening) {
	auto proof = DesktopSyncChatSetupResumeProof();
	const auto resolution = DesktopSyncChatCreateCommand{
		command.origin, command.operation, command.ticket, Step::Resolve, command.journal};
	proof.resolution = SetupReply(resolution, opening).resolution;
	if (command.journal.chat) {
		const auto peer = Route(*command.journal.chat).peer;
		proof.peers = {peer, peer};
		proof.inventory = DesktopSyncChatInventory{Route(*command.journal.chat),
			FinishSyncAccountInventory(123, {SyncHistoryScanStatus::Complete, {}, 0},
				SyncCandidateReadResult{SyncCandidateReadStatus::Complete,{}})};
		if (command.journal.markId) {
			proof.readback = Response(Route(*command.journal.chat), {CreatedMark(*command.journal.chat)});
		}
	}
	return proof;
}

struct FlowHarness {
	DesktopSyncChatSetupSnapshot opening = Fresh();
	DesktopSyncChatSetupSelection selection;
	DesktopSyncChatSetupProgress last;
	QString directory;
	QString statePath;
	std::shared_ptr<std::atomic<bool>> admitted = std::make_shared<std::atomic<bool>>(true);
	std::unique_ptr<DesktopSyncChatSetupFlow> flow;
	std::vector<Step> effects;

	explicit FlowHarness(DesktopSyncChatSetupChoice choice = DesktopSyncChatSetupChoice::Create) {
		opening.scope.root = QString::fromUtf8(qgetenv("TMPDIR"))
			+ u"/purple-q2a-"_q + QUuid::createUuid().toString(QUuid::Id128);
		CHECK(EnsureSyncPrivateDirectory(opening.scope.root) == SyncStoreStatus::Ready);
		directory = opening.scope.root + u"/sync"_q;
		statePath = directory + u"/local.json"_q;
		selection.choice = choice;
		if (choice != DesktopSyncChatSetupChoice::Create) {
			selection.peer = Route(choice == DesktopSyncChatSetupChoice::Self
				? PeerToken{} : PeerToken{PeerTokenKind::Chat,456}).peer;
			selection.readerConsent = true;
		}
	}
	void Open(bool resumeOnly = false) {
		flow = std::make_unique<DesktopSyncChatSetupFlow>(opening, selection, directory,
			statePath, admitted, [this](DesktopSyncChatSetupProgress progress) {
				last = std::move(progress);
			}, resumeOnly);
		flow->open(1000);
	}
	QByteArray JournalBytes() {
		auto result = QByteArray();
		CHECK(ReadSyncPrivateFile(directory + u"/sync-chat-setup.json"_q, 512*1024, result)
			== SyncStoreStatus::Ready);
		return result;
	}
	void Reply() {
		CHECK(last.command);
		if (!last.command) { return; }
		const auto command = *last.command;
		const auto saved = ParseDesktopSyncChatCreateJournal(JournalBytes(),
			DesktopSyncChatSetupAuthority(opening.scope), Origin.account);
		CHECK(saved && saved->operation == command.operation && saved->randomId == command.journal.randomId);
		if (command.step == Step::Create || command.step == Step::PostMark
			|| command.step == Step::Pin || command.step == Step::Mute || command.step == Step::Archive) {
			CHECK(saved && saved->pending);
		}
		effects.push_back(command.step);
		flow->complete(SetupReply(command, opening), 1000);
	}
	void Pump() {
		for (auto n=0; n!=40 && last.command; ++n) { Reply(); }
	}
	DesktopSyncChatCreateCommand At(Step step) {
		for (auto n=0; n!=40 && last.command; ++n) {
			if (last.command->step == step) { return *last.command; }
			Reply();
		}
		CHECK(false); return {};
	}
};

void WorkerFlow() {
	for (const auto choice : {DesktopSyncChatSetupChoice::Create,
		DesktopSyncChatSetupChoice::Self, DesktopSyncChatSetupChoice::Existing}) {
		auto h = FlowHarness(choice); h.Open(); h.Pump();
		CHECK(h.last.phase == Phase::Complete);
		CHECK(std::count(h.effects.begin(), h.effects.end(), Step::PostMark) == 1);
		for (const auto step : {Step::Pin,Step::Mute,Step::Archive}) {
			CHECK(std::count(h.effects.begin(), h.effects.end(), step)
				== (choice == DesktopSyncChatSetupChoice::Create ? 1 : 0));
		}
		auto cache = QByteArray();
		CHECK(ReadSyncPrivateFile(h.opening.scope.root+u"/"_q+DesktopSyncChatCacheFileName(Origin.account),
			64*1024,cache)==SyncStoreStatus::Ready);
		CHECK(ParseDesktopSyncChatCache(cache,Origin.account).record);
		auto lock = QLockFile(h.directory+u"/lock"_q); lock.setStaleLockTime(0);
		CHECK(lock.tryLock(0)); lock.unlock();
		h.flow->cancel();
	}
	for (const auto step : {Step::Create,Step::ApplyUpdates,Step::PostMark,Step::ReadMark,
		Step::Pin,Step::Mute,Step::Archive}) {
		for (const auto completed : {false,true}) {
			auto h = FlowHarness(); h.Open(); const auto command=h.At(step);
			if (completed) { h.Reply(); }
			h.flow.reset(); h.Open(true);
			CHECK(h.last.phase==Phase::Held && h.last.reason==u"setup-resume-required"_q && !h.last.command);
			h.flow->resume(1000); h.Pump();
			CHECK(h.last.phase == (!completed && step==Step::Create ? Phase::Held : Phase::Complete));
			CHECK(std::count(h.effects.begin(),h.effects.end(),Step::Create)<=1);
			CHECK(h.last.journal && h.last.journal->randomId==command.journal.randomId);
		}
	}
	{
		auto h=FlowHarness(); h.Open(true);
		CHECK(h.last.phase==Phase::Held && h.last.reason==u"setup-no-journal"_q && !h.last.command);
		CHECK(SyncPathIdentity(h.directory+u"/sync-chat-setup.json"_q)=="missing");
	}
	{
		auto h=FlowHarness(); h.Open(); const auto command=h.At(Step::Create);
		const auto before=h.JournalBytes(); h.admitted->store(false); h.flow->cancel();
		h.flow->complete(SetupReply(command,h.opening),1000);
		CHECK(!h.last.command && h.JournalBytes()==before);
		h.flow.reset(); h.admitted->store(true); h.Open(true);h.flow->resume(1000);h.Pump();
		CHECK(h.last.phase==Phase::Held && h.last.reason==u"outcome-unknown"_q
			&& !h.last.command && h.last.journal && h.last.journal->pending);
	}
	{
		auto h=FlowHarness(); h.Open(); auto command=h.At(Step::Create);
		auto reply=SetupReply(command,h.opening);reply.outcome=Outcome::Limit;h.flow->complete(reply,1000);
		CHECK(h.last.phase==Phase::Limit);h.flow->useSelf(1000);h.Pump();
		CHECK(h.last.phase==Phase::Complete && h.last.journal->chat==PeerToken{});
		CHECK(std::count(h.effects.begin(),h.effects.end(),Step::Pin)==0);
	}
	{
		auto h = FlowHarness(); h.Open(); const auto command = h.At(Step::Create);
		const auto path = h.directory + u"/sync-chat-setup.json"_q;
		const auto foreign = QByteArray("inert-external-journal-change");
		CHECK(WriteSyncPrivateFile(path, foreign) == SyncStoreStatus::Ready);
		h.flow->complete(SetupReply(command, h.opening), 1000);
		CHECK(h.last.phase == Phase::Held && !h.last.command);
		CHECK(h.JournalBytes() == foreign);
	}
	{
		auto h = FlowHarness();
		CHECK(EnsureSyncPrivateDirectory(h.directory) == SyncStoreStatus::Ready);
		const auto path = h.directory + u"/sync-chat-setup.json"_q;
		CHECK(WriteSyncPrivateFile(path, QByteArray("inert-invalid-journal")) == SyncStoreStatus::Ready);
		h.Open();
		CHECK(h.last.phase == Phase::Held && !h.last.command);
		CHECK(h.JournalBytes() == QByteArray("inert-invalid-journal"));
	}

}

void RenewProofs() {
	for (const auto step : {Step::Create,Step::PostMark,Step::Pin,Step::Mute,Step::Archive}) {
		auto h=FlowHarness();h.Open();const auto command=h.At(step);
		const auto original=SerializeDesktopSyncChatCreateJournal(*h.last.journal);
		CHECK(h.flow->renew(command,SetupProof(command,h.opening),1000));
		CHECK(h.flow->renew(command,SetupProof(command,h.opening),1000));
		const auto saved=ParseDesktopSyncChatCreateJournal(h.JournalBytes(),command.journal.authority,Origin.account);
		CHECK(saved && saved->randomId==command.journal.randomId && saved->createAttempts==command.journal.createAttempts);
		CHECK(saved && saved->pending);
		auto stale=command;stale.ticket++;
		CHECK(!h.flow->renew(stale,SetupProof(command,h.opening),1000));
		for (auto mode=0;mode!=7;++mode) {
			auto proof=SetupProof(command,h.opening);
			if(mode==0)proof.resolution->scope.origin.login+=u"other"_q;
			if(mode==1)proof.resolution->status.complete=false;
			if(mode==2)proof.resolution->scope.settingsBound=true;
			if(mode==3)proof.resolution->scope.carry=true;
			if(mode==4)proof.resolution->scope.binding+='x';
			if(mode==5)proof.resolution.reset();
			if(mode==6)proof.resolution->scope.space=u"bound"_q;
			CHECK(!h.flow->renew(command,proof,1000));
		}
		if(step==Step::PostMark) {
			for(auto mode=0;mode!=6;++mode) {
				auto proof=SetupProof(command,h.opening);
				if(mode==0)proof.inventory.reset();
				if(mode==1)proof.inventory->inventory.status=SyncAccountInventoryStatus::Incomplete;
				if(mode==2)proof.peers.back().facts->canSendFiles=false;
				if(mode==3)proof.peers.back().facts->otherMembers=1;
				if(mode==4)proof.inventory->route.peer.origin.login+=u"other"_q;
				if(mode==5)proof.peers.pop_back();
				CHECK(!h.flow->renew(command,proof,1000));
			}
		}
		if(step==Step::Pin||step==Step::Mute||step==Step::Archive) {
			auto proof=SetupProof(command,h.opening);proof.readback=Response(Route(*command.journal.chat),{CreatedMark(*command.journal.chat,5)});
			CHECK(!h.flow->renew(command,proof,1000));
			proof=SetupProof(command,h.opening);proof.peers.front().facts->otherMembers=1;
			CHECK(h.flow->renew(command,proof,1000));
			auto cache=QByteArray();CHECK(ReadSyncPrivateFile(h.opening.scope.root+u"/"_q+DesktopSyncChatCacheFileName(Origin.account),64*1024,cache)==SyncStoreStatus::Ready);
			CHECK(ParseDesktopSyncChatCache(cache,Origin.account).record->newReaders);
		}
		CHECK(original);
		h.Pump();CHECK(h.last.phase==Phase::Complete);
	}
	{
		auto h = FlowHarness(); h.Open(); const auto command = h.At(Step::PostMark);
		const auto foreign = QByteArray("inert-renewal-journal-change");
		CHECK(WriteSyncPrivateFile(h.directory + u"/sync-chat-setup.json"_q, foreign) == SyncStoreStatus::Ready);
		CHECK(!h.flow->renew(command, SetupProof(command, h.opening), 1000));
		CHECK(h.last.phase == Phase::Held && h.last.reason == u"journal-write-held"_q && !h.last.command);
		CHECK(h.JournalBytes() == foreign);
	}

}

void NativeRequests() {
	for(const auto chat:{PeerToken{},PeerToken{PeerTokenKind::User,456},PeerToken{PeerTokenKind::Chat,456},PeerToken{PeerTokenKind::Channel,456}}) {
		const auto route=Route(chat);const auto mark=DesktopSyncChatMarkRequest(route,1234);CHECK(mark);
		auto wire=mtpBuffer();mark->write(wire);auto from=wire.constData();const auto end=from+wire.size();
		CHECK(Read<MTPint>(from,end).v==int(mtpc_messages_sendMessage));
		CHECK(Read<MTPint>(from,end).v==int(MTPmessages_SendMessage::Flag::f_silent|MTPmessages_SendMessage::Flag::f_no_webpage));
		CHECK(Read<MTPInputPeer>(from,end).type()==route.input.type());
		CHECK(qs(Read<MTPstring>(from,end))==DesktopSyncChatMarkText());
		CHECK(Read<MTPlong>(from,end).v==1234 && from==end);
		CHECK(!DesktopSyncChatMarkRequest(route,0));
	}
	const auto route = Route({PeerTokenKind::Channel,456});
	for (const auto step : {Step::Pin, Step::Mute, Step::Archive}) {
		auto command = DesktopSyncChatCreateCommand();
		command.origin = Origin;
		command.step = step;
		command.journal.chat = route.peer.chat;
		command.journal.markId = 7;
		command.journal.created = true;
		const auto decoration = DesktopSyncChatDecorationRequest(command, route);
		CHECK(decoration);
		auto wire = mtpBuffer();
		std::visit([&](const auto &request) { request.write(wire); }, *decoration);
		auto from = wire.constData(); const auto end = from + wire.size();
		const auto type = Read<MTPint>(from, end).v;
		if (step == Step::Pin) {
			CHECK(type == int(mtpc_messages_updatePinnedMessage));
			CHECK(Read<MTPint>(from,end).v == int(MTPmessages_UpdatePinnedMessage::Flag::f_silent));
			CHECK(Read<MTPInputPeer>(from,end).type() == route.input.type());
			CHECK(Read<MTPint>(from,end).v == 7 && from == end);
		} else if (step == Step::Mute) {
			CHECK(type == int(mtpc_account_updateNotifySettings));
			CHECK(Read<MTPInputNotifyPeer>(from,end).c_inputNotifyPeer().vpeer().type() == route.input.type());
			const auto settings = Read<MTPInputPeerNotifySettings>(from,end);
			CHECK(settings.data().vmute_until()->v == std::numeric_limits<int32_t>::max());
			CHECK(mtpIsTrue(*settings.data().vsilent()) && from == end);
		} else {
			CHECK(type == int(mtpc_folders_editPeerFolders));
			const auto peers = Read<MTPVector<MTPInputFolderPeer>>(from,end);
			CHECK(peers.v.size() == 1 && peers.v.front().data().vfolder_id().v == 1);
			CHECK(peers.v.front().data().vpeer().type() == route.input.type() && from == end);
		}
		for (auto mode = 0; mode != 5; ++mode) {
			auto bad = command;
			if (mode == 0) { bad.journal.created = false; }
			if (mode == 1) { bad.journal.markId = 0; }
			if (mode == 2) { bad.journal.chat = PeerToken{}; }
			if (mode == 3) { bad.origin.login += u"other"_q; }
			if (mode == 4) { bad.step = Step::PostMark; }
			CHECK(!DesktopSyncChatDecorationRequest(bad, route));
		}
	}

	const auto updates=DecodeDesktopSyncChatSetupUpdates(CreatedUpdates());CHECK(updates);
	CHECK(updates && EncodeDesktopSyncChatSetupUpdates(*updates)==CreatedUpdates());
	CHECK(!DecodeDesktopSyncChatSetupUpdates(CreatedUpdates()+'x'));
	for(const auto &type:{u"INTERNAL"_q,u"PURPLE_EFFECT_OUTCOME_UNKNOWN"_q,u"NEW_ERROR"_q}) {
		CHECK(DesktopSyncChatSetupError(type)==Outcome::Ambiguous);
	}
	CHECK(DesktopSyncChatSetupError(u"PURPLE_EFFECT_RESUME_REFUSED"_q)==Outcome::Rejected);
}

}

#ifndef PURPLE_SYNC_CHAT_SETUP_FLOW_EMBEDDED
int main(int argc,char **argv) {
	auto app=QCoreApplication(argc,argv);
	NativeRequests();
	auto worker=std::thread([]{ WorkerFlow(); RenewProofs(); });worker.join();
	std::printf("Q2A FLOW %d checks, %d failures\n",Checks,Failures);
	return Failures?1:0;
}

#endif
