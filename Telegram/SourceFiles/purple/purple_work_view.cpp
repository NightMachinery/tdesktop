/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_session.h"

#include "apiwrap.h"
#include "data/data_chat_filters.h"
#include "data/data_changes.h"
#include "data/data_folder.h"
#include "dialogs/dialogs_entry.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "main/main_session.h"
#include "purple/purple_gate.h"
#include "purple/purple_mute.h"

#include <QtCore/QDateTime>

namespace Data {

void Session::setupPurpleWorkMode() {
	Purple::ActiveChanges(
	) | rpl::on_next([=] {
		refreshPurpleWorkMode();
	}, _lifetime);

	// A show_mode that watches unread makes membership depend on something the
	// chat list has no reason to re-examine on its own. This is the funnel:
	// Entry::notifyUnreadStateChange() fires UnreadView for every unread move,
	// and Changes batches those onto the main loop - so the refresh cannot
	// re-enter the unread bookkeeping that triggered it, which a hook placed
	// inside notifyUnreadStateChange() itself would.
	session().changes().historyUpdates(
		Data::HistoryUpdate::Flag::UnreadView
	) | rpl::on_next([=](const Data::HistoryUpdate &update) {
		update.history->purpleRefreshShowMode();
	}, _lifetime);
}

void Session::refreshPurpleWorkMode() {
	purpleStartTicking();

	// Every "until" arrives here - one being made, one being cancelled, a
	// preset switch putting a different set of them in force - and this is the
	// only place that sees all three. Without it the deadline timer was armed
	// only by the [recent] close buffer, so an override made while nothing was
	// in that buffer would sit there past its time until some unrelated event
	// happened to rearm it.
	purpleRearmGraceTimer();
	// Snapshot first: re-evaluating a mute notifies, and a handler that reacts
	// by resolving a peer would insert into _peers while we walked it.
	auto peers = std::vector<not_null<PeerData*>>();
	peers.reserve(_peers.size());
	for (const auto &[peerId, peer] : _peers) {
		peers.push_back(peer.get());
	}
	auto chats = 0;
	auto hidden = 0;
	auto mentioned = 0;
	auto silenced = 0;
	auto gated = std::vector<not_null<History*>>();
	for (const auto peer : peers) {
		// Mute first: hiding the chat takes its unread out of every running
		// total, and it has to already be counted as muted or not when it
		// goes, or the totals drift by whatever that chat was carrying.
		Purple::RefreshMute(peer);
		const auto history = historyLoaded(peer->id);
		if (!history) {
			continue;
		}
		// And then whether its unread counts at all, for the same reason and in
		// the same order: a "hide until" under
		// `keep_in_folder_but_exclude_from_badge_count' takes this chat out of
		// every total it is in, and that has to be paid across before hiding
		// the chat moves it between lists.
		history->purpleRefreshUncounted();
		++chats;
		const auto visible = Purple::VisibleFor(peer);
		if (visible.show == Purple::ShowMode::Never) {
			++hidden;
		}
		if (!visible.notify) {
			++silenced;
		}
		if (Purple::ShowModeWatchesUnread(visible.show)) {
			gated.push_back(history);
		}
		history->purpleRefreshChatListMembership();
	}
	refreshPurpleView();
	for (const auto &history : gated) {
		if (history->inChatList(kPurpleViewFilterId)) {
			++mentioned;
		}
	}

	// A preset that hides nothing looks exactly like a preset that is working,
	// and the usual cause is a list named slightly wrong. Say what it did once
	// per change rather than leaving it to be guessed at from the chat list.
	// Unread-gated chats are counted apart from hidden ones because they are
	// not the same claim: a gated chat is only out of the view while it has
	// nothing to say, so the count that means anything is how many of them are
	// still showing.
	LOG(("Purple: %1 of %2 loaded chats never shown, %3 unread-gated "
		"(%4 showing), %5 silenced, view holds %6."
		).arg(hidden).arg(chats).arg(gated.size()).arg(mentioned).arg(silenced
		).arg(purpleViewList()->indexed()->size()));

}

void Session::purpleWatchGrace(not_null<History*> history) {
	const auto until = history->purpleGraceUntil();
	if (!until) {
		return;
	}
	_purpleGrace[history->peer->id] = until;
	purpleRearmGraceTimer();
	purpleStartTicking();
}

void Session::purpleRearmGraceTimer() {
	auto earliest = crl::time(0);

	// The "until" decisions share this timer. Their deadlines are unix seconds
	// because they outlive a restart, so they are converted to the monotonic
	// clock here rather than kept in two units - and the conversion is only
	// ever done against a deadline that has not passed, which keeps the
	// arithmetic away from a wall clock that may have been dragged backwards.
	if (const auto deadline = Purple::NextOverrideDeadline()) {
		const auto left = deadline - QDateTime::currentSecsSinceEpoch();
		earliest = crl::now() + std::max(left, int64(0)) * crl::time(1000);
	}
	if (_purpleGrace.empty() && !earliest) {
		_purpleGraceTimer.cancel();
		return;
	}
	for (const auto &[peerId, until] : _purpleGrace) {
		if (!earliest || until < earliest) {
			earliest = until;
		}
	}
	// At least a tick, so a deadline that has already gone by still comes back
	// through the timer rather than re-entering the walk that armed it.
	_purpleGraceTimer.callOnce(
		std::max(earliest - crl::now(), crl::time(1)));
}

void Session::purpleStartTicking() {
	// Cheap to ask and cheap to be wrong about: the tick stops itself the
	// first time it finds nothing to redraw.
	if (Purple::RecentMarkStyle() == Purple::RecentStyle::Timer
		&& !_purpleTickTimer.isActive()) {
		_purpleTickTimer.callOnce(crl::time(1000));
	}
}

void Session::purpleTickTemporary() {
	// Only the rows that actually carry a ring, so a preset with none pays one
	// walk of the loaded peers and then stops the timer. Walks _peers and asks
	// for the history, the way refreshPurpleWorkMode() does - Session owns no
	// map of History objects to iterate.
	auto any = false;
	for (const auto &[peerId, peer] : _peers) {
		const auto history = historyLoaded(peerId);
		if (history && history->purpleTemporary().until) {
			any = true;
			history->updateChatListEntryPostponed();
		}
	}
	if (any) {
		_purpleTickTimer.callOnce(crl::time(1000));
	}
}

void Session::purpleGraceExpired() {
	const auto now = crl::now();
	auto due = std::vector<PeerId>();
	for (const auto &[peerId, until] : _purpleGrace) {
		if (until <= now) {
			due.push_back(peerId);
		}
	}
	// Collected first: the refresh below moves chat lists, and a handler that
	// reacted by opening a chat would edit the map we were walking.
	for (const auto &peerId : due) {
		_purpleGrace.remove(peerId);
		if (const auto history = historyLoaded(peerId)) {
			history->purpleRefreshChatListMembership();
		}
	}
	// An "until" that has run out has to move the chat AND re-evaluate its
	// mute, which is more than membership - so it goes through the same full
	// refresh a preset change does rather than the narrow one above.
	if (Purple::PruneOverrides()) {
		refreshPurpleWorkMode();
	}
	purpleRearmGraceTimer();
}

void Session::refreshPurpleView() {
	// Every view above the live count, emptied. That covers both leaving a
	// preset altogether and switching to one with fewer tabs, which is the case
	// that used not to exist: a view nothing refreshes again keeps its rows, and
	// they come back the moment some later preset reuses the id.
	//
	// Walk each view rather than the main list: an entry can outlive its place
	// in the main list, and leaving it linked to a list nothing will ever
	// refresh again is how a stale row survives into the next preset.
	for (auto i = chatsFilters().purpleViewCount(); i != kPurpleViewLimit; ++i) {
		const auto id = PurpleViewFilterId(i);
		const auto viewList = chatsFilters().chatsListLoaded(id);
		if (!viewList || viewList->indexed()->empty()) {
			continue;
		}
		// Before the rows go, while the pins still have entries to point at. A
		// pinned index nothing clears would outlive its view and reappear the
		// moment some later preset reused the id.
		viewList->pinned()->clear();
		auto stale = std::vector<not_null<Dialogs::Entry*>>();
		for (const auto &row : viewList->indexed()->all()) {
			stale.push_back(row->entry());
		}
		for (const auto &entry : stale) {
			entry->removeFromChatList(id, viewList);
			_chatListEntryRefreshes.fire(ChatListEntryRefresh{
				.key = Dialogs::Key(entry),
				.filterId = id,
				.existenceChanged = true
			});
		}
	}
	// The quiet list goes with them. It is not a view, so it is not in the run
	// above, but it is just as empty once no preset is running.
	if (!Purple::Filtering()) {
		if (const auto quiet = chatsFilters().chatsListLoaded(
				kPurpleQuietFilterId)) {
			auto stale = std::vector<not_null<Dialogs::Entry*>>();
			for (const auto &row : quiet->indexed()->all()) {
				stale.push_back(row->entry());
			}
			for (const auto &entry : stale) {
				entry->removeFromChatList(kPurpleQuietFilterId, quiet);
			}
		}
		return;
	}

	// The main list is what the view is drawn from, so it is also the complete
	// set of candidates - and unlike a walk over the peers it includes the
	// Archive row, which is not a peer at all.
	auto entries = std::vector<not_null<Dialogs::Entry*>>();
	for (const auto &row : _chatsList.indexed()->all()) {
		entries.push_back(row->entry());
	}
	// And the Archive's own list, because a folder can pull an archived chat
	// into the view and nothing walking the main list would ever reach it.
	// Cheap when the preset asks for nothing: the entries are visited, decide
	// they do not belong, and leave.
	if (const auto archive = folderLoaded(Data::Folder::kId)) {
		for (const auto &row : archive->chatsList()->indexed()->all()) {
			entries.push_back(row->entry());
		}
	}
	for (const auto &entry : entries) {
		refreshChatListEntry(entry);
	}
	refreshPurpleViewPinned();

	// After the membership pass above, not during it: a view's pinned order is
	// stated over the chats the view holds, and until every row has arrived
	// there is no way to tell "the file pins a chat this tab does not show"
	// from "it has not been added yet".
	for (auto i = 1, count = chatsFilters().purpleViewCount(); i != count; ++i) {
		refreshPurpleViewPins(i);
	}
}

void Session::refreshPurpleViewPinned() {
	// Purple: the main view's pinned list is a copy of the main list's, minus
	// the chats the preset hides. Pinning is an account-level fact, so the view
	// must not own pins of its own - it only reflects them, and the one place
	// that says the order moved is where the copy is retaken.
	//
	// Only the main view. An extra view is the preset's own invention and its
	// order is the preset's to state, so it holds no pins at all until the file
	// can say what they are.
	//
	// The guard is what stops that copy reporting itself: applying a pinned
	// index re-sorts the entry, which comes back through here.
	if (!Purple::Filtering() || _purpleViewPinning) {
		return;
	}
	if (Purple::PresetOwnsPins()) {
		// The preset states its own order, so there is nothing to mirror. Seeded
		// from settings.toml exactly as an extra view's is - see
		// refreshPurpleViewPins(), which this then shares.
		refreshPurpleViewPins(0);
		return;
	}
	_purpleViewPinning = true;
	auto pinned = std::vector<not_null<History*>>();
	for (const auto &key : _chatsList.pinned()->order()) {
		// Folders are sorted fixed-on-top rather than by pinned index, so the
		// archive never needs a place here even when the server sends one.
		if (const auto history = key.history()) {
			if (history->inChatList(kPurpleViewFilterId)) {
				pinned.push_back(history);
			}
		}
	}
	purpleViewList()->pinned()->applyList(pinned);
	_purpleViewPinning = false;
}

void Session::refreshPurpleViewPins(int index) {
	Expects(index >= 0);

	// Purple: an extra view's order is the preset's own statement, so it is
	// seeded from settings.toml rather than mirrored from the chat list. An id
	// the file names that this tab does not hold is not an error and is not
	// dropped - the chat may not have loaded yet, and savePurpleViewPins()
	// writes it back where it was.
	//
	// Same re-entrancy guard as the mirror above, and for the same reason:
	// applying a pinned index re-sorts the entry, which comes back through
	// refreshChatListEntry() and lands here again.
	if (_purpleViewPinning) {
		return;
	}
	_purpleViewPinning = true;
	const auto guard = gsl::finally([&] { _purpleViewPinning = false; });

	const auto &wanted = index
		? Purple::ExtraViewPins(index - 1)
		: Purple::PresetPins();
	const auto viewList = purpleViewList(index);
	auto pinned = std::vector<not_null<History*>>();
	if (!wanted.empty()) {
		auto byId = base::flat_map<Purple::PeerIdValue, not_null<History*>>();
		for (const auto &row : viewList->indexed()->all()) {
			if (const auto history = row->key().history()) {
				byId.emplace(Purple::IdOf(history->peer), history);
			}
		}
		pinned.reserve(wanted.size());
		for (const auto id : wanted) {
			const auto i = byId.find(id);
			if (i != end(byId)) {
				pinned.push_back(i->second);
			}
		}
	}
	viewList->pinned()->applyList(pinned);
}

void Session::savePurpleViewPins(int index) {
	Expects(index >= 0);

	const auto viewList = purpleViewList(index);
	auto ids = std::vector<Purple::PeerIdValue>();
	for (const auto &key : viewList->pinned()->order()) {
		if (const auto history = key.history()) {
			if (const auto id = Purple::IdOf(history->peer)) {
				ids.push_back(id);
			}
		}
	}
	auto present = base::flat_set<Purple::PeerIdValue>();
	for (const auto &row : viewList->indexed()->all()) {
		if (const auto history = row->key().history()) {
			present.emplace(Purple::IdOf(history->peer));
		}
	}
	// An id the file pins that this tab does not currently hold keeps the place
	// the file gave it. Dropping it would silently unpin whatever it names the
	// first time anything else in the tab moved, and the usual reason a pinned
	// chat is missing is that it has not finished loading.
	const auto &before = index
		? Purple::ExtraViewPins(index - 1)
		: Purple::PresetPins();
	for (auto i = 0, count = int(before.size()); i != count; ++i) {
		if (!present.contains(before[i])) {
			ids.insert(ids.begin() + std::min(i, int(ids.size())), before[i]);
		}
	}

	const auto title = Purple::TitleResolver(&session());
	crl::on_main(&session(), [=, this] {
		// The write lands back here as a settings reload, which rebuilds every
		// chat list. Doing that from inside the drag handler that asked for it
		// would be pulling the rows out from under it.
		const auto saved = index
			? Purple::SaveExtraViewPins(index - 1, ids, title)
			: Purple::SavePresetPins(ids, title);
		if (!saved) {
			// Refused: the file does not parse, or the view is written inline.
			// Put the tab back to what the file says rather than leave it
			// showing an order nothing is going to remember.
			refreshPurpleViewPins(index);
		}
	});
}

not_null<Dialogs::MainList*> Session::purpleViewList(int index) {
	return chatsFilters().chatsList(PurpleViewFilterId(index));
}

Dialogs::UnreadState Session::purpleBadgeUnread() const {
	auto result = purpleBadgeList()->unreadState();

	// The quiet list holds exactly the view members that sit in a folder which
	// asked not to be counted, so this is a subset by construction and cannot
	// drive the badge negative. That is the whole reason it is a real MainList
	// rather than a walk: the total is accumulated through the same
	// addEntry/removeEntry path as the list it is being taken out of.
	const auto filters = &const_cast<Session*>(this)->chatsFilters();
	if (const auto quiet = filters->chatsListLoaded(kPurpleQuietFilterId)) {
		result = result - quiet->unreadState();
	}
	return result;
}

FilterId Session::purpleBadgeFilterId() const {
	return Purple::Filtering() ? kPurpleViewFilterId : FilterId();
}

not_null<const Dialogs::MainList*> Session::purpleBadgeList() const {
	if (!Purple::Filtering()) {
		return &_chatsList;
	}
	// The view is created on demand like every other filter list, and asking
	// for the badge is as good a demand as any - it is asked for constantly
	// and always outlives the first call.
	return const_cast<Session*>(this)->purpleViewList();
}


bool Session::purpleSetChatPinned(
		Dialogs::Key key,
		FilterId filterId,
		bool pinned) {
	if (!IsPurpleView(filterId)) {
		return false;
	}
	const auto view = PurpleViewIndex(filterId);
	if (!view && !Purple::PresetOwnsPins(&session())) {
		setChatPinned(key, FilterId(0), pinned);
	} else {
		purpleViewList(view)->pinned()->setPinned(key, pinned);
		savePurpleViewPins(view);
		notifyPinnedDialogsOrderUpdated();
	}
	return true;
}

std::optional<bool> Session::purpleCanPin(
		FilterId filterId,
		not_null<History*> history) const {
	if (!IsPurpleView(filterId)) {
		return std::nullopt;
	}
	const auto view = PurpleViewIndex(filterId);
	if (!view && !Purple::PresetOwnsPins(&session())) {
		return pinnedCanPin(history);
	}
	const auto pinned = const_cast<Session*>(this)
		->purpleViewList(view)
		->pinned();
	const auto limit = pinnedChatsLimit(filterId);
	return (int(pinned->order().size()) < limit)
		|| ranges::contains(
			pinned->order(),
			history.get(),
			&Dialogs::Key::history);
}

void Session::purpleReorderPinned(
		FilterId filterId,
		Dialogs::Key key1,
		Dialogs::Key key2) {
	const auto view = PurpleViewIndex(filterId);
	if (!view && !Purple::PresetOwnsPins(&session())) {
		reorderTwoPinnedChats(FilterId(0), key1, key2);
		return;
	}
	purpleViewList(view)->pinned()->reorder(key1, key2);
	notifyPinnedDialogsOrderUpdated();
}

void Session::purpleRefreshChatListEntry(Dialogs::Key key) {
	using namespace Dialogs;

	const auto entry = key.entry();
	const auto history = entry->asHistory();
	// Purple: the preset's views, which are filters no server knows about. They
	// have to be maintained here rather than folded into shouldBeInChatList()
	// because that is the whole point of the design: the main list above stays
	// complete and only these views leave anything out.
	//
	// Folders belong to the main view as they belong to All chats, which is why
	// this is above the history-only work below - the Archive row would
	// otherwise disappear the moment a preset started. An extra view holds only
	// chats: its membership comes from lists of peers, and there is nothing a
	// list could say that would put the archive on one.
	for (auto i = 0, count = chatsFilters().purpleViewCount(); i != count; ++i) {
		const auto id = PurpleViewFilterId(i);
		const auto viewList = purpleViewList(i);
		const auto belongs = i
			? (history && Purple::ExtraViewHolds(i - 1, history->peer))
			: ((history || entry->asFolder())
				// Archived chats are not in All chats and so are not in the
				// view either - unless a folder the preset pulls in holds
				// them, which is the one thing that overrides the archive.
				// Topics and sublists live in lists of their own.
				&& (!entry->folder() || entry->purpleShownFromArchive())
				&& !entry->purpleHiddenFromView());
		auto event = ChatListEntryRefresh{ .key = key, .filterId = id };
		if (belongs) {
			event.existenceChanged = !entry->inChatList(id);
			if (event.existenceChanged) {
				entry->addToChatList(id, viewList);
			} else {
				event.moved = entry->adjustByPosInChatList(id, viewList);
			}
		} else if (entry->inChatList(id)) {
			entry->removeFromChatList(id, viewList);
			event.existenceChanged = true;
		}
		const auto joinedOrLeft = event.existenceChanged;
		if (event) {
			_chatListEntryRefreshes.fire(std::move(event));
		}
		if (!joinedOrLeft) {
			continue;
		} else if (!i) {
			// A pinned chat that has just entered the main view needs its place
			// in the order, and one that has just left needs to stop holding it.
			refreshPurpleViewPinned();
		} else if (history && ranges::contains(
				Purple::ExtraViewPins(i - 1),
				Purple::IdOf(history->peer))) {
			// The same for an extra view, except that the order it is taking a
			// place in is the file's. Tested against what the file pins rather
			// than what the tab currently shows, so a chat that loads late still
			// lands where the preset put it.
			refreshPurpleViewPins(i);
		}
	}

	// Purple: the quiet list - the main view's members that sit in a folder
	// which asked not to be counted. Maintained here, right after the views, so
	// it is always a subset of view 0: purpleBadgeUnread() subtracts it, and a
	// subtraction that is not a subset is how a badge goes negative.
	{
		const auto id = kPurpleQuietFilterId;
		const auto quiet = chatsFilters().chatsList(id);
		const auto belongs = history
			&& entry->inChatList(kPurpleViewFilterId)
			&& history->purpleInQuietFolder();
		if (belongs) {
			if (!entry->inChatList(id)) {
				entry->addToChatList(id, quiet);
			} else {
				entry->adjustByPosInChatList(id, quiet);
			}
		} else if (entry->inChatList(id)) {
			entry->removeFromChatList(id, quiet);
		}
		// Deliberately no _chatListEntryRefreshes here. Nothing draws this
		// list, so telling the UI a row moved in it is pure noise.
	}

	// Purple: and the other thing a folder can decide about a chat that drifts
	// into it. A rule-based folder computes membership from the chat's own
	// properties, so nothing announces the crossing, and History::muted() is a
	// cache that would go on ringing. Same place and same shape as the quiet
	// list above, and free for a preset that silences no folder.
	if (history) {
		Purple::RefreshFolderMute(history->peer);
	}

}

void Session::purpleRemoveChatListEntry(Dialogs::Key key) {
	using namespace Dialogs;

	const auto entry = key.entry();
	// Purple: and the quiet list, which is not a view and not in the run.
	if (entry->inChatList(kPurpleQuietFilterId)) {
		entry->removeFromChatList(
			kPurpleQuietFilterId,
			chatsFilters().chatsList(kPurpleQuietFilterId));
	}

	// Purple: same as the loop above, for the filters that are not in it. Every
	// id in the reserved run rather than only the live ones: a chat leaving for
	// good must not stay linked to a view the preset dropped a moment earlier.
	for (auto i = 0; i != kPurpleViewLimit; ++i) {
		const auto id = PurpleViewFilterId(i);
		if (entry->inChatList(id)) {
			entry->removeFromChatList(id, purpleViewList(i));
			_chatListEntryRefreshes.fire(ChatListEntryRefresh{
				.key = key,
				.filterId = id,
				.existenceChanged = true
			});
		}
	}
}

} // namespace Data

namespace Purple {

bool SavePinnedViewOrder(
		not_null<Main::Session*> session,
		FilterId filterId,
		Data::Folder *folder) {
	if (!Data::IsPurpleView(filterId)) {
		return false;
	}
	const auto view = Data::PurpleViewIndex(filterId);
	if (view || PresetOwnsPins(session)) {
		session->data().savePurpleViewPins(view);
	} else {
		session->api().savePinnedOrder(folder);
	}
	return true;
}

} // namespace Purple
