#pragma once
// For compiled suites that include test_sync_chat_carry.cpp and link the owner's sources.
#include "purple/purple_sync_chat_carry_owner.h"
#include <deque>

namespace {

// The production owner behind a switch mapping, with no account environment. An entry it admits
// stops at its held transport or mark port and settles; an entry it refuses asks for nothing.
// The default mapping is DesktopSyncChatCarrySwitchActivated, which the carry service installs.
struct SwitchEntry {
	std::deque<Fn<void()>> queue;
	std::vector<DesktopSyncChatCarryKind> settled;
	QString reason;
	int transports = 0;
	int marks = 0;
	std::unique_ptr<DesktopSyncChatCarryOwner> owner;

	explicit SwitchEntry(Fn<bool(DesktopSyncChatCarrySwitch)> activated = DesktopSyncChatCarrySwitchActivated) {
		auto host = DesktopSyncChatCarryHost();
		host.activated = std::move(activated);
		host.current = [] { return true; };
		host.worker = [this](Fn<void()> task) { queue.push_back(std::move(task)); };
		host.main = [this](Fn<void()> task) { queue.push_back(std::move(task)); };
		host.transport = [this](const DesktopSyncChatSetupSnapshot &, DesktopSyncChatCarryRenewal, Fn<bool()>) {
			++transports; return DesktopSyncCarryTransport();
		};
		host.mark = [this](const DesktopSyncChatSetupSnapshot &, Fn<bool()>) { ++marks; return DesktopSyncChatMarkPort(); };
		host.changed = [this](const DesktopSyncChatCarryState &state) { reason = state.reason; };
		host.settled = [this](DesktopSyncChatCarryKind kind) { settled.push_back(kind); };
		owner = std::make_unique<DesktopSyncChatCarryOwner>(std::move(host));
	}
	~SwitchEntry() { owner.reset(); drain(); }
	void drain() {
		for (auto i = 0; !queue.empty() && i != 100; ++i) { auto task = std::move(queue.front()); queue.pop_front(); task(); }
	}
	bool follow() {
		auto selection = DesktopSyncChatConsentSelection(); selection.offer.window = u"window-A"_q;
		const auto result = owner->follow(std::move(selection)); drain(); return result;
	}
	bool removeMark() {
		const auto result = owner->removeMark({}, u"window-A"_q, {}); drain(); return result;
	}
	bool idle() const { return transports == 0 && marks == 0 && settled.empty() && reason.isEmpty(); }
};

// Every switch's injected OFF control: the same mapping with only that switch OFF.
Fn<bool(DesktopSyncChatCarrySwitch)> AllButSwitch(DesktopSyncChatCarrySwitch off) {
	return [off](DesktopSyncChatCarrySwitch value) { return value != off; };
}

}
