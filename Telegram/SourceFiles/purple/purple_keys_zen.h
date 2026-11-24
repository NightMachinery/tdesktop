#pragma once

#include <QtCore/QRect>

namespace Purple::ModalZen {

// Tracks visibility of a single auxiliary widget (e.g. third column or shadow)
// so Zen mode hides only visible widgets and restores only what Zen itself hid,
// never force-showing widgets hidden by section animations or one-column transitions.
struct ZenWidgetHideState {
	bool hiddenByZen = false;

	enum class Action {
		None,
		Hide,
		Show,
	};

	[[nodiscard]] Action sync(
			bool widgetExists,
			bool widgetCurrentlyVisible,
			bool zenHidesColumns) {
		if (!widgetExists) {
			hiddenByZen = false;
			return Action::None;
		}
		if (zenHidesColumns) {
			if (widgetCurrentlyVisible) {
				hiddenByZen = true;
				return Action::Hide;
			}
			return Action::None;
		}
		if (hiddenByZen) {
			hiddenByZen = false;
			return Action::Show;
		}
		return Action::None;
	}

	void onWidgetDestroyed() {
		hiddenByZen = false;
	}
};

struct WindowStateSnapshot {
	bool wasFullScreen = false;
	bool wasMaximized = false;
	QRect normalGeometry;
};

enum class RestoreWindowAction {
	None,
	ShowMaximized,
	ShowNormalAndRestoreGeometry,
};

[[nodiscard]] inline RestoreWindowAction PlanWindowRestore(
		const WindowStateSnapshot &saved,
		bool currentlyFullScreen) {
	if (saved.wasFullScreen || !currentlyFullScreen) {
		return RestoreWindowAction::None;
	}
	if (saved.wasMaximized) {
		return RestoreWindowAction::ShowMaximized;
	}
	return RestoreWindowAction::ShowNormalAndRestoreGeometry;
}

} // namespace Purple::ModalZen
