#pragma once

#include "purple/purple_local_choice.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace Purple {

class DesktopSyncWriteGate final : public std::enable_shared_from_this<DesktopSyncWriteGate> {
public:
	class Lease final {
	public:
		~Lease() {
			if (_exclusiveLease) {
				if (_gate->_epoch.load() == _epoch) { _gate->_moving.store(false); }
			} else { _gate->_writers.fetch_sub(1); }
		}
		[[nodiscard]] bool current() const {
			return _gate->_epoch.load() == _epoch && (_exclusiveLease == _gate->_moving.load());
		}
		[[nodiscard]] bool quiescent() const { return _exclusiveLease && current() && _gate->_writers.load() == 0; }

	private:
		friend class DesktopSyncWriteGate;
		Lease(std::shared_ptr<DesktopSyncWriteGate> gate, bool exclusive, uint64_t epoch)
		: _gate(std::move(gate)), _exclusiveLease(exclusive), _epoch(epoch) {}
		std::shared_ptr<DesktopSyncWriteGate> _gate;
		bool _exclusiveLease = false;
		uint64_t _epoch = 0;

	};

	[[nodiscard]] std::shared_ptr<Lease> writer() {
		const auto epoch = _epoch.load();
		if (_moving.load()) { return {}; }
		_writers.fetch_add(1);
		if (_moving.load() || _epoch.load() != epoch) { _writers.fetch_sub(1); return {}; }
		return std::shared_ptr<Lease>(new Lease(shared_from_this(), false, epoch));
	}
	[[nodiscard]] std::shared_ptr<Lease> exclusive() {
		auto expected = false;
		if (!_moving.compare_exchange_strong(expected, true)) { return {}; }
		return std::shared_ptr<Lease>(new Lease(shared_from_this(), true, _epoch.fetch_add(1) + 1));
	}
	[[nodiscard]] bool postingHeld() const { return _moving.load(); }

private:
	std::atomic<bool> _moving = false;
	std::atomic<uint64_t> _epoch = 0;
	std::atomic<uint64_t> _writers = 0;

};

[[nodiscard]] inline std::shared_ptr<DesktopSyncWriteGate> DesktopSyncWriteGateFor(const AccountKey &account) {
	static auto mutex = std::mutex();
	static auto gates = std::map<QString, std::weak_ptr<DesktopSyncWriteGate>>();
	const auto key = AccountKeyText(account.environment, account.userId);
	if (!key) { return {}; }
	const auto lock = std::lock_guard(mutex);
	auto &entry = gates[*key];
	auto gate = entry.lock();
	if (!gate) { gate = std::make_shared<DesktopSyncWriteGate>(); entry = gate; }
	return gate;
}

}
