#define PURPLE_SYNC_CHAT_SETUP_EMBEDDED
#include "test_sync_chat_alignment.cpp"
#include "purple/purple_sync_chat_setup.h"

namespace {

std::vector<DesktopSyncChatIo> SetupRoutes() {
	return { Route({}), Route({PeerTokenKind::User, 456}),
		Route({PeerTokenKind::User, 456}, true, true),
		Route({PeerTokenKind::Chat, 789}), Route({PeerTokenKind::Channel, 456}),
		Route({PeerTokenKind::Channel, 456}, false) };
}

DesktopSyncChatSetupSnapshot FreshSetup() {
	auto snapshot = DesktopSyncChatSetupSnapshot();
	snapshot.scope.origin = Origin;
	snapshot.scope.loginBinding = QString(64, u'a');
	snapshot.status.phase = DesktopSyncChatPhase::Unset;
	snapshot.status.resolution.action = SyncChatAction::Unset;
	snapshot.status.complete = true;
	snapshot.input.settingsHint = SyncChatSettingsHint::Absent;
	snapshot.run = 1;
	return snapshot;
}

void SetupChoices() {
	const auto opening = FreshSetup();
	CHECK(DesktopSyncChatFreshSetup(opening));
	CHECK(AcceptDesktopSyncChatSetup(opening, opening, {}) == DesktopSyncChatSetupRefusal::None);
	for (auto n = 0; n != 14; ++n) {
		auto changed = opening;
		if (n == 0) changed.scope.origin.account.environment = 1;
		if (n == 1) changed.scope.origin.account.userId = 124;
		if (n == 2) changed.scope.origin.login += u"other"_q;
		if (n == 3) changed.scope.loginBinding = QString(64, u'b');
		if (n == 4) changed.scope.root += u"other"_q;
		if (n == 5) changed.scope.binding += 'b';
		if (n == 6) changed.scope.owner = Origin.account;
		if (n == 7) changed.scope.mode = AccountWorkMode::Follow;
		if (n == 8) ++changed.run;
		if (n == 9) changed.scope.settingsBound = true;
		if (n == 10) changed.scope.space = QString(32, u'a');
		if (n == 11) changed.scope.mark = Cache().mark;
		if (n == 12) changed.scope.carry = true;
		if (n == 13) changed.scope.settingsChat = {PeerTokenKind::Channel, 999};
		CHECK(AcceptDesktopSyncChatSetup(opening, changed, {}) == DesktopSyncChatSetupRefusal::Stale);
	}
	for (auto n = 0; n != 10; ++n) {
		auto bound = opening;
		if (n == 0) bound.scope.settingsBound = true;
		if (n == 1) bound.cache = Cache();
		if (n == 2) bound.scope.carry = true;
		if (n == 3) bound.input.cache = Cache().mark;
		if (n == 4) bound.input.settingsBound = true;
		if (n == 5) bound.input.settingsHint = SyncChatSettingsHint::Found;
		if (n == 6) bound.status.complete = false;
		if (n == 7) bound.status.phase = DesktopSyncChatPhase::Use;
		if (n == 8) bound.status.resolution.action = SyncChatAction::Follow;
		if (n == 9) bound.input.marks.push_back({Cache().mark});
		CHECK(!DesktopSyncChatFreshSetup(bound));
		CHECK(AcceptDesktopSyncChatSetup(bound, bound, {}) != DesktopSyncChatSetupRefusal::None);
	}
	for (auto route : SetupRoutes()) {
		auto peer = route.peer;
		const auto shown = DesktopSyncChatSetupEligibility(opening, peer);
		CHECK(shown == DesktopSyncChatSetupRefusal::None);
		const auto choice = peer.chat == PeerToken{} ? DesktopSyncChatSetupChoice::Self
			: DesktopSyncChatSetupChoice::Existing;
		const auto eligible = DesktopSyncChatEligibility(peer);
		CHECK(AcceptDesktopSyncChatSetup(opening, opening, {choice, peer, false})
			== (eligible->readers.warningRequired ? DesktopSyncChatSetupRefusal::ConsentRequired : shown));
		CHECK(AcceptDesktopSyncChatSetup(opening, opening, {choice, peer, true}) == shown);
		CHECK(SameDesktopSyncChatDisclosure(peer, peer));
		peer.origin.login += u"other"_q;
		CHECK(DesktopSyncChatSetupEligibility(opening, peer) == DesktopSyncChatSetupRefusal::Stale);
	}
	auto group = GroupMembers({456});
	CHECK(!SameDesktopSyncChatDisclosure(group, GroupMembers({789})));
	CHECK(!SameDesktopSyncChatDisclosure(group, GroupMembers({})));
	CHECK(!SameDesktopSyncChatDisclosure(group, GroupMembers({456, 789})));
	CHECK(!SameDesktopSyncChatDisclosure(group, GroupMembers({456}, false)));
	for (auto n = 0; n != 13; ++n) {
		auto peer = group;
		if (n == 0) peer.facts->publicUsername = true;
		if (n == 1) peer.facts->protectedContent = true;
		if (n == 2) peer.facts->forum = true;
		if (n == 3) peer.facts->linkedDiscussion = true;
		if (n == 4) peer.facts->autoDelete = true;
		if (n == 5) peer.facts->paidMessages = true;
		if (n == 6) peer.facts->canSendFiles = false;
		if (n == 7) peer.facts->postsAsOther = true;
		if (n == 8) peer.facts->migrated = true;
		if (n == 9) { peer.facts->slowMode = true; peer.facts->admin = false; }
		if (n == 10) peer.facts->otherMembers.reset();
		if (n == 11) peer.facts->otherAdmins.reset();
		if (n == 12) peer.facts.reset();
		const auto shown = DesktopSyncChatSetupEligibility(opening, peer);
		CHECK(shown != DesktopSyncChatSetupRefusal::None);
		CHECK(AcceptDesktopSyncChatSetup(opening, opening,
			{DesktopSyncChatSetupChoice::Existing, peer, true}) == shown);
	}
}

void Decision8() {
	for (const auto route : SetupRoutes()) {
		for (auto mode = 0; mode != 5; ++mode) {
			auto discovery = DesktopSyncChatDiscovery(Origin, Cache(route.peer.chat),
				false, {}, {}, QString(64, u'a'));
			discovery.StartBeforePost(1000);
			const auto command = discovery.command();
			CHECK(command.step == DesktopSyncChatStep::ExactMark);
			auto reply = DesktopSyncChatReply{Origin, command.ticket, true};
			reply.peer = route.peer;
			reply.chatReadable = mode != 3;
			if (mode != 0) {
				reply.messages.push_back(MTP_messageEmpty(
					MTP_flags(mode == 2 ? MTPDmessageEmpty::Flag::f_peer_id : MTPDmessageEmpty::Flags()),
					MTP_int(mode == 4 ? 2 : 1), MTP_peerUser(MTP_long(999))));
			}
			CHECK(discovery.Complete(reply, 1000));
			CHECK(discovery.input().cacheRead == (mode == 1
				? SyncChatDirectRead::MarkGone : SyncChatDirectRead::Unknown));
			if (mode != 1) CHECK(discovery.status().postingHeld);
		}
		auto empty = MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flags()), MTP_int(1), MTPPeer());
		auto response = Response(route, {empty});
		auto pages = DesktopSyncChatHistoryGuard(123, route.peer.chat);
		CHECK(pages.Add(response, 0) == DesktopSyncChatPageStatus::Incomplete);
		CHECK(!DesktopSyncChatRecordMessage(route, Response(route, {}), 1));
	}
}

}

int main() {
	AuthorMain(); ServiceReceiptTransitions(); RecentScopes(); RealStoreScope(); FreshBeforePost(); ReaderWarnings(); InventoryPageLimits(); SetupChoices(); Decision8();
	std::printf("T3A %d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
