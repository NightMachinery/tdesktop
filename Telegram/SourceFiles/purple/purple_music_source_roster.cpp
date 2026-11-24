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

MusicSourceRosterResult InspectMusicSourceRoster(
		Data::Session &data,
		const Data::ChatFilter &filter) {
	auto result = MusicSourceRosterResult();
	const auto archive = data.folderLoaded(Data::Folder::kId);
	result.loadedMainDialogs = int(data.chatsList(nullptr)->indexed()->size());
	result.loadedArchiveDialogs = archive
		? int(data.chatsList(archive)->indexed()->size())
		: 0;
	const auto needsDialogs = MusicFilterNeedsDialogLists(filter.flags());
	const auto includeArchive = needsDialogs
		&& archive
		&& !(filter.flags() & Data::ChatFilter::Flag::NoArchived);
	if (needsDialogs) {
		result.waitingMainDialogs = !data.chatsListLoaded();
		result.waitingArchiveDialogs = includeArchive
			&& !data.chatsListLoaded(archive);
		if (result.waitingMainDialogs || result.waitingArchiveDialogs) {
			return result;
		}
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
	if (needsDialogs) {
		addList(nullptr);
		if (includeArchive) addList(archive);
	}
	for (const auto history : filter.always()) {
		add(history);
	}
	result.ready = true;
	result.peers.assign(peers.begin(), peers.end());
	return result;
}

std::optional<std::vector<quint64>> CaptureMusicSourceRoster(
		Data::Session &data,
		const Data::ChatFilter &filter) {
	auto inspected = InspectMusicSourceRoster(data, filter);
	if (!inspected.ready) {
		return std::nullopt;
	}
	return std::move(inspected.peers);
}

} // namespace Purple
