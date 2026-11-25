#pragma once

#include "data/data_types.h"
#include "purple/purple_music_diagnostics.h"
#include "purple/purple_music_index.h"
#include "rpl/producer.h"

class HistoryItem;
namespace Main { class Session; }

namespace Purple {

struct MusicIndexRoster {
	MusicIndexScope scope;
	std::vector<MusicSourceCursor> sources;
};

struct MusicIndexConfigureResult {
	std::optional<MusicIndexRoster> roster;
	QString error;
};

struct MusicIndexSaveResult {
	bool ok = false;
	QString error;
};

struct MusicIndexPageResult {
	bool ok = false;
	std::vector<MusicIndexEntry> rows;
	qint64 totalTracks = 0;
	QString error;
};

struct MusicIndexEntryInspection {
	std::optional<MusicIndexEntry> entry;
	MusicSkipReason skip = MusicSkipReason::None;
};

[[nodiscard]] FilterId MusicSourceFolder(not_null<Main::Session*> session);
void SetMusicSourceFolder(not_null<Main::Session*> session, FilterId folder);
[[nodiscard]] MusicIndexEntryInspection InspectMusicIndexEntry(
	not_null<HistoryItem*> item);
[[nodiscard]] std::optional<MusicIndexEntry> CaptureMusicIndexEntry(
	not_null<HistoryItem*> item);
[[nodiscard]] rpl::producer<> MusicIndexArrivals(not_null<Main::Session*> session);

void ConfigureMusicIndex(
	not_null<Main::Session*> session,
	qint64 folder,
	std::vector<quint64> peers,
	Fn<void(MusicIndexConfigureResult)> done);

void SaveMusicIndexPage(
	not_null<Main::Session*> session,
	MusicIndexScope scope,
	MusicSourceCursor expected,
	MusicSourceCursor next,
	std::vector<MusicIndexEntry> rows,
	Fn<void(MusicIndexSaveResult)> done);

void ReadMusicIndexPage(
	not_null<Main::Session*> session,
	MusicIndexScope scope,
	QString query,
	std::vector<quint64> visiblePeers,
	std::optional<MusicIndexCursor> cursor,
	bool tail,
	Fn<void(MusicIndexPageResult)> done);

} // namespace Purple
