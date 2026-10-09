/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_chat_menus.h"

#include "base/weak_ptr.h"
#include "boxes/premium_limits_box.h"
#include "chat_helpers/compose/compose_show.h"
#include "data/data_chat_filters.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "dialogs/dialogs_entry.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwidget.h"
#include "purple/purple_config.h"
#include "purple/purple_gate.h"
#include "purple/purple_last_seen.h"
#include "purple/purple_list_menu.h"
#include "purple/purple_pinned_music.h"
#include "purple/purple_preset_box.h"
#include "ui/layers/generic_box.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"

#include <utility>

#include "styles/style_menu_icons.h"

namespace Purple {

bool AddPresetMuteRow(
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread,
		const Ui::Menu::MenuCallback &addAction) {
	const auto byPreset = Silenced(thread->peer());
	if (byPreset) {
		const auto show = controller->uiShow();
		const auto session = &controller->session();
		addAction(
			u"Silenced by '%1'"_q.arg(ViewName()),
			[=] { show->showBox(Box(PresetBox, session)); },
			&st::menuIconMute);
	}
	return byPreset;
}

void AddPeerListsSubmenu(
		not_null<Window::SessionController*> controller,
		PeerData *peer,
		bool topic,
		bool sublist,
		const Ui::Menu::MenuCallback &addAction) {
	if (!peer || topic || sublist) {
		return;
	}
	AddListsSubmenu(addAction, controller->uiShow(), peer);
}

void AddLastSeenPeekAction(
		not_null<Window::SessionController*> controller,
		PeerData *peer,
		const Ui::Menu::MenuCallback &addAction) {
	const auto user = peer ? peer->asUser() : nullptr;
	if (!user || !CanPeekLastSeen(user)) {
		return;
	}
	addAction(
		tr::lng_lastseen_peek_action(tr::now),
		[=] { ShowLastSeenPeekBox(controller, user); },
		&st::menuIconStealth);
}

void AddPinnedMusicAction(
		not_null<Window::SessionController*> controller,
		Data::Thread *thread,
		bool sublist,
		const Ui::Menu::MenuCallback &addAction) {
	if (!thread || sublist) {
		return;
	}
	const auto weak = base::make_weak(thread);
	addAction(tr::lng_pinned_music_menu(tr::now), [=] {
		if (const auto strong = weak.get()) {
			ShowPinnedMusicBox(controller, strong);
		}
	}, &st::menuIconDownload);
}

bool TranslationPremiumAvailable(not_null<PeerData*> peer) {
	return peer->session().premium() || LocalPremium();
}

bool TogglePinnedInView(
		not_null<Window::SessionController*> controller,
		not_null<Dialogs::Entry*> entry,
		FilterId filterId,
		Fn<void()> onToggled) {
	if (!Data::IsPurpleView(filterId)) {
		return false;
	}
	const auto view = Data::PurpleViewIndex(filterId);
	if (view > 0 || PresetOwnsPins(&controller->session())) {
		const auto history = entry->asHistory();
		if (!history) {
			return true;
		}
		const auto owner = &history->owner();
		const auto isPinned = !history->isPinnedDialog(filterId);
		if (isPinned && !owner->pinnedCanPin(filterId, history)) {
			controller->show(Box(PinsLimitBox, &history->session()));
			return true;
		}
		owner->setChatPinned(history, filterId, isPinned);
		if (isPinned) {
			controller->content()->dialogsToUp();
			if (onToggled) {
				onToggled();
			}
		}
		return true;
	}
	Window::TogglePinnedThread(
		controller,
		entry,
		FilterId(0),
		std::move(onToggled));
	return true;
}

void ShowPresetBox(not_null<Window::SessionController*> controller) {
	controller->show(Box(PresetBox, &controller->session()));
}

} // namespace Purple
