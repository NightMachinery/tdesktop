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
#include "ui/widgets/menu/menu_multiline_action.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_media_player.h" // mediaPlayerMenuCheck
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h" // defaultFlatLabel

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
			: u"In no list '%1' names"_q.arg(ViewName());
		const auto session = &peer->session();

		// A wrapping label rather than an ordinary row. An ordinary one elides
		// at the menu's widthMax, which cut this off after about thirty-five
		// characters - and the tail is where the answer is, so what survived
		// was the half nobody came here to read.
		//
		// No FixAmpersandInAction on this one, unlike the list rows below: a
		// label draws '&' as itself, and only an action's text reads it as a
		// mnemonic.
		const auto &st = menu->st().menu;
		auto label = base::make_unique_q<Ui::Menu::MultilineAction>(
			menu->menu(),
			st,
			st::defaultFlatLabel,
			// Where an ordinary row puts its text, so this lines up with the
			// list rows under it and clears the icon on its left.
			QPoint(st.itemPadding.left(), st.itemPadding.top()),
			TextWithEntities{ u"%1: %2%3"_q.arg(where, state, folder) },
			&st::menuIconInfo);
		const auto action = menu->addAction(std::move(label));
		QObject::connect(action, &QAction::triggered, action, [=] {
			show->showBox(Box(PresetBox, session));
		});
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

void AddListsSubmenu(
		const Ui::Menu::MenuCallback &add,
		std::shared_ptr<Ui::Show> show,
		not_null<PeerData*> peer) {
	if (!HasCustomLists() || !IdOf(peer)) {
		// Left out entirely rather than opening on an empty submenu, so an
		// unconfigured fork's menus are untouched.
		return;
	}
	// Set apart above and below. Everything around it acts on the chat itself;
	// this one acts on the preset that is deciding the chat, and reaching for
	// it by mistake edits a file. No separator style named: the chat list
	// builds its menu with popupMenuExpandedSeparator, so this is the thick
	// divider there and the ordinary hairline elsewhere, which is right in
	// both.
	add(Ui::Menu::MenuCallback::Args{ .isSeparator = true });
	add(Ui::Menu::MenuCallback::Args{
		.text = u"Work Mode"_q,
		.handler = nullptr,
		.icon = &st::menuIconAddToFolder,
		.fillSubmenu = [=](not_null<Ui::PopupMenu*> menu) {
			FillListsMenu(menu, show, peer);
		},
		.submenuSt = &st::popupMenuWithIcons,
	});
	add(Ui::Menu::MenuCallback::Args{ .isSeparator = true });
}

} // namespace Purple
