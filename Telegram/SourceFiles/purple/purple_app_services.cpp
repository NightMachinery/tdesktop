/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_app_services.h"

#include "purple/purple_focus.h"
#include "purple/purple_gate.h"
#include "purple/purple_peek.h"
#include "purple/purple_schedule.h"
#include "purple/purple_screentime_recorder.h"
#include "purple/purple_ui_witness.h"

namespace Purple {

void StartAppServices() {
	StartSchedule();
	StartFocusSync();
	StartScreenTime();
	StartUiWitness();
}

void OnScreenLockChanged(bool locked) {
	if (locked) {
		ReportLock(LockKind::Screen);
	} else {
		PeekEndedNotice();
	}
}

void OnAppPasscodeLock() {
	ReportLock(LockKind::App);
}

void OnAppPasscodeUnlock() {
	PeekEndedNotice();
}

} // namespace Purple
