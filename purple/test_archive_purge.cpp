#include "base/unixtime.h"
#include "base/algorithm.h"
#include "base/weak_ptr.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_archive_journal.h"
#include "purple/purple_desktop_archive_store.h"
#include "tests/archive_test_support.h"

#include <QtCore/QBuffer>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>

#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <set>
#include <thread>

#define private public
#include "data/data_changes.h"
#undef private
#include "archive-native-peer-manager.inc"

namespace Purple {
#include "archive-purge-scope.inc"
#include "archive-purge-worker.inc"
}

using namespace Purple;
using namespace ArchiveTest;

namespace Purple {

struct ProbePurgeSession : base::has_weak_ptr {
	AccountKey account = {0, 123};
};
struct PurgeServicePort {
	std::shared_ptr<ProbePurgeWorker> worker;
	std::shared_ptr<DesktopArchiveJournal> journal;
} PurgeValue;
std::set<ProbePurgeSession *> PurgeRetired;
std::mutex MainQueueMutex;
std::deque<Fn<void()>> MainQueue;

void HarnessOnMain(Fn<void()> callback) {
	const auto lock = std::lock_guard(MainQueueMutex);
	MainQueue.push_back(std::move(callback));
}

void PumpMain() {
	for (;;) {
		auto callback = Fn<void()>();
		{
			const auto lock = std::lock_guard(MainQueueMutex);
			if (MainQueue.empty())
				return;
			callback = std::move(MainQueue.front());
			MainQueue.pop_front();
		}
		callback();
	}
}

AccountKey SessionAccountKey(not_null<ProbePurgeSession *> session) {
	return session->account;
}
PurgeServicePort &PurgeFor(not_null<ProbePurgeSession *>) {
	return PurgeValue;
}

#include "archive-purge-execute.inc"

struct ProbeMigrationPeer {};
struct ProbeMigrationUpdate {
	using Flag = Data::PeerUpdate::Flag;
	using Flags = Data::PeerUpdate::Flags;
	not_null<ProbeMigrationPeer*> peer;
	Flags flags;
};
struct ProbeMigrationChanges {
	Data::Changes::Manager<ProbeMigrationPeer, ProbeMigrationUpdate> native;
	auto realtimePeerUpdates(Data::PeerUpdate::Flag flag) const { return native.realtimeUpdates(flag); }
};
struct ProbeMigrationSession {
	ProbeMigrationChanges source;
	ProbeMigrationChanges &changes() { return source; }
};
#include "archive-purge-migration-bind.inc"

}

namespace Purple::MediaRetirementProbe {

struct RetirementSession : base::has_weak_ptr { AccountKey key = {0, 123}; };
struct Worker {
	DesktopArchiveStore *store = nullptr;
	std::deque<std::function<void(DesktopArchiveStore&)>> tasks;
	bool Post(std::function<void(DesktopArchiveStore&)> task, bool = false) { tasks.push_back(std::move(task)); return true; }
	void RevokeOnStop(const std::shared_ptr<std::atomic<bool>>&) {}
	void Run() {
		while (!tasks.empty()) { auto task = std::move(tasks.front()); tasks.pop_front(); task(*store); }
	}
};
struct Journal {
	struct Access { std::function<bool()> current = [] { return true; }; };
	Access access() const { return {}; }
	bool Drain(DesktopArchiveStore&) { return true; }
};
struct Value { std::shared_ptr<Worker> worker; std::shared_ptr<Journal> journal; } Published;
std::set<RetirementSession*> Retired;
std::deque<std::function<void()>> Main;
Value &For(not_null<RetirementSession*>) { return Published; }
AccountKey SessionAccountKey(not_null<RetirementSession*> session) { return session->key; }
template <typename Callback> void RetirementOnMain(Callback callback) { Main.push_back(std::move(callback)); }
void Flush() {
	for (;;) {
		Published.worker->Run();
		if (Main.empty()) return;
		while (!Main.empty()) { auto callback = std::move(Main.front()); Main.pop_front(); callback(); }
	}
}
#include "archive-retirement-open.inc"

}

