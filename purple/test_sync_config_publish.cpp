#define PURPLE_SYNC_CHAT_CREATE_EMBEDDED
#include "test_sync_chat_create.cpp"
#include "purple/purple_sync_config_publish_flow.h"
#include "purple/purple_sync_config_review.h"

#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>

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

}

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	auto worker = std::thread([] {
		for (const auto chat : {PeerToken{}, PeerToken{PeerTokenKind::User,456},
			PeerToken{PeerTokenKind::Chat,789}, PeerToken{PeerTokenKind::Channel,456}}) {
			FirstPublish(chat, false); FirstPublish(chat, true);
		}
		PublishRefusals(); CancelAndScope(); NoMoveAndInitializeOnly(); PendingRestartAndFailures(); RoutingSnapshotCases(); StaleBindingReply(); WorkerEffectSchedules(); PublisherTerminalCancellation(); SourceEpochDuringLongWait();
	});
	worker.join();
	std::printf("PUBLISH FLOW %d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
