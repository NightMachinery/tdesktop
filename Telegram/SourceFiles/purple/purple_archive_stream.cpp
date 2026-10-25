#include "purple/purple_archive_stream.h"

#include "purple/purple_archive_files.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <algorithm>

namespace Purple {
namespace {

QString Hash(const QByteArray &bytes) {
	return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

ArchiveSqlProgram Program(std::vector<ArchiveSqlStatement> statements) {
	statements.insert(statements.begin(), {u"BEGIN IMMEDIATE"_q});
	statements.push_back({u"COMMIT"_q});
	return {ArchiveSqlStatus::Ready, std::move(statements)};
}

template <typename T>
const T *Cell(const DesktopArchiveSqlResult &result, int column) {
	return result.ok && result.rows.size() == 1
		&& result.rows.front().size() > size_t(column)
		? std::get_if<T>(&result.rows.front()[column]) : nullptr;
}

}

class DesktopArchiveStream::Impl {
public:
	DesktopArchiveStore &store;
	ArchiveStagingBinding binding;
	ArchiveStagingHeader header;
	DesktopArchiveSqlite index;
	QString root;
	QString token;
	bool ready = false;
	Impl(DesktopArchiveStore &store, ArchiveStagingBinding binding);
	[[nodiscard]] std::optional<ArchiveSqlSlice> State();
	[[nodiscard]] std::optional<ArchiveStagingProof> Proof(uint64_t page);
	[[nodiscard]] std::optional<ArchiveStagingCoveragePage> Page(uint64_t page, QByteArray *frame = nullptr);
	[[nodiscard]] bool Reload();
	[[nodiscard]] bool Commit(const ArchiveStagingUpdate &update);
	[[nodiscard]] bool Piece(const ArchiveStagingRange &range, QByteArray &frame);

};

DesktopArchiveStream::Impl::Impl(DesktopArchiveStore &store, ArchiveStagingBinding binding)
: store(store)
, binding(std::move(binding)) {
}

std::optional<ArchiveSqlSlice> DesktopArchiveStream::Impl::State() {
	auto slice = store.ReadSlice(binding.source.key, {}, binding.source.version);
	return slice && CheckArchiveStagingBinding(binding, slice->state, store.epoch()) == ArchiveStagingStatus::Ready
		? std::move(slice) : std::nullopt;
}

std::optional<ArchiveStagingProof> DesktopArchiveStream::Impl::Proof(uint64_t page) {
	auto proof = ArchiveStagingEmptyProof();
	for (auto level = 0; level != kArchiveStagingProofDepth; ++level) {
		const auto result = index.Read({u"SELECT hash FROM nodes WHERE level=? AND position=?"_q,
			{int64(level), QString::number((page >> level) ^ 1)}, ArchiveSqlExpectation::Rows});
		if (!result.ok || result.rows.size() > 1) {
			return {};
		}
		if (!result.rows.empty()) {
			const auto hash = Cell<QString>(result, 0);
			if (!hash || !IsArchiveHash(*hash)) {
				return {};
			}
			proof.siblings[level] = *hash;
		}
	}
	return proof;
}

std::optional<ArchiveStagingCoveragePage> DesktopArchiveStream::Impl::Page(uint64_t page, QByteArray *frame) {
	const auto result = index.Read({u"SELECT checkpoint,CASE WHEN length(frame)<=69632 THEN frame END FROM pages WHERE position=?"_q,
		{QString::number(page)}, ArchiveSqlExpectation::OneRow});
	const auto sequence = Cell<QString>(result, 0);
	const auto bytes = Cell<QByteArray>(result, 1);
	auto ok = false;
	const auto value = sequence ? sequence->toULongLong(&ok) : 0;
	if (!ok || QString::number(value) != *sequence || value > header.checkpoint
		|| !bytes || bytes->size() > kArchiveStagingRecordByteLimit + 4096) {
		return {};
	}
	const auto decoded = OpenArchiveStagingRecord(binding,
		{ArchiveStagingRecordKind::CoveragePage, page, value}, *bytes, store.security());
	if (!decoded || !decoded.record || !std::holds_alternative<ArchiveStagingCoveragePage>(*decoded.record)) {
		return {};
	}
	if (frame) {
		*frame = *bytes;
	}
	return std::get<ArchiveStagingCoveragePage>(*decoded.record);
}

bool DesktopArchiveStream::Impl::Reload() {
	const auto result = index.Read({u"SELECT checkpoint,CASE WHEN length(frame)<=69632 THEN frame END FROM progress WHERE singleton=1"_q,
		{}, ArchiveSqlExpectation::OneRow});
	const auto sequence = Cell<QString>(result, 0);
	const auto bytes = Cell<QByteArray>(result, 1);
	auto ok = false;
	const auto value = sequence ? sequence->toULongLong(&ok) : 0;
	if (!ok || QString::number(value) != *sequence || !bytes
		|| bytes->size() > kArchiveStagingRecordByteLimit + 4096) {
		return false;
	}
	const auto decoded = OpenArchiveStagingRecord(binding,
		{ArchiveStagingRecordKind::Header, 0, value}, *bytes, store.security());
	if (!decoded || !decoded.record || !std::holds_alternative<ArchiveStagingHeader>(*decoded.record)) {
		return false;
	}
	header = std::get<ArchiveStagingHeader>(*decoded.record);
	return State().has_value();
}

bool DesktopArchiveStream::Impl::Piece(const ArchiveStagingRange &range, QByteArray &frame) {
	const auto context = ArchiveStagingPieceContext(binding, range, store.security());
	const auto bytes = context ? DesktopArchiveRead(root + '/' + QString::fromUtf8(context->rowId),
		kArchiveStagingWindowBytes + 4096) : std::nullopt;
	if (!bytes) {
		return false;
	}
	frame = *bytes;
	return true;
}

bool DesktopArchiveStream::Impl::Commit(const ArchiveStagingUpdate &update) {
	const auto &security = store.security();
	const auto page = SealArchiveStagingRecord(binding, update.page, security);
	const auto delta = SealArchiveStagingRecord(binding, update.next, security);
	const auto headerFrame = SealArchiveStagingRecord(binding, update.next.header, security);
	const auto nodes = ArchiveStagingNodes(update.next);
	if (!page || !delta || !headerFrame || !nodes || !State()) {
		return false;
	}
	auto statements = std::vector<ArchiveSqlStatement>{
		{u"UPDATE progress SET checkpoint=?,frame=?,digest=? WHERE singleton=1 AND digest=?"_q,
			{QString::number(update.next.header.checkpoint), *headerFrame,
				ArchiveStagingHeaderHash(update.next.header), ArchiveStagingHeaderHash(header)},
			ArchiveSqlExpectation::OneChange},
		{u"INSERT INTO pages VALUES(?,?,?) ON CONFLICT(position) DO UPDATE SET checkpoint=excluded.checkpoint,frame=excluded.frame"_q,
			{QString::number(update.page.index), QString::number(update.page.checkpoint), *page}, ArchiveSqlExpectation::OneChange},
		{u"INSERT INTO deltas VALUES(?,?)"_q,
			{QString::number(update.next.header.checkpoint), *delta}, ArchiveSqlExpectation::OneChange}};
	auto changes = *nodes;
	changes.push_back({0, update.page.index, ArchiveStagingPageHash(update.page)});
	for (const auto &node : changes) {
		statements.push_back({u"INSERT INTO nodes VALUES(?,?,?) ON CONFLICT(level,position) DO UPDATE SET hash=excluded.hash"_q,
			{int64(node.level), QString::number(node.index), node.hash}, ArchiveSqlExpectation::OneChange});
	}
	return index.Execute(Program(std::move(statements))).committed && store.Current();
}

DesktopArchiveStream::DesktopArchiveStream(DesktopArchiveStore &store, ArchiveStagingBinding binding)
: _impl(std::make_unique<Impl>(store, std::move(binding))) {
}

DesktopArchiveStream::~DesktopArchiveStream() = default;

bool DesktopArchiveStream::Open() {
	auto &s = *_impl;
	const auto slice = s.State();
	const auto initial = slice ? BeginArchiveStaging(s.binding, slice->state, s.store.epoch()) : std::nullopt;
	s.token = ArchiveStagingBindingId(s.binding, s.store.security());
	if (!initial || s.token.isEmpty()) {
		return false;
	}
	s.root = s.store.directory() + u"/staging/stream-"_q + s.token;
	const auto existed = QFileInfo::exists(s.root);
	if ((!existed && !DesktopArchiveDirectory(s.root))
		|| !DesktopArchivePrivate(s.root, true)) {
		return false;
	}
	const auto path = s.root + u"/catalogue.sqlite"_q;
	s.index.SetActive([&s] { return s.store.Current(); });
	if ((!QFileInfo::exists(path) && !DesktopArchiveWrite(path, {}))
		|| !s.index.Open(path, false).ok || !DesktopArchivePrivate(path, false)
		|| !s.index.Configure().ok) {
		return false;
	}
	const auto schema = std::vector<QString>{
		u"CREATE TABLE progress(singleton INTEGER PRIMARY KEY CHECK(singleton=1),checkpoint TEXT NOT NULL,frame BLOB NOT NULL,digest TEXT NOT NULL)"_q,
		u"CREATE TABLE pages(position TEXT PRIMARY KEY,checkpoint TEXT NOT NULL,frame BLOB NOT NULL)"_q,
		u"CREATE TABLE deltas(checkpoint TEXT PRIMARY KEY,frame BLOB NOT NULL)"_q,
		u"CREATE TABLE nodes(level INTEGER NOT NULL,position TEXT NOT NULL,hash TEXT NOT NULL,PRIMARY KEY(level,position))"_q,
		u"CREATE TABLE frozen(checkpoint TEXT PRIMARY KEY,frame BLOB NOT NULL)"_q};
	const auto tableCount = s.index.Read({u"SELECT count(*) FROM sqlite_schema WHERE type='table'"_q, {}, ArchiveSqlExpectation::OneRow});
	const auto count = Cell<int64>(tableCount, 0);
	if (!count) {
		return false;
	}
	if (!*count) {
		const auto sealed = SealArchiveStagingRecord(s.binding, *initial, s.store.security());
		if (!sealed) {
			return false;
		}
		auto statements = std::vector<ArchiveSqlStatement>();
		for (const auto &sql : schema) {
			statements.push_back({sql});
		}
		statements.push_back({u"INSERT INTO progress VALUES(1,'0',?,?)"_q,
			{*sealed, ArchiveStagingHeaderHash(*initial)}, ArchiveSqlExpectation::OneChange});
		if (!s.index.Execute(Program(std::move(statements))).committed
			|| !DesktopArchiveSyncDirectory(s.root)) {
			return false;
		}
	}
	const auto actual = s.index.Read({u"SELECT sql FROM sqlite_schema WHERE type='table' ORDER BY name"_q,
		{}, ArchiveSqlExpectation::Rows});
	auto expected = schema;
	std::sort(expected.begin(), expected.end());
	auto observed = std::vector<QString>();
	for (const auto &row : actual.rows) {
		if (row.size() != 1 || !std::holds_alternative<QString>(row[0])) {
			return false;
		}
		observed.push_back(std::get<QString>(row[0]));
	}
	std::sort(observed.begin(), observed.end());
	s.ready = actual.ok && observed == expected && s.Reload();
	return s.ready;
}

ArchiveStagingStatus DesktopArchiveStream::Write(uint64_t offset, const QByteArray &bytes) {
	auto &s = *_impl;
	if (!s.ready || bytes.isEmpty() || bytes.size() > kArchiveMediaChunkBytes
		|| offset > s.binding.totalBytes || uint64_t(bytes.size()) > s.binding.totalBytes - offset || !s.Reload()) {
		return ArchiveStagingStatus::Invalid;
	}
	auto consumed = uint64_t(0);
	while (consumed < uint64_t(bytes.size())) {
		const auto size = ArchiveStagingInputSize(s.binding, offset + consumed, bytes.size() - consumed);
		const auto proof = s.Proof((offset + consumed) / kArchiveStagingWindowBytes);
		const auto slice = s.State();
		if (!size || !*size || !proof || !slice) {
			return ArchiveStagingStatus::Stale;
		}
		const auto index = (offset + consumed) / kArchiveStagingWindowBytes;
		const auto present = s.index.Read({u"SELECT position FROM pages WHERE position=?"_q,
			{QString::number(index)}, ArchiveSqlExpectation::Rows});
		if (!present.ok || present.rows.size() > 1) {
			return ArchiveStagingStatus::IntegrityFailure;
		}
		const auto page = present.rows.empty() ? std::optional<ArchiveStagingCoveragePage>() : s.Page(index);
		auto old = QByteArray();
		if ((!present.rows.empty() && !page) || (page && !s.Piece(page->ranges.front(), old))) {
			return ArchiveStagingStatus::IntegrityFailure;
		}
		const auto update = PlanArchiveStagingPiece(s.header, page, *proof, old,
			{offset + consumed, bytes.mid(consumed, *size)}, s.store.security(), slice->state, s.store.epoch());
		if (update.status == ArchiveStagingStatus::NoChange) {
			consumed += *size;
			continue;
		}
		if (!update) {
			return update.status;
		}
		const auto path = s.root + '/' + update.pieceId;
		if (!QFileInfo::exists(path) && !DesktopArchiveWrite(path, update.pieceFrame)) {
			return ArchiveStagingStatus::NotCommitted;
		}
		const auto frame = DesktopArchiveRead(path, kArchiveStagingWindowBytes + 4096);
		const auto context = ArchiveStagingPieceContext(s.binding, update.piece, s.store.security());
		const auto plain = frame && context ? OpenArchiveEnvelope(*context, s.store.security().keyId,
			*frame, s.store.security().crypto) : ArchiveEnvelopeResult();
		if (!plain || plain.bytes.size() != update.piece.length || Hash(plain.bytes) != update.piece.pieceHash) {
			return ArchiveStagingStatus::IntegrityFailure;
		}
		if (!s.Commit(update)) {
			return ArchiveStagingStatus::NotCommitted;
		}
		const auto committed = CompleteArchiveStagingPiece(s.header, update,
			{true, true, s.token, update.pieceId, ArchiveStagingHeaderHash(s.header),
				ArchiveStagingHeaderHash(update.next.header)}, s.store.security(), slice->state, s.store.epoch());
		if (!committed.header) {
			return committed.status;
		}
		s.header = *committed.header;
		consumed += *size;
	}
	return ArchiveStagingStatus::Ready;
}

bool DesktopArchiveStream::Complete() {
	auto &s = *_impl;
	const auto slice = s.State();
	const auto frozen = slice ? FreezeArchiveStagingCheckpoint(s.header, slice->state, s.store.epoch()) : std::nullopt;
	const auto frame = frozen ? SealArchiveStagingRecord(s.binding, *frozen, s.store.security()) : std::nullopt;
	if (!s.ready || !frozen || !frame || !s.index.Execute(Program({
		{u"INSERT INTO frozen VALUES(?,?) ON CONFLICT(checkpoint) DO NOTHING"_q,
			{QString::number(s.header.checkpoint), *frame}, ArchiveSqlExpectation::AtMostOneChange}})).committed) {
		return false;
	}
	const auto saved = s.index.Read({u"SELECT CASE WHEN length(frame)<=69632 THEN frame END FROM frozen WHERE checkpoint=?"_q,
		{QString::number(s.header.checkpoint)}, ArchiveSqlExpectation::OneRow});
	const auto bytes = Cell<QByteArray>(saved, 0);
	const auto snapshot = bytes ? OpenArchiveStagingRecord(s.binding, ArchiveStagingAddress(*frozen),
		*bytes, s.store.security()) : ArchiveStagingDecodeResult();
	if (!snapshot || !snapshot.record || !std::holds_alternative<ArchiveStagingCheckpoint>(*snapshot.record)) {
		return false;
	}
	auto start = BeginArchiveStagingFinalize(s.header, std::get<ArchiveStagingCheckpoint>(*snapshot.record),
		s.store.security(), slice->state, s.store.epoch());
	if (!start.session) {
		return false;
	}
	auto step = std::move(start.step);
	while (s.store.Current()) {
		const auto fresh = s.State();
		if (!fresh) {
			return false;
		}
		auto input = ArchiveStagingFinalizeInput();
		if (step.request.kind == ArchiveStagingRequestKind::Page) {
			auto bytes = QByteArray();
			const auto page = s.Page(step.request.pageIndex, &bytes);
			const auto proof = s.Proof(step.request.pageIndex);
			if (!page || !proof) {
				return false;
			}
			input = ArchiveStagingPageInput{step.request.id, page->checkpoint, bytes, *proof};
		} else if (step.request.kind == ArchiveStagingRequestKind::Piece) {
			auto bytes = QByteArray();
			if (!s.Piece(step.request.range, bytes)) {
				return false;
			}
			input = ArchiveStagingPieceInput{step.request.id, bytes};
		} else if (step.request.kind == ArchiveStagingRequestKind::Publish) {
			if (!step.publication || CheckArchiveStagingPublication(*step.publication, s.header,
				s.store.security(), fresh->state, s.store.epoch()) != ArchiveStagingStatus::Ready) {
				return false;
			}
			const auto &pub = *step.publication;
			const auto root = s.store.directory() + u"/blobs/"_q + pub.blobToken;
			if (!DesktopArchiveDirectory(root)) {
				return false;
			}
			const auto path = root + '/' + QString::number(pub.chunkIndex).rightJustified(20, '0') + u".chunk"_q;
			auto ack = ArchiveStagingPublicationAck(pub, false);
			if (!QFileInfo::exists(path) && !DesktopArchiveWrite(path, pub.frame)) {
				return false;
			}
			const auto actual = DesktopArchiveRead(path, kArchiveMediaChunkBytes + 4096);
			if (!actual) {
				return false;
			}
			ack.existingFrame = *actual;
			ack.frameHash = Hash(*actual);
			ack.durable = true;
			input = std::move(ack);
		} else if (step.request.kind == ArchiveStagingRequestKind::Complete) {
			input = ArchiveStagingFinish{step.request.id};
		} else if (step.request.kind == ArchiveStagingRequestKind::Finished) {
			return false;
		} else {
			return false;
		}
		step = AdvanceArchiveStagingFinalize(*start.session, input, s.header, s.store.security(),
			fresh->state, s.store.epoch());
		if (step.completion) {
			const auto &transaction = *step.completion;
			const auto done = std::find_if(transaction.next.jobs.begin(), transaction.next.jobs.end(),
				[&](const auto &value) { return value.id == s.binding.jobId; });
			if (done == transaction.next.jobs.end()) {
				return false;
			}
			const auto selected = FindArchiveVersion(transaction.next, s.binding.source.key, s.binding.source.version);
			const auto ref = selected ? std::find_if(selected->media.begin(), selected->media.end(),
				[&](const auto &v) { return v.identity == s.binding.source.mediaIdentity; }) : std::vector<ArchiveMediaReference>::const_iterator();
			if (!selected || ref == selected->media.end()) {
				return false;
			}
			const auto owned = std::find_if(transaction.next.blobs.begin(), transaction.next.blobs.end(),
				[&](const auto &v) { return v.id == ref->blobId; });
			const auto token = owned != transaction.next.blobs.end() ? ArchiveSqlToken(s.binding.source.key.account,
				ArchiveIndexKind::Blob, owned->id.toUtf8(), s.store.security()) : QString();
			const auto manifest = owned != transaction.next.blobs.end() ? SealArchiveSqlRow(s.binding.source.key.account,
				token, *owned, s.store.security()) : std::nullopt;
			if (!manifest || !DesktopArchiveWrite(s.store.directory() + u"/blobs/"_q + token + u"/manifest"_q, *manifest)
				|| !s.store.Execute(TranslateArchiveSql(transaction, *fresh, s.store.security()))) {
				return false;
			}
			return true;
		}
		if (step.status != ArchiveStagingStatus::Ready && step.status != ArchiveStagingStatus::NoChange) {
			return false;
		}
	}
	return false;
}

const ArchiveStagingHeader &DesktopArchiveStream::header() const {
	return _impl->header;
}

}
