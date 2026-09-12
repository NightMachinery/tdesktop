/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class InputField;
} // namespace Ui

namespace Purple {

enum class InstantReplacesType {
	Default,
	TextOnly,
};

void InstallInstantReplaces(
	not_null<Ui::InputField*> field,
	InstantReplacesType type = InstantReplacesType::Default);

} // namespace Purple
