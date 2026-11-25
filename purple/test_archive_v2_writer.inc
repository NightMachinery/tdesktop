namespace V2Fixtures {

using namespace Purple::ArchiveV2;
using V2Store = Purple::ArchiveV2::Store;

constexpr auto kMarker = ".purple-v2-writer-fixture";
constexpr auto kCrash = 41;
constexpr auto kChunk = 1024 * 1024;

StoreOptions Options(const QString &path, int64 user = 17, char profile = 'p', char login = 'l') {
	return {
		.root = path + u"/archive-v2"_q,
		.profileId = QByteArray(16, profile),
		.accountSlot = QByteArray(16, char(user)),
		.account = AccountKey{ 0, user },
		.profileKey = QByteArray(32, 'k'),
		.loginSecret = QByteArray(32, login),
		.now = [] { return int64(1700000100); },
	};
}

QString AccountPath(const StoreOptions &options) {
	return options.root + '/' + QString::fromLatin1(options.profileId.toHex())
		+ '/' + QString::fromLatin1(options.accountSlot.toHex());
}

QString ActivePath(const StoreOptions &options, const QByteArray &id) {
	return AccountPath(options) + u"/store-"_q + QString::fromLatin1(id.toHex());
}

QByteArray Read(const QString &path) {
	auto file = QFile(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool Put(const QString &path, const QByteArray &bytes) {
	auto file = QFile(path);
	return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

CapturedEvent Sample(const QByteArray &store, QByteArray event = "first", QByteArray text = "synthetic plain body marker") {
	return {
		.storeId = store,
		.identity = { { PeerTokenKind::User, 19 }, 100 },
		.kind = ChatClass::Direct,
		.eventId = std::move(event),
		.body = std::move(text),
		.observedAt = 1700000010,
		.attachments = { { "complete-fixture-source", "synthetic attachment descriptor", 1, true } },
	};
}

BlobInput SampleBlob(const QByteArray &id, const QByteArray &bytes) {
	return {
		.storeId = id,
		.identity = Sample(id).identity,
		.sequence = 1,
		.ordinal = 0,
		.size = bytes.size(),
		.digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256),
		.source = "complete-fixture-source",
		.read = [bytes, offset = qsizetype(0)](qsizetype maximum) mutable {
			const auto result = bytes.mid(offset, maximum);
			offset += result.size();
			return result;
		},
	};
}

int64 Count(const QString &path, QString sql) {
	auto database = DesktopArchiveSqlite();
	if (!database.Open(path, false, true).ok) return -1;
	const auto read = database.Read({ std::move(sql), {}, ArchiveSqlExpectation::OneRow });
	if (!read.ok) return -1;
	const auto value = std::get_if<int64>(&read.rows.front().front());
	return value ? *value : -1;
}

StoreEffect ConfirmReset(V2Store &store, const QByteArray &id) {
	auto review = store.reviewReset(id);
	const auto token = std::get_if<ResetReview>(&review);
	return token ? store.reset(*token) : StoreEffect{ std::get<QString>(review) };
}

int CrashChild(const QString &root, const QString &mode, Phase phase) {
	const auto marker = root + '/' + QString::fromLatin1(kMarker);
	if (QFileInfo(root).canonicalFilePath() != root || Read(marker) != "archive-v2-writer-fixture-only") return 2;
	auto options = Options(root);
	auto armed = false;
	auto parts = 0;
	options.checkpoint = [&](Phase p) {
		if (armed && p == phase && (p != Phase::BlobPart || ++parts == 5)) std::_Exit(kCrash);
		return true;
	};
	auto store = V2Store(std::move(options));
	if (!store.open()) return 3;
	armed = true;
	if (mode == u"text"_q) {
		auto event = Sample(store.id(), "second", "updated body marker");
		event.event = Observation::Edit;
		static_cast<void>(store.capture(event));
	} else if (mode == u"reset"_q) {
		static_cast<void>(ConfirmReset(store, store.id()));
	} else if (mode == u"blob"_q) {
		static_cast<void>(store.writeBlob(SampleBlob(store.id(), QByteArray(5 * kChunk, 'f'))));
	}
	return 4;
}

void Crash(const QString &root, const QString &mode, Phase phase) {
	auto child = QProcess();
	child.start(QCoreApplication::applicationFilePath(), { u"archive-v2-crash-fixture"_q, root, mode,
		QString::number(int(phase)) });
	CHECK(child.waitForStarted());
	CHECK(child.waitForFinished(15000));
	CHECK(child.exitStatus() == QProcess::NormalExit && child.exitCode() == kCrash);
}

void CrashCuts() {
	Section = "v2 process exits around SQLite, blobs and pointer retirement";
	for (const auto &[mode, phase] : std::vector<std::pair<QString, Phase>>{
		{ u"text"_q, Phase::BeforeSqlCommit }, { u"text"_q, Phase::AfterSqlCommit },
		{ u"blob"_q, Phase::BlobPart }, { u"blob"_q, Phase::AfterSqlCommit },
		{ u"reset"_q, Phase::BeforePublish }, { u"reset"_q, Phase::AfterPublish },
		{ u"reset"_q, Phase::AfterDetach }, { u"reset"_q, Phase::BeforeUnlink },
		{ u"reset"_q, Phase::AfterUnlink },
	}) {
		const auto temp = QTemporaryDir();
		CHECK(temp.isValid());
		CHECK(Put(temp.filePath(QString::fromLatin1(kMarker)), "archive-v2-writer-fixture-only"));
		const auto legacy = temp.filePath(u"previous-v1.fixture"_q);
		CHECK(Put(legacy, "untouched prior archive"));
		auto id = QByteArray();
		{
			auto store = V2Store(Options(temp.path()));
			CHECK(store.open());
			id = store.id();
			CHECK(store.capture(Sample(id)).committed);
		}
		Crash(temp.path(), mode, phase);
		const auto options = Options(temp.path());
		const auto original = ActivePath(options, id);
		const auto database = original + u"/archive.sqlite"_q;
		if (mode == u"blob"_q) {
			const auto committed = phase == Phase::AfterSqlCommit;
			CHECK(Count(database, u"SELECT count(*) FROM blobs"_q) == 1);
			CHECK(Count(database, u"SELECT count(*) FROM blob_parts"_q) == (committed ? 5 : 4));
			CHECK(Count(database, u"SELECT count(*) FROM media WHERE blob_key IS NOT NULL"_q) == (committed ? 1 : 0));
		}
		auto store = V2Store(Options(temp.path()));
		CHECK(store.open());
		if (mode == u"blob"_q && phase == Phase::BlobPart) {
			CHECK(Count(database, u"SELECT count(*) FROM blobs"_q) == 0);
			CHECK(Count(database, u"SELECT count(*) FROM blob_parts"_q) == 0);
			CHECK(store.usage() && store.usage()->timed == 0);
		}
		const auto replaced = mode == u"reset"_q && phase != Phase::BeforePublish;
		CHECK((store.id() != id) == replaced);
		const auto body = store.body(Sample(id).identity, 1);
		CHECK(bool(body) != replaced);
		if (body) CHECK(body->body == Sample(id).body && body->observedAt == Sample(id).observedAt);
		const auto second = store.body(Sample(id).identity, 2);
		CHECK(bool(second) == (mode == u"text"_q && phase == Phase::AfterSqlCommit));
		if (second) CHECK(second->body == "updated body marker");
		CHECK(QFileInfo::exists(original) != replaced);
		CHECK(Read(legacy) == "untouched prior archive");
		CHECK(QDir(AccountPath(options)).entryList({ u"retired-*"_q }, QDir::Dirs).empty());
	}
}

void KeysAndIsolation() {
	Section = "v2 profile, account, login, plaintext exclusion and old generation";
	const auto temp = QTemporaryDir();
	auto id = QByteArray();
	const auto options = Options(temp.path());
	auto event = Sample({});
	{
		auto a = V2Store(Options(temp.path()));
		auto b = V2Store(Options(temp.path(), 18));
		auto c = V2Store(Options(temp.path(), 17, 'z'));
		CHECK(a.open() && b.open() && c.open());
		id = a.id();
		event.storeId = id;
		CHECK(id != b.id() && id != c.id());
		CHECK(a.capture(event).committed);
		CHECK(b.capture(event).reason == u"scope_retired"_q);
		CHECK(!b.body(event.identity, 1) && !c.body(event.identity, 1));
		auto held = V2Store(Options(temp.path()));
		CHECK(held.open().reason == u"store_busy"_q);
		auto edit = Sample(id, "two", "B");
		edit.event = Observation::Edit;
		CHECK(a.capture(edit).committed);
		edit.eventId = "three";
		edit.body = event.body;
		CHECK(a.capture(edit).committed);
		CHECK(!a.capture(edit).committed);
		const auto first = a.body(event.identity, 1);
		const auto second = a.body(event.identity, 2);
		const auto third = a.body(event.identity, 3);
		CHECK(first && third && first->observedAt == third->observedAt);
		CHECK(second && second->body == "B");
		const auto bytes = QByteArray(kChunk + 20, 'X') + "synthetic complete media marker";
		CHECK(a.writeBlob(SampleBlob(id, bytes)).committed);
		const auto path = ActivePath(options, id) + u"/archive.sqlite"_q;
		CHECK(QFileInfo::exists(path + u"-wal"_q));
		for (const auto &suffix : { QString(), u"-wal"_q, u"-shm"_q }) {
			const auto disk = Read(path + suffix);
			CHECK(!disk.contains(event.body));
			CHECK(!disk.contains("synthetic attachment descriptor"));
			CHECK(!disk.contains("synthetic complete media marker"));
		}
	}
	const auto pointer = AccountPath(options) + u"/active.v2"_q;
	const auto published = Read(pointer);
	CHECK(!published.isEmpty() && !published.contains(QByteArray(32, 'l')));
	{
		auto wrong = Options(temp.path());
		wrong.profileKey = QByteArray(32, 'x');
		auto store = V2Store(std::move(wrong));
		CHECK(store.open().reason == u"pointer_authentication_failed"_q);
	}
	{
		auto changed = V2Store(Options(temp.path(), 17, 'p', 'w'));
		CHECK(changed.open().reason == u"key_unavailable"_q);
	}
	CHECK(Read(pointer) == published);
	{
		auto allow = true;
		auto correct = Options(temp.path());
		correct.checkpoint = [&](Phase phase) { return allow || phase != Phase::BeforePublish; };
		auto store = V2Store(std::move(correct));
		CHECK(store.open() && store.id() == id);
		allow = false;
		CHECK(!ConfirmReset(store, id).committed && store.current());
		const auto retained = store.body(event.identity, 1);
		CHECK(Read(pointer) == published && retained && retained->body == event.body);
		allow = true;
		CHECK(ConfirmReset(store, id).committed && !store.current());
		CHECK(!store.body(event.identity, 1));
	}
	auto fresh = V2Store(Options(temp.path()));
	CHECK(fresh.open() && fresh.id() != id);
	CHECK(fresh.capture(event).reason == u"scope_retired"_q);
	CHECK(!fresh.body(event.identity, 1));
}

void LogoutAndOwnership() {
	Section = "v2 logout retirement keeps an exact ownership proof";
	const auto temp = QTemporaryDir();
	const auto natural = Options(temp.path());
	auto id = QByteArray();
	{
		auto options = Options(temp.path());
		options.checkpoint = [](Phase phase) { return phase != Phase::AfterDetach; };
		const auto lease = options.lease;
		auto store = V2Store(std::move(options));
		CHECK(store.open());
		id = store.id();
		CHECK(store.capture(Sample(id)).committed);
		lease->revoke();
		CHECK(store.capture(Sample(id)).reason == u"scope_retired"_q);
		const auto result = store.logout(id);
		CHECK(result.committed && result.reason == u"cleanup_pending"_q);
		auto noLogin = Options(temp.path());
		noLogin.loginSecret.clear();
		auto held = V2Store(std::move(noLogin));
		CHECK(held.recover().reason == u"store_busy"_q);
	}
	const auto detached = AccountPath(natural) + u"/retired-"_q + QString::fromLatin1(id.toHex());
	const auto outside = temp.filePath(u"held-owned-directory"_q);
	CHECK(QDir().rename(detached, outside));
	CHECK(DesktopArchiveDirectory(detached));
	CHECK(Put(detached + u"/replacement.fixture"_q, "do not touch"));
	{
		auto options = Options(temp.path());
		options.loginSecret.clear();
		auto recovery = V2Store(std::move(options));
		CHECK(recovery.recover().reason == u"cleanup_blocked"_q);
	}
	CHECK(Read(detached + u"/replacement.fixture"_q) == "do not touch");
	CHECK(QDir().rename(detached, temp.filePath(u"replacement-held"_q)));
	CHECK(QDir().rename(outside, detached));
	{
		auto options = Options(temp.path());
		options.loginSecret.clear();
		auto recovery = V2Store(std::move(options));
		CHECK(recovery.recover() && recovery.recover());
	}
	CHECK(!QFileInfo::exists(detached));
	auto replacement = V2Store(Options(temp.path(), 17, 'p', 'n'));
	CHECK(replacement.open() && replacement.id() != id);
	CHECK(replacement.capture(Sample(id)).reason == u"scope_retired"_q);
	CHECK(replacement.logout(id).reason == u"scope_retired"_q && replacement.current());
	CHECK(Read(temp.filePath(u"replacement-held/replacement.fixture"_q)) == "do not touch");

	const auto target = temp.filePath(u"real-parent"_q);
	const auto alias = temp.filePath(u"not-a-safe-parent"_q);
	CHECK(QDir().mkdir(target) && QFile::link(target, alias));
	auto unsafe = V2Store(Options(alias));
	CHECK(unsafe.open().reason == u"store_unreadable"_q);
	CHECK(!QFileInfo::exists(target + u"/archive-v2"_q));
}

void CorruptStoreRefuses() {
	Section = "v2 authenticates indexed metadata and refuses corrupt stores without initialization";
	const auto temp = QTemporaryDir();
	const auto options = Options(temp.path());
	auto id = QByteArray();
	{
		auto store = V2Store(Options(temp.path()));
		CHECK(store.open());
		id = store.id();
		CHECK(store.capture(Sample(id)).committed);
	}
	const auto path = ActivePath(options, id) + u"/archive.sqlite"_q;
	const auto pointer = AccountPath(options) + u"/active.v2"_q;
	const auto published = Read(pointer);
	{
		auto database = DesktopArchiveSqlite();
		CHECK(database.Open(path, false).ok);
		CHECK(database.Read({ u"UPDATE messages SET status=1"_q, {}, ArchiveSqlExpectation::OneChange }).ok);
	}
	{
		auto store = V2Store(Options(temp.path()));
		CHECK(store.open());
		CHECK(!store.body(Sample(id).identity, 1));
		CHECK(store.capture(Sample(id, "new", "later")).reason == u"store_unreadable"_q);
	}
	CHECK(Put(path, "corrupt initialized fixture"));
	{
		auto store = V2Store(Options(temp.path()));
		CHECK(store.open().reason == u"store_unreadable"_q);
	}
	CHECK(Read(pointer) == published);
	CHECK(Read(path) == "corrupt initialized fixture");
}

void UnreadableRetirement() {
	Section = "v2 authenticated unreadable generations can be reset and retired on logout";
	for (auto mode = 0; mode != 4; ++mode) {
		for (const auto logout : { false, true }) {
			const auto temp = QTemporaryDir();
			const auto natural = Options(temp.path());
			auto id = QByteArray();
			{
				auto store = V2Store(Options(temp.path()));
				CHECK(store.open());
				id = store.id();
				CHECK(store.capture(Sample(id)).committed);
			}
			const auto directory = ActivePath(natural, id);
			const auto path = directory + u"/archive.sqlite"_q;
			const auto pointer = AccountPath(natural) + u"/active.v2"_q;
			auto options = Options(temp.path());
			if (mode == 0) options.loginSecret = QByteArray(32, 'w');
			if (mode == 1) CHECK(Put(path, "truncated initialized fixture"));
			if (mode == 2) {
				auto database = DesktopArchiveSqlite();
				CHECK(database.Open(path, false).ok);
				CHECK(database.Read({ u"DROP TABLE meta"_q }).ok);
			}
			if (mode == 3) {
				auto bytes = Read(pointer);
				CHECK(bytes.size() > 4);
				bytes[4] = char(bytes[4] ^ 1);
				CHECK(Put(pointer, bytes));
			}
			const auto published = Read(pointer);
			auto store = V2Store(std::move(options));
			CHECK(!store.open());
			const auto effect = logout ? store.logout(id) : ConfirmReset(store, id);
			CHECK(effect.committed == (mode != 3));
			if (mode == 3) CHECK(effect.reason == u"pointer_authentication_failed"_q);
			CHECK(QFileInfo::exists(directory) == (mode == 3));
			CHECK((Read(pointer) == published) == (mode == 3));
		}
	}
}

void UnreadableWriterControls() {
	Section = "v2 worker remains available for confirmed reset and logout after open failure";
	for (const auto byLogout : { false, true }) {
		const auto temp = std::make_shared<QTemporaryDir>();
		const auto normal = Options(temp->path());
		auto id = QByteArray();
		{
			auto store = V2Store(Options(temp->path()));
			CHECK(store.open());
			id = store.id();
			CHECK(store.capture(Sample(id)).committed);
		}
		const auto path = ActivePath(normal, id);
		struct Signals {
			std::promise<std::pair<StoreEffect, QByteArray>> opened;
			std::promise<StoreEffect> retired;
			std::promise<V2Store::ResetCheck> review;
		};
		auto signals = std::make_shared<Signals>();
		auto opened = signals->opened.get_future();
		auto retired = signals->retired.get_future();
		auto review = signals->review.get_future();
		auto worker = Writer(Options(temp->path(), 17, 'p', 'n'), [signals, temp, first = true](StoreEffect e, QByteArray actual) mutable {
			if (std::exchange(first, false)) signals->opened.set_value({ e, actual });
			else signals->retired.set_value(e);
		});
		CHECK(opened.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
		if (opened.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
		const auto [failure, actual] = opened.get();
		CHECK(failure.reason == u"key_unavailable"_q && actual == id);
		if (byLogout) {
			worker.logout(id);
		} else {
			CHECK(worker.query(id, [=](V2Store &store) { signals->review.set_value(store.reviewReset(id)); }));
			CHECK(review.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
			if (review.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
			const auto check = review.get();
			const auto token = std::get_if<ResetReview>(&check);
			CHECK(token && token->bytes() > 0 && worker.reset(*token));
		}
		CHECK(retired.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
		if (retired.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
		CHECK(retired.get().committed && !QFileInfo::exists(path));
		auto fresh = V2Store(Options(temp->path(), 17, 'p', 'n'));
		CHECK(fresh.open() && fresh.id() != id);
	}

	const auto temp = QTemporaryDir();
	const auto options = Options(temp.path());
	auto store = V2Store(Options(temp.path()));
	CHECK(store.open());
	const auto id = store.id();
	const auto review = store.reviewReset(id);
	const auto token = std::get_if<ResetReview>(&review);
	CHECK(token);
	if (!token) return;
	CHECK(Put(ActivePath(options, id) + u"/stale.fixture"_q, "size changed"));
	CHECK(store.reset(*token).reason == u"reset_review_stale"_q && store.current());
}
void ActiveDirectorySubstitution() {
	Section = "v2 authenticated active directory identity refuses a substituted tree";
	const auto temp = QTemporaryDir();
	const auto options = Options(temp.path());
	auto id = QByteArray();
	{
		auto store = V2Store(Options(temp.path()));
		CHECK(store.open());
		id = store.id();
		CHECK(store.capture(Sample(id)).committed);
	}
	const auto active = ActivePath(options, id);
	const auto original = temp.filePath(u"original.fixture"_q);
	CHECK(QDir().rename(active, original));
	CHECK(DesktopArchiveDirectory(active));
	CHECK(Put(active + u"/untouched.fixture"_q, "substituted directory"));
	auto store = V2Store(Options(temp.path()));
	CHECK(store.open().reason == u"store_unreadable"_q && store.id() == id);
	CHECK(ConfirmReset(store, id).reason == u"cleanup_blocked"_q);
	CHECK(store.logout(id).reason == u"cleanup_blocked"_q);
	CHECK(Read(active + u"/untouched.fixture"_q) == "substituted directory");
	CHECK(QFileInfo::exists(original + u"/archive.sqlite"_q));
}

struct Signals {
	std::promise<QByteArray> opened;
	std::promise<void> entered;
	std::promise<void> release;
	std::promise<StoreEffect> loggedOut;
	std::atomic<int> reports{0};
};

void QueueCancellation() {
	Section = "v2 writer quota, coalescing and immediate logout cancellation";
	const auto temp = QTemporaryDir();
	const auto state = std::make_shared<Signals>();
	auto options = Options(temp.path());
	const auto released = state->release.get_future().share();
	const auto lease = options.lease;
	options.checkpoint = [state, released](Phase phase) {
		if (phase == Phase::BeforeSqlCommit) {
			state->entered.set_value();
			released.wait();
		}
		return true;
	};
	auto ready = state->opened.get_future();
	auto entered = state->entered.get_future();
	auto logout = state->loggedOut.get_future();
	auto writer = Writer(std::move(options), [state](StoreEffect effect, QByteArray id) {
		if (effect.stage == IncidentStage::Startup) {
			++state->reports;
			state->opened.set_value(effect ? id : QByteArray());
		} else if (effect.stage == IncidentStage::Cleanup) {
			++state->reports;
			state->loggedOut.set_value(std::move(effect));
		}
	});
	const auto unblock = gsl::finally([&] {
		if (released.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) state->release.set_value();
	});
	CHECK(ready.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
	if (ready.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
	const auto id = ready.get();
	CHECK(id.size() == 16 && writer.post(Sample(id)));
	CHECK(entered.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
	const auto item = Sample(id).identity;
	CHECK(writer.postBlob({ .storeId = id, .revision = lease->mediaRevision.load(), .identity = item,
		.sequence = 3, .ordinal = 1 }));
	CHECK(writer.saving(item, 3, 1));
	CHECK(!writer.saving(item, 2, 1) && !writer.saving(item, 3, 0));
	CHECK(writer.post(Sample(id, "coalesced")));
	CHECK(writer.post(Sample(id, "coalesced")));
	for (auto i = 0; i != 255; ++i) CHECK(writer.post(Sample(id, QByteArray::number(i))));
	CHECK(writer.post(Sample(id, "overflow")).reason == u"text_queue_full"_q);
	writer.logout(id);
	CHECK(!lease->live.load() && !writer.saving(item, 3, 1));
	CHECK(writer.post(Sample(id, "stale")).reason == u"scope_retired"_q);
	state->release.set_value();
	CHECK(logout.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
	if (logout.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		const auto result = logout.get();
		CHECK(result.committed && state->reports.load() == 2);
	}
	CHECK(!QFileInfo::exists(ActivePath(Options(temp.path()), id)));
}


void CutoverCredentialsAndQuit() {
	Section = "v2 Desktop credential rotation, startup text and bounded normal quit";
	using namespace std::chrono_literals;
	const auto temp = QTemporaryDir();
	const auto root = temp.path();
	const auto profile = root + u"/profile-native"_q;
	CHECK(DesktopArchiveDirectory(profile));
	const auto options = [&] {
		auto result = Options(root);
		result.profileKey = QByteArray(256, 'p');
		result.loginSecret.clear();
		result.profilePath = profile;
		result.profileNamespace = "test-fixture";
		result.loginKeys = { { QByteArray(16, 'a'), QByteArray(256, 'b') },
			{ QByteArray(16, 'c'), QByteArray(256, 'd') } };
		return result;
	};
	auto expected = QByteArray();
	const auto stop = std::chrono::steady_clock::now() + 3s;
	{
		auto writer = Writer(options(), [&](StoreEffect effect, QByteArray id) {
			if (effect.stage == IncidentStage::Startup && effect) expected = std::move(id);
		});
		CHECK(writer.post(Sample({})));
		CHECK(writer.drainUntil(stop));
		CHECK(!writer.post(Sample({}, "too-late")));
		while (writer.drainUntil(stop)) std::this_thread::sleep_for(2ms);
	}
	CHECK(std::chrono::steady_clock::now() < stop && expected.size() == 16);
	{
		auto moved = options();
		std::reverse(moved.loginKeys.begin(), moved.loginKeys.end());
		auto restored = V2Store(std::move(moved));
		CHECK(restored.open() && restored.id() == expected);
		const auto first = Sample(expected);
		const auto body = restored.body(first.identity, 1);
		CHECK(body && body->body == first.body);
	}
	{
		auto wrong = options();
		wrong.loginKeys.front().second = QByteArray(256, 'z');
		auto otherLogin = V2Store(std::move(wrong));
		CHECK(otherLogin.open().reason == u"key_unavailable"_q);
		CHECK(!otherLogin.logout({}).committed);
	}
	{
		auto initialized = V2Store(options());
		CHECK(initialized.open() && initialized.id() == expected);
		CHECK(initialized.logout({}).committed);
	}
	{
		auto next = V2Store(options());
		CHECK(next.open() && next.id() != expected);
		CHECK(!next.body(Sample(expected).identity, 1));
	}
}

void CutoverDeleteResolution() {
	Section = "v2 non-channel deletes authenticate exact targets and leave collisions alone";
	const auto temp = QTemporaryDir();
	auto store = V2Store(Options(temp.path()));
	CHECK(store.open());
	auto one = Sample(store.id());
	auto channel = one;
	channel.identity.peer = { PeerTokenKind::Channel, 42 };
	auto two = one;
	two.identity.peer = { PeerTokenKind::Chat, 43 };
	CHECK(store.capture(one).committed);
	CHECK(store.capture(channel).committed);
	CHECK(store.capture(two).committed);
	auto deletion = one;
	deletion.event = Observation::Delete;
	deletion.eventId = "delete";
	deletion.identity.peer.id = 0;
	CHECK(store.capture(deletion).reason == u"delete_unresolved"_q);
	one.identity.message++;
	CHECK(store.capture(one).committed);
	deletion.identity.message++;
	CHECK(store.capture(deletion).committed);
	CHECK(store.history(one.identity, 0, 1)->status == MessageStatus::Deleted);
	CHECK(store.history(channel.identity, 0, 1)->status == MessageStatus::Live);
	CHECK(store.history(two.identity, 0, 1)->status == MessageStatus::Live);
}

}
