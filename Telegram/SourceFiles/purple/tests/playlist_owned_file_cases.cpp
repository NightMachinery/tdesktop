#include "purple/purple_playlist_download_files.h"
#include "purple/purple_sync_local_store.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <iostream>

int main(int argc, char *argv[]) {
	QCoreApplication app(argc, argv);
	using namespace Purple;
	auto failures = 0;
	const auto expect = [&](bool value, const char *name) {
		if (!value) { ++failures; std::cerr << name << '\n'; }
	};
	QTemporaryDir root;
	if (!root.isValid()) return 2;
	const auto token = QString(32, u'a');
	const auto created = CreatePlaylistStagingFile(root.path(), token);
#ifdef Q_OS_WIN
	expect(created.status == PlaylistDownloadStatus::Unsupported, "Windows refuses unsupported owned file operations");
#else
	expect(bool(created), "real private stage is exclusively created");
	if (!created) return 1;
	expect(!CreatePlaylistStagingFile(root.path(), token), "same stage token never truncates an existing file");
	const auto bytes = QByteArray("actual-complete-song-fixture");
	auto file = QFile(PlaylistStagingFilePath(root.path(), token));
	expect(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "fixture writes actual stage bytes");
	file.close();
	auto intent = PlaylistOwnedDownloadIntent{ u"pl-aaaaaaaaaaaaaaaa"_q, 1, u"mp3"_q,
		uint64_t(bytes.size()), created.file.identity,
		QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(), {} };
	expect(bool(InspectPlaylistStagingFile(root.path(), token, intent)), "complete staged bytes verify independently");
	const auto published = PublishPlaylistStagingFile(root.path(), token, intent);
	expect(bool(published) && !QFile::exists(PlaylistStagingFilePath(root.path(), token)),
		"verified publication preserves identity and retires stage name");
	expect(bool(PublishPlaylistStagingFile(root.path(), token, intent)),
		"durable intent recovers final publication before inventory commit");
	auto wrong = intent;
	wrong.sha256 = QByteArray(64, '0');
	expect(!PublishPlaylistStagingFile(root.path(), token, wrong), "recovery refuses a mismatched durable digest");
	const auto secondToken = QString(32, u'b');
	const auto second = CreatePlaylistStagingFile(root.path(), secondToken);
	expect(bool(second), "independent stage created");
	if (!second) return 1;
	file.setFileName(PlaylistStagingFilePath(root.path(), secondToken));
	expect(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "second stage filled");
	file.close();
	wrong = intent;
	wrong.identity = second.file.identity;
	expect(!PublishPlaylistStagingFile(root.path(), secondToken, wrong)
		&& InspectPlaylistKeptFile(root.path(), 1, u"mp3"_q, bytes.size()).file.identity == intent.identity,
		"existing destination is never replaced by another transfer");
	expect(DiscardPlaylistStagingFile(root.path(), secondToken, intent.identity) == PlaylistDownloadStatus::Changed,
		"discard cannot borrow another operation's identity");
	expect(DiscardPlaylistStagingFile(root.path(), secondToken, second.file.identity) == PlaylistDownloadStatus::Ready,
		"owned partial or complete stage can be explicitly discarded");
	expect(RemovePlaylistKeptFile(root.path(), 1, published.file) == PlaylistDownloadStatus::Ready,
		"verified owned final file is removable");
#endif
	return failures ? 1 : 0;
}
