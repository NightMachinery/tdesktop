#define PURPLE_SYNC_CHAT_WRITE_EMBEDDED
#include "test_sync_chat_write.cpp"
#include "purple/purple_sync_chat_create.h"
#include "purple/purple_sync_chat_create_store.h"
#include "purple/purple_sync_chat_create_requests.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QUuid>
#include <thread>

namespace {

using Step = DesktopSyncChatCreateStep;
using Phase = DesktopSyncChatCreatePhase;
using Outcome = DesktopSyncChatCreateOutcome;

DesktopSyncChatSetupSnapshot Fresh() {
	auto snapshot = DesktopSyncChatSetupSnapshot();
	snapshot.scope.origin = Origin;
	snapshot.scope.loginBinding = QString(64, u'a');
	snapshot.scope.root = u"synthetic-root"_q;
	snapshot.status.complete = true;
	snapshot.status.phase = DesktopSyncChatPhase::Unset;
	snapshot.status.resolution.action = SyncChatAction::Unset;
	snapshot.input.settingsHint = SyncChatSettingsHint::Absent;
	snapshot.run = 1;
	return snapshot;
}

QByteArray CreatedUpdates(int mode = 0) {
	auto channel = NativeChannel(); channel.id = MTP_long(456);
	channel.title = MTP_string(mode == 1 ? "other" : "Purple sync");
	channel.flags = MTP_flags(MTPDchannel::Flag::f_access_hash
		| (mode == 2 ? MTPDchannel::Flag() : MTPDchannel::Flag::f_creator)
		| (mode == 3 ? MTPDchannel::Flag::f_megagroup : MTPDchannel::Flag::f_broadcast)
		| (mode == 4 ? MTPDchannel::Flag::f_min : MTPDchannel::Flag()));
	channel.access_hash = MTP_long(mode == 5 ? 0 : 789);
	channel.date = MTP_int(100); channel.photo = MTP_chatPhotoEmpty();
	auto chats = QVector<MTPChat>();
	if (mode != 6) chats.push_back(channel.Build());
	if (mode == 7) chats.push_back(channel.Build());
	auto updates = NativeUpdates(); updates.users = MTP_vector<MTPUser>(); updates.updates = MTP_vector<MTPUpdate>(); updates.chats = MTP_vector<MTPChat>(chats);
	updates.date = MTP_int(100); updates.seq = MTP_int(1);
	auto words = mtpBuffer();
	if (mode == 8) {
		auto combined=NativeUpdatesCombined();combined.chats=updates.chats;
		combined.users=updates.users;combined.updates=updates.updates;
		combined.date=MTP_int(100);combined.seq_start=MTP_int(1);combined.seq=MTP_int(1);
		MTPUpdates(combined.Build()).write(words);
	} else { MTPUpdates(updates.Build()).write(words); }
	return QByteArray(reinterpret_cast<const char*>(words.constData()), words.size() * sizeof(mtpPrime));
}

MTPMessage CreatedMark(PeerToken chat, int mode = 0) {
	auto message = NativeMessage();
	message.flags = MTP_flags(MTPDmessage::Flag::f_out | MTPDmessage::Flag::f_from_id);
	message.id = MTP_int(mode == 1 ? 2 : 1);
	message.date = MTP_int(mode == 2 ? 0 : 100);
	message.peer_id = Target(mode == 3 ? PeerToken{PeerTokenKind::User, 999} : chat);
	message.from_id = MTP_peerUser(MTP_long(mode == 4 ? 999 : 123));
	message.message = MTP_string(mode == 5 ? "#purplechatsync" : DesktopSyncChatMarkText());
	if (mode == 6) {
		message.flags = MTP_flags(message.flags.v | MTPDmessage::Flag::f_media);
		message.media = MTP_messageMediaPhoto(MTP_flags(MTPDmessageMediaPhoto::Flags()), MTPPhoto(), MTP_int(0), MTPDocument());
	}
	if (mode == 7) {
		message.flags = MTP_flags(message.flags.v | MTPDmessage::Flag::f_via_bot_id);
		message.via_bot_id = MTP_long(42);
	}
	if (mode == 8) {
		message.flags = MTP_flags(message.flags.v | MTPDmessage::Flag::f_ttl_period);
		message.ttl_period = MTP_int(100);
	}
	return message.Build();
}

DesktopSyncChatCreateJournal Journal(DesktopSyncChatSetupChoice choice = DesktopSyncChatSetupChoice::Create) {
	auto selection = DesktopSyncChatSetupSelection{choice};
	if (choice != DesktopSyncChatSetupChoice::Create) {
		selection.peer = Route(choice == DesktopSyncChatSetupChoice::Self
			? PeerToken{} : PeerToken{PeerTokenKind::User, 456}).peer;
		selection.readerConsent = true;
	}
	const auto journal = BeginDesktopSyncChatCreate(Fresh(), selection);
	CHECK(journal); return journal.value_or(DesktopSyncChatCreateJournal());
}

struct Injected {
	DesktopSyncChatCreateJournal saved;
	QByteArray bytes;
	bool current = true;
	int saves = 0;
	int failSave = 0;
	std::vector<Step> effects;
	std::unique_ptr<DesktopSyncChatSetupExecutor> executor;

	explicit Injected(DesktopSyncChatSetupChoice choice = DesktopSyncChatSetupChoice::Create)
	: saved(Journal(choice)) { bytes = SerializeDesktopSyncChatCreateJournal(saved).value_or(QByteArray()); Restart(); }

	void Restart() {
		const auto parsed = ParseDesktopSyncChatCreateJournal(bytes, saved.authority, Origin.account);
		CHECK(parsed);
		if (parsed) saved = *parsed;
		executor = std::make_unique<DesktopSyncChatSetupExecutor>(Origin, saved,
			[=](const DesktopSyncChatCreateJournal &j) {
				++saves;
				if (saves == failSave) return false;
				const auto serialized = SerializeDesktopSyncChatCreateJournal(j);
				CHECK(serialized);
				if (!serialized) return false;
				saved = j; bytes = *serialized; return true;
			}, [=] { return current; });
	}

