#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace Purple::KeyRepeat {

enum class Policy {
	Never,
	Count,
	SameTarget,
	ChatNavigation,
};

struct Stroke {
	int physical = 0;
	int chord = 0;
	std::int64_t source = 0;
	std::int64_t arrived = 0;
};

struct Context {
	const void *focus = nullptr;
	const void *scope = nullptr;
	bool modal = false;

	friend bool operator==(const Context &, const Context &) = default;
};

[[nodiscard]] constexpr bool CanArm(
		Policy policy,
		bool normal,
		std::size_t chordSize,
		bool inspecting) {
	return (policy == Policy::SameTarget || policy == Policy::ChatNavigation)
		&& normal && chordSize == 1 && !inspecting;
}

template <typename Action>
class HeldMotion {
public:
	void start(Stroke press, Context target, Action action, Policy policy) {
		if (policy == Policy::Never || policy == Policy::Count) {
			cancel();
		} else {
			_held = State{ press, target, action, policy, press.arrived };
		}
	}

	[[nodiscard]] std::optional<Action> next(Stroke tick, Context target) {
		if (!_held || _held->press.physical != tick.physical) {
			return std::nullopt;
		}
		auto &state = *_held;
		if (state.press.chord != tick.chord || state.context != target) {
			cancel();
			return std::nullopt;
		}
		constexpr auto kStaleMs = 100;
		constexpr auto kMinimumIntervalMs = 8;
		const auto sourceElapsed = tick.source - state.press.source;
		const auto deliveryElapsed = tick.arrived - state.press.arrived;
		if (tick.source && state.press.source
			&& (sourceElapsed < 0 || deliveryElapsed - sourceElapsed > kStaleMs)) {
			return std::nullopt;
		}
		if (tick.arrived - state.lastDelivery < kMinimumIntervalMs) {
			return std::nullopt;
		}
		state.lastDelivery = tick.arrived;
		return state.action;
	}

	[[nodiscard]] std::optional<Action> action() const {
		return _held ? std::optional(_held->action) : std::nullopt;
	}

	void release(int physical) {
		if (_held && _held->press.physical == physical) {
			cancel();
		}
	}

	void cancel(bool keepChatNavigation = false) {
		if (!keepChatNavigation || (_held && _held->policy != Policy::ChatNavigation)) {
			_held.reset();
		}
	}

private:
	struct State {
		Stroke press;
		Context context;
		Action action;
		Policy policy;
		std::int64_t lastDelivery;
	};

	std::optional<State> _held;
};

} // namespace Purple::KeyRepeat
