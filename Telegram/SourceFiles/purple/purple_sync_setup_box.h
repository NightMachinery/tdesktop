/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Main { class Session; }

namespace Purple {

struct SyncConfigApplyResult;
void NoteAutomaticSyncApply(not_null<Main::Session*> session, const SyncConfigApplyResult &result);

void SyncSetupBox(not_null<Ui::GenericBox*> box);

} // namespace Purple
