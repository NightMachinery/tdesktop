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

bool HasCustomLists() {
	return !ActiveSettings().lists.empty();
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

	// Which entry is deciding this chat right now. It is the answer to the
	// question that brings anyone here - "why is this hidden?" - and no entry
	// at all is as real an answer as any, because a preset names what gets
	// through and silence about a chat is a decision too.
	if (Filtering()) {
		// What the entry wanted, and what actually happened - which are not the
		// same thing for a chat a folder pulled back in. The state has to be
		// the second: a line reading "hidden" over a chat sitting in the list
		// is worse than no line at all.
		const auto verdict = VisibleFor(peer);
		const auto history = peer->owner().historyLoaded(peer);
		const auto hidden = history && history->purpleHiddenFromView();

		// A chat held back by a mode is a different answer from one hidden
		// outright: it says what would bring it back, which is the only useful
		// thing to say to somebody asking why they cannot see it.
		const auto waiting = [&] {
			switch (verdict.show) {
			case ShowMode::Message: return u"hidden until a message"_q;
			case ShowMode::MessageOrReaction:
				return u"hidden until a message or reaction"_q;
			case ShowMode::Mention: return u"hidden until a mention"_q;
			default: break;
			}
			return u"hidden"_q;
		};
		const auto state = hidden
			? waiting()
			: verdict.notify
			? u"shown"_q
			: u"silenced"_q;
		const auto folder = (!hidden && verdict.show == ShowMode::Never)
			? u", shown by a folder"_q
			: QString();
		const auto effective = ListFor(peer);
		const auto list = effective
			? ActiveSettings().list(effective->list)
			: nullptr;
		const auto where = list
			? u"In '%1'"_q.arg(DisplayTitle(*list))
			: u"In no list '%1' names"_q.arg(ActiveResolved().preset);
		const auto session = &peer->session();
		menu->addAction(
			// A list title is the user's own text and can hold an '&', which a
			// menu would otherwise eat as a mnemonic - the same reason the list
			// rows below are fixed.
			Ui::Text::FixAmpersandInAction(
				u"%1: %2%3"_q.arg(where, state, folder)),
			[=] { show->showBox(Box(PresetBox, session)); },
			&st::menuIconInfo);
		menu->addSeparator();
	}

	for (const auto &list : ActiveSettings().lists) {
		const auto name = list.name;
		const auto member = std::find(
			list.members.begin(),
			list.members.end(),
			id) != list.members.end();
		// Every list, including one that matches by kind: adding a chat there
		// writes an explicit member id, which is how you pull one chat out of
		// a rule that would otherwise have swept it up somewhere else.
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
