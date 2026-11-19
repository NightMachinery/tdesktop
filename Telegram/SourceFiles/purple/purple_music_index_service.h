#pragma once

#include "data/data_types.h"
#include "purple/purple_music_index.h"
#include "rpl/producer.h"

class HistoryItem;
namespace Main { class Session; }

namespace Purple {

struct MusicIndexRoster {
	MusicIndexScope scope;
	std::vector<MusicSourceCursor> sources;
};

[[nodiscard]] FilterId MusicSourceFolder(not_null<Main::Session*> session);
void SetMusicSourceFolder(not_null<Main::Session*> session, FilterId folder);
[[nodiscard]] std::optional<MusicIndexEntry> CaptureMusicIndexEntry(
	not_null<HistoryItem*> item);
[[nodiscard]] rpl::producer<> MusicIndexArrivals(not_null<Main::Session*> session);

void ConfigureMusicIndex(
	not_null<Main::Session*> session,
	qint64 folder,
	std::vector<quint64> peers,
	Fn<void(std::optional<MusicIndexRoster>)> done);

void SaveMusicIndexPage(
	not_null<Main::Session*> session,
	MusicIndexScope scope,
	MusicSourceCursor expected,
	MusicSourceCursor next,
	std::vector<MusicIndexEntry> rows,
	Fn<void(bool)> done);

void ReadMusicIndexPage(
	not_null<Main::Session*> session,
	MusicIndexScope scope,
	QString query,
	std::vector<quint64> visiblePeers,
	std::optional<MusicIndexCursor> cursor,
	Fn<void(std::optional<std::vector<MusicIndexEntry>>)> done);

} // namespace Purple
