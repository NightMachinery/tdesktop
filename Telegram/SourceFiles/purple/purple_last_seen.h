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
// separately: `base' is what upstream would have written, `link' is the words
// that open the trade and nothing else, and `text' is the two joined the way
// they are shown. A caller that cannot make a link - the chat header paints a
// plain string - shows `text' and hit-tests the tail; one that can - the
// profile - builds `base' plus a link out of `link'.
//
// Every field is already gated: `link' is empty unless there is something to
// offer, so no call site repeats the rules.
struct LastSeenNote {
	QString text;
	QString base;
	QString link;
};

// Why this person's last seen is coarse, or None when it is not coarse, when
// the answer is "a long time ago" (which the server does not explain, and the
// fork will not guess at), or when the peer is one nobody has a last seen for.
[[nodiscard]] LastSeenReason ReasonForUser(not_null<UserData*> user);

// `full' picks Data::OnlineTextFull over Data::OnlineText, the way the profile
// and the chat header already differ. `narrow' asks for the short mark instead
// of the words, for a line with no room for a sentence.
[[nodiscard]] LastSeenNote LastSeenNoteFor(
	not_null<UserData*> user,
	TimeId now,
	bool full,
	bool narrow);

// The sheet. Refuses in a toast rather than opening when there is nothing to
// trade for, or when the last trade with this person is too recent, so a call
// site is a click handler and not a copy of the rules.
void ShowLastSeenTradeBox(
	not_null<Window::SessionController*> controller,
	not_null<UserData*> user);

// What the trades read, newest first. Settings -> Advanced -> Purple.
void LastSeenTradesBox(
	not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session);

} // namespace Purple
