#pragma once

#include "purple/purple_archive_crypto.h"
#include "purple/purple_archive_sqlite.h"
#include "purple/purple_archive_v2.h"
#include "purple/purple_archive_v2_attention.h"

#include <QtCore/QLockFile>
#include <atomic>
#include <span>

namespace Purple::ArchiveV2 {

enum class Phase {
	BeforeSqlCommit, AfterSqlCommit, BlobPart, BeforePublish, AfterPublish,
	BeforeDetach, AfterDetach, BeforeUnlink, AfterUnlink, CleanupCommit,
};

struct Lease {
	std::atomic<bool> live{true};
	std::atomic<uint64_t> mediaRevision{0};
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
	QString profilePath;
	QByteArray profileNamespace;
	std::vector<std::pair<QByteArray, QByteArray>> loginKeys;
	std::shared_ptr<Lease> lease = std::make_shared<Lease>();
	std::function<bool(Phase)> checkpoint;
	Policy policy;
	std::function<int64()> now;
	std::function<int64()> availableBytes;
};

struct StoreEffect {
	QString reason;
	bool committed = false;
	IncidentStage stage = IncidentStage::Capture;
	uint64_t count = 1;
	int64 reclaimed = 0;
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
	int64 serial = 0;
};

struct MediaStatus {
	int64 ordinal = 0;
	QByteArray description;
	int64 savedBytes = 0;
	QString reason;
	MediaKind kind = MediaKind::Other;
};

struct HistoryVersion {
	int64 sequence = 0;
	StoredBody value;
	std::vector<MediaStatus> media;
};

struct HistoryPage {
	MessageStatus status = MessageStatus::Live;
	bool missingPreimage = false;
	std::vector<HistoryVersion> rows;
	int64 before = 0;
	int64 statusAt = 0;
};

class TimelineCursor {
public:
	TimelineCursor() = default;
private:
	friend class Store;
	TimelineCursor(int64 day, QByteArray message, QByteArray scope, QByteArray peer, bool deleted)
		: activity(day), key(std::move(message)), store(std::move(scope)), chat(std::move(peer)), trash(deleted) {
	}
	int64 activity = 0;
	QByteArray key;
	QByteArray store;
	QByteArray chat;
	bool trash = false;
};

struct TimelineEntry {
	MessageIdentity identity;
	MessageStatus status = MessageStatus::Deleted;
	int64 observedAt = 0;
	std::optional<StoredBody> latest;
	TimelineCursor cursor;
	ChatClass kind = ChatClass::Unknown;
};

struct TimelinePage {
	std::vector<TimelineEntry> rows;
	std::optional<TimelineCursor> before;
};

enum class CleanupKind { EditHistory, DeletedMessages };

struct CleanupTicket {
private:
	friend class Store;
	CleanupTicket(QByteArray id, QByteArray peer, CleanupKind type, int64 serial)
		: store(std::move(id)), chat(std::move(peer)), kind(type), cutoff(serial) {
	}
	QByteArray store;
	QByteArray chat;
	CleanupKind kind;
	int64 cutoff;
	std::shared_ptr<bool> started = std::make_shared<bool>(false);
};

struct CleanupResult {
	StoreEffect effect;
	int removed = 0;
	bool complete = false;
};

struct BlobInput {
	QByteArray storeId;
	uint64_t revision = 0;
	MessageIdentity identity;
	int64 sequence = 0;
	int64 ordinal = 0;
	int64 size = 0;
	QByteArray digest;
	QByteArray source;
	std::function<QByteArray(qsizetype)> read;
	bool timed = false;
	[[nodiscard]] bool sameTarget(const BlobInput &other) const {
		return storeId == other.storeId && revision == other.revision && identity == other.identity
			&& sequence == other.sequence && ordinal == other.ordinal && source == other.source;
	}
};

struct Usage {
	int64 database = 0;
	int64 wal = 0;
	int64 sharedMemory = 0;
	int64 support = 0;
	int64 ordinary = 0;
	int64 timed = 0;
	[[nodiscard]] int64 physical() const { return database + wal + sharedMemory + support; }
};

// A bounded SQLite write plus its checkpoint, and page/index/encryption overhead.
inline constexpr auto kWalHeadroom = int64(16 * 1024 * 1024);
inline constexpr auto kMetadataHeadroom = int64(48 * 1024 * 1024);
inline constexpr auto kBlobFramingCredit = int64(4 * 1024 * 1024);

struct StorageSummary {
	Usage active;
	std::optional<int64> pending;
	int64 ordinaryCap = 0;
	int64 timedCap = 0;
	int64 ordinaryFileCap = 0;
	int64 overheadCap = 0;
	bool overLimit = false;
};

class ResetReview {
	friend class Store;
	ResetReview(QByteArray id, QByteArray identity, int64 size)
	: _id(std::move(id)), _directory(std::move(identity)), _bytes(size) {
	}
	QByteArray _id;
	QByteArray _directory;
	int64 _bytes;

public:
	[[nodiscard]] int64 bytes() const { return _bytes; }
};

class Store final {
public:
	explicit Store(StoreOptions options);
	~Store();
	Store(const Store &) = delete;
	Store &operator=(const Store &) = delete;