	DesktopSyncChatCreateReply Reply(const DesktopSyncChatCreateCommand &command) {
		effects.push_back(command.step);
		CHECK(command.origin == Origin && command.operation == saved.operation);
		const auto persistent = ParseDesktopSyncChatCreateJournal(bytes, saved.authority, Origin.account);
		CHECK(persistent && persistent->randomId == command.journal.randomId);
		CHECK(!executor->Next());
		auto reply = DesktopSyncChatCreateReply{Origin, command.operation, command.ticket, Outcome::Okay};
		if (command.step == Step::Resolve) {
			reply.resolution = Fresh();
			if (saved.markId > 0) {
				reply.resolution->status.phase = DesktopSyncChatPhase::Use;
				reply.resolution->status.resolution.action = SyncChatAction::Use;
				reply.resolution->status.resolution.cache = SyncChatCache{*saved.chat, saved.markId, saved.markDate ? saved.markDate : 100};
				reply.resolution->status.resolution.readChat = saved.chat;
			}
		} else if (command.step == Step::Create || command.step == Step::ReconcileCreate) {
			CHECK(saved.pending && saved.step == Step::Create);
			reply.creationProof = DesktopSyncChatCreationProof::MatchedRequest;
			reply.updates = CreatedUpdates();
		} else if (command.step == Step::PeerInfo) {
			reply.peer = Route(*saved.chat).peer;
		} else if (command.step == Step::Inventory) {
			reply.inventory = DesktopSyncChatInventory{Route(*saved.chat),
				FinishSyncAccountInventory(123, {SyncHistoryScanStatus::Complete, {}, 0},
					SyncCandidateReadResult{SyncCandidateReadStatus::Complete,{}})};
		} else if (command.step == Step::PostMark) {
			CHECK(saved.pending && saved.step == Step::PostMark);
			reply.randomId = saved.randomId; reply.markId = 1;
		} else if (command.step == Step::ReadMark || command.step == Step::ReconcileMark) {
			reply.randomId = saved.randomId;
			reply.readback = Response(Route(*saved.chat), {CreatedMark(*saved.chat)}); reply.markId = 1;
		} else if (command.step == Step::Pin || command.step == Step::Mute || command.step == Step::Archive) {
			CHECK(saved.created && saved.chat == (PeerToken{PeerTokenKind::Channel, 456}));
			CHECK(saved.markId == 1 && saved.markDate == 100 && saved.pending);
			const auto cache=DesktopSyncChatSetupCache(saved,Fresh().scope,u"Synthetic"_q);
			CHECK(cache && cache->readerConsent);
			CHECK(cache && SerializeDesktopSyncChatCache(*cache));
		} else if (command.step == Step::Commit) {
			CHECK(saved.markId == 1 && saved.markDate == 100 && saved.pending);
			const auto cache=DesktopSyncChatSetupCache(saved,Fresh().scope,u"Synthetic"_q);
			CHECK(cache && cache->readerConsent);
			CHECK(cache && SerializeDesktopSyncChatCache(*cache));
		}
		if (command.step == Step::Create || command.step == Step::PostMark
			|| command.step == Step::Pin || command.step == Step::Mute || command.step == Step::Archive) {
			const auto route = saved.chat ? std::optional(Route(*saved.chat)) : std::nullopt;
			CHECK(DesktopSyncChatSetupRequest(command, route));
		}
		return reply;
	}

	void Pump(int stop = 100) {
		for (auto n = 0; n != stop; ++n) {
			const auto command = executor->Next();
			if (!command) return;
			const auto reply = Reply(*command);
			CHECK(executor->Complete(reply, 1000));
			CHECK(!executor->Complete(reply, 1000));
		}
	}

