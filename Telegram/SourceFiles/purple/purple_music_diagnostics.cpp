#include "purple/purple_music_diagnostics.h"

#include "purple/purple_types.h"

#include <QtCore/QStringList>
#include <algorithm>

namespace Purple {
namespace {

QString FolderLabel(const MusicDiagnosticsSnapshot &state) {
	return state.folderName.trimmed().isEmpty()
		? u"the selected folder"_q
		: u"\"%1\""_q.arg(state.folderName.trimmed());
}

QString ChatWord(int count) {
	return count == 1 ? u"chat"_q : u"chats"_q;
}

QString TrackWord(qint64 count) {
	return count == 1 ? u"track"_q : u"tracks"_q;
}

QString SkippedSummary(const MusicDiagnosticsSnapshot &state) {
	auto parts = QStringList();
	if (state.skippedProtected > 0) {
		parts.push_back(u"%1 protected"_q.arg(state.skippedProtected));
	}
	if (state.skippedUnsupported > 0) {
		parts.push_back(u"%1 unsupported audio"_q.arg(state.skippedUnsupported));
	}
	if (state.skippedInvalid > 0) {
		parts.push_back(u"%1 invalid metadata"_q.arg(state.skippedInvalid));
	}
	return parts.join(u", "_q);
}

QString ErrorDetail(const MusicDiagnosticsSnapshot &state) {
	if (!state.lastServerError.trimmed().isEmpty()) {
		return u": %1"_q.arg(state.lastServerError.trimmed());
	}
	return QString();
}

} // namespace

QString DescribeMusicState(const MusicDiagnosticsSnapshot &state) {
	if (!state.transientNotice.isEmpty()) {
		return state.transientNotice;
	}
	if (!state.filtersLoaded) {
		return state.offline
			? u"Offline. Waiting for chat folders to load."_q
			: u"Loading chat folders…"_q;
	}
	if (!state.folderSelected) {
		return u"Choose a chat folder to find your music."_q;
	}
	const auto folder = FolderLabel(state);
	if (!state.folderExists) {
		return u"The selected music source folder is no longer available. Choose another chat folder."_q;
	}
	if (state.waitingMainDialogs || state.waitingArchiveDialogs) {
		auto waiting = QStringList();
		if (state.waitingMainDialogs) {
			waiting.push_back(u"main chat list (%1 loaded)"_q.arg(state.loadedMainDialogs));
		}
		if (state.waitingArchiveDialogs) {
			waiting.push_back(u"archived chats (%1 loaded)"_q.arg(state.loadedArchiveDialogs));
		}
		const auto prefix = state.offline ? u"Offline. "_q : QString();
		return u"%1Loading %2 for %3…"_q.arg(
			prefix,
			waiting.join(u" and "_q),
			folder);
	}
	if (!state.indexError.isEmpty()) {
		return u"Could not open the music library (%1). Try refreshing."_q
			.arg(state.indexError);
	}
	if (state.configuring) {
		return u"Opening music library for %1 (%2 %3)…"_q.arg(
			folder,
			QString::number(state.totalChatsInFolder),
			ChatWord(state.totalChatsInFolder));
	}
	if (state.totalChatsInFolder <= 0) {
		return u"No chats in %1. Add chats or channels with music to this folder."_q
			.arg(folder);
	}
	const auto knownTracks = std::max<qint64>(
		state.totalIndexedTracks,
		std::max<qint64>(state.displayedTracks, state.tracksIndexedInScan));
	if (state.scanning) {
		auto details = QStringList();
		if (state.chatsToScan > 0) {
			details.push_back(u"%1/%2 %3 scanned"_q.arg(
				QString::number(state.chatsScanned),
				QString::number(state.chatsToScan),
				ChatWord(state.chatsToScan)));
		}
		details.push_back(u"%1 %2 found"_q.arg(
			QString::number(knownTracks),
			TrackWord(knownTracks)));
		if (state.chatsFailed > 0) {
			details.push_back(u"%1 failed%2"_q.arg(
				QString::number(state.chatsFailed),
				ErrorDetail(state)));
		}
		const auto prefix = state.offline ? u"Offline (paused) · "_q : QString();
		return u"%1Indexing %2: %3…"_q.arg(
			prefix,
			folder,
			details.join(u", "_q));
	}
	if (state.reading && state.displayedTracks == 0) {
		return u"Loading tracks from %1…"_q.arg(folder);
	}

	const auto query = state.searchQuery.trimmed();

	if (state.displayedTracks == 0) {
		if (!query.isEmpty() && state.totalIndexedTracks > 0) {
			return u"No tracks match \"%1\" (filtered out %2 indexed %3 in %4)."_q.arg(
				query,
				QString::number(state.totalIndexedTracks),
				TrackWord(state.totalIndexedTracks),
				folder);
		}
		if (state.serverSearchErrors > 0 || state.localWriteErrors > 0 || state.scanIncomplete) {
			const auto failed = std::max(state.chatsFailed, 1);
			const auto total = std::max(state.chatsToScan, failed);
			if (state.offline) {
				return u"Offline. Could not index %1 (%2 of %3 %4 failed%5). Refresh when back online."_q.arg(
					folder,
					QString::number(failed),
					QString::number(total),
					ChatWord(total),
					ErrorDetail(state));
			}
			if (state.serverSearchErrors > 0) {
				return u"Server search failed for %1 of %2 %3 in %4%5. Refresh to try again."_q.arg(
					QString::number(failed),
					QString::number(total),
					ChatWord(total),
					folder,
					ErrorDetail(state));
			}
			if (state.localWriteErrors > 0) {
				return u"Could not save indexed tracks for %1 of %2 %3 in %4. Refresh to try again."_q.arg(
					QString::number(failed),
					QString::number(total),
					ChatWord(total),
					folder);
			}
			return u"Some chats in %1 could not be indexed. Refresh to try again."_q
				.arg(folder);
		}
		const auto skippedTotal = state.skippedProtected
			+ state.skippedUnsupported
			+ state.skippedInvalid;
		if (state.messagesInspected > 0 && state.totalIndexedTracks == 0 && skippedTotal > 0) {
			return u"No playable tracks in %1 (%2 audio messages inspected across %3 %4; skipped: %5)."_q.arg(
				folder,
				QString::number(state.messagesInspected),
				QString::number(state.totalChatsInFolder),
				ChatWord(state.totalChatsInFolder),
				SkippedSummary(state));
		}
		if (!query.isEmpty()) {
			return u"No tracks match \"%1\" (0 tracks indexed across %2 %3 in %4)."_q.arg(
				query,
				QString::number(state.totalChatsInFolder),
				ChatWord(state.totalChatsInFolder),
				folder);
		}
		if (state.offline) {
			return u"Offline. No cached tracks in %1 (%2 %3 in scope). Refresh when back online."_q.arg(
				folder,
				QString::number(state.totalChatsInFolder),
				ChatWord(state.totalChatsInFolder));
		}
		return u"No music tracks found in %1 (0 tracks indexed across %2 %3)."_q.arg(
			folder,
			QString::number(state.totalChatsInFolder),
			ChatWord(state.totalChatsInFolder));
	}

	if (state.serverSearchErrors > 0 || state.localWriteErrors > 0 || state.scanIncomplete) {
		const auto failed = std::max(state.chatsFailed, 1);
		const auto total = std::max(state.chatsToScan, failed);
		if (state.offline) {
			return u"Offline. Showing %1 cached %2; %3 of %4 %5 could not be updated%6."_q.arg(
				QString::number(state.displayedTracks),
				TrackWord(state.displayedTracks),
				QString::number(failed),
				QString::number(total),
				ChatWord(total),
				ErrorDetail(state));
		}
		if (state.serverSearchErrors > 0) {
			return u"Some chats could not be indexed (%1 of %2 failed%3). Refresh to try again."_q.arg(
				QString::number(failed),
				QString::number(total),
				ErrorDetail(state));
		}
		return u"Some chats in %1 could not be indexed. Refresh to try again."_q
			.arg(folder);
	}
	return QString();
}

} // namespace Purple
