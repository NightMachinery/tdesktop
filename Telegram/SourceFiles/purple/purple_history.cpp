/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/history.h"

#include "data/data_chat_filters.h"
#include "data/data_folder.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "dialogs/dialogs_entry.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history_item.h"
#include "history/history_unread_things.h"
#include "purple/purple_gate.h"

#include <QtCore/QDateTime>

std::optional<Purple::ShowMode> History::purpleExemptFolderMode() const {
	const auto &exempt = Purple::ExemptFolders();
	if (exempt.empty()) {
		return std::nullopt;
	}
	// Only reached for a chat the preset would otherwise hide, and only when
	// some folder actually asked to be exempt, so the walk below costs nothing
	// for every preset that does not use the escape hatch.
	//
	// A folder that names no mode leaves its chats to the default for what they
	// are. The folder decides WHICH chats come in; the kind decides WHEN they
	// show. Forcing Always here would be the folder answering a question it was
	// not asked, and would quietly override the kind defaults for everything
	// filed in it.
	const auto fallback = Purple::DefaultShowMode(Purple::KindOf(peer));
	const auto self = const_cast<History*>(this);
	auto result = std::optional<Purple::ShowMode>();
	for (const auto &filter : owner().chatsFilters().list()) {
		if (!filter.id()) {
			continue;
		}
		for (const auto &folder : exempt) {
			if (filter.title().text.text.compare(
					folder.name,
					Qt::CaseInsensitive)
				|| !filter.contains(self)) {
				continue;
			}
			// A folder that asked for its pinned chats only lets through
			// exactly those. Being filed there is no longer enough, and the
			// rest are not dropped so much as left where they already were -
			// with whatever their own list decided, which for a preset that
			// names nothing else is hidden.
			if (folder.include == Purple::FolderInclude::Pinned
				&& !ranges::contains(
					filter.pinned(),
					not_null<History*>(self))) {
				continue;
			}
			// A chat can sit in two exempt folders, and the most permissive
			// wins: a narrow one saying no must not speak for a wide one that
			// would say yes.
			const auto mode = folder.showMode.value_or(fallback);
			if (!result
				|| (Purple::ShowModeRank(mode)
					> Purple::ShowModeRank(*result))) {
				result = mode;
			}
		}
	}
	return result;
}

bool History::purpleShowModeSatisfied(Purple::ShowMode mode) const {
	switch (mode) {
	case Purple::ShowMode::Always: return true;
	case Purple::ShowMode::Never: return false;
	default: break;
	}

	// A chat you are reading must not vanish from under you. tdesktop keeps a
	// "fake unread while opened" flag for exactly this shape of problem - it is
	// what stops a badge blinking off the moment you open something - and the
	// same flag is the honest answer here: the messages are only read in the
	// sense that you are looking at them right now.
	if (fakeUnreadWhileOpened()) {
		return true;
	}
	const auto state = chatListUnreadState();

	// A manual unread mark counts as a message: it is the user saying "I still
	// have to deal with this", which is the question being asked.
	const auto message = (state.messages > 0) || (state.marks > 0);
	switch (mode) {
	case Purple::ShowMode::Message: return message;
	case Purple::ShowMode::MessageOrReaction:
		return message || (state.reactions > 0);
	case Purple::ShowMode::Mention: return (state.mentions > 0);
	default: break;
	}
	return true;
}

bool History::purpleHiddenFromView() const {
	if (!Purple::Filtering()) {
		return false;
	}
	// Before every rule below it, because it is not a statement about what the
	// preset lets through: it is the chat you are looking at, or were looking at
	// a moment ago. Having just read something is the best evidence there is
	// that you are still working on it, and a chat that vanishes on the frame
	// you click away is startling in a way no policy justifies.
	if (purpleShownAsRecent()) {
		return false;
	}
	// A peek reveals, and says so here rather than leaving it to fall out of
	// what the rules below happen to answer. It used to be implicit twice over
	// - VisibleFor() forces Always while peeking, and ExemptFolders() went
	// empty - and the second of those was hiding archived chats a folder had
	// pulled in, because an empty exempt list also means "not in the view".
	//
	// Above the "until" decisions on purpose, so a peek reveals a chat you hid
	// as well as one the preset hides. That is what makes a hide-until
	// cancellable: the row has to come back for you to be able to right-click
	// it, and a peek is the one gesture that means "show me everything".
	if (Purple::Peeking()) {
		return false;
	}
	// A decision you made about this one chat, which outranks the preset for as
	// long as it lasts.
	if (const auto override = Purple::OverrideFor(peer)) {
		switch (*override) {
		case Purple::OverrideKind::Show: return false;
		case Purple::OverrideKind::Hide: return true;
		case Purple::OverrideKind::Notify: break;
		}
	}
	return purpleHiddenByPreset();
}

