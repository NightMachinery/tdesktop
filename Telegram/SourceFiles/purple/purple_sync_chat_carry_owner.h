#pragma once

#include "purple/purple_sync_chat_carry_service.h"
#include "purple/purple_sync_chat_consent.h"
#include "purple/purple_sync_chat_mark_actions.h"
#include "purple/purple_sync_chat_recovery.h"

namespace Purple {

// The five independent feature switches an owner consults.
enum class DesktopSyncChatCarrySwitch { Move, FollowFill, MarkRemoval, Cleanup, Recovery };
[[nodiscard]] bool DesktopSyncChatCarrySwitchActivated(DesktopSyncChatCarrySwitch value);

enum class DesktopSyncChatCarryKind { None, Resume, Move, Follow, Recovery, Mark };

// What the setup screen shows about the account's one carry operation.
struct DesktopSyncChatCarryState {
	DesktopSyncChatCarryKind kind = DesktopSyncChatCarryKind::None;
	uint64_t generation = 0;
	QString window;
	QString reason;
	std::optional<DesktopSyncCarryProgress> carry;
	bool provisioned = false;
	bool running = false;
	bool finished = false;
	bool removed = false;
};

// Same fields and signatures as T5K's native renewal hooks. Every entry is called on the GUI
// thread and answers on the GUI thread.
struct DesktopSyncChatCarryRenewal {
	Fn<void(DesktopSyncCarryCommand, DesktopSyncCarryPermit, DesktopSyncCarryFresh,
		Fn<void(std::optional<DesktopSyncCarryPermit>)>)> effect;
	Fn<void(DesktopSyncCarryCommand, DesktopSyncCleanupPermit,
		Fn<void(std::optional<DesktopSyncCleanupRead>)>)> beginCleanup;
	Fn<void(DesktopSyncCarryCommand, DesktopSyncCleanupPermit, DesktopSyncCleanupRead,
		DesktopSyncCarryReply, Fn<void(std::optional<DesktopSyncCleanupPermit>)>)> cleanup;
};

using DesktopSyncChatProvisionDone = Fn<void(
	std::optional<DesktopSyncChatCreateJournal>, std::optional<DesktopSyncChatPeer>)>;

// An unfinished carry journal found on disk at startup, with the opening its native
// transport needs. The opening keeps cache.carry; only the native resolver drops it.
struct DesktopSyncChatCarryResume {
	DesktopSyncChatSetupSnapshot opening;
	SyncCarryMode mode = SyncCarryMode::Switch;
};

// One account's native environment. "GUI" entries are called on the GUI thread only and may
// answer on any thread; "worker" entries run on the serialized store worker.
struct DesktopSyncChatCarryHost {
	Fn<bool(DesktopSyncChatCarrySwitch)> activated; // any thread
	Fn<bool()> current; // GUI: account, login and foreground still current
	Fn<std::optional<DesktopSyncChatDiscoveryScope>()> scope; // GUI: live account authority
	Fn<void(Fn<void()>)> worker;
	Fn<void(Fn<void()>)> main;
	Fn<std::optional<QString>(const DesktopSyncChatDiscoveryScope &)> directory; // any thread
	Fn<std::shared_ptr<DesktopSyncCarryGate::Lease>(const AccountKey &)> lease; // worker
	Fn<void()> quiesce; // GUI: retire ordinary publishers before a carry opens
	Fn<void(Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)>)> resolve; // GUI
	Fn<void(PeerToken, Fn<void(std::optional<DesktopSyncChatPeer>)>)> inspect; // GUI
	Fn<void(DesktopSyncChatMoveDraft, bool, DesktopSyncChatProvisionDone)> provision; // GUI
	Fn<std::optional<DesktopSyncChatCarryResume>(const DesktopSyncChatDiscoveryScope &)> resumeOpening; // worker
	Fn<DesktopSyncCarryTransport(const DesktopSyncChatSetupSnapshot &, DesktopSyncChatCarryRenewal,
		Fn<bool()>)> transport; // GUI
	Fn<DesktopSyncChatMarkPort(const DesktopSyncChatSetupSnapshot &, Fn<bool()>)> mark; // GUI
	Fn<void(const DesktopSyncChatCarryState &)> changed; // GUI
	Fn<void(DesktopSyncChatCarryKind)> settled; // GUI: an operation ended
};

[[nodiscard]] SyncChatCarryStoreScope DesktopSyncChatCarryStoreScope(
	const DesktopSyncChatSetupSnapshot &opening);
[[nodiscard]] std::optional<SyncCarryJournalStart> DesktopSyncChatMoveStart(
	const DesktopSyncChatMoveDraft &draft, const DesktopSyncChatPeer &target, bool cleanup);
[[nodiscard]] std::optional<SyncCarryJournalStart> DesktopSyncChatFollowStart(
	const DesktopSyncChatConsentSelection &selection);
[[nodiscard]] std::optional<DesktopSyncChatCarryResume> DesktopSyncChatCarryResumeOpening(
	const DesktopSyncChatDiscoveryScope &scope, const QString &directory);
[[nodiscard]] bool DesktopSyncChatCarryResumeActivated(
	SyncCarryMode mode, const Fn<bool(DesktopSyncChatCarrySwitch)> &activated);

struct DesktopSyncChatCarryTransportBox;

// Owns at most one carry operation for one account: an unfinished journal resumed at
// startup, a Change move, a Follow/Fill consent flow, a Lost recovery or a mark removal.
// GUI thread only. Each operation is revoked by close() or closeWindow(); its controllers
// and engine then stop on the worker and release the account's carry lease.
class DesktopSyncChatCarryOwner final {
public:
	explicit DesktopSyncChatCarryOwner(DesktopSyncChatCarryHost host);
	~DesktopSyncChatCarryOwner();

	[[nodiscard]] bool resume(QString window = {});
	[[nodiscard]] bool move(DesktopSyncChatMoveDraft draft, QString window);
	[[nodiscard]] bool follow(DesktopSyncChatConsentSelection selection);
	[[nodiscard]] bool recover(DesktopSyncChatRecoverySelection selection);
	[[nodiscard]] bool removeMark(DesktopSyncChatStaleMark row, QString window,
		DesktopSyncChatSetupSnapshot opening);
	void retry();
	void skipCleanup(bool confirmed);
	void discardInterrupted(bool confirmed);
	void close(QString reason);
	void closeWindow(const QString &window, QString reason);

	[[nodiscard]] bool busy() const;
	[[nodiscard]] const DesktopSyncChatCarryState &state() const;
	[[nodiscard]] std::optional<DesktopSyncCarryProgress> latest() const;

private:
	class Run;
	struct Active;
	[[nodiscard]] bool admit(DesktopSyncChatCarryKind kind, QString window,
		DesktopSyncChatCarrySwitch gate);
	[[nodiscard]] std::shared_ptr<DesktopSyncChatCarryTransportBox> transportFor(
		const DesktopSyncChatSetupSnapshot &opening, const std::shared_ptr<std::atomic<bool>> &live);
	[[nodiscard]] Fn<void(DesktopSyncCarryProgress)> reporter(uint64_t generation);
	[[nodiscard]] Fn<void(QString)> held(uint64_t generation);
	void progress(uint64_t generation, DesktopSyncCarryProgress value);
	void marked(uint64_t generation, DesktopSyncChatMarkProgress value);
	void finish(uint64_t generation);
	void publish();

	DesktopSyncChatCarryHost _host;
	std::unique_ptr<Active> _active;
	DesktopSyncChatCarryState _state;
	uint64_t _generation = 0;
	std::shared_ptr<DesktopSyncChatCarryOwner*> _self;

};

}
