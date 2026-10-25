#include "purple/purple_archive_attention.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_sync_json.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>

namespace Purple {
namespace {
QString Row(const ArchiveStoreEpoch &epoch, const ArchiveSqlSecurity &security) {
	return ArchiveSqlToken(
		epoch.account, ArchiveIndexKind::Maintenance, "desktop-attention", security);
}
}

DesktopArchiveAttention ReadDesktopArchiveAttention(const QString &root,
	const ArchiveStoreEpoch &epoch, const ArchiveSqlSecurity &security) {
	auto result = DesktopArchiveAttention();
	const auto path = root + u"/retention-gap"_q;
	const auto info = QFileInfo(path);
	result.present = info.exists() || info.isSymLink();
	if (!result.present) {
		result.valid = true;
		return result;
	}
	const auto row = Row(epoch, security);
	if (row.isEmpty() || epoch.storeId != security.storeId) {
		return result;
	}
	const auto bytes = DesktopArchiveRead(path, 8192);
	const auto opened
		= bytes ? OpenArchiveEnvelope({epoch.account, epoch.storeId,
										  ArchiveRecordKind::CaptureGap, row.toUtf8()},
					  security.keyId, *bytes, security.crypto)
				: ArchiveEnvelopeResult();
	const auto canonical = opened ? CanonicalizeSyncJson(opened.bytes)
								  : SyncJsonResult{{}, SyncJsonErrorKind::Syntax};
	if (!canonical || canonical.canonical != opened.bytes) {
		return result;
	}
	const auto object = QJsonDocument::fromJson(opened.bytes).object();
	auto ok = false;
	const auto time = object.value(u"observedAt"_q).toString().toLongLong(&ok);
	if (object.size() != 5
		|| object.value(u"format"_q) != QJsonValue(u"desktop-archive-attention"_q)
		|| object.value(u"version"_q) != QJsonValue(1)
		|| !object.value(u"reason"_q).isString()
		|| !IsArchiveToken(object.value(u"reason"_q).toString())
		|| !object.value(u"observedAt"_q).isString() || !ok || time < 0
		|| QString::number(time) != object.value(u"observedAt"_q).toString()
		|| !object.value(u"id"_q).isString()
		|| !IsArchiveHash(object.value(u"id"_q).toString())) {
		return result;
	}
	result.gap = {epoch.account, epoch.storeId, object.value(u"id"_q).toString(), time,
		object.value(u"reason"_q).toString(), {}};
	result.valid = true;
	return result;
}

bool WriteDesktopArchiveAttention(const QString &root, const ArchiveStoreEpoch &epoch,
	const ArchiveSqlSecurity &security, const QString &reason, int64 time) {
	if (!IsArchiveToken(reason) || time < 0 || epoch.storeId != security.storeId
		|| epoch.removalPending) {
		return false;
	}
	const auto old = ReadDesktopArchiveAttention(root, epoch, security);
	if (old.present || !old.valid) {
		return old.valid;
	}
	const auto row = Row(epoch, security);
	if (row.isEmpty()) {
		return false;
	}
	const auto id = QString::fromLatin1(QCryptographicHash::hash(
		reason.toUtf8() + '/' + QByteArray::number(time), QCryptographicHash::Sha256)
			.toHex());
	const auto bytes = CanonicalizeSyncJson(
		QJsonDocument(QJsonObject{{u"format"_q, u"desktop-archive-attention"_q},
						  {u"version"_q, 1}, {u"id"_q, id}, {u"reason"_q, reason},
						  {u"observedAt"_q, QString::number(time)}})
			.toJson(QJsonDocument::Compact));
	const auto frame
		= bytes ? SealArchiveEnvelope({epoch.account, epoch.storeId,
										  ArchiveRecordKind::CaptureGap, row.toUtf8()},
					  security.keyId, bytes.canonical, security.crypto)
				: ArchiveEnvelopeResult();
	return frame && DesktopArchiveWrite(root + u"/retention-gap"_q, frame.bytes);
}

}