bool History::purpleHiddenByPreset() const {
	// A folder the preset pulls in decides first, because it is the more
	// specific statement: it named this folder, where a list entry named a kind
	// or a set of ids. What it does not decide is WHEN - a folder saying nothing
	// about a mode leaves its chats to the default for what they are.
	if (const auto folder = purpleExemptFolderMode()) {
		return !purpleShowModeSatisfied(*folder);
	}
	// Otherwise the entry that claimed this chat, or the default for whatever
	// the chat is. A mode that watches unread means the chat comes and goes on
	// its own as messages arrive and are read - a group appears exactly while
	// its mention badge would be lit, which is the rule being legible from the
	// chat list itself rather than only from the file.
	return !purpleShowModeSatisfied(Purple::VisibleFor(peer).show);
}

bool History::purpleWouldLeaveTheView() const {
	if (!Purple::Filtering() || Purple::Peeking()) {
		// A peek shows everything, so nothing on screen during one is on screen
		// because of a clock.
		return false;
	} else if (const auto override = Purple::OverrideFor(peer)) {
		// A hide stays in the reckoning - it is a reason the row would be gone,
		// not a reason it is here. A show is taken out, because it is exactly
		// the temporary thing being asked about.
		if (*override == Purple::OverrideKind::Hide) {
			return true;
		}
	}
	return purpleHiddenByPreset();
}

bool History::purpleInQuietFolder() const {
	const auto &quiet = Purple::QuietFolders();
	if (quiet.empty()) {
		return false;
	}
	// Same shape as the exempt and silenced walks, and free for the same
	// reason: a preset that asks for nothing here never reaches the loop.
	const auto self = const_cast<History*>(this);
	for (const auto &filter : owner().chatsFilters().list()) {
		if (!filter.id()) {
			continue;
		}
		for (const auto &name : quiet) {
			if (!filter.title().text.text.compare(name, Qt::CaseInsensitive)
				&& filter.contains(self)) {
				return true;
			}
		}
	}
	return false;
}

bool History::purpleHideUntilReaches(Purple::HideScope scope) const {
	// The file's answer first, because it is a single comparison and every
	// other test here costs something: shouldBeInChatList() asks this on every
	// refresh of every row, and under the default scope that has to come to
	// nothing without looking anything up.
	if (Purple::HideUntilScope() != scope) {
		return false;
	}
	const auto override = Purple::OverrideFor(peer);
	if (!override || (*override != Purple::OverrideKind::Hide)) {
		return false;
	}
	// And whether the hide is hiding anything at this moment. It is not while a
	// peek is running, or while you have the chat open and the close buffer is
	// holding it - and neither is a moment to take a row off a folder tab or a
	// number out of a badge, since both put the row back themselves. Which is
	// why this asks the view rather than the override.
	return purpleHiddenFromView();
}

void History::purpleRefreshUncounted() {
	const auto uncounted = purpleHideUntilReaches(
		Purple::HideScope::KeepInFolderUncounted);
	if (uncounted == _purpleUncounted) {
		return;
	}
	// Notified unconditionally, unlike setMuted() which first works out whether
	// there is anything to move: the flag only ever changes for a chat with a
	// live "hide until" on it, so this runs for a handful of chats at most and
	// there is nothing worth saving.
	const auto notifier = unreadStateChangeNotifier(true);
	_purpleUncounted = uncounted;
}

bool History::purpleReachableElsewhere() const {
	const auto filters = &owner().chatsFilters();

	// The preset's own extra tabs first: cheap, and the common case for a chat
	// somebody keeps deliberately to hand. View 0 is the main view and is the
	// thing being decided, so it is not "elsewhere".
	for (auto i = 1, count = filters->purpleViewCount(); i != count; ++i) {
		if (inChatList(Data::PurpleViewFilterId(i))) {
			return true;
		}
	}
	const auto &shown = Purple::ShownFolders();
	if (shown.empty()) {
		return false;
	}
	const auto self = const_cast<History*>(this);
	const auto all = ranges::any_of(shown, Purple::IsAllFolders);
	for (const auto &filter : filters->list()) {
		if (!filter.id() || !filter.contains(self)) {
			continue;
		} else if (all) {
			return true;
		}
		for (const auto &folder : shown) {
			// show_p = false takes the tab off the strip, and a folder with no
			// tab is not somewhere you can reach the chat from. Nor is one the
			// preset switched off entirely.
			if (!Purple::IsAllFolders(folder)
				&& Purple::FolderEnabled(folder)
				&& folder.show.value_or(true)
				&& !filter.title().text.text.compare(
					folder.name,
					Qt::CaseInsensitive)) {
				return true;
			}
		}
	}
	return false;
}

