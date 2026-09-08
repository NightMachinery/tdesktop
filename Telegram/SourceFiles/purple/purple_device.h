/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_engine.h"

namespace Purple {

// What this install says it is, so a [[schedule.rulesets]] block can name it.
// The whole point of rulesets is that one settings.toml travels between a
// phone and a laptop unchanged, so the file describes devices and each device
// describes itself - nothing here is ever read from the file.
//
// Computed once and cached: the machine identifier behind it does not change
// while the app is up, and the platform calls that produce it are not free.
[[nodiscard]] const DeviceIdentity &ThisDevice();

// What to call a device id on a screen: the name [devices] gives it, or the id
// itself when the file does not name it. Named ...Text because Purple already
// has a DeviceLabel - the struct one of these lines parses into.
[[nodiscard]] QString DeviceLabelText(const QString &id);

} // namespace Purple
