/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_unread_value.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_chat_filters.h"
#include "data/data_folder.h"
#include "data/data_session.h"
#include "purple/purple_gate.h"
#include "purple/purple_folder_strip.h"
#include "main/main_session.h"
#include "window/notifications_manager.h"

namespace Data {
namespace {

rpl::producer<Dialogs::UnreadState> MainListUnreadState(
		not_null<Dialogs::MainList*> list) {
	return rpl::single(rpl::empty) | rpl::then(
		list->unreadStateChanges() | rpl::to_empty
	) | rpl::map([=] {
		return list->unreadState();
	});
}

} // namespace

[[nodiscard]] Dialogs::UnreadState MainListMapUnreadState(
		not_null<Main::Session*> session,
		const Dialogs::UnreadState &state) {
	const auto folderId = Data::Folder::kId;
	if (const auto folder = session->data().folderLoaded(folderId)) {
		return state - folder->chatsList()->unreadState();
	}
	return state;
}

rpl::producer<Dialogs::UnreadState> UnreadStateValue(
		not_null<Main::Session*> session,
		FilterId filterId) {
	if (filterId > 0 && !IsPurpleView(filterId)) {
		const auto filters = &session->data().chatsFilters();

		// Purple: a folder can ask to carry no count at all. Re-asked on every
		// resolution change, because the answer lives in settings.toml and the
		// tab is not rebuilt when only a flag on it moved.
		return rpl::single(rpl::empty) | rpl::then(
			Purple::ActiveChanges()
		) | rpl::map([=]() -> rpl::producer<Dialogs::UnreadState> {
			return Purple::QuietFolderUnread(session, filterId)
				? rpl::single(Dialogs::UnreadState())
				: MainListUnreadState(filters->chatsList(filterId));
		}) | rpl::flatten_latest();
	}
	return Purple::ViewUnreadStateValue(session, filterId);
}

rpl::producer<bool> IncludeMutedCounterFoldersValue() {
	using namespace Window::Notifications;
	return rpl::single(rpl::empty_value()) | rpl::then(
		Core::App().notifications().settingsChanged(
		) | rpl::filter(
			rpl::mappers::_1 == ChangeType::IncludeMuted
		) | rpl::to_empty
	) | rpl::map([] {
		return Core::App().settings().includeMutedCounterFolders();
	});
}

} // namespace Data
