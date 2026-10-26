#include "purple/purple_archive_maintenance.h"

#include "purple/purple_archive_codec.h"
#include "purple/purple_archive_envelope.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_archive_staging.h"

#include <algorithm>

namespace Purple {
namespace {

template <typename Type>
const Type *Value(const std::vector<ArchiveSqlValue> &values, size_t index) {
	return index < values.size() ? std::get_if<Type>(&values[index])
								 : nullptr;
}

std::optional<ArchiveStoredRow> CompactRow(const AccountKey &account,
	ArchiveRecordKind kind,
	const QString &id,
	const QByteArray &frame,
	const ArchiveSqlSecurity &security) {
	const auto opened
		= OpenArchiveEnvelope({account, security.storeId, kind, id.toUtf8()},
			security.keyId,
			frame,
			security.crypto);
	const auto decoded
		= opened ? DecodeArchiveRow(opened.bytes) : ArchiveRowDecodeResult();
	if (!decoded.row || decoded.status != ArchiveCodecStatus::Valid
		|| ArchiveRowKind(*decoded.row) != kind) {
		return {};
	}
	return ArchiveStoredRow{id, 0, *decoded.row};
}

QString MaintenanceScopeToken(const ArchiveSqlPurge &purge,
	const ArchiveSqlSecurity &security) {
	auto input = QByteArray::number(int(purge.kind)) + '/';
	if (purge.chat) {
		input += ArchiveSqlChatId(*purge.chat, purge.account, security)
					 .toUtf8();
		for (const auto &alias : purge.migrationAliases) {
			input += '/'
					 + ArchiveSqlChatId(alias, purge.account, security)
						   .toUtf8();
		}
	}
	return ArchiveSqlToken(purge.account,
		ArchiveIndexKind::Maintenance,
		QByteArray("scope/") + input,
		security);
}

DesktopArchiveMaintenanceRecord Inspect(DesktopArchiveSqlite &database,
	const ArchiveStoreEpoch &epoch,
	const ArchiveSqlSecurity &security) {
	auto result = DesktopArchiveMaintenanceRecord();
	if (epoch.account.userId == 0 || epoch.storeId != security.storeId
		|| !CanRunArchiveWork(epoch, epoch)
		|| DesktopArchiveAdmitSnapshot(database)
			   != ArchiveSqlStatus::Ready) {
		return result;
	}
	const auto metadata = database.Read(
		{u"SELECT maintenance_id FROM archive_meta WHERE singleton=1"_q,
			{},
			ArchiveSqlExpectation::OneRow});
	const auto id = metadata.ok && metadata.rows.size() == 1
							&& metadata.rows[0].size() == 1
						? Value<QString>(metadata.rows[0], 0)
						: nullptr;
	const auto header = id ? DesktopArchiveReadHeader(
								 database, epoch.account, security, *id)
						   : std::nullopt;
	auto journalQuery = ArchiveSqlStatement{
		u"SELECT operation_id,kind,scope_token,phase,cursor,envelope FROM maintenance WHERE singleton=1"_q,
		{},
		ArchiveSqlExpectation::Rows};
	journalQuery.rows.maximum = 1;
	const auto journal = database.Read(journalQuery);
	if (!id || !header || !journal.ok || !header->row.preserved.isEmpty()
		|| header->row.body) {
		return result;
	}
	result.header = std::get<ArchiveState>(header->row.value);
	if (id->isEmpty()) {
		result.valid = journal.rows.empty();
		return result;
	}
	if (!IsArchiveHash(*id) || journal.rows.size() != 1
		|| journal.rows[0].size() != 6) {
		return result;
	}
	const auto &row = journal.rows[0];
	const auto operation = Value<QString>(row, 0);
	const auto kind = Value<int64>(row, 1);
	const auto scope = Value<QString>(row, 2);
	const auto phase = Value<int64>(row, 3);
	const auto cursor = Value<QString>(row, 4);
	const auto frame = Value<QByteArray>(row, 5);
	if (!operation || *operation != *id || !kind || !scope || !phase
		|| !cursor || !frame) {
		return result;
	}
	const auto opened
		= OpenArchiveEnvelope({epoch.account,
								  epoch.storeId,
								  ArchiveRecordKind::Maintenance,
								  id->toUtf8()},
			security.keyId,
			*frame,
			security.crypto);
	const auto purge
		= opened ? DecodeArchiveSqlPurge(opened.bytes) : std::nullopt;
	if (!purge || purge->account != epoch.account
		|| purge->storeId != epoch.storeId
		|| purge->revision != result.header.revision
		|| purge->accountGeneration != result.header.accountGeneration
		|| ArchiveSqlToken(epoch.account,
			   ArchiveIndexKind::Maintenance,
			   purge->operationId.toUtf8(),
			   security)
			   != *operation
		|| int64(purge->kind) != *kind || int64(purge->phase) != *phase
		|| purge->cursor != *cursor
		|| MaintenanceScopeToken(*purge, security) != *scope) {
		return result;
	}
	result.purge = *purge;
	result.valid = true;
	return result;
}

bool AddCopies(DesktopArchiveSqlite &database,
	const ArchiveVersionSnapshot &snapshot,
	std::vector<ArchiveOwnedMediaCopy> &copies,
	const ArchiveSqlSecurity &security) {
	for (const auto &ref : snapshot.version.media) {
		if (ref.state == ArchiveMediaState::Complete) {
			continue;
		}
		const auto descriptor
			= std::find_if(snapshot.version.content.media.begin(),
				snapshot.version.content.media.end(),
				[&](const auto &media) {
					return media.identity == ref.identity;
				});
		if (descriptor == snapshot.version.content.media.end()) {
			return false;
		}
		const auto read = database.Read(ArchiveSqlFindOwnedMedia(
			snapshot.key.account, ref.identity, descriptor->kind, security));
		if (!read.ok || read.rows.size() > 1) {
			return false;
		}
		if (read.rows.empty()) {
			continue;
		}
		const auto &values = read.rows[0];
		const auto blobToken = Value<QString>(values, 0);
		const auto blobFrame = Value<QByteArray>(values, 1);
		const auto owner = Value<QString>(values, 2);
		const auto ownerFrame = Value<QByteArray>(values, 3);
		const auto position = Value<int64>(values, 4);
		if (values.size() != 5 || !blobToken || !blobFrame || !owner
			|| !ownerFrame || !position || *position < 0) {
			return false;
		}
		const auto blob = CompactRow(snapshot.key.account,
			ArchiveRecordKind::Blob,
			*blobToken,
			*blobFrame,
			security);
		const auto version = CompactRow(snapshot.key.account,
			ArchiveRecordKind::Version,
			*owner,
			*ownerFrame,
			security);
		if (!blob || !version) {
			return false;
		}
		const auto &ownedBlob = std::get<ArchiveBlob>(blob->row.value);
		const auto &ownedVersion
			= std::get<ArchiveVersionSnapshot>(version->row.value);
		if (ownedBlob.state != ArchiveBlobState::Complete
			|| ArchiveSqlToken(snapshot.key.account,
				   ArchiveIndexKind::Blob,
				   ownedBlob.id.toUtf8(),
				   security)
				   != *blobToken
			|| ownedVersion.key.account != snapshot.key.account
			|| ArchiveSqlChildId(ownedVersion.key,
				   ArchiveIndexKind::Version,
				   ownedVersion.version.id,
				   security)
				   != *owner) {
			return false;
		}
		copies.push_back({ref.identity,
			descriptor->kind,
			ownedBlob.id,
			*owner,
			*ownerFrame,
			*blobFrame,
			*position});
	}
	return true;
}

std::optional<std::vector<ArchiveSqlPurgeRow>> ReadPurgePage(
	DesktopArchiveSqlite &database,
	const ArchiveSqlPurge &purge,
	const ArchiveSqlSecurity &security) {
	auto result = std::vector<ArchiveSqlPurgeRow>();
	if (purge.phase == ArchivePurgePhase::Finish) {
		return result;
	}
	const auto read
		= database.Read(ArchiveSqlPurgePage(purge, 128, security));
	if (!read.ok) {
		return {};
	}
	const auto kind = purge.phase == ArchivePurgePhase::Versions
						  ? ArchiveRecordKind::Version
					  : purge.phase == ArchivePurgePhase::Messages
						  ? ArchiveRecordKind::Message
					  : purge.phase == ArchivePurgePhase::Jobs
						  ? ArchiveRecordKind::Job
						  : ArchiveRecordKind::Lease;
	auto evidenceBytes = size_t(0);
	for (const auto &values : read.rows) {
		const auto id = Value<QString>(values, 0);
		const auto frame = Value<QByteArray>(values, 1);
		const auto stored
			= values.size() == 2 && id && frame
				  ? CompactRow(purge.account, kind, *id, *frame, security)
				  : std::nullopt;
		if (!stored) {
			return {};
		}
		auto input = ArchiveSqlPurgeRow{*stored, *frame};
		if (kind == ArchiveRecordKind::Version) {
			if (!AddCopies(database,
					std::get<ArchiveVersionSnapshot>(stored->row.value),
					input.copies,
					security)) {
				return {};
			}
		} else if (kind == ArchiveRecordKind::Message
				   && purge.kind == ArchivePurgeKind::EditHistory) {
			const auto &message
				= std::get<ArchiveMessage>(stored->row.value);
			const auto headToken = database.Read(
				{u"SELECT head_version FROM messages WHERE message_key=?"_q,
					{*id},
					ArchiveSqlExpectation::OneRow});
			const auto token = headToken.ok && headToken.rows.size() == 1
									   && headToken.rows[0].size() == 1
								   ? Value<QString>(headToken.rows[0], 0)
								   : nullptr;
			const auto headRead = database.Read(
				ArchiveSqlPurgeHead(purge, message.key, security));
			const auto bytes = headRead.ok && headRead.rows.size() == 1
									   && headRead.rows[0].size() == 1
								   ? Value<QByteArray>(headRead.rows[0], 0)
								   : nullptr;
			const auto head = token && IsArchiveHash(*token) && bytes
								  ? CompactRow(purge.account,
										ArchiveRecordKind::Version,
										*id + '/' + *token,
										*bytes,
										security)
								  : std::nullopt;
			if (!head) {
				return {};
			}
			input.head = std::get<ArchiveVersionSnapshot>(head->row.value);
			input.headFrame = *bytes;
			if (!AddCopies(database, *input.head, input.copies, security)) {
				return {};
			}
		}
		auto bytes = size_t(input.frame.size() + input.headFrame.size());
		for (const auto &copy : input.copies) {
			bytes += size_t(copy.ownerFrame.size() + copy.blobFrame.size());
		}
		if (!result.empty()
			&& (evidenceBytes >= 12 * 1024 * 1024
				|| bytes > 12 * 1024 * 1024 - evidenceBytes)) {
			break;
		}
		evidenceBytes += bytes;
		result.push_back(std::move(input));
	}
	return result;
}

}

DesktopArchiveMaintenanceRecord InspectDesktopArchiveMaintenance(
	DesktopArchiveSqlite &database,
	const ArchiveStoreEpoch &epoch,
	const ArchiveSqlSecurity &security) {
	if (!database.BeginRead()) {
		return {};
	}
	const auto result = Inspect(database, epoch, security);
	return database.EndRead() ? result : DesktopArchiveMaintenanceRecord();
}

DesktopArchivePurgePreview PrepareDesktopArchivePurge(
	DesktopArchiveSqlite &database,
	const ArchiveStoreEpoch &epoch,
	ArchivePurgeKind kind,
	const ArchivePurgeScope &scope,
	const ArchiveSqlSecurity &security) {
	auto result = DesktopArchivePurgePreview();
	result.epoch = epoch;
	if (scope.account != epoch.account || !database.BeginRead()) {
		return result;
	}
	const auto record = Inspect(database, epoch, security);
	if (record.valid && !record.purge) {
		const auto counts
			= database.Read(ArchiveSqlScopeCounts({scope}, security));
		if (counts.ok && counts.rows.size() == 1
			&& counts.rows[0].size() == 7) {
			auto valid = true;
			for (auto i = size_t(0); i != result.counts.size(); ++i) {
				const auto value = Value<int64>(counts.rows[0], i);
				valid = valid && value && *value >= 0;
				result.counts[i] = value ? *value : 0;
			}
			if (valid) {
				result.confirmation = MakeArchivePurgeConfirmation(
					record.header, epoch, kind, scope);
			}
		}
	}
	if (!database.EndRead()) {
		return {};
	}
	result.status = result.confirmation ? ArchiveQueryStatus::Ready
										: ArchiveQueryStatus::Unavailable;
	return result;
}

bool BeginDesktopArchivePurge(DesktopArchiveSqlite &database,
	const ArchiveStoreEpoch &epoch,
	const ArchivePurgeConfirmation &confirmation,
	const QString &operation,
	const ArchiveSqlSecurity &security,
	const DesktopArchiveCommit &commit) {
	if (!commit || !database.BeginRead()) {
		return false;
	}
	const auto record = Inspect(database, epoch, security);
	const auto request = record.valid && !record.purge
							 ? ConfirmArchivePurge(record.header,
								   epoch,
								   confirmation.request.kind,
								   confirmation.request.scope,
								   confirmation)
							 : std::nullopt;
	auto generations = std::vector<ArchiveChatGeneration>();
	auto valid = request.has_value();
	if (request && request->scope.chat) {
		auto chats = request->scope.migrationAliases;
		chats.push_back(*request->scope.chat);
		for (const auto &chat : chats) {
			const auto generation = DesktopArchiveReadChatGeneration(
				database, epoch.account, chat, security);
			if (!generation) {
				valid = false;
				break;
			}
			generations.push_back({chat, *generation});
		}
	}
	if (!database.EndRead() || !valid) {
		return false;
	}
	const auto plan = BeginArchiveSqlPurge(
		record.header, *request, generations, operation, security);
	return plan.program && commit(plan.program);
}

DesktopArchiveMaintenanceStep ContinueDesktopArchivePurge(
	DesktopArchiveSqlite &database,
	const ArchiveStoreEpoch &epoch,
	const ArchiveSqlSecurity &security,
	const DesktopArchiveCommit &commit) {
	auto result = DesktopArchiveMaintenanceStep();
	if (!commit || !database.BeginRead()) {
		return result;
	}
	const auto record = Inspect(database, epoch, security);
	const auto rows = record.valid && record.purge
						  ? ReadPurgePage(database, *record.purge, security)
						  : std::nullopt;
	if (!database.EndRead() || !record.valid || !record.purge || !rows) {
		return result;
	}
	const auto plan
		= ContinueArchiveSqlPurge(*record.purge, *rows, security);
	if (!plan.program || plan.processedRows > rows->size()
		|| (!rows->empty() && !plan.processedRows)) {
		return result;
	}
	if (!commit(plan.program)) {
		return result;
	}
	result.valid = true;
	result.finished = plan.complete;
	result.processedRows = plan.processedRows;
	result.phase = plan.next.phase;
	result.releaseStagedJobs = plan.releaseStagedJobs;
	result.revokeReaders = plan.revokeReaders;
	return result;
}

DesktopArchiveGarbageStep ContinueDesktopArchiveGarbage(
	DesktopArchiveSqlite &database,
	const ArchiveStoreEpoch &epoch,
	const ArchiveSqlSecurity &security,
	const QString &root,
	const std::function<bool()> &current,
	QString &cancelledJobCursor,
	bool &cancelledJobsComplete,
	const DesktopArchiveCommit &commit) {
	if (!commit || !current || !current()) {
		return {};
	}
	const auto record
		= InspectDesktopArchiveMaintenance(database, epoch, security);
	if (!record.valid || record.purge) {
		return {};
	}
	if (!cancelledJobsComplete) {
		auto query = ArchiveSqlStatement{
			u"SELECT job_id,envelope FROM jobs WHERE state=3 AND job_id>? ORDER BY job_id LIMIT 1"_q,
			{cancelledJobCursor},
			ArchiveSqlExpectation::Rows};
		query.rows.maximum = 1;
		const auto read = database.Read(query);
		if (!read.ok) {
			return {};
		}
		if (read.rows.empty()) {
			cancelledJobsComplete = true;
		} else {
			const auto &values = read.rows[0];
			const auto id = Value<QString>(values, 0);
			const auto frame = Value<QByteArray>(values, 1);
			const auto row = values.size() == 2 && id && frame
									 && *id > cancelledJobCursor
								 ? CompactRow(epoch.account,
									   ArchiveRecordKind::Job,
									   *id,
									   *frame,
									   security)
								 : std::nullopt;
			if (!row) {
				return {};
			}
			const auto &job = std::get<ArchiveMediaJob>(row->row.value);
			if (job.key.account != epoch.account
				|| job.state != ArchiveJobState::Cancelled
				|| ArchiveSqlToken(epoch.account,
					   ArchiveIndexKind::Job,
					   job.id.toUtf8(),
					   security)
					   != *id) {
				return {};
			}
			if (job.expectedSize) {
				const auto binding
					= ArchiveStagingBinding{{epoch,
												job.key,
												job.version,
												job.revision,
												job.contentHash,
												job.mediaIdentity,
												job.generation},
						job.id,
						*job.expectedSize,
						job.expectedHash.isEmpty()
							? std::nullopt
							: std::optional(job.expectedHash)};
				const auto token
					= ArchiveStagingBindingId(binding, security);
				auto complete = false;
				if (!IsArchiveHash(token) || !current()
					|| !DesktopArchiveRemoveTreePage(
						root + u"/staging/stream-"_q + token, 64, complete)
					|| !current()) {
					return {};
				}
				if (!complete) {
					return {true, false};
				}
			}
			cancelledJobCursor = *id;
			return {true, false};
		}
	}
	const auto read = database.Read(ArchiveSqlGarbagePage(1));
	if (!read.ok) {
		return {};
	}
	if (read.rows.empty()) {
		return {current(), true};
	}
	const auto &values = read.rows[0];
	const auto token = Value<QString>(values, 0);
	const auto frame = Value<QByteArray>(values, 1);
	const auto row = values.size() == 2 && token && frame
						 ? CompactRow(epoch.account,
							   ArchiveRecordKind::Blob,
							   *token,
							   *frame,
							   security)
						 : std::nullopt;
	if (!row || !row->row.preserved.isEmpty() || row->row.body) {
		return {};
	}
	auto blob = std::get<ArchiveBlob>(row->row.value);
	if (!IsArchiveHash(blob.id)
		|| ArchiveSqlToken(epoch.account,
			   ArchiveIndexKind::Blob,
			   blob.id.toUtf8(),
			   security)
			   != *token
		|| (blob.state != ArchiveBlobState::Complete
			&& blob.state != ArchiveBlobState::GarbagePending)) {
		return {};
	}
	if (blob.state == ArchiveBlobState::Complete) {
		const auto pending
			= PlanArchiveSqlGarbage(record.header, {blob}, security);
		if (!pending.program || !commit(pending.program)) {
			return {};
		}
		blob.state = ArchiveBlobState::GarbagePending;
	}
	const auto evidence = database.Read(
		{u"SELECT envelope FROM blobs WHERE blob_id=? AND state=1 "
		 "AND NOT EXISTS(SELECT 1 FROM media_refs WHERE blob_id=?) "
		 "AND NOT EXISTS(SELECT 1 FROM leases WHERE blob_id=?) "
		 "AND EXISTS(SELECT 1 FROM archive_meta WHERE singleton=1 AND maintenance_id='')"_q,
			{*token, *token, *token},
			ArchiveSqlExpectation::OneRow});
	const auto pendingFrame = evidence.ok && evidence.rows.size() == 1
									  && evidence.rows[0].size() == 1
								  ? Value<QByteArray>(evidence.rows[0], 0)
								  : nullptr;
	const auto pendingRow = pendingFrame ? CompactRow(epoch.account,
											   ArchiveRecordKind::Blob,
											   *token,
											   *pendingFrame,
											   security)
										 : std::nullopt;
	if (!pendingRow || std::get<ArchiveBlob>(pendingRow->row.value) != blob
		|| !current()) {
		return {};
	}
	auto complete = false;
	if (!DesktopArchiveRemoveTreePage(
			root + u"/blobs/"_q + *token, 64, complete)
		|| !current()) {
		return {};
	}
	if (!complete) {
		return {true, false};
	}
	const auto fresh
		= InspectDesktopArchiveMaintenance(database, epoch, security);
	if (!fresh.valid || fresh.purge) {
		return {};
	}
	const auto finish
		= CompleteArchiveSqlGarbage(fresh.header, {blob}, security);
	return {finish.program && commit(finish.program) && current(), false};
}

}
