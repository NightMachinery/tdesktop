#pragma once

#include "purple/purple_sync_chat_setup.h"
#include "purple/purple_sync_chat_carry_executor.h"

namespace Purple {

enum class DesktopSyncChatConsentChoice { Proceed, Pause };

struct DesktopSyncChatFollowOffer {
	uint64_t ticket = 0;
	QString window;
	QString fingerprint;
	QString stream;
	SyncChatAction action = SyncChatAction::Invalid;
	PeerToken source;
	SyncChatCache winner;
	DesktopSyncChatPeer peer;
	bool askReaders = false;
	bool firstPost = false;
};

struct DesktopSyncChatConsentSelection {
	DesktopSyncChatSetupSnapshot opening;
	DesktopSyncChatFollowOffer offer;
	DesktopSyncChatConsentChoice choice = DesktopSyncChatConsentChoice::Proceed;
};

[[nodiscard]] bool DesktopSyncChatFollowFillActivated();
[[nodiscard]] QString DesktopSyncChatConsentFingerprint(const DesktopSyncChatSetupSnapshot &snapshot);
[[nodiscard]] bool DesktopSyncChatConsentMatches(
	const DesktopSyncChatConsentSelection &selection, const DesktopSyncChatSetupSnapshot &fresh,
	const DesktopSyncChatPeer &peer);

class DesktopSyncChatConsentAction final {
public:
	DesktopSyncChatConsentAction(DesktopSyncChatSetupSnapshot opening, QString window,
		Fn<bool()> current, Fn<bool()> foreground);
	[[nodiscard]] std::optional<DesktopSyncChatFollowOffer> offer(
		const DesktopSyncChatPeer &peer, QString stream);
	[[nodiscard]] std::optional<DesktopSyncChatConsentSelection> accept(
		const DesktopSyncChatFollowOffer &offer, const DesktopSyncChatSetupSnapshot &fresh,
		const DesktopSyncChatPeer &peer, DesktopSyncChatConsentChoice choice, bool disclosed);
	void cancel();

private:
	[[nodiscard]] bool current() const;
	DesktopSyncChatSetupSnapshot _opening;
	QString _window;
	Fn<bool()> _current;
	Fn<bool()> _foreground;
	std::optional<DesktopSyncChatFollowOffer> _offer;
	uint64_t _ticket = 0;
	bool _closed = false;

};

struct DesktopSyncChatConsentPort {
	Fn<DesktopSyncChatOrigin()> origin;
	Fn<QString()> login;
	Fn<QString()> window;
	Fn<bool()> current;
	Fn<bool()> foreground;
	Fn<void(Fn<void()>)> worker;
	Fn<void(Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)>)> resolve;
	Fn<void(PeerToken, Fn<void(std::optional<DesktopSyncChatPeer>)>)> inspect;
	Fn<std::optional<SyncCarryJournalStart>(const DesktopSyncChatConsentSelection &)> begin;
	Fn<std::unique_ptr<DesktopSyncChatCarry>(std::shared_ptr<std::atomic<bool>>, Fn<void(DesktopSyncCarryProgress)>)> carry;
	Fn<void(DesktopSyncCarryCommand, Fn<void(DesktopSyncCarryReply)>)> send;
	Fn<void(const DesktopSyncChatConsentSelection &, std::shared_ptr<std::atomic<bool>>, Fn<void(bool)>)> pause;
	Fn<void(DesktopSyncCarryProgress)> status;
	Fn<void()> cancel;
	// The Cleanup switch, captured when the owner admitted this Follow; without it the run plans no cleanup.
	bool cleanup = false;
};

class DesktopSyncChatConsentFlow final {
public:
	DesktopSyncChatConsentFlow(DesktopSyncChatConsentSelection selection, DesktopSyncChatConsentPort port);
	~DesktopSyncChatConsentFlow();
	void start();
	void retry();
	void close();
	// Store worker: runs `work` with this flow's own engine, or with nullptr when the flow is
	// closed, no longer live or has no engine yet. The native transport renews each effect
	// against this engine through it.
	void withEngine(Fn<void(DesktopSyncChatCarry *)> work);

private:
	class State;
	std::shared_ptr<State> _state;

};

}