namespace {

DesktopArchiveOptions Options(const QString &root) {
	CHECK(DesktopArchiveDirectory(root + u"/profile"_q));
	auto result = DesktopArchiveOptions{root + u"/archives"_q,
		root + u"/profile"_q,
		{0, 123},
		u"0"_q,
		QByteArray(256, 'k'),
		true};
	result.loginAuthority = QByteArray(32, 'a');
	result.loginAuthKeyId = result.loginAuthKeyIds = QByteArray(16, '1');
	return result;
}

ArchiveViewScope ViewScope(ArchiveChatKey chat = Key().chat) {
	return {{{0, 123}, chat, {}},
		{},
		ArchiveCollection::Received,
		u"original"_q};
}

ArchivePurgeScope Scope(ArchiveChatKey chat = Key().chat) {
	return {{0, 123}, chat, {}};
}

void Sql(DesktopArchiveStore &store,
	QString sql,
	std::vector<ArchiveSqlValue> values = {},
	ArchiveSqlExpectation expected = ArchiveSqlExpectation::OneChange) {
	CHECK(store.database()
			.Read({std::move(sql), std::move(values), expected})
			.ok);
}

namespace PurgeRouteProbe {

struct Session;
struct Window;
struct Account {
	bool leaving = false;
	Session *current = nullptr;
	bool loggingOut() const { return leaving; }
	Session *maybeSession() const { return current; }
};
struct Session {
	Account owner;
	AccountKey key = {0, 123};
	std::optional<AccountKey> source = AccountKey{0, 123};
	int mode = 1;
	Account &account() { return owner; }
};
struct Root {
	Window *current = nullptr;
	Window *sessionController() const { return current; }
};
struct Window {
	Session *owner = nullptr;
	Root root;
	Session &session() { return *owner; }
	Root &window() { return root; }
};
template <typename Type>
struct Weak {
	std::shared_ptr<Type *> pointer;
	Type *get() const { return *pointer; }
};
struct Guard {
	std::shared_ptr<bool> alive;
	explicit operator bool() const { return *alive; }
};
struct State {
	bool closed = false;
};
AccountKey SessionAccountKey(Session *session) {
	return session->key;
}
std::optional<AccountKey> CachedAppWideSettingsAccount(Session *session) {
	return session->source;
}
int AccountWorkModeForSession(Session *session) {
	return session->mode;
}

void Run() {
	Section
		= "literal view route: opening source/mode/login/controller and "
		  "late callback refusal";
	auto live = Session();
	live.owner.current = &live;
	auto own = Window();
	own.owner = &live;
	own.root.current = &own;
	auto backing = State();
	const auto state = &backing;
	const auto guard = Guard{std::make_shared<bool>(true)};
	const auto session = Weak<Session>{std::make_shared<Session *>(&live)};
	const auto window = Weak<Window>{std::make_shared<Window *>(&own)};
	const auto account = live.key;
	const auto source = live.source;
	const auto mode = live.mode;
	const auto opening = ViewScope();
	auto liveScope = opening;
	const auto scopeCurrent
		= [&] { return SameArchiveViewScope(opening, liveScope); };
#include "archive-purge-route.inc"
	CHECK(current());
	live.source = AccountKey{0, 999};
	CHECK(!current());
	live.source = source;
	live.mode++;
	CHECK(!current());
	live.mode = mode;
	live.key.userId++;
	CHECK(!current());
	live.key = account;
	live.owner.leaving = true;
	CHECK(!current());
	live.owner.leaving = false;
	live.owner.current = nullptr;
	CHECK(!current());
	live.owner.current = &live;
	own.root.current = nullptr;
	CHECK(!current());
	own.root.current = &own;
	*session.pointer = nullptr;
	CHECK(!current());
	*session.pointer = &live;
	*window.pointer = nullptr;
	CHECK(!current());
	*window.pointer = &own;
	backing.closed = true;
	CHECK(!current());
	backing.closed = false;
	*guard.alive = false;
	CHECK(!current());
	*guard.alive = true;
	liveScope.messages.migrationAliases.push_back(
		{ArchiveChatKind::Group, 999});
	CHECK(!current());
	liveScope = opening;
	CHECK(current());
}

}

void Capture(DesktopArchiveStore &store,
	ArchiveMessageKey key,
	QString id,
	bool deleted = true,
	bool attachment = false,
	QString body = u"first"_q) {
	auto first = Event(Empty(), id, 1, body, ArchiveOrigin::Baseline, key);
	first.observation.topicId = key.messageId % 3;
	if (attachment) {
		first.content->media.push_back({u"document:555"_q, u"document"_q});
	}
	CHECK(store.Capture(first).retained);
	if (deleted) {
		CHECK(store
				.Capture(Event(Empty(),
					id + u"deleted"_q,
					2,
					{},
					ArchiveOrigin::AuthoritativeDelete,
					key))
				.retained);
	}
}

void DrainMaintenance(DesktopArchiveStore &store) {
	for (auto i = 0; i != 1024 && store.MaintenancePending(); ++i) {
		const auto step = store.ContinuePurge();
		CHECK(step.valid && step.processedRows <= 128);
		if (!step.valid) {
			return;
		}
	}
	CHECK(!store.MaintenancePending() && store.Current());
}

void DrainGarbage(DesktopArchiveStore &store) {
	auto more = true;
	for (auto i = 0; i != 1024 && more; ++i) {
		CHECK(store.ContinueGarbage(more));
		if (store.MaintenanceHeld()) {
			return;
		}
	}
	CHECK(!more && !store.MaintenanceHeld());
}

void StaleConfirmations() {
	Section
		= "same-snapshot counts and exact confirmation "
		  "revision/scope/kind/store authority";
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path()))
		  == DesktopArchiveStatus::Ready);
	Capture(store, Key(), u"one"_q);
	const auto opening
		= store.PreparePurge(ArchivePurgeKind::Trashbin, Scope());
	CHECK(
		opening.status == ArchiveQueryStatus::Ready && opening.confirmation);
	CHECK(opening.counts[0] == 1 && opening.counts[1] == 1
		  && opening.counts[2] == 1);
	if (!opening.confirmation) {
		return;
	}
	for (auto change = 0; change != 6; ++change) {
		auto stale = *opening.confirmation;
		if (change == 0)
			++stale.epoch.generation;
		if (change == 1)
			++stale.request.expectedRevision;
		if (change == 2)
			stale.request.kind = ArchivePurgeKind::EditHistory;
		if (change == 3)
			++stale.request.scope.account.userId;
		if (change == 4)
			stale.request.scope.chat->kind = ArchiveChatKind::User;
		if (change == 5)
			stale.request.scope.migrationAliases.push_back(
				{ArchiveChatKind::Group, 99});
		CHECK(!store.BeginPurge(stale));
		const auto record = InspectDesktopArchiveMaintenance(
			store.database(), store.epoch(), store.security());
		CHECK(record.valid && !record.purge && store.Current());
	}
	Capture(store, Key({0, 123}, 99), u"concurrent"_q);
	CHECK(!store.BeginPurge(*opening.confirmation));
	CHECK(store.Current() && !store.MaintenancePending());
	store.database().FailForTest(u"step"_q, 1);
	const auto failed
		= store.PreparePurge(ArchivePurgeKind::Trashbin, Scope());
	CHECK(
		failed.status != ArchiveQueryStatus::Ready && !failed.confirmation);
	CHECK(store.Remove());
}

