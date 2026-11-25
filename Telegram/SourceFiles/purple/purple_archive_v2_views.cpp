#include "purple/purple_archive_v2_store.h"

#include "purple/purple_archive_v2_media.h"

#include <algorithm>

namespace Purple::ArchiveV2 {
namespace {

constexpr auto kRows = ArchiveSqlExpectation::Rows;
constexpr auto kChanges = ArchiveSqlExpectation::AnyChanges;
constexpr auto kOne = ArchiveSqlExpectation::OneChange;
constexpr auto kPage = int64(64);

std::optional<MessageIdentity> ParseIdentity(const QByteArray &text) {
	const auto colon = text.lastIndexOf(':');
	if (colon <= 0) return std::nullopt;
	const auto peer = ParsePeerToken(QString::fromLatin1(text.first(colon)));
	const auto suffix = text.mid(colon + 1);
	auto ok = false;
	const auto message = suffix.toULongLong(&ok);
	if (!peer || !peer->id || !ok || !message || suffix != QByteArray::number(message)) return std::nullopt;
	return MessageIdentity{ *peer, uint64_t(message) };
}

ArchiveSqlProgram Begin() {
	return { ArchiveSqlStatus::Ready, {{ u"BEGIN IMMEDIATE"_q }} };
}

}

std::optional<HistoryPage> Store::history(const MessageIdentity &identity, int64 before, int limit) {
	if (!current() || before < 0 || limit < 1 || limit > kPage) return std::nullopt;
	const auto raw = encodeIdentity(identity);
	const auto key = token("message", raw);
	if (raw.isEmpty() || key.isEmpty()) return std::nullopt;
	const auto head = readHead(key, raw);
	if (!head) return std::nullopt;
	auto result = HistoryPage();
	if (!head->head) return result;
	result.status = head->head->status;
	result.missingPreimage = head->missingPreimage;
	const auto ordered = _db.Read({ u"SELECT seq FROM versions WHERE message_key=? "
		"AND (?=0 OR seq<?) ORDER BY seq DESC LIMIT ?"_q, { key, before, before, int64(limit)+1 }, kRows });
	if (!ordered.ok) return std::nullopt;
	for (const auto &row : ordered.rows) {
		const auto index = row.empty() ? nullptr : std::get_if<int64>(&row.front());
		if (!index) return std::nullopt;
		if (int(result.rows.size()) == limit) {
			result.before = result.rows.back().sequence;
			break;
		}
		auto value = body(identity, *index);
		auto attachments = media(identity, *index);
		if (!value || !attachments) return std::nullopt;
		result.rows.push_back({ *index, std::move(*value), std::move(*attachments) });
	}
	return current() ? std::optional(std::move(result)) : std::nullopt;
}

std::optional<TimelinePage> Store::timeline(std::optional<PeerToken> peer, bool deletedOnly,
		std::optional<TimelineCursor> before, int limit) {
	if (!current() || limit < 1 || limit > kPage || (!peer && !deletedOnly)) return std::nullopt;
	auto chat = QByteArray();
	if (peer) {
		const auto name = PeerTokenText(*peer);
		if (!name || !peer->id) return std::nullopt;
		chat = token("chat", name->toLatin1());
		if (chat.isEmpty()) return std::nullopt;
	}
	if (before && (before->store != id() || before->chat != chat || before->trash != deletedOnly)) return std::nullopt;
	auto query = u"SELECT message_key,last_activity FROM messages WHERE cleared=0 AND "_q;
	auto binds = std::vector<ArchiveSqlValue>();
	if (deletedOnly) query += u"status<>0 AND "_q;
	if (peer) {
		query += u"chat_key=? AND "_q;
		binds.push_back(chat);
	}
	if (before) {
		query += u"(last_activity,message_key)<(?,?) AND "_q;
		binds.insert(binds.end(), { before->activity, before->key });
	}
	query += u"1 ORDER BY last_activity DESC,message_key DESC LIMIT ?"_q;
	binds.push_back(int64(limit) + 1);
	const auto selected = _db.Read({ query, std::move(binds), kRows });
	if (!selected.ok) return std::nullopt;
	auto result = TimelinePage();
	for (const auto &row : selected.rows) {
		if (row.size() != 2) return std::nullopt;
		const auto message = std::get_if<QByteArray>(&row.front());
		const auto last = std::get_if<int64>(&row.back());
		if (!message || !last) return std::nullopt;
		if (int(result.rows.size()) == limit) {
			result.before = result.rows.back().cursor;
			break;
		}
		const auto head = readHead(*message, {});
		if (!head || !head->head || head->emptied || (deletedOnly && head->head->status == MessageStatus::Live) || *last != head->activity) {
			return std::nullopt;
		}
		const auto identity = ParseIdentity(head->identity);
		if (!identity || (peer && identity->peer != *peer)) return std::nullopt;
		auto latest = head->sequence ? body(*identity, head->sequence) : std::nullopt;
		if (head->sequence && !latest) return std::nullopt;
		result.rows.push_back({ *identity, head->head->status, head->observedAt, std::move(latest), TimelineCursor(*last, *message, id(), chat, deletedOnly) });
	}
	return current() ? std::optional(std::move(result)) : std::nullopt;
}

std::optional<CleanupTicket> Store::prepareCleanup(CleanupKind kind, std::optional<PeerToken> peer) {
	if (!current() || (kind != CleanupKind::EditHistory && kind != CleanupKind::DeletedMessages)) return std::nullopt;
	auto chat = QByteArray();
	if (peer) {
		const auto name = PeerTokenText(*peer);
		if (!name || !peer->id) return std::nullopt;
		chat = token("chat", name->toLatin1());
		if (chat.isEmpty()) return std::nullopt;
	}
	const auto serial = captureSerial();
	if (!serial) return std::nullopt;
	return CleanupTicket{ id(), std::move(chat), kind, *serial };
}

CleanupResult Store::cleanupPage(const CleanupTicket &ticket) {
	if (!current() || ticket.store != id()) return { .effect = { u"scope_retired"_q } };
	if (const auto failure = physicalLimit(0); !failure.isEmpty()) return { .effect = { failure } };
	const auto edit = ticket.kind == CleanupKind::EditHistory;
	const auto entries = edit
		? _db.Read({ u"SELECT v.message_key,v.seq FROM versions v JOIN messages m USING(message_key) "
			"WHERE v.serial<=? AND v.seq<>m.head_seq AND (length(?)=0 OR m.chat_key=?) "
			"ORDER BY v.serial,v.message_key,v.seq LIMIT ?"_q, { ticket.cutoff, ticket.chat, ticket.chat, kPage+1 }, kRows })
		: _db.Read({ u"SELECT message_key FROM messages WHERE status<>0 AND serial<=? AND head_seq IS NOT NULL "
			"AND (length(?)=0 OR chat_key=?) ORDER BY serial,message_key LIMIT ?"_q,
			{ ticket.cutoff, ticket.chat, ticket.chat, kPage+1 }, kRows });
	if (!entries.ok) return { .effect = { u"cleanup_blocked"_q } };
	const auto count = std::min<size_t>(kPage, entries.rows.size());
	if (!count) return { .complete = true };
	if (!std::exchange(*ticket.started, true)) _options.lease->mediaRevision.fetch_add(1);
	auto program = Begin();
	for (auto i = size_t(0); i != count; ++i) {
		const auto &selected = entries.rows[i];
		const auto key = selected.empty() ? nullptr : std::get_if<QByteArray>(&selected.front());
		if (!key) return { .effect = { u"cleanup_blocked"_q } };
		auto row = readHead(*key, {});
		if (!row || !row->head) return { .effect = { u"cleanup_blocked"_q } };
		if (!ticket.chat.isEmpty()) {
			const auto identity = ParseIdentity(row->identity);
			const auto peer = identity ? PeerTokenText(identity->peer) : std::nullopt;
			if (!peer || token("chat", peer->toLatin1()) != ticket.chat) {
				return { .effect = { u"cleanup_blocked"_q } };
			}
		}
		if (edit) {
			const auto seq = selected.size() == 2 ? std::get_if<int64>(&selected.back()) : nullptr;
			if (!seq || *seq == row->sequence) return { .effect = { u"cleanup_blocked"_q } };
			program.statements.push_back({ u"INSERT INTO purged_edits SELECT event_key,message_key FROM versions "
				"WHERE message_key=? AND seq=? AND serial<=?"_q, { *key, *seq, ticket.cutoff }, kOne });
			program.statements.push_back({ u"DELETE FROM versions WHERE message_key=? AND seq=? AND serial<=?"_q,
				{ *key, *seq, ticket.cutoff }, kOne });
		} else {
			if (row->serial > ticket.cutoff || row->head->status == MessageStatus::Live) {
				return { .effect = { u"cleanup_blocked"_q } };
			}
			row->head->bodyTag.clear();
			row->sequence = 0;
			row->emptied = true;
			const auto sealed = sealMessage(*key, *row);
			if (!sealed) return { .effect = { u"cleanup_blocked"_q } };
			program.statements.push_back({ u"UPDATE messages SET head_seq=NULL,sealed_identity=?,cleared=1 WHERE message_key=?"_q,
				{ *sealed, *key }, kOne });
			program.statements.push_back({ u"DELETE FROM versions WHERE message_key=? AND serial<=?"_q,
				{ *key, ticket.cutoff }, kChanges });
		}
	}
	program.statements.push_back({ u"COMMIT"_q });
	const auto result = _db.Execute(program, [this] { return current(); });
	if (!result.committed) {
		return { .effect = { result.diskFull() ? u"disk_full"_q : u"cleanup_blocked"_q } };
	}
	return { .effect = { {}, true }, .removed = int(count), .complete = count == entries.rows.size() };
}

}
