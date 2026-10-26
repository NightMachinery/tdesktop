#include "purple/purple_archive_queries.h"

#include "purple/purple_archive_codec.h"
#include "purple/purple_archive_envelope.h"
#include <algorithm>
#include <limits>
#include <tuple>

namespace Purple {
namespace {

template <typename Type>
const Type *Value(const std::vector<ArchiveSqlValue> &values, size_t index) {
	return index < values.size() ? std::get_if<Type>(&values[index]) : nullptr;
}

ArchiveQueryStatus QueryStatus(ArchiveSqlStatus status) {
	return status == ArchiveSqlStatus::Ready ? ArchiveQueryStatus::Ready
		: status == ArchiveSqlStatus::ReadOnly ? ArchiveQueryStatus::ReadOnly
		: status == ArchiveSqlStatus::SchemaMismatch ? ArchiveQueryStatus::SchemaMismatch
		: ArchiveQueryStatus::Unavailable;
}

bool CursorBefore(const ArchiveTranscriptCursor &a, const ArchiveTranscriptCursor &b) {
	return std::tie(a.date, a.messageId, a.messageKey) < std::tie(b.date, b.messageId, b.messageKey);
}

}

std::optional<ArchiveStoredRow> DesktopArchiveDecodeRow(
		DesktopArchiveSqlite &database, const AccountKey &account,
		const ArchiveSqlSecurity &security, ArchiveRecordKind kind,
		const QString &id, const QByteArray &frame, int64 position) {
	const auto opened
		= OpenArchiveEnvelope({account, security.storeId, kind, id.toUtf8()},
			security.keyId, frame, security.crypto);
	if (!opened) {
		return {};
	}
	const auto decoded = DecodeArchiveRow(opened.bytes);
	if (!decoded.row || decoded.status != ArchiveCodecStatus::Valid
		|| ArchiveRowKind(*decoded.row) != kind) {
		return {};
	}
	auto row = *decoded.row;
	if (row.body) {
		const auto result = database.Read(ArchiveSqlReadBodyChunks(row, security));
		if (!result.ok) {
			return {};
		}
		auto chunks = std::vector<QByteArray>();
		for (const auto &values : result.rows) {
			const auto bytes = Value<QByteArray>(values, 0);
			if (!bytes || values.size() != 1) {
				return {};
			}
			chunks.push_back(*bytes);
		}
		const auto full = RestoreArchiveSqlVersionBody(row, chunks, security);
		if (!full) {
			return {};
		}
		row = *full;
	}
	return ArchiveStoredRow{id, position, row};
}

ArchiveSqlStatus DesktopArchiveAdmitSnapshot(DesktopArchiveSqlite &database) {
	const auto schema = database.Read(
		{u"SELECT schema_version FROM archive_meta WHERE singleton=1"_q, {},
			ArchiveSqlExpectation::OneRow});
	const auto version
		= schema.ok && schema.rows.size() == 1 && schema.rows.front().size() == 1
			  ? Value<int64>(schema.rows.front(), 0)
			  : nullptr;
	if (!version) {
		return ArchiveSqlStatus::Invalid;
	}
	const auto status = CheckArchiveSqlVersion(*version);
	if (status != ArchiveSqlStatus::Ready) {
		return status;
	}
	const auto metadata = database.Read(
		{u"SELECT schema_fingerprint FROM archive_meta WHERE singleton=1"_q, {},
			ArchiveSqlExpectation::OneRow});
	const auto fingerprint = metadata.ok && metadata.rows.size() == 1
									 && metadata.rows.front().size() == 1
								 ? Value<QString>(metadata.rows.front(), 0)
								 : nullptr;
	const auto actual = database.Read(ArchiveSqlReadSchema());
	if (!fingerprint || !actual.ok) {
		return ArchiveSqlStatus::SchemaMismatch;
	}
	auto definitions = std::vector<QString>();
	for (const auto &row : actual.rows) {
		const auto sql = row.size() == 1 ? Value<QString>(row, 0) : nullptr;
		if (!sql) {
			return ArchiveSqlStatus::SchemaMismatch;
		}
		definitions.push_back(*sql);
	}
	return CheckArchiveSqlSchema(*version, *fingerprint, definitions);
}

std::optional<ArchiveStoredRow> DesktopArchiveReadHeader(
		DesktopArchiveSqlite &database, const AccountKey &account,
		const ArchiveSqlSecurity &security, const QString &requiredMaintenance,
		bool *readFailure) {
	if (readFailure) *readFailure = true;
	const auto result = database.Read(
		{u"SELECT schema_version,account_token,store_id,revision,generation,maintenance_id,envelope,schema_fingerprint FROM archive_meta WHERE singleton=1"_q,
			{}, ArchiveSqlExpectation::OneRow});
	if (!result.ok || result.rows.size() != 1 || result.rows.front().size() != 8) {
		return {};
	}
	const auto &values = result.rows.front();
	const auto schema = Value<int64>(values, 0);
	const auto accountToken = Value<QString>(values, 1);
	const auto store = Value<QString>(values, 2);
	const auto revision = Value<QString>(values, 3);
	const auto generation = Value<QString>(values, 4);
	const auto maintenance = Value<QString>(values, 5);
	const auto bytes = Value<QByteArray>(values, 6);
	const auto fingerprint = Value<QString>(values, 7);
	if (!schema || *schema != kArchiveSqlSchemaVersion || !fingerprint
		|| *fingerprint != ArchiveSqlExpectedSchemaFingerprint() || !accountToken
		|| *accountToken
			   != ArchiveSqlToken(
				   account, ArchiveIndexKind::Account, "account", security)
		|| !store || *store != security.storeId || !revision || !generation
		|| !maintenance || !bytes) {
		return {};
	}
	const auto row = DesktopArchiveDecodeRow(database, account, security, ArchiveRecordKind::Account, u"account"_q, *bytes);
	if (!row) {
		return {};
	}
	const auto &header = std::get<ArchiveState>(row->row.value);
	if (header.account != account || QString::number(header.revision) != *revision
		|| QString::number(header.accountGeneration) != *generation) {
		return {};
	}
	if (readFailure) *readFailure = false;
	return *maintenance == requiredMaintenance ? row : std::nullopt;
}

std::optional<uint64_t> DesktopArchiveReadChatGeneration(
		DesktopArchiveSqlite &database, const AccountKey &account,
		const ArchiveChatKey &chat, const ArchiveSqlSecurity &security) {
	const auto read = database.Read(ArchiveSqlReadChatEpoch(account, chat, security));
	if (!read.ok || read.rows.size() > 1) {
		return {};
	}
	if (read.rows.empty()) {
		return uint64_t(0);
	}
	const auto &values = read.rows.front();
	const auto projection = Value<QString>(values, 0);
	const auto frame = Value<QByteArray>(values, 1);
	const auto row = values.size() == 2 && projection && frame
		? DesktopArchiveDecodeRow(database, account, security, ArchiveRecordKind::Generation,
			ArchiveSqlChatId(chat, account, security), *frame) : std::nullopt;
	if (!row) {
		return {};
	}
	const auto &generation = std::get<ArchiveChatGeneration>(row->row.value);
	return generation.chat == chat && QString::number(generation.generation) == *projection
		? std::optional(generation.generation) : std::nullopt;
}

DesktopArchiveDeletedQuery::DesktopArchiveDeletedQuery(QString id, ArchiveStoreEpoch epoch,
		ArchiveSqlSecurity security, ArchiveViewScope scope, ArchiveTranscriptOrder order)
: _id(std::move(id))
, _epoch(std::move(epoch))
, _security(std::move(security))
, _scope(std::move(scope))
, _order(order)
, _lastUse(std::chrono::steady_clock::now()) {
}

ArchiveQueryStatus DesktopArchiveDeletedQuery::Open(const QString &path, std::function<bool()> active) {
	_database.SetActive(std::move(active));
	if (!IsArchiveToken(_id) || !CanRunArchiveWork(_epoch, _epoch) || _security.storeId != _epoch.storeId
		|| _scope.messages.account != _epoch.account || !_scope.messages.chat
		|| ArchiveSqlDeletedView(_scope, 1, _security, {}, _order).sql.isEmpty()
		|| !_database.Open(path, false, true).ok || !_database.BeginRead()) {
		return ArchiveQueryStatus::Unavailable;
	}
	const auto admission = DesktopArchiveAdmitSnapshot(_database);
	if (admission != ArchiveSqlStatus::Ready) {
		return QueryStatus(admission);
	}
	const auto header = DesktopArchiveReadHeader(_database, _epoch.account, _security);
	const auto counts = _database.Read(ArchiveSqlScopeCounts(_scope, _security));
	if (!header || !counts.ok || counts.rows.size() != 1 || counts.rows.front().size() != 7) {
		return ArchiveQueryStatus::Unavailable;
	}
	_header = std::get<ArchiveState>(header->row.value);
	for (auto i = size_t(0); i != _counts.size(); ++i) {
		const auto value = Value<int64>(counts.rows.front(), i);
		if (!value || *value < 0) {
			return ArchiveQueryStatus::Unavailable;
		}
		_counts[i] = *value;
	}
	auto chats = _scope.messages.migrationAliases;
	chats.push_back(*_scope.messages.chat);
	for (const auto &chat : chats) {
		const auto generation = DesktopArchiveReadChatGeneration(_database, _epoch.account, chat, _security);
		if (!generation) {
			return ArchiveQueryStatus::Unavailable;
		}
		_generations.push_back({chat, *generation});
	}
	return ArchiveQueryStatus::Ready;
}

std::optional<ArchiveDeletedEntry> DesktopArchiveDeletedQuery::DecodeEntry(
		const std::vector<ArchiveSqlValue> &values) {
	const auto token = Value<QString>(values, 0);
	const auto date = Value<int64>(values, 1);
	const auto id = Value<int64>(values, 2);
	const auto frame = Value<QByteArray>(values, 3);
	const auto head = Value<QString>(values, 4);
	if (values.size() != 6 || !token || !date || *date < 0 || !id || !*id || !frame || !head) {
		return {};
	}
	const auto shell = DesktopArchiveDecodeRow(_database, _epoch.account, _security,
		ArchiveRecordKind::Message, *token, *frame);
	if (!shell) {
		return {};
	}
	auto result = ArchiveDeletedEntry();
	result.message = std::get<ArchiveMessage>(shell->row.value);
	result.shellMetadata = shell->row.preserved;
	result.cursor = {*date, *id, *token};
	if (result.message.key.messageId != *id
		|| ArchiveSqlMessageId(result.message.key, _security) != *token) {
		return {};
	}
	if (!head->isEmpty()) {
		const auto bytes = Value<QByteArray>(values, 5);
		const auto version = bytes ? DesktopArchiveDecodeRow(_database, _epoch.account, _security,
			ArchiveRecordKind::Version, *token + '/' + *head, *bytes) : std::nullopt;
		if (!version) {
			return {};
		}
		result.head = std::get<ArchiveVersionSnapshot>(version->row.value);
		result.headMetadata = version->row.preserved;
		const auto &content = result.head->version.content;
		result.contentHash = ArchiveContentHash(content);
		result.displayContent.text = content.text.left(8193);
		result.displayContent.caption = content.caption.left(8193);
		if (content.rich.isObject()) {
			auto rich = QJsonObject();
			for (const auto &key : {u"title"_q, u"description"_q, u"text"_q}) {
				rich.insert(key, content.rich.toObject().value(key).toString().left(8193));
			}
			result.displayContent.rich = rich;
		}
		if (ArchiveSqlChildId(result.message.key, ArchiveIndexKind::Version,
			result.head->version.id, _security) != version->id) {
			return {};
		}
	} else if (!std::holds_alternative<std::nullptr_t>(values[5])) {
		return {};
	}
	if (!ArchiveDeletedViewMatches(_scope, result.message, result.head ? &*result.head : nullptr)) {
		return {};
	}
	const auto generation = std::find_if(_generations.begin(), _generations.end(), [&](const auto &entry) {
		return entry.chat == result.message.key.chat;
	});
	if (generation == _generations.end()) {
		return {};
	}
	result.generation = {_header.accountGeneration, generation->generation};
	return result;
}

bool DesktopArchiveDeletedQuery::ReadUsage() {
	if (_logicalComplete) {
		return true;
	}
	const auto page = _database.Read(ArchiveSqlScopeBlobs(_scope, 128, _security, _blobCursor));
	if (!page.ok) {
		return false;
	}
	for (const auto &values : page.rows) {
		const auto token = Value<QString>(values, 0);
		const auto bytes = Value<QString>(values, 1);
		const auto frame = Value<QByteArray>(values, 2);
		const auto row = values.size() == 3 && token && frame && bytes && *token > _blobCursor
			? DesktopArchiveDecodeRow(_database, _epoch.account, _security,
				ArchiveRecordKind::Blob, *token, *frame) : std::nullopt;
		if (!row) {
			return false;
		}
		const auto &blob = std::get<ArchiveBlob>(row->row.value);
		if (blob.state != ArchiveBlobState::Complete || QString::number(blob.size) != *bytes
			|| ArchiveSqlToken(_epoch.account, ArchiveIndexKind::Blob, blob.id.toUtf8(), _security) != *token
			|| blob.size > std::numeric_limits<uint64_t>::max() - _logicalBytes) {
			return false;
		}
		_logicalBytes += blob.size;
		_blobCursor = *token;
	}
	_logicalComplete = page.rows.size() < 128;
	return true;
}

ArchiveDeletedPage DesktopArchiveDeletedQuery::Read(int limit) {
	auto result = ArchiveDeletedPage();
	result.epoch = _epoch;
	result.view = _id;
	result.active = _active;
	if (Expired() || limit < 1 || limit > 128) {
		result.status = ArchiveQueryStatus::Stale;
		return result;
	}
	const auto read = _more ? _database.Read(ArchiveSqlDeletedView(_scope, limit, _security, _cursor, _order))
		: DesktopArchiveSqlResult{true};
	if (!read.ok || !ReadUsage()) {
		result.reason = u"deleted-query-unreadable"_q;
		return result;
	}
	auto previous = _cursor;
	for (const auto &values : read.rows) {
		const auto entry = DecodeEntry(values);
		if (!entry || (previous && !(_order == ArchiveTranscriptOrder::Chronological
			? CursorBefore(*previous, entry->cursor) : CursorBefore(entry->cursor, *previous)))) {
			result.entries.clear();
			result.reason = u"deleted-row-unreadable"_q;
			return result;
		}
		previous = entry->cursor;
		result.entries.push_back(*entry);
	}
	_cursor = previous;
	_more = read.rows.size() == size_t(limit);
	_lastUse = std::chrono::steady_clock::now();
	result.status = ArchiveQueryStatus::Ready;
	result.header = _header;
	result.counts = _counts;
	result.logicalBytes = _logicalBytes;
	result.logicalComplete = _logicalComplete;
	result.more = _more;
	result.refreshAvailable = _refreshAvailable;
	return result;
}

bool DesktopArchiveDeletedQuery::Current(DesktopArchiveSqlite &writer) {
	if (Expired() || !writer.BeginRead()) {
		return false;
	}
	const auto valid = [&] {
		const auto header = DesktopArchiveReadHeader(writer, _epoch.account, _security);
		if (!header || std::get<ArchiveState>(header->row.value).accountGeneration != _header.accountGeneration) {
			return false;
		}
		_refreshAvailable = std::get<ArchiveState>(header->row.value).revision != _header.revision;
		for (const auto &generation : _generations) {
			const auto live = DesktopArchiveReadChatGeneration(writer, _epoch.account, generation.chat, _security);
			if (!live || *live != generation.generation) {
				return false;
			}
		}
		return true;
	}();
	return writer.EndRead() && valid;
}

bool DesktopArchiveDeletedQuery::Expired() const {
	return !_active->load() || std::chrono::steady_clock::now() - _lastUse >= std::chrono::seconds(30);
}

bool DesktopArchiveDeletedQuery::Close() {
	_active->store(false);
	return _database.CloseFinal();
}

const ArchiveStoreEpoch &DesktopArchiveDeletedQuery::epoch() const {
	return _epoch;
}

}
