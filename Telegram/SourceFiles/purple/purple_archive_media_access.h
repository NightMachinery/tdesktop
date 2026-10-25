#pragma once

#include "purple/purple_archive_media_lifecycle.h"
#include "purple/purple_archive_media.h"
#include <atomic>
#include <memory>

namespace Purple {

struct ArchiveLeaseRevisionChange {
	uint64_t from = 0;
	uint64_t to = 0;
	bool leaseOnly = false;
};

[[nodiscard]] ArchiveLeaseRevisionChange DescribeArchiveLeaseCommit(
	const ArchiveTransaction &transaction);
void AdvanceArchiveLeaseRevision(std::optional<uint64_t> &revision,
	const ArchiveLeaseRevisionChange &change);

struct ArchiveMediaRequest {
	ArchiveMediaSource source;
	QString blob;
	std::function<void(ArchiveLeaseRevisionChange)> leaseCommitted;
};

struct DesktopArchiveMediaHandle {
	ArchiveMediaSource source;
	QString lease;
	QString blob;
	uint64_t size = 0;
	std::shared_ptr<std::atomic<bool>> active;
};

[[nodiscard]] std::optional<QByteArray> DesktopArchiveReadMediaRange(
	const QString &root, const AccountKey &account, const ArchiveSqlSecurity &security,
	const DesktopArchiveMediaCopy &copy, uint64_t offset, uint32_t length,
	const std::function<bool()> &current);
[[nodiscard]] bool DesktopArchiveExportMedia(const QString &root,
	const AccountKey &account, const ArchiveSqlSecurity &security,
	const DesktopArchiveMediaCopy &copy, const QString &path,
	const std::function<bool()> &current);

class ArchiveMediaAccess final : public std::enable_shared_from_this<ArchiveMediaAccess> {
public:
	using ReadDone = std::function<void(std::optional<QByteArray>)>;
	using SaveDone = std::function<void(bool)>;
	using Reader = std::function<void(uint64_t, uint32_t, ReadDone)>;
	using Saver = std::function<void(QString, SaveDone)>;
	ArchiveMediaAccess(DesktopArchiveMediaHandle handle, Reader read, Saver save,
		std::function<void()> release, std::function<bool()> current);
	~ArchiveMediaAccess();
	[[nodiscard]] bool current() const;
	[[nodiscard]] uint64_t size() const;
	void read(uint64_t offset, uint32_t length, ReadDone done);
	void save(QString path, SaveDone done);
	void close();

private:
	DesktopArchiveMediaHandle _handle;
	Reader _read;
	Saver _save;
	std::function<void()> _release;
	std::function<bool()> _current;
	bool _closed = false;

};

}
