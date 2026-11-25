#include "purple/purple_archive_menu.h"

#include "base/weak_ptr.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "purple/purple_archive_v2_boxes.h"
#include "purple/purple_archive_v2_service.h"
#include "purple/purple_picker.h"
#include "purple/purple_settings_editor.h"
#include "styles/style_menu_icons.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

namespace Purple {

void AddArchiveChatActions(not_null<Window::SessionController*> controller,
		Dialogs::EntryState request, const Window::PeerMenuCallback &callback) {
	const auto history = request.key.owningHistory();
	if (!history || request.key.sublist() || request.section == Dialogs::EntryState::Section::Scheduled
		|| request.section == Dialogs::EntryState::Section::ShortcutMessages || !PickerVisible(history)) return;
	const auto peer = ArchiveV2::Peer(history->peer->id);
	const auto name = PeerTokenText(peer);
	if (!name) return;
	const auto window = base::make_weak(controller.get());
	const auto lease = ArchiveV2::SessionLease(&controller->session());
	const auto current = [=] {
		const auto host = window.get();
		return host && lease && lease->live.load() && ArchiveV2::SessionLease(&host->session()) == lease
			&& host->window().maybeSession() == &host->session();
	};
	callback(tr::lng_purple_archive_history(tr::now), [=] {
		if (current()) OpenArchivedMessages(window.get(), peer, false);
	}, &st::menuIconViewReplies);
	callback(tr::lng_purple_archive_deleted(tr::now), [=] {
		if (current()) OpenArchivedMessages(window.get(), peer, true);
	}, &st::menuIconDelete);
	const auto change = [=](QString key, std::optional<SettingScalar> value) {
		if (!current()) return;
		const auto host = &window->window();
		const auto opening = CaptureWindowSettingsEditor(host, current);
		auto review = PrepareWindowSettingsEditorEdit(opening, std::vector<SettingsEdit>{
			SettingsEdit{ { u"archive"_q, u"chats"_q, *name, std::move(key) }, std::move(value) },
		});
		if (!review.review) {
			window->showToast(review.error);
			return;
		}
		RequestSettingsEditorApply(host, std::move(review.review), [=](SettingsEditorApplyResult result) {
			if (current() && !result.error.isEmpty()) window->showToast(result.error);
		});
	};
	const auto &policy = ArchiveV2::CurrentPolicy(&controller->session());
	const auto row = policy.chats.find(*name);
	const auto ordinary = row != policy.chats.end() && row->second.ordinaryOnArrival;
	callback(ordinary ? u"Keep descriptions for new files"_q : u"Save whole files on arrival"_q,
		[=] { change(u"ordinary_on_arrival"_q, SettingScalar(!ordinary)); }, &st::menuIconStorage);
	callback(u"Timed downloads · On open"_q, [=] {
		change(u"timed_download"_q, SettingScalar(u"on_open"_q));
	}, &st::menuIconStorage);
	callback(u"Timed downloads · Off"_q, [=] {
		change(u"timed_download"_q, SettingScalar(u"off"_q));
	}, &st::menuIconStorage);
	callback(u"Timed downloads · Inherit"_q, [=] {
		change(u"timed_download"_q, std::nullopt);
	}, &st::menuIconStorage);
}

}
