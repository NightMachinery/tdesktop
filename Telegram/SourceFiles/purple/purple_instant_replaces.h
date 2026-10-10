/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

class QString;

namespace Ui {
class InputField;
} // namespace Ui

namespace Purple {

[[nodiscard]] bool ReplaceDashes();
void SetReplaceDashes(bool value);
[[nodiscard]] bool InstantReplaceAllowed(const QString &what);
void TextOnlyFollowToggles(not_null<Ui::InputField*> field);

} // namespace Purple
