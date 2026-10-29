#pragma once

#include "purple/purple_sync_chat_discovery.h"

namespace Purple {

struct DesktopSyncChatSetupSnapshot {
	DesktopSyncChatDiscoveryScope scope;
	DesktopSyncChatStatus status;
	SyncChatResolveInput input;
	std::optional<DesktopSyncChatCacheRecord> cache;
	uint64_t run = 0;
	QByteArray stateImage;
	QByteArray cacheImage;
};

enum class DesktopSyncChatSetupChoice { Create, Self, Existing };
enum class DesktopSyncChatSetupRefusal {
	None, Stale, Bound, Incomplete, Ineligible, UnknownReaders, ConsentRequired
};

struct DesktopSyncChatSetupSelection {
	DesktopSyncChatSetupChoice choice = DesktopSyncChatSetupChoice::Create;
	std::optional<DesktopSyncChatPeer> peer;
	bool readerConsent = false;
	bool provisionOnly = false;
};

[[nodiscard]] bool SameDesktopSyncChatDisclosure(
	const DesktopSyncChatPeer &opening, const DesktopSyncChatPeer &fresh);
[[nodiscard]] bool DesktopSyncChatFreshSetup(
	const DesktopSyncChatSetupSnapshot &snapshot);
[[nodiscard]] DesktopSyncChatSetupRefusal DesktopSyncChatSetupEligibility(
	const DesktopSyncChatSetupSnapshot &snapshot,
	const DesktopSyncChatPeer &peer);
[[nodiscard]] DesktopSyncChatSetupRefusal AcceptDesktopSyncChatSetup(
	const DesktopSyncChatSetupSnapshot &opening,
	const DesktopSyncChatSetupSnapshot &fresh,
	const DesktopSyncChatSetupSelection &selection);

enum class DesktopSyncChatMovePhase { Choosing, Confirming, Held, Cancelled };
enum class DesktopSyncChatMoveNotice { Held, Retryable, Committed, Complete, Cancelled };
struct SyncCarryJournal;

[[nodiscard]] DesktopSyncChatMoveNotice DesktopSyncChatMoveNoticeOf(
	const SyncCarryJournal &journal, bool storeReady, bool retryable);

struct DesktopSyncChatMoveOffer {
	uint64_t ticket = 0;
	DesktopSyncChatSetupSelection selection;
	QString sourceFingerprint;
	bool cleanup = false;
};

struct DesktopSyncChatMoveDraft {
	DesktopSyncChatSetupSnapshot opening;
	DesktopSyncChatSetupSelection selection;
	bool cleanup = false;
};

[[nodiscard]] QString DesktopSyncChatMoveFingerprint(
	const DesktopSyncChatSetupSnapshot &snapshot);
[[nodiscard]] DesktopSyncChatSetupRefusal DesktopSyncChatMoveEligibility(
	const DesktopSyncChatSetupSnapshot &snapshot, const DesktopSyncChatPeer &peer);
[[nodiscard]] bool DesktopSyncChatMoveActivated();

class DesktopSyncChatMoveAction final {
public:
	DesktopSyncChatMoveAction(DesktopSyncChatSetupSnapshot opening, Fn<bool()> current);
	[[nodiscard]] std::optional<DesktopSyncChatMoveOffer> choose(
		DesktopSyncChatSetupSelection selection);
	[[nodiscard]] bool confirm(const DesktopSyncChatMoveOffer &offer,
		const DesktopSyncChatSetupSnapshot &fresh,
		std::optional<DesktopSyncChatPeer> peer, bool cleanup, bool consent,
		Fn<void(DesktopSyncChatMoveDraft)> start);
	void dismiss(uint64_t ticket);
	void cancel();
	[[nodiscard]] DesktopSyncChatMovePhase phase() const;
	[[nodiscard]] QString reason() const;
	[[nodiscard]] const std::optional<DesktopSyncChatMoveDraft> &reviewed() const;

private:
	DesktopSyncChatSetupSnapshot _opening;
	Fn<bool()> _current;
	std::optional<DesktopSyncChatMoveOffer> _offer;
	std::optional<DesktopSyncChatMoveDraft> _reviewed;
	DesktopSyncChatMovePhase _phase = DesktopSyncChatMovePhase::Choosing;
	QString _reason;
	uint64_t _ticket = 0;

};

}
