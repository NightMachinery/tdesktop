namespace V2Fixtures {

void MediaGate() {
	Section = "v2 whole-file trigger, generation, cancellation and playback ordering";
	const auto lease = std::make_shared<Lease>();
	const auto id = QByteArray(16, 's');
	auto target = MediaTarget{ id, Sample(id).identity, 1, 0, "source", MediaKind::Photo, true };
	auto policy = Policy();
	auto request = [&] { return MediaRequest{ target, ChatClass::Bot, u"user:19"_q, 1000 }; };
	auto admits = MediaAdmission(lease, id);
	for (const auto kind : { MediaKind::Photo, MediaKind::Video, MediaKind::Round, MediaKind::Voice }) {
		target.kind = kind;
		CHECK(admits.begin(policy, request(), MediaTrigger::Arrival, true, true).action == MediaAction::Describe);
		CHECK(admits.begin(policy, request(), MediaTrigger::WholeFileAvailable, true, true).action == MediaAction::Describe);
		auto history = std::vector<int>();
		const auto open = [&] {
			if (kind == MediaKind::Photo || kind == MediaKind::Video) {
				history.push_back(1);
				history.push_back(2);
			} else {
				history.push_back(2);
				history.push_back(1);
			}
		};
		policy.timed = TimedDownload::Off;
		CHECK(admits.opened(policy, request(), true, true, open).action == MediaAction::Describe);
		const auto stock = std::exchange(history, {});
		policy.timed = TimedDownload::OnOpen;
		CHECK(admits.opened(policy, request(), true, false, open).action == MediaAction::Request);
		CHECK(history == stock);
		CHECK(admits.begin(policy, request(), MediaTrigger::ExplicitOpen, true, false).action == MediaAction::Describe);
		auto wrong = target;
		++wrong.sequence;
		CHECK(!admits.finish(policy, wrong));
		CHECK(admits.finish(policy, target));
		CHECK(!admits.finish(policy, target));
		CHECK(admits.begin(policy, request(), MediaTrigger::ExplicitOpen, true, true).action == MediaAction::Adopt);
		admits.cancel(target);
		CHECK(!admits.finish(policy, target));
	}
	target.kind = MediaKind::Voice;
	policy.classTimed[int(ChatClass::Bot)] = TimedDownload::Off;
	CHECK(admits.begin(policy, request(), MediaTrigger::ExplicitOpen, true, true).action == MediaAction::Describe);
	policy.chats[u"user:19"_q].timed = TimedDownload::OnOpen;
	CHECK(admits.begin(policy, request(), MediaTrigger::ExplicitOpen, true, false).action == MediaAction::Request);
	policy.chats[u"user:19"_q].timed = TimedDownload::Off;
	CHECK(!admits.finish(policy, target));

	target.timed = false;
	CHECK(admits.begin(policy, request(), MediaTrigger::Arrival, true, true).action == MediaAction::Describe);
	policy.chats[u"user:19"_q].ordinaryOnArrival = true;
	CHECK(admits.begin(policy, request(), MediaTrigger::ExplicitOpen, true, true).action == MediaAction::Describe);
	CHECK(admits.begin(policy, request(), MediaTrigger::WholeFileAvailable, true, true).action == MediaAction::Describe);
	CHECK(admits.begin(policy, request(), MediaTrigger::Arrival, true, true).action == MediaAction::Adopt);
	CHECK(admits.finish(policy, target));
	auto oversized = request();
	oversized.size = int64(policy.ordinaryFileMiB) * kMiB + 1;
	CHECK(admits.begin(policy, oversized, MediaTrigger::Arrival, true, true).reason == MediaReason::FileLimit);
	auto old = request();
	old.target.store = QByteArray(16, 't');
	CHECK(admits.begin(policy, old, MediaTrigger::Arrival, true, true).reason == MediaReason::ScopeRetired);
	CHECK(admits.begin(policy, request(), MediaTrigger::Arrival, true, false).action == MediaAction::Request);
	lease->revoke();
	CHECK(!admits.finish(policy, target));
}

CapturedEvent MediaRow(const QByteArray &id, int number, bool timed, int64 observedAt) {
	auto event = Sample(id, QByteArray::number(number));
	event.identity.message += number;
	event.attachments.front().timed = timed;
	event.observedAt = observedAt;
	return event;
}

BlobInput MediaFile(const CapturedEvent &event, const QByteArray &data) {
	auto result = SampleBlob(event.storeId, data);
	result.identity = event.identity;
	return result;
}

void SeparatePressure() {
	Section = "v2 timed allowance evicts its oldest copy and ordinary pressure preserves it";
	const auto temp = QTemporaryDir();
	auto options = Options(temp.path());
	options.policy.totalMiB = 16;
	options.policy.ordinaryFileMiB = 4;
	options.policy.timedMiB = 2;
	auto path = QString();
	auto peak = int64(0);
	options.checkpoint = [&](Phase phase) {
		if (phase == Phase::AfterSqlCommit && !path.isEmpty()) {
			peak = std::max(peak, QFileInfo(path).size() + QFileInfo(path + u"-wal"_q).size()
				+ QFileInfo(path + u"-shm"_q).size());
		}
		return true;
	};
	auto store = V2Store(options);
	CHECK(store.open());
	const auto id = store.id();
	path = ActivePath(options, id) + u"/archive.sqlite"_q;
	const auto at = int64(1700000000);
	for (auto i = 1; i <= 3; ++i) {
		const auto event = MediaRow(id, i, true, at - (4 - i) * 86400);
		CHECK(store.capture(event).committed);
		CHECK(store.writeBlob(MediaFile(event, QByteArray(kMiB, char('A' + i)))).committed);
	}
	CHECK(Count(path, u"SELECT count(*) FROM media WHERE timed=1 AND blob_key IS NOT NULL"_q) == 2);
	CHECK(Count(path, u"SELECT min(last_activity) FROM media JOIN messages USING(message_key) "
		"WHERE timed=1 AND blob_key IS NOT NULL"_q) == (at - 2 * 86400) / 86400);
	CHECK(store.usage() && store.usage()->timed == 2 * kMiB);
	for (auto i = 4; i <= 7; ++i) {
		const auto event = MediaRow(id, i, false, at);
		CHECK(store.capture(event).committed);
		CHECK(store.writeBlob(MediaFile(event, QByteArray(4 * kMiB, char('A' + i)))).committed);
		CHECK(Count(path, u"SELECT count(*) FROM media WHERE timed=1 AND blob_key IS NOT NULL"_q) == 2);
		CHECK(store.usage() && store.usage()->physical() <= 18 * kMiB);
	}
	CHECK(peak > 0 && peak <= 18 * kMiB);
	std::printf("V2 quota physical peak at commit: %lld bytes, cap %lld bytes\n",
		static_cast<long long>(peak), static_cast<long long>(18 * kMiB));
	const auto before = store.usage();
	CHECK(store.maintain());
	const auto after = store.usage();
	CHECK(before && after && after->database <= before->database && after->timed == before->timed);

	const auto event = MediaRow(id, 8, false, at);
	CHECK(store.capture(event).committed);
	CHECK(store.writeBlob(MediaFile(event, QByteArray(4 * kMiB + 1, 'Q'))).reason == u"over_file_limit"_q);
	auto input = MediaFile(event, "complete declaration");
	input.read = [](qsizetype) { return QByteArray("short"); };
	CHECK(store.writeBlob(input).reason == u"copy_incomplete"_q);
}

void QuotaDiskAndAge() {
	Section = "v2 shared blob accounting, readers, clock and disk failures";
	const auto temp = QTemporaryDir();
	auto options = Options(temp.path());
	const auto at = int64(1700000000);
	auto now = at;
	auto space = int64(64 * kMiB);
	auto failAfter = false;
	options.now = [&] { return now; };
	options.availableBytes = [&] { return space; };
	options.policy.totalMiB = 16;
	options.policy.timedMiB = 2;
	options.checkpoint = [&](Phase phase) {
		if (phase == Phase::AfterSqlCommit && failAfter) space = 0;
		return true;
	};
	auto store = V2Store(options);
	CHECK(store.open());
	const auto id = store.id();
	const auto path = ActivePath(options, id) + u"/archive.sqlite"_q;
	const auto source = QByteArray(kMiB, 'S');
	const auto ordinary = MediaRow(id, 1, false, at);
	const auto timed = MediaRow(id, 2, true, at);
	CHECK(store.capture(ordinary).committed);
	CHECK(store.writeBlob(MediaFile(ordinary, source)).committed);
	CHECK(store.usage() && store.usage()->ordinary > kMiB && store.usage()->timed == 0);
	CHECK(store.capture(timed).committed);
	CHECK(store.writeBlob(MediaFile(timed, source)).committed);
	const auto shared = store.usage();
	CHECK(shared && shared->ordinary < kMiB && shared->timed == kMiB);
	CHECK(Count(path, u"SELECT count(*) FROM blobs"_q) == 1);
	CHECK(store.maintain());
	auto reader = DesktopArchiveSqlite();
	CHECK(reader.Open(path, false, true).ok && reader.BeginRead());
	CHECK(reader.Read({ u"SELECT count(*) FROM messages"_q, {}, ArchiveSqlExpectation::OneRow }).ok);
	const auto whileRead = MediaRow(id, 3, false, at);
	CHECK(store.capture(whileRead).committed);
	const auto blocked = MediaRow(id, 4, false, at);
	CHECK(store.capture(blocked).reason == u"store_busy"_q);
	CHECK(reader.EndRead() && reader.Close());
	CHECK(store.capture(blocked).committed);

	const auto low = MediaRow(id, 5, false, at);
	space = 0;
	CHECK(store.capture(low).reason == u"disk_full"_q);
	CHECK(!store.body(low.identity, 1));
	space = 64 * kMiB;
	failAfter = true;
	CHECK(store.capture(low).committed);
	CHECK(space == 0);
	CHECK(store.body(low.identity, 1));
	CHECK(store.capture(MediaRow(id, 6, false, at)).reason == u"disk_full"_q);
	failAfter = false;
	space = 64 * kMiB;
	now -= 10 * 86400;
	CHECK(store.maintain() && store.body(low.identity, 1));
	now = at + 2 * 86400;
	auto shorter = options.policy;
	shorter.classDays[int(ChatClass::Direct)] = 1;
	CHECK(store.updatePolicy(shorter));
	const auto trimmed = store.usage();
	CHECK(!store.body(low.identity, 1) && trimmed && trimmed->timed == 0);
	const auto reusable = Count(path, u"PRAGMA freelist_count"_q);
	const auto pageSize = Count(path, u"PRAGMA page_size"_q);
	CHECK(reusable > 64 && pageSize > 0);
	CHECK(trimmed && trimmed->database == QFileInfo(path).size() && trimmed->database >= reusable * pageSize);
}

void VerifiedCompleteFile() {
	Section = "v2 complete owned file is matched at adoption and read";
	const auto temp = QTemporaryDir();
	const auto file = temp.filePath(u"completed.fixture"_q);
	const auto bytes = QByteArray(kMiB, 'x') + "full";
	CHECK(Put(file, bytes));
	const auto id = QByteArray(16, 'v');
	auto input = SampleBlob(id, bytes);
	input.read = {};
	input.digest.clear();
	auto opened = CompleteFile(input, file);
	CHECK(opened && opened->digest == QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
	CHECK(opened && opened->read(32) == bytes.first(32));
	input.size += 1;
	CHECK(!CompleteFile(input, file));
}

}