	DesktopSyncChatCreateCommand At(Step target) {
		executor->Start(1000);
		for (auto n = 0; n != 30; ++n) {
			const auto command = executor->Next(); CHECK(command);
			if (!command) break;
			if (command->step == target) return *command;
			CHECK(executor->Complete(Reply(*command), 1000));
		}
		CHECK(false); return {};
	}
};

void Codec() {
	auto journal = Journal(); const auto bytes = SerializeDesktopSyncChatCreateJournal(journal);
	CHECK(bytes && ParseDesktopSyncChatCreateJournal(*bytes, journal.authority, Origin.account));
	const auto object = QJsonDocument::fromJson(*bytes).object();
	CHECK(object.size() == 22);
	for (auto i = object.begin(); i != object.end(); ++i) {
		auto missing = object; missing.remove(i.key());
		CHECK(!ParseDesktopSyncChatCreateJournal(QJsonDocument(missing).toJson(QJsonDocument::Compact), journal.authority, Origin.account));
		auto malformed = object; malformed.insert(i.key(), QJsonArray());
		CHECK(!ParseDesktopSyncChatCreateJournal(QJsonDocument(malformed).toJson(QJsonDocument::Compact), journal.authority, Origin.account));
	}
	auto extra = object; extra.insert(u"extra"_q, true);
	CHECK(!ParseDesktopSyncChatCreateJournal(QJsonDocument(extra).toJson(QJsonDocument::Compact), journal.authority, Origin.account));
	CHECK(!ParseDesktopSyncChatCreateJournal(*bytes, QString(64, u'b'), Origin.account));
	CHECK(!ParseDesktopSyncChatCreateJournal(*bytes, journal.authority, {1, 123}));
	CHECK(!ParseDesktopSyncChatCreateJournal(*bytes, journal.authority, {0, 124}));
	CHECK(!ParseDesktopSyncChatCreateJournal(QByteArray(512*1024+1, 'a'), journal.authority, Origin.account));
	auto duplicate = *bytes; duplicate.insert(1, "\"version\":1,");
	CHECK(!ParseDesktopSyncChatCreateJournal(duplicate, journal.authority, Origin.account));
	for (auto n = 0; n != 8; ++n) {
		auto changed = Fresh();
		if (n == 0) changed.scope.origin.account.environment = 1;
		if (n == 1) changed.scope.origin.account.userId = 124;
		if (n == 2) changed.scope.loginBinding = QString(64, u'b');
		if (n == 3) changed.scope.root += u"other"_q;
		if (n == 4) changed.scope.binding += 'b';
		if (n == 5) changed.scope.mode = AccountWorkMode::Follow;
		if (n == 6) changed.scope.owner = Origin.account;
		if (n == 7) changed.scope.loginBinding.clear();
		CHECK(DesktopSyncChatSetupAuthority(changed.scope) != journal.authority);
	}
	for (auto mode = 1; mode != 8; ++mode) CHECK(!DesktopSyncChatCreatedChannel(CreatedUpdates(mode), 123));
	CHECK(DesktopSyncChatCreatedChannel(CreatedUpdates(8),123)==(PeerToken{PeerTokenKind::Channel,456}));
	CHECK(DesktopSyncChatCreatedChannel(CreatedUpdates(), 123) == (PeerToken{PeerTokenKind::Channel,456}));
}

void NormalAndRestart() {
	for (const auto choice : {DesktopSyncChatSetupChoice::Create, DesktopSyncChatSetupChoice::Self, DesktopSyncChatSetupChoice::Existing}) {
		auto h = Injected(choice); h.executor->Start(1000); h.Pump();
		CHECK(h.executor->phase() == Phase::Complete);
		CHECK(h.effects.front() == Step::Resolve);
		for (const auto step : {Step::Pin, Step::Mute, Step::Archive}) {
			CHECK((std::count(h.effects.begin(), h.effects.end(), step) == 1)
				== (choice == DesktopSyncChatSetupChoice::Create));
		}
		CHECK(std::count(h.effects.begin(), h.effects.end(), Step::PostMark) == 1);
		const auto before = h.effects.size(); h.Restart(); h.executor->Start(1000); h.Pump();
		CHECK(h.effects.size() == before && h.executor->phase() == Phase::Complete);
	}
	for (const auto step : {Step::Create, Step::ApplyUpdates, Step::PeerInfo, Step::Inventory,
		Step::PostMark, Step::ReadMark, Step::Pin, Step::Mute, Step::Archive, Step::Commit}) {
		for (const auto replied : {false,true}) {
			auto h = Injected(); const auto command = h.At(step);
			if (replied) CHECK(h.executor->Complete(h.Reply(command),1000));
			const auto originalRandom = h.saved.randomId;
			h.Restart(); h.executor->Start(1000); h.Pump();
			CHECK(h.executor->phase() == Phase::Complete);
			CHECK(h.saved.randomId == originalRandom);
			CHECK(std::count(h.effects.begin(),h.effects.end(),Step::Create) <= 1);
			CHECK(std::count(h.effects.begin(),h.effects.end(),Step::PostMark) <= 1);
		}
	}
}

void FailureSchedules() {
	for (auto fail = 1; fail != 30; ++fail) {
		auto h = Injected(); h.failSave = fail; h.executor->Start(1000); h.Pump();
		if (h.saves < fail) continue;
		CHECK(h.executor->phase() == Phase::Held);
		const auto old = h.bytes; CHECK(!h.executor->Next()); CHECK(old == h.bytes);
		h.failSave = 0; h.Restart(); h.executor->Start(1000); h.Pump();
		CHECK(h.executor->phase() == Phase::Complete);
		CHECK(std::count(h.effects.begin(),h.effects.end(),Step::Create) <= 1);
		CHECK(std::count(h.effects.begin(),h.effects.end(),Step::PostMark) <= 1);
	}
	for (const auto step : {Step::Resolve,Step::Create,Step::ApplyUpdates,Step::PeerInfo,Step::Inventory,Step::PostMark,Step::ReadMark,Step::Pin,Step::Mute,Step::Archive,Step::Commit}) {
		for (auto mode = 0; mode != 4; ++mode) {
			auto h = Injected(); const auto command = h.At(step); const auto reply = h.Reply(command);
			if (mode == 0) {
				h.current = false; CHECK(h.executor->Complete(reply,1000));
				CHECK(h.executor->phase() == Phase::Held && !h.executor->Next());
			} else if (mode == 1) {
				h.executor->Cancel(); CHECK(h.executor->phase() == Phase::Cancelled);
				CHECK(!h.executor->Complete(reply,1000));
			} else {
				auto stale = reply;
				if (mode == 2) stale.origin.login += u"other"_q;
				if (mode == 3) stale.ticket++;
				CHECK(!h.executor->Complete(stale,1000));
				CHECK(h.executor->Complete(reply,1000));
			}
		}
	}
	auto h = Injected(); const auto old = h.At(Step::Create); const auto oldReply = h.Reply(old);
	h.executor->Cancel(); h.executor->Start(1000);
	const auto current = h.executor->Next(); CHECK(current && current->step == Step::Resolve);
	CHECK(!h.executor->Complete(oldReply,1000));
	CHECK(h.executor->Complete(h.Reply(*current),1000)); h.Pump();
	CHECK(h.executor->phase() == Phase::Complete);
}

void AmbiguousAndLimits() {
	for (const auto proof : {DesktopSyncChatCreationProof::None,DesktopSyncChatCreationProof::TitleOnly}) {
		auto h = Injected(); const auto command = h.At(Step::Create);
		auto reply = h.Reply(command); reply.outcome = Outcome::Ambiguous;
		CHECK(h.executor->Complete(reply,1000)); CHECK(h.saved.pending);
		h.Restart(); h.executor->Start(1000);
		auto resolve = h.executor->Next(); CHECK(resolve && resolve->step == Step::Resolve);
		CHECK(h.executor->Complete(h.Reply(*resolve),1000));
		const auto reconcile = h.executor->Next(); CHECK(reconcile && reconcile->step == Step::ReconcileCreate);
		auto found = h.Reply(*reconcile); found.creationProof = proof;
		CHECK(h.executor->Complete(found,1000)); CHECK(h.executor->phase() == Phase::Held);
		h.executor->UseSelf(1000); CHECK(h.executor->phase() == Phase::Held && h.saved.pending);
		CHECK(std::count(h.effects.begin(),h.effects.end(),Step::Create) == 1);
	}
	for (auto mode = 0; mode != 3; ++mode) {
		auto h = Injected(); const auto command = h.At(Step::Create);
		auto reply = h.Reply(command); reply.outcome = Outcome::Limit;
		CHECK(h.executor->Complete(reply,1000)); CHECK(h.executor->phase() == Phase::Limit && !h.saved.pending);
		if (mode == 0) { h.executor->UseSelf(1000); h.Pump(); CHECK(h.executor->phase() == Phase::Complete && !h.saved.created); }
		if (mode == 1) {
			for (auto attempt = 1; attempt != 3; ++attempt) {
				const auto command = h.At(Step::Create); auto rejected = h.Reply(command); rejected.outcome = Outcome::Limit;
				CHECK(h.executor->Complete(rejected,1000));
			}
			h.executor->Start(1000); h.Pump(); CHECK(h.executor->reason() == u"create-retry-budget"_q);
			CHECK(h.saved.createAttempts == 3);
		}
		if (mode == 2) { h.executor->Cancel(); CHECK(h.executor->phase() == Phase::Cancelled); }
	}
	for (const auto outcome : {Outcome::FloodWait, Outcome::Restricted, Outcome::Rejected}) {
		auto h = Injected(); const auto command = h.At(Step::Create); auto reply = h.Reply(command);
		reply.outcome = outcome; reply.retryAfter = 30;
		CHECK(h.executor->Complete(reply,1000)); CHECK(!h.saved.pending);
		h.Restart(); h.executor->Start(1001);
		if (outcome == Outcome::FloodWait) {
			CHECK(h.executor->reason() == u"retry-backoff"_q && h.saved.retryAt == 1030);
			h.executor->Start(1030);
		}
		h.Pump(); CHECK(h.executor->phase() == Phase::Complete);
	}
}

void PendingMarkDiscovery() {
	for (auto mode=0;mode!=3;++mode) {
		auto h=Injected();const auto posted=h.At(Step::PostMark);h.Reply(posted);
		h.Restart();h.executor->Start(1000);const auto resolve=h.executor->Next();
		CHECK(resolve && resolve->step==Step::Resolve);
		auto response=h.Reply(*resolve);response.resolution->status.phase=DesktopSyncChatPhase::Use;
		response.resolution->status.resolution.action=SyncChatAction::Use;
		response.resolution->status.resolution.readChat=h.saved.chat;
		response.resolution->status.resolution.cache=SyncChatCache{*h.saved.chat,mode==1?2:1,mode==2?200:100};
		CHECK(h.executor->Complete(response,1000));h.Pump();
		CHECK(h.executor->phase()==(mode==0?Phase::Complete:Phase::Held));
		CHECK(std::count(h.effects.begin(),h.effects.end(),Step::PostMark)==1);
		CHECK(std::count(h.effects.begin(),h.effects.end(),Step::ReconcileMark)==1);
	}
}

void SafetyControls() {
	for (auto mode = 0; mode != 9; ++mode) {
		auto h = Injected(); const auto command = h.At(Step::ReadMark); auto reply = h.Reply(command);
		reply.readback = Response(Route(*h.saved.chat), {CreatedMark(*h.saved.chat, mode)});
		if (mode == 0) reply.readback.reset();
		if (mode == 4) {
			auto wrong = NativeMessage(); wrong.id=MTP_int(1);wrong.date=MTP_int(100);
			wrong.peer_id=Target(*h.saved.chat);wrong.message=MTP_string(DesktopSyncChatMarkText());
			reply.readback=Response(Route(*h.saved.chat), {wrong.Build()});
		}
		CHECK(h.executor->Complete(reply,1000)); CHECK(h.executor->phase() == Phase::Held);
		CHECK(std::count(h.effects.begin(),h.effects.end(),Step::Pin)==0);
	}
	for (auto mode = 0; mode != 7; ++mode) {
		auto h = Injected(); const auto command = h.At(Step::PeerInfo); auto reply = h.Reply(command);
		if (mode == 0) reply.peer->facts.reset();
		if (mode == 1) reply.peer->facts->otherAdmins.reset();
		if (mode == 2) reply.peer->facts->canSendFiles=false;
		if (mode == 3) reply.peer->facts->otherMembers=1;
		if (mode == 4) reply.peer->origin.login+=u"other"_q;
		if (mode == 5) reply.peer->kind=SyncChatKind::Private;
		if (mode == 6) {reply.peer->kind=SyncChatKind::Private;reply.peer->facts->kind=SyncChatKind::Private;}
		CHECK(h.executor->Complete(reply,1000)); CHECK(h.executor->phase()==Phase::Held);
		CHECK(std::count(h.effects.begin(),h.effects.end(),Step::PostMark)==0);
	}
	for (auto mode = 0; mode != 3; ++mode) {
		auto h=Injected();const auto command=h.At(Step::Resolve);auto reply=h.Reply(command);
		if(mode==0){reply.resolution->status.complete=false;}
		if(mode==1){reply.resolution->scope.settingsBound=true;}
		if(mode==2){reply.resolution->scope.loginBinding=QString(64,u'b');}
		CHECK(h.executor->Complete(reply,1000));CHECK(h.executor->phase()==Phase::Held);
	}
	auto h=Injected();const auto command=h.At(Step::Resolve);auto reply=h.Reply(command);
	reply.resolution->status.phase=DesktopSyncChatPhase::Use;reply.resolution->status.postingHeld=false;
	reply.resolution->status.resolution.action=SyncChatAction::Use;reply.resolution->status.resolution.postingHeld=false;
	reply.resolution->status.resolution.cache=SyncChatCache{{PeerTokenKind::Channel,789},1,100};
	reply.resolution->status.resolution.readChat=PeerToken{PeerTokenKind::Channel,789};
	CHECK(h.executor->Complete(reply,1000));CHECK(h.executor->phase()==Phase::Complete&&h.saved.adopted);
	CHECK(std::count(h.effects.begin(),h.effects.end(),Step::Create)==0);
}

void TargetInventory() {
	for(const auto choice:{DesktopSyncChatSetupChoice::Create,DesktopSyncChatSetupChoice::Self,DesktopSyncChatSetupChoice::Existing}) {
		for(auto mode=0;mode!=6;++mode) {
			auto h=Injected(choice);const auto command=h.At(Step::Inventory);auto reply=h.Reply(command);
			if(mode==0)reply.inventory.reset();
			if(mode==1)reply.inventory->inventory.scan.status=SyncHistoryScanStatus::InvalidResponse;
			if(mode==2)reply.inventory->route.peer.origin.login+=u"other"_q;
			if(mode==3)reply.inventory->inventory.read.reset();
			if(mode==4){reply.inventory->inventory=FinishSyncAccountInventory(123,{SyncHistoryScanStatus::Complete,{10},1},
				SyncCandidateReadResult{SyncCandidateReadStatus::Complete,{ClassifySyncCandidate(10,RecordBytes())}});}
			if(mode==5)reply.inventory->inventory.scan.candidateIds={10};
			CHECK(h.executor->Complete(reply,1000));CHECK(h.executor->phase()==Phase::Held);
			CHECK(std::count(h.effects.begin(),h.effects.end(),Step::PostMark)==0);
		}
		auto h=Injected(choice);const auto inventory=h.At(Step::Inventory);
		CHECK(h.executor->Complete(h.Reply(inventory),1000));
		CHECK(!h.saved.markId && !h.saved.pending && h.saved.step==Step::PostMark);
		h.Restart();h.executor->Start(1000);h.Pump();CHECK(h.executor->phase()==Phase::Complete);
		CHECK(std::count(h.effects.begin(),h.effects.end(),Step::Inventory)==2);
		CHECK(std::count(h.effects.begin(),h.effects.end(),Step::PostMark)==1);
	}
}

void ReadbackBoundaries() {
	for (const auto choice : {DesktopSyncChatSetupChoice::Create, DesktopSyncChatSetupChoice::Self,
		DesktopSyncChatSetupChoice::Existing}) {
		for (auto mode=0;mode!=8;++mode) {
			auto h=Injected(choice);const auto command=h.At(Step::ReadMark);auto reply=h.Reply(command);
			const auto route=Route(*h.saved.chat);const auto message=CreatedMark(*h.saved.chat);
			if(mode==0)reply.readback=Response(route,{});
			if(mode==1)reply.readback=Response(route,{message,message});
			if(mode==2)reply.readback=MTP_messages_messagesNotModified(MTP_int(1));
			if(mode>=3&&mode<=5){
				auto data=NativeMessagesSlice();data.count=MTP_int(mode==5?0:1);
				data.flags=MTP_flags(mode==3?MTPDmessages_messagesSlice::Flag::f_inexact
					:mode==4?MTPDmessages_messagesSlice::Flag::f_search_flood:MTPDmessages_messagesSlice::Flags());
				data.messages=MTP_vector<MTPMessage>({message});data.search_flood=MTP_searchPostsFlood(MTP_flags(MTPDsearchPostsFlood::Flags()),MTP_int(0),MTP_int(0),MTP_int(0),MTP_long(0));
				reply.readback=data.Build();
			}
			if(mode==6)reply.readback=Response(route,{MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flags()),MTP_int(1),MTPPeer())});
			if(mode==7){auto data=NativeChannelMessages();data.count=MTP_int(0);data.messages=MTP_vector<MTPMessage>({message});reply.readback=data.Build();}
			CHECK(h.executor->Complete(reply,1000));CHECK(h.executor->phase()==Phase::Held);
			CHECK(std::count(h.effects.begin(),h.effects.end(),Step::Commit)==0);
		}
	}
}

