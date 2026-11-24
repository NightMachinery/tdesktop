#include "purple/purple_keys_zen.h"

#include <iostream>
#include <string>

namespace {

using namespace Purple::ModalZen;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string &name) {
	++gChecks;
	if (!ok) {
		++gFailures;
		std::cerr << "FAIL: " << name << "\n";
	}
}

} // namespace

int main() {
	// 1. Normal mode never force-shows a widget hidden by someone else (e.g. section animation).
	{
		ZenWidgetHideState st;
		Check(st.sync(true, false, false) == ZenWidgetHideState::Action::None,
			"non-zen hidden widget stays untouched");
		Check(st.sync(true, true, false) == ZenWidgetHideState::Action::None,
			"non-zen visible widget stays untouched");
	}

	// 2. Zen hides a visible third column, then restores it on exit.
	{
		ZenWidgetHideState st;
		Check(st.sync(true, true, true) == ZenWidgetHideState::Action::Hide,
			"zen hides visible widget");
		Check(st.hiddenByZen, "tracks hiddenByZen=true");
		Check(st.sync(true, false, true) == ZenWidgetHideState::Action::None,
			"repeated layout in zen does not re-hide");
		Check(st.sync(true, false, false) == ZenWidgetHideState::Action::Show,
			"leaving zen restores widget zen hid");
		Check(!st.hiddenByZen, "clears hiddenByZen after restore");
		Check(st.sync(true, false, false) == ZenWidgetHideState::Action::None,
			"later hidden widget outside zen is not force-shown");
	}

	// 3. Widget already hidden before zen is NOT force-shown after zen exits.
	{
		ZenWidgetHideState st;
		Check(st.sync(true, false, true) == ZenWidgetHideState::Action::None,
			"zen does not claim already-hidden widget");
		Check(!st.hiddenByZen, "hiddenByZen stays false");
		Check(st.sync(true, false, false) == ZenWidgetHideState::Action::None,
			"exiting zen does not force-show pre-hidden widget");
	}

	// 4. Widget destroyed during zen clears flag; newly created visible widget during zen gets hidden and restored.
	{
		ZenWidgetHideState st;
		Check(st.sync(true, true, true) == ZenWidgetHideState::Action::Hide, "hide initial");
		st.onWidgetDestroyed();
		Check(!st.hiddenByZen, "destroy clears hiddenByZen");
		Check(st.sync(false, false, false) == ZenWidgetHideState::Action::None,
			"missing widget does nothing on exit");

		// New third section created while zen is active:
		Check(st.sync(true, true, true) == ZenWidgetHideState::Action::Hide,
			"hides widget created during zen");
		Check(st.sync(true, false, false) == ZenWidgetHideState::Action::Show,
			"restores widget created during zen on exit");
	}

	// 5. Window-state restore plans (windowed, maximized, already-fullscreen, already exited fullscreen).
	{
		const WindowStateSnapshot normal{ false, false, QRect(40, 60, 1100, 800) };
		Check(PlanWindowRestore(normal, true) == RestoreWindowAction::ShowNormalAndRestoreGeometry,
			"windowed -> fullscreen -> restore normal + geometry");
		Check(PlanWindowRestore(normal, false) == RestoreWindowAction::None,
			"windowed user already exited OS fullscreen manually -> no redundant showNormal");

		const WindowStateSnapshot maxed{ false, true, QRect(40, 60, 1100, 800) };
		Check(PlanWindowRestore(maxed, true) == RestoreWindowAction::ShowMaximized,
			"maximized -> fullscreen -> restore maximized");

		const WindowStateSnapshot alreadyFs{ true, false, QRect(40, 60, 1100, 800) };
		Check(PlanWindowRestore(alreadyFs, true) == RestoreWindowAction::None,
			"already fullscreen before zen -> stays fullscreen on exit");
	}

	std::cout << gChecks << " checks, " << gFailures << " failures\n";
	return gFailures ? 1 : 0;
}
