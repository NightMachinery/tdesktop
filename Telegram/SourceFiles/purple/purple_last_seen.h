/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_settings.h" // LastSeenReason

class UserData;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// "last seen recently" with nothing after it is the fork withholding something
// it knows: the server said WHY the time is coarse, and the one case the user
// can do anything about is the one where their own privacy caused it. See
// docs/purple/work_mode.md.
namespace Purple {

// A status line as the fork writes it, in the pieces a caller has to have
// separately: `base' is what upstream would have written - or the remembered
// read that replaces it outright - `tail' is what the fork appends after the
// middle dot, and `text' is the two joined the way they are shown. `link' is
// the tail again, and only when tapping it opens the peek sheet: a caller
// that cannot make a link - a peer list row paints one elided string - shows
// `text' and stops there, one that can hit-tests or wraps `link'.
//
// Every field is already gated: `link' is empty, and `tappable' false, unless
// there is a sheet worth opening, so no call site repeats the rules. The
// decision behind all of it is `Purple::LastSeenNoteNow' in the core; this is
// only the words for it, which is why the two names differ.
struct LastSeenText {
	QString text;
	QString base;
	QString tail;
	QString link;
	bool tappable = false;
};

// Why this person's last seen is coarse, or None when it is not coarse, when
// the answer is "a long time ago" (which the server does not explain, and the
// fork will not guess at), or when the peer is one nobody has a last seen for.
[[nodiscard]] LastSeenReason ReasonForUser(not_null<UserData*> user);

// `full' picks Data::OnlineTextFull over Data::OnlineText, the way the profile
// and the chat header already differ. `narrow' asks for the short mark instead
// of the words, for a line with no room for a sentence.
[[nodiscard]] LastSeenText LastSeenNoteFor(
	not_null<UserData*> user,
	TimeId now,
	bool full,
	bool narrow);

[[nodiscard]] bool CanPeekLastSeen(not_null<UserData*> user);

void ShowLastSeenPeekBox(
	not_null<Window::SessionController*> controller,
	not_null<UserData*> user);

void LastSeenPeeksBox(
	not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session);

} // namespace Purple