void InventoryAdapterChain() {
	for(const auto choice:{DesktopSyncChatSetupChoice::Create,DesktopSyncChatSetupChoice::Self,DesktopSyncChatSetupChoice::Existing}) {
		for(auto mode=0;mode!=4;++mode) {
			auto h=Injected(choice);const auto command=h.At(Step::Inventory);auto reply=h.Reply(command);
			const auto route=Route(*h.saved.chat);auto scan=ScanProbe(route);
			if(mode==0)scan.Reply(Response(route,{}));
			else {
				auto data=NativeMessagesSlice();data.count=MTP_int(mode==3?1:0);
				data.flags=MTP_flags(mode==1?MTPDmessages_messagesSlice::Flag::f_inexact
					:mode==2?MTPDmessages_messagesSlice::Flag::f_search_flood:MTPDmessages_messagesSlice::Flags());
				data.messages=MTP_vector<MTPMessage>();
				data.search_flood=MTP_searchPostsFlood(MTP_flags(MTPDsearchPostsFlood::Flags()),MTP_int(0),MTP_int(0),MTP_int(0),MTP_long(0));
				scan.Reply(data.Build());
			}
			reply.inventory=DesktopSyncChatInventory{route,FinishSyncAccountInventory(123,
				{scan.status,scan._pages.candidates(),scan._pages.count()},
				SyncCandidateReadResult{SyncCandidateReadStatus::Complete,{}})};
			CHECK(h.executor->Complete(reply,1000));
			CHECK((h.executor->phase()==Phase::Held)==(mode!=0));
			if(mode==0){const auto info=h.executor->Next();CHECK(info&&info->step==Step::PeerInfo);
				CHECK(h.executor->Complete(h.Reply(*info),1000));
				const auto next=h.executor->Next();CHECK(next&&next->step==Step::PostMark);}
		}
	}
}


