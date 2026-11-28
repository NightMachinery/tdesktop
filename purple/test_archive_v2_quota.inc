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
	result.timed = event.attachments.front().timed;
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
	CHECK(store.usage() && store.usage()->timed == 2 * (kMiB + 3456));
	for (auto i = 4; i <= 7; ++i) {
		const auto event = MediaRow(id, i, false, at);
		CHECK(store.capture(event).committed);
		CHECK(store.writeBlob(MediaFile(event, QByteArray(4 * kMiB, char('A' + i)))).committed);
		CHECK(Count(path, u"SELECT count(*) FROM media WHERE timed=1 AND blob_key IS NOT NULL"_q) == 2);
		CHECK(store.usage() && store.usage()->physical() <= 18 * kMiB + kMetadataHeadroom + kWalHeadroom);
	}
	CHECK(peak > 0 && peak <= 18 * kMiB + kMetadataHeadroom + kWalHeadroom);
	std::printf("V2 quota physical peak at commit: %lld bytes, cap %lld bytes\n",
		static_cast<long long>(peak), static_cast<long long>(18 * kMiB + kMetadataHeadroom + kWalHeadroom));
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
	CHECK(shared && shared->ordinary < kMiB && shared->timed == kMiB + 3456);
	CHECK(Count(path, u"SELECT count(*) FROM blobs"_q) == 1);
	CHECK(store.maintain());
	auto reader = DesktopArchiveSqlite();
	CHECK(reader.Open(path, false, true).ok && reader.BeginRead());
	CHECK(reader.Read({ u"SELECT count(*) FROM messages"_q, {}, ArchiveSqlExpectation::OneRow }).ok);
	const auto whileRead = MediaRow(id, 3, false, at);
	CHECK(store.capture(whileRead).committed);
	auto blocked = MediaRow(id, 4, false, at);
	auto seenBusy = false;
	for (auto n = 0; n != 32; ++n) {
		blocked.identity.message += 1;
		blocked.body = QByteArray(kMiB, 'W');
		const auto result = store.capture(blocked);
		if (result.reason == u"store_busy"_q) { seenBusy = true; break; }
		CHECK(result.committed);
	}
	CHECK(seenBusy && !store.body(blocked.identity, 1));
	CHECK(reader.EndRead() && reader.Close());
	CHECK(store.capture(blocked).committed);

	// Keep the rewind probe newer than the pressure candidates. Their same-day
	// order depends on the store's random HMAC key.
	const auto low = MediaRow(id, 90, false, at + 86400);
	space = 0;
	CHECK(store.capture(low).reason == u"disk_full"_q);
	CHECK(!store.body(low.identity, 1));
	space = 64 * kMiB;
	failAfter = true;
	CHECK(store.capture(low).committed);
	CHECK(space == 0);
	CHECK(store.body(low.identity, 1));
	CHECK(store.capture(MediaRow(id, 91, false, at)).reason == u"disk_full"_q);
	failAfter = false;
	space = 64 * kMiB;
	now -= 10 * 86400;
	const auto rewound = store.maintain();
	if (!rewound) std::cerr << "Clock rewind maintenance: " << rewound.reason.toStdString() << '\n';
	CHECK(rewound);
	CHECK(store.body(low.identity, 1));
	now = at + 2 * 86400;
	auto shorter = options.policy;
	shorter.classDays[int(ChatClass::Direct)] = 1;
	CHECK(store.updatePolicy(shorter));
	const auto trimmed = store.usage();
	CHECK(!store.body(low.identity, 1) && trimmed && trimmed->timed == 0);
	const auto reusable = Count(path, u"PRAGMA freelist_count"_q);
	const auto pageSize = Count(path, u"PRAGMA page_size"_q);
	CHECK(reusable > 64 && pageSize > 0);
	CHECK(trimmed && trimmed->database == QFileInfo(path).size()
		&& trimmed->database + trimmed->wal >= reusable * pageSize);
}

