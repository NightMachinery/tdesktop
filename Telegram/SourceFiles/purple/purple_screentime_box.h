/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class GenericBox;
} // namespace Ui

// Purple: the reading half of screen time. Every number on screen here comes
// out of purple/purple_screentime.h in the core, from the raw log the recorder
// appends to - nothing is stored in the shape it is drawn in, so a changed
// threshold in [screen_time] redraws the history you already have.
namespace Purple {

// Settings -> Advanced -> Purple -> Screen time.
void ScreenTimeBox(
	not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session);

// The line under the settings row: "Last week: 6 h 12 m, 41 % active, top:
// Alice", or "Off" while [screen_time] enabled_p is false. Recomputed on every
// settings reload, which is also how it turns into the digest the moment the
// feature is switched on.
[[nodiscard]] rpl::producer<QString> ScreenTimeDigestValue(
	not_null<Main::Session*> session);

} // namespace Purple
