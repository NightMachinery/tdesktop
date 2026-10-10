/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "data/data_chat_filters.h"
#include "purple/purple_folder_strip.h"

namespace Data {
class Folder;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace Purple {

[[nodiscard]] bool SavePinnedViewOrder(
	not_null<Main::Session*> session,
	FilterId filterId,
	Data::Folder *folder);

} // namespace Purple
