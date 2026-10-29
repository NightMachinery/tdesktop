#define PURPLE_SYNC_CHAT_SETUP_EMBEDDED
#include "test_sync_chat_alignment.cpp"
#include "purple/purple_sync_chat_setup.h"
#include "purple/purple_sync_chat_journal.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QThread>

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

DesktopSyncChatSetupSnapshot BoundMove(PeerToken source = {}) {
	auto snapshot = FreshSetup();
	auto state = SyncLocalState();
	state.version = source == PeerToken{} ? 1 : 2;
	state.install = u"in-"_q + QString(26, u'a');
	state.space = u"sp-"_q + QString(26, u'a');
	state.createdDevice = u"Synthetic"_q;
	state.bindingToken = QString(32, u'a');
	state.chat = source;
	snapshot.stateImage = SerializeSyncLocalState(state).canonical;
	snapshot.scope.root = u"synthetic-root"_q;
	snapshot.scope.binding = state.bindingToken.toLatin1();
	snapshot.scope.settingsBound = true;
	snapshot.scope.settingsChat = source;
	snapshot.scope.space = state.space;
	snapshot.status.phase = DesktopSyncChatPhase::Use;
	snapshot.status.postingHeld = false;
	snapshot.status.resolution.postingHeld = false;
	snapshot.status.resolution.action = SyncChatAction::Use;
	snapshot.status.resolution.readChat = source;
	snapshot.input.settingsBound = true;
	snapshot.input.settingsChat = source;
	return snapshot;
}

void BoundMoveActions() {
	CHECK(QCoreApplication::instance());
	CHECK(QThread::currentThread() == QCoreApplication::instance()->thread());
	CHECK(!DesktopSyncChatMoveActivated());
	for (const auto source : {PeerToken{}, PeerToken{PeerTokenKind::Chat, 999}}) {
		const auto opening = BoundMove(source);
		CHECK(!DesktopSyncChatMoveFingerprint(opening).isEmpty());
		CHECK(!DesktopSyncChatFreshSetup(opening));
		for (auto route : SetupRoutes()) {
			const auto selected = DesktopSyncChatSetupSelection{
				route.peer.chat == PeerToken{} ? DesktopSyncChatSetupChoice::Self
					: DesktopSyncChatSetupChoice::Existing, route.peer};
			const auto eligible = DesktopSyncChatMoveEligibility(opening, route.peer);
			auto current = true;
			auto action = DesktopSyncChatMoveAction(opening, [&] { return current; });
			const auto offer = action.choose(selected);
			CHECK(bool(offer) == (eligible == DesktopSyncChatSetupRefusal::None));
			if (!offer) { continue; }
			CHECK(offer->cleanup == (source != PeerToken{}));
			auto writes = 0;
			CHECK(action.confirm(*offer, opening, route.peer, !offer->cleanup, true,
				[&](DesktopSyncChatMoveDraft) { ++writes; }));
			CHECK(writes == 0 && action.reason() == u"move-qualification-held"_q);
			CHECK(action.reviewed() && action.reviewed()->cleanup == !offer->cleanup);
			CHECK(action.reviewed()->selection.readerConsent);
			CHECK(action.reviewed()->opening.stateImage == opening.stateImage);
			CHECK(action.phase() == DesktopSyncChatMovePhase::Held);
			CHECK(!action.confirm(*offer, opening, route.peer, true, true,
				[&](DesktopSyncChatMoveDraft) { ++writes; }));
			CHECK(writes == 0);
			for (auto n = 0; n != 19; ++n) {
				auto fresh = opening;
				auto peer = route.peer;
				auto guarded = DesktopSyncChatMoveAction(opening, [&] { return current; });
				const auto candidate = guarded.choose(selected); CHECK(candidate);
				if (n == 0) fresh.scope.origin.account.userId = 124;
				if (n == 1) fresh.scope.origin.login += u"other"_q;
				if (n == 2) fresh.scope.binding += 'b';
				if (n == 3) fresh.scope.loginBinding = QString(64, u'b');
				if (n == 4) fresh.scope.mode = AccountWorkMode::Follow;
				if (n == 5) fresh.scope.owner = Origin.account;
				if (n == 6) fresh.scope.space += u"changed"_q;
				if (n == 7) fresh.scope.settingsChat = route.peer.chat;
				if (n == 8) fresh.status.complete = false;
				if (n == 9) fresh.status.resolution.winner = Cache().mark;
				if (n == 10) fresh.status.resolution.action = SyncChatAction::Follow;
				if (n == 11) fresh.stateImage += ' ';
				if (n == 12) fresh.cache = Cache();
				if (n == 13) current = false;
				if (n == 14) peer.origin.login += u"stale"_q;
				if (n == 15) peer.facts.reset();
				if (n == 16) guarded.cancel();
				if (n == 17) guarded.dismiss(candidate->ticket);
				if (n == 18) fresh.scope.carry = true;
				CHECK(!guarded.confirm(*candidate, fresh, peer, true, true,
					[&](DesktopSyncChatMoveDraft) { ++writes; }));
				CHECK(writes == 0);
				current = true;
			}
		}
		auto action = DesktopSyncChatMoveAction(opening, [] { return true; });
		const auto first = action.choose({}); CHECK(first);
		action.dismiss(first->ticket);
		const auto second = action.choose({}); CHECK(second && second->ticket != first->ticket);
		CHECK(!action.confirm(*first, opening, {}, true, true, {}));
		CHECK(!action.confirm(*second, opening, {}, true, false, {}));
		const auto third = action.choose({}); CHECK(third);
		CHECK(action.confirm(*third, opening, {}, true, true, {}));
		CHECK(action.reason() == u"move-qualification-held"_q);
		CHECK(action.reviewed() && action.reviewed()->selection.provisionOnly);
		action.cancel(); CHECK(!action.reviewed());
	}
	const auto opening = BoundMove();
	for (auto mode = 0; mode != 6; ++mode) {
		auto peer = GroupMembers({456});
		if (mode == 0) peer.facts->protectedContent = true;
		if (mode == 1) peer.facts->canSendFiles = false;
		if (mode == 2) peer.facts->otherMembers.reset();
		if (mode == 3) peer.facts->postsAsOther = true;
		if (mode == 4) peer.facts.reset();
		if (mode == 5) peer.origin.account.environment = 1;
		CHECK(DesktopSyncChatMoveEligibility(opening, peer) != DesktopSyncChatSetupRefusal::None);
	}
	auto reentrant = std::unique_ptr<DesktopSyncChatMoveAction>();
	auto revoke = false;
	reentrant = std::make_unique<DesktopSyncChatMoveAction>(opening, [&] {
		if (revoke) { reentrant->cancel(); }
		return true;
	});
	const auto offer = reentrant->choose({}); CHECK(offer);
	revoke = true;
	CHECK(!reentrant->confirm(*offer, opening, {}, false, true, {}));
	CHECK(reentrant->phase() == DesktopSyncChatMovePhase::Cancelled);
}

