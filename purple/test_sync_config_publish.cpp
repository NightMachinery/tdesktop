#define PURPLE_SYNC_CHAT_CREATE_EMBEDDED
#include "test_sync_chat_create.cpp"
#include "purple/purple_sync_config_publish_flow.h"
#include "purple/purple_sync_config_review.h"
#include "purple/purple_config_payload.h"
#include "purple/purple_sync_config_flow.h"
#include "purple/purple_sync_envelope.h"
#include "purple/purple_sync_magic_chat.h"
#include "purple/purple_sync_account_recovery.h"
#include "purple/purple_sync_local_store.h"

#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>

#include <algorithm>
#include <cstring>
#include <functional>

namespace {

struct PublishFiles {
	QTemporaryDir root;
	AccountRoutingSnapshot routing;
	AccountFiles files;
	DesktopSyncChatSetupSnapshot setup;
	QByteArray token;

	explicit PublishFiles(PeerToken chat = {}) {
		CHECK(root.isValid());
		auto choice = LocalChoice();
		choice.rootFileAccount = Origin.account;
		choice.globalAccount = Origin.account;
		const auto local = SerializeLocalChoice(choice);
		const auto owner = SerializeOwnerRecord(Origin.account);
		CHECK(local && owner);
		CHECK(EnsureSyncPrivateDirectory(root.path() + u"/accounts"_q) == SyncStoreStatus::Ready);
		CHECK(WriteSyncPrivateFile(root.path() + u"/local.toml"_q, local->toUtf8()) == SyncStoreStatus::Ready);
		CHECK(WriteSyncPrivateFile(root.path() + u"/accounts/root.toml"_q, owner->toUtf8()) == SyncStoreStatus::Ready);
		routing = { root.path(), Origin.account, choice,
			{ SettingsFileState::Readable, local->toUtf8() }, 1, true };
		auto config = DesktopAccountConfig(routing);
		const auto paths = config.files(Origin.account);
		CHECK(paths); files = *paths;
		CHECK(WriteSyncPrivateFile(files.settings, "version = 2\n") == SyncStoreStatus::Ready);
		setup = Fresh();
		setup.scope.root = root.path(); setup.scope.owner = Origin.account;
		setup.status.phase = DesktopSyncChatPhase::Use;
		setup.status.postingHeld = false;
		setup.status.resolution.readChat = chat;
		setup.cache = Cache(chat);
		setup.cache->readerConsent = true;
		const auto route = Route(chat);
		const auto eligibility = DesktopSyncChatEligibility(route.peer);
		CHECK(eligibility); setup.cache->readers = eligibility->readers;
		setup.cache->readerMembers = route.peer.readerMembers;
		setup.scope.mark = setup.cache->mark;
		const auto bytes = SerializeDesktopSyncChatCache(*setup.cache); CHECK(bytes);
		CHECK(EnsureSyncPrivateDirectory(files.syncDirectory) == SyncStoreStatus::Ready);
		CHECK(WriteSyncPrivateFile(QDir(root.path()).filePath(DesktopSyncChatCacheFileName(Origin.account)), *bytes) == SyncStoreStatus::Ready);
	}

	SyncConfigPublishDiskInput input(PeerToken chat = {}) const {
		auto result = SyncConfigPublishDiskInput();
		result.routing = routing; result.account = Origin.account; result.chat = chat;
		result.binding = token; result.setup = setup; result.setup->scope.binding = token;
		result.inventory = FinishSyncAccountInventory(123,
			{ SyncHistoryScanStatus::Complete, {}, 0 },
			SyncCandidateReadResult{ SyncCandidateReadStatus::Complete, {} });
		result.request = { .expectedFingerprint = MakeSyncSettingsFile(
			SyncSettingsFileStatus::Present, "version = 2\n").fingerprint,
			.expectedParents = std::vector<QString>() };
		result.writer = { u"macOS"_q, u"Purple Telegram Desktop"_q };
		result.queue = SyncConfigSendQueue::Empty; result.now = 1800000000;
		return result;
	}

