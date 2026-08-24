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

// Whether this real folder is one the active preset asked to keep quiet. By
// title, like every other folder rule here - the file names folders the way the
// user sees them, not by an id the server assigned.
[[nodiscard]] bool QuietFolderId(
		not_null<Main::Session*> session,
		FilterId filterId) {
	const auto &quiet = Purple::QuietFolders();
	if (quiet.empty()) {
		return false;
	}
	const auto &list = session->data().chatsFilters().list();
	const auto i = ranges::find(list, filterId, &Data::ChatFilter::id);
	if (i == end(list)) {
		return false;
	}
	return ranges::any_of(quiet, [&](const QString &name) {
		return !i->title().text.text.compare(name, Qt::CaseInsensitive);
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
			return QuietFolderId(session, filterId)
				? rpl::single(Dialogs::UnreadState())
				: MainListUnreadState(filters->chatsList(filterId));
		}) | rpl::flatten_latest();
	}
	// Purple: an extra view holds chats and no folders, so the Archive row is
	// not in its total and there is nothing to take out of it. Subtracting the
	// archive anyway drives the tab's badge negative by exactly the archive's
	// own unread count, which is what it did.
	const auto index = IsPurpleView(filterId) ? PurpleViewIndex(filterId) : 0;
	if (index > 0) {
		return MainListUnreadState(session->data().purpleViewList(index));
	}

	// The preset's main view stands where All chats stands, so its tab is
	// counted the same way - the Archive row is in it and carries its own
	// count, and adding that to the tab would count it twice.
	return MainListUnreadState(IsPurpleView(filterId)
		? session->data().purpleViewList()
		: session->data().chatsList()
	) | rpl::map([=](const Dialogs::UnreadState &state) {
		return MainListMapUnreadState(session, state);
	});
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