void ScopeAndFreshCapture() {
	Section
		= "131 rows, typed aliases, all topics and message forms, "
		  "tombstones and new generation";
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path()))
		  == DesktopArchiveStatus::Ready);
	for (auto i = 0; i != 131; ++i) {
		Capture(store,
			Key({0, 123}, 45, 1000 + i),
			u"row"_q + QString::number(i));
	}
	const auto alias = Key({0, 123}, 55, 67, ArchiveChatKind::Group);
	Capture(store, alias, u"alias"_q);
	const auto other = Key({0, 123}, 45, 67, ArchiveChatKind::User);
	Capture(store, other, u"same-number-wrong-type"_q);
	Capture(store, Key({0, 123}, 45, 9), u"live"_q, false);
	auto scheduled = Key();
	scheduled.collection = ArchiveCollection::Scheduled;
	auto quick = Key();
	quick.collection = ArchiveCollection::QuickReply;
	quick.collectionId = 7;
	auto translated = Key();
	translated.representation = u"translated"_q;
	Capture(store, scheduled, u"scheduled"_q);
	Capture(store, quick, u"quick"_q);
	Capture(store, translated, u"translated"_q);
	auto scope = Scope();
	scope.migrationAliases.push_back(alias.chat);
	const auto preview
		= store.PreparePurge(ArchivePurgeKind::Trashbin, scope);
	CHECK(preview.confirmation && preview.counts[0] == 136
		  && preview.counts[1] == 135);
	const auto view = store.OpenDeletedView({scope,
		ArchiveTopicScope{Key().chat, 1},
		ArchiveCollection::Received,
		u"original"_q});
	CHECK(view.status == ArchiveQueryStatus::Ready);
	if (!preview.confirmation)
		return;
	CHECK(store.BeginPurge(*preview.confirmation));
	CHECK(view.active && !view.active->load() && !store.Current());
	DrainMaintenance(store);
	CHECK(store.OpenDeletedView({scope}).entries.empty());
	CHECK(store.Versions(alias).versions.empty());
	CHECK(store.Versions(scheduled).versions.empty());
	CHECK(store.Versions(quick).versions.empty());
	CHECK(store.Versions(translated).versions.empty());
	CHECK(store.Versions(other).versions.size() == 1);
	CHECK(store.Versions(Key({0, 123}, 45, 9)).versions.size() == 1);
	const auto access = store.JournalAccess();
	CHECK(access);
	if (!access)
		return;
	auto journal = DesktopArchiveJournal(*access);
	CHECK(journal.Open());
	CHECK(journal.CaptureGeneration(Key().chat).chat == 1);
	CHECK(journal.CaptureGeneration(alias.chat).chat == 1);
	CHECK(journal.CaptureGeneration(other.chat).chat == 0);
	auto fresh = Event(Empty(),
		u"fresh"_q,
		1,
		u"after Finish"_q,
		ArchiveOrigin::Baseline,
		Key({0, 123}, 45, 99999));
	fresh.generation = journal.CaptureGeneration(fresh.key.chat);
	CHECK(journal.Append(fresh) && journal.Drain(store));
	CHECK(store.Versions(fresh.key).versions.size() == 1);
	const auto stale = store.Capture(Event(Empty(),
		u"old-reload"_q,
		99,
		u"must not return"_q,
		ArchiveOrigin::Baseline,
		Key({0, 123}, 45, 1000)));
	CHECK(!stale.retained
		  && store.Versions(Key({0, 123}, 45, 1000)).versions.empty());
	const auto aliasFresh
		= Key({0, 123}, 55, 888888, ArchiveChatKind::Group);
	auto aliasEvent = Event(Empty(),
		u"alias-fresh"_q,
		1,
		u"new group message"_q,
		ArchiveOrigin::Baseline,
		aliasFresh);
	journal.BindCaptureGeneration(aliasEvent);
	CHECK(journal.Append(aliasEvent) && journal.Drain(store));
	auto oldDelete = Event(Empty(),
		u"pre-purge-delete"_q,
		1,
		{},
		ArchiveOrigin::AuthoritativeDelete,
		Key({0, 123}, 1, aliasFresh.messageId, ArchiveChatKind::User));
	oldDelete.observation.context.insert(u"desktopNonChannelDelete"_q, true);
	oldDelete.observation.context.insert(
		u"desktopDeleteGenerations"_q, QJsonObject());
	CHECK(journal.Append(oldDelete) && journal.Drain(store));
	const auto stillLive = store.ReadSlice(aliasFresh);
	CHECK(stillLive && FindArchiveMessage(stillLive->state, aliasFresh)
		  && !FindArchiveMessage(stillLive->state, aliasFresh)->deleted);
	auto freshDelete = oldDelete;
	freshDelete.id = u"post-purge-delete"_q;
	journal.BindCaptureGeneration(freshDelete);
	CHECK(journal.Append(freshDelete) && journal.Drain(store));
	const auto nowDeleted = store.ReadSlice(aliasFresh);
	CHECK(nowDeleted && FindArchiveMessage(nowDeleted->state, aliasFresh)
		  && FindArchiveMessage(nowDeleted->state, aliasFresh)->deleted);
	DrainGarbage(store);
	CHECK(store.Remove());
}

void NamedAccountScope() {
	Section
		= "named account scope changes account generation, preserving "
		  "another account namespace";
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path()))
		  == DesktopArchiveStatus::Ready);
	Capture(store, Key(), u"one"_q);
	Capture(store, Key({0, 123}, 56), u"two"_q);
	const auto preview
		= store.PreparePurge(ArchivePurgeKind::Trashbin, {{0, 123}});
	CHECK(preview.confirmation && preview.counts[1] == 2);
	if (!preview.confirmation)
		return;
	CHECK(store.BeginPurge(*preview.confirmation));
	DrainMaintenance(store);
	CHECK(store.Versions(Key()).versions.empty()
		  && store.Versions(Key({0, 123}, 56)).versions.empty());
	const auto access = store.JournalAccess();
	CHECK(access);
	if (!access)
		return;
	auto journal = DesktopArchiveJournal(*access);
	CHECK(journal.Open());
	CHECK(journal.CaptureGeneration(Key().chat).account == 1);
	auto fresh = Event(Empty(),
		u"new-account-generation"_q,
		1,
		u"fresh"_q,
		ArchiveOrigin::Baseline,
		Key({0, 123}, 56, 999));
	journal.BindCaptureGeneration(fresh);
	CHECK(journal.Append(fresh) && journal.Drain(store));
	CHECK(store.Versions(fresh.key).versions.size() == 1);
	auto wrong = fresh;
	wrong.key.account.userId++;
	CHECK(!journal.Append(wrong));
	CHECK(store.Remove());
}

