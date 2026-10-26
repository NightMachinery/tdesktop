#pragma once

#include "base/weak_ptr.h"
#include "purple/purple_sync_chat_setup.h"

namespace Purple {

enum class DesktopSyncChatCreateStep {
	Resolve, Create, ReconcileCreate, ApplyUpdates, PeerInfo, Inventory,
	PostMark, ReconcileMark, ReadMark, Pin, Mute, Archive, Commit, Done
};
enum class DesktopSyncChatCreatePhase { Idle, Running, Held, Limit, Complete, Cancelled };
enum class DesktopSyncChatCreateOutcome { Okay, Rejected, Ambiguous, Limit, FloodWait, Restricted };
enum class DesktopSyncChatCreationProof { None, TitleOnly, MatchedRequest };

struct DesktopSyncChatCreateJournal {
	AccountKey account;
	QString authority;
	QString operation;
	DesktopSyncChatSetupChoice choice = DesktopSyncChatSetupChoice::Create;
	DesktopSyncChatCreateStep step = DesktopSyncChatCreateStep::Create;
	std::optional<PeerToken> chat;
	bool created = false;
	bool pending = false;
	bool cancelled = false;
	bool adopted = false;
	int createAttempts = 0;
	int64_t retryAt = 0;
	uint64_t randomId = 0;
	int32_t markId = 0;
	int64_t markDate = 0;
	QByteArray updates;
	QString readerConsent;
	QJsonObject readerSnapshot;
	bool newReaders = false;
	std::optional<DesktopSyncChatCreateStep> decorationAfterCache;
};

struct DesktopSyncChatCreateCommand {
	DesktopSyncChatOrigin origin;
	QString operation;
	uint64_t ticket = 0;
	DesktopSyncChatCreateStep step = DesktopSyncChatCreateStep::Resolve;
	DesktopSyncChatCreateJournal journal;
	bool silent = true;
};

struct DesktopSyncChatCreateReply {
	DesktopSyncChatOrigin origin;
	QString operation;
	uint64_t ticket = 0;
	DesktopSyncChatCreateOutcome outcome = DesktopSyncChatCreateOutcome::Ambiguous;
	std::optional<DesktopSyncChatSetupSnapshot> resolution;
	DesktopSyncChatCreationProof creationProof = DesktopSyncChatCreationProof::None;
	QByteArray updates;
	std::optional<DesktopSyncChatPeer> peer;
	std::optional<MTPmessages_Messages> readback;
	std::optional<DesktopSyncChatInventory> inventory;
	uint64_t randomId = 0;
	int32_t markId = 0;
	int retryAfter = 0;
};

[[nodiscard]] QString DesktopSyncChatSetupAuthority(
	const DesktopSyncChatDiscoveryScope &scope);
[[nodiscard]] QString DesktopSyncChatReaderConsent(const DesktopSyncChatPeer &peer);
[[nodiscard]] QString DesktopSyncChatMarkText();
[[nodiscard]] MTPchannels_CreateChannel DesktopSyncChatCreateRequest();
[[nodiscard]] std::optional<PeerToken> DesktopSyncChatCreatedChannel(
	const QByteArray &updates, uint64_t selfUserId);
[[nodiscard]] std::optional<QByteArray> SerializeDesktopSyncChatCreateJournal(
	const DesktopSyncChatCreateJournal &journal);
[[nodiscard]] std::optional<DesktopSyncChatCreateJournal> ParseDesktopSyncChatCreateJournal(
	const QByteArray &bytes, const QString &authority, const AccountKey &account);
[[nodiscard]] std::optional<DesktopSyncChatCreateJournal> BeginDesktopSyncChatCreate(
	const DesktopSyncChatSetupSnapshot &snapshot,
	const DesktopSyncChatSetupSelection &selection);

struct DesktopSyncChatConsentOffer {
	DesktopSyncChatOrigin origin;
	QString operation;
	uint64_t ticket = 0;
	DesktopSyncChatPeer peer;
};

[[nodiscard]] std::optional<DesktopSyncChatCacheRecord> DesktopSyncChatSetupCache(
	const DesktopSyncChatCreateJournal &journal,
	const DesktopSyncChatDiscoveryScope &scope, QString title);

class DesktopSyncChatSetupExecutor final {
public:
	DesktopSyncChatSetupExecutor(DesktopSyncChatOrigin origin,
		DesktopSyncChatCreateJournal journal,
		Fn<bool(const DesktopSyncChatCreateJournal &)> save,
		Fn<bool()> foregroundCurrent);
	void Start(int64_t now);
	void Cancel();
	void UseSelf(int64_t now);
	[[nodiscard]] std::optional<DesktopSyncChatConsentOffer> offeredReaders() const;
	[[nodiscard]] bool Reconsent(const DesktopSyncChatConsentOffer &offer, int64_t now);
	[[nodiscard]] std::optional<DesktopSyncChatCreateCommand> Next();
	[[nodiscard]] bool Complete(const DesktopSyncChatCreateReply &reply, int64_t now);
	[[nodiscard]] const DesktopSyncChatCreateJournal &journal() const;
	[[nodiscard]] DesktopSyncChatCreatePhase phase() const;
	[[nodiscard]] QString reason() const;

private:
	bool Save(DesktopSyncChatCreateJournal next);
	void Hold(QString reason);
	void ContinueAfterPeer();
	bool AcceptMark(const MTPMessage &message);
	bool AcceptReadback(const MTPmessages_Messages &response, int32_t id);

	DesktopSyncChatOrigin _origin;
	DesktopSyncChatCreateJournal _journal;
	Fn<bool(const DesktopSyncChatCreateJournal &)> _save;
	Fn<bool()> _current;
	std::optional<DesktopSyncChatCreateCommand> _issued;
	std::optional<DesktopSyncChatPeer> _peer;
	std::optional<SyncChatCache> _resolvedMark;
	std::optional<DesktopSyncChatConsentOffer> _offer;
	bool _inventoryReady = false;
	DesktopSyncChatCreatePhase _phase = DesktopSyncChatCreatePhase::Idle;
	DesktopSyncChatCreateStep _next = DesktopSyncChatCreateStep::Resolve;
	uint64_t _ticket = 0;
	QString _reason;

};

}
