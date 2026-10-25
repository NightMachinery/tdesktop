#include "purple/purple_archive_menu.h"

#include "base/weak_ptr.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "purple/purple_archive_capture.h"
#include "purple/purple_archive_deleted_box.h"
#include "purple/purple_config.h"
#include "purple/purple_picker.h"
#include "styles/style_menu_icons.h"
#include "window/window_session_controller.h"

#include <algorithm>

namespace Purple {

ArchiveViewScope ArchiveDeletedScope(not_null<PeerData*> peer, std::optional<int64> topic) {
	auto result = ArchiveViewScope();
	result.messages.account = SessionAccountKey(&peer->owner().session());
	result.messages.chat = DesktopArchiveChat(peer->id);
	const auto addAlias = [&](PeerData *alias) {
		if (!alias) {
			return;
		}
		const auto key = DesktopArchiveChat(alias->id);
		if (key != *result.messages.chat
			&& std::find(result.messages.migrationAliases.begin(), result.messages.migrationAliases.end(), key)
				== result.messages.migrationAliases.end()) {
			result.messages.migrationAliases.push_back(key);
		}
	};
	addAlias(peer->migrateFrom());
	addAlias(peer->migrateTo());
	result.collection = ArchiveCollection::Received;
	result.representation = u"original"_q;
	if (topic) {
		result.topic = ArchiveTopicScope{*result.messages.chat, *topic};
	}
	return result;
}

bool SameArchiveViewScope(const ArchiveViewScope &a, const ArchiveViewScope &b) {
	return a.messages.account == b.messages.account && a.messages.chat == b.messages.chat
		&& a.messages.migrationAliases == b.messages.migrationAliases
		&& bool(a.topic) == bool(b.topic)
		&& (!a.topic || (a.topic->chat == b.topic->chat && a.topic->id == b.topic->id))
		&& a.collection == b.collection && a.representation == b.representation;
}

void AddArchiveChatActions(not_null<Window::SessionController*> controller,
		Dialogs::EntryState request, const Window::PeerMenuCallback &callback) {
	const auto history = request.key.owningHistory();
	if (!history || request.key.sublist() || request.section == Dialogs::EntryState::Section::Scheduled
		|| request.section == Dialogs::EntryState::Section::ShortcutMessages || !PickerVisible(history)) {
		return;
	}
	const auto peer = history->peer;
	const auto topic = request.key.topic();
	const auto topicId = topic ? std::optional<int64>(topic->rootId().bare) : std::nullopt;
	const auto scope = ArchiveDeletedScope(peer, topicId);
	const auto window = base::make_weak(controller.get());
	const auto session = base::make_weak(&controller->session());
	const auto current = [=] {
		return window && session && &window->session() == session.get() && PickerVisible(peer)
			&& SameArchiveViewScope(scope, ArchiveDeletedScope(peer, topicId));
	};
	callback(tr::lng_purple_archive_deleted(tr::now), [=] {
		if (current()) {
			ShowArchiveDeleted(window.get(), scope, peer->name(), current);
		}
	}, &st::menuIconDelete);
}

}
