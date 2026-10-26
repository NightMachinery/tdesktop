#include "purple/purple_sync_chat_facts.h"
#include "sync-chat-native-cases.inc"

#include <cstdio>
#include <limits>

using namespace Purple;
namespace {

auto Checks = 0;
auto Failures = 0;
#define CHECK(value) do { ++Checks; if (!(value)) { ++Failures; std::printf("FAIL line%d: %s\n", __LINE__, #value); } } while (false)

const auto Origin = DesktopSyncChatOrigin{{0, 123}, u"login-a"_q};

DesktopSyncChatPeerInput Input(PeerToken chat) {
	auto input = DesktopSyncChatPeerInput{Origin, chat};
	input.canSendFiles = true;
	input.chosenSendAs = PeerToken{};
	return input;
}

MTPMessage Message(MTPDmessage::Flags flags, PeerToken target, const MTPPeer &sender = MTP_peerUser(MTP_long(123))) {
	auto values = NativeMessage();
	values.flags = MTP_flags(flags);
	values.id = MTP_int(10);
	values.date = MTP_int(100);
	values.message = MTP_string("#purplechatsync");
	values.from_id = sender;
	values.peer_id = target.kind == PeerTokenKind::Self ? MTP_peerUser(MTP_long(123))
		: target.kind == PeerTokenKind::User ? MTP_peerUser(MTP_long(target.id))
		: target.kind == PeerTokenKind::Chat ? MTP_peerChat(MTP_long(target.id))
		: MTP_peerChannel(MTP_long(target.id));
	values.media = MTP_messageMediaEmpty();
	return values.Build();
}

DesktopSyncChatPeer Self() {
	auto input = Input({});
	auto user = NativeUser(); user.flags = MTP_flags(MTPDuser::Flag::f_self); user.id = MTP_long(123);
	auto full = NativeUserFull(); full.id = MTP_long(123);
	input.user = user.Build(); input.userFull = full.Build();
	return MapDesktopSyncChatPeer(Origin, input);
}

void Messages() {
	const auto self = Self();
	CHECK(self.kind == SyncChatKind::Self && DesktopSyncChatEligibility(self) && bool(*DesktopSyncChatEligibility(self)));
	const auto original = Message({}, {});
	const auto facts = MapDesktopSyncChatMessage(Origin, self, original);
	CHECK(facts.regular == true && facts.out == false && !facts.sender);
	CHECK(IsOwnSyncChatMessage(facts));
	CHECK(ParseSyncChatMark(qs(original.c_message().vmessage()), false, GetSyncChatMessageOwnership(facts)).status == SyncChatMarkStatus::Supported);
	using F = MTPDmessage::Flag;
	for (const auto flag : {F::f_fwd_from, F::f_via_bot_id, F::f_via_business_bot_id,
		F::f_guestchat_via_from, F::f_from_scheduled, F::f_ttl_period, F::f_schedule_repeat_period}) {
		const auto excluded = MapDesktopSyncChatMessage(Origin, self, Message(flag, {}));
		CHECK(GetSyncChatMessageOwnership(excluded) == SyncChatMessageOwnership::Other);
	}
	auto media = NativeMessageMediaDocument(); media.flags = MTP_flags(MTPDmessageMediaDocument::Flag::f_ttl_seconds); media.ttl_seconds = MTP_int(3);
	auto ttl = NativeMessage(); ttl.flags = MTP_flags(F::f_media); ttl.id = MTP_int(10); ttl.date = MTP_int(100); ttl.peer_id = MTP_peerUser(MTP_long(123)); ttl.media = media.Build();
	CHECK(GetSyncChatMessageOwnership(MapDesktopSyncChatMessage(Origin, self, ttl.Build())) == SyncChatMessageOwnership::Other);
	CHECK(GetSyncChatMessageOwnership(MapDesktopSyncChatMessage(Origin, self, MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flags()), MTP_int(10), MTP_peerUser(MTP_long(123))))) == SyncChatMessageOwnership::Unknown);
	auto service = NativeMessageService(); service.id = MTP_int(10); service.peer_id = MTP_peerUser(MTP_long(123));
	CHECK(GetSyncChatMessageOwnership(MapDesktopSyncChatMessage(Origin, self, service.Build())) == SyncChatMessageOwnership::Other);
	for (const auto kind : {SyncChatKind::Private, SyncChatKind::Bot, SyncChatKind::BasicGroup, SyncChatKind::Supergroup, SyncChatKind::Broadcast}) {
		const auto target = PeerToken{kind == SyncChatKind::Private || kind == SyncChatKind::Bot ? PeerTokenKind::User
			: kind == SyncChatKind::BasicGroup ? PeerTokenKind::Chat : PeerTokenKind::Channel, 456};
		auto peer = DesktopSyncChatPeer{Origin, target, kind, true, true};
		for (const auto outgoing : {false, true}) {
			for (const auto &sender : {MTP_peerUser(MTP_long(123)), MTP_peerUser(MTP_long(456)), MTP_peerChannel(MTP_long(123)), MTP_peerChannel(MTP_long(456))}) {
				const auto own = MapDesktopSyncChatMessage(Origin, peer, Message((outgoing ? F::f_out : F()) | F::f_from_id, target, sender));
				const auto expected = outgoing && ((kind == SyncChatKind::Private || kind == SyncChatKind::Bot)
					|| (sender.type() == mtpc_peerUser && sender.c_peerUser().vuser_id().v == 123)
					|| (kind == SyncChatKind::Broadcast && sender.type() == mtpc_peerChannel && sender.c_peerChannel().vchannel_id().v == 456));
				CHECK(IsOwnSyncChatMessage(own) == expected);
			}
		}
		const auto absent = MapDesktopSyncChatMessage(Origin, peer, Message(F::f_out, target));
		CHECK(IsOwnSyncChatMessage(absent) == (kind != SyncChatKind::BasicGroup && kind != SyncChatKind::Supergroup));
		peer.soleAdmin.reset();
		CHECK(!IsOwnSyncChatMessage(MapDesktopSyncChatMessage(Origin, peer, Message(F::f_out, target))) || kind != SyncChatKind::Broadcast);
	}
	auto other = Origin; other.login = u"login-b"_q;
	CHECK(!IsOwnSyncChatMessage(MapDesktopSyncChatMessage(other, self, original)));
	other = Origin; other.account.environment = 1;
	CHECK(!IsOwnSyncChatMessage(MapDesktopSyncChatMessage(other, self, original)));
	CHECK(!IsOwnSyncChatMessage(MapDesktopSyncChatMessage(Origin, self, Message(F::f_out, {PeerTokenKind::User, 456}))));
}