DesktopSyncChatPeer GroupReaders(int count) {
	auto peer = Route({PeerTokenKind::Chat,456}).peer;
	peer.facts->otherMembers = count;
	peer.readerMembers = std::vector<QString>();
	for (auto n = 0; n != count; ++n) peer.readerMembers->push_back(QString(64, QChar(u'a' + n)));
	return peer;
}

void ReaderConsentSchedules() {
	for (auto mode = 0; mode != 5; ++mode) {
		auto h = Injected(); const auto inventory = h.At(Step::Inventory);
		CHECK(h.executor->Complete(h.Reply(inventory),1000));
		const auto info = h.executor->Next(); CHECK(info && info->step == Step::PeerInfo);
		auto reply = h.Reply(*info);
		if (mode == 0) reply.peer->facts->canSendFiles = false;
		if (mode == 1) reply.peer->facts->otherAdmins.reset();
		if (mode == 2) reply.peer->facts->otherMembers.reset();
		if (mode == 3) reply.peer->facts->otherMembers = 1;
		if (mode == 4) reply.peer->origin.login += u"changed"_q;
		CHECK(h.executor->Complete(reply,1000)); CHECK(h.executor->phase() == Phase::Held);
		CHECK(std::count(h.effects.begin(),h.effects.end(),Step::PostMark) == 0);
	}
	for (auto mode = 0; mode != 10; ++mode) {
		auto h = Injected(); const auto info = h.At(Step::PeerInfo); auto reply = h.Reply(info);
		reply.peer->facts->otherMembers = 1;
		CHECK(h.executor->Complete(reply,1000));
		const auto offer = h.executor->offeredReaders(); CHECK(offer);
		const auto old = h.bytes;
		CHECK(h.bytes == old && !h.executor->Next());
		auto changed = *offer;
		if (mode == 1) changed.origin.login += u"changed"_q;
		if (mode == 2) changed.operation += u"changed"_q;
		if (mode == 3) ++changed.ticket;
		if (mode == 4) changed.peer.chat = {PeerTokenKind::Channel,789};
		if (mode == 5) changed.peer.facts->otherMembers = 2;
		if (mode == 6) h.current = false;
		if (mode == 7) h.executor->Cancel();
		CHECK(h.executor->Reconsent(changed,mode == 8 ? 0 : mode == 9 ? std::numeric_limits<int64_t>::max() : 1000) == (mode == 0));
		if (mode != 0) { if (mode != 7) CHECK(h.bytes == old); continue; }
		const auto resolve = h.executor->Next(); CHECK(resolve && resolve->step == Step::Resolve);
		CHECK(h.executor->Complete(h.Reply(*resolve),1000));
		const auto fresh = h.executor->Next(); CHECK(fresh && fresh->step == Step::PeerInfo);
		auto accepted = h.Reply(*fresh); accepted.peer = offer->peer;
		CHECK(h.executor->Complete(accepted,1000));
		const auto inventory = h.executor->Next(); CHECK(inventory && inventory->step == Step::Inventory);
		auto empty = h.Reply(*inventory); empty.inventory->route.peer = offer->peer;
		CHECK(h.executor->Complete(empty,1000));
		const auto last = h.executor->Next(); CHECK(last && last->step == Step::PeerInfo);
		auto final = h.Reply(*last); final.peer = offer->peer;
		CHECK(h.executor->Complete(final,1000));
		const auto post = h.executor->Next(); CHECK(post && post->step == Step::PostMark);
		CHECK(h.saved.chat == offer->peer.chat);
		CHECK(!h.executor->Reconsent(*offer,1000));
	}
	for (auto mode = 0; mode != 5; ++mode) {
		auto h = Injected(DesktopSyncChatSetupChoice::Existing);
		h.saved.chat = PeerToken{PeerTokenKind::Chat,456};
		const auto original = GroupReaders(2);
		auto selection = DesktopSyncChatSetupSelection{DesktopSyncChatSetupChoice::Existing,original,true};
		const auto journal = BeginDesktopSyncChatCreate(Fresh(),selection); CHECK(journal);
		h.saved = *journal; h.bytes = *SerializeDesktopSyncChatCreateJournal(h.saved); h.Restart();
		auto info = h.At(Step::PeerInfo); auto peer = h.Reply(info); peer.peer = original;
		CHECK(h.executor->Complete(peer,1000));
		const auto inventory = h.executor->Next(); CHECK(inventory && inventory->step == Step::Inventory);
		auto empty = h.Reply(*inventory); empty.inventory->route.peer = original;
		CHECK(h.executor->Complete(empty,1000));
		info = *h.executor->Next(); CHECK(info.step == Step::PeerInfo);
		peer = h.Reply(info); peer.peer = original; CHECK(h.executor->Complete(peer,1000));
		const auto post = h.executor->Next(); CHECK(post && post->step == Step::PostMark);
		if (mode != 4) CHECK(h.executor->Complete(h.Reply(*post),1000));
		h.Restart(); h.executor->Start(1000);
		const auto resolve = h.executor->Next(); CHECK(resolve && resolve->step == Step::Resolve);
		CHECK(h.executor->Complete(h.Reply(*resolve),1000));
		const auto fresh = h.executor->Next(); CHECK(fresh && fresh->step == Step::PeerInfo);
		auto readers = h.Reply(*fresh); readers.peer = GroupReaders(mode == 1 ? 1 : 3);
		if (mode == 2) readers.peer->facts->otherMembers.reset();
		if (mode == 3) readers.peer->facts->canSendFiles = false;
		CHECK(h.executor->Complete(readers,1000));
		if (mode == 2 || mode == 3) { CHECK(h.executor->phase() == Phase::Held); continue; }
		CHECK(h.saved.newReaders == (mode != 1)); CHECK(!h.executor->offeredReaders());
		h.Pump(); CHECK(h.executor->phase() == Phase::Complete);
		const auto cache = DesktopSyncChatSetupCache(h.saved,Fresh().scope,u"Synthetic"_q);
		CHECK(cache && cache->readers.otherMembers == 2 && cache->readerMembers == original.readerMembers);
		CHECK(cache && cache->newReaders == (mode != 1));
		auto scope = Fresh().scope; scope.loginBinding = QString(64,u'b');
		CHECK(!DesktopSyncChatSetupCache(h.saved,scope,{}));
		CHECK(h.saved.chat == original.chat);
	}
}

