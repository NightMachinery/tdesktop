#include "purple/purple_wait_gate.h"

#include "purple/purple_engine.h"
#include "purple/purple_config_file.h"

#include <algorithm>
#include <utility>

#if defined Q_OS_WIN
#include <windows.h>
#else // Q_OS_WIN
#include <time.h>
#endif // Q_OS_WIN

namespace Purple {
namespace {

constexpr auto kInactiveGrace = int64_t(2000);

}

int64_t WaitClockNow() {
#if defined Q_OS_WIN
	// Counts sleep and hibernation, unlike QueryUnbiasedInterruptTime.
	return int64_t(GetTickCount64());
#elif defined Q_OS_DARWIN
	// CLOCK_MONOTONIC keeps counting while the Mac sleeps; CLOCK_UPTIME_RAW
	// and mach_absolute_time(), behind crl::now(), do not.
	return int64_t(clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000000);
#else
	auto value = timespec();
	clock_gettime(CLOCK_BOOTTIME, &value);
	return int64_t(value.tv_sec) * 1000 + value.tv_nsec / 1000000;
#endif
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

bool PeekSettingsInputUsable(const SettingsFileInput &input) {
	if (input.state != SettingsFileState::Readable) return false;
	const auto text = QString::fromUtf8(input.bytes);
	const auto parsed = ParseSettings(text, QString());
	return text.toUtf8() == input.bytes && parsed.ok()
		&& parsed.settings.version <= kHighestSettingsVersion;
}

bool WaitNeedsSettingsEdit(WaitAction action, bool writesSettings) {
	return action != WaitAction::StartPeek && action != WaitAction::ExtendPeek
		&& action != WaitAction::EndPeek && action != WaitAction::ChangeAccountMode
		&& action != WaitAction::ChangeGlobalAccount && action != WaitAction::ChangePeekFocus
		&& (action != WaitAction::ChangePeekOption || writesSettings);
}

PeekRequestRefusal CheckPeekRequest(
		WaitAction action,
		bool filtering,
		const State &state,
		int64_t now,
		int seconds) {
	if (!filtering) return PeekRequestRefusal::WorkModeOff;
	if (action != WaitAction::ExtendPeek) return PeekRequestRefusal::None;
	if (!PeekLive(state, now)) return PeekRequestRefusal::Ended;
	if (!state.peekDeadlineUnix) return PeekRequestRefusal::Untimed;
	if (seconds <= 0) return PeekRequestRefusal::InvalidExtension;
	auto preview = state;
	return ExtendPeek(preview, now, seconds, kPeekExtendCapSeconds)
		? PeekRequestRefusal::None : PeekRequestRefusal::Limit;
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
	_operation = std::move(operation);
	if (!delay) {
		return complete();
	}
	_phase = WaitPhase::Countdown;
	_until = now + int64_t(delay) * 1000;
	return tick(now);
}

WaitResult WaitGate::tick(int64_t now) {
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
	if (_phase == WaitPhase::Countdown) {
		if (now < _until) {
			return { WaitOutcome::Pending };
		}
		// Counted from the end of the countdown, not from the tick that saw
		// it, so a deadline slept through does not buy a fresh ready step.
		const auto timeout = operation.readyTimeoutSeconds > 0
			? std::max(operation.readyTimeoutSeconds, kPeekReadyTimeoutMinimumSeconds)
			: kPeekReadyTimeoutDefaultSeconds;
		_phase = WaitPhase::Ready;
		_readyUntil = _until + int64_t(timeout) * 1000;
		guardInput(now);
	}
	if (now >= _readyUntil) {
		cancel();
		return { WaitOutcome::Expired };
	}
	return { WaitOutcome::Pending };
}

WaitResult WaitGate::confirm(int64_t now) {
	if (!_operation) {
		return {};
	}
	if (_phase != WaitPhase::Ready || now < _guardUntil) {
		return { WaitOutcome::Pending };
	}
	if (now >= _readyUntil) {
		cancel();
		return { WaitOutcome::Expired };
	}
	return complete();
}

WaitResult WaitGate::complete() {
	const auto operation = *_operation;
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

void WaitGate::guardInput(int64_t now) {
	if (_phase == WaitPhase::Ready) {
		_guardUntil = now + kWaitReadyGuard;
	}
}

void WaitGate::cancel() {
	++_generation;
	_operation.reset();
	_phase = WaitPhase::Idle;
	_until = _readyUntil = _guardUntil = 0;
}

bool WaitGate::pending() const {
	return _operation.has_value();
}

WaitPhase WaitGate::phase() const {
	return _phase;
}

bool WaitGate::inputGuarded(int64_t now) const {
	return _phase == WaitPhase::Ready && now < _guardUntil;
}

int WaitGate::remaining(int64_t now) const {
	return (_phase == WaitPhase::Countdown)
		? int((std::max(_until - now, int64_t(0)) + 999) / 1000)
		: 0;
}

std::optional<WaitAction> WaitGate::action() const {
	return _operation ? std::make_optional(_operation->action) : std::nullopt;
}

void WaitPresence::reset() {
	_phase = Phase::Present;
	_since = 0;
}

bool WaitPresence::update(bool foreground, bool locked, int64_t now) {
	if (_phase == Phase::Returning
		&& (now - _since >= kWaitAwaySettle || locked)) {
		return true;
	}
	// Checked before the lock: a lock that comes later than this follows a
	// switch to another app, and the switch already cancelled the wait.
	if (_phase == Phase::Away && now - _since >= kWaitLockAfterAway) {
		return true;
	} else if (locked) {
		_phase = Phase::Locked;
		return false;
	} else if (_phase == Phase::Locked) {
		_phase = Phase::Returning;
		_since = now;
	}
	switch (_phase) {
	case Phase::Returning:
		if (foreground) {
			_phase = Phase::Present;
			return false;
		}
		return (now - _since >= kWaitAwaySettle);
	case Phase::Away:
		// Back without a lock in between: it was a switch to another app.
		return foreground;
	case Phase::Present:
		if (!foreground) {
			_phase = Phase::Away;
			_since = now;
		}
		return false;
	case Phase::Locked:
		break;
	}
	return false;
}

bool WaitPresence::locked() const {
	return _phase == Phase::Locked;
}

WaitCoverRects LayoutWaitCover(const WaitCoverSizes &sizes) {
	const auto centred = [&](int top, int width, int height) {
		return QRect((sizes.width - width) / 2, top, width, height);
	};
	auto result = WaitCoverRects();
	const auto total = sizes.countdownTitle + sizes.number
		+ sizes.abort.height() + 2 * sizes.gap;
	auto top = std::max((sizes.height - total) / 2, 0);
	result.countdownTitle = centred(top, sizes.width, sizes.countdownTitle);
	top += sizes.countdownTitle + sizes.gap;
	result.number = centred(top, sizes.width, sizes.number);
	top += sizes.number + sizes.gap;
	result.abort = centred(top, sizes.abort.width(), sizes.abort.height());

	// Upwards from the countdown's Abort. A title that no longer fits starts
	// above the top edge rather than pushing the buttons down.
	result.readyAbort = centred(
		result.abort.top()
			+ (sizes.abort.height() - sizes.readyAbort.height()) / 2,
		sizes.readyAbort.width(),
		sizes.readyAbort.height());
	auto bottom = std::min(result.readyAbort.top(), result.abort.top())
		- sizes.gap;
	result.confirm = centred(
		bottom - sizes.confirm.height(),
		sizes.confirm.width(),
		sizes.confirm.height());
	bottom = result.confirm.top() - sizes.gap;
	result.hint = centred(bottom - sizes.hint, sizes.width, sizes.hint);
	bottom = result.hint.top() - sizes.gap;
	result.readyTitle = centred(bottom - sizes.readyTitle, sizes.width, sizes.readyTitle);
	return result;
}

WaitKeyVerdict ClassifyWaitKey(
		WaitPhase phase,
		bool guarded,
		bool enter,
		bool autoRepeat) {
	if (phase != WaitPhase::Ready) {
		return WaitKeyVerdict::Abort;
	} else if (guarded || autoRepeat) {
		return WaitKeyVerdict::Ignore;
	}
	return enter ? WaitKeyVerdict::Confirm : WaitKeyVerdict::Abort;
}

} // namespace Purple
