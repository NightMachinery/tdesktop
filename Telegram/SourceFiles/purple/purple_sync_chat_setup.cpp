#include "purple/purple_sync_chat_setup.h"

namespace Purple {

bool SameDesktopSyncChatDisclosure(
		const DesktopSyncChatPeer &opening, const DesktopSyncChatPeer &fresh) {
	const auto first = DesktopSyncChatEligibility(opening);
	const auto second = DesktopSyncChatEligibility(fresh);
	return opening.origin == fresh.origin && opening.chat == fresh.chat
		&& opening.kind == fresh.kind && first && second && *first && *second
		&& !second->readers.populationUnknown
		&& first->readers == second->readers
		&& opening.creator == fresh.creator && opening.soleAdmin == fresh.soleAdmin
		&& opening.readerMembers == fresh.readerMembers;
}

bool DesktopSyncChatFreshSetup(const DesktopSyncChatSetupSnapshot &snapshot) {
	return ValidDesktopSyncChatOrigin(snapshot.scope.origin)
		&& snapshot.run != 0 && snapshot.scope.loginBinding.size() == 64
		&& !snapshot.scope.settingsBound && snapshot.scope.space.isEmpty()
		&& !snapshot.scope.mark && !snapshot.scope.carry && !snapshot.cache
		&& !snapshot.input.settingsBound && !snapshot.input.cache
		&& snapshot.input.settingsHint == SyncChatSettingsHint::Absent
		&& snapshot.input.marks.empty() && snapshot.status.complete
		&& snapshot.status.phase == DesktopSyncChatPhase::Unset
		&& snapshot.status.resolution.action == SyncChatAction::Unset;
}

DesktopSyncChatSetupRefusal DesktopSyncChatSetupEligibility(
		const DesktopSyncChatSetupSnapshot &snapshot,
		const DesktopSyncChatPeer &peer) {
	if (peer.origin != snapshot.scope.origin) {
		return DesktopSyncChatSetupRefusal::Stale;
	} else if (snapshot.scope.settingsBound || snapshot.scope.mark
		|| snapshot.scope.carry || !snapshot.scope.space.isEmpty()
		|| snapshot.cache || snapshot.input.settingsBound || snapshot.input.cache) {
		return DesktopSyncChatSetupRefusal::Bound;
	} else if (!DesktopSyncChatFreshSetup(snapshot)) {
		return DesktopSyncChatSetupRefusal::Incomplete;
	}
	const auto token = PeerTokenText(peer.chat, peer.origin.account.userId);
	if (!peer.facts || peer.facts->kind != peer.kind
		|| !token || ParsePeerToken(*token, peer.origin.account.userId) != peer.chat
		|| (peer.chat == PeerToken{}) != (peer.kind == SyncChatKind::Self)) {
		return DesktopSyncChatSetupRefusal::Ineligible;
	}
	const auto eligible = DesktopSyncChatEligibility(peer);
	if (!eligible || !*eligible) {
		return DesktopSyncChatSetupRefusal::Ineligible;
	} else if (eligible->readers.populationUnknown) {
		return DesktopSyncChatSetupRefusal::UnknownReaders;
	}
	return DesktopSyncChatSetupRefusal::None;
}

DesktopSyncChatSetupRefusal AcceptDesktopSyncChatSetup(
		const DesktopSyncChatSetupSnapshot &opening,
		const DesktopSyncChatSetupSnapshot &fresh,
		const DesktopSyncChatSetupSelection &selection) {
	if (opening.scope != fresh.scope || opening.run != fresh.run) {
		return DesktopSyncChatSetupRefusal::Stale;
	} else if (!DesktopSyncChatFreshSetup(fresh)) {
		return fresh.scope.settingsBound || fresh.scope.mark || fresh.cache
			? DesktopSyncChatSetupRefusal::Bound
			: DesktopSyncChatSetupRefusal::Incomplete;
	} else if (selection.choice == DesktopSyncChatSetupChoice::Create) {
		return selection.peer ? DesktopSyncChatSetupRefusal::Ineligible
			: DesktopSyncChatSetupRefusal::None;
	} else if (!selection.peer
		|| (selection.choice == DesktopSyncChatSetupChoice::Self)
			!= (selection.peer->chat == PeerToken{})) {
		return DesktopSyncChatSetupRefusal::Ineligible;
	}
	const auto result = DesktopSyncChatSetupEligibility(fresh, *selection.peer);
	if (result != DesktopSyncChatSetupRefusal::None) {
		return result;
	}
	const auto eligible = DesktopSyncChatEligibility(*selection.peer);
	return eligible->readers.warningRequired && !selection.readerConsent
		? DesktopSyncChatSetupRefusal::ConsentRequired
		: DesktopSyncChatSetupRefusal::None;
}

}