	void journal(PeerToken chat) {
		auto fresh = setup; fresh.cache.reset(); fresh.scope.mark.reset();
		fresh.status.phase = DesktopSyncChatPhase::Unset;
		fresh.status.resolution.action = SyncChatAction::Unset;
		fresh.scope.settingsBound = false; fresh.scope.space.clear();
		fresh.input.settingsHint = SyncChatSettingsHint::Absent;
		auto value = BeginDesktopSyncChatCreate(fresh,
			{ chat == PeerToken{} ? DesktopSyncChatSetupChoice::Self : DesktopSyncChatSetupChoice::Existing,
			Route(chat).peer, true });
		CHECK(value);
		value->chat = chat; value->markId = 1; value->markDate = 100;
		value->randomId = 42; value->pending = false;
		value->step = DesktopSyncChatCreateStep::Done;
		const auto bytes = SerializeDesktopSyncChatCreateJournal(*value); CHECK(bytes);
		CHECK(WriteSyncPrivateFile(files.syncDirectory + u"/sync-chat-setup.json"_q, *bytes) == SyncStoreStatus::Ready);
	}
};

void FirstPublish(PeerToken chat, bool withJournal) {
	auto files = PublishFiles(chat);
	if (withJournal) { files.journal(chat); }
	auto admitted = std::make_shared<std::atomic<bool>>(true);
	auto reply = SyncConfigPublishDiskReply();
	auto flow = SyncConfigPublishFlow(files.input(chat), admitted,
		[&](SyncConfigPublishDiskReply value) { reply = std::move(value); });
	flow.start();
	CHECK(reply.step == SyncConfigPublishDiskStep::Binding && reply.initial);
	if (!reply.initial) { return; }
	CHECK(reply.initial->version == (chat == PeerToken{} ? 1 : 2));
	CHECK(reply.initial->chat == chat);
	files.token = reply.initial->bindingToken.toLatin1();
	auto discoveryFirst = QByteArray();
	const auto cachePath = QDir(files.root.path()).filePath(DesktopSyncChatCacheFileName(Origin.account));
	if (withJournal) {
		auto cache = *files.setup.cache;
		cache.newReaders = true;
		const auto bytes = SerializeDesktopSyncChatCache(cache); CHECK(bytes);
		discoveryFirst = *bytes;
		CHECK(WriteSyncPrivateFile(cachePath, discoveryFirst) == SyncStoreStatus::Ready);
	}
	flow.bindingCommitted(files.token);
	CHECK(reply.step == SyncConfigPublishDiskStep::Post && !reply.staged.isEmpty());
	CHECK(flow.authorize());
	const auto staged = reply.staged;
	const auto parsed = ParseSyncEnvelope(staged); CHECK(parsed);
	CHECK(staged.contains("Purple Telegram Desktop"));
	CHECK(ReadSettingsFileInput(files.files.syncStore).bytes.contains("pending_seq"));
	flow.confirm(staged, 10);
	CHECK(reply.step == SyncConfigPublishDiskStep::Finished);
	CHECK(reply.result.status == SyncConfigPublishStatus::Confirmed && reply.result.messageId == 10);
	const auto state = ParseSyncLocalState(ReadSettingsFileInput(files.files.syncStore).bytes);
	CHECK(state && state.state.config.pendingSeq == 0 && state.state.config.seq == 1);
	CHECK(state.state.chat == chat);
	if (withJournal) {
		auto scope = files.setup.scope; scope.binding = files.token;
		auto bytes = QByteArray();
		CHECK(ReadSyncPrivateFile(files.files.syncDirectory + u"/sync-chat-setup.json"_q, 512 * 1024, bytes) == SyncStoreStatus::Ready);
		CHECK(ParseDesktopSyncChatCreateJournal(bytes, DesktopSyncChatSetupAuthority(scope), Origin.account));
		auto cacheBytes = QByteArray();
		CHECK(ReadSyncPrivateFile(cachePath, 512 * 1024, cacheBytes) == SyncStoreStatus::Ready);
		CHECK(cacheBytes == discoveryFirst);
		const auto cache = ParseDesktopSyncChatCache(cacheBytes, Origin.account);
		CHECK(cache.record && cache.record->newReaders && cache.record->readerConsent);
	}
	auto lock = QLockFile(files.files.syncDirectory + u"/lock"_q); lock.setStaleLockTime(0);
	CHECK(lock.tryLock(0)); lock.unlock();
	files.setup.scope.settingsBound = true; files.setup.scope.settingsChat = chat;
	files.setup.scope.space = state.state.space;
	auto next = files.input(chat); next.request.pendingOnly = true;
	auto after = SyncConfigPublishFlow(next, admitted,
		[&](SyncConfigPublishDiskReply value) { reply = std::move(value); });
	after.start();
	CHECK(reply.step == SyncConfigPublishDiskStep::Finished);
	CHECK(reply.result.status == SyncConfigPublishStatus::NeedsReview);
}

void PublishRefusals() {
	for (auto mode = 0; mode != 16; ++mode) {
		const auto chat = PeerToken{ PeerTokenKind::Channel, 456 };
		auto files = PublishFiles(chat); auto input = files.input(chat);
		if (mode == 0) input.inventory.status = SyncAccountInventoryStatus::Incomplete;
		if (mode == 1) input.inventory.scan.status = SyncHistoryScanStatus::InvalidResponse;
		if (mode == 2) input.inventory.read->status = SyncCandidateReadStatus::Incomplete;
		if (mode == 3) input.inventory.scan.candidateIds = { 10 };
		if (mode == 4) input.inventory.directory.unreadableCandidate = true;
		if (mode == 5) input.inventory.directory.messageIdCollision = true;
		if (mode == 6) input.account.userId = 124;
		if (mode == 7) input.setup->cache->readerConsent = false;
		if (mode == 8) input.setup->scope.loginBinding = QString(64, u'b');
		if (mode == 9) input.setup->cache->mark.chat = PeerToken{};
		if (mode == 10) input.request.pendingOnly = true;
		if (mode == 11) input.queue = SyncConfigSendQueue::HoldsSyncRecord;
		if (mode == 12) input.inventory.directory.canCreateSpace = false;
		if (mode == 13) input.setup->scope.owner = AccountKey{0,124};
		if (mode == 14) input.setup->scope.mode = AccountWorkMode::Follow;
		if (mode == 15) input.setup->scope.binding = "different";
		auto reply = SyncConfigPublishDiskReply();
		auto flow = SyncConfigPublishFlow(input, std::make_shared<std::atomic<bool>>(true),
			[&](SyncConfigPublishDiskReply value) { reply = std::move(value); });
		flow.start(); CHECK(reply.step == SyncConfigPublishDiskStep::Finished);
		CHECK(SyncPathIdentity(files.files.syncStore) == "missing");
	}
	for (auto mode = 0; mode != 5; ++mode) {
		auto files = PublishFiles({ PeerTokenKind::Channel, 456 });
		files.journal({ PeerTokenKind::Channel, 456 });
		const auto path = files.files.syncDirectory + u"/sync-chat-setup.json"_q;
		auto bytes = QByteArray(); CHECK(ReadSyncPrivateFile(path, 512 * 1024, bytes) == SyncStoreStatus::Ready);
		const auto journal = ParseDesktopSyncChatCreateJournal(bytes,
			DesktopSyncChatSetupAuthority(files.setup.scope), Origin.account); CHECK(journal);
		auto altered = *journal;
		if (mode == 0) altered.step = DesktopSyncChatCreateStep::PostMark;
		if (mode == 1) altered.authority = QString(64, u'b');
		if (mode == 2) altered.chat = PeerToken{};
		if (mode == 3) altered.account.userId = 124;
		if (mode == 4) altered.cancelled = true;
		const auto encoded = SerializeDesktopSyncChatCreateJournal(altered);
		bytes = encoded.value_or(QByteArray("malformed"));
		CHECK(WriteSyncPrivateFile(path, bytes) == SyncStoreStatus::Ready);
		auto reply = SyncConfigPublishDiskReply();
		auto flow = SyncConfigPublishFlow(files.input({PeerTokenKind::Channel,456}),
			std::make_shared<std::atomic<bool>>(true), [&](auto value) { reply = std::move(value); });
		flow.start(); CHECK(reply.step == SyncConfigPublishDiskStep::Finished);
		auto after = QByteArray(); CHECK(ReadSyncPrivateFile(path, 512 * 1024, after) == SyncStoreStatus::Ready);
		CHECK(after == bytes && SyncPathIdentity(files.files.syncStore) == "missing");
	}
}

void CancelAndScope() {
	for (auto mode = 0; mode != 7; ++mode) {
		auto files = PublishFiles(); auto admitted = std::make_shared<std::atomic<bool>>(true);
		auto reply = SyncConfigPublishDiskReply();
		auto flow = SyncConfigPublishFlow(files.input(), admitted, [&](auto value) { reply = std::move(value); });
		flow.start(); CHECK(reply.initial); if (!reply.initial) return;
		files.token = reply.initial->bindingToken.toLatin1(); flow.bindingCommitted(files.token);
		CHECK(reply.step == SyncConfigPublishDiskStep::Post && flow.authorize());
		const auto state = ReadSettingsFileInput(files.files.syncStore);
		if (mode == 0) admitted->store(false);
		if (mode == 1) CHECK(WriteSyncPrivateFile(files.files.settings, "version = 2\n[work_mode]\n") == SyncStoreStatus::Ready);
		if (mode == 2) CHECK(WriteSyncPrivateFile(files.root.path() + u"/local.toml"_q, "bad") == SyncStoreStatus::Ready);
		if (mode == 3) CHECK(WriteSyncPrivateFile(files.root.path() + u"/accounts/root.toml"_q, "bad") == SyncStoreStatus::Ready);
		if (mode == 4) CHECK(WriteSyncPrivateFile(files.files.syncStore, "bad") == SyncStoreStatus::Ready);
		if (mode == 5) CHECK(WriteSyncPrivateFile(files.files.syncDirectory + u"/sync-chat-setup.json"_q, "bad") == SyncStoreStatus::Ready);
		if (mode == 6) {
			auto cache = *files.setup.cache; ++cache.mark.markId;
			const auto bytes = SerializeDesktopSyncChatCache(cache); CHECK(bytes);
			CHECK(WriteSyncPrivateFile(QDir(files.root.path()).filePath(
				DesktopSyncChatCacheFileName(Origin.account)), *bytes) == SyncStoreStatus::Ready);
		}
		CHECK(!flow.authorize()); flow.cancel();
		if (mode != 4) CHECK(ReadSettingsFileInput(files.files.syncStore) == state);
		auto lock = QLockFile(files.files.syncDirectory + u"/lock"_q); lock.setStaleLockTime(0); CHECK(lock.tryLock(0));
	}
}

void ChangeDestination(PublishFiles &files, PeerToken chat) {
	files.setup.cache = Cache(chat);
	files.setup.cache->readerConsent = true;
	const auto route = Route(chat);
	files.setup.cache->readers = DesktopSyncChatEligibility(route.peer)->readers;
	files.setup.cache->readerMembers = route.peer.readerMembers;
	files.setup.scope.mark = files.setup.cache->mark;
	files.setup.status.resolution.readChat = chat;
	const auto bytes = SerializeDesktopSyncChatCache(*files.setup.cache); CHECK(bytes);
	CHECK(WriteSyncPrivateFile(QDir(files.root.path()).filePath(
		DesktopSyncChatCacheFileName(Origin.account)), *bytes) == SyncStoreStatus::Ready);
}

void NoMoveAndInitializeOnly() {
	for (const auto initialChat : {PeerToken{}, PeerToken{PeerTokenKind::Channel,456}}) {
		auto files = PublishFiles(initialChat); auto input = files.input(initialChat);
		input.initializeOnly = true;
		auto reply = SyncConfigPublishDiskReply();
		auto flow = SyncConfigPublishFlow(input, std::make_shared<std::atomic<bool>>(true),
			[&](auto value) { reply = std::move(value); });
		flow.start(); CHECK(reply.initial);
		files.token = reply.initial->bindingToken.toLatin1(); flow.bindingCommitted(files.token);
		CHECK(reply.step == SyncConfigPublishDiskStep::Finished);
		CHECK(reply.result.status == SyncConfigPublishStatus::AlreadySynced);
		const auto before = ReadSettingsFileInput(files.files.syncStore);
		const auto parsed = ParseSyncLocalState(before.bytes); CHECK(parsed);
		CHECK(parsed.state.config.seq == 0 && parsed.state.config.pendingSeq == 0);
		CHECK(parsed.state.version == (initialChat == PeerToken{} ? 1 : 2));
		const auto nextChat = initialChat == PeerToken{}
			? PeerToken{PeerTokenKind::Channel,456} : PeerToken{};
		ChangeDestination(files, nextChat);
		auto next = SyncConfigPublishFlow(files.input(nextChat),
			std::make_shared<std::atomic<bool>>(true), [&](auto value) { reply = std::move(value); });
		next.start(); CHECK(reply.step == SyncConfigPublishDiskStep::Finished);
		CHECK(reply.result.status == SyncConfigPublishStatus::NeedsReview);
		CHECK(ReadSettingsFileInput(files.files.syncStore) == before);
	}
}

void PendingRestartAndFailures() {
	for (auto mode = 0; mode != 4; ++mode) {
		const auto chat = PeerToken{PeerTokenKind::Channel,456};
		auto files = PublishFiles(chat); auto admitted = std::make_shared<std::atomic<bool>>(true);
		auto reply = SyncConfigPublishDiskReply();
		auto flow = SyncConfigPublishFlow(files.input(chat), admitted, [&](auto value) { reply = std::move(value); });
		flow.start(); CHECK(reply.initial); files.token = reply.initial->bindingToken.toLatin1();
		if (mode == 3) {
			CHECK(EnsureSyncPrivateDirectory(files.files.syncDirectory + u"/pending"_q) == SyncStoreStatus::Ready);
			CHECK(WriteSyncPrivateFile(files.files.syncDirectory + u"/pending/config-1.json"_q, "collision") == SyncStoreStatus::Ready);
		}
		flow.bindingCommitted(files.token);
		if (mode == 3) {
			CHECK(reply.step == SyncConfigPublishDiskStep::Finished);
			CHECK(reply.result.status == SyncConfigPublishStatus::StoreError);
			continue;
		}
		CHECK(reply.step == SyncConfigPublishDiskStep::Post);
		const auto staged = reply.staged; const auto before = ReadSettingsFileInput(files.files.syncStore);
		if (mode == 0) {
			flow.confirm(staged + 'x', 10);
			CHECK(reply.result.status == SyncConfigPublishStatus::StoreError);
			CHECK(ReadSettingsFileInput(files.files.syncStore) == before);
			continue;
		}
		flow.cancel(); CHECK(!admitted->load());
		auto input = files.input(chat); input.request = { .pendingOnly = true };
		if (mode == 2) {
			auto candidate = ClassifySyncCandidate(10, staged);
			candidate.documentId = 5010;
			candidate.editDate = 1800000000;
			input.inventory = FinishSyncAccountInventory(123,
				{SyncHistoryScanStatus::Complete,{10},1},
				SyncCandidateReadResult{SyncCandidateReadStatus::Complete,{candidate}});
		}
		auto restart = SyncConfigPublishFlow(input, std::make_shared<std::atomic<bool>>(true),
			[&](auto value) { reply = std::move(value); });
		restart.start();
		if (mode == 1) {
			CHECK(reply.step == SyncConfigPublishDiskStep::Post && reply.staged == staged);
			CHECK(ReadSettingsFileInput(files.files.syncStore) == before);
			restart.cancel();
		} else {
			CHECK(reply.step == SyncConfigPublishDiskStep::Finished);
			if (reply.result.status != SyncConfigPublishStatus::Confirmed) std::printf("restart status=%d store=%d inventory=%d\n", int(reply.result.status), reply.result.storeStatus ? int(*reply.result.storeStatus) : -1, int(input.inventory.status));
			CHECK(reply.result.status == SyncConfigPublishStatus::Confirmed && reply.result.messageId == 10);
			const auto state = ParseSyncLocalState(ReadSettingsFileInput(files.files.syncStore).bytes);
			CHECK(state && state.state.config.pendingSeq == 0 && state.state.chat == chat);
		}
	}
}

void StaleBindingReply() {
	for (const auto chat : {PeerToken{}, PeerToken{PeerTokenKind::User,456},
		PeerToken{PeerTokenKind::Chat,789}, PeerToken{PeerTokenKind::Channel,456}}) {
		auto files = PublishFiles(chat);
		auto oldReply = SyncConfigPublishDiskReply();
		auto old = SyncConfigPublishFlow(files.input(chat),
			std::make_shared<std::atomic<bool>>(true), [&](auto value) { oldReply = std::move(value); });
		old.start(); CHECK(oldReply.initial); if (!oldReply.initial) return;
		files.token = oldReply.initial->bindingToken.toLatin1();
		old.cancel();
		auto lock = QLockFile(files.files.syncDirectory + u"/lock"_q); lock.setStaleLockTime(0);
		CHECK(lock.tryLock(0)); lock.unlock();
		auto freshReply = SyncConfigPublishDiskReply();
		auto fresh = SyncConfigPublishFlow(files.input(chat),
			std::make_shared<std::atomic<bool>>(true), [&](auto value) { freshReply = std::move(value); });
		fresh.start(); CHECK(freshReply.step == SyncConfigPublishDiskStep::Post);
		const auto pending = ReadSettingsFileInput(files.files.syncStore);
		old.bindingCommitted(files.token);
		CHECK(fresh.authorize() && freshReply.step == SyncConfigPublishDiskStep::Post);
		CHECK(ReadSettingsFileInput(files.files.syncStore) == pending);
		CHECK(!lock.tryLock(0));
		fresh.cancel(); CHECK(lock.tryLock(0));
	}
}


auto PublishMono = int64_t(1000);
auto PublishUnix = int64_t(1800000000);
struct PublishTimerEndpoint {
	int64_t due = 0;
	Fn<void()> callback;
	void cancel() { due = 0; }
	void setCallback(Fn<void()> value) { callback = std::move(value); }
	void callOnce(int64_t delay) { due = PublishMono + delay; }
};
struct PublishRetryProbe : base::has_weak_ptr {
	struct Pending {
		explicit Pending(int64_t now) : policy(now) {}
		SyncRetryState policy;
		PublishTimerEndpoint timer;
		Fn<void()> dispatch;
		Fn<void()> expired;
		Fn<void(Fn<void(bool)>)> resume;
		Fn<void(const MTP::Error &)> fail;
		std::optional<SyncRequestTransient> transient;
		QString method;
		mtpRequestId id = 1;
		mtpRequestId wire = 0;
		int64_t retryAtMs = 0;
		uint64_t generation = 0;
		bool active = true;
	};
	struct ApiEndpoint {
		struct Cancellation { void cancel() {} };
		Cancellation request(mtpRequestId) { return {}; }
	} _api;
	Fn<bool()> _current;
	Fn<void(mtpRequestId, SyncRequestTransient)> _deferred;
	std::map<mtpRequestId, std::shared_ptr<Pending>> _pending;
	void publish() {}
	int64_t kept(const QString &) const { return 0; }
	void keep(const QString &, int64_t) const {}
	void cancelAll()
#include "publisher-retry-cancelAll.inc"
	void cancel(mtpRequestId id)
#include "publisher-retry-cancel.inc"
	void dispatch(const std::shared_ptr<Pending> &pending)
#include "publisher-retry-dispatch.inc"
	void failed(const std::shared_ptr<Pending> &pending, const MTP::Error &error, bool timeout = false)
#include "publisher-retry-failed.inc"
	void arm(const std::shared_ptr<Pending> &pending, int64_t delayMs)
#include "publisher-retry-arm.inc"
	void tick(const std::shared_ptr<Pending> &pending)
#include "publisher-retry-tick.inc"
	void resume(const std::shared_ptr<Pending> &pending)
#include "publisher-retry-resume.inc"
};
struct PublishUploadProbe {
	std::optional<DesktopSyncCarryPermit> _carry;
	bool _done = false;
	std::shared_ptr<PublishRetryProbe> _api = std::make_shared<PublishRetryProbe>();
	rpl::lifetime _lifetime;
	Fn<void(std::optional<MsgId>)> _finished;
	void Finish(std::optional<MsgId> messageId)
#include "upload-Finish.inc"
	void Cancel()
#include "upload-Cancel.inc"
};
struct PublishPostProbe {
	bool _done = false;
	bool _posted = true;
	MsgId _messageId;
	std::shared_ptr<PublishUploadProbe> _upload = std::make_shared<PublishUploadProbe>();
	std::unique_ptr<int> _reader;
	Fn<void(SyncConfigPostResult)> _finished;
	void Cancel()
#include "post-Cancel.inc"
};

struct PublishWorkerEndpoint {
	std::shared_ptr<SyncConfigPublishFlow> flow;
	std::vector<std::shared_ptr<SyncConfigPublishFlow>> *retired;
	~PublishWorkerEndpoint() { retired->push_back(std::move(flow)); }
	template <typename Callback> void with(Callback callback) { callback(*flow); }
};

struct PublishCoordinatorProbe : base::has_weak_ptr {
	std::shared_ptr<DesktopSyncCarryGate::Lease> _carryWriter;
	bool _done = false;
	bool _posted = true;
	std::shared_ptr<std::atomic<bool>> _admitted = std::make_shared<std::atomic<bool>>(true);
	std::unique_ptr<PublishPostProbe> _post;
	std::unique_ptr<PublishWorkerEndpoint> _worker;
	Fn<void(SyncConfigPublishResult)> _finished;
	DesktopAccountConfig *router = nullptr;
	UploadSession *session = nullptr;
	std::optional<AccountRoutingSnapshot> opening;
	DesktopAccountConfig &AccountRouting() const { return *router; }
	QString ConfigDirectory() const { return router->snapshot().root; }
	bool CachedAccountSyncOperationsAllowed(UploadSession *value) const {
		return value && !value->account().loggingOut()
			&& value->account().maybeSession() == value && router->permissionsReady();
	}
	bool AccountSyncRoutingCurrent(UploadSession *session, const AccountRoutingSnapshot &snapshot) const
#include "publisher-routing-current.inc"
	bool AccountAvailable() const {
		return !_done && _admitted->load()
			&& (!router || AccountSyncRoutingCurrent(session, *opening));
	}
	void Cancel()
#include "publisher-Cancel.inc"
	void Finish(SyncConfigPublishResult result)
#include "publisher-Finish.inc"
	void OnPost(SyncConfigPostResult result)
#include "publisher-OnPost.inc"
	void SourceChanged() {
		const auto weak = base::make_weak(this);
		const auto changed = [weak]
#include "publisher-source-change.inc"
		;
		changed();
	}
};

void PublisherTerminalCancellation() {
	for (auto mode = 0; mode != 4; ++mode) {
		auto session = UploadSession();
		auto publisher = std::make_unique<PublishCoordinatorProbe>();
		const auto weak = base::make_weak(publisher.get());
		publisher->_post = std::make_unique<PublishPostProbe>();
		publisher->_posted = mode != 0;
		publisher->_post->_finished = [weak](SyncConfigPostResult result) {
			if (const auto self = weak.get(); self && !self->_done) { self->OnPost(std::move(result)); }
		};
		if (mode == 2) { publisher->_post->_done = true; }
		if (mode == 3) { publisher->_post.reset(); }
		auto completions = 0;
		publisher->_finished = [&](SyncConfigPublishResult result) {
			++completions;
			CHECK(result.status == (mode == 0
				? SyncConfigPublishStatus::Cancelled : SyncConfigPublishStatus::OutcomeUnknown));
			CHECK(publisher->_done && !publisher->_admitted->load());
			CHECK(!publisher->_post || publisher->_post->_done);
			publisher.reset();
		};
		publisher->Cancel();
		CHECK(!publisher && !weak && completions == 1);
	}
}

void SourceEpochDuringLongWait() {
	for (const auto restore : {false, true}) {
		auto files = PublishFiles();
		auto router = DesktopAccountConfig(files.root.path());
		const auto opening = router.snapshot();
		CHECK(opening.ready);
		auto session = UploadSession();
		auto publisher = std::make_unique<PublishCoordinatorProbe>();
		publisher->router = &router;
		publisher->session = &session;
		publisher->opening = opening;
		auto replies = SyncConfigPublishDiskReply();
		auto flow = std::make_shared<SyncConfigPublishFlow>(files.input(), publisher->_admitted,
			[&](auto value) { replies = std::move(value); });
		flow->start(); CHECK(replies.initial); if (!replies.initial) { return; }
		files.token = replies.initial->bindingToken.toLatin1(); flow->bindingCommitted(files.token);
		CHECK(replies.step == SyncConfigPublishDiskStep::Post && flow->authorize());
		const auto state = ReadSettingsFileInput(files.files.syncStore);
		const auto stagePath = files.files.syncDirectory + u"/pending/config-1.json"_q;
		const auto pending = ReadSettingsFileInput(stagePath);
		CHECK(pending.state == SettingsFileState::Readable);
		auto retired = std::vector<std::shared_ptr<SyncConfigPublishFlow>>();
		publisher->_worker = std::make_unique<PublishWorkerEndpoint>(PublishWorkerEndpoint{flow, &retired});
		retired.clear(); flow.reset();
		publisher->_post = std::make_unique<PublishPostProbe>();
		const auto weak = base::make_weak(publisher.get());
		publisher->_post->_finished = [weak](SyncConfigPostResult result) {
			if (const auto self = weak.get(); self && !self->_done) { self->OnPost(std::move(result)); }
		};
		auto upload = publisher->_post->_upload;
		PublishMono = 1000;
		upload->_api->_current = [weak] { return weak && weak->AccountAvailable(); };
		const auto waiting = std::make_shared<PublishRetryProbe::Pending>(PublishMono);
		upload->_api->_pending.emplace(waiting->id, waiting);
		auto sends = 0;
		waiting->dispatch = [&] { ++sends; waiting->wire = 1; };
		waiting->resume = [](Fn<void(bool)> ready) { ready(true); };
		upload->_api->dispatch(waiting);
		const auto flood = MTP::Error(MTP_rpc_error(MTP_int(420), MTP_string("FLOOD_WAIT_3600")));
		upload->_api->failed(waiting, flood);
		CHECK(waiting->active && waiting->transient && !waiting->transient->shortRetry);
		const auto retryAt = waiting->retryAtMs;
		CHECK(retryAt == PublishMono + 3600000 && waiting->timer.due == retryAt);
		const auto late = waiting->timer.callback;
		auto completions = 0;
		publisher->_finished = [&](auto result) { ++completions; CHECK(result.status == SyncConfigPublishStatus::OutcomeUnknown); };
		CHECK(WriteSyncPrivateFile(files.files.settings, "version = 2\n[work_mode]\n") == SyncStoreStatus::Ready);
		router.refreshSettings();
		CHECK(router.generation() != opening.sourceGeneration);
		CHECK(!SameAccountRoutingSnapshot(opening, router.snapshot()));
		if (restore) {
			CHECK(WriteSyncPrivateFile(files.files.settings, "version = 2\n") == SyncStoreStatus::Ready);
			router.refreshSettings();
			CHECK(!SameAccountRoutingSnapshot(opening, router.snapshot()));
		}
		publisher->SourceChanged();
		CHECK(publisher->_done && !publisher->_admitted->load() && completions == 1);
		CHECK(upload->_done && upload->_api->_pending.empty() && !publisher->_worker);
		CHECK(!waiting->active && waiting->timer.due == 0 && PublishMono < retryAt);
		PublishMono = retryAt; late();
		CHECK(sends == 1 && !waiting->active && waiting->timer.due == 0);
		CHECK(ReadSettingsFileInput(files.files.syncStore) == state);
		CHECK(ReadSettingsFileInput(stagePath) == pending);
		auto lock = QLockFile(files.files.syncDirectory + u"/lock"_q); lock.setStaleLockTime(0);
		CHECK(!lock.tryLock(0));
		retired.clear();
		CHECK(lock.tryLock(0)); lock.unlock();
		publisher->Cancel(); CHECK(completions == 1);
		auto fresh = router.snapshot();
		CHECK(SameAccountRoutingSnapshot(fresh, router.snapshot()));
	}
}

void RoutingSnapshotCases() {
	auto files = PublishFiles();
	for (auto mode = 0; mode != 8; ++mode) {
		auto changed = files.routing;
		if (mode == 0) changed.root += u"elsewhere"_q;
		if (mode == 1) changed.owner = AccountKey{0,124};
		if (mode == 2) changed.choice.globalAccount = AccountKey{0,124};
		if (mode == 3) changed.choice.accountWorkMode[u"prod-123"_q] = AccountWorkMode::Follow;
		if (mode == 4) changed.local.bytes += 'x';
		if (mode == 5) ++changed.generation;
		if (mode == 6) changed.ready = false;
		if (mode == 7) ++changed.sourceGeneration;
		CHECK(!SameAccountRoutingSnapshot(files.routing, changed));
	}
	CHECK(SameAccountRoutingSnapshot(files.routing, files.routing));
}

// Another install's canonical config record, as a joining device reads it from the chat.
struct OtherRecord {
	ConfigRecordBuildResult built;
	QString space;
};

OtherRecord BuildOther(int writer, uint64_t milliseconds, const QByteArray &text = QByteArray()) {
	auto input = ConfigRecordBuildInput();
	input.text = text.isEmpty() ? "version = 2\n# other device " + QByteArray::number(writer) + '\n' : text;
	const auto space = FormatTimeOrderedSyncSpaceId(milliseconds, QByteArray(10, char(writer)));
	const auto install = FormatSyncInstallId(QByteArray(16, char(0x40 + writer)));
	CHECK(space && install);
	input.space = space.value_or(QString()); input.install = install.value_or(QString());
	input.device = u"android:"_q + input.install; input.platform = u"Android"_q;
	input.app = u"Purple Telegram"_q; input.seq = 1; input.at = 1700000000;
	auto built = BuildConfigRecord(input); CHECK(built);
	return { std::move(built), input.space };
}

SyncAccountInventoryResult InventoryOf(const std::vector<QByteArray> &records) {
	auto candidates = std::vector<SyncCandidateRecord>();
	auto ids = std::vector<int32_t>();
	for (auto i = 0; i != int(records.size()); ++i) {
		auto candidate = ClassifySyncCandidate(20 + i, records[i]);
		candidate.documentId = 6020 + i; candidate.editDate = 1700000000;
		candidates.push_back(candidate); ids.push_back(20 + i);
	}
	return FinishSyncAccountInventory(123,
		{ SyncHistoryScanStatus::Complete, ids, uint64_t(ids.size()) },
		SyncCandidateReadResult{ SyncCandidateReadStatus::Complete, candidates });
}

// A device without sync state joins the one sync space another device already uses in the chat
// the user chose, as Android's A5Q does: Saved Messages (selfJoin), an existing channel picked at
// setup (existingChannelJoin) and a channel another device set up, found by discovery with no setup
// journal here (discoveredChannelJoin). Joining initializes this chat's ledger in that space and
// sends nothing; keeping this device's settings publishes once with the other record as parent.
// A channel this device's setup created, or one holding records from two spaces, stays refused.
void RewriteJournal(PublishFiles &files, PeerToken chat,
		const std::function<void(DesktopSyncChatCreateJournal&)> &change) {
	files.journal(chat);
	const auto path = files.files.syncDirectory + u"/sync-chat-setup.json"_q;
	auto bytes = QByteArray();
	CHECK(ReadSyncPrivateFile(path, 512 * 1024, bytes) == SyncStoreStatus::Ready);
	auto journal = ParseDesktopSyncChatCreateJournal(bytes,
		DesktopSyncChatSetupAuthority(files.setup.scope), Origin.account);
	CHECK(journal);
	if (!journal) { return; }
	change(*journal);
	const auto changed = SerializeDesktopSyncChatCreateJournal(*journal);
	CHECK(changed);
	CHECK(WriteSyncPrivateFile(path, changed.value_or(QByteArray())) == SyncStoreStatus::Ready);
}

void ExistingJoins() {
	const auto channel = PeerToken{ PeerTokenKind::Channel, 456 };
	// The adopted journals are what Desktop setup leaves when the chosen chat already carries
	// another device's mark: no mark of its own, and for Create nothing was created.
	for (const auto name : { "selfJoin", "existingChannelJoin", "discoveredChannelJoin",
			"adoptedChannelJoin", "adoptedCreateJoin" }) {
		for (const auto keep : { false, true }) {
			const auto self = !std::strcmp(name, "selfJoin");
			const auto chat = self ? PeerToken{} : channel;
			auto files = PublishFiles(chat);
			if (!std::strcmp(name, "existingChannelJoin")) { files.journal(chat); }
			if (!std::strncmp(name, "adopted", 7)) {
				const auto create = !std::strcmp(name, "adoptedCreateJoin");
				RewriteJournal(files, chat, [&](DesktopSyncChatCreateJournal &journal) {
					if (create) { journal.choice = DesktopSyncChatSetupChoice::Create; }
					journal.adopted = true; journal.markId = 0; journal.markDate = 0;
					journal.readerConsent.clear(); journal.readerSnapshot = {}; journal.newReaders = false;
				});
			}
			const auto other = BuildOther(2, 1700000000000);
			auto input = files.input(chat);
			input.inventory = InventoryOf({ other.built.canonical });
			if (keep) {
				input.request.expectedParents = SyncConfigVersionKeys({ other.built.version });
			} else {
				input.initializeOnly = true;
			}
			auto reply = SyncConfigPublishDiskReply();
			auto flow = SyncConfigPublishFlow(input, std::make_shared<std::atomic<bool>>(true),
				[&](SyncConfigPublishDiskReply value) { reply = std::move(value); });
			flow.start();
			CHECK(reply.step == SyncConfigPublishDiskStep::Binding && reply.initial);
			std::printf("JOIN %s keep=%d admitted=%d\n", name, int(keep), int(bool(reply.initial)));
			if (!reply.initial) { continue; }
			CHECK(reply.initial->space == other.space && reply.initial->chat == chat);
			CHECK(reply.initial->version == (self ? 1 : 2));
			files.token = reply.initial->bindingToken.toLatin1();
			flow.bindingCommitted(files.token);
			if (keep) {
				CHECK(reply.step == SyncConfigPublishDiskStep::Post && !reply.staged.isEmpty());
				const auto parsed = ParseSyncEnvelope(reply.staged);
				CHECK(parsed.header && parsed.header->space == other.space);
				const auto parents = InspectConfigPayload(parsed).version.parents;
				CHECK(parents == std::vector<QString>{ other.built.version.key });
				flow.confirm(reply.staged, 30);
				CHECK(reply.result.status == SyncConfigPublishStatus::Confirmed && reply.result.messageId == 30);
			} else {
				CHECK(reply.step == SyncConfigPublishDiskStep::Finished
					&& reply.result.status == SyncConfigPublishStatus::AlreadySynced);
			}
			const auto state = ParseSyncLocalState(ReadSettingsFileInput(files.files.syncStore).bytes);
			CHECK(state && state.state.space == other.space && state.state.chat == chat
				&& state.state.version == (self ? 1 : 2) && state.state.config.seq == (keep ? 1u : 0u));
		}
	}
	for (const auto name : { "occupiedFresh", "occupiedMixed" }) {
		auto files = PublishFiles(channel);
		auto records = std::vector<QByteArray>{ BuildOther(2, 1700000000000).built.canonical };
		if (!std::strcmp(name, "occupiedFresh")) {
			RewriteJournal(files, channel, [](DesktopSyncChatCreateJournal &journal) {
				journal.choice = DesktopSyncChatSetupChoice::Create;
				journal.created = true; journal.updates = CreatedUpdates();
			});
		} else {
			records.push_back(BuildOther(3, 1700000001000).built.canonical);
		}
		auto input = files.input(channel);
		input.inventory = InventoryOf(records);
		CHECK(input.inventory.status == SyncAccountInventoryStatus::Complete);
		auto reply = SyncConfigPublishDiskReply();
		auto flow = SyncConfigPublishFlow(input, std::make_shared<std::atomic<bool>>(true),
			[&](SyncConfigPublishDiskReply value) { reply = std::move(value); });
		flow.start();
		CHECK(reply.step == SyncConfigPublishDiskStep::Finished
			&& reply.result.status == SyncConfigPublishStatus::NeedsReview);
		CHECK(SyncPathIdentity(files.files.syncStore) == "missing");
		std::printf("JOIN %s refused=%d\n", name, int(SyncPathIdentity(files.files.syncStore) == "missing"));
	}
}

void WriteMagicBinding(const PublishFiles &files, PeerToken chat, int64_t verifiedAt,
		SyncChatProblem problem = SyncChatProblem::None) {
	auto binding = SyncChatBinding();
	binding.mode = SyncChatBindingMode::Magic; binding.chat = chat;
	binding.verifiedAt = verifiedAt; binding.problem = problem;
	const auto bytes = SerializeSyncChatBinding(binding); CHECK(bytes);
	if (!bytes) { return; }
	CHECK(WriteSyncPrivateFile(QDir(files.files.syncDirectory).filePath(
		QString::fromLatin1(kSyncMagicBindingFile)), *bytes) == SyncStoreStatus::Ready);
}

void MagicPublish() {
	const auto channel = PeerToken{PeerTokenKind::Channel,456};
	for (const auto join : { false, true }) {
		auto files = PublishFiles(channel);
		WriteMagicBinding(files, channel, 100);
		const auto other = BuildOther(2, 1700000000000);
		auto input = files.input(channel); input.setup.reset();
		if (join) {
			input.inventory = InventoryOf({ other.built.canonical });
			input.request.expectedParents = SyncConfigVersionKeys({ other.built.version });
		}
		auto reply = SyncConfigPublishDiskReply();
		auto flow = SyncConfigPublishFlow(input, std::make_shared<std::atomic<bool>>(true),
			[&](SyncConfigPublishDiskReply value) { reply = std::move(value); });
		flow.start();
		CHECK(reply.step == SyncConfigPublishDiskStep::Binding && reply.initial);
		std::printf("MAGIC join=%d admitted=%d\n", int(join), int(bool(reply.initial)));
		if (!reply.initial) { continue; }
		CHECK(reply.initial->chat == channel && reply.initial->version == 2);
		CHECK(!join || reply.initial->space == other.space);
		files.token = reply.initial->bindingToken.toLatin1();
		flow.bindingCommitted(files.token);
		CHECK(reply.step == SyncConfigPublishDiskStep::Post && !reply.staged.isEmpty());
		CHECK(flow.authorize());
		WriteMagicBinding(files, PeerToken{PeerTokenKind::Channel,457}, 100);
		CHECK(!flow.authorize());
		WriteMagicBinding(files, channel, 100);
		flow.confirm(reply.staged, 10);
		CHECK(reply.result.status == SyncConfigPublishStatus::Confirmed && reply.result.messageId == 10);
	}
	for (auto mode = 0; mode != 4; ++mode) {
		auto files = PublishFiles(channel);
		if (mode == 1) WriteMagicBinding(files, channel, 0);
		if (mode == 2) WriteMagicBinding(files, PeerToken{PeerTokenKind::Channel,457}, 100);
		if (mode == 3) WriteMagicBinding(files, channel, 100, SyncChatProblem::MagicIneligible);
		auto input = files.input(channel); input.setup.reset();
		auto reply = SyncConfigPublishDiskReply();
		auto flow = SyncConfigPublishFlow(input, std::make_shared<std::atomic<bool>>(true),
			[&](SyncConfigPublishDiskReply value) { reply = std::move(value); });
		flow.start();
		CHECK(reply.step == SyncConfigPublishDiskStep::Finished
			&& reply.result.status == SyncConfigPublishStatus::NeedsReview);
		CHECK(SyncPathIdentity(files.files.syncStore) == "missing");
	}
}


const auto kCustom = QByteArray("version = 2\n[last_seen]\ntrade_p = false\n");
const auto kCustomSame = QByteArray("version = 2\n# kept here\n[last_seen]\ntrade_p = false\n");
const auto kCustomOther = QByteArray("version = 2\n[last_seen]\nreasons_p = false\n");
const auto kMagicChat = PeerToken{PeerTokenKind::Channel,456};
const auto kArchiveDate = u"20261103-120000"_q;

struct FirstUseRun {
	SyncConfigPublishDiskReply reply;
	QByteArray staged;
};

FirstUseRun RunMagicFlow(PublishFiles &files, const SyncAccountInventoryResult &inventory,
		const std::optional<SyncConfigPublishRequest> &request, int32_t messageId) {
	auto input = files.input(kMagicChat); input.setup.reset();
	input.inventory = inventory;
	if (request) { input.request = *request; } else { input.initializeOnly = true; }
	auto run = FirstUseRun();
	auto flow = SyncConfigPublishFlow(input, std::make_shared<std::atomic<bool>>(true),
		[&](SyncConfigPublishDiskReply value) { run.reply = std::move(value); });
	flow.start();
	if (run.reply.step == SyncConfigPublishDiskStep::Binding && run.reply.initial) {
		files.token = run.reply.initial->bindingToken.toLatin1();
		flow.bindingCommitted(files.token);
	}
	if (run.reply.step == SyncConfigPublishDiskStep::Post) {
		run.staged = run.reply.staged;
		flow.confirm(run.staged, messageId);
	}
	return run;
}

PublishFiles &MagicDevice(PublishFiles &files, const QByteArray &settings) {
	WriteMagicBinding(files, kMagicChat, 100);
	CHECK(WriteSyncPrivateFile(files.files.settings, settings) == SyncStoreStatus::Ready);
	return files;
}

SyncAccountInventoryResult InventoryAt(const std::vector<std::pair<int32_t, QByteArray>> &records) {
	auto candidates = std::vector<SyncCandidateRecord>();
	auto ids = std::vector<int32_t>();
	for (const auto &[id, bytes] : records) {
		auto candidate = ClassifySyncCandidate(id, bytes);
		candidate.documentId = 6000 + uint64_t(id); candidate.editDate = 1700000000;
		candidates.push_back(candidate); ids.push_back(id);
	}
	return FinishSyncAccountInventory(123,
		{ SyncHistoryScanStatus::Complete, ids, uint64_t(ids.size()) },
		SyncCandidateReadResult{ SyncCandidateReadStatus::Complete, candidates });
}

SyncLocalState StateOf(const PublishFiles &files) {
	const auto parsed = ParseSyncLocalState(ReadSettingsFileInput(files.files.syncStore).bytes);
	CHECK(parsed);
	return parsed ? parsed.state : SyncLocalState();
}

std::vector<QString> ParentsOf(const QByteArray &staged) {
	return InspectConfigPayload(ParseSyncEnvelope(staged)).version.parents;
}

QString SpaceOf(const QByteArray &record) {
	const auto parsed = ParseSyncEnvelope(record);
	return parsed.header ? parsed.header->space : QString();
}

bool ArchiveForFirstUse(PublishFiles &files, const SyncAccountInventoryResult &inventory) {
	const auto discovered = DiscoverDesktopSyncStores(files.root.path());
	CHECK(discovered);
	if (!discovered) { return false; }
	auto input = SyncAccountRecoveryInput();
	input.account = Origin.account; input.startupOwner = Origin.account; input.signedIn = true;
	input.preferenceToken = files.token; input.stores = discovered->stores;
	input.local = ReadSyncSettingsFile(files.files); input.discoveryComplete = true;
	input.cloud = SyncRecoveryCloud{ Origin.account, true, inventory };
	const auto plan = PlanSyncFirstUseArchive(input, kArchiveDate, true);
	CHECK(plan && plan->status == SyncStartAgainStatus::Ready);
	if (!plan) { return false; }
	const auto archived = ArchiveSyncAccountState(files.root.path(), files.files, *plan, [] { return true; });
	CHECK(archived.ready && !archived.partial);
	CHECK(SyncPathIdentity(files.files.syncStore) == "missing");
	CHECK(SyncPathIdentity(files.files.syncDirectory + u"/orphaned-"_q + kArchiveDate + u"/state.json"_q) != "missing");
	return archived.ready;
}

void FirstUseActions() {
	const auto other = BuildOther(2, 1700000000000);
	const auto otherCustom = BuildOther(3, 1700000000000, kCustom);
	const auto empty = InventoryOf({});
	const auto withOther = InventoryOf({ other.built.canonical });
	const auto withCustom = InventoryOf({ otherCustom.built.canonical });
	struct Row {
		const char *name;
		QByteArray local;
		SyncAccountInventoryResult inventory;
		SyncFirstUseChoice choice;
		SyncFirstUseAction action;
	};
	const auto rows = std::vector<Row>{
		{ "wait_empty_defaults", "version = 2\n", empty, SyncFirstUseChoice::None, SyncFirstUseAction::Wait },
		{ "wait_unreadable", "version = 2\n[last_seen\n", withOther, SyncFirstUseChoice::None, SyncFirstUseAction::Wait },
		{ "publish_empty_custom", kCustom, empty, SyncFirstUseChoice::None, SyncFirstUseAction::Publish },
		{ "join_defaults", "version = 2\n", withOther, SyncFirstUseChoice::None, SyncFirstUseAction::Join },
		{ "join_silently_same_bytes", kCustom, withCustom, SyncFirstUseChoice::None, SyncFirstUseAction::JoinSilently },
		{ "join_silently_same_meaning", kCustomSame, withCustom, SyncFirstUseChoice::None, SyncFirstUseAction::JoinSilently },
		{ "ask_custom", kCustom, withOther, SyncFirstUseChoice::None, SyncFirstUseAction::Ask },
		{ "ask_synced", kCustom, withOther, SyncFirstUseChoice::Synced, SyncFirstUseAction::Join },
		{ "ask_this_device", kCustom, withOther, SyncFirstUseChoice::ThisDevice, SyncFirstUseAction::Publish },
	};
	for (const auto &row : rows) {
		auto files = PublishFiles(kMagicChat);
		MagicDevice(files, row.local);
		const auto review = ReviewSyncFirstUse(files.files, row.inventory, row.choice);
		CHECK(review.need == SyncFirstUseNeed::NoState && !review.carry);
		CHECK(review.action == row.action);
		std::printf("FIRST USE %s action=%d\n", row.name, review.action ? int(*review.action) : -1);
		if (!review.action || *review.action == SyncFirstUseAction::Wait
			|| *review.action == SyncFirstUseAction::Ask) {
			CHECK(SyncPathIdentity(files.files.syncStore) == "missing");
			continue;
		}
		const auto publish = *review.action == SyncFirstUseAction::Publish
			|| (*review.action == SyncFirstUseAction::JoinSilently && !review.sameBytes);
		CHECK(review.sameBytes == !std::strcmp(row.name, "join_silently_same_bytes"));
		const auto settings = ReadSettingsFileInput(files.files.settings);
		const auto run = RunMagicFlow(files, row.inventory,
			publish ? std::optional(review.request) : std::nullopt, 40);
		CHECK(run.reply.result.status == (publish
			? SyncConfigPublishStatus::Confirmed : SyncConfigPublishStatus::AlreadySynced));
		CHECK(ReadSettingsFileInput(files.files.settings) == settings);
		const auto state = StateOf(files);
		CHECK(state.chat == kMagicChat && state.config.seq == (publish ? 1u : 0u));
		if (row.inventory.directory.selectedSpace) {
			CHECK(state.space == *row.inventory.directory.selectedSpace);
		}
		if (publish && row.inventory.directory.selectedSpace) {
			CHECK(review.request.expectedParents && review.request.expectedParents->size() == 1 && review.headKey
				&& ParentsOf(run.staged) == review.request.expectedParents);
		}
		auto later = row.inventory.read->records.empty()
			? InventoryOf({ run.staged })
			: row.inventory;
		const auto again = ReviewSyncFirstUse(files.files, later);
		CHECK(again.need == SyncFirstUseNeed::None && !again.action);
	}
	auto stale = PublishFiles(kMagicChat);
	MagicDevice(stale, kCustom);
	const auto partial = FinishSyncAccountInventory(123,
		{ SyncHistoryScanStatus::Complete, { 20 }, 1 },
		SyncCandidateReadResult{ SyncCandidateReadStatus::Incomplete, {} });
	const auto refused = ReviewSyncFirstUse(stale.files, partial);
	CHECK(refused.need == SyncFirstUseNeed::NoState && !refused.action);
}

void FirstUseD1() {
	for (const auto choice : { SyncFirstUseChoice::Synced, SyncFirstUseChoice::ThisDevice }) {
		auto files = PublishFiles(kMagicChat);
		MagicDevice(files, kCustom);
		const auto old = RunMagicFlow(files, InventoryOf({}), std::nullopt, 0);
		CHECK(old.reply.result.status == SyncConfigPublishStatus::AlreadySynced);
		const auto oldSpace = StateOf(files).space;
		CHECK(WriteSyncPrivateFile(files.files.syncDirectory + u"/carry.json"_q, "{}") == SyncStoreStatus::Ready);
		CHECK(WriteSyncPrivateFile(files.files.syncDirectory + u"/carry-cache.json"_q, "{}") == SyncStoreStatus::Ready);
		CHECK(EnsureSyncPrivateDirectory(files.files.syncDirectory + u"/carry-objects"_q) == SyncStoreStatus::Ready);
		CHECK(SyncCarryStartupAdmission(files.files.syncDirectory, files.token) != SyncStoreStatus::Ready);
		const auto fresh = BuildOther(2, 1800000000000, kCustomOther);
		const auto inventory = InventoryOf({ fresh.built.canonical });
		CHECK(*inventory.directory.selectedSpace != oldSpace);
		const auto blocked = RunMagicFlow(files, inventory, std::nullopt, 0);
		CHECK(blocked.reply.result.status != SyncConfigPublishStatus::AlreadySynced);
		CHECK(StateOf(files).space == oldSpace);
		const auto review = ReviewSyncFirstUse(files.files, inventory);
		CHECK(review.need == SyncFirstUseNeed::OtherSpace && review.carry);
		CHECK(review.action == SyncFirstUseAction::Ask);
		CHECK(RetireSyncCarryLeftovers(files.files.syncDirectory, kArchiveDate));
		CHECK(!SyncCarryLeftoversPresent(files.files.syncDirectory));
		CHECK(SyncPathIdentity(files.files.syncDirectory + u"/carry-retired-"_q + kArchiveDate + u"/carry.json"_q) != "missing");
		CHECK(SyncCarryStartupAdmission(files.files.syncDirectory, files.token) == SyncStoreStatus::Ready);
		CHECK(RetireSyncCarryLeftovers(files.files.syncDirectory, kArchiveDate));
		const auto answered = ReviewSyncFirstUse(files.files, inventory, choice);
		CHECK(answered.need == SyncFirstUseNeed::OtherSpace && !answered.carry);
		const auto join = (choice == SyncFirstUseChoice::Synced);
		CHECK(answered.action == (join ? SyncFirstUseAction::Join : SyncFirstUseAction::Publish));
		CHECK(answered.request.expectedParents == SyncConfigVersionKeys({ fresh.built.version }));
		CHECK(ArchiveForFirstUse(files, inventory));
		const auto settings = ReadSettingsFileInput(files.files.settings);
		const auto run = RunMagicFlow(files, inventory, join ? std::nullopt : std::optional(answered.request), 50);
		CHECK(run.reply.result.status == (join
			? SyncConfigPublishStatus::AlreadySynced : SyncConfigPublishStatus::Confirmed));
		CHECK(ReadSettingsFileInput(files.files.settings) == settings);
		const auto state = StateOf(files);
		CHECK(state.space == fresh.space && state.chat == kMagicChat);
		if (!join) {
			CHECK(SpaceOf(run.staged) == fresh.space);
			CHECK(ParentsOf(run.staged) == SyncConfigVersionKeys({ fresh.built.version }));
		}
		const auto later = join ? inventory : InventoryAt({ { 20, fresh.built.canonical }, { 50, run.staged } });
		const auto again = ReviewSyncFirstUse(files.files, later);
		CHECK(again.need == SyncFirstUseNeed::None && !again.action && !again.carry);
		const auto checked = ReviewSyncConfigInventory(later, &state, QByteArray(), ReadSyncSettingsFile(files.files));
		CHECK(checked.status == SyncConfigReviewStatus::Ready && checked.reason == SyncConfigReviewReason::None);
		std::printf("FIRST USE D1 choice=%d space_ok=%d\n", int(choice), int(state.space == fresh.space));
	}
}

void FirstUseMigration() {
	auto first = PublishFiles(kMagicChat);
	auto second = PublishFiles(kMagicChat);
	MagicDevice(first, kCustom);
	MagicDevice(second, kCustom);
	const auto seed = RunMagicFlow(first, InventoryOf({}), SyncConfigPublishRequest{
		.expectedFingerprint = MakeSyncSettingsFile(SyncSettingsFileStatus::Present, kCustom).fingerprint,
		.expectedParents = std::vector<QString>() }, 60);
	CHECK(seed.reply.result.status == SyncConfigPublishStatus::Confirmed);
	const auto shared = RunMagicFlow(second, InventoryOf({ seed.staged }), std::nullopt, 0);
	CHECK(shared.reply.result.status == SyncConfigPublishStatus::AlreadySynced);
	const auto space = StateOf(first).space;
	CHECK(!space.isEmpty() && StateOf(second).space == space);
	const auto oldChat = PeerToken{ PeerTokenKind::Channel, 999 };
	for (const auto device : { &first, &second }) {
		auto before = StateOf(*device);
		before.chat = oldChat;
		const auto old = SerializeSyncLocalState(before); CHECK(old);
		CHECK(WriteSyncPrivateFile(device->files.syncStore, old.canonical) == SyncStoreStatus::Ready);
		const auto moved = MoveSyncStateToChat(StateOf(*device), kMagicChat, uint64_t(Origin.account.userId));
		CHECK(moved.move && moved.move.changed && moved.move.state.space == space);
		const auto encoded = SerializeSyncLocalState(moved.move.state); CHECK(encoded);
		CHECK(WriteSyncPrivateFile(device->files.syncStore, encoded.canonical) == SyncStoreStatus::Ready);
		CHECK(StateOf(*device).chat == kMagicChat && StateOf(*device).space == space);
	}
	auto prompts = 0;
	const auto empty = InventoryOf({});
	for (const auto device : { &first, &second }) {
		const auto review = ReviewSyncFirstUse(device->files, empty);
		CHECK(review.need == SyncFirstUseNeed::None && !review.action);
		prompts += (review.action == SyncFirstUseAction::Ask) ? 1 : 0;
	}
	const auto republished = RunMagicFlow(first, empty, SyncConfigPublishRequest{
		.expectedFingerprint = MakeSyncSettingsFile(SyncSettingsFileStatus::Present, kCustom).fingerprint,
		.expectedParents = std::vector<QString>() }, 61);
	CHECK(republished.reply.result.status == SyncConfigPublishStatus::Confirmed);
	CHECK(SpaceOf(republished.staged) == space && StateOf(first).space == space);
	const auto magic = InventoryAt({ { 61, republished.staged } });
	CHECK(magic.directory.selectedSpace == space);
	for (const auto device : { &first, &second }) {
		const auto review = ReviewSyncFirstUse(device->files, magic);
		CHECK(review.need == SyncFirstUseNeed::None && !review.action);
		prompts += (review.action == SyncFirstUseAction::Ask) ? 1 : 0;
	}
	const auto joined = RunMagicFlow(second, magic, std::nullopt, 0);
	CHECK(joined.reply.result.status == SyncConfigPublishStatus::AlreadySynced);
	CHECK(StateOf(second).space == space && StateOf(second).chat == kMagicChat);
	const auto secondState = StateOf(second);
	const auto checked = ReviewSyncConfigInventory(magic, &secondState, QByteArray(), ReadSyncSettingsFile(second.files));
	CHECK(checked.status == SyncConfigReviewStatus::Ready && checked.reason == SyncConfigReviewReason::None);
	CHECK(checked.plan.verdict == ConfigSyncVerdict::Adopt || checked.plan.verdict == ConfigSyncVerdict::UpToDate);
	prompts += (checked.plan.verdict == ConfigSyncVerdict::Adopt || checked.plan.verdict == ConfigSyncVerdict::UpToDate) ? 0 : 1;
	CHECK(prompts == 0);
	std::printf("FIRST USE migration prompts=%d one_space=%d\n", prompts,
		int(StateOf(first).space == StateOf(second).space));
}

void FirstUseMarkKeepsAnswer() {
	const auto other = BuildOther(2, 1700000000000);
	const auto inventory = InventoryOf({ other.built.canonical });
	auto files = PublishFiles(kMagicChat);
	MagicDevice(files, kCustom);
	CHECK(ReadSyncFirstUseMark(files.files) == SyncFirstUseMark());
	CHECK(ReviewSyncFirstUse(files.files, inventory).action == SyncFirstUseAction::Ask);
	auto mark = SyncFirstUseMark();
	mark.choice = SyncFirstUseChoice::Synced;
	mark.joinSpace = other.space;
	mark.heads = { other.built.version.key };
	mark.checkedAt = 1800000000;
	mark.fingerprint = u"fp"_q;
	CHECK(WriteSyncFirstUseMark(files.files, mark));
	CHECK(ReadSyncFirstUseMark(files.files) == mark);
	const auto answered = ReviewSyncFirstUse(files.files, inventory);
	CHECK(answered.need == SyncFirstUseNeed::NoState && answered.action == SyncFirstUseAction::Join);
	CHECK(ReviewSyncFirstUse(files.files, inventory, SyncFirstUseChoice::ThisDevice).action
		== SyncFirstUseAction::Publish);
	const auto joined = RunMagicFlow(files, inventory, std::nullopt, 0);
	CHECK(joined.reply.result.status == SyncConfigPublishStatus::AlreadySynced);
	const auto resumed = ReviewSyncFirstUse(files.files, inventory);
	CHECK(resumed.need == SyncFirstUseNeed::Unjoined && resumed.action == SyncFirstUseAction::Join);
	CHECK(!resumed.settled && resumed.headKey);
	mark.joinSpace.clear();
	CHECK(WriteSyncFirstUseMark(files.files, mark));
	const auto settled = ReviewSyncFirstUse(files.files, inventory);
	CHECK(settled.need == SyncFirstUseNeed::None && !settled.action && settled.settled);
	CHECK(ClearSyncFirstUseMark(files.files) && ClearSyncFirstUseMark(files.files));
	CHECK(ReadSyncFirstUseMark(files.files) == SyncFirstUseMark());
	CHECK(WriteSyncPrivateFile(files.files.syncDirectory + u"/first-use.json"_q, "{\"version\":2}") == SyncStoreStatus::Ready);
	CHECK(!ReadSyncFirstUseMark(files.files));
	std::printf("FIRST USE mark resumed=%d settled=%d\n", int(resumed.need == SyncFirstUseNeed::Unjoined),
		int(settled.settled));
}

ConfigRecordBuildResult BuildInSpace(const QString &space, char writer, const QByteArray &text,
		std::vector<ConfigVersion> parents, uint64_t at) {
	auto input = ConfigRecordBuildInput();
	input.text = text; input.parents = std::move(parents); input.space = space;
	input.install = FormatSyncInstallId(QByteArray(16, writer)).value_or(QString());
	input.device = u"android:"_q + input.install; input.platform = u"Android"_q;
	input.app = u"Purple Telegram"_q; input.seq = 1; input.at = at;
	auto built = BuildConfigRecord(input); CHECK(built);
	return built;
}

void FirstUseCurrentHead() {
	const auto space = FormatTimeOrderedSyncSpaceId(1700000000000, QByteArray(10, 'w')).value_or(QString());
	const auto r1 = BuildInSpace(space, 'D', kCustomOther, {}, 1700000000);
	const auto r2 = BuildInSpace(space, 'A', kCustom, { r1.version }, 1700000100);
	const auto inventory = InventoryAt({ { 20, r1.canonical }, { 21, r2.canonical } });
	CHECK(inventory.directory.selectedSpace == space);
	const auto heads = ExtractSyncConfigHeads(inventory, space, QString());
	CHECK(heads.status == SyncConfigHeadsStatus::Complete && heads.heads.size() == 2);
	struct Row {
		const char *name;
		QByteArray local;
		SyncFirstUseChoice choice;
		SyncFirstUseAction action;
	};
	const auto rows = std::vector<Row>{
		{ "join_defaults", "version = 2\n", SyncFirstUseChoice::None, SyncFirstUseAction::Join },
		{ "join_silently_same_bytes", kCustom, SyncFirstUseChoice::None, SyncFirstUseAction::JoinSilently },
		{ "join_silently_same_meaning", kCustomSame, SyncFirstUseChoice::None, SyncFirstUseAction::JoinSilently },
		{ "ask_holding_r1", kCustomOther, SyncFirstUseChoice::None, SyncFirstUseAction::Ask },
		{ "use_synced", kCustomOther, SyncFirstUseChoice::Synced, SyncFirstUseAction::Join },
		{ "use_this_device", kCustomOther, SyncFirstUseChoice::ThisDevice, SyncFirstUseAction::Publish },
	};
	for (const auto &row : rows) {
		auto files = PublishFiles(kMagicChat);
		MagicDevice(files, row.local);
		const auto review = ReviewSyncFirstUse(files.files, inventory, row.choice);
		CHECK(review.need == SyncFirstUseNeed::NoState && review.action == row.action);
		CHECK(review.headKey == r2.version.key && review.input.headKey == r2.version.key);
		CHECK(review.request.expectedParents == std::vector<QString>{ r2.version.key });
		std::printf("FIRST USE current head %s action=%d head_r2=%d\n", row.name,
			review.action ? int(*review.action) : -1, int(review.headKey == r2.version.key));
		if (!review.action || *review.action == SyncFirstUseAction::Ask) {
			continue;
		}
		const auto publish = *review.action == SyncFirstUseAction::Publish
			|| (*review.action == SyncFirstUseAction::JoinSilently && !review.sameBytes);
		CHECK(review.sameBytes == !std::strcmp(row.name, "join_silently_same_bytes"));
		const auto run = RunMagicFlow(files, inventory, publish ? std::optional(review.request) : std::nullopt, 40);
		CHECK(run.reply.result.status == (publish
			? SyncConfigPublishStatus::Confirmed : SyncConfigPublishStatus::AlreadySynced));
		const auto state = StateOf(files);
		CHECK(state.space == space);
		if (publish) {
			CHECK(ParentsOf(run.staged) == std::vector<QString>{ r2.version.key });
			continue;
		}
		const auto joined = ReviewSyncConfigInventory(inventory, &state, QByteArray(), ReadSyncSettingsFile(files.files));
		CHECK(joined.status == SyncConfigReviewStatus::Ready);
		if (joined.plan.verdict == ConfigSyncVerdict::Adopt) {
			CHECK(review.sameBytes);
		} else {
			CHECK(!joined.plan.offered.empty() && joined.plan.offered.front().key == *review.headKey);
		}
	}
}

void FirstUseSplitHeads() {
	const auto space = FormatTimeOrderedSyncSpaceId(1700000000000, QByteArray(10, 'v')).value_or(QString());
	const auto kThird = QByteArray("version = 2\n[last_seen]\ntrade_p = false\nreasons_p = false\n");
	const auto r1 = BuildInSpace(space, 'D', kCustomOther, {}, 1700000000);
	const auto r2 = BuildInSpace(space, 'A', kCustom, {}, 1700000100);
	const auto split = InventoryAt({ { 20, r1.canonical }, { 21, r2.canonical } });
	auto both = std::vector<QString>{ r1.version.key, r2.version.key };
	std::sort(both.begin(), both.end());
	const auto sorted = [](std::vector<QString> keys) { std::sort(keys.begin(), keys.end()); return keys; };
	auto files = PublishFiles(kMagicChat);
	MagicDevice(files, kThird);
	const auto asked = ReviewSyncFirstUse(files.files, split);
	CHECK(asked.action == SyncFirstUseAction::Ask && !asked.headKey && asked.heads == both);
	const auto answered = ReviewSyncFirstUse(files.files, split, SyncFirstUseChoice::ThisDevice);
	CHECK(answered.action == SyncFirstUseAction::Publish && answered.request.expectedParents
		&& sorted(*answered.request.expectedParents) == both);
	auto mark = SyncFirstUseMark();
	mark.choice = SyncFirstUseChoice::ThisDevice;
	mark.heads = answered.heads;
	CHECK(WriteSyncFirstUseMark(files.files, mark));
	CHECK(ReviewSyncFirstUse(files.files, split).action == SyncFirstUseAction::Publish);
	const auto r3 = BuildInSpace(space, 'D', kCustom, { r1.version, r2.version }, 1700000200);
	const auto resolved = InventoryAt({ { 20, r1.canonical }, { 21, r2.canonical }, { 22, r3.canonical } });
	const auto stale = ReviewSyncFirstUse(files.files, resolved);
	CHECK(stale.action == SyncFirstUseAction::Ask && stale.input.choice == SyncFirstUseChoice::None);
	CHECK(stale.heads != mark.heads && stale.headKey);
	const auto run = RunMagicFlow(files, split, answered.request, 45);
	CHECK(run.reply.result.status == SyncConfigPublishStatus::Confirmed);
	CHECK(sorted(ParentsOf(run.staged)) == both && SpaceOf(run.staged) == space);
	const auto after = InventoryAt({ { 20, r1.canonical }, { 21, r2.canonical }, { 45, run.staged } });
	const auto state = StateOf(files);
	const auto checked = ReviewSyncConfigInventory(after, &state, QByteArray(), ReadSyncSettingsFile(files.files));
	CHECK(checked.status == SyncConfigReviewStatus::Ready);
	CHECK(checked.plan.verdict == ConfigSyncVerdict::UpToDate);
	std::printf("FIRST USE split superseded=%d stale_dropped=%d\n",
		int(sorted(ParentsOf(run.staged)) == both), int(stale.action == SyncFirstUseAction::Ask));
}

void FirstUseRace() {
	for (const auto choice : { SyncFirstUseChoice::Synced, SyncFirstUseChoice::ThisDevice }) {
		auto first = PublishFiles(kMagicChat);
		auto second = PublishFiles(kMagicChat);
		MagicDevice(first, kCustom);
		MagicDevice(second, kCustomOther);
		const auto empty = InventoryOf({});
		const auto a = ReviewSyncFirstUse(first.files, empty);
		const auto b = ReviewSyncFirstUse(second.files, empty);
		CHECK(a.action == SyncFirstUseAction::Publish && b.action == SyncFirstUseAction::Publish);
		const auto ra = RunMagicFlow(first, empty, a.request, 70);
		const auto rb = RunMagicFlow(second, empty, b.request, 71);
		CHECK(ra.reply.result.status == SyncConfigPublishStatus::Confirmed);
		CHECK(rb.reply.result.status == SyncConfigPublishStatus::Confirmed);
		const auto mixed = InventoryOf({ ra.staged, rb.staged });
		CHECK(mixed.directory.selectedSpace && SpaceOf(ra.staged) != SpaceOf(rb.staged));
		if (!mixed.directory.selectedSpace) { continue; }
		const auto winnerFirst = (*mixed.directory.selectedSpace == SpaceOf(ra.staged));
		auto &winner = winnerFirst ? first : second;
		auto &loser = winnerFirst ? second : first;
		const auto &head = winnerFirst ? ra.staged : rb.staged;
		const auto won = ReviewSyncFirstUse(winner.files, mixed);
		CHECK(won.need == SyncFirstUseNeed::None && !won.action);
		const auto lost = ReviewSyncFirstUse(loser.files, mixed);
		CHECK(lost.need == SyncFirstUseNeed::OtherSpace && lost.action == SyncFirstUseAction::Ask);
		const auto answered = ReviewSyncFirstUse(loser.files, mixed, choice);
		const auto join = (choice == SyncFirstUseChoice::Synced);
		CHECK(answered.action == (join ? SyncFirstUseAction::Join : SyncFirstUseAction::Publish));
		CHECK(ArchiveForFirstUse(loser, mixed));
		const auto run = RunMagicFlow(loser, mixed, join ? std::nullopt : std::optional(answered.request), 72);
		CHECK(run.reply.result.status == (join
			? SyncConfigPublishStatus::AlreadySynced : SyncConfigPublishStatus::Confirmed));
		CHECK(StateOf(loser).space == *mixed.directory.selectedSpace);
		if (!join) {
			CHECK(SpaceOf(run.staged) == *mixed.directory.selectedSpace);
			CHECK(ParentsOf(run.staged) == std::vector<QString>{ answered.headKey.value_or(QString()) });
			CHECK(answered.headKey && InspectConfigPayload(ParseSyncEnvelope(head)).version.key == *answered.headKey);
		}
		const auto after = join ? mixed : InventoryOf({ ra.staged, rb.staged, run.staged });
		const auto again = ReviewSyncFirstUse(loser.files, after);
		CHECK(again.need == SyncFirstUseNeed::None && !again.action);
		std::printf("FIRST USE race choice=%d loser_joined=%d\n", int(choice),
			int(StateOf(loser).space == *mixed.directory.selectedSpace));
	}
}

}

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	auto worker = std::thread([] {
		for (const auto chat : {PeerToken{}, PeerToken{PeerTokenKind::User,456},
			PeerToken{PeerTokenKind::Chat,789}, PeerToken{PeerTokenKind::Channel,456}}) {
			FirstPublish(chat, false); FirstPublish(chat, true);
		}
		PublishRefusals(); CancelAndScope(); NoMoveAndInitializeOnly(); PendingRestartAndFailures(); RoutingSnapshotCases(); StaleBindingReply(); WorkerEffectSchedules(); PublisherTerminalCancellation(); SourceEpochDuringLongWait();
		ExistingJoins(); MagicPublish();
		FirstUseActions(); FirstUseD1(); FirstUseMigration(); FirstUseRace(); FirstUseMarkKeepsAnswer(); FirstUseCurrentHead(); FirstUseSplitHeads();
	});
	worker.join();
	std::printf("PUBLISH FLOW %d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
