/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_notification_privacy.h"

#include "data/data_peer.h"
#include "purple/purple_config.h"
#include "purple/purple_gate.h"

namespace Purple {

bool WithPreviewAlways(not_null<PeerData*> peer) {
	return PreviewAlways(ActiveSettings(), IdOf(peer), KindOf(peer));
}

} // namespace Purple
