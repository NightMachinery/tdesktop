#include "purple/purple_wait_gate.h"

#include "purple/purple_engine.h"

#include <algorithm>
#include <utility>

namespace Purple {
namespace {

constexpr auto kInactiveGrace = int64_t(2000);

}

int PeekWaitDelaySeconds(const Settings &settings) {
	return settings.peek.startDelaySeconds.value_or(kPeekStartDelayMobileDefaultSeconds);
}

void FinishNoOpWait(
		const WaitResult &result,
		const std::function<void()> &finished) {
	if (result.outcome == WaitOutcome::NoOp && finished) {
		finished();
	}
}

bool WorkModeWait(WaitAction action) {
	return action == WaitAction::ChangePreset
		|| action == WaitAction::ChangeAccountMode
		|| action == WaitAction::ChangeGlobalAccount;
}

bool WaitOriginValid(WaitAction action, WaitOrigin origin) {
	return origin.accountValid && origin.foregroundUnlocked
		&& (WorkModeWait(action) || origin.displayedAccountSame);
}

bool WaitRequired(
		WaitAction action,
		bool same,
		bool fromBypass,
		bool toBypass,
		bool onlyTightening) {
	if (same) {
		return false;
	}
	switch (action) {
	case WaitAction::ChangePeekOption:
		return !onlyTightening;
	case WaitAction::EndPeek:
		return false;
	case WaitAction::ChangePreset:
	case WaitAction::ChangeAccountMode:
		return !fromBypass || toBypass;
	case WaitAction::ChangeGlobalAccount:
	case WaitAction::ChangePeekFocus:
		return toBypass;
	default:
		return true;
	}
}

bool PeekFocusGate::update(
		bool enabled,
		const State &state,
		bool applicationActive,
		bool locked,
		int64_t now) {
	if (!enabled || !state.peekActive || applicationActive || locked) {
		_inactiveSince.reset();
		_peekDeadline.reset();
		return enabled && state.peekActive && locked;
	}
	if (!_inactiveSince || _peekDeadline != state.peekDeadlineUnix) {
		_inactiveSince = now;
		_peekDeadline = state.peekDeadlineUnix;
	}
	if (now - *_inactiveSince >= kInactiveGrace) {
		_inactiveSince.reset();
		_peekDeadline.reset();
		return true;
	}
	return false;
}

std::optional<int64_t> PeekFocusGate::remaining(int64_t now) const {
	if (!_inactiveSince) {
		return std::nullopt;
	}
	return std::max(int64_t(0), kInactiveGrace - (now - *_inactiveSince));
}

WaitResult WaitGate::request(WaitOperation operation, int64_t now) {
	cancel();
	const auto generation = _generation;
	const auto error = operation.validate ? operation.validate() : QString();
	if (generation != _generation || !error.isEmpty() || !operation.validate || !operation.apply) {
		return { WaitOutcome::Refused, error };
	}
	if (operation.same) {
		return {};
	}
	const auto delay = WaitRequired(
		operation.action,
		false,
		operation.fromBypass,
		operation.toBypass,
		operation.onlyTightening)
		? std::max(operation.delaySeconds, 0) : 0;
	_until = now + int64_t(delay) * 1000;
	_operation = std::move(operation);
	return tick(now);
}

WaitResult WaitGate::tick(int64_t now, bool foregroundUnlocked) {
	if (_operation && !foregroundUnlocked) {
		cancel();
		return {};
	}
	if (!_operation) {
		return {};
	}
	const auto generation = _generation;
	const auto operation = *_operation;
	const auto error = operation.validate();
	if (generation != _generation) {
		return { WaitOutcome::Refused, error };
	}
	if (!error.isEmpty()) {
		cancel();
		return { WaitOutcome::Refused, error };
	}
	if (now < _until) {
		return { WaitOutcome::Pending };
	}
	cancel();
	const auto completion = _generation;
	const auto current = [=, this] {
		if (_generation != completion) {
			return false;
		}
		const auto valid = !operation.originCurrent || operation.originCurrent();
		return valid && _generation == completion;
	};
	const auto finalError = operation.validate();
	if (!finalError.isEmpty() || !current()) {
		return { WaitOutcome::Refused, finalError };
	}
	const auto failed = operation.apply(current);
	return { failed.isEmpty() ? WaitOutcome::Applied : WaitOutcome::Refused, failed };
}

void WaitGate::cancel() {
	++_generation;
	_operation.reset();
	_until = 0;
}

bool WaitGate::pending() const {
	return _operation.has_value();
}

int WaitGate::remaining(int64_t now) const {
	return pending() ? int((std::max(_until - now, int64_t(0)) + 999) / 1000) : 0;
}

std::optional<WaitAction> WaitGate::action() const {
	return _operation ? std::make_optional(_operation->action) : std::nullopt;
}

} // namespace Purple