QString Own(DesktopArchiveStore &store,
	const ArchiveMessageKey &key,
	const QString &version) {
	auto bytes = QByteArray("abc");
	const auto hash = QString::fromLatin1(
		QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
	auto buffer = QBuffer();
	buffer.setData(bytes);
	CHECK(buffer.open(QIODevice::ReadOnly));
	CHECK(
		store.CopyMedia(key, version, u"document:555"_q, buffer, {hash, 3}));
	return hash;
}

void PurgeCommitBoundary() {
	Section = "every purge commit uses Execute; refusal preserves live readers and durable evidence";
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
	Capture(store, Key(), u"boundary"_q, true, true);
	const auto hash = Own(store, Key(), u"boundary"_q);
	const auto shown = store.OpenDeletedView(ViewScope());
	const auto queued = store.OpenDeletedView(ViewScope());
	const auto history = store.Versions(Key());
	CHECK(shown.active && queued.active && history.versions.size() == 1);
	if (!shown.active || !queued.active || history.versions.empty()) return;
	const auto &version = history.versions.front().version;
	const auto media = store.OpenMedia({{history.epoch, Key(), version.id, version.revision,
		ArchiveContentHash(version.content), u"document:555"_q, history.generation}, hash});
	CHECK(media && media->active->load());
	if (!media) return;
	CHECK(shown.active->load() && queued.active->load());
	const auto preview = store.PreparePurge(ArchivePurgeKind::Trashbin, Scope());
	CHECK(preview.confirmation);
	if (!preview.confirmation) return;
	const auto before = InspectDesktopArchiveMaintenance(store.database(), store.epoch(), store.security());
	CHECK(before.valid && !before.purge);
	auto calls = 0;
	const auto refuse = [&](const ArchiveSqlProgram &program) {
		++calls;
		CHECK(program && program.statements.front().sql == u"BEGIN IMMEDIATE"_q
			&& program.statements.back().sql == u"COMMIT"_q);
		return false;
	};
	CHECK(!BeginDesktopArchivePurge(store.database(), store.epoch(), *preview.confirmation,
		u"refused-boundary"_q, store.security(), {}));
	CHECK(calls == 0);
	CHECK(!BeginDesktopArchivePurge(store.database(), store.epoch(), *preview.confirmation,
		u"refused-boundary"_q, store.security(), refuse));
	CHECK(calls == 1);
	store.database().FailForTest(u"commit"_q, 1);
	CHECK(!store.BeginPurge(*preview.confirmation));
	const auto unchanged = InspectDesktopArchiveMaintenance(store.database(), store.epoch(), store.security());
	CHECK(unchanged.valid && !unchanged.purge && unchanged.header == before.header);
	CHECK(store.Current() && shown.active->load() && queued.active->load() && media->active->load());
	CHECK(store.BeginPurge(*preview.confirmation));
	CHECK(!shown.active->load() && !queued.active->load() && !media->active->load());
	const auto pending = InspectDesktopArchiveMaintenance(store.database(), store.epoch(), store.security());
	CHECK(pending.valid && pending.purge && store.MaintenancePending());
	auto ordinary = ArchiveSqlProgram();
	ordinary.status = ArchiveSqlStatus::Ready;
	CHECK(!store.Execute(ordinary));
	CHECK(!ContinueDesktopArchivePurge(store.database(), store.epoch(), store.security(), {}).valid);
	CHECK(!ContinueDesktopArchivePurge(store.database(), store.epoch(), store.security(), refuse).valid);
	CHECK(calls == 2);
	const auto retained = InspectDesktopArchiveMaintenance(store.database(), store.epoch(), store.security());
	CHECK(retained.valid && retained.purge && retained.header == pending.header);
	if (retained.purge && pending.purge) {
		CHECK(retained.purge->phase == pending.purge->phase
			&& retained.purge->cursor == pending.purge->cursor);
	}
	DrainMaintenance(store);
	CHECK(!store.MaintenanceHeld() && store.failure().isEmpty());
	const auto token = ArchiveSqlToken(Key().account, ArchiveIndexKind::Blob, hash.toUtf8(), store.security());
	const auto path = store.directory() + u"/blobs/"_q + token;
	CHECK(QFileInfo::exists(path));
	auto cursor = QString();
	auto jobsComplete = true;
	const auto current = [&] { return store.Current(); };
	const auto state = [&] {
		return store.database().Read({u"SELECT state,envelope FROM blobs WHERE blob_id=?"_q,
			{token}, ArchiveSqlExpectation::OneRow});
	};
	const auto blob = state();
	CHECK(blob.ok && blob.rows.size() == 1);
	CHECK(!ContinueDesktopArchiveGarbage(store.database(), store.epoch(), store.security(),
		store.directory(), current, cursor, jobsComplete, {}).valid);
	CHECK(!ContinueDesktopArchiveGarbage(store.database(), store.epoch(), store.security(),
		store.directory(), current, cursor, jobsComplete, refuse).valid);
	CHECK(calls == 3 && QFileInfo::exists(path));
	const auto untouched = state();
	CHECK(untouched.ok && untouched.rows == blob.rows);
	calls = 0;
	const auto commit = [&](const ArchiveSqlProgram &program) {
		++calls;
		return store.Execute(program);
	};
	CHECK(ContinueDesktopArchiveGarbage(store.database(), store.epoch(), store.security(),
		store.directory(), current, cursor, jobsComplete, commit).valid);
	CHECK(calls == 2 && !QFileInfo::exists(path));
	DrainGarbage(store);
	CHECK(store.Remove());
}

void ReuseAndHeads() {
	Section
		= "reconcile surviving owned media before dropping history; retain "
		  "exact deleted head and chunks";
	for (const auto kind :
		{ArchivePurgeKind::Trashbin, ArchivePurgeKind::EditHistory}) {
		auto temporary = QTemporaryDir();
		auto store = DesktopArchiveStore();
		CHECK(store.Open(Options(temporary.path()))
			  == DesktopArchiveStatus::Ready);
		Capture(store, Key(), u"owner"_q, true, true);
		const auto hash = Own(store, Key(), u"owner"_q);
		const auto live = Key({0, 123}, 45, 68);
		Capture(store, live, u"old-live"_q, false, true);
		auto edit = Event(Empty(),
			u"head"_q,
			2,
			QString(30000, QChar('z')),
			ArchiveOrigin::AuthoritativeEdit,
			live);
		edit.content->media.push_back({u"document:555"_q, u"document"_q});
		CHECK(store.Capture(edit).retained);
		const auto preview = store.PreparePurge(kind, Scope());
		CHECK(preview.confirmation);
		if (!preview.confirmation)
			continue;
		CHECK(store.BeginPurge(*preview.confirmation));
		DrainMaintenance(store);
		const auto heads = store.Versions(live);
		CHECK(heads.status == DesktopArchiveStatus::Ready
			  && !heads.versions.empty());
		if (heads.versions.empty())
			continue;
		CHECK(heads.versions.front().version.id == u"head"_q);
		CHECK(heads.versions.front().version.content.text
			  == edit.content->text);
		CHECK(heads.versions.front().version.media.front().state
			  == ArchiveMediaState::Complete);
		CHECK(heads.versions.front().version.media.front().blobId == hash);
		auto recovered = QByteArray();
		CHECK(store.ReadMedia(live,
			u"head"_q,
			u"document:555"_q,
			[&](const QByteArray &bytes) {
				recovered += bytes;
				return true;
			}));
		CHECK(recovered == QByteArray("abc"));
		CHECK(store.Versions(Key()).versions.size()
			  == (kind == ArchivePurgeKind::Trashbin ? 0 : 1));
		CHECK(heads.versions.size()
			  == (kind == ArchivePurgeKind::Trashbin ? 2 : 1));
		DrainGarbage(store);
		CHECK(store.Remove());
	}
}

void CrashAndRefusal() {
	Section
		= "restart every bounded phase/cursor cut, commit failure and "
		  "authenticated cursor mismatch";
	for (auto cut = 0; cut != 9; ++cut) {
		auto temporary = QTemporaryDir();
		auto store = DesktopArchiveStore();
		CHECK(store.Open(Options(temporary.path()))
			  == DesktopArchiveStatus::Ready);
		Capture(store, Key(), u"one"_q);
		const auto preview
			= store.PreparePurge(ArchivePurgeKind::Trashbin, Scope());
		CHECK(preview.confirmation);
		if (!preview.confirmation)
			continue;
		CHECK(store.BeginPurge(*preview.confirmation));
		for (auto step = 0; step != cut && store.MaintenancePending();
			++step) {
			CHECK(store.ContinuePurge().valid);
		}
		const auto pending = store.MaintenancePending();
		CHECK(store.Close());
		CHECK(store.Open(Options(temporary.path()))
			  == (pending ? DesktopArchiveStatus::Maintenance
						  : DesktopArchiveStatus::Ready));
		if (pending)
			CHECK(!store.Current());
		DrainMaintenance(store);
		CHECK(store.Versions(Key()).versions.empty());
		CHECK(store.Remove());
	}
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path()))
		  == DesktopArchiveStatus::Ready);
	Capture(store, Key(), u"one"_q);
	const auto preview
		= store.PreparePurge(ArchivePurgeKind::Trashbin, Scope());
	CHECK(preview.confirmation);
	if (!preview.confirmation)
		return;
	const auto before = InspectDesktopArchiveMaintenance(
		store.database(), store.epoch(), store.security());
	Sql(store,
		u"UPDATE versions SET envelope=?"_q,
		{QByteArray(65537, 'x')});
	CHECK(!store.BeginPurge(*preview.confirmation) && store.Current());
	const auto after = InspectDesktopArchiveMaintenance(
		store.database(), store.epoch(), store.security());
	CHECK(after.valid && !after.purge && after.header == before.header);
	CHECK(store.Remove());

	auto next = DesktopArchiveStore();
	CHECK(
		next.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
	Capture(next, Key(), u"next"_q);
	const auto confirmation
		= next.PreparePurge(ArchivePurgeKind::Trashbin, Scope())
			  .confirmation;
	CHECK(confirmation);
	if (!confirmation)
		return;
	CHECK(next.BeginPurge(*confirmation));
	next.database().FailForTest(u"commit"_q, 1);
	CHECK(!next.ContinuePurge().valid && next.MaintenanceHeld());
	CHECK(next.RetryMaintenance());
	Sql(next, u"UPDATE maintenance SET cursor='tampered'"_q);
	CHECK(!next.ContinuePurge().valid && next.MaintenanceHeld());
	Sql(next, u"UPDATE maintenance SET cursor=''"_q);
	CHECK(next.RetryMaintenance());
	DrainMaintenance(next);
	CHECK(next.Remove());
}

