#include "purple/purple_archive_v2_schema.h"

namespace Purple::ArchiveV2 {

ArchiveSqlProgram Schema() {
	return { ArchiveSqlStatus::Ready, {
		{ u"BEGIN IMMEDIATE"_q },
		{ uR"sql(CREATE TABLE meta (
			format INTEGER PRIMARY KEY CHECK(format = 2),
			store_id BLOB NOT NULL, sealed_header BLOB NOT NULL
		) STRICT)sql"_q },
		{ uR"sql(CREATE TABLE messages (
			message_key BLOB PRIMARY KEY NOT NULL, chat_key BLOB NOT NULL,
			nonchannel_lookup BLOB, class INTEGER NOT NULL,
			status INTEGER NOT NULL, last_activity INTEGER NOT NULL,
			head_seq INTEGER, sealed_identity BLOB NOT NULL
		) STRICT)sql"_q },
		{ uR"sql(CREATE TABLE versions (
			message_key BLOB NOT NULL REFERENCES messages ON DELETE CASCADE,
			seq INTEGER NOT NULL CHECK(seq > 0), event_key BLOB NOT NULL UNIQUE,
			observed_at INTEGER NOT NULL, body_tag BLOB NOT NULL, sealed_body BLOB NOT NULL,
			PRIMARY KEY(message_key, seq)
		) STRICT)sql"_q },
		{ uR"sql(CREATE TABLE blobs (
			blob_key BLOB PRIMARY KEY NOT NULL, size INTEGER NOT NULL CHECK(size > 0),
			sealed_manifest BLOB NOT NULL
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
		{ u"CREATE INDEX messages_lookup ON messages(nonchannel_lookup) WHERE nonchannel_lookup IS NOT NULL"_q },
		{ u"CREATE INDEX media_blob ON media(blob_key) WHERE blob_key IS NOT NULL"_q },
		{ uR"sql(CREATE TRIGGER media_release_delete AFTER DELETE ON media
			WHEN OLD.blob_key IS NOT NULL AND NOT EXISTS(SELECT 1 FROM media WHERE blob_key=OLD.blob_key)
			BEGIN DELETE FROM blobs WHERE blob_key=OLD.blob_key; END)sql"_q },
		{ uR"sql(CREATE TRIGGER media_release_update AFTER UPDATE OF blob_key ON media
			WHEN OLD.blob_key IS NOT NULL AND NEW.blob_key IS NOT OLD.blob_key
				AND NOT EXISTS(SELECT 1 FROM media WHERE blob_key=OLD.blob_key)
			BEGIN DELETE FROM blobs WHERE blob_key=OLD.blob_key; END)sql"_q },
		{ u"COMMIT"_q },
	} };
}

}
