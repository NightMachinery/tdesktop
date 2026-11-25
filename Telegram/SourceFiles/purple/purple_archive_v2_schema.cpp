#include "purple/purple_archive_v2_schema.h"

namespace Purple::ArchiveV2 {

ArchiveSqlProgram Schema() {
	auto plan = ArchiveSqlProgram{ ArchiveSqlStatus::Ready, {
		{ u"BEGIN IMMEDIATE"_q },
		{ uR"sql(CREATE TABLE meta (
			format INTEGER PRIMARY KEY CHECK(format = 2),
			store_id BLOB NOT NULL, sealed_header BLOB NOT NULL, serial INTEGER NOT NULL CHECK(serial>=0),
			ordinary INTEGER NOT NULL CHECK(ordinary>=0), timed INTEGER NOT NULL CHECK(timed>=0)
		) STRICT)sql"_q },
		{ uR"sql(CREATE TABLE messages (
			message_key BLOB PRIMARY KEY NOT NULL, chat_key BLOB NOT NULL,
			nonchannel_lookup BLOB, class INTEGER NOT NULL,
			status INTEGER NOT NULL, last_activity INTEGER NOT NULL,
			head_seq INTEGER, sealed_identity BLOB NOT NULL, serial INTEGER NOT NULL CHECK(serial>0),
			cleared INTEGER NOT NULL CHECK(cleared IN (0,1))
		) STRICT)sql"_q },
		{ uR"sql(CREATE TABLE versions (
			message_key BLOB NOT NULL REFERENCES messages ON DELETE CASCADE,
			seq INTEGER NOT NULL CHECK(seq > 0), event_key BLOB NOT NULL UNIQUE,
			observed_at INTEGER NOT NULL, body_tag BLOB NOT NULL, sealed_body BLOB NOT NULL,
			serial INTEGER NOT NULL CHECK(serial>0),
			PRIMARY KEY(message_key, seq)
		) STRICT)sql"_q },
		{ uR"sql(CREATE TABLE purged_edits (
			event_key BLOB PRIMARY KEY NOT NULL,
			message_key BLOB NOT NULL REFERENCES messages ON DELETE CASCADE
		) STRICT)sql"_q },
		{ u"CREATE INDEX purged_edits_message ON purged_edits(message_key)"_q },
		{ u"CREATE INDEX messages_timeline ON messages(chat_key,last_activity,message_key)"_q },
		{ uR"sql(CREATE TABLE blobs (
			blob_key BLOB PRIMARY KEY NOT NULL, size INTEGER NOT NULL CHECK(size > 0),
			sealed_manifest BLOB, timed INTEGER NOT NULL CHECK(timed IN (0,1))
		) STRICT)sql"_q },
		{ uR"sql(CREATE TABLE blob_parts (
			blob_key BLOB NOT NULL REFERENCES blobs ON DELETE CASCADE,
			part_no INTEGER NOT NULL CHECK(part_no >= 0), sealed_bytes BLOB NOT NULL,
			PRIMARY KEY(blob_key, part_no)
		) STRICT)sql"_q },
		{ uR"sql(CREATE TABLE media (
			message_key BLOB NOT NULL, seq INTEGER NOT NULL, ordinal INTEGER NOT NULL,
			media_key BLOB NOT NULL, kind INTEGER NOT NULL,
			timed INTEGER NOT NULL CHECK(timed IN (0,1)),
			state INTEGER NOT NULL, reason INTEGER NOT NULL,
			sealed_descriptor BLOB NOT NULL, blob_key BLOB REFERENCES blobs, sealed_blob_ref BLOB,
			CHECK((blob_key IS NULL) = (sealed_blob_ref IS NULL)),
			PRIMARY KEY(message_key, seq, ordinal),
			FOREIGN KEY(message_key, seq) REFERENCES versions ON DELETE CASCADE
		) STRICT)sql"_q },
		{ u"CREATE INDEX messages_chat ON messages(chat_key,status,last_activity,message_key)"_q },
		{ u"CREATE INDEX messages_status ON messages(status,last_activity,message_key)"_q },
		{ u"CREATE INDEX messages_deleted_page ON messages(last_activity DESC,message_key DESC) WHERE status<>0 AND cleared=0"_q },
		{ u"CREATE INDEX messages_lookup ON messages(nonchannel_lookup) WHERE nonchannel_lookup IS NOT NULL"_q },
		{ u"CREATE INDEX media_blob ON media(blob_key) WHERE blob_key IS NOT NULL"_q },
		{ u"CREATE INDEX blobs_bucket ON blobs(timed,blob_key)"_q },
		{ u"CREATE INDEX versions_cleanup ON versions(serial,message_key,seq)"_q },
		{ u"CREATE INDEX messages_cleanup ON messages(serial,message_key) WHERE status<>0 AND head_seq IS NOT NULL"_q },
		{ u"CREATE INDEX messages_age ON messages(last_activity,message_key)"_q },
	} };
	for (const auto &[table, ordinary, timed] : std::array<std::array<QStringView, 3>, 5>{{
		{ u"messages", u"length(R.sealed_identity)+160", u"0" },
		{ u"versions", u"length(R.sealed_body)+144", u"0" },
		{ u"media", u"length(R.sealed_descriptor)+144", u"0" },
		{ u"purged_edits", u"128", u"0" },
		{ u"blobs", u"R.size*(1-R.timed)", u"R.size*R.timed" },
	}}) {
		for (const auto &op : { u"INSERT"_q, u"UPDATE"_q, u"DELETE"_q }) {
			const auto delta = [&](QStringView expr) {
				const auto row = [&](QStringView side) { return expr.toString().replace(u"R."_q, side.toString() + '.'); };
				return (op == u"DELETE"_q ? u"0"_q : row(u"NEW")) + u"-("_q
					+ (op == u"INSERT"_q ? u"0"_q : row(u"OLD")) + ')';
			};
			plan.statements.push_back({ u"CREATE TRIGGER %1_usage_%2 AFTER %2 ON %1 BEGIN "
				"UPDATE meta SET ordinary=ordinary+(%3),timed=timed+(%4); END"_q
					.arg(table, op, delta(ordinary), delta(timed)) });
		}
	}
	for (const auto &op : { u"DELETE"_q, u"UPDATE OF blob_key"_q }) {
		plan.statements.push_back({ u"CREATE TRIGGER media_release_%1 AFTER %2 ON media "
			"WHEN OLD.blob_key IS NOT NULL BEGIN "
			"DELETE FROM blobs WHERE blob_key=OLD.blob_key "
			"AND NOT EXISTS(SELECT 1 FROM media WHERE blob_key=OLD.blob_key); "
			"UPDATE blobs SET timed=0 WHERE blob_key=OLD.blob_key AND timed=1 "
			"AND NOT EXISTS(SELECT 1 FROM media WHERE blob_key=OLD.blob_key AND timed=1); END"_q
				.arg(op.left(6), op) });
	}
	plan.statements.push_back({ u"COMMIT"_q });
	return plan;
}

}
