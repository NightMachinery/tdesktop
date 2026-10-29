#pragma once

#include "purple/purple_sync_chat_carry_io.h"
#include "purple/purple_sync_chat_carry_gate.h"

namespace Purple {

struct DesktopSyncCarryOpening {
	QByteArray state;
	QByteArray cache;
};

struct DesktopSyncCarryCommand {
	uint64_t ticket = 0;
	uint64_t run = 0;
	QString operation;
	uint64_t revision = 0;
	SyncCarryJournalAction action = SyncCarryJournalAction::None;
	DesktopSyncChatOrigin origin;
	PeerToken source;
	PeerToken target;
	bool dispatch = false;
	QByteArray pendingRecord;
	int32_t markId = 0;
	std::vector<DesktopSyncCarryPermit> effects;
};

struct DesktopSyncCarryReply {
	DesktopSyncCarryCommand command;
	SyncCarryOutcome outcome = SyncCarryOutcome::Unknown;
	std::optional<DesktopSyncCarryFresh> fresh;
	std::optional<DesktopSyncChatInventory> source;
	std::optional<DesktopSyncChatInventory> destination;
	SyncCarrySourceAccess access = SyncCarrySourceAccess::Unknown;
	SyncCarrySendIntent sendIntent = SyncCarrySendIntent::Unknown;
	SyncChatDirectRead oldRead = SyncChatDirectRead::Unknown;
	QByteArray retainedLocalText;
	std::optional<MTPmessages_Messages> markResponse;
	int32_t markId = 0;
	std::vector<MTPUpdates> updates;
	SyncPlaylistCarryReceipt playlist;
	bool neverLandedConfirmed = false;
	QString neverLandedRequestId;
};

struct DesktopSyncCarryProgress {
	QString reason;
	SyncStoreStatus storeStatus = SyncStoreStatus::Ready;
	SyncCarryJournalStatus journalStatus = SyncCarryJournalStatus::Valid;
	std::optional<SyncCarryJournal> journal;
	std::optional<DesktopSyncCarryCommand> command;
	std::vector<SyncCarryJournal> retainedEffects;
	std::vector<QByteArray> retainedImages;
	std::optional<SyncLocalState> bindingRecoveryState;
	bool terminal = false;
	bool absent = false;
};

class DesktopSyncChatCarry final {
public:
	DesktopSyncChatCarry(QString directory, SyncChatCarryStoreScope scope,
		DesktopSyncChatOrigin origin, std::shared_ptr<std::atomic<bool>> admitted,
		std::shared_ptr<DesktopSyncCarryGate::Lease> lease,
		Fn<void(DesktopSyncCarryProgress)> progress, SyncDurableFault fault = {});
	~DesktopSyncChatCarry();
	void open(std::optional<SyncCarryJournalStart> start = std::nullopt,
		std::optional<DesktopSyncChatPeer> consentPeer = std::nullopt, QString stream = u"config"_q,
		std::optional<DesktopSyncCarryOpening> opening = std::nullopt);
	void resume();
	void complete(DesktopSyncCarryReply reply);
	void cancel();
	void discardInterrupted(bool confirmed);
	void skipCleanup(bool confirmed);
	[[nodiscard]] std::optional<DesktopSyncCarryPermit> renew(
		const DesktopSyncCarryCommand &command, const DesktopSyncCarryPermit &permit,
		const DesktopSyncCarryFresh &fresh);

private:
	[[nodiscard]] bool current() const;
	[[nodiscard]] bool accept(const DesktopSyncCarryCommand &command) const;
	[[nodiscard]] bool fresh(const DesktopSyncCarryFresh &facts, bool sameResolution = true);
	void advance();
	void issue(SyncCarryJournalAction action, bool dispatch = false,
		std::vector<DesktopSyncCarryPermit> effects = {});
	void publish(QString reason = {}, SyncChatCarryStorageResult result = {SyncStoreStatus::Ready});
	void retire();
	[[nodiscard]] bool result(SyncChatCarryStorageResult value);
	[[nodiscard]] SyncCarryJournalEvent event(SyncCarryEventKind kind) const;
	void plan(const DesktopSyncCarryReply &reply);
	void prepareEffects(const DesktopSyncCarryReply &reply);

	QString _directory;
	SyncChatCarryStoreScope _scope;
	DesktopSyncChatOrigin _origin;
	std::shared_ptr<std::atomic<bool>> _admitted;
	std::shared_ptr<DesktopSyncCarryGate::Lease> _lease;
	Fn<void(DesktopSyncCarryProgress)> _progress;
	SyncDurableFault _fault;
	std::unique_ptr<SyncChatCarryStore> _store;
	std::unique_ptr<DesktopSyncChatCarryIo> _io;
	std::optional<DesktopSyncCarryCommand> _command;
	std::optional<DesktopSyncChatPeer> _peer;
	uint64_t _run = 1;
	uint64_t _ticket = 0;
	std::optional<SyncCarryJournalStart> _start;
	std::optional<SyncChatCache> _recoveryWinner;
	bool _recoveryOnly = false;
	bool _needsFresh = false;
	bool _resumeRefused = false;
	bool _closed = false;

};

}