	[[nodiscard]] StoreEffect open();
	[[nodiscard]] StoreEffect capture(const CapturedEvent &event);
	[[nodiscard]] std::vector<StoreEffect> captureBatch(std::span<const CapturedEvent> events);
	void noteMediaFailure(const BlobInput &input, QStringView reason);
	[[nodiscard]] StoreEffect writeBlob(const BlobInput &input, const std::function<void()> &betweenParts = {});
	[[nodiscard]] bool maintenancePending() const;
	using ResetCheck = std::variant<ResetReview, QString>;
	[[nodiscard]] ResetCheck reviewReset(const QByteArray &expectedId) const;
	[[nodiscard]] StoreEffect reset(const ResetReview &review);
	[[nodiscard]] StoreEffect logout(const QByteArray &expectedId);
	[[nodiscard]] StoreEffect recover();
	[[nodiscard]] std::optional<StoredBody> body(const MessageIdentity &identity, int64 sequence);
	[[nodiscard]] std::optional<std::vector<MediaStatus>> media(const MessageIdentity &identity, int64 sequence);
	[[nodiscard]] std::optional<int64> mediaSequence(const MessageIdentity &identity,
		const QByteArray &source, bool timed);
	[[nodiscard]] bool readSaved(const QByteArray &expected, const MessageIdentity &identity, int64 sequence,
		int64 ordinal, const std::function<bool(const QByteArray &)> &sink);
	[[nodiscard]] QByteArray id() const;
	[[nodiscard]] bool current() const;
	[[nodiscard]] std::optional<Usage> usage();
	[[nodiscard]] std::optional<StorageSummary> storageSummary();
	[[nodiscard]] std::optional<int64> pendingBytes() const;
	[[nodiscard]] StoreEffect maintain();
	[[nodiscard]] std::optional<int64> estimateExpired(const Policy &policy);
	[[nodiscard]] StoreEffect updatePolicy(Policy policy);
	[[nodiscard]] std::optional<HistoryPage> history(const MessageIdentity &identity, int64 before, int limit);
	[[nodiscard]] std::optional<TimelinePage> timeline(std::optional<PeerToken> peer,
		bool deletedOnly, std::optional<TimelineCursor> before, int limit);
	[[nodiscard]] std::optional<CleanupTicket> prepareCleanup(CleanupKind kind, std::optional<PeerToken> peer = {});
	[[nodiscard]] CleanupResult cleanupPage(const CleanupTicket &ticket);

private:
	struct Retired {
		QByteArray id;
		QByteArray directory;
	};
	struct Pointer {
		QByteArray id;
		QByteArray wrapped;
		QByteArray directory;
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
		int64 serial = 0;
		QByteArray identity;
		bool missingPreimage = false;
		bool emptied = false;
	};

