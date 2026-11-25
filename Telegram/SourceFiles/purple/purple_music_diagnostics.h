#pragma once

#include <QtCore/QString>
#include <QtCore/QtTypes>

namespace Purple {

enum class MusicSkipReason {
	None,
	NotSong,
	ProtectedContent,
	InvalidMetadata,
	Unavailable,
	Other,
};

struct MusicDiagnosticsSnapshot {
	bool filtersLoaded = false;
	bool folderSelected = false;
	bool folderExists = false;
	QString folderName;
	bool waitingMainDialogs = false;
	bool waitingArchiveDialogs = false;
	int loadedMainDialogs = 0;
	int loadedArchiveDialogs = 0;
	bool offline = false;
	int totalChatsInFolder = 0;
	bool configuring = false;
	bool reading = false;
	bool scanning = false;
	int chatsToScan = 0;
	int chatsScanned = 0;
	qint64 retrySeconds = 0;
	int chatsFailed = 0;
	int serverSearchErrors = 0;
	QString lastServerError;
	int localWriteErrors = 0;
	QString indexError;
	int messagesInspected = 0;
	int tracksIndexedInScan = 0;
	int skippedProtected = 0;
	int skippedUnsupported = 0;
	int skippedInvalid = 0;
	int displayedTracks = 0;
	qint64 totalIndexedTracks = 0;
	QString searchQuery;
	bool scanIncomplete = false;
	QString transientNotice;
};

[[nodiscard]] QString DescribeMusicState(const MusicDiagnosticsSnapshot &state);

class MusicScanPacer final {
public:
	void sent(qint64 now);
	void succeeded();
	void restore(qint64 now, qint64 remaining);
	[[nodiscard]] qint64 retry(qint64 now, int code, const QString &type);
	[[nodiscard]] qint64 delay(qint64 now) const;
	[[nodiscard]] qint64 retrySeconds(qint64 now) const;

private:
	qint64 _nextRequest = 0;
	qint64 _retryAt = 0;
	int _failures = 0;

};

} // namespace Purple
