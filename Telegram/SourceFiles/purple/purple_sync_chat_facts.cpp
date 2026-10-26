#include "purple/purple_sync_chat_facts.h"

#include <QtCore/QCryptographicHash>

#include <algorithm>
#include <limits>
#include <set>

namespace Purple {
namespace {

constexpr auto BroadcastPolicy = SyncChatBroadcastOwnership::CreatorAndSoleAdmin;

bool PublicNames(const tl::conditional<MTPVector<MTPUsername>> &names) {
	if (names) {
		for (const auto &name : names->v) {
			if (name.data().is_active()) {
				return true;
			}
		}
	}
	return false;
}

std::optional<bool> MediaTtl(const MTPMessageMedia &media) {
	if (media.type() == mtpc_messageMediaEmpty) {
		return false;
	} else if (media.type() == mtpc_messageMediaDocument) {
		return bool(media.c_messageMediaDocument().vttl_seconds());
	} else if (media.type() == mtpc_messageMediaPhoto) {
		return bool(media.c_messageMediaPhoto().vttl_seconds());
	}
	return {};
}

}

bool ValidDesktopSyncChatOrigin(const DesktopSyncChatOrigin &origin) {
	return origin.account.environment >= 0 && origin.account.environment <= 1 && origin.account.userId > 0
		&& origin.account.userId <= uint64_t(std::numeric_limits<int64_t>::max())
		&& !origin.login.isEmpty();
}

std::optional<PeerToken> DesktopSyncChatToken(
		const MTPPeer &peer,
		uint64_t selfUserId) {
	if (!selfUserId || selfUserId > uint64_t(std::numeric_limits<int64_t>::max())) {
		return {};
	}
	auto kind = PeerTokenKind::Self;
	auto id = int64_t(0);
	if (peer.type() == mtpc_peerUser) {
		kind = PeerTokenKind::User;
		id = peer.c_peerUser().vuser_id().v;
	} else if (peer.type() == mtpc_peerChat) {
		kind = PeerTokenKind::Chat;
		id = peer.c_peerChat().vchat_id().v;
	} else if (peer.type() == mtpc_peerChannel) {
		kind = PeerTokenKind::Channel;
		id = peer.c_peerChannel().vchannel_id().v;
	} else {
		return {};
	}
	if (id <= 0) {
		return {};
	}
	return (kind == PeerTokenKind::User && uint64_t(id) == selfUserId)
		? PeerToken{} : PeerToken{kind, uint64_t(id)};
}

DesktopSyncChatPeer MapDesktopSyncChatPeer(
		const DesktopSyncChatOrigin &origin,
		const DesktopSyncChatPeerInput &input) {
	auto result = DesktopSyncChatPeer{origin, input.chat};
	if (!ValidDesktopSyncChatOrigin(origin) || origin != input.origin
		|| !PeerTokenText(input.chat, origin.account.userId)) {
		return result;
	}
	const auto canonical = ParsePeerToken(*PeerTokenText(input.chat, origin.account.userId), origin.account.userId);
	if (!canonical || *canonical != input.chat) {
		return result;
	}
	auto facts = SyncChatFacts();
	if (input.chat.kind == PeerTokenKind::Self) {
		result.kind = facts.kind = SyncChatKind::Self;
		if (!input.user || input.user->type() != mtpc_user
			|| !input.user->c_user().is_self() || input.user->c_user().is_min()
			|| input.user->c_user().vid().v != origin.account.userId
			|| !input.userFull || input.userFull->data().vid().v != origin.account.userId) {
			return result;
		}
		const auto &full = input.userFull->data();
		facts.autoDelete = full.vttl_period() && full.vttl_period()->v != 0;
		facts.protectedContent = full.is_noforwards_my_enabled() || full.is_noforwards_peer_enabled();
		facts.paidMessages = full.vsend_paid_messages_stars() && full.vsend_paid_messages_stars()->v != 0;
		facts.canSendFiles = true;
		result.facts = facts;
		return result;
	}
	if (input.chat.kind == PeerTokenKind::User) {
		if (!input.user || input.user->type() != mtpc_user) {
			return result;
		}
		const auto &user = input.user->c_user();
		if (user.is_min() || user.is_deleted() || user.is_self()
			|| user.vid().v != int64_t(input.chat.id)) {
			return result;
		}
		result.kind = facts.kind = user.is_bot() ? SyncChatKind::Bot : SyncChatKind::Private;
		if (!input.userFull || input.userFull->data().vid().v != int64_t(input.chat.id)) {
			return result;
		}
		const auto &full = input.userFull->data();
		facts.protectedContent = full.is_noforwards_my_enabled() || full.is_noforwards_peer_enabled();
		facts.autoDelete = full.vttl_period() && full.vttl_period()->v != 0;
		facts.paidMessages = (full.vsend_paid_messages_stars() && full.vsend_paid_messages_stars()->v != 0)
			|| (user.vsend_paid_messages_stars() && user.vsend_paid_messages_stars()->v != 0);
		if (full.is_blocked() || full.is_contact_require_premium() || user.is_contact_require_premium()) {
			facts.canSendFiles = false;
		} else if (input.canSendFiles) {
			facts.canSendFiles = *input.canSendFiles;
		} else {
			return result;
		}
	} else if (input.chat.kind == PeerTokenKind::Chat) {
		if (!input.group || input.group->type() != mtpc_chat) {
			return result;
		}
		const auto &chat = input.group->c_chat();
		if (chat.vid().v != int64_t(input.chat.id)) {
			return result;
		}
		result.kind = facts.kind = SyncChatKind::BasicGroup;
		result.creator = facts.creator = chat.is_creator();
		facts.migrated = chat.is_deactivated() || bool(chat.vmigrated_to());
		if (!input.groupFull || input.groupFull->type() != mtpc_chatFull
			|| input.groupFull->c_chatFull().vid().v != int64_t(input.chat.id)) {
			return result;
		}
		const auto &full = input.groupFull->c_chatFull();
		facts.protectedContent = chat.is_noforwards();
		facts.autoDelete = full.vttl_period() && full.vttl_period()->v != 0;
		facts.admin = facts.creator || bool(chat.vadmin_rights());
		facts.anonymousAdmin = chat.vadmin_rights() && chat.vadmin_rights()->data().is_anonymous();
		if (full.vparticipants().type() == mtpc_chatParticipants) {
			const auto &participants = full.vparticipants().c_chatParticipants();
			if (participants.vchat_id().v != int64_t(input.chat.id)) {
				return result;
			}
			auto ids = std::set<int64_t>();
			auto admins = uint64_t(0);
			auto selfAdmin = false;
			for (const auto &participant : participants.vparticipants().v) {
				auto id = int64_t(0);
				auto admin = false;
				participant.match([&](const MTPDchatParticipant &value) {
					id = value.vuser_id().v;
				}, [&](const MTPDchatParticipantCreator &value) {
					id = value.vuser_id().v;
					admin = true;
				}, [&](const MTPDchatParticipantAdmin &value) {
					id = value.vuser_id().v;
					admin = true;
				});
				if (id <= 0 || !ids.emplace(id).second) {
					return result;
				}
				admins += admin ? 1 : 0;
				selfAdmin = selfAdmin || (admin && id == origin.account.userId);
			}
			if (!chat.is_left() && ids.contains(origin.account.userId)) {
				facts.otherMembers = ids.size() - 1;
				if (ids.size() <= 200) {
					result.readerMembers = std::vector<QString>();
					for (const auto id : ids) {
						if (uint64_t(id) != origin.account.userId) {
							result.readerMembers->push_back(QString::fromLatin1(
								QCryptographicHash::hash("purple-sync-chat-reader-v1:"
									+ QByteArray::number(origin.account.environment) + ':'
									+ QByteArray::number(origin.account.userId) + ':'
									+ QByteArray::number(id), QCryptographicHash::Sha256).toHex()));
						}
					}
					std::sort(result.readerMembers->begin(), result.readerMembers->end());
				}
				facts.otherAdmins = admins - (selfAdmin ? 1 : 0);
			}
		}
		if (!input.canSendFiles) {
			return result;
		}
		facts.canSendFiles = !chat.is_left() && *input.canSendFiles;
	} else if (input.chat.kind == PeerTokenKind::Channel) {
		if (!input.group || input.group->type() != mtpc_channel) {
			return result;
		}
		const auto &chat = input.group->c_channel();
		if (chat.is_min() || chat.vid().v != int64_t(input.chat.id)
			|| chat.is_broadcast() == chat.is_megagroup()) {
			return result;
		}
		result.kind = facts.kind = chat.is_monoforum() ? SyncChatKind::ChannelDirectMessages
			: chat.is_megagroup() ? SyncChatKind::Supergroup : SyncChatKind::Broadcast;
		result.creator = facts.creator = chat.is_creator();
		if (!input.groupFull || input.groupFull->type() != mtpc_channelFull
			|| input.groupFull->c_channelFull().vid().v != int64_t(input.chat.id)) {
			return result;
		}
		const auto &full = input.groupFull->c_channelFull();
		facts.publicUsername = (chat.vusername() && !qs(*chat.vusername()).isEmpty()) || PublicNames(chat.vusernames());
		facts.protectedContent = chat.is_noforwards();
		facts.forum = chat.is_forum();
		facts.linkedDiscussion = bool(full.vlinked_chat_id());
		facts.autoDelete = full.vttl_period() && full.vttl_period()->v != 0;
		facts.paidMessages = (full.vsend_paid_messages_stars() && full.vsend_paid_messages_stars()->v != 0)
			|| (chat.vsend_paid_messages_stars() && chat.vsend_paid_messages_stars()->v != 0);
		facts.slowMode = chat.is_slowmode_enabled() || (full.vslowmode_seconds() && full.vslowmode_seconds()->v > 0);
		facts.admin = facts.creator || bool(chat.vadmin_rights());
		facts.anonymousAdmin = chat.vadmin_rights() && chat.vadmin_rights()->data().is_anonymous();
		if (full.vdefault_send_as()) {
			const auto chosen = DesktopSyncChatToken(*full.vdefault_send_as(), origin.account.userId);
			if (!chosen) {
				return result;
			}
			facts.postsAsOther = *chosen != PeerToken{};
		}
		if (full.vadmins_count() && full.vadmins_count()->v > 0) {
			result.soleAdmin = facts.soleAdmin = full.vadmins_count()->v == 1;
			facts.otherAdmins = uint64_t(full.vadmins_count()->v - (facts.admin ? 1 : 0));
		}
		if (!chat.is_left() && full.vparticipants_count() && full.vparticipants_count()->v > 0) {
			facts.otherMembers = uint64_t(full.vparticipants_count()->v - 1);
		}
		if (!input.canSendFiles || (facts.kind == SyncChatKind::Broadcast && !result.soleAdmin.has_value())) {
			return result;
		}
		facts.canSendFiles = !chat.is_left() && *input.canSendFiles;
	}
	if (result.kind == SyncChatKind::Unknown || !input.chosenSendAs
		|| !PeerTokenText(*input.chosenSendAs, origin.account.userId)) {
		return result;
	}
	facts.postsAsOther = facts.postsAsOther || *input.chosenSendAs != PeerToken{};
	result.facts = facts;
	return result;
}

SyncChatMessageFacts MapDesktopSyncChatMessage(
		const DesktopSyncChatOrigin &origin,
		const DesktopSyncChatPeer &peer,
		const MTPMessage &message) {
	auto result = SyncChatMessageFacts();
	if (!ValidDesktopSyncChatOrigin(origin) || origin != peer.origin) {
		return result;
	}
	result.kind = peer.kind;
	result.selfUserId = origin.account.userId;
	result.creator = peer.creator;
	result.soleAdmin = peer.soleAdmin;
	result.broadcastOwnership = BroadcastPolicy;
	if (message.type() == mtpc_messageService) {
		result.regular = false;
		return result;
	} else if (message.type() != mtpc_message) {
		return result;
	}
	const auto &data = message.c_message();
	const auto target = DesktopSyncChatToken(data.vpeer_id(), origin.account.userId);
	if (!target || *target != peer.chat || data.vid().v <= 0 || data.vdate().v <= 0) {
		return SyncChatMessageFacts();
	}
	result.regular = true;
	result.forwarded = bool(data.vfwd_from());
	result.viaBot = bool(data.vvia_bot_id()) || bool(data.vvia_business_bot_id()) || bool(data.vguestchat_via_from());
	result.scheduled = data.is_from_scheduled() || bool(data.vschedule_repeat_period());
	result.hasTtl = data.vttl_period() ? std::optional(true)
		: data.vmedia() ? MediaTtl(*data.vmedia()) : std::optional(false);
	result.out = data.is_out();
	if (data.vfrom_id()) {
		result.sender = DesktopSyncChatToken(*data.vfrom_id(), origin.account.userId);
	}
	result.sentAsOther = false;
	if (peer.kind == SyncChatKind::BasicGroup || peer.kind == SyncChatKind::Supergroup) {
		result.sentAsOther = result.sender ? std::optional(*result.sender != PeerToken{}) : std::nullopt;
	} else if (peer.kind == SyncChatKind::Broadcast && data.vfrom_id()) {
		result.sentAsOther = result.sender
			? std::optional(*result.sender != PeerToken{} && *result.sender != peer.chat)
			: std::nullopt;
	}
	return result;
}

std::optional<SyncChatEligibilityResult> DesktopSyncChatEligibility(
		const DesktopSyncChatPeer &peer) {
	return peer.facts ? std::optional(SyncChatEligibility(*peer.facts, BroadcastPolicy)) : std::nullopt;
}

}