void History::purpleSetOpened(bool opened) {
	if (_purpleOpened == opened) {
		return;
	}
	_purpleOpened = opened;
	const auto seconds = Purple::RecentStaySeconds();
	if (opened) {
		// Decided now rather than on close, because two of the three scopes ask
		// where the chat was when you opened it - and reading it is exactly what
		// moves that answer.
		_purpleGraceUntil = 0;
		_purpleGraceEligible = (seconds > 0) && [&] {
			switch (Purple::RecentAppliesTo()) {
			case Purple::RecentScope::AlreadyInView:
				return inChatList(Data::kPurpleViewFilterId);
			case Purple::RecentScope::AnyOpenChat:
				return true;
			case Purple::RecentScope::AnyOpenChatExceptInFolder:
				return !purpleReachableElsewhere();
			}
			return false;
		}();
	} else if (_purpleGraceEligible && seconds > 0) {
		_purpleGraceUntil = crl::now() + seconds * crl::time(1000);
		owner().purpleWatchGrace(this);
	}
	if (seconds > 0 && Purple::Filtering()) {
		// Both directions. Opening can reveal a chat the preset hides, under the
		// wider scopes, and closing one the grace does not cover has to take it
		// away again - the flag below only speaks for chats that had unread.
		purpleRefreshChatListMembership();
	}
}

bool History::purpleShownAsRecent() const {
	if (!_purpleGraceEligible || !Purple::RecentStaySeconds()) {
		// The second test is what makes turning [recent] off take effect at
		// once rather than at the end of whatever was already running.
		return false;
	} else if (_purpleOpened) {
		return true;
	}
	return _purpleGraceUntil && (crl::now() < _purpleGraceUntil);
}

crl::time History::purpleGraceUntil() const {
	return _purpleGraceUntil;
}

History::PurpleTemporary History::purpleTemporary() const {
	const auto span = Purple::OverrideDeadline(peer);

	// A "show until" and nothing else. The mark means "this row is here on a
	// clock", and that is only true of a row the override put there: a chat a
	// "hide until" has put away is still a permanent member of whatever folder
	// tab is still showing it, and marking it as leaving would say the opposite
	// of what is happening. A "notify until" does not touch visibility at all.
	// What is on a clock in those two cases is the decision, not the row.
	//
	// And in both branches below, purpleWouldLeaveTheView() - the whole claim
	// the mark makes is that the row goes when the clock stops, so a chat the
	// preset lets through anyway must not carry one. Asked last because it is
	// the expensive test and the two cheap ones drop every ordinary row before
	// it, which matters: this runs per row per paint.
	if (span.untilUnix
		&& (span.kind == Purple::OverrideKind::Show)
		&& purpleWouldLeaveTheView()) {
		// Unix seconds, so it is converted here rather than stored twice. Only
		// ever against a deadline still in the future, which keeps the
		// arithmetic away from a wall clock that has been dragged backwards.
		const auto now = QDateTime::currentSecsSinceEpoch();
		const auto left = span.untilUnix - now;
		if (left <= 0) {
			return {};
		}
		return {
			crl::now() - std::max(now - span.startedUnix, int64(0))
				* crl::time(1000),
			crl::now() + left * crl::time(1000),
			true, // Held open by the override, not lingering from the buffer.
		};
	} else if (_purpleOpened) {
		// Open, so it is not counting down yet - the clock starts when you
		// stop looking at it.
		return {};
	} else if (purpleShownAsRecent()
		&& _purpleGraceUntil
		&& purpleWouldLeaveTheView()) {
		const auto span = Purple::RecentStaySeconds() * crl::time(1000);
		return { _purpleGraceUntil - span, _purpleGraceUntil };
	}
	return {};
}

