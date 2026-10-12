#pragma once

#include "base/basic_types.h"

namespace Data {

// Purple: the id of the chat list a work preset shows in place of All chats,
// and the first of a small reserved run - the preset's main view, then one id
// per extra view it defines. They are filters like any other as far as the chat
// list machinery is concerned, which is the whole point - the main list stays
// complete, so pins, unread totals and "in a filter implies in the main list"
// all keep working, and a preset only decides what its views contain.
//
// The value is "PURP" as ASCII, which no server will ever hand out - real
// folder ids are small - and is recognisable on sight in a log line.
//
// Positive on purpose, even though "not a real folder" would read better as a
// negative: the dialogs code already spends `id < 0' on its own sentinels (the
// side bar's Edit button, FiltersMenu's cleared-drag marker), and a negative id
// would silently inherit all of that. The places that must not treat a view as
// an editable folder test IsPurpleView() instead, where the intent is written
// down. See docs/purple/work_mode.md.
inline constexpr auto kPurpleViewFilterId = FilterId(0x50555250);

// How many ids the run reserves. A preset asking for more tabs than this is
// asking for a strip nobody can read, and the cap is what keeps the test below
// a range check rather than an open-ended one that could collide with a real
// folder id if the server ever grew them.
inline constexpr auto kPurpleViewLimit = 16;

[[nodiscard]] inline bool IsPurpleView(FilterId id) {
	return (id >= kPurpleViewFilterId)
		&& (id < kPurpleViewFilterId + kPurpleViewLimit);
}

// Which view an id names: 0 is the preset's main view - the one that stands
// where All chats stands and carries the badge - and n > 0 its nth extra one.
// Only meaningful for an id IsPurpleView() accepts.
[[nodiscard]] inline int PurpleViewIndex(FilterId id) {
	return int(id - kPurpleViewFilterId);
}

[[nodiscard]] inline FilterId PurpleViewFilterId(int index) {
	return kPurpleViewFilterId + index;
}

// One more reserved id, just past the run, for the "quiet" list: the main
// view's members that sit in a folder which asked not to be counted. It is
// never a tab and never in Purple::ShownList() - it exists so the app badge can
// be "the view, minus these" with the subtraction guaranteed non-negative,
// because a real MainList accumulates its total through the same
// addEntry/removeEntry path as the list it is subtracted from.
//
// Deliberately outside the run above, so IsPurpleView() stays a clean range
// test and nothing treats this as a view to be drawn.
inline constexpr auto kPurpleQuietFilterId
	= FilterId(kPurpleViewFilterId + kPurpleViewLimit);


} // namespace Data