	[[nodiscard]] bool step(Phase phase) const;
	[[nodiscard]] QString bind(bool create);
	[[nodiscard]] QString readPointer();
	[[nodiscard]] bool ownsPointer() const;
	[[nodiscard]] bool publish(const Pointer &next);
	[[nodiscard]] bool makeActive(Pointer &next);
	[[nodiscard]] std::optional<QByteArray> nativeWrap(const QByteArray &id,
		const QByteArray &wrapped, const QByteArray *dataKey = nullptr);
	[[nodiscard]] QString cleanup();
	[[nodiscard]] StoreEffect retire(const QByteArray &expectedId, const ResetReview *review);
	[[nodiscard]] QString archivePath(const QByteArray &id, bool detached = false) const;
	[[nodiscard]] QString pointerPath() const;
	[[nodiscard]] QString directory() const;
	[[nodiscard]] std::optional<QByteArray> seal(QStringView key, const QByteArray &context, const QByteArray &plain);
	[[nodiscard]] std::optional<QByteArray> unseal(QStringView key, const QByteArray &context, const QByteArray &data);
	[[nodiscard]] QByteArray token(const QByteArray &type, const QByteArray &identity) const;
	[[nodiscard]] QByteArray associated(const QByteArray &type, const QByteArray &key) const;
	struct SavedBlob {
		QByteArray key;
		QByteArray digest;
		int64 size = 0;
		int64 parts = 0;
	};
	[[nodiscard]] std::optional<SavedBlob> savedBlob(const QByteArray &key, int64 sequence, int64 ordinal);
	[[nodiscard]] static std::optional<MessageIdentity> parseIdentity(const QByteArray &identity);
	[[nodiscard]] std::optional<MessageIdentity> resolveDelete(uint64_t message);
	[[nodiscard]] static QByteArray encodeIdentity(const MessageIdentity &identity);
	[[nodiscard]] std::optional<Row> readHead(const QByteArray &key, const QByteArray &identity);
	[[nodiscard]] std::optional<QByteArray> sealMessage(const QByteArray &key, const Row &row);
	[[nodiscard]] std::optional<int64> captureSerial();
	[[nodiscard]] QString openDatabase(const QByteArray &dataKey);
	[[nodiscard]] StoreEffect write(ArchiveSqlProgram program);
	[[nodiscard]] StoreEffect secureWrite(ArchiveSqlProgram program);
	[[nodiscard]] StoreEffect scrubParts(bool drain);
	[[nodiscard]] StoreEffect captureOne(const CapturedEvent &event);
	[[nodiscard]] std::optional<std::pair<int64, int64>> totals(bool budget = false);
	[[nodiscard]] QString reserve(int64 logical, int64 framing, bool timed, const QByteArray &keep,
		const QByteArray &keepBlob, const std::function<void()> &betweenParts);
	[[nodiscard]] QString expire(int64 day, const QByteArray &keep = {});
	[[nodiscard]] QString trim(int64 bytes, bool timed, const QByteArray &keep, const QByteArray &keepBlob = {},
		int64 framing = 0, int64 freePages = 0);
	[[nodiscard]] QString physicalLimit(int64 extra, bool reclaiming = false, int64 *freePages = nullptr);
	[[nodiscard]] QString reservePages(int64 bytes, bool timed, const QByteArray &keep = {}, const QByteArray &blob = {});
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
	QString _bindFailure;
	int64 _lastTrimDay = -1;
	int64 _pageLimit = 0;
	bool _trimPending = true;
	bool _vacuumPending = false;
	bool _scrubPending = false;
	QByteArray _mediaKeep;
	const DesktopArchiveSqlite::Write *_batch = nullptr;
};

}