void ReaderJournalCodec() {
	const auto j = Journal(DesktopSyncChatSetupChoice::Existing);
	const auto bytes = *SerializeDesktopSyncChatCreateJournal(j);
	const auto object = QJsonDocument::fromJson(bytes).object();
	const auto snapshot = object.value(u"reader_snapshot"_q).toObject(); CHECK(snapshot.size() == 10);
	for (auto i = snapshot.begin(); i != snapshot.end(); ++i) {
		auto bad = snapshot; bad.remove(i.key());
		auto changed = object; changed.insert(u"reader_snapshot"_q,bad);
		CHECK(!ParseDesktopSyncChatCreateJournal(QJsonDocument(changed).toJson(QJsonDocument::Compact),j.authority,Origin.account));
		bad = snapshot; bad.insert(i.key(),QJsonValue(QJsonValue::Null)); changed.insert(u"reader_snapshot"_q,bad);
		CHECK(!ParseDesktopSyncChatCreateJournal(QJsonDocument(changed).toJson(QJsonDocument::Compact),j.authority,Origin.account));
	}
	auto bad = snapshot; bad.insert(u"extra"_q,true);
	auto changed = object; changed.insert(u"reader_snapshot"_q,bad);
	CHECK(!ParseDesktopSyncChatCreateJournal(QJsonDocument(changed).toJson(QJsonDocument::Compact),j.authority,Origin.account));
	changed = object; changed.insert(u"reader_consent"_q,QString(64,u'b'));
	CHECK(!ParseDesktopSyncChatCreateJournal(QJsonDocument(changed).toJson(QJsonDocument::Compact),j.authority,Origin.account));
}


