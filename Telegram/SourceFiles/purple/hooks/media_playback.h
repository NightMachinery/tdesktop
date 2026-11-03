/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/qnamespace.h>

#include <memory>

class ClickHandler;

namespace Purple {

[[nodiscard]] bool RestartOnDoubleClick(
	const std::shared_ptr<ClickHandler> &active,
	Qt::MouseButton button,
	bool selecting);

} // namespace Purple
