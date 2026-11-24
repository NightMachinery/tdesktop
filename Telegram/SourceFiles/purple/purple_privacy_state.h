#pragma once

#include <QtCore/Qt>

namespace Purple::PrivacyState {

enum class TopLevelAction {
	Ignore,
	HideEphemeral,
	HideAndRecordRestore,
};

[[nodiscard]] inline TopLevelAction ClassifyTopLevelSurface(
		bool isWindow,
		bool isMainWindow,
		Qt::WindowType windowType) {
	if (!isWindow || isMainWindow) {
		return TopLevelAction::Ignore;
	}
	if (windowType == Qt::Popup
		|| windowType == Qt::ToolTip
		|| windowType == Qt::SubWindow) {
		return TopLevelAction::HideEphemeral;
	}
	return TopLevelAction::HideAndRecordRestore;
}

struct SurfaceTracker {
	bool privacyActive = false;
	bool passcodeLocked = false;
	bool deferRestoreUntilUnlock = false;

	[[nodiscard]] bool shouldSuppressNewSurfaces() const {
		return privacyActive;
	}

	[[nodiscard]] bool shouldKeepRecordedHidden() const {
		return privacyActive || (deferRestoreUntilUnlock && passcodeLocked);
	}

	[[nodiscard]] bool needsAppFilter() const {
		return privacyActive || (deferRestoreUntilUnlock && passcodeLocked);
	}

	void onActivate(bool currentlyLocked) {
		privacyActive = true;
		passcodeLocked = currentlyLocked;
		if (currentlyLocked) {
			deferRestoreUntilUnlock = true;
		}
	}

	// Returns true when hidden surfaces should be restored immediately;
	// returns false when restore must wait until passcode unlock.
	[[nodiscard]] bool onDismiss(bool currentlyLocked, bool hasHiddenSurfaces) {
		privacyActive = false;
		passcodeLocked = currentlyLocked;
		if (currentlyLocked) {
			deferRestoreUntilUnlock = hasHiddenSurfaces;
			return false;
		}
		deferRestoreUntilUnlock = false;
		return true;
	}

	// Returns true when deferred hidden surfaces should be restored now.
	[[nodiscard]] bool onPasscodeLockChanged(bool locked, bool hasHiddenSurfaces) {
		passcodeLocked = locked;
		if (locked) {
			if (privacyActive || hasHiddenSurfaces) {
				deferRestoreUntilUnlock = true;
			}
			return false;
		}
		if (privacyActive) {
			// App unlocked while privacy screen is still covering windows:
			// keep hidden until privacy screen itself is dismissed.
			deferRestoreUntilUnlock = false;
			return false;
		}
		if (deferRestoreUntilUnlock) {
			deferRestoreUntilUnlock = false;
			return true;
		}
		return false;
	}
};

} // namespace Purple::PrivacyState
