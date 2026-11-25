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
	Policy policy;
	std::function<int64()> now;
	std::function<int64()> availableBytes;
	std::function<bool()> canWritePart;
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

struct Usage {
	int64 database = 0;
	int64 wal = 0;
	int64 sharedMemory = 0;
	int64 support = 0;
	int64 ordinary = 0;
	int64 timed = 0;
	int64 reclaimed = 0;
	[[nodiscard]] int64 physical() const { return database + wal + sharedMemory + support; }
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
	[[nodiscard]] std::optional<Usage> usage();
	[[nodiscard]] StoreEffect maintain();
	[[nodiscard]] StoreEffect updatePolicy(Policy policy);

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
	[[nodiscard]] QString reserve(int64 logical, int64 physical, bool timed, const QByteArray &keep,
		const QByteArray &keepBlob = {});
	[[nodiscard]] QString expire(int64 day, const QByteArray &keep = {});
	[[nodiscard]] QString trim(int64 bytes, bool timed, const QByteArray &keep, const QByteArray &keepBlob = {}, bool forceOne = false);
	[[nodiscard]] QString physicalLimit(int64 extra);
	[[nodiscard]] std::optional<int64> pragma(const QString &query);
	[[nodiscard]] int64 availableBytes() const;
	[[nodiscard]] int64 today() const;

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
	int64 _lastTrimDay = -1;
	int64 _reclaimed = 0;
};

}
