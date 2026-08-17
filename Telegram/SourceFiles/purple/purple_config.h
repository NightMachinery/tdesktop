/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Purple {

// Fork-local configuration, kept as hand-editable TOML outside Telegram's own
// encrypted settings blob. See docs/purple/config.md.
[[nodiscard]] QString ConfigDirectory();
[[nodiscard]] QString SettingsFilePath();

// The Premium features Telegram Desktop gates on the client alone. Features
// the server enforces are deliberately not covered - see docs/purple/premium.md.
[[nodiscard]] bool LocalPremium();
[[nodiscard]] rpl::producer<bool> LocalPremiumValue();
void SetLocalPremium(bool value);

} // namespace Purple
