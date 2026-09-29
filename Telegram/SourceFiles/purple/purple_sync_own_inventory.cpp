/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_own_inventory.h"

#include "purple/purple_config_payload.h"

#include <QtCore/QCryptographicHash>

#include <algorithm>
#include <map>

namespace Purple {
namespace {

[[nodiscard]] bool OwnCanonicalConfigRecord(
		const QByteArray &bytes,
		const SyncLocalState &state,
		std::optional<uint64_t> expectedSeq = std::nullopt) {
	const auto parsed = ParseSyncEnvelope(bytes);
	if (!parsed || !parsed.header
		|| InspectConfigPayload(parsed).status != ConfigPayloadStatus::Valid) {
		return false;
	}
	const auto written = SerializeSyncEnvelope(parsed.envelope);
	const auto &header = *parsed.header;
	return written && written.canonical == bytes
		&& header.stream == u"config"_q
		&& header.writerInstall == state.install
		&& header.writerDevice == state.createdDevice
		&& header.space == state.space
		&& (!expectedSeq || header.seq == *expectedSeq);
}

[[nodiscard]] QString RecordHash(const QByteArray &bytes) {
	return QString::fromLatin1(QCryptographicHash::hash(
		bytes, QCryptographicHash::Sha256).toHex());
}

} // namespace

SyncOwnInventoryResult ReconcileOwnConfigInventory(
		const SyncLocalState &state,
		const SyncAccountInventoryResult &inventory,
		uint64_t expectedAccountUserId,
		const QByteArray &stagedCanonicalRecord) {
	auto result = SyncOwnInventoryResult();
	if (!expectedAccountUserId
		|| inventory.accountUserId != expectedAccountUserId) {
		result.status = SyncOwnInventoryStatus::NeedsReview;
		return result;
	}
	if (!inventory.scan.complete()
		|| !inventory.read
		|| inventory.read->status == SyncCandidateReadStatus::Incomplete
		|| !inventory.directory.complete
		|| inventory.status == SyncAccountInventoryStatus::Incomplete) {
		return result;
	}
	if (inventory.status != SyncAccountInventoryStatus::Complete
		|| !inventory.read->complete()
		|| inventory.directory.unreadableCandidate
		|| inventory.directory.messageIdCollision
		|| !inventory.directory.selectedSpace
		|| !inventory.directory.publishableSpace
		|| *inventory.directory.selectedSpace != state.space
		|| *inventory.directory.publishableSpace != state.space
		|| !SerializeSyncLocalState(state)) {
		result.status = SyncOwnInventoryStatus::NeedsReview;
		return result;
	}
	if (state.config.pendingSeq != 0) {
		if (stagedCanonicalRecord.isEmpty()
			|| !OwnCanonicalConfigRecord(stagedCanonicalRecord,
				state, state.config.pendingSeq)) {
			result.status = SyncOwnInventoryStatus::NeedsReview;
			return result;
		}
		const auto staged = ParseSyncEnvelope(stagedCanonicalRecord);
		const auto issued = std::find_if(
			state.config.issuedRecords.begin(),
			state.config.issuedRecords.end(),
			[&](const auto &record) {
				return record.seq == state.config.pendingSeq;
			});
		if (staged.envelope.document.value(u"payload_sha256"_q).toString()
			!= state.config.ownHash
			|| issued == state.config.issuedRecords.end()
			|| issued->recordHash != RecordHash(stagedCanonicalRecord)) {
			result.status = SyncOwnInventoryStatus::NeedsReview;
			return result;
		}
	}

	auto recordsBySeq = std::map<uint64_t, QByteArray>();
	auto headId = int32_t(0);
	for (const auto &record : inventory.read->records) {
		if (!record.header
			|| record.header->stream != u"config"_q
			|| record.header->writerInstall != state.install
			|| record.header->space != state.space) {
			continue;
		}
		if (record.id <= 0
			|| record.status != SyncCandidateStatus::Valid
			|| !OwnCanonicalConfigRecord(record.bytes, state)) {
			if (record.header->writerDevice != state.createdDevice) {
				result.status = SyncOwnInventoryStatus::CloneDetected;
				result.cloneVerdict = SyncCloneVerdict::DeviceMismatch;
				return result;
			}
			result.status = SyncOwnInventoryStatus::NeedsReview;
			return result;
		}
		const auto parsed = ParseSyncEnvelope(record.bytes);
		if (parsed.header->seq != record.header->seq
			|| parsed.header->space != record.header->space
			|| parsed.header->stream != record.header->stream
			|| parsed.header->writerInstall != record.header->writerInstall
			|| parsed.header->writerDevice != record.header->writerDevice) {
			result.status = SyncOwnInventoryStatus::NeedsReview;
			return result;
		}
		const auto seq = parsed.header->seq;
		if (state.config.pendingSeq == seq
			&& record.bytes != stagedCanonicalRecord) {
			result.status = SyncOwnInventoryStatus::NeedsReview;
			return result;
		}
		const auto [position, inserted] = recordsBySeq.try_emplace(
			seq, record.bytes);
		if (!inserted && position->second != record.bytes) {
			result.status = SyncOwnInventoryStatus::NeedsReview;
			return result;
		}
		if (state.config.pendingSeq == seq
			&& record.bytes == stagedCanonicalRecord
			&& (!result.pendingMessageId || record.id < *result.pendingMessageId)) {
			result.pendingMessageId = record.id;
		}
		if (seq == recordsBySeq.rbegin()->first) {
			if (!headId || seq > result.observation.seq) {
				result.duplicateHeadMessageIds.clear();
				headId = record.id;
			} else if (record.id < headId) {
				result.duplicateHeadMessageIds.push_back(headId);
				headId = record.id;
			} else {
				result.duplicateHeadMessageIds.push_back(record.id);
			}
			result.observation.seq = seq;
		}
	}
	if (recordsBySeq.empty()) {
		result.observation.kind = OwnRecordObservationKind::Absent;
		result.cloneVerdict = CheckSyncClone(
			state, state.createdDevice, SyncLocalStream::Config,
			result.observation);
		result.status = (result.cloneVerdict == SyncCloneVerdict::NoClone)
			? SyncOwnInventoryStatus::Absent
			: SyncOwnInventoryStatus::CloneDetected;
		return result;
	}

	const auto &headBytes = recordsBySeq.rbegin()->second;
	const auto head = ParseSyncEnvelope(headBytes);
	result.observation.kind = OwnRecordObservationKind::Present;
	result.observation.seq = head.header->seq;
	result.observation.payloadHash = head.envelope.document.value(
		u"payload_sha256"_q).toString();
	result.head = { headId, RecordHash(headBytes), true };
	std::sort(result.duplicateHeadMessageIds.begin(),
		result.duplicateHeadMessageIds.end());
	result.cloneVerdict = CheckSyncClone(
		state, state.createdDevice, SyncLocalStream::Config,
		result.observation);
	result.status = (result.cloneVerdict != SyncCloneVerdict::NoClone)
		? SyncOwnInventoryStatus::CloneDetected
		: result.pendingMessageId
		? SyncOwnInventoryStatus::PendingFound
		: SyncOwnInventoryStatus::Present;
	return result;
}

} // namespace Purple
