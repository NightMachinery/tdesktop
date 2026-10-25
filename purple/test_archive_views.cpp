#include "purple/purple_desktop_archive_store.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_archive_journal.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "purple/purple_archive_sql_maintenance.h"
#include <deque>
#include "tests/archive_test_support.h"
#include <QtCore/QBuffer>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QTemporaryDir>
#include <set>
#include <thread>
#include <mutex>
#include <future>
#include <condition_variable>
#include <tuple>

namespace Purple {
#include "archive-scope-match.inc"
#include "archive-view-worker.inc"
}

namespace Purple::ViewDeliveryProbe {

struct Session : base::has_weak_ptr {};

struct Worker {
	DesktopArchiveStore *store = nullptr;
	std::deque<std::function<void(DesktopArchiveStore&)>> tasks;
	std::vector<std::shared_ptr<std::atomic<bool>>> flags;
	bool Post(std::function<void(DesktopArchiveStore&)> task) {
		tasks.push_back(std::move(task));
		return true;
	}
	void RevokeOnStop(std::shared_ptr<std::atomic<bool>> active) {
		flags.push_back(std::move(active));
	}
	void Run() {
		while (!tasks.empty()) {
			auto task = std::move(tasks.front());
			tasks.pop_front();
			task(*store);
		}
	}
};

struct Value {
	std::shared_ptr<Worker> worker;
};

Value Published;
std::set<Session*> Retired;
std::deque<std::function<void()>> Main;
Value &For(not_null<Session*>) { return Published; }

template <typename Callback>
void OnMain(Callback callback) { Main.push_back(std::move(callback)); }

void FlushMain() {
	while (!Main.empty()) {
		auto callback = std::move(Main.front());
		Main.pop_front();
		callback();
	}
}

#include "archive-deleted-delivery.inc"

struct Window {};
int Actions = 0;
void ShowArchiveHistory(Window*, ArchiveMessageKey) { ++Actions; }

std::function<void()> ShownHistoryAction(std::shared_ptr<std::atomic<bool>> active) {
	const auto window = std::make_shared<Window>();
	const auto key = ArchiveTest::Key();
	const auto current = [] { return true; };
#include "archive-deleted-history-action.inc"
}

}

using namespace Purple;
using namespace ArchiveTest;

