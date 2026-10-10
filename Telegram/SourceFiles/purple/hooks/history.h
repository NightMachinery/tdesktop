/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "purple/purple_bypass.h"

class PeerData;

namespace Data {
class Thread;
} // namespace Data

namespace HistoryUnreadThings {
enum class Type;
} // namespace HistoryUnreadThings

namespace Purple {

[[nodiscard]] bool StartsUncounted(not_null<PeerData*> peer);
void UnreadThingEdge(
	not_null<Data::Thread*> thread,
	HistoryUnreadThings::Type type);

} // namespace Purple
