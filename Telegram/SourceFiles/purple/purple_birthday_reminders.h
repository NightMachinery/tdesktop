/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class UserData;

namespace Main {
class Session;
} // namespace Main

namespace Settings::Builder {
class SectionBuilder;
} // namespace Settings::Builder

namespace Window {
class SessionController;
} // namespace Window

namespace Purple {

void AddBirthdayReminderSettings(::Settings::Builder::SectionBuilder &builder);

[[nodiscard]] rpl::producer<> BirthdayReminderChanges(
	not_null<Main::Session*> session);
[[nodiscard]] bool BirthdayReminderFor(not_null<UserData*> user);
[[nodiscard]] rpl::producer<bool> BirthdayReminderValue(not_null<UserData*> user);
[[nodiscard]] bool BirthdayRemindersAvailable(not_null<Main::Session*> session);
[[nodiscard]] std::vector<UserId> BirthdayReminderUsers(
	not_null<Main::Session*> session);
void HideBirthdayRemindersToday(not_null<Main::Session*> session);
void ShowBirthdayReminders(not_null<Window::SessionController*> controller);

} // namespace Purple
