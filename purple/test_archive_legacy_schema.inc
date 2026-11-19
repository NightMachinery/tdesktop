std::vector<QString> LegacySchema() {
	return {
		u"CREATE TABLE archive_meta(singleton INTEGER PRIMARY KEY CHECK(singleton=1),schema_version INTEGER NOT NULL,"
		"account_token TEXT NOT NULL,store_id TEXT NOT NULL,revision TEXT NOT NULL,generation TEXT NOT NULL,"
		"maintenance_id TEXT NOT NULL,envelope BLOB NOT NULL,schema_fingerprint TEXT NOT NULL)"_q,
		u"CREATE TABLE messages(position INTEGER PRIMARY KEY,message_key TEXT UNIQUE NOT NULL,chat_kind INTEGER NOT NULL,"
		"chat_token TEXT NOT NULL,message_id INTEGER NOT NULL,deleted INTEGER NOT NULL,purged INTEGER NOT NULL,"
		"transcript_date INTEGER NOT NULL,head_version TEXT NOT NULL,causal_order TEXT NOT NULL,envelope BLOB NOT NULL,"
		"topic_token TEXT NOT NULL DEFAULT '',collection INTEGER NOT NULL DEFAULT 0,representation_token TEXT NOT NULL DEFAULT '')"_q,
		u"CREATE INDEX deleted_transcript ON messages(chat_token,deleted,purged,transcript_date DESC,message_id DESC,message_key DESC)"_q,
		u"CREATE INDEX deleted_topic ON messages(chat_token,topic_token,deleted,purged,transcript_date,message_id,message_key)"_q,
		u"CREATE INDEX message_purge ON messages(chat_token,message_key)"_q,
		u"CREATE TABLE versions(message_key TEXT NOT NULL,version_id TEXT NOT NULL,order_key TEXT NOT NULL,"
		"chat_token TEXT NOT NULL,envelope BLOB NOT NULL,PRIMARY KEY(message_key,version_id),"
		"FOREIGN KEY(message_key) REFERENCES messages(message_key) ON DELETE CASCADE)"_q,
		u"CREATE TABLE body_refs(message_key TEXT NOT NULL,version_id TEXT NOT NULL,blob_id TEXT NOT NULL,bytes INTEGER NOT NULL,"
		"chunk_count INTEGER NOT NULL,PRIMARY KEY(message_key,version_id),"
		"FOREIGN KEY(message_key,version_id) REFERENCES versions(message_key,version_id) ON DELETE CASCADE)"_q,
		u"CREATE TABLE body_chunks(message_key TEXT NOT NULL,version_id TEXT NOT NULL,chunk_index INTEGER NOT NULL,envelope BLOB NOT NULL,"
		"PRIMARY KEY(message_key,version_id,chunk_index),FOREIGN KEY(message_key,version_id) REFERENCES body_refs(message_key,version_id) ON DELETE CASCADE)"_q,
		u"CREATE INDEX version_cursor ON versions((message_key||'/'||version_id))"_q,
		u"CREATE INDEX version_order ON versions(message_key,order_key DESC)"_q,
		u"CREATE INDEX version_chat ON versions(chat_token,message_key,version_id)"_q,
		u"CREATE TABLE events(position INTEGER PRIMARY KEY,message_key TEXT NOT NULL,event_id TEXT NOT NULL,fingerprint TEXT NOT NULL,envelope BLOB NOT NULL,"
		"UNIQUE(message_key,event_id),FOREIGN KEY(message_key) REFERENCES messages(message_key) ON DELETE CASCADE)"_q,
		u"CREATE INDEX event_cursor ON events((message_key||'/'||event_id))"_q,
		u"CREATE TABLE blobs(position INTEGER PRIMARY KEY,blob_id TEXT UNIQUE NOT NULL,bytes TEXT NOT NULL,state INTEGER NOT NULL,envelope BLOB NOT NULL)"_q,
		u"CREATE INDEX blob_state ON blobs(state,blob_id)"_q,
		u"CREATE TABLE media_refs(message_key TEXT NOT NULL,version_id TEXT NOT NULL,media_token TEXT NOT NULL,"
		"position INTEGER NOT NULL,state INTEGER NOT NULL,blob_id TEXT,PRIMARY KEY(message_key,version_id,media_token),"
		"FOREIGN KEY(message_key,version_id) REFERENCES versions(message_key,version_id) ON DELETE CASCADE,"
		"FOREIGN KEY(blob_id) REFERENCES blobs(blob_id) DEFERRABLE INITIALLY DEFERRED)"_q,
		u"CREATE INDEX blob_references ON media_refs(blob_id)"_q,
		u"CREATE INDEX media_copies ON media_refs(media_token,state,blob_id)"_q,
		u"CREATE TABLE jobs(position INTEGER PRIMARY KEY,job_id TEXT UNIQUE NOT NULL,message_key TEXT NOT NULL,version_id TEXT NOT NULL,"
		"chat_token TEXT NOT NULL,state INTEGER NOT NULL,generation TEXT NOT NULL,chat_generation TEXT NOT NULL,envelope BLOB NOT NULL)"_q,
		u"CREATE INDEX pending_jobs ON jobs(chat_token,state,job_id)"_q,
		u"CREATE TABLE leases(position INTEGER PRIMARY KEY,lease_id TEXT UNIQUE NOT NULL,blob_id TEXT NOT NULL,chat_token TEXT NOT NULL,"
		"revoked INTEGER NOT NULL,envelope BLOB NOT NULL,FOREIGN KEY(blob_id) REFERENCES blobs(blob_id) DEFERRABLE INITIALLY DEFERRED)"_q,
		u"CREATE INDEX blob_readers ON leases(blob_id)"_q,
		u"CREATE INDEX chat_readers ON leases(chat_token,revoked,lease_id)"_q,
		u"CREATE TABLE purge_epochs(position INTEGER PRIMARY KEY,chat_token TEXT UNIQUE NOT NULL,generation TEXT NOT NULL,envelope BLOB NOT NULL)"_q,
		u"CREATE TABLE maintenance(singleton INTEGER PRIMARY KEY CHECK(singleton=1),operation_id TEXT NOT NULL,kind INTEGER NOT NULL,"
		"scope_token TEXT NOT NULL,phase INTEGER NOT NULL,cursor TEXT NOT NULL,envelope BLOB NOT NULL)"_q,
		u"CREATE TABLE capture_gaps(gap_id TEXT PRIMARY KEY,observed_at INTEGER NOT NULL,reason TEXT NOT NULL,"
		"fingerprint TEXT NOT NULL,envelope BLOB NOT NULL)"_q,
		u"CREATE INDEX gap_order ON capture_gaps(observed_at,gap_id)"_q,
	};
}
