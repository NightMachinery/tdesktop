/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_list_menu.h"

#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "history/history.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_gate.h"
#include "purple/purple_preset_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_media_player.h" // mediaPlayerMenuCheck
#include "styles/style_menu_icons.h"

namespace Purple {
namespace {

[[nodiscard]] QString DisplayTitle(const List &list) {
	return list.title.isEmpty() ? list.name : list.title;
}

} // namespace

MemberTitle TitleResolver(not_null<Main::Session*> session) {
	const auto owner = &session->data();
	return [=](PeerIdValue id) {
		const auto bare = BareId(id);
		// The file keeps the bare id, which is all Purple::IdOf() ever had, so
		// the type has to be guessed back. Bare ids are unique across the three
		// kinds in practice; if they ever were not, the worst case is a stale
		// name in a comment nothing reads back.
		const auto peer = [&]() -> PeerData* {
			if (const auto user = owner->peerLoaded(peerFromUser(bare))) {
				return user;
			} else if (const auto chat = owner->peerLoaded(peerFromChat(bare))) {
				return chat;
			}
			return owner->peerLoaded(peerFromChannel(bare));
		}();
		return peer ? peer->name() : QString();
	};
}

bool HasCustomLists() {
	for (const auto &list : ActiveSettings().lists) {
		if (!IsCatchAll(list.kind)) {
			return true;
		}
	}
	return false;
}

void FillListsMenu(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<Ui::Show> show,
		not_null<PeerData*> peer) {
	const auto id = IdOf(peer);
	if (!id) {
		return;
	}
	const auto title = TitleResolver(&peer->session());

	// Which list is deciding this chat right now. It is the answer to the
	// question that brings anyone here - "why is this hidden?" - and it names a
	// catch-all just as readily as one of the user's own lists, because
	// "@bots is hiding it" is exactly as useful an answer.
	if (Filtering()) {
		if (const auto effective = ListFor(peer)) {
			if (const auto list = ActiveSettings().list(effective->list)) {
				// What the list wanted, and what actually happened - which are
				// not the same thing for a chat an exempt folder rescued. The
				// state has to be the second: a line reading "hidden" over a
				// chat sitting in the list is worse than no line at all.
				const auto verdict = VisibleFor(peer);
				const auto history = peer->owner().historyLoaded(peer);
				const auto hidden = history && history->purpleHiddenFromView();
				const auto state = hidden
					? (verdict.mentionGated
						? u"hidden until a mention"_q
						: u"hidden"_q)
					: verdict.notify
					? u"shown"_q
					: u"silenced"_q;
				const auto folder = (!hidden && !verdict.show)
					? u", shown by a folder"_q
					: QString();
				menu->addAction(
					u"In '%1': %2%3"_q.arg(DisplayTitle(*list), state, folder),
					[=] { show->showBox(Box(PresetBox)); },
					&st::menuIconInfo);
				menu->addSeparator();
			}
		}
	}

	for (const auto &list : ActiveSettings().lists) {
		if (IsCatchAll(list.kind)) {
			// Membership is by chat type, so there is nothing to toggle. The
			// preset box is where their defaults are explained.
			continue;
		}
		const auto name = list.name;
		const auto member = std::find(
			list.members.begin(),
			list.members.end(),
			id) != list.members.end();
		// Locked lists are offered like any other: `locked' stops a preset
		// overriding what the list does, not the user deciding who is in it.
		menu->addAction(
			Ui::Text::FixAmpersandInAction(DisplayTitle(list)),
			[=] {
				// Written before the write, because a successful one reloads
				// settings.toml and rebuilds every chat list from inside this
				// callback - with the menu that owns it still open.
				menu->hideMenu();

				const auto ok = member
					? RemoveFromList(name, id, title)
					: AddToList(name, id, title);
				if (!ok) {
					show->showToast(u"Could not edit %1. See the log."_q.arg(
						SettingsFilePath()));
				}
			},
			(member ? &st::mediaPlayerMenuCheck : nullptr));
	}
}

} // namespace Purple
