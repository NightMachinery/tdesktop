#pragma once

#include <QtCore/QString>

#include <cstdint>
#include <functional>
#include <optional>

namespace Purple {

struct Settings;
struct State;

[[nodiscard]] int PeekWaitDelaySeconds(const Settings &settings);

enum class WaitAction {
	StartPeek,
	ExtendPeek,
	EndPeek,
	ChangeDelay,
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
[[nodiscard]] bool WaitOriginValid(WaitAction action, WaitOrigin origin);

enum class WaitOutcome { NoOp, Pending, Applied, Refused };

struct WaitResult {
	WaitOutcome outcome = WaitOutcome::NoOp;
	QString error;
};

[[nodiscard]] bool WaitRequired(
	WaitAction action,
	bool same,
	bool fromBypass,
	bool toBypass);

void FinishNoOpWait(const WaitResult &result, const std::function<void()> &finished);

struct WaitOperation {
	WaitAction action = WaitAction::StartPeek;
	int delaySeconds = 0;
	bool same = false;
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

class WaitGate final {
public:
	WaitResult request(WaitOperation operation, int64_t now);
	WaitResult tick(int64_t now, bool foregroundUnlocked = true);
	void cancel();
	[[nodiscard]] bool pending() const;
	[[nodiscard]] int remaining(int64_t now) const;
	[[nodiscard]] std::optional<WaitAction> action() const;

private:
	std::optional<WaitOperation> _operation;
	uint64_t _generation = 0;
	int64_t _until = 0;

};

} // namespace Purple