void UnmarkedTargetRecords() {
	for (const auto &route : {Route({PeerTokenKind::User,456}),
		Route({PeerTokenKind::Chat,456}), Route({PeerTokenKind::Channel,456}),
		Route({PeerTokenKind::Channel,456},false)}) {
		auto selection = DesktopSyncChatSetupSelection{DesktopSyncChatSetupChoice::Existing,route.peer,true};
		const auto journal = BeginDesktopSyncChatCreate(Fresh(),selection); CHECK(journal);
		auto h = Injected(DesktopSyncChatSetupChoice::Existing);
		h.saved = *journal; h.bytes = *SerializeDesktopSyncChatCreateJournal(*journal); h.Restart();
		h.executor->Start(1000); const auto resolve = h.executor->Next();
		CHECK(resolve && resolve->step == Step::Resolve);
		const auto emptySelf = h.Reply(*resolve);
		CHECK(emptySelf.resolution->status.phase == DesktopSyncChatPhase::Unset);
		CHECK(h.executor->Complete(emptySelf,1000));
		const auto info = h.executor->Next(); CHECK(info && info->step == Step::PeerInfo);
		auto peer = h.Reply(*info); peer.peer = route.peer;
		CHECK(h.executor->Complete(peer,1000));
		const auto command = h.executor->Next(); CHECK(command && command->step == Step::Inventory);
		const auto message = DocumentMessage(route);
		auto scan = ScanProbe(route); scan.Reply(Response(route,{message}));
		if (!scan._done) scan.Reply(Response(route,{}));
		CHECK(scan.status == SyncHistoryScanStatus::Complete);
		auto reader = ReaderProbe(route); reader.Reply(Response(route,{message}));
		CHECK(reader.downloading);
		reader._loader = std::make_unique<DownloadProbe>(); reader._loader->data = RecordBytes(); reader.OnDownload();
		CHECK(reader.rereading && !reader.record); reader.Reply(Response(route,{message}));
		CHECK(reader.record && reader.record->status == SyncCandidateStatus::Valid);
		auto read = SyncCandidateReadResult{SyncCandidateReadStatus::Complete,{*reader.record}};
		auto reply = h.Reply(*command);
		reply.inventory = DesktopSyncChatInventory{route,FinishSyncAccountInventory(123,
			{scan.status,scan._pages.candidates(),scan._pages.count()},read)};
		CHECK(MapDesktopSyncChatInventoryReply(Origin,command->ticket,route.peer.chat,*reply.inventory).records == SyncChatRecordPresence::Present);
		CHECK(h.executor->Complete(reply,1000)); CHECK(h.executor->phase() == Phase::Held);
		CHECK(std::count(h.effects.begin(),h.effects.end(),Step::PostMark) == 0);
		CHECK(std::count(h.effects.begin(),h.effects.end(),Step::Commit) == 0);
	}
}


void LateWarningCacheFailures() {
	for (const auto resume : {Step::Pin,Step::Mute,Step::Archive}) {
		auto h = Injected(); h.At(resume);
		auto cache = DesktopSyncChatSetupCache(h.saved,Fresh().scope,u"Synthetic"_q);
		CHECK(cache && !cache->newReaders);
		h.Restart(); h.executor->Start(1000);
		for (auto n = 0; n != 10; ++n) {
			const auto command = h.executor->Next(); CHECK(command);
			if (command->step == Step::Commit) {
				CHECK(h.saved.decorationAfterCache == resume && h.saved.pending);
				auto failed = h.Reply(*command); failed.outcome = Outcome::Rejected;
				CHECK(h.executor->Complete(failed,1000));
				break;
			}
			auto reply = h.Reply(*command);
			if (command->step == Step::PeerInfo) reply.peer->facts->otherMembers = 1;
			CHECK(h.executor->Complete(reply,1000));
		}
		CHECK(h.executor->phase() == Phase::Held && !h.executor->Next());
		CHECK(h.saved.newReaders && cache && !cache->newReaders);
		CHECK(h.saved.step == Step::Commit && h.saved.decorationAfterCache == resume);
		const auto decorations = std::count(h.effects.begin(),h.effects.end(),resume);
		h.Restart(); h.executor->Start(1000);
		auto writes = 0;
		for (auto n = 0; n != 20; ++n) {
			const auto command = h.executor->Next(); if (!command) break;
			auto reply = h.Reply(*command);
			if (command->step == Step::PeerInfo) CHECK(reply.peer->facts->otherMembers == 0);
			if (command->step == Step::Commit) {
				++writes; CHECK(h.saved.decorationAfterCache == resume);
				cache = DesktopSyncChatSetupCache(h.saved,Fresh().scope,u"Synthetic"_q);
			}
			CHECK(h.executor->Complete(reply,1000));
		}
		CHECK(writes == 1 && h.executor->phase() == Phase::Complete);
		CHECK(cache && cache->newReaders && cache->readers.otherMembers == 0);
		CHECK(h.saved.newReaders && !h.saved.decorationAfterCache);
		CHECK(std::count(h.effects.begin(),h.effects.end(),resume) == decorations + 1);
	}
	CHECK(DesktopSyncChatMarkText() == u"This account's Purple Telegram apps keep their sync data in this chat: settings and playlists. Deleting this message makes them stop using it.\n#purplechatsync v1"_q);
	auto h = Injected(); const auto command = h.At(Step::ReadMark); auto reply = h.Reply(command);
	auto mark = NativeMessage(); mark.flags = MTP_flags(MTPDmessage::Flag::f_out | MTPDmessage::Flag::f_from_id);
	mark.id = MTP_int(1); mark.date = MTP_int(100); mark.peer_id = Target(*h.saved.chat); mark.from_id = MTP_peerUser(MTP_long(123));
	mark.message = MTP_string("Purple Telegram finds this account's sync data here.\n#purplechatsync v1");
	reply.readback = Response(Route(*h.saved.chat),{mark.Build()});
	CHECK(h.executor->Complete(reply,1000)); CHECK(h.executor->phase() == Phase::Held);
	CHECK(std::count(h.effects.begin(),h.effects.end(),Step::Commit) == 0);
}

