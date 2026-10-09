/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "data/data_types.h"
#include "purple/hooks/mute.h"
#include "purple/purple_preset_box.h"
#include "ui/widgets/menu/menu_add_action_callback.h"

class PeerData;

namespace Data {
class Thread;
} // namespace Data

namespace Dialogs {
class Entry;
} // namespace Dialogs

namespace Window {
class SessionController;
} // namespace Window

namespace Purple {

[[nodiscard]] bool AddPresetMuteRow(
	not_null<Window::SessionController*> controller,
	not_null<Data::Thread*> thread,
	const Ui::Menu::MenuCallback &addAction);
void AddPeerListsSubmenu(
	not_null<Window::SessionController*> controller,
	PeerData *peer,
	bool topic,
	bool sublist,
	const Ui::Menu::MenuCallback &addAction);
void AddLastSeenPeekAction(
	not_null<Window::SessionController*> controller,
	PeerData *peer,
	const Ui::Menu::MenuCallback &addAction);
void AddPinnedMusicAction(
	not_null<Window::SessionController*> controller,
	Data::Thread *thread,
	bool sublist,
	const Ui::Menu::MenuCallback &addAction);
[[nodiscard]] bool TranslationPremiumAvailable(not_null<PeerData*> peer);
[[nodiscard]] bool TogglePinnedInView(
	not_null<Window::SessionController*> controller,
	not_null<Dialogs::Entry*> entry,
	FilterId filterId,
	Fn<void()> onToggled);
void ShowPresetBox(not_null<Window::SessionController*> controller);

} // namespace Purple