void LeaseAndGarbage() {
	Section
		= "reader pins survive Begin, bounded owned-only GC and interrupted "
		  "deletion retry";
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path()))
		  == DesktopArchiveStatus::Ready);
	Capture(store, Key(), u"owned"_q, true, true);
	const auto hash = Own(store, Key(), u"owned"_q);
	const auto history = store.Versions(Key());
	CHECK(history.versions.size() == 1);
	if (history.versions.empty())
		return;
	const auto &version = history.versions[0].version;
	const auto handle
		= store.OpenMedia({{store.epoch(),
							   Key(),
							   version.id,
							   version.revision,
							   ArchiveContentHash(version.content),
							   u"document:555"_q,
							   history.generation},
			hash});
	CHECK(handle);
	if (!handle)
		return;
	const auto preview
		= store.PreparePurge(ArchivePurgeKind::Trashbin, Scope());
	CHECK(preview.confirmation);
	if (!preview.confirmation)
		return;
	const auto token = ArchiveSqlToken(Key().account,
		ArchiveIndexKind::Blob,
		hash.toUtf8(),
		store.security());
	const auto path = store.directory() + u"/blobs/"_q + token;
	for (auto i = 0; i != 150; ++i) {
		CHECK(DesktopArchiveWrite(
			path + '/' + QString::number(i), QByteArray("x")));
	}
	CHECK(store.BeginPurge(*preview.confirmation));
	CHECK(!handle->active->load());
	const auto leases
		= store.database().Read({u"SELECT COUNT(*) FROM leases"_q,
			{},
			ArchiveSqlExpectation::OneRow});
	CHECK(leases.ok && std::get<int64>(leases.rows[0][0]) == 1);
	DrainMaintenance(store);
	CHECK(!store.MediaCurrent(*handle));
	const auto outside = temporary.path() + u"/ordinary-cache-file"_q;
	CHECK(DesktopArchiveWrite(outside, QByteArray("keep")));
	CHECK(QDir().rename(path, path + u"-saved"_q));
	CHECK(DesktopArchiveWrite(path, QByteArray("blocked directory")));
	auto more = true;
	CHECK(!store.ContinueGarbage(more) && store.MaintenanceHeld());
	CHECK(!store.MediaPublicationAllowed(hash));
	CHECK(DesktopArchiveRead(outside, 10)
		  == std::optional(QByteArray("keep")));
	CHECK(DesktopArchiveRemoveFile(path));
	CHECK(QDir().rename(path + u"-saved"_q, path));
	CHECK(store.RetryMaintenance());
	CHECK(store.ContinueGarbage(more) && more && QFileInfo::exists(path));
	DrainGarbage(store);
	CHECK(!QFileInfo::exists(path));
	CHECK(DesktopArchiveRead(outside, 10)
		  == std::optional(QByteArray("keep")));
	CHECK(store.Remove());
}