void RequestWire() {
	for(const auto step:{Step::Create,Step::PostMark,Step::Pin,Step::Mute,Step::Archive}) {
		auto h=Injected();const auto command=h.At(step);
		const auto route=h.saved.chat?std::optional(Route(*h.saved.chat)):std::nullopt;
		const auto request=DesktopSyncChatSetupRequest(command,route);CHECK(request);
		auto words=mtpBuffer();std::visit([&](const auto &value){value.write(words);},*request);
		auto from=words.constData();const auto end=from+words.size();const auto type=Read<MTPint>(from,end).v;
		if(step==Step::Create){
			CHECK(type==int(mtpc_channels_createChannel));CHECK(Read<MTPint>(from,end).v==1);
			CHECK(qs(Read<MTPstring>(from,end))==u"Purple sync"_q);
			CHECK(qs(Read<MTPstring>(from,end))==u"Purple Telegram keeps sync data for your devices here."_q);
		}else if(step==Step::PostMark){
			CHECK(type==int(mtpc_messages_sendMessage));CHECK(Read<MTPint>(from,end).v==32);
			CHECK(Read<MTPInputPeer>(from,end).type()==route->input.type());
			CHECK(qs(Read<MTPstring>(from,end))==DesktopSyncChatMarkText());
			CHECK(uint64_t(Read<MTPlong>(from,end).v)==h.saved.randomId);
		}else if(step==Step::Pin){
			CHECK(type==int(mtpc_messages_updatePinnedMessage));CHECK(Read<MTPint>(from,end).v==1);
			CHECK(Read<MTPInputPeer>(from,end).type()==mtpc_inputPeerChannel);
			CHECK(Read<MTPint>(from,end).v==1);
		}else if(step==Step::Mute){
			CHECK(type==int(mtpc_account_updateNotifySettings));
			const auto peer=Read<MTPInputNotifyPeer>(from,end);CHECK(peer.type()==mtpc_inputNotifyPeer);
			CHECK(peer.c_inputNotifyPeer().vpeer().type()==mtpc_inputPeerChannel);
			const auto settings=Read<MTPInputPeerNotifySettings>(from,end);
			CHECK(settings.data().vmute_until()&&settings.data().vmute_until()->v==std::numeric_limits<int32_t>::max());
		}else if(step==Step::Archive){
			CHECK(type==int(mtpc_folders_editPeerFolders));const auto peers=Read<MTPVector<MTPInputFolderPeer>>(from,end);
			CHECK(peers.v.size()==1&&peers.v.front().data().vfolder_id().v==1);
			CHECK(peers.v.front().data().vpeer().type()==mtpc_inputPeerChannel);
		}
		CHECK(from==end);
		for(auto mode=0;mode!=4;++mode){auto stale=command;
			if(mode==0)stale.origin.login+=u"other"_q;
			if(mode==1)stale.operation+=u"x"_q;
			if(mode==2)stale.silent=false;
			if(mode==3)stale.journal.pending=false;
			if(mode!=0||route)CHECK(!DesktopSyncChatSetupRequest(stale,route));
		}
	}
}

void StoreCases() {
	const auto make = [] { return QString::fromUtf8(qgetenv("TMPDIR"))
		+ u"/purple-t3-journal-"_q + QUuid::createUuid().toString(QUuid::Id128); };
	for (auto mode=0;mode!=8;++mode) {
		const auto directory=make();auto journal=Journal();auto current=true;
		const auto path=directory+u"/sync-chat-setup.json"_q;
		{
			auto store=DesktopSyncChatCreateStore(directory,journal.authority,Origin.account,[&]{return current;});
			CHECK(store.Open());CHECK(!store.journal());CHECK(store.Save(journal));
			auto locked=DesktopSyncChatCreateStore(directory,journal.authority,Origin.account,[]{return true;});
			CHECK(!locked.Open());
			const auto original=SerializeDesktopSyncChatCreateJournal(journal).value_or(QByteArray());
			if(mode==0){CHECK(store.Save(journal));}
			if(mode==1){auto other=Journal();CHECK(!store.Save(other));}
			if(mode==2){current=false;CHECK(!store.Save(journal));}
			if(mode>=3){
				const auto bytes=mode==3?QByteArray("corrupt"):mode==4?QByteArray(512*1024+1,'a')
					:mode==5?original+' ':mode==6?original:original;
				CHECK(WriteSyncPrivateFile(path,bytes)==SyncStoreStatus::Ready);
				if(mode==3)CHECK(!store.Save(journal));
			}
		}
		current=true;
		auto reopened=DesktopSyncChatCreateStore(directory,mode==6?QString(64,u'b'):journal.authority,
			mode==7?AccountKey{1,123}:Origin.account,[]{return true;});
		CHECK(reopened.Open()==(mode<3));
		if(mode>=3){auto after=QByteArray();CHECK(ReadSyncPrivateFile(path,600*1024,after)==SyncStoreStatus::Ready);CHECK(after==(mode==3?QByteArray("corrupt"):mode==4?QByteArray(512*1024+1,'a'):mode==5?SerializeDesktopSyncChatCreateJournal(journal).value_or(QByteArray())+' ':SerializeDesktopSyncChatCreateJournal(journal).value_or(QByteArray())));}
	}
}

}

#ifndef PURPLE_SYNC_CHAT_CREATE_EMBEDDED
int main(int argc, char **argv) {
	std::setvbuf(stdout,nullptr,_IONBF,0);
	std::puts("Typed controls"); RoutesAndWire(); ReceiptMatrix(); UploadSchedules(); PostReadbackSchedules(); BorrowedRunProof(); CounterScopes();
	std::puts("Codec"); Codec();
	std::puts("NormalAndRestart"); NormalAndRestart();
	std::puts("FailureSchedules"); FailureSchedules();
	std::puts("AmbiguousAndLimits"); AmbiguousAndLimits();
	std::puts("SafetyControls"); SafetyControls(); PendingMarkDiscovery(); ReadbackBoundaries(); RequestWire(); TargetInventory(); InventoryAdapterChain(); ReaderConsentSchedules(); ReaderJournalCodec(); UnmarkedTargetRecords(); LateWarningCacheFailures();
	auto app=QCoreApplication(argc,argv);
	{
		auto j=Journal();auto store=DesktopSyncChatCreateStore(u"not-created"_q,j.authority,Origin.account,[]{return true;});
		CHECK(!store.Open());
	}
	auto worker=std::thread([]{ StoreCases(); }); worker.join();
	std::printf("T3B %d checks, %d failures\n",Checks,Failures);
	return Failures?1:0;
}

#endif
