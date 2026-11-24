#include "data/data_chat_filters.h"
#include "purple/purple_music_diagnostics.h"
#include "purple/purple_music_source_roster.h"

#include <QtCore/QCoreApplication>
#include <cstdlib>
#include <iostream>

namespace {

void Check(bool passed, const char *label, const QString &actual = {}) {
	if (!passed) {
		std::cerr << "FAIL: " << label;
		if (!actual.isEmpty()) {
			std::cerr << " -> " << actual.toStdString();
		}
		std::cerr << '\n';
		std::exit(1);
	}
}

Purple::MusicDiagnosticsSnapshot ReadyMusicFolder() {
	auto s = Purple::MusicDiagnosticsSnapshot();
	s.filtersLoaded = true;
	s.folderSelected = true;
	s.folderExists = true;
	s.folderName = u"Music"_q;
	s.totalChatsInFolder = 4;
	s.visibleChatsInFolder = 4;
	return s;
}

} // namespace

int main(int argc, char **argv) {
	const auto app = QCoreApplication(argc, argv);

	// 1. Explicit-only Music folders (no category flags) must not block on
	// main/archive dialog list loading (reproduces the archive deadlock root cause).
	using Flag = Data::ChatFilter::Flag;
	Check(!Purple::MusicFilterNeedsDialogLists(Flag(0)),
		"explicit-only folder does not require full dialog lists");
	Check(!Purple::MusicFilterNeedsDialogLists(Flag::Chatlist | Flag::NoArchived),
		"chatlist folder without category flags does not require full dialog lists");
	Check(Purple::MusicFilterNeedsDialogLists(Flag::Channels),
		"category-flagged folder requires dialog lists");
	Check(Purple::MusicFilterNeedsDialogLists(Flag::Groups | Flag::NoArchived),
		"groups folder requires dialog lists");

	// 2. Folder not loaded / offline / not selected / deleted folder.
	{
		auto s = Purple::MusicDiagnosticsSnapshot();
		const auto loading = Purple::DescribeMusicState(s);
		Check(loading.contains(u"chat folders"_q, Qt::CaseInsensitive), "folders loading", loading);
		s.offline = true;
		const auto offlineFolders = Purple::DescribeMusicState(s);
		Check(offlineFolders.contains(u"Offline"_q) && offlineFolders.contains(u"folders"_q),
			"offline waiting for folders", offlineFolders);
	}
	{
		auto s = Purple::MusicDiagnosticsSnapshot();
		s.filtersLoaded = true;
		const auto choose = Purple::DescribeMusicState(s);
		Check(choose.contains(u"Choose a chat folder"_q), "no folder selected", choose);
		s.folderSelected = true;
		s.folderExists = false;
		const auto missing = Purple::DescribeMusicState(s);
		Check(missing.contains(u"no longer available"_q), "deleted folder explained", missing);
	}

	// 3. Waiting for main and/or archive dialog lists with counts and offline notice.
	{
		auto s = ReadyMusicFolder();
		s.waitingMainDialogs = true;
		s.waitingArchiveDialogs = true;
		s.loadedMainDialogs = 120;
		s.loadedArchiveDialogs = 20;
		const auto text = Purple::DescribeMusicState(s);
		Check(text.contains(u"main chat list (120 loaded)"_q)
			&& text.contains(u"archived chats (20 loaded)"_q)
			&& text.contains(u"\"Music\""_q),
			"waiting dialog lists reports both counts and folder", text);
		s.offline = true;
		const auto offlineText = Purple::DescribeMusicState(s);
		Check(offlineText.startsWith(u"Offline."_q), "waiting dialog lists shows offline", offlineText);
	}

	// 4. Local index open/read error shows concrete reason.
	{
		auto s = ReadyMusicFolder();
		s.indexError = u"library.sqlite failed ownership or 0600/0700 privacy check"_q;
		const auto text = Purple::DescribeMusicState(s);
		Check(text.contains(s.indexError) && text.contains(u"Could not open the music library"_q),
			"index error reports exact reason", text);
	}

	// 5. Empty folder (no chats in scope) and Work Mode hiding all chats.
	{
		auto s = ReadyMusicFolder();
		s.totalChatsInFolder = 0;
		s.visibleChatsInFolder = 0;
		const auto empty = Purple::DescribeMusicState(s);
		Check(empty.contains(u"No chats in \"Music\""_q), "empty folder explained", empty);

		s.workModeActive = true;
		s.workModePreset = u"Focus"_q;
		const auto emptyWork = Purple::DescribeMusicState(s);
		Check(emptyWork.contains(u"Work Mode (Focus)"_q), "empty folder under work mode notes preset", emptyWork);
	}
	{
		auto s = ReadyMusicFolder();
		s.totalChatsInFolder = 5;
		s.visibleChatsInFolder = 0;
		s.hiddenByWorkMode = 5;
		s.workModeActive = true;
		s.workModePreset = u"Deep Work"_q;
		const auto hiddenAll = Purple::DescribeMusicState(s);
		Check(hiddenAll.contains(u"All 5 chats in \"Music\" are hidden by Work Mode (Deep Work)"_q),
			"all chats hidden by Work Mode explained", hiddenAll);
	}

	// 6. Scanning progress with counts, partial Work Mode hidden count, and offline pause.
	{
		auto s = ReadyMusicFolder();
		s.totalChatsInFolder = 5;
		s.visibleChatsInFolder = 3;
		s.hiddenByWorkMode = 2;
		s.workModeActive = true;
		s.workModePreset = u"Focus"_q;
		s.scanning = true;
		s.chatsToScan = 3;
		s.chatsScanned = 1;
		s.tracksIndexedInScan = 14;
		const auto progress = Purple::DescribeMusicState(s);
		Check(progress.contains(u"1/3 chats scanned"_q)
			&& progress.contains(u"14 tracks found"_q)
			&& progress.contains(u"2 of 5 chats hidden by Work Mode (Focus)"_q),
			"scanning progress reports counts and hidden chats", progress);
		s.offline = true;
		const auto paused = Purple::DescribeMusicState(s);
		Check(paused.contains(u"Offline (paused)"_q), "offline scanning state", paused);
	}

	// 7. Reproduces blank-screen bug: scan finishes with 0 tracks must NOT be empty string.
	{
		auto s = ReadyMusicFolder();
		s.scanning = false;
		s.displayedTracks = 0;
		s.totalIndexedTracks = 0;
		const auto text = Purple::DescribeMusicState(s);
		Check(!text.isEmpty() && text.contains(u"0 tracks indexed across 4 chats"_q),
			"finished empty scan explains zero tracks instead of blank screen", text);
	}

	// 8. Filtered out by search query reports filter and indexed track count.
	{
		auto s = ReadyMusicFolder();
		s.displayedTracks = 0;
		s.totalIndexedTracks = 87;
		s.searchQuery = u"bach cello"_q;
		const auto filtered = Purple::DescribeMusicState(s);
		Check(filtered.contains(u"\"bach cello\""_q)
			&& filtered.contains(u"87 indexed tracks"_q),
			"search filter empty state shows filter and total count", filtered);
	}

	// 9. Server search error & offline search error & local write error.
	{
		auto s = ReadyMusicFolder();
		s.chatsToScan = 4;
		s.chatsFailed = 2;
		s.serverSearchErrors = 2;
		s.lastServerError = u"CHANNEL_PRIVATE"_q;
		s.scanIncomplete = true;
		const auto err = Purple::DescribeMusicState(s);
		Check(err.contains(u"Server search failed for 2 of 4 chats"_q)
			&& err.contains(u"CHANNEL_PRIVATE"_q),
			"server search error reports count and error code", err);

		s.offline = true;
		const auto offErr = Purple::DescribeMusicState(s);
		Check(offErr.startsWith(u"Offline."_q) && offErr.contains(u"CHANNEL_PRIVATE"_q),
			"offline search error reports offline and code", offErr);

		s.offline = false;
		s.serverSearchErrors = 0;
		s.localWriteErrors = 1;
		s.chatsFailed = 1;
		const auto writeErr = Purple::DescribeMusicState(s);
		Check(writeErr.contains(u"Could not save indexed tracks for 1 of 4 chats"_q),
			"local write error reported distinctly", writeErr);
	}

	// 10. All inspected audio messages skipped during qualification/metadata parsing.
	{
		auto s = ReadyMusicFolder();
		s.messagesInspected = 12;
		s.skippedProtected = 7;
		s.skippedUnsupported = 3;
		s.skippedInvalid = 2;
		const auto skipped = Purple::DescribeMusicState(s);
		Check(skipped.contains(u"12 audio messages inspected"_q)
			&& skipped.contains(u"7 protected"_q)
			&& skipped.contains(u"3 unsupported audio"_q)
			&& skipped.contains(u"2 invalid metadata"_q),
			"skipped tracks report counts and reasons", skipped);
	}

	// 11. Non-empty results stay clean unless partial Work Mode hiding or errors exist.
	{
		auto s = ReadyMusicFolder();
		s.displayedTracks = 10;
		s.totalIndexedTracks = 10;
		Check(Purple::DescribeMusicState(s).isEmpty(), "healthy non-empty list has no status banner");
		s.hiddenByWorkMode = 1;
		s.totalChatsInFolder = 5;
		s.visibleChatsInFolder = 4;
		s.workModeActive = true;
		s.workModePreset = u"Focus"_q;
		const auto partial = Purple::DescribeMusicState(s);
		Check(partial.contains(u"1 of 5 chats hidden by Work Mode (Focus)"_q),
			"partial Work Mode hiding warns even with tracks", partial);
	}

	// 12. User-controlled strings with QString placeholders (%1/%2/%3) must not corrupt formatting.
	{
		auto s = ReadyMusicFolder();
		s.folderName = u"100%2"_q;
		s.displayedTracks = 0;
		s.totalIndexedTracks = 12;
		s.searchQuery = u"%1 %3"_q;
		const auto filtered = Purple::DescribeMusicState(s);
		Check(filtered == u"No tracks match \"%1 %3\" (filtered out 12 indexed tracks in \"100%2\")."_q,
			"placeholder tokens in query and folder stay literal when filtered", filtered);

		s.totalIndexedTracks = 0;
		const auto emptyQuery = Purple::DescribeMusicState(s);
		Check(emptyQuery == u"No tracks match \"%1 %3\" (0 tracks indexed across 4 chats in \"100%2\")."_q,
			"placeholder tokens in query and folder stay literal with 0 tracks", emptyQuery);

		s.searchQuery.clear();
		s.chatsToScan = 4;
		s.chatsFailed = 1;
		s.serverSearchErrors = 1;
		s.lastServerError = u"ERR_%1"_q;
		s.scanIncomplete = true;
		s.hiddenByWorkMode = 1;
		s.totalChatsInFolder = 5;
		s.workModeActive = true;
		s.workModePreset = u"%2Preset%1"_q;
		const auto errWithHidden = Purple::DescribeMusicState(s);
		Check(errWithHidden == u"Server search failed for 1 of 4 chats in \"100%2\": ERR_%1. Refresh to try again. 1 of 5 chats hidden by Work Mode (%2Preset%1)."_q,
			"placeholder tokens in folder, error and preset stay literal", errWithHidden);

		s.visibleChatsInFolder = 0;
		s.hiddenByWorkMode = 5;
		s.serverSearchErrors = 0;
		s.scanIncomplete = false;
		const auto allHidden = Purple::DescribeMusicState(s);
		Check(allHidden == u"All 5 chats in \"100%2\" are hidden by Work Mode (%2Preset%1). Switch presets or exempt this folder to browse its music."_q,
			"placeholder tokens in folder and preset stay literal when all hidden", allHidden);
	}

	std::cout << "Music diagnostics and roster checks passed\n";
	return 0;
}