void ExpectedMediaRetirement() {
	Section = "production media releases before/after Finish are expected; genuine IO remains a gap";
	using namespace Purple::MediaRetirementProbe;
	for (const auto mode : {u"before"_q, u"after"_q, u"io-failure"_q}) {
		auto temporary = QTemporaryDir();
		auto store = DesktopArchiveStore();
		CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
		Capture(store, Key(), u"retirement"_q, true, true);
		const auto hash = Own(store, Key(), u"retirement"_q);
		const auto page = store.Versions(Key());
		CHECK(!page.versions.empty());
		if (page.versions.empty()) continue;
		const auto &version = page.versions.front().version;
		auto proofs = std::vector<ArchiveLeaseRevisionChange>();
		auto request = ArchiveMediaRequest{{page.epoch, Key(), version.id, version.revision,
			ArchiveContentHash(version.content), u"document:555"_q, page.generation}, hash};
		request.leaseCommitted = [&](const ArchiveLeaseRevisionChange &proof) { proofs.push_back(proof); };
		Published = {std::make_shared<Worker>(), std::make_shared<Journal>()};
		Published.worker->store = &store;
		auto session = RetirementSession();
		auto access = std::shared_ptr<ArchiveMediaAccess>();
		OpenArchiveMedia(&session, request, [] { return true; }, [&](auto value) { access = std::move(value); });
		Flush();
		CHECK(access && access->current());
		CHECK(proofs.size() == 1 && proofs.front().leaseOnly);
		if (!access) { Published = {}; continue; }
		if (mode == u"io-failure"_q) {
			store.database().FailForTest(u"commit"_q, 1);
			access->close(); Flush();
			const auto gaps = store.database().Read(ArchiveSqlReadCaptureGaps(16));
			CHECK(gaps.ok && !gaps.rows.empty());
			CHECK(store.failure() == u"media-reader-release-held"_q);
		} else {
			const auto preview = store.PreparePurge(ArchivePurgeKind::Trashbin, Scope());
			CHECK(preview.confirmation && store.BeginPurge(*preview.confirmation));
			CHECK(!access->current());
			if (mode == u"before"_q) { access->close(); Flush(); }
			DrainMaintenance(store);
			if (mode == u"after"_q) { access->close(); Flush(); }
			const auto gaps = store.database().Read(ArchiveSqlReadCaptureGaps(16));
			const auto leases = store.database().Read({u"SELECT COUNT(*) FROM leases"_q, {}, ArchiveSqlExpectation::OneRow});
			CHECK(gaps.ok && gaps.rows.empty() && store.failure().isEmpty());
			CHECK(leases.ok && std::get<int64>(leases.rows[0][0]) == 0);
			CHECK(store.Current() && !store.MaintenanceHeld());
		}
		CHECK(proofs.size() == 1);
		CHECK(!store.ReleaseMedia({}));
		access.reset(); Flush(); Published = {};
		CHECK(store.Remove());
	}
}

bool Wait(const std::function<bool()> &ready, std::chrono::seconds timeout = std::chrono::seconds(5)) {
	const auto deadline
		= std::chrono::steady_clock::now() + timeout;
	do {
		PumpMain();
		if (ready())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	} while (std::chrono::steady_clock::now() < deadline);
	return false;
}

