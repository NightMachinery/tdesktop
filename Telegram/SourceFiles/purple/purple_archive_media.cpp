#include "purple/purple_archive_media.h"
#include "purple/purple_archive_files.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFileInfo>
#include <limits>

namespace Purple {
namespace {

QString Token(
	const AccountKey &account, const ArchiveSqlSecurity &security, const QString &hash) {
	return IsArchiveHash(hash)
			   ? ArchiveSqlToken(account, ArchiveIndexKind::Blob, hash.toUtf8(), security)
			   : QString();
}

QString Chunk(uint64_t index) {
	return QString::number(index).rightJustified(20, '0') + u".chunk"_q;
}

bool Read(const QString &directory, const AccountKey &account,
	const ArchiveSqlSecurity &security, const DesktopArchiveMediaCopy &copy,
	const std::function<bool(const QByteArray &)> &sink,
	const std::function<bool()> &current) {
	const auto token = Token(account, security, copy.hash);
	const auto native = DesktopArchiveIdentity(directory);
	if (token.isEmpty() || native.isEmpty() || !DesktopArchivePrivate(directory, true)
		|| !current()) {
		return false;
	}
	const auto manifest = DesktopArchiveRead(directory + u"/manifest"_q, 8192);
	const auto opened
		= manifest ? OpenArchiveEnvelope({account, security.storeId,
											 ArchiveRecordKind::Blob, token.toUtf8()},
						 security.keyId, *manifest, security.crypto)
				   : ArchiveEnvelopeResult();
	const auto decoded
		= opened ? DecodeArchiveRow(opened.bytes) : ArchiveRowDecodeResult();
	if (!decoded.row || !std::holds_alternative<ArchiveBlob>(decoded.row->value)) {
		return false;
	}
	const auto blob = std::get<ArchiveBlob>(decoded.row->value);
	if (blob.id != copy.hash || blob.size != copy.size
		|| blob.state != ArchiveBlobState::Complete) {
		return false;
	}
	const auto chunks = copy.size ? (copy.size - 1) / kArchiveMediaChunkBytes + 1 : 1;
	auto iterator = QDirIterator(
		directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
	auto entries = uint64_t(0);
	while (iterator.hasNext()) {
		const auto path = iterator.next();
		if (++entries > chunks + 1 || !DesktopArchivePrivate(path, false)) {
			return false;
		}
	}
	if (entries != chunks + 1) {
		return false;
	}
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	for (auto index = uint64_t(0); index != chunks; ++index) {
		if (!current() || DesktopArchiveIdentity(directory) != native) {
			return false;
		}
		const auto context = ArchiveMediaChunkContext(
			account, security.storeId, token, copy.size, index);
		const auto bytes = DesktopArchiveRead(
			directory + '/' + Chunk(index), 2 * kArchiveMediaChunkBytes);
		const auto size = ArchiveMediaChunkSize(copy.size, index);
		const auto plain = context && bytes ? OpenArchiveEnvelope(*context,
												  security.keyId, *bytes, security.crypto)
											: ArchiveEnvelopeResult();
		if (!plain || !size || plain.bytes.size() != *size) {
			return false;
		}
		hash.addData(plain.bytes);
		if (!current() || DesktopArchiveIdentity(directory) != native) {
			return false;
		}
		if (!sink(plain.bytes)) {
			return false;
		}
	}
	return current() && DesktopArchiveIdentity(directory) == native
		   && QString::fromLatin1(hash.result().toHex()) == copy.hash;
}

}

bool DesktopArchiveReadMedia(const QString &root, const AccountKey &account,
	const ArchiveSqlSecurity &security, const DesktopArchiveMediaCopy &copy,
	const std::function<bool(const QByteArray &)> &sink,
	const std::function<bool()> &current) {
	const auto token = Token(account, security, copy.hash);
	return !token.isEmpty() && sink && current
		   && Read(root + u"/blobs/"_q + token, account, security, copy, sink, current);
}

bool DesktopArchivePublishMedia(const QString &root, const AccountKey &account,
	const ArchiveSqlSecurity &security, QIODevice &source,
	const DesktopArchiveMediaCopy &copy, const std::function<bool()> &current) {
	const auto token = Token(account, security, copy.hash);
	if (token.isEmpty() || !current || !current() || !source.isReadable()
		|| copy.size > uint64_t(std::numeric_limits<qint64>::max())) {
		return false;
	}
	const auto final = root + u"/blobs/"_q + token;
	const auto discard = [](const QByteArray &) { return true; };
	if (QFileInfo::exists(final) || QFileInfo(final).isSymLink()) {
		return Read(final, account, security, copy, discard, current);
	}
	const auto random
		= security.crypto.random ? security.crypto.random(16) : std::nullopt;
	if (!random || random->size() != 16) {
		return false;
	}
	const auto staging = root + u"/staging/"_q + QString::fromLatin1(random->toHex());
	if (!DesktopArchiveDirectory(staging)) {
		return false;
	}
	const auto chunks = copy.size ? (copy.size - 1) / kArchiveMediaChunkBytes + 1 : 1;
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	for (auto index = uint64_t(0); index != chunks; ++index) {
		if (!current()) {
			return false;
		}
		const auto size = ArchiveMediaChunkSize(copy.size, index);
		if (!size) {
			return false;
		}
		auto plain = QByteArray();
		while (plain.size() < *size) {
			const auto bytes = source.read(*size - plain.size());
			if (bytes.isEmpty()) {
				return false;
			}
			plain += bytes;
		}
		hash.addData(plain);
		const auto context = ArchiveMediaChunkContext(
			account, security.storeId, token, copy.size, index);
		const auto sealed = context ? SealArchiveEnvelope(*context, security.keyId, plain,
										  security.crypto)
									: ArchiveEnvelopeResult();
		if (!sealed || !DesktopArchiveWrite(staging + '/' + Chunk(index), sealed.bytes)) {
			return false;
		}
	}
	if (!source.atEnd() || QString::fromLatin1(hash.result().toHex()) != copy.hash
		|| !current()) {
		return false;
	}
	const auto bytes = EncodeArchiveRow(
		{ArchiveBlob{copy.hash, copy.size, ArchiveBlobState::Complete}});
	const auto sealed
		= bytes ? SealArchiveEnvelope({account, security.storeId, ArchiveRecordKind::Blob,
										  token.toUtf8()},
					  security.keyId, *bytes, security.crypto)
				: ArchiveEnvelopeResult();
	if (!sealed || !DesktopArchiveWrite(staging + u"/manifest"_q, sealed.bytes)
		|| !Read(staging, account, security, copy, discard, current) || !current()
		|| !QDir().rename(staging, final)
		|| !DesktopArchiveSyncDirectory(root + u"/blobs"_q)
		|| !DesktopArchiveSyncDirectory(root + u"/staging"_q)) {
		return false;
	}
	return Read(final, account, security, copy, discard, current);
}

}
