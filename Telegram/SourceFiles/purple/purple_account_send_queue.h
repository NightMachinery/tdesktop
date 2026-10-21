/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_account_config.h"
#include "purple/purple_state.h"

#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace Purple {

template <typename Owner>
class AccountSendQueue final {
public:
	struct Operation {
		AccountOperationTarget target;
		Owner owner;
		std::function<bool(const Owner &)> ownerCurrent;
	};
	struct Receipt {
		QString key;
		std::shared_ptr<const int> token;
	};

	void observe(DesktopAccountConfig &config, const AccountKey &key,
			Owner owner, std::function<bool(const Owner &)> ownerCurrent) {
		const auto name = AccountKeyText(key.environment, key.userId);
		if (!name) {
			return;
		}
		const auto previousOwner = _observers.find(*name);
		if (previousOwner != _observers.end()
			&& !previousOwner->second.ownerCurrent(previousOwner->second.owner)) {
			forget(config, key);
		}
		if (!ownerCurrent || !ownerCurrent(owner)) {
			forget(config, key);
			return;
		}
		_observers[*name] = { owner, ownerCurrent };
		const auto qualified = config.observeAccountSave(key);
		const auto old = _pending.find(*name);
		if (old != _pending.end() && !valid(config, old->second)) {
			_pending.erase(old);
		}
		if (qualified) {
			_pending[*name] = { *qualified, std::move(owner), std::move(ownerCurrent) };
		}
	}

	void forget(DesktopAccountConfig &config, const AccountKey &key) {
		if (const auto name = AccountKeyText(key.environment, key.userId)) {
			_pending.erase(*name);
			_observers.erase(*name);
		}
		config.forgetAccountSave(key);
	}

	[[nodiscard]] bool empty() const {
		return _pending.empty();
	}

	[[nodiscard]] std::vector<Operation> takeReady(DesktopAccountConfig &config) {
		auto result = std::vector<Operation>();
		for (auto &[name, operation] : std::exchange(_pending, {})) {
			if (valid(config, operation)) {
				result.push_back(std::move(operation));
			}
		}
		return result;
	}

	[[nodiscard]] std::optional<Receipt> beginSend(
			DesktopAccountConfig &config, const Operation &operation) {
		if (!valid(config, operation)) {
			return std::nullopt;
		}
		const auto account = AccountKeyText(operation.target.file.account.environment,
			operation.target.file.account.userId);
		if (!account) {
			return std::nullopt;
		}
		const auto key = *account + u"/"_q + SettingsFingerprint(operation.target.file.input.bytes);
		const auto old = _inFlight.find(key);
		if (old != _inFlight.end() && old->second.operation.ownerCurrent(old->second.operation.owner)) {
			return std::nullopt;
		}
		const auto receipt = Receipt{ key, std::make_shared<const int>(0) };
		_inFlight[key] = { operation, receipt.token };
		return receipt;
	}

	void finishSend(const Receipt &receipt) {
		const auto current = _inFlight.find(receipt.key);
		if (current != _inFlight.end() && current->second.token == receipt.token) {
			_inFlight.erase(current);
		}
	}

private:
	[[nodiscard]] static bool valid(DesktopAccountConfig &config, const Operation &operation) {
		return operation.ownerCurrent && operation.ownerCurrent(operation.owner)
			&& config.validateOperation(operation.target)
			&& config.shouldAutoSend(operation.target);
	}

	struct Observer {
		Owner owner;
		std::function<bool(const Owner &)> ownerCurrent;
	};
	struct InFlight {
		Operation operation;
		std::shared_ptr<const int> token;
	};
	std::map<QString, Observer> _observers;
	std::map<QString, Operation> _pending;
	std::map<QString, InFlight> _inFlight;

};

}
