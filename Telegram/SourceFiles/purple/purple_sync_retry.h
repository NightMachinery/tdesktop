#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace Purple {

enum class SyncTransientKind { Flood, Server, Transport, Timeout };

struct SyncRequestTransient {
	SyncTransientKind kind = SyncTransientKind::Transport;
	int64_t retryAt = 0;
	int64_t delayMs = 0;
	bool shortRetry = false;
};

class SyncRetryState {
public:
	explicit SyncRetryState(int64_t nowMs) : _deadline(nowMs + 30000) {
	}
	[[nodiscard]] int64_t deadline() const { return _deadline; }
	void accountForResumeWait(int64_t started, int64_t now) {
		const auto elapsed = std::max(int64_t(0), now - started);
		_deadline += std::min(elapsed, std::numeric_limits<int64_t>::max() - _deadline);
	}
	[[nodiscard]] std::optional<SyncRequestTransient> failed(
			int code, std::string_view type, int64_t nowMs, int64_t nowUnix,
			bool timeout = false) {
		auto kind = SyncTransientKind::Transport;
		auto flood = std::optional<int64_t>();
		for (const auto prefix : { std::string_view("FLOOD_WAIT_"),
				std::string_view("FLOOD_PREMIUM_WAIT_") }) {
			if (type.substr(0, prefix.size()) != prefix) {
				continue;
			}
			auto seconds = int64_t(0);
			const auto digits = type.substr(prefix.size());
			if (digits.empty()) {
				flood = 60;
				break;
			}
			for (const auto c : digits) {
				if (c < '0' || c > '9') {
					seconds = 60;
					break;
				}
				seconds = std::min(
					(std::numeric_limits<int64_t>::max() - std::max(int64_t(0), nowMs) - 30000) / 1000,
					seconds * 10 + c - '0');
			}
			flood = seconds;
			break;
		}
		if (timeout) {
			kind = SyncTransientKind::Timeout;
		} else if (flood) {
			kind = SyncTransientKind::Flood;
		} else if (code >= 500 || type == "INTERNAL" || type == "INTERNAL_SERVER_ERROR") {
			kind = SyncTransientKind::Server;
		} else if (code >= 0) {
			return {};
		}
		const auto seconds = flood.value_or(std::min(int64_t(60), int64_t(5) << std::min(_failures, 4)));
		const auto shortRetry = flood && *flood <= 30 && !_shortUsed && nowMs < _deadline
			&& *flood * 1000 <= std::max(int64_t(0), _deadline - nowMs);
		const auto delay = (shortRetry ? seconds + 1 : std::max(int64_t(1), seconds)) * 1000;
		if (shortRetry) {
			_shortUsed = true;
			_deadline += delay;
		} else {
			_deadline = nowMs + delay + 30000;
		}
		_failures = std::min(_failures + 1, 5);
		return SyncRequestTransient{ kind, nowUnix + delay / 1000, delay, shortRetry };
	}

private:
	int64_t _deadline = 0;
	int _failures = 0;
	bool _shortUsed = false;

};

}