void MoveNotices() {
	auto journal = SyncCarryJournal();
	CHECK(DesktopSyncChatMoveNoticeOf(journal, true, false) == DesktopSyncChatMoveNotice::Held);
	CHECK(DesktopSyncChatMoveNoticeOf(journal, true, true) == DesktopSyncChatMoveNotice::Retryable);
	CHECK(DesktopSyncChatMoveNoticeOf(journal, false, true) == DesktopSyncChatMoveNotice::Held);
	journal.phase = SyncCarryPhase::Done;
	CHECK(DesktopSyncChatMoveNoticeOf(journal, true, false) == DesktopSyncChatMoveNotice::Held);
	journal.stateCommitted = true; journal.cacheCompleted = true;
	journal.destinationVerified = true; journal.markVerified = true;
	CHECK(DesktopSyncChatMoveNoticeOf(journal, true, false) == DesktopSyncChatMoveNotice::Complete);
	for (const auto phase : {SyncCarryPhase::Cleanup, SyncCarryPhase::CleanupSkipped}) {
		journal.phase = phase;
		CHECK(DesktopSyncChatMoveNoticeOf(journal, true, true) == DesktopSyncChatMoveNotice::Committed);
		for (auto n = 0; n != 4; ++n) {
			auto pending = journal;
			if (n == 0) pending.stateCommitted = false;
			if (n == 1) pending.cacheCompleted = false;
			if (n == 2) pending.destinationVerified = false;
			if (n == 3) pending.markVerified = false;
			CHECK(DesktopSyncChatMoveNoticeOf(pending, true, false) == DesktopSyncChatMoveNotice::Held);
		}
	}
	journal = {}; journal.phase = SyncCarryPhase::Aborted;
	CHECK(DesktopSyncChatMoveNoticeOf(journal, true, false) == DesktopSyncChatMoveNotice::Cancelled);
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

int main(int argc, char **argv) {

	AuthorMain(); ServiceReceiptTransitions(); RecentScopes(); RealStoreScope(); FreshBeforePost(); ReaderWarnings(); InventoryPageLimits(); SetupChoices(); Decision8();
	auto app = QCoreApplication(argc, argv);
	BoundMoveActions(); MoveNotices();
	std::printf("T3A %d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
