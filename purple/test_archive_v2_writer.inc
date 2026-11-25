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

int CrashChild(const QString &root, const QString &mode, Phase phase) {
	const auto marker = root + '/' + QString::fromLatin1(kMarker);
	if (QFileInfo(root).canonicalFilePath() != root || Read(marker) != "archive-v2-writer-fixture-only") return 2;
	auto options = Options(root);
	auto armed = false;
	auto parts = 0;
	options.checkpoint = [&](Phase p) {
		if (armed && p == phase && (p != Phase::BlobPart || ++parts == 2)) std::_Exit(kCrash);
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
		static_cast<void>(store.reset(store.id()));
	} else if (mode == u"blob"_q) {
		static_cast<void>(store.writeBlob(SampleBlob(store.id(), QByteArray(2 * kChunk, 'f'))));
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
			CHECK(Count(database, u"SELECT count(*) FROM blobs"_q) == (committed ? 1 : 0));
			CHECK(Count(database, u"SELECT count(*) FROM blob_parts"_q) == (committed ? 2 : 0));
			CHECK(Count(database, u"SELECT count(*) FROM media WHERE blob_key IS NOT NULL"_q) == (committed ? 1 : 0));
		}
		auto store = V2Store(Options(temp.path()));
		CHECK(store.open());
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
		CHECK(store.open().reason == u"key_unavailable"_q);
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
		CHECK(!store.reset(id).committed && store.current());
		const auto retained = store.body(event.identity, 1);
		CHECK(Read(pointer) == published && retained && retained->body == event.body);
		allow = true;
		CHECK(store.reset(id).committed && !store.current());
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
		if (++state->reports == 1) state->opened.set_value(effect ? id : QByteArray());
		else state->loggedOut.set_value(std::move(effect));
	});
	const auto unblock = gsl::finally([&] {
		if (released.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) state->release.set_value();
	});
	CHECK(ready.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
	if (ready.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
	const auto id = ready.get();
	CHECK(id.size() == 16 && writer.post(Sample(id)));
	CHECK(entered.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
	CHECK(writer.post(Sample(id, "coalesced")));
	CHECK(writer.post(Sample(id, "coalesced")));
	for (auto i = 0; i != 255; ++i) CHECK(writer.post(Sample(id, QByteArray::number(i))));
	CHECK(writer.post(Sample(id, "overflow")).reason == u"text_queue_full"_q);
	writer.logout(id);
	CHECK(!lease->live.load());
	CHECK(writer.post(Sample(id, "stale")).reason == u"scope_retired"_q);
	state->release.set_value();
	CHECK(logout.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
	if (logout.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		const auto result = logout.get();
		CHECK(result.committed && state->reports.load() == 2);
	}
	CHECK(!QFileInfo::exists(ActivePath(Options(temp.path()), id)));
}

}
