/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_app_services.h"

#include "purple/purple_sync_chat_service.h"

#include "purple/purple_focus.h"
#include "purple/purple_emoji_export.h"
#include "purple/purple_archive_service.h"
#include "purple/purple_config.h"
#include "purple/purple_sync_auto_service.h"
#include "purple/purple_gate.h"
#include "purple/purple_peek.h"
#include "purple/purple_peek_controls.h"
#include "purple/purple_schedule.h"
#include "purple/purple_screentime_recorder.h"
#include "purple/purple_ui_witness.h"

namespace Purple {

void StartAppServices() {
	StartDesktopAccountRouting();
	StartSyncChatDiscovery();
	StartAutomaticSettingsSync();
	StartArchiveServices();
	StartSchedule();
	StartFocusSync();
	StartScreenTime();
	StartUiWitness();
	StartEmojiExport();
}

void OnScreenLockChanged(bool locked) {
	if (locked) {
		ReportLock(LockKind::Screen);
		EndPeekOnApplicationLock();
	} else {
		PeekEndedNotice();
		TriggerAutomaticSettingsSync(SyncAutoEvent::Unlock);
	}
}

void OnAppPasscodeLock() {
	ReportLock(LockKind::App);
	EndPeekOnApplicationLock();
}

void OnAppPasscodeUnlock() {
	PeekEndedNotice();
	TriggerAutomaticSettingsSync(SyncAutoEvent::Unlock);
}

} // namespace Purple
