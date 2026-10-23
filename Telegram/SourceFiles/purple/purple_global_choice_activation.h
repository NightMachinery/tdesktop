#pragma once

#include "purple/purple_account_config.h"

namespace Purple {

[[nodiscard]] inline bool RunGlobalAccountChoiceActivation(
		GlobalAccountChoiceResult &result,
		bool enterNormal,
		const std::function<bool()> &stillCurrent,
		const std::function<bool()> &normal,
		const std::function<bool()> &activate) {
	if (!result.changed) { return result.ok; }
	if (!result.ok || !stillCurrent || !stillCurrent()) {
		result.ok = false;
		return false;
	}
	if (enterNormal && (!normal || !normal())) {
		result.ok = false;
		return false;
	}
	if (!result.ok || !stillCurrent() || !activate || !activate() || !stillCurrent()) {
		result.ok = false;
		return false;
	}
	return true;
}

}