bool History::purpleShownFromArchive() const {
	// Only asked of an archived chat, and only then does the folder walk run.
	// A preset that pulls in no folders never pays for this at all.
	return folder()
		&& Purple::Filtering()
		&& purpleExemptFolderMode().has_value();
}

bool History::purpleHiddenFromChatList() const {
	// Only a preset that asked for it takes a chat out of the app rather than
	// out of its own view. Everything else keeps the chat where it is and
	// leaves it out of the view alone, which is what lets it stay pinned,
	// searchable and reachable from the forward picker. See
	// docs/purple/work_mode.md.
	//
	// And the one-chat version of the same request: a "hide until" made while
	// `hide_scope = "hide_everywhere"'. A preset-wide switch and a decision
	// about a single chat, doing the same thing for the same reason - the
	// second one simply expires by itself.
	return (Purple::HideEverywhere() && purpleHiddenFromView())
		|| purpleHideUntilReaches(Purple::HideScope::Everywhere);
}

bool History::purpleKeptForView() const {
	// A "show until" on a chat with no conversation needs the same two gates a
	// hand-named one does, or it would be let through by every rule and still
	// have no row to appear in.
	if (const auto override = Purple::OverrideFor(peer)) {
		if (*override == Purple::OverrideKind::Show) {
			return true;
		}
	}
	return Purple::NamedExplicitly(peer);
}

void History::purpleRefreshShowMode() {
	if (!Purple::Filtering()) {
		return;
	}
	// The guard that keeps this off the hot path: a preset made of Always and
	// Never answers here without touching the chat list at all, and so does
	// every chat a folder pulled in without naming a mode.
	const auto folder = purpleExemptFolderMode();
	const auto mode = folder
		? *folder
		: Purple::VisibleFor(peer).show;
	if (!Purple::ShowModeWatchesUnread(mode)) {
		return;
	}
	const auto view = Data::kPurpleViewFilterId;
	const auto was = inChatList(view);
	purpleRefreshChatListMembership();
	if (inChatList(view) != was) {
		// An event that explains a chat appearing or vanishing with nobody
		// touching anything, so it is worth a line. It stays rare by
		// construction: only unread-gated chats reach here, and only on the
		// edge.
		//
		// Rarer than it looks, and measured: on an account with real folders
		// this never fires for a plain message. notifyUnreadStateChange() calls
		// chatsFilters().refreshHistory() first, which fixes the membership
		// before Data::Changes delivers here, so `was' already matches. The
		// subscription earns its keep only when refreshHistory() returns early
		// on list().empty() - an account with no folders at all.
		LOG(("Purple: show_mode '%1' %2 peer %3."
			).arg(Purple::ShowModeName(mode)
			).arg(was ? u"hid"_q : u"revealed"_q
			).arg(peer->id.value));
	}
}

void History::purpleRefreshChatListMembership() {
	// A chat the preset names by hand, with no dialog on the server, has never
	// been told which folder it is in - and shouldBeInChatList() refuses
	// anything whose folder is unknown, long before it gets as far as asking
	// whether the preset wants it. That check is right and stays: an entry with
	// no folder would trip the Expects() in refreshChatListEntry().
	//
	// So answer the question instead of skipping it. "The main list, then" is
	// the truthful answer for a conversation that does not exist yet, and the
	// server overwrites it the moment one does. setFolderPointer() takes it from
	// there, including the sort position, because a folder going from unknown to
	// known is exactly the case it already handles.
	if (!folderKnown() && purpleKeptForView()) {
		setFolderPointer(nullptr);
	}

	// Both, in this order. Neither call does both directions: the first drops
	// an entry that should now be gone, and only the second brings one back,
	// because leaving the chat list zeroes the sort key and
	// setChatListExistence(true) quietly removes an entry that has none.
	updateChatListExistence();
	updateChatListSortPosition();
}

namespace Purple {

bool StartsUncounted(not_null<PeerData*> peer) {
	const auto hide = OverrideFor(peer);
	return hide
		&& (*hide == OverrideKind::Hide)
		&& (HideUntilScope() == HideScope::KeepInFolderUncounted);
}

void UnreadThingEdge(
		not_null<Data::Thread*> thread,
		HistoryUnreadThings::Type type) {
	if ((type != HistoryUnreadThings::Type::Mentions)
		&& (type != HistoryUnreadThings::Type::Reactions)) {
		return;
	} else if (!Filtering()) {
		return;
	}
	thread->owningHistory()->purpleRefreshShowMode();
}

} // namespace Purple