void ConfiguredRetention() {
	Section = "v2 TOML retention prunes at the class boundary and uses global days for other chats";
	const auto temp = QTemporaryDir();
	auto options = Options(temp.path());
	const auto day = int64(86400);
	auto now = 20000 * day;
	options.now = [&] { return now; };
	auto store = V2Store(options);
	CHECK(store.open());
	const auto make = [&](int number, ChatClass kind, int daysAgo) {
		auto event = MediaRow(store.id(), number, false, now - daysAgo * day);
		event.kind = kind;
		CHECK(store.capture(event).committed);
		return event.identity;
	};
	const auto oldDirect = make(1, ChatClass::Direct, 7);
	const auto recentDirect = make(2, ChatClass::Direct, 6);
	const auto oldBot = make(3, ChatClass::Bot, 2);
	const auto recentBot = make(4, ChatClass::Bot, 1);
	const auto parsed = ParseSettings(
		u"[archive]\ndays = 7\n[archive.days_by_class]\nbot = 2\n"_q, u"retention.toml"_q);
	CHECK(parsed.ok() && parsed.settings.archive.valid);
	CHECK(store.estimateExpired(parsed.settings.archive) == 2);
	CHECK(store.updatePolicy(parsed.settings.archive));
	CHECK(!store.body(oldDirect, 1) && !store.body(oldBot, 1));
	CHECK(store.body(recentDirect, 1) && store.body(recentBot, 1));
	now += day;
	CHECK(store.maintenancePending() && store.maintain());
	CHECK(!store.body(recentDirect, 1) && !store.body(recentBot, 1));
}

void BothAllowances(int ordinaryMiB = 16, int timedMiB = 8) {
	Section = "v2 both independent buckets fit at their limits in either order";
	for (const auto timedFirst : { false, true }) {
		const auto temp = QTemporaryDir();
		auto options = Options(temp.path());
		options.policy.totalMiB = options.policy.ordinaryFileMiB = ordinaryMiB;
		options.policy.timedMiB = timedMiB;
		const auto ordinaryCap = int64(ordinaryMiB) * kMiB;
		const auto timedCap = int64(timedMiB) * kMiB;
		auto path = QString();
		auto peak = int64(0);
		auto wal = int64(0);
		options.checkpoint = [&](Phase phase) {
			if (!path.isEmpty() && (phase == Phase::BlobPart || phase == Phase::AfterSqlCommit)) {
				const auto bytes = QFileInfo(path + u"-wal"_q).size();
				wal = std::max(wal, bytes);
				peak = std::max(peak, QFileInfo(path).size() + bytes + QFileInfo(path + u"-shm"_q).size());
			}
			return true;
		};
		auto store = V2Store(options);
		CHECK(store.open());
		const auto id = store.id();
		path = ActivePath(options, id) + u"/archive.sqlite"_q;
		const auto timed = MediaRow(id, 1, true, 1700000000);
		const auto ordinary = MediaRow(id, 2, false, 1700000000);
		CHECK(store.capture(timed).committed && store.capture(ordinary).committed);
		const auto initial = store.usage();
		if (!initial) continue;
		const auto save = [&](const CapturedEvent &event, int64 size, char fill) {
			auto input = MediaFile(event, {});
			input.size = size;
			auto hash = QCryptographicHash(QCryptographicHash::Sha256);
			const auto part = QByteArray(kChunk, fill);
			for (auto left = size; left > 0; left -= kChunk) hash.addData(QByteArrayView(part).first(std::min<int64>(left, kChunk)));
			input.digest = hash.result();
			input.read = [size, fill, sent = int64(0)](qsizetype maximum) mutable {
				const auto count = std::min<int64>(maximum, size - sent);
				sent += count;
				return QByteArray(count, fill);
			};
			CHECK(store.writeBlob(input).committed);
		};
		if (timedFirst) save(timed, timedCap, 'T');
		save(ordinary, ordinaryCap - initial->ordinary, 'O');
		if (!timedFirst) save(timed, timedCap, 'T');
		const auto final = store.usage();
		const auto budget = store.storageSummary();
		CHECK(budget && budget->active.ordinary == ordinaryCap && budget->active.timed == timedCap);
		CHECK(final && final->ordinary > ordinaryCap && final->timed > timedCap && budget && !budget->overLimit);
		CHECK(wal > 0 && wal <= kWalHeadroom);
		CHECK(peak > ordinaryCap + timedCap && peak <= ordinaryCap + timedCap + kMetadataHeadroom + kWalHeadroom);
		std::printf("V2 %d + %d MiB (%s first): peak=%lld WAL=%lld bytes\n",
			ordinaryMiB, timedMiB, timedFirst ? "timed" : "ordinary", static_cast<long long>(peak), static_cast<long long>(wal));
	}
}

