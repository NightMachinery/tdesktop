#include "purple/purple_music_source_roster.h"

#include "data/data_chat_filters.h"
#include "data/data_folder.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"

#include <optional>
#include <set>
#include <vector>

namespace Purple {

std::optional<std::vector<quint64>> CaptureMusicSourceRoster(
		Data::Session &data,
		const Data::ChatFilter &filter) {
	const auto archive = data.folderLoaded(Data::Folder::kId);
	const auto includeArchive = archive && !(filter.flags() & Data::ChatFilter::Flag::NoArchived);
	if (!data.chatsListLoaded() || (includeArchive && !data.chatsListLoaded(archive))) {
		return std::nullopt;
	}
	auto peers = std::set<quint64>();
	const auto add = [&](not_null<History*> history) {
		if (!filter.contains(history, true, true)) return;
		peers.emplace(history->peer->id.value);
		if (const auto migrated = history->migrateFrom()) {
			peers.emplace(migrated->peer->id.value);
		}
	};
	const auto addList = [&](Data::Folder *folder) {
		for (const auto row : *data.chatsList(folder)->indexed()) {
			if (const auto history = row->history()) {
				add(history);
			}
		}
	};
	addList(nullptr);
	if (includeArchive) addList(archive);
	for (const auto history : filter.always()) {
		add(history);
	}
	return std::vector<quint64>(peers.begin(), peers.end());
}

} // namespace Purple
