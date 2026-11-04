/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <string>
#include <vector>

namespace Purple {

void StartRoundCopies();
[[nodiscard]] std::vector<std::string> RoundCopyIds();
[[nodiscard]] std::vector<std::string> RequestRoundCopiesClose();
void FinishRoundCopiesLater();

} // namespace Purple