void FragmentationEvictsOnlyTheGrowingBucket() {
	Section = "v2 physical pressure reclaims pages exclusively from the growing bucket";
	for (const auto timedGrowth : { false, true }) {
		const auto temp = QTemporaryDir();
		auto options = Options(temp.path());
		options.policy.totalMiB = 16;
		options.policy.timedMiB = 8;
		auto id = QByteArray();
		{
			auto store = V2Store(options);
			CHECK(store.open());
			id = store.id();
			for (auto i = 1; i <= 3; ++i) {
				const auto event = MediaRow(id, i, i == 3 ? timedGrowth : i == 2, 1700000000);
				CHECK(store.capture(event).committed);
				if (i != 3) CHECK(store.writeBlob(MediaFile(event, QByteArray(2 * kMiB, char('A' + i)))).committed);
			}
		}
		const auto dbPath = ActivePath(options, id) + u"/archive.sqlite"_q;
		{
			// Occupied, uncharged pages stand in for unusable space inside fragmented B-trees.
			auto probe = DesktopArchiveSqlite();
			CHECK(probe.Open(dbPath, false).ok);
			CHECK(probe.Read({ u"CREATE TABLE page_pressure (payload BLOB NOT NULL)"_q }).ok);
			const auto fill = 71 * kMiB - QFileInfo(dbPath).size() - 512 * 1024;
			CHECK(probe.Read({ u"INSERT INTO page_pressure VALUES(zeroblob(?))"_q,
				{ int64(fill) }, ArchiveSqlExpectation::OneChange }).ok);
		}
		CHECK(QFileInfo(dbPath).size() > 70 * kMiB && QFileInfo(dbPath).size() <= 71 * kMiB);
		auto store = V2Store(Options(temp.path()));
		auto lower = options.policy;
		CHECK(store.open() && store.updatePolicy(lower));
		auto stale = MediaRow(QByteArray(16, 'q'), 4, false, 1700000000);
		stale.body = QByteArray(kMiB, 'X');
		CHECK(store.capture(stale).reason == u"scope_retired"_q);
		const auto original = store.media(MediaRow(id, 1, false, 0).identity, 1);
		CHECK(original && original->front().savedBytes == 2 * kMiB);
		const auto target = MediaRow(id, 3, timedGrowth, 1700000000);
		CHECK(store.writeBlob(MediaFile(target, QByteArray(kMiB, 'N'))).committed);
		const auto other = store.media(MediaRow(id, timedGrowth ? 1 : 2, !timedGrowth, 0).identity, 1);
		const auto evicted = store.media(MediaRow(id, timedGrowth ? 2 : 1, timedGrowth, 0).identity, 1);
		CHECK(other && other->front().savedBytes == 2 * kMiB);
		CHECK(evicted && evicted->front().savedBytes == 0);
		CHECK(store.usage() && store.usage()->physical() <= 24 * kMiB + kMetadataHeadroom + kWalHeadroom);
	}
}

void LiveTrim() {
	Section = "v2 capture during a quota reduction and clean idle maintenance";
	const auto temp = QTemporaryDir();
	auto options = Options(temp.path());
	options.policy.totalMiB = 48;
	options.policy.timedMiB = 0;
	auto capacityChecks = 0;
	options.availableBytes = [&] { ++capacityChecks; return 256 * kMiB; };
	auto store = V2Store(options);
	CHECK(store.open());
	const auto id = store.id();
	for (auto n = 1; n <= 240; ++n) {
		auto event = MediaRow(id, n, false, 1700000000);
		event.body = QByteArray(120 * 1024, 'A');
		CHECK(store.capture(event).committed);
	}
	auto smaller = options.policy;
	smaller.totalMiB = 16;
	CHECK(store.updatePolicy(smaller));
	CHECK(store.usage() && store.usage()->ordinary > 16 * kMiB);
	for (auto n = 241; n <= 244; ++n) {
		const auto incoming = MediaRow(id, n, false, 1700000000);
		CHECK(store.capture(incoming).committed);
		CHECK(store.body(incoming.identity, 1));
	}
	for (auto n = 0; n != 512 && store.maintenancePending(); ++n) CHECK(store.maintain());
	CHECK(store.usage() && store.usage()->ordinary <= 16 * kMiB);
	CHECK(!store.maintenancePending());
	const auto checked = capacityChecks;
	CHECK(store.maintain());
	CHECK(capacityChecks == checked);
}

