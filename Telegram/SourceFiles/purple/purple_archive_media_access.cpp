#include "purple/purple_archive_media_access.h"

#include "purple/purple_archive_files.h"
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <algorithm>
#include <limits>

namespace Purple {

ArchiveLeaseRevisionChange DescribeArchiveLeaseCommit(
		const ArchiveTransaction &transaction) {
	auto expected = transaction.expected;
	auto next = transaction.next;
	const auto from = expected.revision;
	const auto to = next.revision;
	expected.leases.clear();
	next.leases.clear();
	expected.revision = next.revision = 0;
	return {from, to, transaction && from != std::numeric_limits<uint64_t>::max()
		&& to == from + 1 && expected == next};
}

void AdvanceArchiveLeaseRevision(std::optional<uint64_t> &revision,
		const ArchiveLeaseRevisionChange &change) {
	if (revision && *revision == change.from && change.leaseOnly
		&& change.from != std::numeric_limits<uint64_t>::max()
		&& change.to == change.from + 1) {
		revision = change.to;
	}
}

std::optional<QByteArray> DesktopArchiveReadMediaRange(const QString &root,
	const AccountKey &account, const ArchiveSqlSecurity &security,
	const DesktopArchiveMediaCopy &copy, uint64_t offset, uint32_t length,
	const std::function<bool()> &current) {
	if (!current || !current() || length > kArchiveMediaChunkBytes
		|| offset > copy.size || uint64_t(length) > copy.size - offset
		|| !IsArchiveHash(copy.hash)) {
		return {};
	}
	const auto token = ArchiveSqlToken(account, ArchiveIndexKind::Blob, copy.hash.toUtf8(), security);
	const auto directory = root + u"/blobs/"_q + token;
	const auto native = DesktopArchiveIdentity(directory);
	if (token.isEmpty() || native.isEmpty() || !DesktopArchivePrivate(directory, true)) {
		return {};
	}
	const auto frame = DesktopArchiveRead(directory + u"/manifest"_q, 8192);
	const auto opened = frame ? OpenArchiveEnvelope({account, security.storeId,
		ArchiveRecordKind::Blob, token.toUtf8()}, security.keyId, *frame, security.crypto) : ArchiveEnvelopeResult();
	const auto decoded = opened ? DecodeArchiveRow(opened.bytes) : ArchiveRowDecodeResult();
	const auto blob = decoded.row ? std::get_if<ArchiveBlob>(&decoded.row->value) : nullptr;
	if (!blob || blob->id != copy.hash || blob->size != copy.size || blob->state != ArchiveBlobState::Complete) {
		return {};
	}
	auto result = QByteArray();
	const auto first = offset / kArchiveMediaChunkBytes;
	const auto last = length ? (offset + length - 1) / kArchiveMediaChunkBytes : first;
	for (auto index = first; length && index <= last; ++index) {
		if (!current() || DesktopArchiveIdentity(directory) != native) {
			return {};
		}
		const auto context = ArchiveMediaChunkContext(account, security.storeId, token, copy.size, index);
		const auto frame = DesktopArchiveRead(directory + '/' + QString::number(index).rightJustified(20, '0')
			+ u".chunk"_q, kArchiveMediaChunkBytes + 4096);
		const auto plain = context && frame ? OpenArchiveEnvelope(*context, security.keyId,
			*frame, security.crypto) : ArchiveEnvelopeResult();
		const auto expected = ArchiveMediaChunkSize(copy.size, index);
		if (!plain || !expected || uint64_t(plain.bytes.size()) != *expected) {
			return {};
		}
		const auto begin = index * kArchiveMediaChunkBytes;
		const auto from = std::max(begin, offset);
		const auto till = std::min(begin + *expected, offset + length);
		result += plain.bytes.mid(from - begin, till - from);
	}
	return current() && DesktopArchiveIdentity(directory) == native && result.size() == length
		? std::optional(std::move(result)) : std::nullopt;
}

bool DesktopArchiveExportMedia(const QString &root, const AccountKey &account,
	const ArchiveSqlSecurity &security, const DesktopArchiveMediaCopy &copy,
	const QString &path, const std::function<bool()> &current) {
	if (path.isEmpty() || !current || !current()) {
		return false;
	}
	auto file = QSaveFile(path);
	file.setDirectWriteFallback(false);
	if (!file.open(QIODevice::WriteOnly)
		|| !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
		return false;
	}
	const auto read = DesktopArchiveReadMedia(root, account, security, copy,
		[&](const QByteArray &bytes) { return current() && file.write(bytes) == bytes.size(); }, current);
	if (!read || !file.flush() || !current()) {
		file.cancelWriting();
		return false;
	}
	return file.commit();
}

ArchiveMediaAccess::ArchiveMediaAccess(DesktopArchiveMediaHandle handle, Reader read,
	Saver save, std::function<void()> release, std::function<bool()> current)
: _handle(std::move(handle))
, _read(std::move(read))
, _save(std::move(save))
, _release(std::move(release))
, _current(std::move(current)) {
}

ArchiveMediaAccess::~ArchiveMediaAccess() {
	close();
}

bool ArchiveMediaAccess::current() const {
	return !_closed && _handle.active && _handle.active->load() && _current && _current();
}

uint64_t ArchiveMediaAccess::size() const {
	return _handle.size;
}

void ArchiveMediaAccess::read(uint64_t offset, uint32_t length, ReadDone done) {
	if (!done) {
		return;
	}
	if (!current() || !_read || length > kArchiveMediaChunkBytes
		|| offset > size() || uint64_t(length) > size() - offset) {
		close();
		done({});
		return;
	}
	_read(offset, length, [weak = weak_from_this(), length, done = std::move(done)](std::optional<QByteArray> bytes) mutable {
		const auto live = weak.lock();
		if (!live || !live->current() || !bytes || bytes->size() != length) {
			if (live) {
				live->close();
			}
			done({});
			return;
		}
		done(std::move(bytes));
	});
}

void ArchiveMediaAccess::save(QString path, SaveDone done) {
	if (!done) {
		return;
	}
	if (!current() || !_save || path.isEmpty()) {
		done(false);
		return;
	}
	_save(std::move(path), [weak = weak_from_this(), done = std::move(done)](bool ok) mutable {
		const auto live = weak.lock();
		const auto current = live && live->current();
		if (live && !current) {
			live->close();
		}
		done(ok && current);
	});
}

void ArchiveMediaAccess::close() {
	if (_closed) {
		return;
	}
	_closed = true;
	if (_handle.active) {
		_handle.active->store(false);
	}
	if (_release) {
		_release();
	}
}

}
