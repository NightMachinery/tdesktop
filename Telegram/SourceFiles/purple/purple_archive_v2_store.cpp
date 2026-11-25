#include "purple/purple_archive_v2_store.h"

#include "purple/purple_archive_files.h"
#include "purple/purple_archive_v2_schema.h"
#include "purple/purple_archive_v2_media.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QtEndian>
#include <algorithm>
#include <limits>

namespace Purple::ArchiveV2 {
namespace {

constexpr auto kPartSize = 1024 * 1024;
constexpr auto kOne = ArchiveSqlExpectation::OneChange;
constexpr auto kRows = ArchiveSqlExpectation::Rows;
constexpr auto kMediaReasons = std::array{ u"policy_description_only", u"over_allowance", u"expired",
	u"over_file_limit", u"copy_incomplete", u"quota_unreclaimable", u"disk_full", u"store_busy" };

QByteArray Number(int64 number) {
	auto bytes = QByteArray(8, Qt::Uninitialized);
	qToBigEndian(number, bytes.data());
	return bytes;
}

ArchiveSqlProgram Program() {
	return { ArchiveSqlStatus::Ready, {{ u"BEGIN IMMEDIATE"_q }} };
}

StoreEffect SqlFailure(const DesktopArchiveSqlResult &result, const QString &otherwise) {
	return { result.diskFull() ? u"disk_full"_q : otherwise };
}

template <typename Value>
const Value *Column(const std::vector<ArchiveSqlValue> &row, size_t i) {
	return i < row.size() ? std::get_if<Value>(&row[i]) : nullptr;
}

}

QByteArray Store::encodeIdentity(const MessageIdentity &identity) {
	const auto peer = PeerTokenText(identity.peer);
	if (!peer || !identity.peer.id || !identity.message) return {};
	return peer->toLatin1() + ':' + QByteArray::number(qulonglong(identity.message));
}

std::optional<QByteArray> Store::seal(QStringView key, const QByteArray &context, const QByteArray &plain) {
	const auto crypto = _options.lease->crypto.callbacks();
	const auto nonce = crypto.random(12);
	const auto sealed = nonce ? crypto.sealAes256Gcm(key.toString(), *nonce, plain, context) : std::nullopt;
	return sealed ? std::optional(*nonce + sealed->tag + sealed->ciphertext) : std::nullopt;
}

std::optional<QByteArray> Store::unseal(QStringView key, const QByteArray &context, const QByteArray &data) {
	if (data.size() < 28) return std::nullopt;
	return _options.lease->crypto.callbacks().openAes256Gcm(key.toString(), data.first(12),
		{ data.mid(28), data.mid(12, 16) }, context);
}

QByteArray Store::associated(const QByteArray &type, const QByteArray &key) const {
	return _scope + _pointer.id.toHex() + ':' + QByteArray::number(_pointer.generation) + '/' + type + '/' + key;
}

QByteArray Store::token(const QByteArray &type, const QByteArray &identity) const {
	return _options.lease->crypto.indexHmac(u"index"_q)(associated(type, identity)).value_or(QByteArray());
}

QString Store::openDatabase(const QByteArray &dataKey) {
	for (const auto &purpose : { u"body"_q, u"index"_q }) {
		auto key = DesktopArchiveCrypto::Derive(dataKey, { _pointer.id, associated(purpose.toLatin1(), {}) });
		const auto added = key && _options.lease->crypto.AddKey(purpose, *key);
		if (key) DesktopArchiveCrypto::Clear(*key);
		if (!added) return u"key_unavailable"_q;
	}
	const auto root = directory();
	const auto path = root + u"/archive.sqlite"_q;
	const auto identity = DesktopArchivePersistentIdentity(root);
	if (!_pointer.directory.isEmpty()) {
		if (identity != _pointer.directory || !DesktopArchivePrivate(root, true)) return u"store_unreadable"_q;
	} else {
		if (!DesktopArchiveDirectory(root) || !QDir(root).isEmpty(
				QDir::AllEntries | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot)) {
			return u"store_unreadable"_q;
		}
		auto next = _pointer;
		next.directory = DesktopArchivePersistentIdentity(root);
		if (next.directory.isEmpty() || !publish(next)) return u"store_unreadable"_q;
	}
	if (!QFileInfo(path).exists() && (_pointer.initialized || !DesktopArchiveWrite(path, {}))) return u"store_unreadable"_q;
	for (const auto &suffix : { QString(), u"-wal"_q, u"-shm"_q, u"-journal"_q }) {
		const auto info = QFileInfo(path + suffix);
		if ((info.exists() || info.isSymLink()) && !DesktopArchivePrivate(info.filePath(), false)) {
			return u"store_unreadable"_q;
		}
	}
	if (!_db.Open(path, false).ok || !_db.ConfigureV2().ok) return u"store_unreadable"_q;
	const auto schema = _db.Read({ u"SELECT name FROM sqlite_schema WHERE name='meta'"_q, {}, kRows });
	if (!schema.ok) return u"store_unreadable"_q;
	const auto aad = associated("meta", {});
	if (schema.rows.empty()) {
		const auto objects = _db.Read({ u"SELECT count(*) FROM sqlite_schema"_q, {}, ArchiveSqlExpectation::OneRow });
		if (_pointer.initialized || !objects.ok || objects.rows != std::vector<std::vector<ArchiveSqlValue>>{{ int64(0) }}) {
			return u"schema_unknown"_q;
		}
		const auto header = seal(u"body", aad, QByteArray("purple archive 2") + Number(0));
		if (!header) return u"key_unavailable"_q;
		auto init = Schema();
		init.statements.insert(init.statements.end() - 1,
			{ u"INSERT INTO meta VALUES(2,?,?,0,0,0)"_q, { _pointer.id, *header }, kOne });
		if (!_db.Execute(init, [this] { return _options.lease->live.load(); }).committed) return u"store_unreadable"_q;
	}
	if (!captureSerial() || !totals()) return u"schema_unknown"_q;
	auto sweep = Program();
	sweep.statements.push_back({ u"DELETE FROM blobs WHERE sealed_manifest IS NULL"_q, {}, ArchiveSqlExpectation::AnyChanges });
	sweep.statements.push_back({ u"COMMIT"_q });
	if (!_db.Execute(sweep, [this] { return _options.lease->live.load(); }).ok) return u"store_unreadable"_q;
	_vacuumPending = pragma(u"PRAGMA freelist_count"_q).value_or(0) > 0;
	if (!_pointer.initialized) {
		auto next = _pointer;
		next.initialized = true;
		if (!publish(next)) return u"store_unreadable"_q;
	}
	return {};
}

std::optional<int64> Store::captureSerial() {
	const auto header = _db.Read({ u"SELECT format,store_id,sealed_header,serial FROM meta"_q, {}, ArchiveSqlExpectation::OneRow });
	if (!header.ok) return std::nullopt;
	const auto &row = header.rows.front();
	const auto format = Column<int64>(row, 0);
	const auto id = Column<QByteArray>(row, 1);
	const auto sealed = Column<QByteArray>(row, 2);
	const auto serial = Column<int64>(row, 3);
	if (!format || *format != 2 || !id || *id != _pointer.id || !sealed || !serial || *serial < 0
		|| unseal(u"body", associated("meta", {}), *sealed) != QByteArray("purple archive 2") + Number(*serial)) {
		return std::nullopt;
	}
	return *serial;
}

std::optional<QByteArray> Store::sealMessage(const QByteArray &key, const Row &row) {
	if (!row.head) return std::nullopt;
	return seal(u"body", associated("message", key), row.identity + Number(row.observedAt) + char((row.emptied ? 2 : 0) | (row.missingPreimage ? 1 : 0))
		+ Number(int64(row.head->status)) + Number(row.sequence) + Number(row.activity)
		+ Number(row.kind) + Number(row.serial) + row.head->bodyTag);
}

std::optional<Store::Row> Store::readHead(const QByteArray &key, const QByteArray &identity) {
	const auto read = _db.Read({ u"SELECT m.status,m.head_seq,v.body_tag,m.sealed_identity,m.last_activity,m.class,m.serial,m.cleared FROM messages m "
		"LEFT JOIN versions v ON v.message_key=m.message_key AND v.seq=m.head_seq WHERE m.message_key=?"_q,
		{ key }, kRows });
	if (!read.ok) return std::nullopt;
	if (read.rows.empty()) return Row();
	const auto &row = read.rows.front();
	const auto status = Column<int64>(row, 0);
	const auto sequence = Column<int64>(row, 1);
	const auto tag = Column<QByteArray>(row, 2);
	const auto sealed = Column<QByteArray>(row, 3);
	const auto activity = Column<int64>(row, 4);
	const auto kind = Column<int64>(row, 5);
	const auto serial = Column<int64>(row, 6);
	const auto plain = sealed ? unseal(u"body", associated("message", key), *sealed) : std::nullopt;
	if (!status || *status < 0 || *status > int64(MessageStatus::Expired) || !activity
		|| !kind || *kind < 0 || *kind > int64(ChatClass::Unknown) || !serial || *serial <= 0
		|| (sequence && (*sequence <= 0 || !tag || tag->size() != 32)) || !plain) return std::nullopt;
	const auto seq = sequence ? *sequence : 0;
	const auto digest = tag ? *tag : QByteArray();
	const auto suffix = Number(*status) + Number(seq) + Number(*activity) + Number(*kind) + Number(*serial) + digest;
	if (plain->size() <= suffix.size() + 9 || !plain->endsWith(suffix)) return std::nullopt;
	const auto raw = plain->first(plain->size() - suffix.size() - 9);
	const auto flags = plain->at(raw.size() + 8);
	const auto cleared = Column<int64>(row, 7);
	if (flags < 0 || flags > 3 || !cleared || *cleared != int64((flags & 2) != 0)) return std::nullopt;
	if ((!identity.isEmpty() && raw != identity) || token("message", raw) != key) return std::nullopt;
	const auto observedAt = qFromBigEndian<int64>(plain->constData() + raw.size());
	return Row{ Head{ MessageStatus(*status), digest }, seq, *activity, observedAt, *kind, *serial, raw, (flags & 1) != 0, *cleared != 0 };
}

StoreEffect Store::write(ArchiveSqlProgram program) {
	if (_batch) {
		for (auto i = size_t(1); i < program.statements.size(); ++i) {
			if (!(*_batch)(program.statements[i])) return { u"text_write_failed"_q };
		}
		return { {}, true };
	}
	program.statements.push_back({ u"COMMIT"_q });
	const auto result = _db.Execute(program, [this] { return current(); });
	if (!result.committed) return SqlFailure(result, current() ? u"text_write_failed"_q : u"scope_retired"_q);
	return { {}, true };
}

StoreEffect Store::capture(const CapturedEvent &event) {
	return captureBatch(std::span(&event, 1)).front();
}

std::vector<StoreEffect> Store::captureBatch(std::span<const CapturedEvent> events) {
	auto effects = std::vector<StoreEffect>(events.size());
	if (events.empty()) return effects;
	auto bytes = int64(0);
	for (const auto &event : events) {
		bytes += event.body.size() + event.preimage.size() + 4096;
		for (const auto &entry : event.attachments) bytes += entry.description.size() + 512;
	}
	auto failure = current() ? physicalLimit(bytes) : u"scope_retired"_q;
	while (failure == u"quota_unreclaimable"_q && maintenancePending() && bytes < kWalHeadroom / 2) {
		const auto maintained = maintain();
		failure = maintained ? physicalLimit(bytes) : maintained.reason;
	}
	if (!failure.isEmpty()) return std::vector<StoreEffect>(events.size(), StoreEffect{ failure });
	const auto result = _db.ExecuteV2([&](const DesktopArchiveSqlite::Write &put) {
		_batch = &put;
		const auto clear = gsl::finally([&] { _batch = nullptr; });
		for (auto i = size_t(0); i != events.size(); ++i) effects[i] = captureOne(events[i]);
		return true;
	}, [this] { return current() && step(Phase::BeforeSqlCommit) && current(); });
	if (!result.committed) {
		for (auto &effect : effects) {
			if (effect.committed || effect.reason == u"text_write_failed"_q) {
				effect = SqlFailure(result, current() ? u"text_write_failed"_q : u"scope_retired"_q);
			}
		}
	} else {
		_trimPending = true;
		static_cast<void>(step(Phase::AfterSqlCommit));
	}
	return effects;
}

StoreEffect Store::captureOne(const CapturedEvent &event) {
	if (!current() || event.storeId != id()) return { u"scope_retired"_q };
	const auto identity = encodeIdentity(event.identity);
	const auto chat = PeerTokenText(event.identity.peer);
	if (identity.isEmpty() || !chat || event.eventId.isEmpty() || event.observedAt < 0
		|| int(event.kind) < 0 || event.kind > ChatClass::Unknown) return { u"text_encode_failed"_q };
	const auto key = token("message", identity);
	const auto eventKey = token("event", key + event.eventId);
	const auto bodyTag = event.body.isEmpty() ? QByteArray() : token("body-tag", event.body);
	if (key.isEmpty() || eventKey.isEmpty() || (!event.body.isEmpty() && bodyTag.isEmpty())) return { u"key_unavailable"_q };
	const auto old = readHead(key, identity);
	const auto exists = _db.Read({ u"SELECT 1 FROM versions WHERE event_key=? UNION ALL SELECT 1 FROM purged_edits WHERE event_key=?"_q,
		{ eventKey, eventKey }, kRows });
	if (!old || !exists.ok) return { u"store_unreadable"_q };
	const auto action = Reduce(old->head, event.event, bodyTag, !exists.rows.empty(), event.fillsMissing, !event.preimage.isEmpty());
	if (action == Mutation::Ignore) return {};
	auto program = Program();
	auto &sql = program.statements;
	const auto previous = captureSerial();
	if (!previous || *previous == std::numeric_limits<int64>::max()) return { u"store_unreadable"_q };
	const auto serial = *previous + 1;
	const auto header = seal(u"body", associated("meta", {}), QByteArray("purple archive 2") + Number(serial));
	if (!header) return { u"text_encode_failed"_q };
	sql.push_back({ u"UPDATE meta SET serial=?,sealed_header=? WHERE store_id=? AND serial=?"_q,
		{ serial, *header, id(), *previous }, kOne });
	const auto day = std::max(old->activity, event.observedAt / 86400);
	const auto message = [&](MessageStatus status, int64 seq, const QByteArray &tag,
			int64 stamp, int64 days, int64 kind) {
		const auto missing = old->head ? old->missingPreimage
			: (event.event != Observation::Baseline && event.preimage.isEmpty());
		return sealMessage(key, { Head{ status, tag }, seq, days, stamp, kind, serial, identity, missing });
	};
	if (!old->head) {
		const auto chatKey = token("chat", chat->toLatin1());
		const auto nonChannel = event.identity.peer.kind == PeerTokenKind::Channel ? ArchiveSqlValue(nullptr)
			: ArchiveSqlValue(token("nonchannel", QByteArray::number(qulonglong(event.identity.message))));
		const auto initial = message(MessageStatus::Live, 0, {}, event.observedAt, day, int64(event.kind));
		if (chatKey.isEmpty() || !initial) return { u"key_unavailable"_q };
		sql.push_back({ u"INSERT INTO messages VALUES(?,?,?,?,?,?,NULL,?,?,0)"_q,
			{ key, chatKey, nonChannel, int64(event.kind), int64(0), day, *initial, serial }, kOne });
	}
	auto sequence = old->sequence;
	const auto append = [&](const QByteArray &body, const QByteArray &tokenKey, const QByteArray &tag) {
		if (sequence == std::numeric_limits<int64>::max() || tokenKey.isEmpty() || tag.isEmpty()) return false;
		const auto data = seal(u"body", associated("version", key + Number(++sequence) + Number(serial)), Number(event.observedAt) + body);
		if (!data) return false;
		sql.push_back({ u"INSERT INTO versions VALUES(?,?,?,?,?,?,?)"_q,
			{ key, sequence, tokenKey, day, tag, *data, serial }, kOne });
		return true;
	};
	if (action == Mutation::End) {
		const auto status = event.event == Observation::Delete ? MessageStatus::Deleted
			: event.event == Observation::Consume ? MessageStatus::Consumed : MessageStatus::Expired;
		auto tag = old->head ? old->head->bodyTag : QByteArray();
		if (!sequence && !event.preimage.isEmpty()) {
			tag = token("body-tag", event.preimage);
			if (!append(event.preimage, token("preimage", eventKey), tag)) return { u"text_encode_failed"_q };
		}
		const auto sealed = message(status, sequence, tag, event.observedAt, day, int64(event.kind));
		if (!sealed) return { u"text_encode_failed"_q };
		sql.push_back({ u"UPDATE messages SET status=?,last_activity=?,class=?,sealed_identity=?,serial=?,head_seq=? "
			"WHERE message_key=?"_q, { int64(status), day, int64(event.kind), *sealed, serial,
				sequence ? ArchiveSqlValue(sequence) : ArchiveSqlValue(nullptr), key }, kOne });
		return write(std::move(program));
	}
	if (action == Mutation::FillMissing) {
		if (sequence <= 0 || event.body.isEmpty()) return { u"text_encode_failed"_q };
		const auto original = body(event.identity, sequence);
		if (!original) return { u"store_unreadable"_q };
		const auto data = seal(u"body", associated("version", key + Number(sequence) + Number(serial)), Number(original->observedAt) + event.body);
		const auto sealed = message(MessageStatus::Live, sequence, bodyTag, old->observedAt, old->activity, old->kind);
		if (!data || !sealed) return { u"text_encode_failed"_q };
		sql.push_back({ u"UPDATE versions SET body_tag=?,sealed_body=?,serial=? WHERE message_key=? AND seq=?"_q,
			{ bodyTag, *data, serial, key, sequence }, kOne });
		sql.push_back({ u"UPDATE messages SET sealed_identity=?,serial=? WHERE message_key=?"_q, { *sealed, serial, key }, kOne });
		return write(std::move(program));
	}
	if (action == Mutation::AppendWithPreimage && !append(event.preimage,
			token("preimage", eventKey), token("body-tag", event.preimage))) return { u"text_encode_failed"_q };
	if (!append(event.body, eventKey, bodyTag)) return { u"text_encode_failed"_q };
	for (auto i = size_t(0); i != event.attachments.size(); ++i) {
		const auto &entry = event.attachments[i];
		const auto media = token("media", entry.source);
		const auto mediaKey = key + Number(sequence) + Number(int64(i)) + media + Number(entry.kind) + Number(entry.timed);
		const auto data = seal(u"body", associated("media", mediaKey), entry.description);
		if (entry.source.isEmpty() || media.isEmpty() || !data) return { u"text_encode_failed"_q };
		sql.push_back({ u"INSERT INTO media VALUES(?,?,?,?,?,?,0,0,?,NULL,NULL)"_q,
			{ key, sequence, int64(i), media, int64(entry.kind), int64(entry.timed), *data }, kOne });
	}
	const auto sealed = message(MessageStatus::Live, sequence, bodyTag, event.observedAt, day, int64(event.kind));
	if (!sealed) return { u"text_encode_failed"_q };
	sql.push_back({ u"UPDATE messages SET head_seq=?,last_activity=?,class=?,sealed_identity=?,serial=? WHERE message_key=?"_q,
		{ sequence, day, int64(event.kind), *sealed, serial, key }, kOne });
	return write(std::move(program));
}

std::optional<StoredBody> Store::body(const MessageIdentity &identity, int64 sequence) {
	if (!current() || sequence <= 0 || encodeIdentity(identity).isEmpty()) return std::nullopt;
	const auto key = token("message", encodeIdentity(identity));
	const auto head = readHead(key, encodeIdentity(identity));
	if (!head || !head->head) return std::nullopt;
	const auto result = _db.Read({ u"SELECT sealed_body,serial FROM versions WHERE message_key=? AND seq=?"_q,
		{ key, sequence }, ArchiveSqlExpectation::OneRow });
	if (!result.ok) return std::nullopt;
	const auto sealed = Column<QByteArray>(result.rows.front(), 0);
	const auto serial = Column<int64>(result.rows.front(), 1);
	const auto plain = sealed && serial && *serial > 0
		? unseal(u"body", associated("version", key + Number(sequence) + Number(*serial)), *sealed) : std::nullopt;
	if (!plain || plain->size() < 8 || !current()) return std::nullopt;
	return StoredBody{ qFromBigEndian<int64>(plain->constData()), plain->mid(8), *serial };
}

std::optional<Store::SavedBlob> Store::savedBlob(const QByteArray &key, int64 seq, int64 ordinal) {
	const auto record = _db.Read({ u"SELECT b.blob_key,b.size,b.sealed_manifest,m.sealed_blob_ref FROM media m "
		"JOIN blobs b USING(blob_key) WHERE m.message_key=? AND m.seq=? AND m.ordinal=? AND m.state=1"_q,
		{ key, seq, ordinal }, ArchiveSqlExpectation::OneRow });
	if (!record.ok) return std::nullopt;
	const auto &row = record.rows.front();
	const auto blob = Column<QByteArray>(row, 0);
	const auto size = Column<int64>(row, 1);
	const auto sealed = Column<QByteArray>(row, 2);
	const auto binding = Column<QByteArray>(row, 3);
	if (!blob || blob->size() != 32 || !size || *size <= 0 || !sealed || !binding
		|| unseal(u"body", associated("media-ref", key + Number(seq) + Number(ordinal)), *binding) != *blob) {
		return std::nullopt;
	}
	const auto manifest = unseal(u"body", associated("blob", *blob), *sealed);
	const auto parts = (*size - 1) / kPartSize + 1;
	const auto prefix = Number(*size) + Number(parts);
	if (!manifest || manifest->size() != 48 || !manifest->startsWith(prefix)) return std::nullopt;
	const auto digest = manifest->mid(prefix.size());
	if (token("blob", digest) != *blob) return std::nullopt;
	return SavedBlob{ *blob, digest, *size, parts };
}

std::optional<std::vector<MediaStatus>> Store::media(const MessageIdentity &identity, int64 sequence) {
	if (!current() || sequence <= 0) return std::nullopt;
	const auto raw = encodeIdentity(identity);
	const auto key = token("message", raw);
	const auto head = readHead(key, raw);
	if (raw.isEmpty() || !head || !head->head) return std::nullopt;
	const auto source = _db.Read({ u"SELECT ordinal,media_key,kind,timed,sealed_descriptor,blob_key,reason FROM media "
		"WHERE message_key=? AND seq=? ORDER BY ordinal LIMIT 64"_q, { key, sequence }, kRows });
	if (!source.ok) return std::nullopt;
	auto results = std::vector<MediaStatus>();
	for (const auto &row : source.rows) {
		const auto ordinal = Column<int64>(row, 0);
		const auto media = Column<QByteArray>(row, 1);
		const auto kind = Column<int64>(row, 2);
		const auto timed = Column<int64>(row, 3);
		const auto sealed = Column<QByteArray>(row, 4);
		if (!ordinal || !media || !kind || !timed || !sealed) return std::nullopt;
		const auto descriptor = unseal(u"body", associated("media", key + Number(sequence)
			+ Number(*ordinal) + *media + Number(*kind) + Number(*timed)), *sealed);
		if (!descriptor) return std::nullopt;
		auto item = MediaStatus{ *ordinal, *descriptor };
		if (Column<QByteArray>(row, 5)) {
			const auto saved = savedBlob(key, sequence, *ordinal);
			if (!saved) return std::nullopt;
			item.savedBytes = saved->size;
		} else {
			const auto reason = Column<int64>(row, 6);
			if (!reason || *reason < 0 || *reason >= int64(kMediaReasons.size())) return std::nullopt;
			item.reason = QStringView(kMediaReasons[*reason]).toString();
		}
		results.push_back(std::move(item));
	}
	return current() ? std::optional(std::move(results)) : std::nullopt;
}

bool Store::readSaved(const QByteArray &expected, const MessageIdentity &identity, int64 sequence,
		int64 ordinal, const std::function<bool(const QByteArray &)> &sink) {
	if (!current() || expected != id() || !sink || sequence <= 0 || ordinal < 0) return false;
	const auto raw = encodeIdentity(identity);
	const auto key = token("message", raw);
	const auto head = readHead(key, raw);
	if (raw.isEmpty() || !head || !head->head) return false;
	const auto saved = savedBlob(key, sequence, ordinal);
	if (!saved) return false;
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	auto remaining = saved->size;
	for (auto part = int64(0); part != saved->parts; ++part) {
		if (!current()) return false;
		const auto row = _db.Read({ u"SELECT sealed_bytes FROM blob_parts WHERE blob_key=? AND part_no=?"_q,
			{ saved->key, part }, ArchiveSqlExpectation::OneRow });
		const auto cipher = row.ok ? Column<QByteArray>(row.rows.front(), 0) : nullptr;
		auto plain = cipher ? unseal(u"body", associated("part", saved->key + Number(part)), *cipher) : std::nullopt;
		if (!plain) return false;
		const auto clear = gsl::finally([&] { DesktopArchiveCrypto::Clear(*plain); });
		if (plain->size() != std::min<int64>(remaining, kPartSize) || !current() || !sink(*plain)) return false;
		hash.addData(*plain);
		remaining -= plain->size();
	}
	return current() && !remaining && hash.result() == saved->digest;
}

StoreEffect Store::writeBlob(const BlobInput &input, const std::function<void()> &betweenParts) {
	if (!current() || input.storeId != id() || input.revision != _options.lease->mediaRevision.load()) {
		return { u"scope_retired"_q };
	}
	if (!input.read || input.size <= 0 || input.digest.size() != 32 || input.source.isEmpty()
		|| input.sequence <= 0 || input.ordinal < 0 || encodeIdentity(input.identity).isEmpty()) return { u"copy_incomplete"_q };
	const auto key = token("message", encodeIdentity(input.identity));
	const auto media = token("media", input.source);
	const auto blob = token("blob", input.digest);
	const auto target = _db.Read({ u"SELECT media_key,sealed_descriptor,blob_key,kind,timed,sealed_blob_ref FROM media WHERE message_key=? AND seq=? AND ordinal=?"_q,
		{ key, input.sequence, input.ordinal }, kRows });
	if (!target.ok || target.rows.empty() || key.isEmpty() || media.isEmpty() || blob.isEmpty()) return { u"scope_retired"_q };
	const auto &row = target.rows.front();
	const auto source = Column<QByteArray>(row, 0);
	const auto sealed = Column<QByteArray>(row, 1);
	const auto kind = Column<int64>(row, 3);
	const auto timed = Column<int64>(row, 4);
	if (!source || *source != media || !sealed || !kind || !timed) return { u"scope_retired"_q };
	const auto old = readHead(key, encodeIdentity(input.identity));
	if (!old || !old->head) return { u"scope_retired"_q };
	const auto aad = associated("media", key + Number(input.sequence) + Number(input.ordinal) + media
		+ Number(*kind) + Number(*timed));
	if (!unseal(u"body", aad, *sealed)) return { u"scope_retired"_q };
	const auto describe = [&](QString reason) {
		const auto found = std::find_if(kMediaReasons.begin(), kMediaReasons.end(),
			[&](auto value) { return reason == QStringView(value); });
		if (found != kMediaReasons.end() && current() && physicalLimit(0).isEmpty()) {
			auto program = Program();
			program.statements.push_back({ u"UPDATE media SET reason=? WHERE message_key=? AND seq=? AND ordinal=? "
				"AND media_key=? AND blob_key IS NULL"_q, { int64(found - kMediaReasons.begin()), key,
					input.sequence, input.ordinal, media }, ArchiveSqlExpectation::AnyChanges });
			program.statements.push_back({ u"COMMIT"_q });
			static_cast<void>(_db.Execute(program, [this] { return current(); }));
		}
		return StoreEffect{ std::move(reason) };
	};
	if (old->activity <= today() - _options.policy.daysFor(ChatClass(old->kind))) return describe(u"expired"_q);
	if (!*timed && input.size > int64(_options.policy.ordinaryFileMiB) * kMiB) return describe(u"over_file_limit"_q);
	if (*timed && input.size > int64(_options.policy.timedMiB) * kMiB) return describe(u"over_allowance"_q);
	const auto saved = Column<QByteArray>(row, 2);
	const auto binding = Column<QByteArray>(row, 5);
	const auto reference = associated("media-ref", key + Number(input.sequence) + Number(input.ordinal));
	if (saved && (!binding || unseal(u"body", reference, *binding) != *saved)) return { u"store_unreadable"_q };
	if (saved && *saved != blob) return { u"scope_retired"_q };
	const auto parts = (input.size - 1) / kPartSize + 1;
	const auto manifest = Number(input.size) + Number(parts) + input.digest;
	const auto previous = _db.Read({ u"SELECT size,sealed_manifest,timed FROM blobs WHERE blob_key=?"_q, { blob }, kRows });
	if (!previous.ok) return { u"store_unreadable"_q };
	auto charge = input.size;
	if (!previous.rows.empty()) {
		const auto &record = previous.rows.front();
		const auto size = Column<int64>(record, 0);
		const auto sealed = Column<QByteArray>(record, 1);
		const auto previousTimed = Column<int64>(record, 2);
		if (!size || *size != input.size || !sealed || !previousTimed
			|| unseal(u"body", associated("blob", blob), *sealed) != manifest) {
			return { u"store_unreadable"_q };
		}
		charge = *timed && !*previousTimed ? input.size : 0;
	}
	if (saved) return previous.rows.empty() ? StoreEffect{ u"store_unreadable"_q } : StoreEffect{};
	const auto fresh = previous.rows.empty();
	if (const auto failure = reserve(charge, *timed, key, blob, betweenParts); !failure.isEmpty()) return describe(failure);
	const auto sealedReference = seal(u"body", reference, blob);
	const auto sealedManifest = fresh ? seal(u"body", associated("blob", blob), manifest) : std::nullopt;
	if (!sealedReference || (fresh && !sealedManifest)) return { u"key_unavailable"_q };
	const auto live = [&] { return current() && input.revision == _options.lease->mediaRevision.load(); };
	auto staged = false;
	const auto release = gsl::finally([&] {
		_mediaKeep.clear();
		if (!staged || !current()) return;
		auto program = Program();
		program.statements.push_back({ u"DELETE FROM blobs WHERE blob_key=? AND sealed_manifest IS NULL"_q,
			{ blob }, ArchiveSqlExpectation::AnyChanges });
		static_cast<void>(write(std::move(program)));
	});
	_mediaKeep = key;
	if (fresh) {
		auto hash = QCryptographicHash(QCryptographicHash::Sha256);
		auto remaining = input.size;
		for (auto part = int64(0); part < parts;) {
			const auto count = std::min<int64>(4, parts - part);
			if (const auto failure = physicalLimit(std::min<int64>(remaining, count * kPartSize) + 128 * 1024);
					!failure.isEmpty()) return describe(failure);
			auto program = Program();
			if (!part) program.statements.push_back({ u"INSERT INTO blobs VALUES(?,?,NULL,?)"_q, { blob, input.size, *timed }, kOne });
			for (auto i = int64(0); i != count; ++i, ++part) {
				if (!live() || !step(Phase::BlobPart)) return { u"scope_retired"_q };
				const auto length = std::min<int64>(remaining, kPartSize);
				auto bytes = input.read(length);
				const auto clear = gsl::finally([&] { DesktopArchiveCrypto::Clear(bytes); });
				if (bytes.size() != length) return describe(u"copy_incomplete"_q);
				hash.addData(bytes);
				const auto sealedPart = seal(u"body", associated("part", blob + Number(part)), bytes);
				if (!sealedPart) return describe(u"copy_incomplete"_q);
				program.statements.push_back({ u"INSERT INTO blob_parts VALUES(?,?,?)"_q, { blob, part, *sealedPart }, kOne });
				remaining -= length;
			}
			if (!live()) return { u"scope_retired"_q };
			const auto result = write(std::move(program));
			if (!result) return describe(result.reason == u"disk_full"_q ? result.reason : u"copy_incomplete"_q);
			staged = true;
			if (betweenParts) betweenParts();
		}
		if (!input.read(1).isEmpty() || hash.result() != input.digest) return describe(u"copy_incomplete"_q);
	}
	if (!live()) return { u"scope_retired"_q };
	const auto allowed = int64(*timed ? _options.policy.timedMiB : _options.policy.ordinaryFileMiB) * kMiB;
	if (input.size > allowed) return describe(*timed ? u"over_allowance"_q : u"over_file_limit"_q);
	if (const auto failure = physicalLimit(128 * 1024); !failure.isEmpty()) return describe(failure);
	auto program = Program();
	if (fresh) program.statements.push_back({ u"UPDATE blobs SET sealed_manifest=? WHERE blob_key=? AND sealed_manifest IS NULL"_q,
		{ *sealedManifest, blob }, kOne });
	if (*timed && !fresh) program.statements.push_back({ u"UPDATE blobs SET timed=1 WHERE blob_key=?"_q, { blob }, kOne });
	program.statements.push_back({ u"UPDATE media SET blob_key=?,sealed_blob_ref=?,state=1,reason=0 WHERE message_key=? AND seq=? "
		"AND ordinal=? AND media_key=? AND blob_key IS NULL"_q, { blob, *sealedReference, key, input.sequence, input.ordinal, media }, kOne });
	program.statements.push_back({ u"COMMIT"_q });
	const auto result = _db.Execute(program, [&] { return live() && step(Phase::BeforeSqlCommit) && live(); });
	if (!result.committed) return describe(SqlFailure(result, live() ? u"copy_incomplete"_q : u"scope_retired"_q).reason);
	staged = false;
	_trimPending = true;
	static_cast<void>(step(Phase::AfterSqlCommit));
	return { {}, true };
}

}
