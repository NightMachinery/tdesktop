#include "purple/purple_archive_v2_store.h"

#include "purple/purple_archive_files.h"
#include "purple/purple_archive_v2_schema.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFileInfo>
#include <QtCore/QtEndian>
#include <algorithm>
#include <limits>

namespace Purple::ArchiveV2 {
namespace {

constexpr auto kPartSize = 1024 * 1024;
constexpr auto kOne = ArchiveSqlExpectation::OneChange;
constexpr auto kRows = ArchiveSqlExpectation::Rows;

QByteArray Number(int64 number) {
	auto bytes = QByteArray(8, Qt::Uninitialized);
	qToBigEndian(number, bytes.data());
	return bytes;
}

QByteArray Identity(const MessageIdentity &identity) {
	const auto peer = PeerTokenText(identity.peer);
	if (!peer || !identity.peer.id || !identity.message) return {};
	return peer->toLatin1() + ':' + QByteArray::number(qulonglong(identity.message));
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
	if ((_pointer.initialized && !QFileInfo(path).exists()) || !DesktopArchiveDirectory(root)) {
		return u"store_unreadable"_q;
	}
	if (!QFileInfo(path).exists() && !DesktopArchiveWrite(path, {})) return u"store_unreadable"_q;
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
		const auto header = seal(u"body", aad, "purple archive 2");
		if (!header) return u"key_unavailable"_q;
		auto init = Schema();
		init.statements.insert(init.statements.end() - 1,
			{ u"INSERT INTO meta VALUES(2,?,?)"_q, { _pointer.id, *header }, kOne });
		if (!_db.Execute(init, [this] { return _options.lease->live.load(); }).committed) return u"store_unreadable"_q;
	}
	const auto header = _db.Read({ u"SELECT format,store_id,sealed_header FROM meta"_q, {}, ArchiveSqlExpectation::OneRow });
	if (!header.ok) return u"schema_unknown"_q;
	const auto &row = header.rows.front();
	const auto format = Column<int64>(row, 0);
	const auto id = Column<QByteArray>(row, 1);
	const auto sealed = Column<QByteArray>(row, 2);
	if (!format || *format != 2 || !id || *id != _pointer.id) return u"schema_unknown"_q;
	if (!sealed || unseal(u"body", aad, *sealed) != QByteArray("purple archive 2")) return u"store_unreadable"_q;
	if (!_pointer.initialized) {
		auto next = _pointer;
		next.initialized = true;
		if (!publish(next)) return u"store_unreadable"_q;
	}
	return {};
}

std::optional<Store::Row> Store::readHead(const QByteArray &key, const QByteArray &identity) {
	const auto read = _db.Read({ u"SELECT m.status,m.head_seq,v.body_tag,m.sealed_identity,m.last_activity,m.class FROM messages m "
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
	const auto plain = sealed ? unseal(u"body", associated("message", key), *sealed) : std::nullopt;
	if (!status || *status < 0 || *status > int64(MessageStatus::Expired) || !activity
		|| !kind || *kind < 0 || *kind > int64(ChatClass::Unknown)
		|| (sequence && (*sequence <= 0 || !tag || tag->size() != 32))
		|| !plain || plain->size() < identity.size() + 8 || !plain->startsWith(identity)) return std::nullopt;
	const auto seq = sequence ? *sequence : 0;
	const auto digest = tag ? *tag : QByteArray();
	const auto observedAt = qFromBigEndian<int64>(plain->constData() + identity.size());
	if (plain->mid(identity.size() + 8) != Number(*status) + Number(seq) + Number(*activity) + Number(*kind) + digest) {
		return std::nullopt;
	}
	return Row{ Head{ MessageStatus(*status), digest }, seq, *activity, observedAt, *kind };
}

StoreEffect Store::write(ArchiveSqlProgram program) {
	program.statements.push_back({ u"COMMIT"_q });
	const auto result = _db.Execute(program, [this] {
		return current() && step(Phase::BeforeSqlCommit) && current();
	});
	if (!result.committed) return SqlFailure(result, current() ? u"text_write_failed"_q : u"scope_retired"_q);
	static_cast<void>(step(Phase::AfterSqlCommit));
	return { {}, true };
}

StoreEffect Store::capture(const CapturedEvent &event) {
	if (!current() || event.storeId != id()) return { u"scope_retired"_q };
	const auto identity = Identity(event.identity);
	const auto chat = PeerTokenText(event.identity.peer);
	if (identity.isEmpty() || !chat || event.eventId.isEmpty() || event.observedAt < 0
		|| int(event.kind) < 0 || event.kind > ChatClass::Unknown) return { u"text_encode_failed"_q };
	const auto key = token("message", identity);
	const auto eventKey = token("event", key + event.eventId);
	const auto bodyTag = event.body.isEmpty() ? QByteArray() : token("body-tag", event.body);
	if (key.isEmpty() || eventKey.isEmpty() || (!event.body.isEmpty() && bodyTag.isEmpty())) return { u"key_unavailable"_q };
	const auto old = readHead(key, identity);
	const auto exists = _db.Read({ u"SELECT 1 FROM versions WHERE event_key=?"_q, { eventKey }, kRows });
	if (!old || !exists.ok) return { u"store_unreadable"_q };
	const auto action = Reduce(old->head, event.event, bodyTag, !exists.rows.empty(), event.fillsMissing, !event.preimage.isEmpty());
	if (action == Mutation::Ignore) return {};
	auto program = Program();
	auto &sql = program.statements;
	const auto day = std::max(old->activity, event.observedAt / 86400);
	const auto message = [&](MessageStatus status, int64 seq, const QByteArray &tag,
			int64 stamp, int64 days, int64 kind) {
		return seal(u"body", associated("message", key),
			identity + Number(stamp) + Number(int64(status)) + Number(seq) + Number(days) + Number(kind) + tag);
	};
	if (!old->head) {
		const auto chatKey = token("chat", chat->toLatin1());
		const auto nonChannel = event.identity.peer.kind == PeerTokenKind::Channel ? ArchiveSqlValue(nullptr)
			: ArchiveSqlValue(token("nonchannel", QByteArray::number(qulonglong(event.identity.message))));
		const auto initial = message(MessageStatus::Live, 0, {}, event.observedAt, day, int64(event.kind));
		if (chatKey.isEmpty() || !initial) return { u"key_unavailable"_q };
		sql.push_back({ u"INSERT INTO messages VALUES(?,?,?,?,?,?,NULL,?)"_q,
			{ key, chatKey, nonChannel, int64(event.kind), int64(0), day, *initial }, kOne });
	}
	if (action == Mutation::End) {
		const auto status = event.event == Observation::Delete ? MessageStatus::Deleted
			: event.event == Observation::Consume ? MessageStatus::Consumed : MessageStatus::Expired;
		const auto sealed = message(status, old->sequence, old->head ? old->head->bodyTag : QByteArray(),
			event.observedAt, day, int64(event.kind));
		if (!sealed) return { u"text_encode_failed"_q };
		sql.push_back({ u"UPDATE messages SET status=?,last_activity=?,class=?,sealed_identity=? WHERE message_key=?"_q,
			{ int64(status), day, int64(event.kind), *sealed, key }, kOne });
		return write(std::move(program));
	}
	auto sequence = old->sequence;
	const auto append = [&](const QByteArray &body, const QByteArray &tokenKey, const QByteArray &tag) {
		if (sequence == std::numeric_limits<int64>::max() || tokenKey.isEmpty() || tag.isEmpty()) return false;
		const auto data = seal(u"body", associated("version", key + Number(++sequence)), Number(event.observedAt) + body);
		if (!data) return false;
		sql.push_back({ u"INSERT INTO versions VALUES(?,?,?,?,?,?)"_q,
			{ key, sequence, tokenKey, day, tag, *data }, kOne });
		return true;
	};
	if (action == Mutation::FillMissing) {
		if (sequence <= 0 || event.body.isEmpty()) return { u"text_encode_failed"_q };
		const auto original = body(event.identity, sequence);
		if (!original) return { u"store_unreadable"_q };
		const auto data = seal(u"body", associated("version", key + Number(sequence)), Number(original->observedAt) + event.body);
		const auto sealed = message(MessageStatus::Live, sequence, bodyTag, old->observedAt, old->activity, old->kind);
		if (!data || !sealed) return { u"text_encode_failed"_q };
		sql.push_back({ u"UPDATE versions SET body_tag=?,sealed_body=? WHERE message_key=? AND seq=?"_q,
			{ bodyTag, *data, key, sequence }, kOne });
		sql.push_back({ u"UPDATE messages SET sealed_identity=? WHERE message_key=?"_q, { *sealed, key }, kOne });
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
	sql.push_back({ u"UPDATE messages SET head_seq=?,last_activity=?,class=?,sealed_identity=? WHERE message_key=?"_q,
		{ sequence, day, int64(event.kind), *sealed, key }, kOne });
	return write(std::move(program));
}

std::optional<StoredBody> Store::body(const MessageIdentity &identity, int64 sequence) {
	if (!current() || sequence <= 0 || Identity(identity).isEmpty()) return std::nullopt;
	const auto key = token("message", Identity(identity));
	const auto head = readHead(key, Identity(identity));
	if (!head || !head->head) return std::nullopt;
	const auto result = _db.Read({ u"SELECT sealed_body FROM versions WHERE message_key=? AND seq=?"_q,
		{ key, sequence }, ArchiveSqlExpectation::OneRow });
	if (!result.ok) return std::nullopt;
	const auto sealed = Column<QByteArray>(result.rows.front(), 0);
	const auto plain = sealed ? unseal(u"body", associated("version", key + Number(sequence)), *sealed) : std::nullopt;
	if (!plain || plain->size() < 8 || !current()) return std::nullopt;
	return StoredBody{ qFromBigEndian<int64>(plain->constData()), plain->mid(8) };
}

StoreEffect Store::writeBlob(const BlobInput &input) {
	if (!current() || input.storeId != id()) return { u"scope_retired"_q };
	if (!input.read || input.size <= 0 || input.digest.size() != 32 || input.source.isEmpty()
		|| input.sequence <= 0 || input.ordinal < 0 || Identity(input.identity).isEmpty()) return { u"copy_incomplete"_q };
	const auto key = token("message", Identity(input.identity));
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
	const auto aad = associated("media", key + Number(input.sequence) + Number(input.ordinal) + media
		+ Number(*kind) + Number(*timed));
	if (!unseal(u"body", aad, *sealed)) return { u"scope_retired"_q };
	const auto saved = Column<QByteArray>(row, 2);
	const auto binding = Column<QByteArray>(row, 5);
	const auto reference = associated("media-ref", key + Number(input.sequence) + Number(input.ordinal));
	if (saved && (!binding || unseal(u"body", reference, *binding) != *saved)) return { u"store_unreadable"_q };
	if (saved && *saved != blob) return { u"scope_retired"_q };
	const auto parts = (input.size - 1) / kPartSize + 1;
	const auto manifest = Number(input.size) + Number(parts) + input.digest;
	const auto previous = _db.Read({ u"SELECT size,sealed_manifest FROM blobs WHERE blob_key=?"_q, { blob }, kRows });
	if (!previous.ok) return { u"store_unreadable"_q };
	if (!previous.rows.empty()) {
		const auto &record = previous.rows.front();
		const auto size = Column<int64>(record, 0);
		const auto sealed = Column<QByteArray>(record, 1);
		if (!size || *size != input.size || !sealed || unseal(u"body", associated("blob", blob), *sealed) != manifest) {
			return { u"store_unreadable"_q };
		}
	}
	if (saved) return previous.rows.empty() ? StoreEffect{ u"store_unreadable"_q } : StoreEffect{};
	const auto sealedReference = seal(u"body", reference, blob);
	if (!sealedReference) return { u"key_unavailable"_q };
	const auto result = _db.ExecuteV2([&](const DesktopArchiveSqlite::Write &put) {
		if (previous.rows.empty()) {
			const auto meta = seal(u"body", associated("blob", blob), manifest);
			if (!meta || !put({ u"INSERT INTO blobs VALUES(?,?,?)"_q, { blob, input.size, *meta }, kOne })) return false;
			auto hash = QCryptographicHash(QCryptographicHash::Sha256);
			auto remaining = input.size;
			for (auto part = int64(0); part != parts; ++part) {
				if (!current() || !step(Phase::BlobPart)) return false;
				const auto length = std::min<int64>(remaining, kPartSize);
				auto bytes = input.read(length);
				const auto clear = gsl::finally([&] { DesktopArchiveCrypto::Clear(bytes); });
				if (bytes.size() != length) return false;
				hash.addData(bytes);
				const auto sealed = seal(u"body", associated("part", blob + Number(part)), bytes);
				if (!sealed || !put({ u"INSERT INTO blob_parts VALUES(?,?,?)"_q, { blob, part, *sealed }, kOne })) return false;
				remaining -= length;
			}
			if (!input.read(1).isEmpty() || hash.result() != input.digest) return false;
		}
		return put({ u"UPDATE media SET blob_key=?,sealed_blob_ref=?,state=1,reason=0 WHERE message_key=? AND seq=? AND ordinal=? AND blob_key IS NULL"_q,
			{ blob, *sealedReference, key, input.sequence, input.ordinal }, kOne });
	}, [this] { return current() && step(Phase::BeforeSqlCommit) && current(); });
	if (!result.committed) return SqlFailure(result, current() ? u"copy_incomplete"_q : u"scope_retired"_q);
	static_cast<void>(step(Phase::AfterSqlCommit));
	return { {}, true };
}

}