namespace {

DesktopArchiveOptions Options(const QString &root) {
	CHECK(DesktopArchiveDirectory(root + u"/profile"_q));
	auto result = DesktopArchiveOptions{root + u"/archives"_q, root + u"/profile"_q,
		{0, 123}, u"0"_q, QByteArray(256, 'k'), true};
	result.loginAuthority = QByteArray(32, 'a');
	result.loginAuthKeyId = result.loginAuthKeyIds = QByteArray(16, '1');
	return result;
}

ArchiveViewScope Scope(ArchiveChatKey chat = Key().chat) {
	return {{{0, 123}, chat, {}}, {}, ArchiveCollection::Received, u"original"_q};
}

namespace ViewRouteProbe {

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
	AccountKey key = {0,123};
	std::optional<AccountKey> source = AccountKey{0,123};
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
template <typename Type> struct Weak {
	std::shared_ptr<Type*> pointer;
	Type *get() const { return *pointer; }
};
struct Guard {
	std::shared_ptr<bool> alive;
	explicit operator bool() const { return *alive; }
};
struct State { bool closed = false; };
AccountKey SessionAccountKey(Session *session) { return session->key; }
std::optional<AccountKey> CachedAppWideSettingsAccount(Session *session) { return session->source; }
int AccountWorkModeForSession(Session *session) { return session->mode; }

void Run() {
	Section = "literal view route: opening source/mode/login/controller and late callback refusal";
	auto live=Session();live.owner.current=&live;
	auto own=Window();own.owner=&live;own.root.current=&own;
	auto backing=State();
	const auto state=&backing;
	const auto guard=Guard{std::make_shared<bool>(true)};
	const auto session=Weak<Session>{std::make_shared<Session*>(&live)};
	const auto window=Weak<Window>{std::make_shared<Window*>(&own)};
	const auto account=live.key;
	const auto source=live.source;
	const auto mode=live.mode;
	const auto opening=Scope();
	auto liveScope=opening;
	const auto scopeCurrent=[&] { return SameArchiveViewScope(opening,liveScope); };
#include "archive-view-route.inc"
	CHECK(current());
	live.source=AccountKey{0,999};CHECK(!current());live.source=source;
	live.mode++;CHECK(!current());live.mode=mode;
	live.key.userId++;CHECK(!current());live.key=account;
	live.owner.leaving=true;CHECK(!current());live.owner.leaving=false;
	live.owner.current=nullptr;CHECK(!current());live.owner.current=&live;
	own.root.current=nullptr;CHECK(!current());own.root.current=&own;
	*session.pointer=nullptr;CHECK(!current());*session.pointer=&live;
	*window.pointer=nullptr;CHECK(!current());*window.pointer=&own;
	backing.closed=true;CHECK(!current());backing.closed=false;
	*guard.alive=false;CHECK(!current());*guard.alive=true;
	liveScope.messages.migrationAliases.push_back({ArchiveChatKind::Group,999});CHECK(!current());
	liveScope=opening;CHECK(current());
}

}

void Capture(DesktopArchiveStore &store, ArchiveMessageKey key, QString id,
		QString text = u"retained"_q, int64 topic = 0, bool deleted = true, bool media = false) {
	auto event = Event(Empty(), id, 1, text, ArchiveOrigin::Baseline, key);
	event.observation.topicId = topic;
	if (media) event.content->media.push_back({u"document:555"_q,u"document"_q});
	event.observation.serverTime = 90;
	event.observation.context.insert(u"sender"_q, QJsonObject{{u"kind"_q, 0}, {u"id"_q, u"55"_q}});
	CHECK(store.Capture(event).retained);
	if (deleted) {
		auto removal = Event(Empty(), id + u"delete"_q, 2, {}, ArchiveOrigin::AuthoritativeDelete, key);
		CHECK(store.Capture(removal).retained);
	}
}

void Sql(DesktopArchiveStore &store, QString sql, std::vector<ArchiveSqlValue> binds = {},
		ArchiveSqlExpectation expected = ArchiveSqlExpectation::OneChange) {
	CHECK(store.database().Read({std::move(sql), std::move(binds), expected}).ok);
}

void CommitSql(DesktopArchiveStore &store, QString sql, std::vector<ArchiveSqlValue> binds) {
	auto program = ArchiveSqlProgram();
	program.status = ArchiveSqlStatus::Ready;
	program.statements = {{u"BEGIN IMMEDIATE"_q},
		{std::move(sql), std::move(binds), ArchiveSqlExpectation::OneChange},
		{u"COMMIT"_q}};
	CHECK(store.Execute(program));
}

void OriginScopeFence() {
	Section = "literal menu scope fence covers account, typed aliases, topic and representation";
	const auto opening = Scope();
	CHECK(SameArchiveViewScope(opening,opening));
	auto changed = opening;changed.messages.account.userId++;
	CHECK(!SameArchiveViewScope(opening,changed));
	changed=opening;changed.messages.account.environment++;
	CHECK(!SameArchiveViewScope(opening,changed));
	changed=opening;changed.messages.chat->kind=ArchiveChatKind::User;
	CHECK(!SameArchiveViewScope(opening,changed));
	changed=opening;changed.messages.migrationAliases.push_back({ArchiveChatKind::User,45});
	CHECK(!SameArchiveViewScope(opening,changed));
	changed=opening;changed.topic=ArchiveTopicScope{*opening.messages.chat,8};
	CHECK(!SameArchiveViewScope(opening,changed));
	const auto topic=changed;changed.topic->chat.kind=ArchiveChatKind::User;
	CHECK(!SameArchiveViewScope(topic,changed));
	changed=topic;changed.topic->id++;
	CHECK(!SameArchiveViewScope(topic,changed));
	changed=opening;changed.collection=ArchiveCollection::QuickReply;
	CHECK(!SameArchiveViewScope(opening,changed));
	changed=opening;changed.representation=u"translated"_q;
	CHECK(!SameArchiveViewScope(opening,changed));
}

void Paging() {
	Section = "real readonly snapshot paging, full-width tuple and concurrent writer";
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
	for (auto i = 0; i != 137; ++i) {
		Capture(store, Key({0,123}, 45, int64(2147483648LL + i)), u"v"_q + QString::number(i));
	}
	Capture(store, Key({0,123}, 45, 9000000000000000000LL), u"wide-positive"_q);
	auto page = store.OpenDeletedView(Scope(), ArchiveTranscriptOrder::Chronological, 128);
	CHECK(page.status == ArchiveQueryStatus::Ready && page.entries.size() == 128 && page.more);
	CHECK(page.counts[0] == 138 && page.counts[1] == 138 && page.counts[2] == 138);
	const auto view = page.view;
	const auto epoch = page.epoch;
	const auto active = page.active;
	std::set<int64> ids;
	std::optional<ArchiveTranscriptCursor> previous;
	const auto consume = [&](const ArchiveDeletedPage &part) {
		for (const auto &entry : part.entries) {
			CHECK(entry.head && entry.head->key == entry.message.key);
			CHECK(entry.message.deleted && entry.generation.account == 0);
			CHECK(ids.insert(entry.message.key.messageId).second);
			if (previous) {
				CHECK(std::tie(previous->date, previous->messageId, previous->messageKey)
					< std::tie(entry.cursor.date, entry.cursor.messageId, entry.cursor.messageKey));
			}
			previous = entry.cursor;
		}
	};
	consume(page);
	Capture(store, Key({0,123}, 45, 999999999999LL), u"concurrent"_q);
	page = store.ReadDeletedView(view, epoch, 128);
	CHECK(page.status == ArchiveQueryStatus::Ready && page.entries.size() == 10 && !page.more);
	CHECK(page.refreshAvailable && page.counts[0] == 138);
	consume(page);
	CHECK(ids.size() == 138 && !ids.contains(999999999999LL));
	CHECK(store.CloseDeletedView(view, epoch));
	CHECK(active && !active->load());
	CHECK(store.ReadDeletedView(view, epoch).status == ArchiveQueryStatus::Stale);
	auto reverse = store.OpenDeletedView(Scope(), ArchiveTranscriptOrder::ReverseChronological, 1);
	CHECK(reverse.status == ArchiveQueryStatus::Ready && reverse.entries.size() == 1);
	if (reverse.entries.empty()) return;
	CHECK(reverse.entries.front().message.key.messageId == 9000000000000000000LL);
	previous = reverse.entries.front().cursor;
	for (auto i = 0; reverse.more && i != 140; ++i) {
		reverse = store.ReadDeletedView(reverse.view, reverse.epoch, 1);
		CHECK(reverse.status == ArchiveQueryStatus::Ready);
		if (reverse.entries.empty()) { CHECK(!reverse.more); break; }
		CHECK(reverse.entries.size() == 1);
		const auto cursor = reverse.entries.front().cursor;
		CHECK(std::tie(cursor.date, cursor.messageId, cursor.messageKey)
			< std::tie(previous->date, previous->messageId, previous->messageKey));
		previous = cursor;
	}
	const auto secretKey = Key({0,123},99,-9000000000000000000LL,ArchiveChatKind::Secret);
	Capture(store,secretKey,u"signed-synthetic-core-key"_q);
	const auto signedPage = store.OpenDeletedView(Scope(secretKey.chat));
	CHECK(signedPage.status == ArchiveQueryStatus::Ready && signedPage.entries.size() == 1);
	CHECK(signedPage.entries.front().message.key == secretKey);
	CHECK(store.Close());
	CHECK(reverse.active && !reverse.active->load());
}

void ScopesAndTombstones() {
	Section = "typed parents, trusted aliases, actual-parent topics, namespaces and bodyless tombstones";
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
	Capture(store, Key(), u"main"_q, u"parent topic"_q, 8);
	const auto aliasKey = Key({0,123}, 55, 67, ArchiveChatKind::Group);
	Capture(store, aliasKey, u"alias"_q, u"alias same topic number"_q, 8);
	Capture(store, Key({0,123}, 45, 67, ArchiveChatKind::User), u"same-number-other-type"_q, {}, 8);
	auto scheduled = Key(); scheduled.collection = ArchiveCollection::Scheduled;
	Capture(store, scheduled, u"scheduled"_q);
	auto quick = Key(); quick.collection = ArchiveCollection::QuickReply; quick.collectionId = 1;
	Capture(store, quick, u"quick"_q);
	auto alternate = Key(); alternate.representation = u"translated"_q;
	Capture(store, alternate, u"alternate"_q);
	Capture(store, Key({0,123},45,68), u"live"_q, {}, 0, false);
	auto deletion = Event(Empty(), u"bodyless"_q, 1, {}, ArchiveOrigin::AuthoritativeDelete, Key({0,123},45,69));
	CHECK(store.Capture(deletion).retained);
	auto scope = Scope(); scope.messages.migrationAliases.push_back(aliasKey.chat);
	auto page = store.OpenDeletedView(scope);
	CHECK(page.status == ArchiveQueryStatus::Ready && page.entries.size() == 3);
	CHECK(page.counts[0] == 4 && page.counts[1] == 3 && page.counts[2] == 3);
	CHECK(std::count_if(page.entries.begin(), page.entries.end(), [](const auto &entry) { return !entry.head; }) == 1);
	CHECK(page.logicalComplete && page.logicalBytes == 0);
	scope.topic = ArchiveTopicScope{Key().chat, 8};
	auto topic = store.OpenDeletedView(scope);
	CHECK(topic.status == ArchiveQueryStatus::Ready && topic.entries.size() == 1);
	if (!topic.entries.empty()) CHECK(topic.entries.front().message.key == Key());
	scope.topic = ArchiveTopicScope{aliasKey.chat, 8};
	auto alias = store.OpenDeletedView(scope);
	CHECK(alias.status == ArchiveQueryStatus::Ready && alias.entries.size() == 1);
	if (!alias.entries.empty()) CHECK(alias.entries.front().message.key == aliasKey);
	scope.topic = ArchiveTopicScope{{ArchiveChatKind::Channel, 999}, 8};
	CHECK(store.OpenDeletedView(scope).status != ArchiveQueryStatus::Ready);
	scope=Scope();scope.collection=ArchiveCollection::QuickReply;
	const auto quickPage=store.OpenDeletedView(scope);
	CHECK(quickPage.status == ArchiveQueryStatus::Ready && quickPage.entries.size() == 1);
	if (!quickPage.entries.empty()) CHECK(quickPage.entries.front().message.key == quick);
	scope=Scope();scope.representation=u"translated"_q;
	const auto translated=store.OpenDeletedView(scope);
	CHECK(translated.status == ArchiveQueryStatus::Ready && translated.entries.size() == 1);
	if (!translated.entries.empty()) CHECK(translated.entries.front().message.key == alternate);
	scope=Scope({ArchiveChatKind::User,45});
	const auto typed=store.OpenDeletedView(scope);
	CHECK(typed.status == ArchiveQueryStatus::Ready && typed.entries.size() == 1);
	scope = Scope(); scope.messages.account.userId++;
	CHECK(store.OpenDeletedView(scope).status != ArchiveQueryStatus::Ready);
	auto wrong = page.epoch; wrong.generation++;
	CHECK(store.ReadDeletedView(page.view, wrong).status == ArchiveQueryStatus::Stale);
	CHECK(page.active->load());
	CHECK(store.CloseDeletedView(page.view, page.epoch));
	CHECK(!page.active->load() && topic.active->load());
	CHECK(store.ExpireDeletedViews(true));
	CHECK(!topic.active->load() && !alias.active->load());
	CHECK(store.Remove());
}

void SnapshotBodies() {
	Section = "body chunks come from the same snapshot, never the writer";
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
	Capture(store, Key({0,123},45,1), u"small"_q);
	const auto body = QString(30000, QChar('x'));
	Capture(store, Key({0,123},45,2), u"large"_q, body);
	auto page = store.OpenDeletedView(Scope(), ArchiveTranscriptOrder::Chronological, 1);
	CHECK(page.status == ArchiveQueryStatus::Ready && page.entries.size() == 1 && page.more);
	const auto before = store.database().Read({
		u"SELECT envelope FROM body_chunks ORDER BY message_key,version_id,chunk_index"_q,
		{}, ArchiveSqlExpectation::Rows});
	CHECK(before.ok && !before.rows.empty());
	Sql(store, u"UPDATE body_chunks SET envelope=x'00'"_q, {}, ArchiveSqlExpectation::AnyChanges);
	const auto after = store.database().Read({
		u"SELECT envelope FROM body_chunks ORDER BY message_key,version_id,chunk_index"_q,
		{}, ArchiveSqlExpectation::Rows});
	CHECK(after.ok && after.rows.size() == before.rows.size() && after.rows != before.rows);
	CHECK(std::all_of(after.rows.begin(), after.rows.end(), [](const auto &row) {
		return row.size() == 1 && std::get_if<QByteArray>(&row[0])
			&& std::get<QByteArray>(row[0]) == QByteArray(1, '\0');
	}));
	page = store.ReadDeletedView(page.view, page.epoch, 1);
	CHECK(page.status == ArchiveQueryStatus::Ready && page.entries.size() == 1);
	if (page.entries.empty()) return;
	CHECK(page.entries.front().head && page.entries.front().head->version.content.text == body);
	CHECK(page.entries.front().displayContent.text.size() == 8193);
	CHECK(page.entries.front().contentHash == ArchiveContentHash(page.entries.front().head->version.content));
	CHECK(store.CloseDeletedView(page.view, page.epoch));
	const auto fresh = store.OpenDeletedView(Scope());
	CHECK(fresh.status == ArchiveQueryStatus::Unavailable && fresh.entries.empty());
	CHECK(!fresh.active || !fresh.active->load());
	CHECK(store.Remove());
}

void WrapperMetadata() {
	Section = "payload metadata is retained; unsupported outer row metadata refuses";
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
	Capture(store, Key(), u"wrapper"_q);
	const auto &security = store.security();
	const auto token = ArchiveSqlMessageId(Key(), security);
	const auto rows = store.database().Read(ArchiveSqlReadMessage(Key(), security));
	CHECK(rows.ok && rows.rows.size() == 1);
	if (!rows.ok || rows.rows.empty()) return;
	const auto shell = DesktopArchiveDecodeRow(store.database(), Key().account, security,
		ArchiveRecordKind::Message, token, std::get<QByteArray>(rows.rows[0][2]));
	CHECK(shell);
	const auto version = store.Versions(Key());
	CHECK(version.versions.size() == 1);
	if (!shell || version.versions.empty()) return;
	const auto extra = QJsonObject{{u"future-field"_q, QJsonArray{u"preserved"_q, 9}}};
	auto shellRow = shell->row;
	std::get<ArchiveMessage>(shellRow.value).preserved = extra;
	auto versionRow = ArchiveRow{version.versions.front()};
	std::get<ArchiveVersionSnapshot>(versionRow.value).version.preserved = extra;
	const auto rewrite = [&](ArchiveRow row, ArchiveRecordKind kind,
			const QString &id, QString sql, bool unsupported = false) {
		auto outer = row;
		outer.preserved = extra;
		CHECK(!EncodeArchiveRow(outer));
		const auto bytes = EncodeArchiveRow(row);
		CHECK(bytes);
		if (!bytes) return;
		auto encoded = *bytes;
		if (unsupported) {
			auto object = QJsonDocument::fromJson(encoded).object();
			object.insert(u"unsupported-outer-field"_q, extra);
			encoded = QJsonDocument(object).toJson(QJsonDocument::Compact);
			CHECK(DecodeArchiveRow(encoded).status == ArchiveCodecStatus::Unsupported);
		}
		const auto frame = SealArchiveEnvelope({Key().account, security.storeId,
			kind, id.toUtf8()}, security.keyId, encoded, security.crypto);
		CHECK(frame);
		if (!frame) return;
		Sql(store, sql, {frame.bytes});
	};
	const auto shellSql = u"UPDATE messages SET envelope=?"_q;
	const auto versionSql = u"UPDATE versions SET envelope=?"_q;
	const auto child = ArchiveSqlChildId(Key(), ArchiveIndexKind::Version, u"wrapper"_q, security);
	rewrite(shellRow, ArchiveRecordKind::Message, token, shellSql);
	rewrite(versionRow, ArchiveRecordKind::Version, child, versionSql);
	const auto page = store.OpenDeletedView(Scope());
	CHECK(page.status == ArchiveQueryStatus::Ready && page.entries.size() == 1);
	if (!page.entries.empty()) {
		CHECK(page.entries[0].message.preserved == extra);
		CHECK(page.entries[0].head && page.entries[0].head->version.preserved == extra);
		CHECK(page.entries[0].shellMetadata.isEmpty() && page.entries[0].headMetadata.isEmpty());
	}
	CHECK(store.CloseDeletedView(page.view, page.epoch));
	for (const auto kind : {ArchiveRecordKind::Message, ArchiveRecordKind::Version}) {
		const auto &row = kind == ArchiveRecordKind::Message ? shellRow : versionRow;
		const auto &id = kind == ArchiveRecordKind::Message ? token : child;
		const auto &sql = kind == ArchiveRecordKind::Message ? shellSql : versionSql;
		rewrite(row, kind, id, sql, true);
		const auto refused = store.OpenDeletedView(Scope());
		CHECK(refused.status == ArchiveQueryStatus::Unavailable && refused.entries.empty());
		CHECK(!refused.active || !refused.active->load());
		rewrite(row, kind, id, sql);
	}
	CHECK(store.Remove());
}

void InitialAdmissionStatus() {
	Section = "initial future/schema refusal retains its specific view status";
	{
		auto temporary = QTemporaryDir();
		auto first = DesktopArchiveStore();
		auto second = DesktopArchiveStore();
		CHECK(first.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
		CHECK(second.Open(Options(temporary.path())) == DesktopArchiveStatus::Busy);
		CHECK(second.OpenDeletedView(Scope()).status == ArchiveQueryStatus::Busy);
		CHECK(second.Close());
		CHECK(first.Close());
	}
	for (const auto future : {false, true}) {
		auto temporary = QTemporaryDir();
		auto store = DesktopArchiveStore();
		CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
		Capture(store, Key(), u"admission"_q);
		Sql(store, future ? u"UPDATE archive_meta SET schema_version=3"_q
			: u"UPDATE archive_meta SET schema_fingerprint='changed'"_q);
		CHECK(store.Close());
		CHECK(store.Open(Options(temporary.path())) == (future
			? DesktopArchiveStatus::ReadOnly : DesktopArchiveStatus::SchemaMismatch));
		const auto page = store.OpenDeletedView(Scope());
		CHECK(page.status == (future ? ArchiveQueryStatus::ReadOnly : ArchiveQueryStatus::SchemaMismatch));
		CHECK(page.entries.empty() && page.view.isEmpty() && !page.active);
		CHECK(store.Close());
	}
}

void Corruption() {
	Section = "MAC, expected identities, head absence, counts/admission refusal and fail-closed cleanup";
	for (const auto &sql : {
		u"UPDATE messages SET envelope=x'00'"_q,
		u"UPDATE versions SET envelope=x'00'"_q,
		u"DELETE FROM versions"_q,
		u"UPDATE messages SET message_id=999"_q,
		u"UPDATE messages SET transcript_date=-1"_q,
		u"UPDATE messages SET chat_token='wrong'"_q,
		u"DELETE FROM archive_meta"_q,
		u"UPDATE archive_meta SET schema_version=3"_q,
		u"UPDATE archive_meta SET schema_fingerprint='changed'"_q}) {
		auto temporary = QTemporaryDir();
		auto store = DesktopArchiveStore();
		CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
		Capture(store, Key(), u"body"_q);
		if (sql.contains(u"chat_token"_q)) {
			Sql(store, sql);
			const auto page = store.OpenDeletedView(Scope());
			CHECK(page.status == ArchiveQueryStatus::Ready && page.entries.empty());
		} else {
			Sql(store, sql);
			const auto page = store.OpenDeletedView(Scope());
			CHECK(page.status != ArchiveQueryStatus::Ready && page.entries.empty());
		}
		CHECK(store.Remove());
	}
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
	Capture(store, Key({0,123},46,67), u"other-scope"_q);
	const auto token = ArchiveSqlChatId(Key().chat, Key().account, store.security());
	Sql(store, u"UPDATE messages SET chat_token=?"_q, {token});
	CHECK(store.OpenDeletedView(Scope()).status == ArchiveQueryStatus::Unavailable);
	CHECK(store.Remove());
}

void BlobSize(DesktopArchiveStore &store, const QString &hash, uint64_t size, QString indexed = {}) {
	const auto &security = store.security();
	const auto token = ArchiveSqlToken({0,123}, ArchiveIndexKind::Blob, hash.toUtf8(), security);
	const auto bytes = EncodeArchiveRow({ArchiveBlob{hash, size, ArchiveBlobState::Complete}});
	CHECK(bytes);
	if (!bytes) return;
	const auto frame = SealArchiveEnvelope({{0,123}, security.storeId, ArchiveRecordKind::Blob, token.toUtf8()},
		security.keyId, *bytes, security.crypto);
	CHECK(frame);
	if (!frame) return;
	Sql(store, u"UPDATE blobs SET bytes=?,envelope=? WHERE blob_id=?"_q,
		{indexed.isEmpty() ? QString::number(size) : indexed, frame.bytes, token});
}

void LogicalUsage() {
	Section = "unique authenticated logical blob sums, full-width sizes, canonical text and overflow";
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
	auto hashes = std::vector<QString>();
	for (auto i = 0; i != 3; ++i) {
		const auto id = u"blob"_q + QString::number(i);
		const auto key = Key({0,123},45,100+i);
		auto event = Event(Empty(), id, 1, u"attachment"_q, ArchiveOrigin::Baseline, key);
		event.content->media.push_back({u"document:555"_q, u"document"_q});
		CHECK(store.Capture(event).retained);
		const auto bytes = i == 2 ? QByteArray("different") : QByteArray("abc");
		const auto hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
		hashes.push_back(hash);
		auto buffer = QBuffer();buffer.setData(bytes);CHECK(buffer.open(QIODevice::ReadOnly));
		CHECK(store.CopyMedia(key,id,u"document:555"_q,buffer,{hash,uint64_t(bytes.size())}));
	}
	auto page = store.OpenDeletedView(Scope());
	CHECK(page.status == ArchiveQueryStatus::Ready && page.entries.empty());
	CHECK(page.counts[4] == 3 && page.counts[5] == 3 && page.counts[6] == 2);
	CHECK(page.logicalComplete && page.logicalBytes == 12);
	CHECK(store.CloseDeletedView(page.view,page.epoch));
	BlobSize(store,hashes[0],std::numeric_limits<uint64_t>::max()-3);
	BlobSize(store,hashes[2],3);
	page=store.OpenDeletedView(Scope());
	CHECK(page.status == ArchiveQueryStatus::Ready && page.logicalBytes == std::numeric_limits<uint64_t>::max());
	CHECK(store.CloseDeletedView(page.view,page.epoch));
	BlobSize(store,hashes[2],4);
	CHECK(store.OpenDeletedView(Scope()).status == ArchiveQueryStatus::Unavailable);
	BlobSize(store,hashes[2],3,u"03"_q);
	CHECK(store.OpenDeletedView(Scope()).status == ArchiveQueryStatus::Unavailable);
	CHECK(store.Remove());
}

void BoundedUsagePages() {
	Section = "129 distinct complete blobs require bounded128/1 usage pages";
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
	for (auto i = 0; i != 129; ++i) {
		const auto id = u"bounded"_q + QString::number(i);
		const auto key = Key({0,123},45,1000+i);
		auto event = Event(Empty(),id,1,u"owned"_q,ArchiveOrigin::Baseline,key);
		event.content->media.push_back({u"document:555"_q,u"document"_q});
		CHECK(store.Capture(event).retained);
		const auto bytes = QByteArray(1,char(i));
		const auto hash = QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
		auto source = QBuffer();source.setData(bytes);CHECK(source.open(QIODevice::ReadOnly));
		CHECK(store.CopyMedia(key,id,u"document:555"_q,source,{hash,1}));
	}
	auto page=store.OpenDeletedView(Scope());
	CHECK(page.status == ArchiveQueryStatus::Ready && !page.logicalComplete && page.logicalBytes == 128);
	CHECK(page.counts[6] == 129);
	page=store.ReadDeletedView(page.view,page.epoch);
	CHECK(page.status == ArchiveQueryStatus::Ready && page.logicalComplete && page.logicalBytes == 129);
	CHECK(store.Remove());
	CHECK(page.active && !page.active->load());
}

void WorkerExpiryAndLogout() {
	Section = "literal worker closes inactive read snapshots even during a busy queue; logout revokes immediately";
	auto temporary=QTemporaryDir();
	auto ready=std::promise<std::shared_ptr<DesktopArchiveJournal>>();
	auto readiness=ready.get_future();
	auto worker=std::make_shared<ProbeViewWorker>(Options(temporary.path()),
		[](DesktopArchiveStatus,QString,uint64_t) {},
		[&](std::shared_ptr<DesktopArchiveJournal> journal) { ready.set_value(std::move(journal)); });
	const auto stop = gsl::finally([&] {
		worker->Stop(false);
		worker->WaitForStopped(std::chrono::seconds(5));
	});
	CHECK(readiness.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	if (readiness.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
		worker->Stop(false);return;
	}
	CHECK(readiness.get());
	const auto open=[&] {
		auto result=std::make_shared<std::promise<ArchiveDeletedPage>>();
		auto future=result->get_future();
			auto accepted=false;
		for (auto i=0;i!=100 && !accepted;++i) {
			accepted=worker->Post([result](DesktopArchiveStore &store) { result->set_value(store.OpenDeletedView(Scope())); });
			if (!accepted) std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		CHECK(accepted);
		CHECK(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
		return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready ? future.get() : ArchiveDeletedPage();
	};
	const auto page=open();CHECK(page.status == ArchiveQueryStatus::Ready && page.active);
	if (!page.active) { worker->Stop(false);return; }
	worker->RevokeOnStop(page.active);
	const auto start=std::chrono::steady_clock::now();
	auto queued=0;
	while (page.active->load() && std::chrono::steady_clock::now()-start < std::chrono::seconds(37)) {
		if (worker->Post([](DesktopArchiveStore &) {})) ++queued;
		std::this_thread::sleep_for(std::chrono::milliseconds(40));
	}
	CHECK(queued > 100 && !page.active->load());
	const auto next=open();CHECK(next.status == ArchiveQueryStatus::Ready && next.active);
	if (next.active) worker->RevokeOnStop(next.active);
	const auto stopping=std::chrono::steady_clock::now();
	CHECK(worker->Stop(true));
	CHECK(std::chrono::steady_clock::now()-stopping < std::chrono::milliseconds(100));
	CHECK(!next.active || !next.active->load());
	CHECK(worker->WaitForStopped(std::chrono::seconds(5)));
	CHECK(worker->Removed());
}

void DeliveryAtCommit() {
	Section = "held production page and shown action revoke at committed purge without a sweep";
	using namespace Purple::ViewDeliveryProbe;
	for (const auto mode : {u"ordinary"_q, u"chat-purge"_q, u"account-purge"_q}) {
		auto temporary = QTemporaryDir();
		auto store = DesktopArchiveStore();
		CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
		Capture(store, Key(), u"before"_q);
		auto session = Session();
		Published.worker = std::make_shared<Worker>();
		Published.worker->store = &store;
		auto shown = ArchiveDeletedPage();
		DeliverDeletedPage(&session, [](DesktopArchiveStore &store) {
			return store.OpenDeletedView(Scope());
		}, [&](ArchiveDeletedPage page) { shown = std::move(page); }, true);
		Published.worker->Run();
		CHECK(Main.size() == 1);
		FlushMain();
		CHECK(shown.status == ArchiveQueryStatus::Ready && shown.active && shown.active->load());
		if (!shown.active) {
			Published = {};
			continue;
		}
		const auto action = ShownHistoryAction(shown.active);
		const auto actions = Actions;
		action();
		CHECK(Actions == actions + 1);
		auto delivered = false;
		auto held = ArchiveDeletedPage();
		DeliverDeletedPage(&session, [](DesktopArchiveStore &store) {
			return store.OpenDeletedView(Scope());
		}, [&](ArchiveDeletedPage page) { delivered = true; held = std::move(page); }, true);
		Published.worker->Run();
		CHECK(!delivered && Main.size() == 1 && Published.worker->flags.size() == 2);
		const auto queuedFlag = Published.worker->flags.back();
		CHECK(queuedFlag->load());
		CHECK(Published.worker->Post([mode](DesktopArchiveStore &store) {
			if (mode == u"ordinary"_q) {
				Capture(store, Key({0, 123}, 45, 68), u"after"_q);
				return;
			}
			const auto header = DesktopArchiveReadHeader(store.database(), Key().account, store.security());
			CHECK(header);
			if (header) {
				const auto &state = std::get<ArchiveState>(header->row.value);
				const auto chat = mode == u"chat-purge"_q ? std::optional(Key().chat) : std::nullopt;
				const auto generations = chat ? std::vector<ArchiveChatGeneration>{{*chat, 0}}
					: std::vector<ArchiveChatGeneration>();
				const auto plan = BeginArchiveSqlPurge(state,
					Purge(state, ArchivePurgeKind::Trashbin, chat), generations,
					u"delivery-purge"_q, store.security());
				CHECK(plan.program && store.Execute(plan.program));
			}
		}));
		Published.worker->Run();
		if (mode == u"ordinary"_q) {
			CHECK(queuedFlag->load() && shown.active->load());
		} else {
			CHECK(!queuedFlag->load() && !shown.active->load());
		}
		FlushMain();
		CHECK(delivered);
		action();
		if (mode == u"ordinary"_q) {
			CHECK(Actions == actions + 2);
			CHECK(held.status == ArchiveQueryStatus::Ready && held.entries.size() == 1);
			CHECK(held.header == shown.header && held.counts == shown.counts);
			const auto next = store.ReadDeletedView(held.view, held.epoch);
			CHECK(next.status == ArchiveQueryStatus::Ready && next.refreshAvailable);
			CHECK(next.header == held.header && next.entries.empty());
		} else {
			CHECK(Actions == actions + 1);
			CHECK(held.status == ArchiveQueryStatus::Stale && held.entries.empty());
			CHECK(held.active && !held.active->load());
			CHECK(store.ReadDeletedView(shown.view, shown.epoch).status == ArchiveQueryStatus::Stale);
		}
		Published = {};
		CHECK(Main.empty());
		CHECK(store.Remove());
	}
}

void HistoryRevocation() {
	Section = "independent History capability clears shown text/actions after commit without another read";
	using namespace ViewRouteProbe;
	for (const auto purge : {false, true}) {
		auto temporary = QTemporaryDir();
		auto store = DesktopArchiveStore();
		CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
		Capture(store, Key(), u"history"_q);
		const auto parent = store.OpenDeletedView(Scope());
		const auto notifications = std::make_shared<std::deque<Fn<void()>>>();
		auto notify = std::make_shared<Fn<void()>>();
		const auto notice = [notifications, notify] { notifications->push_back([notify] { if (*notify) (*notify)(); }); };
		auto reads = 0;
		const auto load = [&] { ++reads; return store.History(Key(), 32, {}, {}, {}, notice); };
		const auto history = load();
		CHECK(history.status == DesktopArchiveStatus::Ready && history.active && !history.history.isEmpty());
		if (!history.active) continue;
		CHECK(store.CloseDeletedView(parent.view, parent.epoch));
		CHECK(history.active->load() && parent.active && !parent.active->load());
		auto live = Session(); live.owner.current = &live;
		auto own = Window(); own.owner = &live; own.root.current = &own;
		struct HistoryState { bool closed = false; std::shared_ptr<std::atomic<bool>> active; };
		const auto state = std::make_shared<HistoryState>(); state->active = history.active;
		const auto guard = Guard{std::make_shared<bool>(true)};
		const auto session = Weak<Session>{std::make_shared<Session*>(&live)};
		const auto window = Weak<Window>{std::make_shared<Window*>(&own)};
		const auto source = live.source;
		const auto mode = live.mode;
		const auto key = Key();
#include "archive-history-current.inc"
		struct Label { QString text = u"shown history"_q; void setText(QString value) { text = std::move(value); } };
		struct Box { bool closed = false; void closeBox() { closed = true; } };
		const auto label = std::make_shared<Label>();
		const auto box = std::make_shared<Box>();
#include "archive-history-notice.inc"
		*notify = check;
		CHECK(current() && !label->text.isEmpty() && !box->closed);
		auto page = history;
		auto callbackSession = Purple::ViewDeliveryProbe::Session();
		const auto weak = base::make_weak(&callbackSession);
		const auto fence = std::make_shared<std::atomic<uint64_t>>(0);
		const auto ticket = uint64_t(0);
		const auto &Retired = Purple::ViewDeliveryProbe::Retired;
		auto delivered = DesktopArchiveHistoryPage();
		const auto done = [&](DesktopArchiveHistoryPage result) { delivered = std::move(result); };
		const auto delivery = [&] {
#include "archive-page-callback.inc"
		};
		if (purge) {
			const auto header = DesktopArchiveReadHeader(store.database(), Key().account, store.security());
			CHECK(header);
			if (!header) continue;
			const auto &snapshot = std::get<ArchiveState>(header->row.value);
			const auto begin = BeginArchiveSqlPurge(snapshot,
				Purge(snapshot, ArchivePurgeKind::Trashbin, Key().chat), {{Key().chat, 0}},
				u"history-revoke"_q, store.security());
			CHECK(begin.program && store.Execute(begin.program));
			CHECK(!history.active->load() && !current());
			CHECK(!notifications->empty());
		} else {
			Capture(store, Key({0, 123}, 45, 68), u"ordinary"_q);
			CHECK(history.active->load() && current() && notifications->empty());
		}
		delivery();
		while (!notifications->empty()) {
			auto event = std::move(notifications->front()); notifications->pop_front(); event();
		}
		CHECK(reads == 1);
		if (purge) {
			CHECK(delivered.status == DesktopArchiveStatus::Stale && delivered.versions.empty());
			CHECK(label->text.isEmpty() && box->closed && !current());
		} else {
			CHECK(delivered.status == DesktopArchiveStatus::Ready && !delivered.versions.empty());
			CHECK(!label->text.isEmpty() && !box->closed && current());
			const auto sibling = load();
			CHECK(sibling.active && sibling.active != history.active && sibling.history != history.history);
			auto wrong = history.epoch; ++wrong.generation;
			store.CloseHistory(history.history, wrong);
			CHECK(history.active->load());
			store.CloseHistory(history.history, history.epoch);
			CHECK(!history.active->load() && sibling.active->load());
			const auto stale = store.History(Key(), 32, {}, {}, history.history, notice);
			CHECK(stale.status == DesktopArchiveStatus::Stale);
		}
		CHECK(store.Remove());
		while (!notifications->empty()) {
			auto event = std::move(notifications->front()); notifications->pop_front(); event();
		}
	}
}

void GenerationRevocation() {
	Section = "authenticated account/chat generations revoke only the addressed snapshot capability";
	auto temporary=QTemporaryDir();
	auto store=DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
	Capture(store,Key(),u"one"_q);
	const auto a=store.OpenDeletedView(Scope());
	const auto b=store.OpenDeletedView(Scope({ArchiveChatKind::Channel,46}));
	CHECK(a.status == ArchiveQueryStatus::Ready && b.status == ArchiveQueryStatus::Ready);
	const auto &security=store.security();
	const auto token=ArchiveSqlChatId(Key().chat,Key().account,security);
	const auto bytes=EncodeArchiveRow({ArchiveChatGeneration{Key().chat,1}});CHECK(bytes);if (!bytes) return;
	const auto frame=SealArchiveEnvelope({Key().account,security.storeId,ArchiveRecordKind::Generation,token.toUtf8()},security.keyId,*bytes,security.crypto);
	CHECK(frame);if (!frame) return;
	CommitSql(store,u"INSERT INTO purge_epochs(chat_token,generation,envelope) VALUES(?,?,?)"_q,{token,u"1"_q,frame.bytes});
	CHECK(a.active && !a.active->load());CHECK(b.active && b.active->load());
	CHECK(store.ReadDeletedView(a.view,a.epoch).status == ArchiveQueryStatus::Stale);
	const auto fresh=store.OpenDeletedView(Scope());
	CHECK(fresh.status == ArchiveQueryStatus::Ready && fresh.entries.size() == 1);
	if (!fresh.entries.empty()) CHECK(fresh.entries.front().generation.chat == 1);
	const auto header=DesktopArchiveReadHeader(store.database(),Key().account,security);CHECK(header);if (!header) return;
	auto account=std::get<ArchiveState>(header->row.value);++account.accountGeneration;++account.revision;
	const auto encoded=EncodeArchiveRow({account});CHECK(encoded);if (!encoded) return;
	const auto sealed=SealArchiveEnvelope({Key().account,security.storeId,ArchiveRecordKind::Account,"account"},security.keyId,*encoded,security.crypto);CHECK(sealed);if (!sealed) return;
	CommitSql(store,u"UPDATE archive_meta SET revision=?,generation=?,envelope=?"_q,
		{QString::number(account.revision),QString::number(account.accountGeneration),sealed.bytes});
	CHECK(!b.active->load() && !fresh.active->load());
	const auto latest=store.OpenDeletedView(Scope());
	CHECK(latest.status == ArchiveQueryStatus::Ready && latest.entries.size() == 1);
	if (!latest.entries.empty()) CHECK(latest.entries.front().generation.account == account.accountGeneration);
	CHECK(store.Remove());
}

void ReaderLimitsAndRevocation() {
	Section = "shared reader cap, epoch ownership, disposal and immediate revocation";
	auto temporary = QTemporaryDir();
	auto options = Options(temporary.path());
	const auto active = std::make_shared<std::atomic<bool>>(true);
	options.active = [active] { return active->load(); };
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	Capture(store, Key(), u"body"_q, u"retained"_q, 0, true, true);
	const auto bytes=QByteArray("abc");
	const auto hash=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
	auto source=QBuffer();source.setData(bytes);CHECK(source.open(QIODevice::ReadOnly));
	CHECK(store.CopyMedia(Key(),u"body"_q,u"document:555"_q,source,{hash,3}));
	CHECK(store.OpenDeletedView(Scope(),ArchiveTranscriptOrder::Chronological,0).status == ArchiveQueryStatus::Stale);
	CHECK(store.OpenDeletedView(Scope(),ArchiveTranscriptOrder::Chronological,129).status == ArchiveQueryStatus::Stale);
	auto pages = std::vector<ArchiveDeletedPage>();
	for (auto i = 0; i != 16; ++i) {
		pages.push_back(store.OpenDeletedView(Scope()));
		CHECK(pages.back().status == ArchiveQueryStatus::Ready);
	}
	CHECK(store.OpenDeletedView(Scope()).status == ArchiveQueryStatus::Busy);
	CHECK(!pages.front().entries.empty());
	if (pages.front().entries.empty() || !pages.front().entries.front().head) return;
	const auto &entry=pages.front().entries.front();
	const auto &version=entry.head->version;
	const auto request=ArchiveMediaRequest{{pages.front().epoch,Key(),version.id,version.revision,
		entry.contentHash,u"document:555"_q,entry.generation},version.media.front().blobId};
	CHECK(!store.OpenMedia(request));
	CHECK(store.CloseDeletedView(pages.back().view, pages.back().epoch));
	CHECK(!pages.back().active->load());
	const auto handle=store.OpenMedia(request);
	CHECK(handle);
	CHECK(store.OpenDeletedView(Scope()).status == ArchiveQueryStatus::Busy);
	if (handle) CHECK(store.ReleaseMedia(*handle));
	auto replacement = store.OpenDeletedView(Scope());
	CHECK(replacement.status == ArchiveQueryStatus::Ready && replacement.view != pages.back().view);
	CHECK(store.ReadDeletedView(pages.back().view, pages.back().epoch).status == ArchiveQueryStatus::Stale);
	CHECK(replacement.active->load());
	active->store(false);
	CHECK(store.ExpireDeletedViews());
	for (const auto &page : pages) CHECK(!page.active->load());
	CHECK(!replacement.active->load());
	CHECK(store.Close());
	active->store(true);
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(store.ReadDeletedView(replacement.view, replacement.epoch).status == ArchiveQueryStatus::Stale);
	const auto idle = store.OpenDeletedView(Scope());
	CHECK(idle.status == ArchiveQueryStatus::Ready);
	idle.active->store(false);
	CHECK(store.ExpireDeletedViews());
	CHECK(!idle.active->load());
	CHECK(store.ReadDeletedView(idle.view, idle.epoch).status == ArchiveQueryStatus::Stale);
	CHECK(store.Close());
	options.loginAuthority=QByteArray(32,'b');
	options.loginAuthKeyId=options.loginAuthKeyIds=QByteArray(16,'2');
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	const auto relogin = store.OpenDeletedView(Scope());
	CHECK(relogin.status == ArchiveQueryStatus::Ready && relogin.counts[0] == 0);
	CHECK(relogin.epoch.storeId != idle.epoch.storeId);
	CHECK(store.ReadDeletedView(idle.view, idle.epoch).status == ArchiveQueryStatus::Stale);
	CHECK(relogin.active && relogin.active->load());
	CHECK(store.Remove());
}

}

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	ViewRouteProbe::Run();
	OriginScopeFence();
	Paging();
	ScopesAndTombstones();
	SnapshotBodies();
	WrapperMetadata();
	InitialAdmissionStatus();
	Corruption();
	LogicalUsage();
	BoundedUsagePages();
	DeliveryAtCommit();
	HistoryRevocation();
	GenerationRevocation();
	ReaderLimitsAndRevocation();
	WorkerExpiryAndLogout();
	return Finish();
}
