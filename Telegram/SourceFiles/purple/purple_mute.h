/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "crl/crl_time.h"

class PeerData;

namespace Data {
class Thread;
}

namespace Purple {

[[nodiscard]] bool PresetMutes(
	not_null<const PeerData*> peer,
	crl::time *changesIn,
	crl::time maxCheckDelay);
[[nodiscard]] bool MutedWithoutPreset(not_null<const Data::Thread*> thread);
[[nodiscard]] bool MutedWithoutPreset(not_null<const PeerData*> peer);
[[nodiscard]] bool Silenced(not_null<const PeerData*> peer);
void RefreshMute(not_null<PeerData*> peer);
void RefreshFolderMute(not_null<PeerData*> peer);

}
