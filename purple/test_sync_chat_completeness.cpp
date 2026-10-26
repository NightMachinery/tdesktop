#define main AuthorMain
#include "test_sync_chat_discovery.cpp"
#undef main
#include "sync-chat-completeness-probes.inc"

namespace {

SyncCandidateRecord OwnedRecord(int id) {
	auto input = ConfigRecordBuildInput();
	input.text = "version = 1\n";
	input.space = u"sp-"_q + QString(26, u'a');
	input.install = u"in-"_q + QString(26, u'a');
	input.device = u"Synthetic"_q;
	input.platform = u"unit"_q;
	input.app = u"Purple unit"_q;
	input.seq = input.at = 1;
	const auto built = BuildConfigRecord(input);
	CHECK(built);
	auto record = ClassifySyncCandidate(id, built.canonical);
	record.documentId = 10;
	CHECK(record.status == SyncCandidateStatus::Valid);
	return record;
}

SyncAccountInventoryResult FinishProbe(
		const ProbeScanReply &scanner,
		bool present = false) {
	auto scan = SyncHistoryScanResult();
	scan.status = scanner.status;
	scan.candidateIds = scanner._pages.candidates();
	scan.scannedCount = scanner._pages.count();
	auto read = SyncCandidateReadResult();
	if (present) read.records.push_back(OwnedRecord(10));
	read.status = AggregateSyncCandidateRead(read.records);
	return FinishSyncAccountInventory(123, scan, read);
}

DesktopSyncChatDiscovery ResolveInventory(
		const SyncAccountInventoryResult &inventory) {
	auto model = DesktopSyncChatDiscovery(Origin, {}, false, {}, {}, QString(64, u'a'));
	model.Start(1000);
	auto deliveredInventory = 0;
	for (auto n = 0; n < 20 && model.command().step != DesktopSyncChatStep::Idle; ++n) {
		const auto command = model.command();
		auto reply = DesktopSyncChatReply{Origin, command.ticket, true};
		if (command.step == DesktopSyncChatStep::Search) {
			auto full = NativeMessages(); full.messages = MTP_vector<MTPMessage>({Mark({}, 10)});
			reply.messages = {Mark({}, 10)};
			reply.page = MapDesktopSyncChatSearchPage(Origin, full.Build(), command.search,
				command.search == DesktopSyncChatSearch::Global ? std::nullopt : std::optional(PeerToken{}));
		} else if (command.step == DesktopSyncChatStep::PeerInfo) {
			reply.peer = Peer(command.chat);
		} else if (command.step == DesktopSyncChatStep::ExactMark) {
			reply.peer = Peer(command.chat); reply.chatReadable = true;
			reply.messages = {Mark(command.chat, command.mark->markId, command.mark->markDate)};
		} else if (command.step == DesktopSyncChatStep::Inventory) {
			++deliveredInventory;
			reply = MapDesktopSyncChatInventoryReply(Origin, command.ticket, inventory);
		}
		CHECK(model.Complete(reply, 1000));
	}
	CHECK(deliveredInventory == 1);
	return model;
}

void QCompleteInventory() {
	for (auto mode = 0; mode < 4; ++mode) {
		auto model = DesktopSyncChatDiscovery(Origin, {}, false, {}, {}, QString(64, u'a'));
		model.Start(1000);
		auto inventories = 0;
		for (auto n = 0; n < 20 && model.command().step != DesktopSyncChatStep::Idle; ++n) {
			const auto command = model.command();
			auto reply = DesktopSyncChatReply{Origin, command.ticket, true};
			if (command.step == DesktopSyncChatStep::Search) {
				auto empty = NativeMessages();
				reply.page = MapDesktopSyncChatSearchPage(Origin, empty.Build(), command.search,
					command.search == DesktopSyncChatSearch::Global ? std::nullopt : std::optional(PeerToken{}));
			} else if (command.step == DesktopSyncChatStep::PeerInfo) {
				reply.peer = Peer({});
			} else if (command.step == DesktopSyncChatStep::SettingsHint) {
				auto response = NativeMessagesSlice(); response.count = MTP_int(mode == 0 ? 0 : 2);
				if (mode != 0) response.messages = MTP_vector<MTPMessage>({SettingsHit(mode == 1)});
				auto hint = ProbeHint(); hint.Response(command, response.Build()); reply = hint.reply;
			} else if (command.step == DesktopSyncChatStep::Inventory) {
				++inventories;
				auto scanner = ProbeScanReply();
				scanner._owned = [](const MTPMessage &message) { return GetSyncChatMessageOwnership(MapDesktopSyncChatMessage(Origin, Peer({}), message)); };
				if (mode == 0) {
					auto empty = NativeMessages(); scanner.Response(empty.Build());
				} else {
					auto all = NativeMessagesSlice(); all.count = MTP_int(2);
					all.messages = MTP_vector<MTPMessage>({SettingsHit(true,20),SettingsHit(false,10)});
					if (mode == 3) all.flags = MTP_flags(MTPDmessages_messagesSlice::Flag::f_inexact);
					scanner.Response(all.Build());
				}
				reply = MapDesktopSyncChatInventoryReply(Origin, command.ticket, FinishProbe(scanner, mode != 0));
			}
			CHECK(model.Complete(reply, 1000));
		}
		CHECK(inventories == 1);
		if (mode == 0) CHECK(model.status().phase == DesktopSyncChatPhase::Unset && model.status().complete);
		else if (mode == 3) CHECK(model.status().phase == DesktopSyncChatPhase::Paused && model.status().postingHeld && !model.status().complete);
		else CHECK(model.status().phase == DesktopSyncChatPhase::Use && !model.status().postingHeld);
	}
}

void FullResponseInventory() {
	for (auto mode = 0; mode < 12; ++mode) {
		auto scanner = ProbeScanReply(); scanner._owned = [](const MTPMessage &) { return SyncChatMessageOwnership::Own; };
		auto slice = NativeMessagesSlice(); slice.count = MTP_int(mode == 0 ? 0 : 2);
		if (mode == 1) slice.flags = MTP_flags(MTPDmessages_messagesSlice::Flag::f_inexact);
		if (mode == 2) {
			slice.flags = MTP_flags(MTPDmessages_messagesSlice::Flag::f_search_flood);
			slice.search_flood = MTP_searchPostsFlood(MTP_flags(MTPDsearchPostsFlood::Flags()),MTP_int(1),MTP_int(2),MTP_int(3),MTP_long(4));
		}
		if (mode == 3) slice.count = MTP_int(-1);
		if (mode == 4) scanner.Response(NativeChannelMessages().Build());
		else if (mode == 5) scanner.Response(MTP_messages_messagesNotModified(MTP_int(0)));
		else if (mode == 6) {
			slice.messages = MTP_vector<MTPMessage>({Mark({PeerTokenKind::Channel,456},20)}); scanner.Response(slice.Build());
		} else if (mode == 7) {
			slice.messages = MTP_vector<MTPMessage>({Mark({},20)}); scanner.Response(slice.Build());
			auto changed = NativeMessagesSlice(); changed.count = MTP_int(3); scanner.Response(changed.Build());
		} else if (mode == 8) {
			for (auto page = 0; page < 20; ++page) {
				slice.count = MTP_int(21); slice.messages = MTP_vector<MTPMessage>({Mark({},100-page)}); scanner.Response(slice.Build());
			}
		 } else if (mode == 10) {
			slice.messages = MTP_vector<MTPMessage>({Mark({},20)}); scanner.Response(slice.Build());
		} else if (mode == 9) {
			scanner.status = SyncHistoryScanStatus::RequestFailed; scanner._done = true;
		} else scanner.Response(slice.Build());
		const auto inventory = FinishProbe(scanner);
		const auto reply = MapDesktopSyncChatInventoryReply(Origin, 1, inventory);
		CHECK(mode == 0 ? reply.success && reply.records == SyncChatRecordPresence::Empty : !reply.success && reply.records == SyncChatRecordPresence::Unknown);
		const auto model = ResolveInventory(inventory);

		CHECK(mode == 0 ? model.status().phase == DesktopSyncChatPhase::Use && !model.status().postingHeld : model.status().postingHeld && !model.status().complete && model.status().phase == DesktopSyncChatPhase::Paused);
	}
	{
		auto legacy = ProbeScanReply(); auto response = NativeMessagesSlice();
		response.flags = MTP_flags(MTPDmessages_messagesSlice::Flag::f_inexact); response.count = MTP_int(2);
		legacy.Response(response.Build()); CHECK(legacy.status == SyncHistoryScanStatus::Complete);
		auto modern = ProbeScanReply(); modern._owned = [](const MTPMessage &) { return SyncChatMessageOwnership::Own; };
		modern.Response(NativeMessages().Build());
		CHECK(MapDesktopSyncChatInventoryReply(Origin,1,FinishProbe(modern)).records == SyncChatRecordPresence::Empty);
	}
	{
		auto scanner = ProbeScanReply(); scanner._owned = [](const MTPMessage &) { return SyncChatMessageOwnership::Other; };
		auto first = NativeMessagesSlice(); first.count = MTP_int(2); first.messages = MTP_vector<MTPMessage>({Mark({},20)}); scanner.Response(first.Build());
		CHECK(!scanner._done);
		auto second = first; second.messages = MTP_vector<MTPMessage>({Mark({},10)}); scanner.Response(second.Build());
		const auto inventory = FinishProbe(scanner);
		CHECK(MapDesktopSyncChatInventoryReply(Origin,1,inventory).records == SyncChatRecordPresence::Empty);
		for (auto bad = 0; bad < 4; ++bad) {
			auto changed = inventory;
			if (bad == 0) changed.read.reset();
			if (bad == 1) changed.read->status = SyncCandidateReadStatus::Incomplete;
			if (bad == 2) changed.scan.status = SyncHistoryScanStatus::RequestFailed;
			if (bad == 3) changed.accountUserId = 124;
			const auto reply = MapDesktopSyncChatInventoryReply(Origin,1,changed);
			CHECK(!reply.success && reply.records == SyncChatRecordPresence::Unknown);
		}
	}
	{
		auto model = DesktopSyncChatDiscovery(Origin, Cache(), true, {}, {}, QString(64,u'a'));
		model.Start(1000); auto scenario = Scenario(); scenario.failGlobal = true; Drive(model, scenario,1000);
		CHECK(model.status().postingHeld && !model.status().complete);
		CHECK(SerializeDesktopSyncChatCache(*model.cache()) == SerializeDesktopSyncChatCache(Cache()));
	}
}

}

int main() {
	QCompleteInventory(); FullResponseInventory();
	std::printf("COMPLETENESS %d checks, %d failures\n",Checks,Failures);
	return Failures ? 1 : 0;
}
