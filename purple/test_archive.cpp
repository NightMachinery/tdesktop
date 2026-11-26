#include "purple/purple_archive_files.h"
#include "purple/purple_archive_previous.h"
#include "purple/purple_archive_v2_acquisition.h"
#include "purple/purple_archive_v2_schema.h"
#include "purple/purple_archive_v2_writer.h"
#include "tests/archive_test_support.h"
#include <QtCore/QBuffer>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

using namespace Purple;
using namespace ArchiveTest;

namespace {

void ArchiveV2Schema() {
	Section = "v2 SQLite format, version order, reference identity and indexes";
	const auto temp = QTemporaryDir();
	auto db = DesktopArchiveSqlite();
	CHECK(db.Open(temp.filePath(u"v2.sqlite"_q), true).ok);
	CHECK(db.ConfigureV2().ok);
	CHECK(db.Execute(ArchiveV2::Schema()).committed);
	const auto one = ArchiveSqlExpectation::OneRow;
	const auto scalar = [&](QString sql) -> ArchiveSqlValue {
		const auto result = db.Read({ sql, {}, one });
		return result.ok && result.rows.size() == 1 && result.rows.front().size() == 1
			? result.rows.front().front() : ArchiveSqlValue(nullptr);
	};
	const auto apply = [&](std::initializer_list<ArchiveSqlStatement> ops) {
		auto program = ArchiveSqlProgram{ ArchiveSqlStatus::Ready, {{ u"BEGIN IMMEDIATE"_q }} };
		for (const auto &op : ops) {
			program.statements.emplace_back(op.sql, op.binds, ArchiveSqlExpectation::OneChange);
		}
		program.statements.push_back({ u"COMMIT"_q });
		return db.Execute(program).committed;
	};
	CHECK(scalar(u"PRAGMA auto_vacuum"_q) == ArchiveSqlValue(int64(2)));
	CHECK(apply({
		{u"INSERT INTO meta VALUES(2,?,?,0,0,0,0,0,0)"_q, {QByteArray("store"), QByteArray("header")}},
		{u"INSERT INTO messages VALUES(x'01',x'02',x'03',0,0,25,3,x'FF',4,0)"_q},
		{u"INSERT INTO versions VALUES(x'01',1,x'A1',25,x'A0',x'AA',1)"_q},
		{u"INSERT INTO versions VALUES(x'01',2,x'A2',25,x'B0',x'BB',2)"_q},
		{u"INSERT INTO versions VALUES(x'01',3,x'A3',25,x'A0',x'AA',1)"_q},
		{u"INSERT INTO blobs VALUES(x'04',1000,x'FE',1,3456)"_q},
		{u"INSERT INTO blob_parts VALUES(x'04',0,x'FD')"_q},
		{u"INSERT INTO media VALUES(x'01',1,0,x'50',1,1,1,0,x'FC',x'04',x'FA')"_q},
		{u"INSERT INTO media VALUES(x'01',3,0,x'50',1,0,1,0,x'FC',x'04',x'FA')"_q},
	}));
	CHECK(!apply({ {u"INSERT INTO versions VALUES(x'01',4,x'A3',25,x'A0',x'AA',1)"_q} }));
	CHECK(!apply({ {u"INSERT INTO media VALUES(x'01',4,0,x'50',1,0,1,0,x'FC',x'04',x'FA')"_q} }));
	CHECK(!apply({ {u"INSERT INTO meta VALUES(1,x'01',x'02',0,0,0,0,0,0)"_q} }));
	CHECK(scalar(u"SELECT group_concat(seq) FROM (SELECT seq FROM versions ORDER BY seq)"_q)
		== ArchiveSqlValue(u"1,2,3"_q));
	CHECK(scalar(u"SELECT count(*) FROM sqlite_schema WHERE type='index' AND name IN "
		"('messages_chat','messages_status','messages_lookup','media_blob')"_q) == ArchiveSqlValue(int64(4)));
	CHECK(apply({ {u"DELETE FROM versions WHERE seq=1"_q} }));
	CHECK(scalar(u"SELECT count(*) FROM blobs"_q) == ArchiveSqlValue(int64(1)));
	CHECK(scalar(u"SELECT timed FROM meta"_q) == ArchiveSqlValue(int64(0)));
	CHECK(apply({ {u"UPDATE media SET blob_key=NULL,sealed_blob_ref=NULL WHERE seq=3"_q} }));
	CHECK(scalar(u"SELECT count(*) FROM blobs"_q) == ArchiveSqlValue(int64(0)));
	CHECK(scalar(u"SELECT count(*) FROM blob_parts"_q) == ArchiveSqlValue(int64(0)));
	CHECK(apply({ {u"DELETE FROM messages WHERE message_key=x'01'"_q} }));
	CHECK(scalar(u"SELECT count(*) FROM versions"_q) == ArchiveSqlValue(int64(0)));
	CHECK(scalar(u"SELECT count(*) FROM media"_q) == ArchiveSqlValue(int64(0)));
	CHECK(db.Close());
}

#include "test_archive_v2_writer.inc"
#include "test_archive_v2_quota.inc"
#include "test_archive_v2_views.inc"

}

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	const auto application = QCoreApplication(argc, argv);
	if (argc == 5 && QByteArray(argv[1]) == "archive-v2-crash-fixture") {
		const auto phase = QByteArray(argv[4]).toInt();
		if (phase < 0 || phase > int(Purple::ArchiveV2::Phase::AfterUnlink)) return 2;
		return V2Fixtures::CrashChild(QString::fromUtf8(argv[2]), QString::fromUtf8(argv[3]),
			Purple::ArchiveV2::Phase(phase));
	}
	if (argc == 4 && QByteArray(argv[1]) == "archive-v2-legacy-crash-fixture") {
		return V2Fixtures::LegacyCrashChild(QString::fromUtf8(argv[2]), QByteArray(argv[3]) == "after");
	}
	if (argc == 2 && QByteArray(argv[1]) == "archive-v2-default-allowances") {
		V2Fixtures::BothAllowances(512, 256);
		std::printf("Archive v2 default allowances: %d checks, %d failures\n", Checks, Failures);
		return Failures ? 1 : 0;
	}
	if (argc == 3 && QByteArray(argv[1]) == "archive-v2-charge-calibration") {
		return V2Fixtures::MeasureChurn(QString::fromUtf8(argv[2]));
	}
	if (argc == 2 && QByteArray(argv[1]) == "archive-v2-review-benchmark") {
		return V2Fixtures::ReviewBenchmark();
	}
	ArchiveV2Schema();
	V2Fixtures::CrashCuts();
	V2Fixtures::KeysAndIsolation();
	V2Fixtures::LogoutAndOwnership();
	V2Fixtures::CorruptStoreRefuses();
	V2Fixtures::UnreadableRetirement();
	V2Fixtures::UnreadableWriterControls();
	V2Fixtures::LogoutAtResetDelivery();
	V2Fixtures::ActiveDirectorySubstitution();
	V2Fixtures::QueueCancellation();
	V2Fixtures::CutoverCredentialsAndQuit();
	V2Fixtures::CutoverDeleteResolution();
	V2Fixtures::MediaGate();
	V2Fixtures::SeparatePressure();
	V2Fixtures::BothAllowances();
	V2Fixtures::FragmentationEvictsOnlyTheGrowingBucket();
	V2Fixtures::OverlappingAdmissions();
	V2Fixtures::ReadyMediaPriority();
	V2Fixtures::IncomingDuringBlob();
	V2Fixtures::LiveTrim();
	V2Fixtures::TotalsMatchAfterMixedWrites();
	V2Fixtures::QuotaDiskAndAge();
	V2Fixtures::VerifiedCompleteFile();
	V2Fixtures::HistoryAndCleanup();
	V2Fixtures::ConfirmedCleanupScrubsFiles();
	V2Fixtures::ConfirmedLargeScrubResumes();
	V2Fixtures::PendingBytesAndMediaOutcome();
	V2Fixtures::ConfirmedPagesAndNoticeDeduplication();
	V2Fixtures::PreviousArchiveRemoval();
	std::printf("Desktop archive: %d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
