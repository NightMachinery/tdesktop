#pragma once

#include "purple/purple_archive_crypto.h"
#include "purple/purple_archive_sqlite.h"
#include "purple/purple_archive_v2.h"

#include <QtCore/QLockFile>
#include <atomic>

namespace Purple::ArchiveV2 {

enum class Phase {
	BeforeSqlCommit, AfterSqlCommit, BlobPart, BeforePublish, AfterPublish,
	BeforeDetach, AfterDetach, BeforeUnlink, AfterUnlink,
};

struct Lease {
	std::atomic<bool> live{true};
	DesktopArchiveCrypto crypto;
	void revoke();
};

struct StoreOptions {
	QString root;
	QByteArray profileId;
	QByteArray accountSlot;
	AccountKey account;
	QByteArray profileKey;
	QByteArray loginSecret;
	std::shared_ptr<Lease> lease = std::make_shared<Lease>();
	std::function<bool(Phase)> checkpoint;
};

struct StoreEffect {
	QString reason;
	bool committed = false;
	[[nodiscard]] explicit operator bool() const { return reason.isEmpty(); }
};

struct Attachment {
	QByteArray source;
	QByteArray description;
	int kind = 0;
	bool timed = false;
};

struct CapturedEvent {
	QByteArray storeId;
	MessageIdentity identity;
	ChatClass kind = ChatClass::Unknown;
	Observation event = Observation::Baseline;
	QByteArray eventId;
	QByteArray body;
	QByteArray preimage;
	int64 observedAt = 0;
	bool fillsMissing = false;
	std::vector<Attachment> attachments;
};

struct StoredBody {
	int64 observedAt = 0;
	QByteArray body;
};

struct BlobInput {
	QByteArray storeId;
	MessageIdentity identity;
	int64 sequence = 0;
	int64 ordinal = 0;
	int64 size = 0;
	QByteArray digest;
	QByteArray source;
	std::function<QByteArray(qsizetype)> read;
};

class Store final {
public:
	explicit Store(StoreOptions options);
	~Store();
	Store(const Store &) = delete;
	Store &operator=(const Store &) = delete;

	[[nodiscard]] StoreEffect open();
	[[nodiscard]] StoreEffect capture(const CapturedEvent &event);
	[[nodiscard]] StoreEffect writeBlob(const BlobInput &input);
	[[nodiscard]] StoreEffect reset(const QByteArray &expectedId);
	[[nodiscard]] StoreEffect logout(const QByteArray &expectedId);
	[[nodiscard]] StoreEffect recover();
	[[nodiscard]] std::optional<StoredBody> body(const MessageIdentity &identity, int64 sequence);
	[[nodiscard]] QByteArray id() const;
	[[nodiscard]] bool current() const;

private:
	struct Retired {
		QByteArray id;
		QByteArray directory;
	};
	struct Pointer {
		QByteArray id;
		QByteArray wrapped;
		int64 generation = 0;
		bool initialized = false;
		std::vector<Retired> retired;
	};
	struct Row {
		std::optional<Head> head;
		int64 sequence = 0;
		int64 activity = 0;
		int64 observedAt = 0;
		int64 kind = 0;
	};

	[[nodiscard]] bool step(Phase phase) const;
	[[nodiscard]] QString bind(bool create);
	[[nodiscard]] QString readPointer();
	[[nodiscard]] bool ownsPointer() const;
	[[nodiscard]] bool publish(const Pointer &next);
	[[nodiscard]] bool makeActive(Pointer &next);
	[[nodiscard]] QString cleanup();
	[[nodiscard]] StoreEffect retire(const QByteArray &expectedId, bool replace);
	[[nodiscard]] QString archivePath(const QByteArray &id, bool detached = false) const;
	[[nodiscard]] QString pointerPath() const;
	[[nodiscard]] QString directory() const;
	[[nodiscard]] std::optional<QByteArray> seal(QStringView key, const QByteArray &context, const QByteArray &plain);
	[[nodiscard]] std::optional<QByteArray> unseal(QStringView key, const QByteArray &context, const QByteArray &data);
	[[nodiscard]] QByteArray token(const QByteArray &type, const QByteArray &identity) const;
	[[nodiscard]] QByteArray associated(const QByteArray &type, const QByteArray &key) const;
	[[nodiscard]] std::optional<Row> readHead(const QByteArray &key, const QByteArray &identity);
	[[nodiscard]] QString openDatabase(const QByteArray &dataKey);
	[[nodiscard]] StoreEffect write(ArchiveSqlProgram program);

	StoreOptions _options;
	QByteArray _scope;
	QByteArray _manifestKey;
	QString _root;
	QByteArray _rootIdentity;
	QByteArray _published;
	Pointer _pointer;
	std::unique_ptr<QLockFile> _lock;
	DesktopArchiveSqlite _db;
	bool _ready = false;
	bool _bound = false;
};

}
