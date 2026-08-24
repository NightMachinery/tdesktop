/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_splice.h"

class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class PopupMenu;
class Show;
} // namespace Ui

// The chat-menu side of Work Mode: putting a chat into one of the user's lists,
// and taking it back out, without opening settings.toml. Everything it writes
// goes through Purple::AddToList / RemoveFromList, so the file keeps its
// comments and its layout. See docs/purple/work_mode.md.
namespace Purple {

// Whether settings.toml defines a list a chat could be put into at all. False
// for an unconfigured fork, and for one that only tuned the four catch-alls -
// in both cases the menu item is left out entirely rather than opening on an
// empty submenu.
[[nodiscard]] bool HasCustomLists();

// One checkable item per list the user wrote, in the priority order the file
// gives them, plus - while a preset is running - a line naming the list that
// is currently deciding how this chat behaves.
void FillListsMenu(
	not_null<Ui::PopupMenu*> menu,
	std::shared_ptr<Ui::Show> show,
	not_null<PeerData*> peer);

} // namespace Purple