bool Post(const std::shared_ptr<ProbePurgeWorker> &worker, ProbePurgeWorker::Task task) {
	for (auto attempt = 0; attempt != 100; ++attempt) {
		if (worker->Post(task)) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return false;
}

thread_local bool WorkerHoldAppend = false;
thread_local int WorkerCurrentCalls = 0;

void HeldWriterWorkerRenewal() {
	Section = "real worker retirement timeout, eventual renewal and nonblocking logout";
	for (const auto logout : {false, true}) {
		auto temporary = QTemporaryDir();
		auto reports = std::vector<QString>();
		auto entered = std::make_shared<std::promise<void>>(); auto enteredFuture = entered->get_future().share();
		auto proceed = std::make_shared<std::promise<void>>(); auto proceedFuture = proceed->get_future().share();
		auto writer = std::make_shared<std::thread>();
		auto result = std::make_shared<std::atomic<bool>>(false);
		auto released = false;
		PurgeValue.worker = std::make_shared<ProbePurgeWorker>(Options(temporary.path()),
			[&](DesktopArchiveStatus, QString reason, uint64_t) { HarnessOnMain([&, reason] { reports.push_back(reason); }); },
			[](std::shared_ptr<DesktopArchiveJournal> journal) { HarnessOnMain([journal] { PurgeValue.journal = journal; }); });
		const auto worker = PurgeValue.worker;
		const auto stop = gsl::finally([&] {
			worker->Stop(false); worker->WaitForStopped(std::chrono::seconds(5));
			if (!released) proceed->set_value();
			if (writer->joinable()) writer->join();
			PumpMain(); PurgeValue = {};
		});
		CHECK(Wait([] { return bool(PurgeValue.journal); }));
		if (!PurgeValue.journal) continue;
		const auto opening = PurgeValue.journal;
		const auto completed = std::make_shared<std::promise<bool>>(); auto outcome = completed->get_future();
		CHECK(Post(worker, [opening, entered, enteredFuture, proceedFuture, writer, result, completed](DesktopArchiveStore &store) {
			auto baseline = Event(Empty(), u"writer-baseline"_q, 1, u"baseline"_q);
			opening->BindCaptureGeneration(baseline);
			const auto seeded = opening->Append(baseline) && opening->Drain(store);
			auto &access = const_cast<DesktopArchiveJournalAccess&>(opening->access());
			const auto current = access.current;
			access.current = [current, entered, proceedFuture] {
				const auto valid = current();
				if (WorkerHoldAppend && ++WorkerCurrentCalls == 2) {
					entered->set_value(); static_cast<void>(proceedFuture.wait_for(std::chrono::seconds(15)));
				}
				return valid;
			};
			auto edit = Event(Empty(), u"held-old-edit"_q, 2, u"old"_q, ArchiveOrigin::AuthoritativeEdit);
			opening->BindCaptureGeneration(edit);
			*writer = std::thread([opening, result, edit] { WorkerHoldAppend = true; WorkerCurrentCalls = 0; result->store(opening->Append(edit)); });
			if (enteredFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
				completed->set_value(false);
				return;
			}
			const auto program = ArchiveSqlProgram{ArchiveSqlStatus::Ready, {{u"BEGIN IMMEDIATE"_q},
				{u"UPDATE archive_meta SET revision=revision WHERE singleton=1"_q, {}, ArchiveSqlExpectation::OneChange}, {u"COMMIT"_q}}};
			store.database().FailForTest(u"prepare"_q, 3);
			completed->set_value(seeded && store.Execute(program));
		}));
		CHECK(enteredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
		CHECK(Wait([&] { return outcome.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }));
		if (outcome.wait_for(std::chrono::seconds(0)) == std::future_status::ready) CHECK(outcome.get());
		CHECK(Wait([&] { return std::find(reports.begin(), reports.end(), u"capture-admission-writer-retirement-held"_q) != reports.end(); }));
		CHECK(opening->WriterRetired() && PurgeValue.journal == opening);
		if (logout) {
			const auto started = std::chrono::steady_clock::now(); CHECK(worker->Stop(true));
			CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(100));
			CHECK(worker->WaitForStopped(std::chrono::seconds(5)) && worker->Removed());
			PumpMain(); CHECK(PurgeValue.journal == opening && !opening->access().active());
			proceed->set_value(); released = true; writer->join();
		} else {
			proceed->set_value(); released = true; writer->join(); CHECK(result->load());
			CHECK(Wait([&] { return PurgeValue.journal && PurgeValue.journal != opening; }, std::chrono::seconds(12)));
			CHECK(PurgeValue.journal->access().active() && !opening->access().active());
			CHECK(Wait([&] { return std::find(reports.begin(), reports.end(), u"capture-admission-renewed"_q) != reports.end(); }));
		}
	}
}

void HeaderIoWorkerRenewal() {
	Section = "real worker replaces transiently revoked admission and reports it";
	for (const auto stopDuringWrite : {false, true}) {
		auto temporary = QTemporaryDir();
		auto reports = std::vector<QString>();
		PurgeValue.worker = std::make_shared<ProbePurgeWorker>(Options(temporary.path()),
			[&](DesktopArchiveStatus, QString reason, uint64_t) {
				HarnessOnMain([&, reason] { reports.push_back(reason); });
			}, [](std::shared_ptr<DesktopArchiveJournal> journal) {
				HarnessOnMain([journal] { PurgeValue.journal = journal; });
			});
		const auto worker = PurgeValue.worker;
		const auto stop = gsl::finally([&] {
			worker->Stop(false);
			worker->WaitForStopped(std::chrono::seconds(5));
			PumpMain(); PurgeValue = {};
		});
		CHECK(Wait([] { return bool(PurgeValue.journal); }));
		if (!PurgeValue.journal) continue;
		const auto opening = PurgeValue.journal;
		const auto completed = std::make_shared<std::promise<bool>>();
		auto outcome = completed->get_future();
		CHECK(Post(worker, [worker, completed, opening, stopDuringWrite](DesktopArchiveStore &store) {
			const auto program = ArchiveSqlProgram{ArchiveSqlStatus::Ready, {
				{u"BEGIN IMMEDIATE"_q},
				{u"UPDATE archive_meta SET revision=revision WHERE singleton=1"_q, {}, ArchiveSqlExpectation::OneChange},
				{u"COMMIT"_q}}};
			store.database().FailForTest(u"prepare"_q, 3);
			const auto executed = store.Execute(program);
			completed->set_value(executed && !opening->access().active()
				&& store.failure() == u"capture-admission-header-unreadable"_q);
			if (stopDuringWrite) worker->Stop(true);
		}));
		CHECK(Wait([&] { return outcome.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }));
		if (outcome.wait_for(std::chrono::seconds(0)) == std::future_status::ready) CHECK(outcome.get());
		if (stopDuringWrite) {
			CHECK(worker->WaitForStopped(std::chrono::seconds(5)) && worker->Removed());
			PumpMain();
			CHECK(PurgeValue.journal == opening && !opening->access().active());
		} else {
			CHECK(Wait([&] { return PurgeValue.journal && PurgeValue.journal != opening; }));
			CHECK(!opening->access().active() && !opening->CaptureAllowed());
			CHECK(PurgeValue.journal->access().active() && PurgeValue.journal->CaptureAllowed());
			CHECK(Wait([&] { return std::find(reports.begin(), reports.end(), u"capture-admission-renewed"_q) != reports.end(); }));
		}
	}
}

void ProductionExecutionBoundary() {
	Section
		= "literal Execute callback with real worker: lost authority "
		  "releases its claim; obsolete delivery leaves newer work alone";
	auto temporary = QTemporaryDir();
	auto session = ProbePurgeSession();
	PurgeValue.worker = std::make_shared<ProbePurgeWorker>(
		Options(temporary.path()),
		[](DesktopArchiveStatus status, QString reason, uint64_t) {
			if (!reason.isEmpty()) {
				std::fprintf(stderr, "worker status=%d reason=%s\n", int(status), reason.toUtf8().constData());
			}
		},
		[](std::shared_ptr<DesktopArchiveJournal> journal) {
			HarnessOnMain([journal] { PurgeValue.journal = journal; });
		});
	const auto worker = PurgeValue.worker;
	const auto stop = gsl::finally([&] {
		worker->Stop(false);
		worker->WaitForStopped(std::chrono::seconds(5));
		PurgeValue = {};
		PumpMain();
	});
	CHECK(Wait([] { return bool(PurgeValue.journal); }));
	if (!PurgeValue.journal)
		return;
	const auto original = PurgeValue.journal;
	const auto nativeHandles = std::make_shared<std::vector<DesktopArchiveMediaHandle>>();
	const auto seeded
		= std::make_shared<std::promise<DesktopArchivePurgePreview>>();
	auto seed = seeded->get_future();
	CHECK(Post(worker, [seeded, nativeHandles, journal = original](DesktopArchiveStore &store) {
		auto event = Event(Empty(), u"worker-body"_q, 1);
		event.content->media.push_back({u"document:555"_q, u"document"_q});
		auto deleted = Event(Empty(),
			u"worker-delete"_q,
			2,
			{},
			ArchiveOrigin::AuthoritativeDelete);
		journal->BindCaptureGeneration(event);
		journal->BindCaptureGeneration(deleted);
		const auto captured = journal->Append(event) && journal->Append(deleted) && journal->Drain(store);
		CHECK(captured && journal->failure().isEmpty());
		const auto hash = Own(store, Key(), u"worker-body"_q);
		const auto history = store.Versions(Key());
		if (!history.versions.empty()) {
			const auto &version = history.versions.front().version;
			const auto request = ArchiveMediaRequest{{history.epoch, Key(), version.id, version.revision,
				ArchiveContentHash(version.content), u"document:555"_q, history.generation}, hash};
			for (auto i = 0; i != 2; ++i) {
				const auto opened = store.OpenMedia(request);
				CHECK(opened);
				if (opened) nativeHandles->push_back(*opened);
			}
		}
		seeded->set_value(
			captured
				? store.PreparePurge(ArchivePurgeKind::Trashbin, Scope())
				: DesktopArchivePurgePreview());
	}));
	CHECK(Wait([&] {
		return seed.wait_for(std::chrono::seconds(0))
			   == std::future_status::ready;
	}));
	if (seed.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return;
	const auto preview = seed.get();
	CHECK(preview.confirmation);
	if (!preview.confirmation)
		return;
	const auto blocked = std::make_shared<std::promise<void>>();
	auto blockedReady = blocked->get_future();
	const auto released = std::make_shared<std::promise<void>>();
	const auto release = released->get_future().share();
	CHECK(Post(worker, [blocked, release](DesktopArchiveStore &) {
		blocked->set_value();
		static_cast<void>(release.wait_for(std::chrono::seconds(5)));
	}));
	CHECK(Wait([&] {
		return blockedReady.wait_for(std::chrono::seconds(0))
			   == std::future_status::ready;
	}));
	const auto permitted = std::make_shared<std::atomic<bool>>(true);
	auto callbacks = 0;
	const auto start = std::chrono::steady_clock::now();
	ExecuteProbePurge(&session,
		*preview.confirmation,
		permitted,
		[&](DesktopArchiveStatus) { ++callbacks; });
	CHECK(std::chrono::steady_clock::now() - start
		  < std::chrono::milliseconds(50));
	CHECK(!original->CaptureAllowed());
	auto nativePeer = ProbeMigrationPeer();
	auto nativeSession = ProbeMigrationSession();
	auto scopeLifetime = rpl::lifetime();
	const auto openingScope = ViewScope();
	auto liveScope = openingScope;
	auto scopeClosed = false;
	BindArchivePurgeScopeChanges(&nativeSession,
		[&] { return SameArchiveViewScope(openingScope, liveScope); }, permitted,
		[&] { scopeClosed = true; }, scopeLifetime);
	nativeSession.changes().native.updated(&nativePeer, Data::PeerUpdate::Flag::Name);
	CHECK(permitted->load());
	liveScope.messages.migrationAliases.push_back({ArchiveChatKind::Group, 99});
	nativeSession.changes().native.updated(&nativePeer, Data::PeerUpdate::Flag::Migration);
	CHECK(!permitted->load() && scopeClosed);
	liveScope = openingScope;
	nativeSession.changes().native.updated(&nativePeer, Data::PeerUpdate::Flag::Migration);
	CHECK(!permitted->load());
	released->set_value();
	CHECK(Wait([&] { return original->CaptureAllowed(); }));
	CHECK(callbacks == 0);
	CHECK(worker->ClaimPurge());
	worker->ReleasePurgeClaim();
	const auto next = std::make_shared<std::atomic<bool>>(true);
	auto status = DesktopArchiveStatus::Disabled;
	for (auto attempt = 0; attempt != 100; ++attempt) {
		status = DesktopArchiveStatus::Disabled;
		ExecuteProbePurge(&session,
			*preview.confirmation,
			next,
			[&](DesktopArchiveStatus result) { status = result; });
		if (status != DesktopArchiveStatus::Busy) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	CHECK(Wait([&] {
		return PurgeValue.journal && PurgeValue.journal != original;
	}));
	CHECK(status == DesktopArchiveStatus::Maintenance);
	CHECK(
		!original->CaptureAllowed() && PurgeValue.journal->CaptureAllowed());
	CHECK(PurgeValue.journal->CaptureGeneration(Key().chat).chat == 1);
	CHECK(nativeHandles->size() == 2);
	for (const auto &handle : *nativeHandles) CHECK(!handle.active->load());
	const auto freshResult = std::make_shared<std::promise<bool>>();
	auto freshReady = freshResult->get_future();
	CHECK(Post(worker, [freshResult, journal = PurgeValue.journal, nativeHandles](DesktopArchiveStore &store) {
		if (!nativeHandles->empty()) {
			auto forged = nativeHandles->front(); ++forged.source.key.messageId;
			CHECK(!store.ReleaseMedia(forged));
			forged = nativeHandles->front(); forged.active = std::make_shared<std::atomic<bool>>(false);
			CHECK(!store.ReleaseMedia(forged));
			auto retiredCommit = ArchiveLeaseRevisionChange{1, 2, true};
			CHECK(store.ReleaseMedia(nativeHandles->front(), &retiredCommit));
			CHECK(!retiredCommit.leaseOnly);
		}
		const auto leases = store.database().Read({u"SELECT COUNT(*) FROM leases"_q, {}, ArchiveSqlExpectation::OneRow});
		auto fresh = Event(Empty(), u"after-two-handle-finish"_q, 1, u"fresh"_q,
			ArchiveOrigin::Baseline, Key({0, 123}, 45, 99999));
		journal->BindCaptureGeneration(fresh);
		freshResult->set_value(store.Current() && !store.MaintenanceHeld() && leases.ok
			&& std::get<int64>(leases.rows[0][0]) == 0 && journal->CaptureAllowed()
			&& journal->Append(fresh) && journal->Drain(store)
			&& store.Versions(fresh.key).versions.size() == 1);
	}));
	CHECK(Wait([&] { return freshReady.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }));
	if (freshReady.wait_for(std::chrono::seconds(0)) == std::future_status::ready) CHECK(freshReady.get());
	PumpMain();
	CHECK(callbacks == 0);
	CHECK(worker->ClaimPurge());
	worker->ReleasePurgeClaim();
	PurgeRetired.insert(&session);
	ExecuteProbePurge(&session,
		*preview.confirmation,
		next,
		[&](DesktopArchiveStatus result) { status = result; });
	CHECK(status == DesktopArchiveStatus::Stale
		  && PurgeValue.journal->CaptureAllowed());
	PurgeRetired.erase(&session);
	const auto stopped = std::chrono::steady_clock::now();
	CHECK(worker->Stop(true));
	CHECK(std::chrono::steady_clock::now() - stopped
		  < std::chrono::milliseconds(100));
	CHECK(worker->WaitForStopped(std::chrono::seconds(5))
		  && worker->Removed());
}

}

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	PurgeRouteProbe::Run();
	StaleConfirmations();
	ScopeAndFreshCapture();
	NamedAccountScope();
	PurgeCommitBoundary();
	ReuseAndHeads();
	CrashAndRefusal();
	LeaseAndGarbage();
	ExpectedMediaRetirement();
	HeldWriterWorkerRenewal();
	HeaderIoWorkerRenewal();
	ProductionExecutionBoundary();
	return Finish();
}
