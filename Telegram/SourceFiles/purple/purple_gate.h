/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_engine.h"

class PeerData;

// The seam between the engine and the app. Everything below purple_engine.h is
// pure data and is tested standalone; this is the one file that knows what a
// PeerData is, so the rest of tdesktop asks its questions here rather than
// assembling engine arguments at each call site.
namespace Purple {

[[nodiscard]] ChatKind KindOf(not_null<const PeerData*> peer);
[[nodiscard]] PeerIdValue IdOf(not_null<const PeerData*> peer);

// The resolution the active preset produced, recomputed whenever settings.toml
// or state.toml changes. Normal until a preset is chosen, and Normal is a
// bypass rather than a permissive preset.
[[nodiscard]] const Resolved &ActiveResolved();
[[nodiscard]] rpl::producer<> ActiveChanges();

// False under Normal, which is the whole point: every gate in the app tests
// this first, so an unconfigured fork pays one bool load and behaves exactly
// like upstream. Call sites must not skip it - the engine answers "show
// everything" under Normal too, but only this is cheap enough for hot paths.
[[nodiscard]] bool Filtering();

// Both assume Filtering(). Under Normal they still answer correctly, but the
// caller has already paid for a peer classification it did not need.
[[nodiscard]] Visibility VisibleFor(not_null<const PeerData*> peer);
[[nodiscard]] const EffectiveList *ListFor(not_null<const PeerData*> peer);

} // namespace Purple