void OverlappingAdmissions() {
	Section = "v2 multiple pending media requests and explicit queue saturation";
	const auto id = QByteArray(16, 'q');
	const auto lease = std::make_shared<Lease>();
	auto admits = MediaAdmission(lease, id);
	const auto request = [&](int i) {
		return MediaRequest{ MediaTarget{ id, Sample(id).identity, 1, i, "source",
			MediaKind::Photo, true }, ChatClass::Direct, u"user:19"_q, 100 };
	};
	for (auto i = 0; i != 32; ++i) {
		CHECK(admits.begin(Policy(), request(i), MediaTrigger::ExplicitOpen, true, false).action == MediaAction::Request);
	}
	const auto overflow = admits.begin(Policy(), request(32), MediaTrigger::ExplicitOpen, true, false);
	CHECK(overflow.action == MediaAction::Describe && overflow.reason == MediaReason::QueueFull);
	CHECK(admits.begin(Policy(), request(0), MediaTrigger::ExplicitOpen, true, false).action == MediaAction::Describe);
	for (auto i = 0; i != 32; ++i) CHECK(admits.finish(Policy(), request(i).target));
}

void ReadyMediaPriority() {
	Section = "v2 in-flight media leaves room for timed FIFO, ordinary FIFO and target deduplication";
	const auto temp = std::make_shared<QTemporaryDir>();
	struct Queue {
		std::promise<QByteArray> opened;
		std::promise<void> entered;
		std::promise<void> release;
		std::promise<std::vector<int>> finished;
		std::vector<int> reads;
	};
	const auto state = std::make_shared<Queue>();
	auto ready = state->opened.get_future();
	auto entered = state->entered.get_future();
	const auto release = state->release.get_future().share();
	auto finished = state->finished.get_future();
	auto worker = Writer(Options(temp->path()), [state, temp, first = true](StoreEffect result, QByteArray id) mutable {
		if (std::exchange(first, false)) state->opened.set_value(result ? id : QByteArray());
		if (state->reads.size() == 5 && result.committed) state->finished.set_value(std::exchange(state->reads, {}));
	});
	CHECK(ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	if (ready.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
	const auto id = ready.get();
	const auto staged = std::make_shared<std::promise<void>>();
	auto rowsReady = staged->get_future();
	const auto event = [id](int n) { return MediaRow(id, n, n == 3 || n == 4, 1700000000); };
	for (auto n = 1; n <= 5; ++n) CHECK(worker.post(event(n)));
	CHECK(worker.query(id, [staged](V2Store &) { staged->set_value(); }));
	CHECK(rowsReady.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	if (rowsReady.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
	const auto file = [&](int n) {
		auto input = MediaFile(event(n), QByteArray(16, char('A' + n)));
		input.read = [state, release, n, read = std::move(input.read)](qsizetype size) {
			if (size > 1) {
				state->reads.push_back(n);
				if (n == 1) { state->entered.set_value(); release.wait(); }
			}
			return read(size);
		};
		return input;
	};
	CHECK(worker.postBlob(file(1)));
	CHECK(entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	for (const auto n : { 1, 2, 3, 4, 2, 5 }) CHECK(worker.postBlob(file(n)));
	state->release.set_value();
	CHECK(finished.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
	if (finished.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		CHECK((finished.get() == std::vector{ 1, 3, 4, 2, 5 }));
	}
}

void IncomingDuringBlob() {
	Section = "v2 text arriving at every blob part is saved in order and the file survives";
	const auto temp = std::make_shared<QTemporaryDir>();
	struct Run {
		std::promise<QByteArray> opened;
		std::promise<bool> saved;
		int reports = 0;
		int count = 0;
		bool accepted = true;
	};
	const auto state = std::make_shared<Run>();
	auto ready = state->opened.get_future();
	auto result = state->saved.get_future();
	const auto worker = std::make_shared<Writer>(Options(temp->path()), [state, temp, done = false](StoreEffect effect, QByteArray id) mutable {
		if (effect.stage == IncidentStage::Startup) state->opened.set_value(id);
		else if (!done && effect.stage == IncidentStage::WholeFile) {
			done = true;
			state->saved.set_value(bool(effect));
		}
	});
	CHECK(ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	if (ready.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
	const auto id = ready.get();
	const auto target = MediaRow(id, 1, true, 1700000000);
	CHECK(worker->post(target));
	auto input = MediaFile(target, QByteArray(40 * kMiB, 'V'));
	input.read = [state, id, weak = std::weak_ptr(worker), read = std::move(input.read)](qsizetype size) {
		if (size > 1) {
			const auto writer = weak.lock();
			state->accepted &= writer && bool(writer->post(MediaRow(id, ++state->count + 1, true, 1700000000)));
		}
		return read(size);
	};
	CHECK(worker->postBlob(std::move(input)));
	CHECK(result.wait_for(std::chrono::seconds(15)) == std::future_status::ready);
	if (result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) CHECK(result.get());
	const auto inspected = std::make_shared<std::promise<bool>>();
	auto finished = inspected->get_future();
	CHECK(worker->query(id, [=](V2Store &store) {
		auto good = state->accepted && state->count == 40;
		const auto files = store.media(target.identity, 1);
		good = good && files && files->front().savedBytes == 40 * kMiB;
		for (auto n = 2; n <= 41 && good; ++n) {
			const auto body = store.body(MediaRow(id, n, true, 0).identity, 1);
			good = body && body->serial == n;
		}
		inspected->set_value(good);
	}));
	CHECK(finished.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	if (finished.wait_for(std::chrono::seconds(0)) == std::future_status::ready) CHECK(finished.get());
}

void CheckTotals(V2Store &store, const QString &path) {
	const auto usage = store.usage();
	const auto ordinary = Count(path, u"SELECT coalesce((SELECT sum(length(sealed_identity)+768) FROM messages),0)+"
		"coalesce((SELECT sum(length(sealed_body)*5/4+384) FROM versions),0)+"
		"coalesce((SELECT sum(length(sealed_descriptor)+400) FROM media),0)+"
		"coalesce((SELECT count(*)*256 FROM purged_edits),0)+"
		"coalesce((SELECT sum(size+384+3072*((size-1)/1048576+1)) FROM blobs b WHERE NOT EXISTS("
		"SELECT 1 FROM media r WHERE r.blob_key=b.blob_key AND r.timed=1)),0)"_q);
	const auto timed = Count(path, u"SELECT coalesce(sum(size+384+3072*((size-1)/1048576+1)),0) FROM blobs b "
		"WHERE EXISTS(SELECT 1 FROM media r WHERE r.blob_key=b.blob_key AND r.timed=1)"_q);
	CHECK(usage && usage->ordinary == ordinary && usage->timed == timed);
}

void TotalsMatchAfterMixedWrites() {
	Section = "v2 committed byte totals equal a full sum after mixed mutations";
	const auto temp = QTemporaryDir();
	auto options = Options(temp.path());
	auto store = V2Store(Options(temp.path()));
	CHECK(store.open());
	const auto id = store.id();
	const auto path = ActivePath(options, id) + u"/archive.sqlite"_q;
	const auto one = MediaRow(id, 1, true, 1700000000);
	const auto two = MediaRow(id, 2, false, 1700000000);
	const auto bytes = QByteArray(kMiB, 'A');
	CHECK(store.capture(one).committed && store.capture(two).committed);
	CHECK(store.writeBlob(MediaFile(two, bytes)).committed);
	CheckTotals(store, path);
	CHECK(store.writeBlob(MediaFile(one, bytes)).committed);
	CheckTotals(store, path);
	auto edit = one;
	edit.event = Observation::Edit;
	edit.eventId = "updated";
	edit.body = "another body";
	CHECK(store.capture(edit).committed);
	auto cleaned = store.prepareCleanup(CleanupKind::EditHistory);
	CHECK(cleaned && store.cleanupPage(*cleaned).effect);
	CheckTotals(store, path);
	CHECK(store.usage() && store.usage()->timed == 0 && store.usage()->ordinary > kMiB);
}

int MeasureChurn(const QString &scratch) {
	Section = "v2 charge calibration after insertion, eviction and insertion";
	for (const auto mix : { 0, 1, 2 }) {
		auto temp = QTemporaryDir(scratch + u"/mix-%1-XXXXXX"_q.arg(mix));
		CHECK(temp.isValid());
		temp.setAutoRemove(false);
		auto options = Options(temp.path());
		options.policy.totalMiB = 96;
		options.policy.timedMiB = mix ? 32 : 0;
		auto path = QString();
		auto usage = std::optional<Usage>();
		{
			auto store = V2Store(options);
			CHECK(store.open());
			const auto id = store.id();
			path = ActivePath(options, id) + u"/archive.sqlite"_q;
			const auto text = [&](int start, int count) {
				for (auto n = start; n < start + count; n += 48) {
					auto batch = std::vector<CapturedEvent>();
					for (auto i = n; i < std::min(n + 48, start + count); ++i) {
						auto row = MediaRow(id, 1000 + i / 6, false, 1700000000);
						row.event = i % 6 ? Observation::Edit : Observation::Baseline;
						row.eventId = QByteArray::number(i);
						const auto lengths = std::array{ 48, 360, 950, 1200, 1900, 2600, 3920, 6000 };
						row.body = row.eventId + QByteArray(lengths[(i / 6 + i) % lengths.size()], 'X');
						row.attachments.clear();
						batch.push_back(std::move(row));
					}
					for (const auto &effect : store.captureBatch(batch)) CHECK(effect.committed);
					CHECK(store.maintain());
				}
			};
			const auto files = [&](int start, int count) {
				for (auto n = start; n < start + count; ++n) {
					const auto row = MediaRow(id, n + 1, n % 2, 1700000000);
					CHECK(store.capture(row).committed);
					const auto lengths = std::array{ 220, 2300, 28000, 120000, 620000, 1000000, 1250000, 2100000 };
					auto source = MediaFile(row, QByteArray(lengths[n % lengths.size()], char(1 + n)));
					source.revision = options.lease->mediaRevision.load();
					const auto outcome = store.writeBlob(source);
					if (!outcome) std::printf("Churn rejected copy: %s\n", qPrintable(outcome.reason));
					CHECK(outcome.committed);
					CHECK(store.maintain());
				}
			};
			if (mix != 1) text(0, mix == 0 ? 14400 : 7200);
			if (mix != 0) files(0, mix == 1 ? 96 : 48);
			auto lower = options.policy;
			lower.totalMiB = 16;
			lower.timedMiB = mix ? 8 : 0;
			CHECK(store.updatePolicy(lower));
			for (auto i = 0; i != 1024 && store.maintenancePending(); ++i) CHECK(store.maintain());
			CHECK(!store.maintenancePending());
			if (mix != 1) {
				const auto ticket = store.prepareCleanup(CleanupKind::EditHistory);
				CHECK(ticket && store.cleanupPage(*ticket).effect);
				text(14400, mix == 0 ? 7200 : 3600);
			}
			if (mix != 0) files(112, mix == 1 ? 48 : 32);
			for (auto i = 0; i != 1024 && store.maintenancePending(); ++i) CHECK(store.maintain());
			usage = store.usage();
		}
		const auto physical = QFileInfo(path).size();
		const auto page = Count(path, u"PRAGMA page_size"_q);
		const auto free = Count(path, u"PRAGMA freelist_count"_q) * page;
		const auto charged = usage ? usage->ordinary + usage->timed : 0;
		CHECK(usage && charged && physical > 0 && free >= 0);
		if (charged) {
			std::printf("Churn mix=%d db=%lld free=%lld charged=%lld ratio=%.4f active_ratio=%.4f %s\n", mix,
				(long long)physical, (long long)free, (long long)charged, double(physical) / charged,
				double(physical - free) / charged, qPrintable(path));
		}
	}
	std::printf("Archive v2 churn: %d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}

int ReviewBenchmark() {
	using Clock = std::chrono::steady_clock;
	const auto temp = std::make_shared<QTemporaryDir>();
	if (!temp->isValid()) return 2;
	const auto at = int64(1700000000);
	auto id = QByteArray();
	{
		auto store = V2Store(Options(temp->path()));
		if (!store.open()) return 2;
		id = store.id();
		for (auto offset = 0; offset < 200000; offset += 128) {
			auto batch = std::vector<CapturedEvent>();
			for (auto n = offset; n < std::min(200000, offset + 128); ++n) {
				auto event = MediaRow(id, n / 10 + 1, false, at);
				event.event = (n % 10) ? Observation::Edit : Observation::Baseline;
				event.eventId = QByteArray::number(n);
				event.body = event.eventId + QByteArray(950, 'B');
				event.attachments.clear();
				batch.push_back(std::move(event));
			}
			for (const auto &effect : store.captureBatch(batch)) if (!effect.committed) return 3;
		}
		const auto bytes = store.usage();
		if (!bytes || !store.maintain()) return 4;
		std::printf("V2 benchmark seeded: versions=200000 ordinary=%lld DB+WAL+SHM=%lld bytes\n",
			static_cast<long long>(bytes->ordinary), static_cast<long long>(bytes->physical()));
	}
	struct Metrics {
		std::mutex lock;
		std::condition_variable changed;
		int committed = 0;
		bool open = false;
		bool failed = false;
		bool retiring = false;
		bool retired = false;
	};
	const auto metrics = std::make_shared<Metrics>();
	auto worker = Writer(Options(temp->path()), [metrics, temp](StoreEffect effect, QByteArray) {
		const auto guard = std::lock_guard(metrics->lock);
		metrics->open = true;
		metrics->failed |= !effect;
		if (metrics->retiring) metrics->retired = effect.committed;
		else metrics->committed += effect.committed;
		metrics->changed.notify_one();
	});
	const auto wait = [&](const auto &test) {
		auto lock = std::unique_lock(metrics->lock);
		return metrics->changed.wait_for(lock, std::chrono::seconds(25), [&] { return metrics->failed || test(); })
			&& !metrics->failed;
	};
	const auto elapsed = [](auto start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
	if (!wait([&] { return metrics->open; })) return 5;
	auto latencies = std::vector<double>();
	for (auto n = 0; n < 100; ++n) {
		const auto start = Clock::now();
		if (!worker.post(MediaRow(id, 21000 + n, false, at))
			|| !wait([&] { return metrics->committed >= n + 1; })) return 6;
		latencies.push_back(elapsed(start));
	}
	std::sort(latencies.begin(), latencies.end());
	std::printf("V2 benchmark capture: p50=%.3f ms p95=%.3f ms p99=%.3f ms\n",
		latencies[49], latencies[94], latencies[98]);
	const auto burst = Clock::now();
	for (auto n = 0; n < 1000; ++n) {
		if (n && n % 128 == 0 && !wait([&] { return metrics->committed >= 100 + n; })) return 7;
		if (!worker.post(MediaRow(id, 22000 + n, false, at))) return 7;
	}
	if (!wait([&] { return metrics->committed == 1100; })) return 7;
	std::printf("V2 benchmark 1000-event burst: %.3f ms\n", elapsed(burst));
	const auto inspected = std::make_shared<std::promise<bool>>();
	auto done = inspected->get_future();
	if (!worker.query(id, [=](V2Store &store) {
		if (!store.maintain() || store.maintenancePending()) { inspected->set_value(false); return; }
		auto start = Clock::now();
		for (auto i = 0; i != 10000; ++i) if (!store.maintain()) { inspected->set_value(false); return; }
		std::printf("V2 benchmark clean idle maintenance: %.6f ms per call\n", elapsed(start) / 10000);
		start = Clock::now();
		const auto page = store.timeline(PeerToken{ PeerTokenKind::User, 19 }, false, std::nullopt, 40);
		std::printf("V2 benchmark 40-row timeline page: %.3f ms\n", elapsed(start));
		const auto ticket = store.prepareCleanup(CleanupKind::EditHistory);
		start = Clock::now();
		const auto cleaned = ticket ? store.cleanupPage(*ticket) : CleanupResult{};
		std::printf("V2 benchmark cleanup page (%d items): %.3f ms\n", cleaned.removed, elapsed(start));
		inspected->set_value(page && page->rows.size() == 40 && ticket && cleaned.effect && cleaned.removed > 0);
	})) return 8;
	if (done.wait_for(std::chrono::seconds(25)) != std::future_status::ready || !done.get()) return 8;
	{
		const auto guard = std::lock_guard(metrics->lock);
		metrics->retiring = true;
	}
	worker.logout(id);
	return wait([&] { return metrics->retired; }) ? 0 : 9;
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