void Peers() {
	CHECK(!MapDesktopSyncChatPeer(Origin, Input({})).facts);
	for (const auto bot : {false, true}) {
		auto input = Input({PeerTokenKind::User, 456});
		auto user = NativeUser(); user.id = MTP_long(456); user.flags = MTP_flags(bot ? MTPDuser::Flag::f_bot : MTPDuser::Flag());
		auto full = NativeUserFull(); full.id = MTP_long(456);
		input.user = user.Build();
		CHECK(!MapDesktopSyncChatPeer(Origin, input).facts);
		input.userFull = full.Build();
		auto peer = MapDesktopSyncChatPeer(Origin, input);
		CHECK(peer.kind == (bot ? SyncChatKind::Bot : SyncChatKind::Private));
		CHECK(DesktopSyncChatEligibility(peer) && bool(*DesktopSyncChatEligibility(peer)));
		CHECK(DesktopSyncChatEligibility(peer)->readers.warningRequired);
		input.canSendFiles.reset(); CHECK(!MapDesktopSyncChatPeer(Origin, input).facts); input.canSendFiles = true;
		input.chosenSendAs.reset(); CHECK(!MapDesktopSyncChatPeer(Origin, input).facts); input.chosenSendAs = PeerToken{};
		full.flags = MTP_flags(MTPDuserFull::Flag::f_ttl_period); full.ttl_period = MTP_int(60); input.userFull = full.Build();
		CHECK(DesktopSyncChatEligibility(MapDesktopSyncChatPeer(Origin, input))->refusal == SyncChatRefusal::AutoDelete);
		full.id = MTP_long(457); input.userFull = full.Build(); CHECK(!MapDesktopSyncChatPeer(Origin, input).facts);
	}
	for (const auto broadcast : {false, true}) {
		for (const auto creator : {false, true}) {
			for (const auto admins : {0, 1, 2}) {
				auto input = Input({PeerTokenKind::Channel, 456});
				auto chat = NativeChannel(); chat.id = MTP_long(456);
				chat.flags = MTP_flags((broadcast ? MTPDchannel::Flag::f_broadcast : MTPDchannel::Flag::f_megagroup) | (creator ? MTPDchannel::Flag::f_creator : MTPDchannel::Flag()));
				auto full = NativeChannelFull(); full.id = MTP_long(456); full.flags = MTP_flags(MTPDchannelFull::Flag::f_participants_count | (admins ? MTPDchannelFull::Flag::f_admins_count : MTPDchannelFull::Flag())); full.admins_count = MTP_int(admins); full.participants_count = MTP_int(1);
				input.group = chat.Build(); input.groupFull = full.Build();
				const auto peer = MapDesktopSyncChatPeer(Origin, input);
				const auto eligibility = DesktopSyncChatEligibility(peer);
				CHECK(broadcast && !admins ? !eligibility : eligibility && bool(*eligibility) == (!broadcast || creator && admins == 1));
				input.chosenSendAs = PeerToken{PeerTokenKind::Channel, 456};
				const auto asOther = DesktopSyncChatEligibility(MapDesktopSyncChatPeer(Origin, input));
				CHECK(!asOther || asOther->refusal == SyncChatRefusal::SendAsOther);
			}
		}
	}
	{
		auto input = Input({PeerTokenKind::Chat, 456});
		auto group = NativeChat(); group.id = MTP_long(456); group.participants_count = MTP_int(999);
		group.flags = MTP_flags(MTPDchat::Flag::f_creator);
		auto full = NativeChatFull(); full.id = MTP_long(456);
		const auto creator = MTP_chatParticipantCreator(MTP_flags(MTPDchatParticipantCreator::Flags()), MTP_long(123), MTP_string(""));
		full.participants = MTP_chatParticipants(MTP_long(456), MTP_vector<MTPChatParticipant>({creator}), MTP_int(1));
		input.group = group.Build(); input.groupFull = full.Build();
		auto peer = MapDesktopSyncChatPeer(Origin, input);
		CHECK(peer.facts && peer.facts->otherMembers == 0 && peer.facts->otherAdmins == 0);
		CHECK(DesktopSyncChatEligibility(peer) && !DesktopSyncChatEligibility(peer)->readers.warningRequired);
		full.participants = MTP_chatParticipants(MTP_long(456), MTP_vector<MTPChatParticipant>({creator, creator}), MTP_int(1));
		input.groupFull = full.Build(); CHECK(!MapDesktopSyncChatPeer(Origin, input).facts);
		full.participants = MTP_chatParticipantsForbidden(MTP_flags(MTPDchatParticipantsForbidden::Flags()), MTP_long(456), creator);
		input.groupFull = full.Build(); peer = MapDesktopSyncChatPeer(Origin, input);
		CHECK(DesktopSyncChatEligibility(peer) && DesktopSyncChatEligibility(peer)->readers.populationUnknown);
		group.flags = MTP_flags(MTPDchat::Flag::f_deactivated); input.group = group.Build();
		CHECK(DesktopSyncChatEligibility(MapDesktopSyncChatPeer(Origin, input))->refusal == SyncChatRefusal::Migrated);
	}
	CHECK(DesktopSyncChatToken(MTP_peerUser(MTP_long(123)), 123) == PeerToken{});
	CHECK(DesktopSyncChatToken(MTP_peerChannel(MTP_long(123)), 123) == (PeerToken{PeerTokenKind::Channel, 123}));
	CHECK(!DesktopSyncChatToken(MTP_peerChat(MTP_long(0)), 123));
	CHECK(!DesktopSyncChatToken(MTP_peerUser(MTP_long(-1)), 123));
	CHECK(!DesktopSyncChatToken(MTP_peerUser(MTP_long(123)), 0));
	auto invalid = Origin; invalid.login.clear(); CHECK(!ValidDesktopSyncChatOrigin(invalid));
	invalid = Origin; invalid.account.environment = -1; CHECK(!ValidDesktopSyncChatOrigin(invalid));
	invalid = Origin; invalid.account.userId = -1; CHECK(!ValidDesktopSyncChatOrigin(invalid));
}

}

int main() {
	Messages();
	Peers();
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
