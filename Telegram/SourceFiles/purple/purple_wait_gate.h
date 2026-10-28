#pragma once

#include <QtCore/QRect>
#include <QtCore/QString>

#include <cstdint>
#include <functional>
#include <optional>

namespace Purple {

struct Settings;
struct State;
struct SettingsFileInput;

[[nodiscard]] int PeekWaitDelaySeconds(const Settings &settings);

enum class WaitAction {
	StartPeek,
	ExtendPeek,
	EndPeek,
	ChangePeekOption,
	ChangePreset,
	ChangeAccountMode,
	ChangeGlobalAccount,
	ChangePeekFocus,
};

struct WaitOrigin {
	bool accountValid = false;
	bool displayedAccountSame = false;
	bool foregroundUnlocked = false;
};

[[nodiscard]] bool WorkModeWait(WaitAction action);
[[nodiscard]] bool PeekSettingsInputUsable(const SettingsFileInput &input);
[[nodiscard]] bool WaitNeedsSettingsEdit(WaitAction action, bool writesSettings);

enum class PeekRequestRefusal { None, WorkModeOff, Ended, Untimed, Limit, InvalidExtension };

[[nodiscard]] PeekRequestRefusal CheckPeekRequest(
	WaitAction action,
	bool filtering,
	const State &state,
	int64_t now,
	int seconds);
[[nodiscard]] bool WaitOriginValid(WaitAction action, WaitOrigin origin);

// Expired is the ready step's own timeout: quiet, like Abort.
enum class WaitOutcome { NoOp, Pending, Applied, Refused, Expired };

struct WaitResult {
	WaitOutcome outcome = WaitOutcome::NoOp;
	QString error;
};

[[nodiscard]] bool WaitRequired(
	WaitAction action,
	bool same,
	bool fromBypass,
	bool toBypass,
	bool onlyTightening = false);

void FinishNoOpWait(const WaitResult &result, const std::function<void()> &finished);

struct WaitOperation {
	WaitAction action = WaitAction::StartPeek;
	int delaySeconds = 0;
	// Zero means kPeekReadyTimeoutDefaultSeconds; never below its minimum.
	int readyTimeoutSeconds = 0;
	bool same = false;
	bool onlyTightening = false;
	bool fromBypass = false;
	bool toBypass = false;
	std::function<QString()> validate;
	std::function<bool()> originCurrent;
	std::function<QString(const std::function<bool()>&)> apply;
};

inline constexpr auto kPeekFocusLossDefault = false;

class PeekFocusGate final {
public:
	[[nodiscard]] bool update(
		bool enabled,
		const State &state,
		bool applicationActive,
		bool locked,
		int64_t now);
	[[nodiscard]] std::optional<int64_t> remaining(int64_t now) const;

private:
	std::optional<int64_t> _inactiveSince;
	std::optional<int64_t> _peekDeadline;

};

// Milliseconds on a monotonic clock that keeps counting while the machine
// sleeps, so a deadline slept through is over on wake. crl::now() stops in
// sleep on macOS and Linux until something adjusts it.
[[nodiscard]] int64_t WaitClockNow();

// Ignore every key and the action button this long after the ready step
// shows, so a held or late key from the countdown cannot confirm.
inline constexpr auto kWaitReadyGuard = int64_t(500);

// How long after the host window loses activation a lock signal may still
// arrive and make it a lock rather than a switch to another app. Short, so
// that switching away and locking a moment later stays a switch.
inline constexpr auto kWaitLockAfterAway = int64_t(300);

// How long after an unlock the host window has to be in the foreground again.
inline constexpr auto kWaitAwaySettle = int64_t(2000);

enum class WaitPhase { Idle, Countdown, Ready };

// A positive delay counts down, then waits in a ready step until confirm()
// applies it, Abort cancels it, or the ready timeout expires it. Each tick
// re-validates the context; foreground is checked only at confirm(), because
// presence while waiting is WaitPresence's job.
class WaitGate final {
public:
	WaitResult request(WaitOperation operation, int64_t now);
	WaitResult tick(int64_t now);
	WaitResult confirm(int64_t now);
	void guardInput(int64_t now);
	void cancel();
	[[nodiscard]] bool pending() const;
	[[nodiscard]] WaitPhase phase() const;
	[[nodiscard]] bool inputGuarded(int64_t now) const;
	[[nodiscard]] int remaining(int64_t now) const;
	[[nodiscard]] std::optional<WaitAction> action() const;

private:
	WaitResult complete();

	std::optional<WaitOperation> _operation;
	uint64_t _generation = 0;
	WaitPhase _phase = WaitPhase::Idle;
	int64_t _until = 0;
	int64_t _readyUntil = 0;
	int64_t _guardUntil = 0;

};

// Whether the person is at the host window, away in another app, or behind
// a lock. The order of "inactive" and "locked" is not fixed on any platform,
// so leaving the window leaves kWaitLockAfterAway for a lock to arrive. A
// return, or that time passing, without one cancels. After an unlock the
// window has kWaitAwaySettle to be in the foreground again.
class WaitPresence final {
public:
	void reset();
	// True when the wait must be cancelled.
	[[nodiscard]] bool update(bool foreground, bool locked, int64_t now);
	[[nodiscard]] bool locked() const;

private:
	enum class Phase { Present, Away, Locked, Returning };

	Phase _phase = Phase::Present;
	int64_t _since = 0;

};

// The cover's two layouts. The countdown stacks its title, number and Abort,
// centred. The ready step keeps Abort exactly where the countdown's was and
// stacks the action, hint and title above it, so a click aimed at Abort
// never lands on the action, whatever wraps.
struct WaitCoverSizes {
	int width = 0;
	int height = 0;
	int gap = 0;
	int countdownTitle = 0;
	int number = 0;
	QSize abort;
	int readyTitle = 0;
	int hint = 0;
	QSize confirm;
	QSize readyAbort;
};

struct WaitCoverRects {
	QRect countdownTitle;
	QRect number;
	QRect abort;
	QRect readyTitle;
	QRect hint;
	QRect confirm;
	QRect readyAbort;
};

[[nodiscard]] WaitCoverRects LayoutWaitCover(const WaitCoverSizes &sizes);

enum class WaitKeyVerdict { Ignore, Abort, Confirm };

// Countdown: every key aborts. Ready: nothing during the guard or on
// auto-repeat; Return and keypad Enter confirm; any other key aborts.
[[nodiscard]] WaitKeyVerdict ClassifyWaitKey(
	WaitPhase phase,
	bool guarded,
	bool enter,
	bool autoRepeat);

} // namespace Purple
