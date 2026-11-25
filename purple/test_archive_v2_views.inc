namespace V2Fixtures {

void HistoryAndCleanup() {
	Section = "v2 version and timeline keysets, authenticated cleanup cutoff and old media";
	const auto temp = QTemporaryDir();
	const auto options = Options(temp.path());
	auto store = V2Store(options);
	auto other = V2Store(Options(temp.path(), 18));
	CHECK(store.open() && other.open());
	const auto id = store.id();
	auto event = Sample(id, "a", "A");
	const auto message = event.identity;
	CHECK(store.capture(event).committed);
	event.event = Observation::Edit;
	event.eventId = "b";
	event.body = "B";
	CHECK(store.capture(event).committed);
	const auto replay = event;
	event.eventId = "c";
	event.body = "C";
	CHECK(store.capture(event).committed);
	const auto first = store.history(message, 0, 2);
	CHECK(first && first->rows.size() == 2 && first->before == 2);
	if (!first || first->rows.size() != 2) return;
	CHECK(first->rows[0].value.body == "C" && first->rows[1].value.body == "B");
	event.eventId = "d";
	event.body = "D";
	CHECK(store.capture(event).committed);
	const auto second = store.history(message, first->before, 2);
	CHECK(second && second->rows.size() == 1 && second->rows.front().value.body == "A" && second->before == 0);
	const auto edits = store.prepareCleanup(CleanupKind::EditHistory, message.peer);
	CHECK(edits && other.cleanupPage(*edits).effect.reason == u"scope_retired"_q);
	if (!edits) return;
	auto pending = SampleBlob(id, "complete payload for selected head");
	pending.sequence = 4;
	const auto cleared = store.cleanupPage(*edits);
	CHECK(cleared.effect.committed && cleared.complete && cleared.removed == 3);
	const auto head = store.history(message, 0, 64);
	CHECK(head && head->rows.size() == 1 && head->rows.front().value.body == "D");
	CHECK(store.capture(replay) && !store.capture(replay).committed);
	CHECK(!store.body(message, 5));
	CHECK(store.writeBlob(pending).reason == u"scope_retired"_q);
	pending.revision = options.lease->mediaRevision.load();
	CHECK(store.writeBlob(pending).committed);
	const auto saved = store.media(message, 4);
	CHECK(saved && saved->size() == 1 && saved->front().savedBytes == pending.size && saved->front().reason.isEmpty());
	auto exported = QByteArray();
	const auto exportPart = [&](const QByteArray &part) { exported += part; return true; };
	CHECK(!store.readSaved(other.id(), message, 4, 0, exportPart) && exported.isEmpty());
	CHECK(store.readSaved(id, message, 4, 0, exportPart) && exported == "complete payload for selected head");
	auto badInput = SampleBlob(id, "expected complete payload");
	badInput.sequence = 4;
	badInput.digest = QByteArray(32, 'q');
	badInput.revision = options.lease->mediaRevision.load();
	CHECK(store.writeBlob(badInput).reason == u"scope_retired"_q);
	CHECK(store.media(message, 4)->front().savedBytes == pending.size);

	const auto terminal = [&](int offset, Observation kind, QByteArray preimage = {}) {
		auto row = Sample(id, QByteArray::number(offset));
		row.identity.message += offset;
		row.event = kind;
		row.body.clear();
		row.preimage = std::move(preimage);
		return row;
	};
	auto deleted = terminal(1, Observation::Delete, "earlier body known at deletion");
	auto expired = terminal(2, Observation::Expire);
	auto foreign = terminal(3, Observation::Consume, "different chat");
	++foreign.identity.peer.id;
	CHECK(store.capture(deleted).committed && store.capture(expired).committed && store.capture(foreign).committed);
	const auto missing = store.history(expired.identity, 0, 1);
	CHECK(missing && missing->missingPreimage && missing->rows.empty());
	auto archive = store.timeline(message.peer, true, {}, 1);
	CHECK(archive && archive->before && archive->rows.size() == 1);
	if (!archive || !archive->before) return;
	const auto cursor = archive->before;
	CHECK(!other.timeline(message.peer, true, cursor, 1));
	CHECK(!store.timeline(foreign.identity.peer, true, cursor, 1));
	CHECK(!store.timeline(message.peer, false, cursor, 1));
	const auto older = store.timeline(message.peer, true, cursor, 1);
	CHECK(older && older->rows.size() == 1 && older->rows.front().identity != archive->rows.front().identity);
	for (const auto transition : { Observation::Consume, Observation::Expire, Observation::Delete }) {
		auto original = Sample(id);
		original.identity.message += 20 + int(transition);
		original.identity.peer.id += 20;
		original.observedAt -= 2 * 86400;
		CHECK(store.capture(original).committed);
		auto attachment = SampleBlob(id, "Complete retained media for a terminal message");
		attachment.identity = original.identity;
		attachment.revision = options.lease->mediaRevision.load();
		CHECK(store.writeBlob(attachment).committed);
		original.event = transition;
		original.eventId += "/terminal";
		CHECK(store.capture(original).committed);
		const auto timeline = store.timeline(original.identity.peer, true, {}, 64);
		CHECK(timeline && std::ranges::any_of(timeline->rows, [&](const auto &row) { return row.identity == original.identity; }));
		const auto history = store.history(original.identity, 0, 1);
		CHECK(history && history->status != MessageStatus::Live && history->rows.size() == 1
			&& history->rows[0].media.size() == 1 && history->rows[0].media[0].savedBytes == attachment.size);
		auto bytes = QByteArray();
		CHECK(store.readSaved(id, original.identity, 1, 0, [&](const QByteArray &part) { bytes += part; return true; }));
		CHECK(bytes == "Complete retained media for a terminal message");
	}
	const auto trash = store.prepareCleanup(CleanupKind::DeletedMessages, message.peer);
	CHECK(trash);
	if (!trash) return;
	auto late = terminal(4, Observation::Consume, "after confirmation");
	late.observedAt -= 30;
	CHECK(store.capture(late).committed);
	CHECK(store.cleanupPage(*trash).removed == 1);
	CHECK(!store.body(deleted.identity, 1) && store.body(late.identity, 1) && store.body(foreign.identity, 1));
	CHECK(store.cleanupPage(*trash).complete);
	auto resurrect = Sample(id, "replayed baseline");
	resurrect.identity = deleted.identity;
	CHECK(!store.capture(resurrect).committed && !store.body(deleted.identity, 1));
	const auto tombstone = store.history(deleted.identity, 0, 1);
	CHECK(tombstone && tombstone->status == MessageStatus::Deleted && tombstone->rows.empty());
	const auto shown = store.timeline(message.peer, true, {}, 64);
	CHECK(shown && shown->rows.size() == 2 && std::none_of(shown->rows.begin(), shown->rows.end(),
		[&](const TimelineEntry &entry) { return entry.identity == deleted.identity; }));
	const auto stats = store.storageSummary();
	CHECK(stats && stats->pending == 0 && stats->ordinaryCap == 512 * kMiB && stats->timedCap == 256 * kMiB);
	CHECK(stats && stats->active.database > 0 && !stats->overLimit);
}

void ConfirmedCleanupScrubsFiles() {
	Section = "v2 confirmed cleanup scrubs removed ciphertext and truncates WAL";
	for (const auto kind : { CleanupKind::EditHistory, CleanupKind::DeletedMessages }) {
		const auto temp = QTemporaryDir();
		const auto options = Options(temp.path());
		auto store = V2Store(options);
		CHECK(store.open());
		const auto id = store.id();
		const auto path = ActivePath(options, id) + u"/archive.sqlite"_q;
		auto event = Sample(id, "first", QByteArray(180, 'A'));
		CHECK(store.capture(event).committed);
		CHECK(store.writeBlob(SampleBlob(id, QByteArray(220, 'B'))).committed);
		event.eventId = "second";
		event.body = QByteArray(160, 'C');
		event.event = kind == CleanupKind::EditHistory ? Observation::Edit : Observation::Delete;
		CHECK(store.capture(event).committed);
		auto removed = std::vector<QByteArray>();
		{
			auto probe = DesktopArchiveSqlite();
			CHECK(probe.Open(path, false).ok);
			for (const auto &query : { u"SELECT sealed_body FROM versions WHERE seq=1"_q,
					u"SELECT sealed_descriptor FROM media WHERE seq=1"_q, u"SELECT sealed_bytes FROM blob_parts"_q }) {
				const auto row = probe.Read({ query, {}, ArchiveSqlExpectation::OneRow });
				CHECK(row.ok);
				if (!row.ok) continue;
				const auto bytes = std::get_if<QByteArray>(&row.rows.front().front());
				CHECK(bytes && bytes->size() > 32);
				if (bytes && bytes->size() > 32) removed.push_back(*bytes);
			}
			CHECK(probe.CheckpointV2().ok);
		}
		const auto present = [&](const QByteArray &bytes, const QString &suffix) { return Read(path + suffix).contains(bytes); };
		CHECK(removed.size() == 3);
		for (const auto &bytes : removed) CHECK(present(bytes, {}) || present(bytes, u"-wal"_q));
		const auto ticket = store.prepareCleanup(kind);
		CHECK(ticket);
		if (!ticket) continue;
		{
			auto reader = DesktopArchiveSqlite();
			CHECK(reader.Open(path, false, true).ok && reader.BeginRead());
			CHECK(reader.Read({ u"SELECT count(*) FROM versions"_q, {}, ArchiveSqlExpectation::OneRow }).ok);
			auto unrelated = Sample(id, "snapshot", "another live message");
			unrelated.identity.message += 1;
			CHECK(store.capture(unrelated).committed);
			const auto blocked = store.cleanupPage(*ticket);
			CHECK(blocked.effect.reason == u"cleanup_pending"_q && !blocked.effect.committed);
			CHECK(reader.EndRead());
		}
		CHECK(store.cleanupPage(*ticket).effect);
		for (const auto &bytes : removed) CHECK(!present(bytes, {}) && !present(bytes, u"-wal"_q));
		CHECK(QFileInfo(path + u"-wal"_q).size() == 0);
		if (kind == CleanupKind::EditHistory) CHECK(store.body(event.identity, 2));
	}
}

void ConfirmedLargeScrubResumes() {
	Section = "v2 confirmed large-file scrub resumes after interruption with bounded WAL";
	const auto temp = QTemporaryDir();
	auto path = QString();
	auto id = QByteArray();
	auto markers = std::vector<QByteArray>();
	auto walPeak = int64(0);
	auto cut = true;
	auto cutLast = false;
	const auto configure = [&] {
		auto options = Options(temp.path());
		options.checkpoint = [&](Phase step) {
			if (step != Phase::CleanupCommit) return true;
			walPeak = std::max(walPeak, QFileInfo(path + u"-wal"_q).size());
			if (cutLast && Count(path, u"SELECT count(*) FROM blob_parts"_q) == 0) {
				cutLast = false;
				return false;
			}
			return !std::exchange(cut, false);
		};
		return options;
	};
	{
		const auto options = configure();
		auto store = V2Store(options);
		CHECK(store.open());
		id = store.id();
		path = ActivePath(options, id) + u"/archive.sqlite"_q;
		auto event = Sample(id);
		CHECK(store.capture(event).committed);
		CHECK(store.writeBlob(SampleBlob(id, QByteArray(21 * kMiB, 'W'))).committed);
		{
			auto probe = DesktopArchiveSqlite();
			CHECK(probe.Open(path, false, true).ok);
			const auto parts = probe.Read({ u"SELECT substr(sealed_bytes,1,32) FROM blob_parts ORDER BY part_no"_q,
				{}, ArchiveSqlExpectation::Rows });
			CHECK(parts.ok && parts.rows.size() == 21);
			if (parts.ok) for (const auto &part : parts.rows) markers.push_back(std::get<QByteArray>(part.front()));
		}
		event.event = Observation::Edit;
		event.eventId = "new-head";
		event.body = "kept version";
		CHECK(store.capture(event).committed);
		const auto ticket = store.prepareCleanup(CleanupKind::EditHistory);
		CHECK(ticket);
		if (!ticket) return;
		const auto interrupted = store.cleanupPage(*ticket);
		CHECK(interrupted.effect.committed && interrupted.effect.reason == u"cleanup_pending"_q && !interrupted.complete);
		CHECK(!store.body(event.identity, 1));
		CHECK(Count(path, u"SELECT count(*) FROM blobs WHERE sealed_manifest=x''"_q) == 1);
	}
	const auto saved = Read(path);
	CHECK(!markers.empty() && std::any_of(markers.begin(), markers.end(), [&](const auto &mark) { return saved.contains(mark); }));
	auto reopened = V2Store(configure());
	CHECK(reopened.open() && reopened.id() == id);
	cutLast = true;
	auto tailInterrupted = false;
	for (auto i = 0; i != 128 && Count(path, u"SELECT count(*) FROM blob_parts"_q) > 0; ++i) {
		const auto result = reopened.maintain();
		if (!result) {
			CHECK(result.reason == u"cleanup_pending"_q && result.committed);
			tailInterrupted = true;
		}
	}
	CHECK(tailInterrupted && !cutLast && reopened.maintain());
	CHECK(Count(path, u"SELECT count(*) FROM blobs"_q) == 0);
	CHECK(Count(path, u"SELECT count(*) FROM blob_parts"_q) == 0);
	const auto disk = Read(path);
	const auto wal = Read(path + u"-wal"_q);
	for (const auto &mark : markers) CHECK(!disk.contains(mark) && !wal.contains(mark));
	CHECK(wal.isEmpty() && walPeak > 0 && walPeak <= kWalHeadroom);
	CHECK(reopened.body(Sample(id).identity, 2));
	auto deleted = Sample(id);
	deleted.identity.message += 1;
	deleted.attachments.clear();
	for (auto i = 0; i != 9; ++i) {
		deleted.eventId = QByteArray::number(i);
		deleted.body = QByteArray(kMiB, char(i + 1));
		deleted.event = i ? Observation::Edit : Observation::Baseline;
		CHECK(reopened.capture(deleted).committed);
	}
	deleted.eventId = "terminal";
	deleted.event = Observation::Delete;
	CHECK(reopened.capture(deleted).committed);
	const auto trash = reopened.prepareCleanup(CleanupKind::DeletedMessages);
	CHECK(trash);
	if (trash) {
		const auto result = reopened.cleanupPage(*trash);
		CHECK(result.effect && result.complete && result.removed == 1);
	}
	CHECK(!reopened.body(deleted.identity, 1) && !reopened.body(deleted.identity, 9));
	CHECK(QFileInfo(path + u"-wal"_q).size() == 0 && walPeak <= kWalHeadroom);
	std::printf("V2 confirmed scrub peak WAL: %lld bytes\n", static_cast<long long>(walPeak));
}

void PendingBytesAndMediaOutcome() {
	Section = "v2 row-level failed media outcome and published retirement size";
	const auto temp = QTemporaryDir();
	auto options = Options(temp.path());
	auto canDetach = false;
	options.checkpoint = [&](Phase phase) { return phase != Phase::BeforeDetach || canDetach; };
	auto store = V2Store(options);
	CHECK(store.open());
	const auto id = store.id();
	const auto event = Sample(id);
	CHECK(store.capture(event).committed);
	auto incomplete = SampleBlob(id, "complete planned content");
	incomplete.read = [](qsizetype) { return QByteArray("short"); };
	CHECK(store.writeBlob(incomplete).reason == u"copy_incomplete"_q);
	const auto described = store.media(event.identity, 1);
	CHECK(described && described->size() == 1 && !described->front().savedBytes
		&& described->front().reason == u"copy_incomplete"_q);
	store.noteMediaFailure(incomplete, u"transfer_failed");
	CHECK(store.media(event.identity, 1)->front().reason == u"transfer_failed"_q);
	store.noteMediaFailure(incomplete, u"media_queue_full");
	CHECK(store.media(event.identity, 1)->front().reason == u"media_queue_full"_q);
	++incomplete.revision;
	store.noteMediaFailure(incomplete, u"expired");
	CHECK(store.media(event.identity, 1)->front().reason == u"media_queue_full"_q);
	const auto reset = ConfirmReset(store, id);
	CHECK(reset.committed && reset.reason == u"cleanup_pending"_q);
	const auto pending = store.pendingBytes();
	CHECK(pending && *pending > 0);
	canDetach = true;
	CHECK(store.recover() && store.pendingBytes() == 0);
}

void ConfirmedPagesAndNoticeDeduplication() {
	Section = "v2 bounded pages, writer confirmation barrier and per-episode notices";
	const auto temp = QTemporaryDir();
	auto initial = std::promise<QByteArray>();
	auto options = Options(temp.path());
	auto writer = Writer(options, [&](StoreEffect result, QByteArray id) {
		if (result.stage == IncidentStage::Startup && result) initial.set_value(std::move(id));
	});
	auto ready = initial.get_future();
	CHECK(ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	if (ready.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
	const auto id = ready.get();
	auto event = Sample(id);
	for (auto i = 0; i != 67; ++i) {
		event.event = i ? Observation::Edit : Observation::Baseline;
		event.eventId = event.body = QByteArray::number(i);
		CHECK(writer.post(event));
	}
	auto tested = std::promise<std::vector<int>>();
	auto result = tested.get_future();
	CHECK(writer.query(id, [&](V2Store &store) {
		const auto ticket = store.prepareCleanup(CleanupKind::EditHistory);
		if (!ticket) { tested.set_value({}); return; }
		const auto first = store.cleanupPage(*ticket);
		const auto second = store.cleanupPage(*ticket);
		tested.set_value({ first.removed, int(first.complete), second.removed, int(second.complete) });
	}));
	CHECK(result.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
	if (result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		CHECK((result.get() == std::vector<int>{ 64, 0, 2, 1 }));
	}
	auto attention = Attention();
	using Stage = IncidentStage;
	using Warn = ArchiveWarning;
	CHECK(attention.observe(Stage::Startup, {}, false).reason.isEmpty());
	CHECK(attention.observe(Stage::WholeFile, u"disk_full", false).warning == Warn::None);
	CHECK(attention.observe(Stage::Cleanup, u"cleanup_pending", false).warning == Warn::None);
	CHECK(attention.observe(Stage::Capture, u"text_queue_full", false).warning == Warn::MessageLost);
	CHECK(attention.observe(Stage::Capture, u"text_write_failed", false).warning == Warn::None);
	CHECK(attention.observe(Stage::Capture, u"disk_full", false).warning == Warn::CapturePaused);
	CHECK(attention.observe(Stage::Capture, u"store_busy", false).warning == Warn::None);
	CHECK(attention.observe(Stage::Capture, {}, false).reason.isEmpty());
	CHECK(attention.observe(Stage::Maintenance, {}, false).reason == u"capture_resumed");
	CHECK(attention.observe(Stage::Capture, u"text_encode_failed", false).warning == Warn::MessageLost);
	CHECK(attention.observe(Stage::Cleanup, u"untrusted diagnostic sample", false).reason == u"archive_operation_failed");
	CHECK(Attention().observe(Stage::Startup, {}, false).warning == Warn::None);
}

std::map<QString, QByteArray> FixtureTree(const QString &root) {
	auto snapshot = std::map<QString, QByteArray>();
	auto files = QDirIterator(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
	while (files.hasNext()) {
		const auto path = files.next();
		snapshot.emplace(path, Read(path));
	}
	return snapshot;
}

constexpr auto kLegacyFixture = "archive-v2-confirmed-legacy-fixture";

PreviousArchiveOptions LegacyOptions(const QString &root, int64 account = 123) {
	return { root + u"/archives"_q, root + u"/profile"_q, { 0, account }, {},
		QByteArray(32, 'a'), QByteArray(16, '1') };
}

std::pair<QString, QString> SeedPrevious(const PreviousArchiveOptions &first,
		const PreviousArchiveOptions &second) {
	const auto root = first.archiveRoot;
	const auto profileId = QString(32, u'c');
	const auto profile = root + '/' + profileId;
	CHECK(DesktopArchiveDirectory(first.profileRoot));
	for (const auto &directory : { root, root + u"/profile-links"_q, profile, profile + u"/accounts"_q,
			profile + u"/fences"_q, profile + u"/intents"_q, profile + u"/locks"_q }) {
		CHECK(DesktopArchiveDirectory(directory));
	}
	const auto digest = [](const QByteArray &bytes) {
		return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
	};
	const auto write = [&](const QString &path, QJsonObject object) {
		CHECK(DesktopArchiveWrite(path, QJsonDocument(object).toJson(QJsonDocument::Compact)));
	};
	const auto identity = DesktopArchivePersistentIdentity(first.profileRoot);
	const auto marker = QJsonObject{ { u"version"_q, 1 }, { u"profile"_q, profileId },
		{ u"identity"_q, QString::fromLatin1(identity) } };
	write(first.profileRoot + u"/purple-archive-profile.json"_q, marker);
	write(root + u"/profile-links/"_q + digest(identity) + u".json"_q, marker);
	auto accounts = QJsonObject();
	const auto seed = [&](const PreviousArchiveOptions &options, QChar suffix) {
		const auto name = AccountKeyText(options.account.environment, options.account.userId).value();
		const auto store = QString(32, suffix);
		const auto path = profile + u"/accounts/"_q + name;
		const auto directory = path + '/' + store;
		CHECK(DesktopArchiveDirectory(path));
		CHECK(DesktopArchiveDirectory(directory));
		CHECK(DesktopArchiveWrite(directory + u"/archive.sqlite"_q, "corrupt read-only fixture"));
		write(profile + u"/fences/"_q + name + u".json"_q, {
			{ u"version"_q, 1 }, { u"account"_q, name }, { u"store"_q, store },
			{ u"generation"_q, u"1"_q }, { u"removing"_q, false },
		});
		accounts.insert(name, QJsonObject{ { u"store"_q, store }, { u"key"_q, QString(32, u'2') },
			{ u"generation"_q, u"1"_q }, { u"phase"_q, u"ready"_q },
			{ u"loginAuthority"_q, digest(options.loginAuthority) },
			{ u"loginAuthKeys"_q, QString::fromLatin1(options.loginAuthKeyIds) } });
		return directory;
	};
	const auto result = std::pair(seed(first, u'a'), seed(second, u'b'));
	write(profile + u"/catalogue.json"_q, { { u"version"_q, 1 }, { u"accounts"_q, accounts } });
	return result;
}

int LegacyCrashChild(const QString &root, bool detached) {
	if (QFileInfo(root).canonicalFilePath() != root || Read(root + u"/confirmed-test.marker"_q) != kLegacyFixture) return 2;
	auto legacy = LegacyOptions(root);
	const auto review = PreviousArchive::ReviewPreviousArchive(legacy);
	if (!review.review) return 3;
	auto checks = 0;
	legacy.active = [&] {
		if (++checks == (detached ? 4 : 3)) std::_Exit(kCrash);
		return true;
	};
	static_cast<void>(PreviousArchive::DeletePreviousArchive(legacy, *review.review));
	return 4;
}

void PreviousArchiveRemoval() {
	for (const auto detached : { false, true }) {
		Section = "v2 previous archive reviewed without opening and resumed after crash";
		const auto temp = QTemporaryDir();
		const auto legacy = LegacyOptions(temp.path());
		const auto other = LegacyOptions(temp.path(), 456);
		const auto [old, otherDir] = SeedPrevious(legacy, other);
		const auto baseline = FixtureTree(temp.path());
		const auto selected = FixtureTree(QFileInfo(old).absolutePath());
		const auto neighbors = FixtureTree(QFileInfo(otherDir).absolutePath());
		auto size = int64(0);
		for (const auto &[path, bytes] : selected) size += bytes.size();
		const auto v2 = Options(temp.path());
		{
			auto store = V2Store(v2);
			CHECK(store.open());
			CHECK(store.capture(Sample(store.id())).committed);
			CHECK(ConfirmReset(store, store.id()).committed);
		}
		{
			auto fresh = V2Store(Options(temp.path()));
			CHECK(fresh.open());
			CHECK(fresh.logout(fresh.id()).committed);
		}
		for (const auto &[path, bytes] : baseline) CHECK(Read(path) == bytes && QFileInfo::exists(path));
		const auto former = PreviousArchive::ReviewPreviousArchive(legacy);
		CHECK(former.review && former.review->bytes() == size && former.reason.isEmpty());
		if (!former.review) continue;
		CHECK(PreviousArchive::DeletePreviousArchive(other, *former.review).reason == u"discard-account-changed"_q);
		auto wrong = legacy;
		wrong.loginAuthority = QByteArray(32, 'z');
		CHECK(!PreviousArchive::ReviewPreviousArchive(wrong).review);
		const auto marker = legacy.profileRoot + u"/purple-archive-profile.json"_q;
		const auto saved = Read(marker);
		CHECK(!saved.isEmpty() && QFile::remove(marker));
		CHECK(PreviousArchive::ReviewPreviousArchive(legacy).reason == u"discard-profile-binding-unavailable"_q);
		CHECK(!QFileInfo::exists(marker));
		CHECK(DesktopArchiveWrite(marker, saved));
		CHECK(DesktopArchiveWrite(old + u"/changed-size.fixture"_q, "added bytes"));
		CHECK(PreviousArchive::DeletePreviousArchive(legacy, *former.review).reason == u"discard-archive-changed"_q);
		CHECK(QFileInfo::exists(old));
		CHECK(DesktopArchiveRemoveFile(old + u"/changed-size.fixture"_q));
		CHECK(Put(temp.filePath(u"confirmed-test.marker"_q), kLegacyFixture));
		auto process = QProcess();
		process.start(QCoreApplication::applicationFilePath(), { u"archive-v2-legacy-crash-fixture"_q, temp.path(),
			detached ? u"after"_q : u"before"_q });
		CHECK(process.waitForStarted() && process.waitForFinished(15000));
		CHECK(process.exitStatus() == QProcess::NormalExit && process.exitCode() == kCrash);
		const auto selectedAccount = QFileInfo(old).absolutePath();
		CHECK(QFileInfo::exists(selectedAccount + u".discard.json"_q));
		CHECK(QFileInfo::exists(selectedAccount + u".discard"_q) == detached);
		CHECK(PreviousArchive::ReviewPreviousArchive(legacy).reason == u"cleanup_pending"_q);
		CHECK(QFileInfo::exists(selectedAccount + u".discard.json"_q));
		auto signedOut = legacy;
		signedOut.loginAuthority.clear();
		signedOut.loginAuthKeyIds.clear();
		CHECK(!PreviousArchive::RetryPreviousArchiveDeletions(signedOut));
		CHECK(!QFileInfo::exists(selectedAccount) && !QFileInfo::exists(selectedAccount + u".discard"_q));
		CHECK(!QFileInfo::exists(selectedAccount + u".discard.json"_q));
		CHECK(FixtureTree(QFileInfo(otherDir).absolutePath()) == neighbors);
	}
}

}
