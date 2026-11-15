#pragma once

#include "purple/purple_playlist_download_state.h"
#include "base/basic_types.h"
#include "data/data_file_origin.h"
#include "rpl/lifetime.h"
#include <atomic>
#include <memory>

class DocumentData;
namespace Media::Streaming { class Reader; }
namespace Storage { class StreamedFileDownloader; }

namespace Purple {

class PlaylistOwnedTransfer final
	: public std::enable_shared_from_this<PlaylistOwnedTransfer> {
public:
	PlaylistOwnedTransfer(
		not_null<DocumentData*> document, Data::FileOrigin origin,
		QString path, QByteArray identity,
		std::shared_ptr<std::atomic<bool>> authority,
		Fn<void(PlaylistDownloadStatus)> done);
	~PlaylistOwnedTransfer();
	void start();
	void cancel();

private:
	void terminal(PlaylistDownloadStatus status);
	void release();

	QString _path;
	QByteArray _identity;
	uint64_t _size = 0;
	std::shared_ptr<std::atomic<bool>> _authority;
	Fn<void(PlaylistDownloadStatus)> _done;
	std::shared_ptr<void> _slot;
	std::shared_ptr<Media::Streaming::Reader> _reader;
	std::unique_ptr<Storage::StreamedFileDownloader> _loader;
	rpl::lifetime _updates;
	bool _unsupported = false;
	bool _admissionBusy = false;
	bool _queued = false;
	bool _started = false;

};

} // namespace Purple
