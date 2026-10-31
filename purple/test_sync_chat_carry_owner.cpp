#define PURPLE_SYNC_CHAT_CARRY_EXECUTOR_EMBEDDED
#include "test_sync_chat_carry.cpp"
#include "purple/purple_sync_chat_carry_owner.h"
#include "purple/purple_sync_chat_setup_flow.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <chrono>

namespace {

#include "store-thread.inc"

void StoreThreadTimers() {
	using Clock = std::chrono::steady_clock;
	const auto identity = std::make_shared<std::promise<std::thread::id>>();
	auto identityReady = identity->get_future();
	const auto timed = std::make_shared<std::promise<std::thread::id>>();
	auto timedReady = timed->get_future();
	{
		auto thread = StoreThread();
		thread.poster()([identity] { identity->set_value(std::this_thread::get_id()); });
		thread.delayedPoster()(20, [timed] { timed->set_value(std::this_thread::get_id()); });
		const auto first = identityReady.wait_for(std::chrono::seconds(2));
		const auto second = timedReady.wait_for(std::chrono::seconds(2));
		CHECK(first == std::future_status::ready && second == std::future_status::ready);
		if (first == std::future_status::ready && second == std::future_status::ready) {
			const auto worker = identityReady.get();
			CHECK(worker == timedReady.get() && worker != std::this_thread::get_id());
		}
	}
	struct Dropped {
		std::shared_ptr<std::promise<void>> result;
		~Dropped() { result->set_value(); }
	};
	const auto result = std::make_shared<std::promise<void>>();
	auto dropped = result->get_future();
	const auto ran = std::make_shared<std::atomic<bool>>(false);
	const auto drained = std::make_shared<std::atomic<bool>>(false);
	const auto early = std::make_shared<std::promise<void>>();
	auto earlyReady = early->get_future();
	const auto begin = Clock::now();
	{
		auto thread = StoreThread();
		auto lifetime = std::make_shared<Dropped>(); lifetime->result = result;
		thread.delayedPoster()(600000, [lifetime, ran] { ran->store(true); });
		thread.delayedPoster()(20, [early] { early->set_value(); });
		CHECK(earlyReady.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
		thread.poster()([] { throw std::runtime_error("synthetic worker failure"); });
		thread.poster()([drained] { drained->store(true); });
	}
	CHECK(dropped.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
	CHECK(!ran->load() && drained->load() && Clock::now() - begin < std::chrono::seconds(3));
}

DesktopSyncChatCacheRecord SourceCache(ExecutorFiles &f, const SyncChatCache &mark) {
	auto cache = DesktopSyncChatCacheRecord();
	cache.account = f.scope.account; cache.mark = mark; cache.loginBinding = f.scope.loginAuthority;
	cache.readers = DesktopSyncChatEligibility(f.fresh.source.peer)->readers;
	cache.readerConsent = true; cache.disclosedStreams = {u"config"_q};
	cache.readerCreator = f.fresh.source.peer.facts->creator; cache.readerAdmin = f.fresh.source.peer.facts->admin;
	cache.readerKind = f.fresh.source.peer.kind; cache.readerMembers = f.fresh.source.peer.readerMembers;
	return cache;
}

DesktopSyncChatSetupSnapshot OpeningFrame(ExecutorFiles &f, bool lost) {
	auto frame = DesktopSyncChatSetupSnapshot();
	frame.scope.origin = f.fresh.target.peer.origin; frame.scope.root = f.temporary.path();
	frame.scope.binding = f.scope.binding; frame.scope.loginBinding = f.scope.loginAuthority;
	frame.scope.settingsBound = true; frame.scope.settingsChat = f.initial.chat; frame.scope.space = f.initial.space;
	frame.scope.mark = f.fresh.resolutionInput.cache;
	frame.input = f.fresh.resolutionInput;
	if (!lost) {
		frame.input.marks.push_back({*frame.scope.mark, SyncChatMarkStatus::Supported,
			frame.input.cachedEligibility, SyncChatRecordPresence::Present, true, true});
	}
	frame.status.complete = true; frame.status.postingHeld = false; frame.status.resolution = ResolveSyncChat(frame.input);
	frame.status.phase = frame.status.resolution.action == SyncChatAction::Use ? DesktopSyncChatPhase::Use
		: frame.status.resolution.action == SyncChatAction::Lost ? DesktopSyncChatPhase::Lost : DesktopSyncChatPhase::Searching;
	frame.run = 1; frame.stateImage = CaptureSyncPrivateFile(f.root + u"/state.json"_q, kSyncCarryJournalStorageLimit).bytes;
	const auto cache = SourceCache(f, *frame.scope.mark);
	frame.cache = cache;
	const auto bytes = SerializeDesktopSyncChatCache(cache); Must(bytes); frame.cacheImage = *bytes;
	Must(WriteSyncPrivateFile(f.scope.cacheFile, *bytes) == SyncStoreStatus::Ready);
	return frame;
}

// Use-state snapshot read back from disk after a move, as the mark actions test builds it.
DesktopSyncChatSetupSnapshot MovedFrame(ExecutorFiles &f) {
	auto result = DesktopSyncChatSetupSnapshot();
	result.stateImage = CaptureSyncPrivateFile(f.root + u"/state.json"_q, kSyncCarryJournalStorageLimit).bytes;
	const auto state = ParseSyncLocalState(result.stateImage); Must(state);
	result.cacheImage = CaptureSyncPrivateFile(f.scope.cacheFile, 64 * 1024).bytes;
	const auto cache = ParseDesktopSyncChatCache(result.cacheImage, f.scope.account); Must(cache.record);
	result.cache = cache.record;
	result.scope.origin = Origin; result.scope.root = f.temporary.path(); result.scope.binding = f.scope.binding;
	result.scope.loginBinding = f.scope.loginAuthority; result.scope.settingsBound = true;
	result.scope.settingsChat = state.state.chat; result.scope.space = state.state.space;
	result.scope.mark = cache.record->mark;
	result.input.selfUserId = Origin.account.userId; result.input.cache = cache.record->mark;
	result.input.cacheRead = SyncChatDirectRead::Valid; result.input.settingsBound = true; result.input.settingsChat = state.state.chat;
	result.input.cachedEligibility = *DesktopSyncChatEligibility(Route(state.state.chat).peer);
	result.input.readerWarningAccepted = true;
	result.input.marks.push_back({cache.record->mark, SyncChatMarkStatus::Supported,
		result.input.cachedEligibility, SyncChatRecordPresence::Present, true, true});
	result.status.complete = true; result.status.postingHeld = false; result.status.phase = DesktopSyncChatPhase::Use;
	result.status.resolution = ResolveSyncChat(result.input); Must(result.status.resolution.action == SyncChatAction::Use);
	result.run = 1;
	return result;
}

QByteArray CreatedUpdates() {
	auto channel = NativeChannel(); channel.id = MTP_long(456);
	channel.flags = MTP_flags(MTPDchannel::Flag::f_access_hash | MTPDchannel::Flag::f_creator | MTPDchannel::Flag::f_broadcast);
	channel.access_hash = MTP_long(789); channel.title = MTP_string("Purple sync"); channel.date = MTP_int(100); channel.photo = MTP_chatPhotoEmpty();
	auto updates = NativeUpdates(); updates.users = MTP_vector<MTPUser>(); updates.updates = MTP_vector<MTPUpdate>();
	updates.chats = MTP_vector<MTPChat>({channel.Build()}); updates.date = MTP_int(100); updates.seq = MTP_int(1);
	auto words = mtpBuffer(); MTPUpdates(updates.Build()).write(words);
	return QByteArray(reinterpret_cast<const char*>(words.constData()), words.size() * sizeof(mtpPrime));
}

MTPMessage CleanupEmpty(int id, PeerToken peer) {
	return MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flag::f_peer_id), MTP_int(id), Target(peer));
}

DesktopSyncChatSetupSnapshot FollowFrame(ExecutorFiles &f) {
	auto frame = DesktopSyncChatSetupSnapshot();
	frame.scope.origin = f.fresh.target.peer.origin; frame.scope.root = f.temporary.path();
	frame.scope.binding = f.scope.binding; frame.scope.loginBinding = f.scope.loginAuthority;
	frame.scope.settingsBound = f.start.state.has_value();
	frame.scope.settingsChat = f.start.state ? f.start.state->chat : PeerToken{};
	frame.scope.space = f.start.state ? f.start.state->space : QString();
	frame.scope.mark = f.fresh.resolutionInput.cache;
	frame.input = f.fresh.resolutionInput;
	frame.status.complete = true; frame.status.phase = DesktopSyncChatPhase::Paused; frame.status.postingHeld = true;
	frame.status.resolution = ResolveSyncChat(frame.input);
	frame.stateImage = f.start.state ? SerializeSyncLocalState(*f.start.state).canonical : QByteArray();
	frame.run = 1;
	f.fresh.target.peer.facts->otherMembers = 1;
	f.fresh.target.peer.readerMembers = std::vector<QString>{QString(64, u'a')};
	f.fresh.consentPeer = f.fresh.target.peer;
	for (auto &mark : f.fresh.resolutionInput.marks) { mark.eligibility = *DesktopSyncChatEligibility(f.fresh.target.peer); }
	frame.input.marks = f.fresh.resolutionInput.marks;
	frame.status.resolution = ResolveSyncChat(frame.input);
	return frame;
}

// One persistent thread runs every worker task: the carry store binds to the thread that
// opened it, as production's dedicated store thread keeps it.
struct WorkerThread {
	std::deque<Fn<void()>> &work;
	std::mutex mutex;
	std::condition_variable condition;
	bool pending = false;
	bool stop = false;
	std::thread thread;

	explicit WorkerThread(std::deque<Fn<void()>> &work) : work(work), thread([this] { loop(); }) {}
	~WorkerThread() {
		{ const auto lock = std::lock_guard(mutex); stop = true; }
		condition.notify_all(); thread.join();
	}
	void loop() {
		auto lock = std::unique_lock(mutex);
		while (true) {
			condition.wait(lock, [&] { return pending || stop; });
			if (!pending) { return; }
			lock.unlock();
			while (!work.empty()) { auto task = std::move(work.front()); work.pop_front(); task(); }
			lock.lock(); pending = false; condition.notify_all();
		}
	}
	// The GUI thread waits, so the worker owns both queues while it drains.
	void run() {
		auto lock = std::unique_lock(mutex);
		pending = true; condition.notify_all();
		condition.wait(lock, [&] { return !pending; });
	}
};

// The owner's GUI side runs on the QCoreApplication thread.
struct OwnerHarness {
	ExecutorFiles files;
	DesktopSyncChatSetupSnapshot frame;
	std::deque<Fn<void()>> gui;
	std::deque<Fn<void()>> work;
	WorkerThread worker{work};
	std::unique_ptr<DesktopSyncChatCarryOwner> owner;
	std::set<DesktopSyncChatCarrySwitch> on = {DesktopSyncChatCarrySwitch::Move, DesktopSyncChatCarrySwitch::FollowFill,
		DesktopSyncChatCarrySwitch::MarkRemoval, DesktopSyncChatCarrySwitch::Cleanup, DesktopSyncChatCarrySwitch::Recovery};
	DesktopSyncChatCarryState state;
	std::vector<DesktopSyncChatCarryKind> settled;
	std::vector<QString> reasons;
	int cleanupPhaseStates = 0;
	int cleanupPendingStates = 0;
	bool live = true;
	bool transportReady = true;
	bool holdReply = false;
	bool dropMark = false;
	std::vector<std::pair<int64_t, Fn<void()>>> timers;
	bool provisionCreates = true;
	bool wrongThread = false;
	bool cleanupShape = false;
	bool badCleanupRead = false;
	std::set<int> removed;
	std::vector<DesktopSyncCarryCommand> commands; // as the transport received them
	int cleanupDeletes = 0;
	int cleanupApplied = 0;
	int cleanupReads = 0;
	Fn<void(DesktopSyncCarryReply)> heldReply;
	DesktopSyncCarryReply held;
	std::optional<DesktopSyncChatMarkPort> nativeMark;
	Fn<void()> onSettled;
	Fn<void(const DesktopSyncChatCarryState &)> onChanged;
	int resolves = 0;
	int inspections = 0;
	int provisions = 0;
	int quiesces = 0;
	int sends = 0;
	int transports = 0;
	int transportCancels = 0;
	int applied = 0;
	int leases = 0;
	int renewals = 0;
	int unrenewed = 0;
	bool holdEffects = false;
	Fn<void(Fn<void(bool)>)> pendingRenewal;
	std::unique_ptr<DesktopSyncChatSetupFlow> setup; // worker thread only
	DesktopSyncChatProvisionDone provided;

	explicit OwnerHarness(SyncCarryMode mode = SyncCarryMode::Switch, bool lost = false, bool cleanup = false, bool late = false)
	: files(late, mode), cleanupShape(cleanup) {
		files.engine.reset();
		if (late) {
			// An explicit old chat, not Saved Messages, whose settings post is this install's and newer than N's mark.
			files.own = false; files.sourceDate = 300;
			files.fresh.source.peer.facts->protectedContent = false;
		}
		if (cleanup) {
			// The cleanup test's shape: the old mark and the settings record are different messages.
			files.fresh.source.peer.facts->protectedContent = false;
			files.fresh.resolutionInput.cache = SyncChatCache{files.initial.chat, 9, 100};
			files.start.resolution = ResolveSyncChat(files.fresh.resolutionInput);
		}
		if (lost) {
			files.fresh.source.peer.readerMembers = std::vector<QString>{};
			files.fresh.target.peer.readerMembers = files.fresh.source.peer.readerMembers;
			files.fresh.resolutionInput.cacheRead = SyncChatDirectRead::MarkGone;
			files.start.oldRead = SyncChatDirectRead::MarkGone;
			files.start.resolution = ResolveSyncChat(files.fresh.resolutionInput);
		}
		frame = mode == SyncCarryMode::Follow || mode == SyncCarryMode::Fill ? FollowFrame(files) : OpeningFrame(files, lost);
		make();
	}
	~OwnerHarness() {
		owner.reset(); drain();
		work.push_back([this] { setup.reset(); }); worker.run();
	}

	DesktopSyncChatCarryHost host() {
		auto host = DesktopSyncChatCarryHost();
		host.activated = [this](DesktopSyncChatCarrySwitch value) { return on.contains(value); };
		host.current = [this] { if (SyncStoreWorkerThread()) { wrongThread = true; } return live; };
		host.scope = [this] { return std::optional(frame.scope); };
		host.worker = [this](Fn<void()> task) { work.push_back(std::move(task)); };
		host.delayedWorker = [this](int64_t after, Fn<void()> task) { timers.emplace_back(after, std::move(task)); };
		host.clock = [this] { CHECK(SyncStoreWorkerThread()); return files.clock; };
		host.main = [this](Fn<void()> task) { gui.push_back(std::move(task)); };
		host.directory = [this](const DesktopSyncChatDiscoveryScope &) { return std::optional(files.root); };
		host.lease = [this](const AccountKey &) { if (!SyncStoreWorkerThread()) { wrongThread = true; } ++leases; return files.gate->carry(); };
		host.quiesce = [this] { if (SyncStoreWorkerThread()) { wrongThread = true; } ++quiesces; };
		host.resolve = [this](Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)> done) {
			if (SyncStoreWorkerThread()) { wrongThread = true; }
			++resolves; done(frame);
		};
		host.inspect = [this](PeerToken chat, Fn<void(std::optional<DesktopSyncChatPeer>)> done) {
			if (SyncStoreWorkerThread()) { wrongThread = true; }
			++inspections;
			done(chat == files.fresh.target.peer.chat ? std::optional(files.fresh.target.peer)
				: chat == files.fresh.source.peer.chat ? std::optional(files.fresh.source.peer) : std::nullopt);
		};
		// The real provision-only setup flow on the worker, answered by a fake create transport.
		host.provision = [this](DesktopSyncChatMoveDraft draft, bool resume, DesktopSyncChatProvisionDone done) {
			if (SyncStoreWorkerThread()) { wrongThread = true; }
			++provisions;
			CHECK(draft.selection.provisionOnly && draft.selection.readerConsent && !draft.cleanup && !draft.removeOldMark);
			if (!provisionCreates) { done({}, {}); return; }
			provided = std::move(done);
			work.push_back([this, draft = std::move(draft), resume] {
				if (resume && setup) { setup->resume(1001); return; }
				setup = std::make_unique<DesktopSyncChatSetupFlow>(draft.opening, draft.selection, files.root + u"/provision"_q,
					files.temporary.path() + u"/state.toml"_q, files.admitted, [this](DesktopSyncChatSetupProgress progress) { setupProgress(std::move(progress)); });
				setup->open(1000);
			});
		};
		host.resumeOpening = [this](const DesktopSyncChatDiscoveryScope &scope) {
			if (!SyncStoreWorkerThread()) { wrongThread = true; }
			return DesktopSyncChatCarryResumeOpening(scope, files.root);
		};
		host.transport = [this](const DesktopSyncChatSetupSnapshot &opening, DesktopSyncChatCarryRenewal renewal, Fn<bool()> current) {
			if (SyncStoreWorkerThread()) { wrongThread = true; }
			++transports;
			CHECK(opening.scope.origin == frame.scope.origin && renewal.effect && renewal.beginCleanup && renewal.cleanup && current);
			auto transport = DesktopSyncCarryTransport();
			if (!transportReady) { return transport; }
			transport.send = [this, renewal](DesktopSyncCarryCommand command, Fn<void(DesktopSyncCarryReply)> done) {
				if (SyncStoreWorkerThread()) { wrongThread = true; }
				++sends; commands.push_back(command); renewEffects(renewal, std::move(command), 0, std::move(done));
			};
			transport.cancel = [this] { if (SyncStoreWorkerThread()) { wrongThread = true; } ++transportCancels; };
			transport.applyUpdates = [this](const MTPUpdates &) { if (SyncStoreWorkerThread()) { wrongThread = true; } ++applied; };
			transport.applyCleanup = [this](const DesktopSyncChatIo &, const MTPmessages_AffectedMessages &) {
				if (SyncStoreWorkerThread()) { wrongThread = true; }
				++cleanupApplied; return true;
			};
			return transport;
		};
		host.mark = [this](const DesktopSyncChatSetupSnapshot &, Fn<bool()>) {
			if (SyncStoreWorkerThread()) { wrongThread = true; }
			return nativeMark.value_or(DesktopSyncChatMarkPort());
		};
		host.changed = [this](const DesktopSyncChatCarryState &value) {
			state = value;
			if (!value.reason.isEmpty() && (reasons.empty() || reasons.back() != value.reason)) { reasons.push_back(value.reason); }
			if (value.carry && value.carry->journal && value.carry->journal->phase == SyncCarryPhase::Cleanup) { ++cleanupPhaseStates; }
			if (value.carry && value.carry->cleanup.pending) { ++cleanupPendingStates; }
			if (value.carry && (value.carry->journal || value.carry->command)) { files.progress = *value.carry; }
			if (onChanged) { const auto callback = onChanged; callback(value); }
		};
		host.settled = [this](DesktopSyncChatCarryKind kind) { settled.push_back(kind); if (onSettled) { onSettled(); } };
		return host;
	}
	void make() { owner = std::make_unique<DesktopSyncChatCarryOwner>(host()); }
	// The cleanup test's replies: the source's own settings record, its exact reads, and deletes.
	DesktopSyncCarryReply cleanupDelete(const DesktopSyncCarryCommand &command) {
		const auto permit = *command.cleanupPermit;
		CHECK(permit.current() && permit.request() && !permit.request());
		auto reply = DesktopSyncCarryReply(); reply.command = command; reply.outcome = SyncCarryOutcome::Success;
		reply.cleanupResult = MTP_messages_affectedMessages(MTP_int(7), MTP_int(1));
		++cleanupDeletes; removed.emplace(permit.item().reference.messageId);
		return reply;
	}
	void cleanupRead(DesktopSyncCarryReply &reply) {
		reply.source = InventoryRows(files.fresh.source, {Receipt(files.fresh.source, files.original, 10, files.sourceDate)});
		if (!reply.command.cleanupRead) { return; }
		const auto &item = reply.command.cleanupRead->item;
		++cleanupReads;
		if (std::exchange(badCleanupRead, false)) {
			reply.cleanupResponse = Response(files.fresh.source, {});
		} else if (removed.contains(item.reference.messageId)) {
			reply.cleanupResponse = Response(files.fresh.source, {CleanupEmpty(item.reference.messageId, item.reference.chat)});
		} else {
			auto record = Receipt(files.fresh.source, files.original, item.reference.messageId, item.date);
			reply.cleanupResponse = record.response; reply.fresh->sourceRecord = record;
		}
	}
	// T5K's contract: before each effect, the first one included, the transport reads fresh facts
	// and asks the renewal hook for a current permit. With none, or an incompatible one, the
	// command ends Unknown and nothing is sent.
	void renewEffects(DesktopSyncChatCarryRenewal renewal, DesktopSyncCarryCommand command, size_t index,
			Fn<void(DesktopSyncCarryReply)> done) {
		if (!command.dispatch || command.cleanupPermit || index == command.effects.size()) { answer(std::move(command), std::move(done)); return; }
		auto read = command; read.dispatch = false; read.effects.clear();
		files.progress.command = read;
		auto gathered = files.reply(); Must(gathered.fresh && gathered.source);
		auto fresh = *gathered.fresh;
		const auto permit = command.effects[index];
		if (const auto id = permit.sourceMessageId(); id > 0) {
			const auto &receipts = gathered.source->receipts;
			const auto found = std::find_if(receipts.begin(), receipts.end(), [&](const auto &receipt) { return receipt.record.id == id; });
			if (found == receipts.end()) { unknown(std::move(command), std::move(done)); return; }
			fresh.sourceRecord = *found;
		}
		if (holdEffects) {
			pendingRenewal = [=](Fn<void(bool)> report) {
				renewal.effect(command, permit, fresh, [report](std::optional<DesktopSyncCarryPermit> renewed) { report(renewed.has_value()); });
			};
			return;
		}
		++renewals;
		renewal.effect(command, permit, fresh, [=, this](std::optional<DesktopSyncCarryPermit> renewed) mutable {
			if (SyncStoreWorkerThread()) { wrongThread = true; }
			if (!renewed || !permit.compatible(*renewed)) { unknown(std::move(command), std::move(done)); return; }
			command.effects[index] = *renewed;
			renewEffects(renewal, std::move(command), index + 1, std::move(done));
		});
	}
	void unknown(DesktopSyncCarryCommand command, Fn<void(DesktopSyncCarryReply)> done) {
		++unrenewed;
		auto reply = DesktopSyncCarryReply(); reply.command = std::move(command); reply.outcome = SyncCarryOutcome::Unknown;
		done(std::move(reply));
	}
	void answer(DesktopSyncCarryCommand command, Fn<void(DesktopSyncCarryReply)> done) {
		files.progress.command = command;
		auto reply = DesktopSyncCarryReply();
		if (dropMark && command.dispatch && command.action == SyncCarryJournalAction::PostMark) {
			reply.command = command;
		} else { reply = command.dispatch && command.cleanupPermit ? cleanupDelete(command) : files.reply(); }
		if (cleanupShape && !command.cleanupPermit) { cleanupRead(reply); }
		// The fixture forges a lost mark's receipt with its own start's request id; an owner
		// move draws a random one, so stamp the journal's.
		if (command.action == SyncCarryJournalAction::ReadMark && files.unknownMark && reply.updates.size() == 1 && files.progress.journal) {
			reply.updates = {Receipt(files.fresh.target, DesktopSyncCarryTransportId(files.progress.journal->markRequestId).value_or(0), reply.markId)};
		}
		if (holdReply) { heldReply = std::move(done); held = std::move(reply); } else { done(std::move(reply)); }
	}
	void setupProgress(DesktopSyncChatSetupProgress progress) {
		if (!SyncStoreWorkerThread()) { wrongThread = true; }
		if (progress.provisioned && progress.journal) {
			gui.push_back([this, journal = progress.journal] { if (provided) { provided(journal, files.fresh.target.peer); } });
			return;
		}
		if (!progress.command) { return; }
		work.push_back([this, command = *progress.command] {
			if (!setup) { return; }
			auto reply = DesktopSyncChatCreateReply{command.origin, command.operation, command.ticket}; reply.outcome = DesktopSyncChatCreateOutcome::Okay;
			if (command.step == DesktopSyncChatCreateStep::Resolve) { reply.resolution = frame; }
			if (command.step == DesktopSyncChatCreateStep::Create) { reply.updates = CreatedUpdates(); reply.creationProof = DesktopSyncChatCreationProof::MatchedRequest; }
			if (command.step == DesktopSyncChatCreateStep::PeerInfo) { reply.peer = files.fresh.target.peer; }
			if (command.step == DesktopSyncChatCreateStep::Inventory) { reply.inventory = InventoryRows(files.fresh.target, {}); }
			setup->complete(reply, 1000);
		});
	}
	void reconcile() {
		for (auto i = 0; i != 6 && !state.finished; ++i) {
			if (state.reason == u"carry-retry-required"_q) {
				owner->retry(); drain();
			} else if (state.carry && state.carry->markRecoveryAfter) {
				files.clock += *state.carry->markRecoveryAfter; owner->retry(); drain();
			} else {
				return;
			}
		}
	}
	bool unchanged() const {
		return CaptureSyncPrivateFile(files.root + u"/state.json"_q, kSyncCarryJournalStorageLimit).bytes == frame.stateImage
			&& SyncCarryStartupAdmission(files.root, files.scope.binding) == SyncStoreStatus::Ready;
	}
	bool journal() const { return SyncCarryStartupAdmission(files.root, files.scope.binding) != SyncStoreStatus::Ready; }
	void drain(int limit = 4000) {
		for (auto i = 0; i != limit && (!gui.empty() || !work.empty()); ++i) {
			if (!gui.empty()) {
				auto task = std::move(gui.front()); gui.pop_front(); task();
			} else {
				worker.run();
			}
		}
		CHECK(gui.empty() && work.empty());
	}
	DesktopSyncChatMoveDraft draft(bool create = false, bool cleanup = false) {
		auto selection = DesktopSyncChatSetupSelection{create ? DesktopSyncChatSetupChoice::Create : DesktopSyncChatSetupChoice::Existing,
			create ? std::nullopt : std::optional(files.fresh.target.peer), true, create};
		return DesktopSyncChatMoveDraft{frame, selection, cleanup, false};
	}
	bool done() const {
		return state.finished && state.carry && state.carry->journal && state.carry->journal->phase == SyncCarryPhase::Done;
	}
	bool followFinished() const {
		return state.finished && state.carry && state.carry->journal
			&& state.carry->journal->phase == SyncCarryPhase::FinishedForFollow;
	}
	bool moved() const {
		const auto cache = ParseDesktopSyncChatCache(CaptureSyncPrivateFile(files.scope.cacheFile, 64 * 1024).bytes, files.scope.account);
		auto store = SyncLocalStore(files.root);
		return cache.record && !cache.record->carry && cache.record->mark.chat == files.fresh.target.peer.chat
			&& store.Open(true, files.scope.binding) && store.state()->chat == files.fresh.target.peer.chat;
	}
};

void Moves() {
	for (const auto create : {false, true}) {
		auto h = OwnerHarness();
		CHECK(DesktopSyncChatMoveFingerprint(h.frame).size() > 0);
		CHECK(h.owner->move(h.draft(create), u"window-A"_q));
		CHECK(h.owner->busy() && !h.owner->move(h.draft(create), u"window-A"_q));
		h.drain();
		if (!h.done()) { std::fprintf(stderr, "Move hold create=%d: %s\n", int(create), qPrintable(h.state.reason)); }
		CHECK(h.done() && h.moved());
		CHECK(h.provisions == (create ? 1 : 0) && h.state.provisioned == create);
		CHECK(h.resolves == (create ? 3 : 2) && h.inspections == 2 && h.quiesces == 1 && h.transports == 1 && h.leases == 1);
		CHECK(h.files.markWrites == 1 && h.sends > 0 && h.applied > 0 && h.renewals > 0 && h.unrenewed == 0);
		CHECK(!h.files.gate->postingHeld() && SyncCarryStartupAdmission(h.files.root, h.files.scope.binding) == SyncStoreStatus::Ready);
		CHECK(h.settled == std::vector{DesktopSyncChatCarryKind::Move} && h.transportCancels == 1 && !h.owner->busy());
		CHECK(!h.wrongThread);
	}
	{
		auto h = OwnerHarness(); h.provisionCreates = false;
		CHECK(h.owner->move(h.draft(true), u"window-A"_q)); h.drain();
		CHECK(!h.state.finished && h.state.reason == u"move-provision-held"_q && h.leases == 0 && h.sends == 0);
		h.provisionCreates = true; h.owner->retry(); h.drain();
		CHECK(h.done() && h.provisions == 2);
	}
}

// Cleanup ON deletes the old chat's own settings record through the transport and applies the
// result once; Cleanup OFF moves without any cleanup command. A malformed read leaves cleanup
// pending: Skip needs confirmation, Retry reads again.
void Cleanup() {
	for (const auto activated : {true, false}) {
		auto h = OwnerHarness(SyncCarryMode::Switch, false, true);
		if (!activated) { h.on.erase(DesktopSyncChatCarrySwitch::Cleanup); }
		CHECK(h.owner->move(h.draft(false, true), u"window-A"_q)); h.drain(); h.reconcile();
		if (!h.done()) { std::fprintf(stderr, "Cleanup hold activated=%d: %s\n", int(activated), qPrintable(h.state.reason)); }
		CHECK(h.done() && h.moved() && !h.wrongThread);
		CHECK(h.cleanupDeletes == (activated ? 1 : 0) && h.cleanupApplied == h.cleanupDeletes);
		CHECK((h.cleanupReads > 0) == activated);
	}
	for (const auto skip : {true, false}) {
		auto h = OwnerHarness(SyncCarryMode::Switch, false, true); h.badCleanupRead = true;
		CHECK(h.owner->move(h.draft(false, true), u"window-A"_q)); h.drain();
		const auto latest = h.owner->latest();
		CHECK(h.owner->busy() && latest && latest->cleanup.pending && latest->cleanup.canAct && !latest->terminal);
		h.owner->skipCleanup(false); h.drain(); CHECK(h.owner->busy() && h.cleanupDeletes == 0);
		if (skip) {
			h.owner->skipCleanup(true); h.drain();
			CHECK(h.state.finished && h.state.carry->cleanup.skipped && h.state.carry->journal->phase == SyncCarryPhase::CleanupSkipped);
			CHECK(h.cleanupDeletes == 0 && !h.files.gate->postingHeld());
		} else {
			h.owner->retry(); h.drain();
			CHECK(h.done() && h.cleanupDeletes == 1 && h.cleanupApplied == 1);
		}
		CHECK(!h.owner->busy() && !h.wrongThread);
	}
}

void MoveRefusals() {
	{
		auto h = OwnerHarness(); h.on.erase(DesktopSyncChatCarrySwitch::Move);
		CHECK(!h.owner->move(h.draft(), u"window-A"_q)); h.drain();
		CHECK(h.resolves == 0 && h.leases == 0 && h.sends == 0 && h.transports == 0 && !h.state.running);
	}
	{
		auto h = OwnerHarness(); h.transportReady = false;
		CHECK(h.owner->move(h.draft(), u"window-A"_q)); h.drain();
		CHECK(h.state.reason == u"carry-transport-held"_q && !h.state.running && h.leases == 0 && h.quiesces == 0);
		CHECK(SyncCarryStartupAdmission(h.files.root, h.files.scope.binding) == SyncStoreStatus::Ready);
		h.owner->close(u"closed"_q); CHECK(h.settled.size() == 1 && !h.owner->busy());
	}
	{
		auto h = OwnerHarness(); auto changed = h.frame; changed.stateImage += ' ';
		auto stale = h.draft(); stale.opening = changed;
		CHECK(h.owner->move(stale, u"window-A"_q)); h.drain();
		CHECK(h.state.reason == u"move-opening-changed"_q && h.leases == 0);
	}
	{
		auto h = OwnerHarness(); CHECK(!h.owner->move(h.draft(), QString())); h.live = false;
		CHECK(!h.owner->move(h.draft(), u"window-A"_q)); h.drain(); CHECK(h.resolves == 0);
	}
}

// Simulates a process that died mid-move: the fixture's own engine opens the journal on the
// worker thread and stops after `steps` replies without cancelling it.
bool Crash(OwnerHarness &h, int steps) {
	auto terminal = false;
	h.work.push_back([&] {
		h.files.newEngine(); h.files.engine->open(h.files.start);
		for (auto i = 0; i != steps && h.files.progress.command && !h.files.progress.terminal; ++i) { h.files.step(); }
		terminal = h.files.progress.terminal;
		h.files.engine.reset();
	});
	h.worker.run();
	return terminal;
}

void Revocation() {
	{
		auto h = OwnerHarness(); h.holdReply = true;
		CHECK(h.owner->move(h.draft(), u"window-A"_q)); h.drain();
		Must(h.heldReply); CHECK(h.files.gate->postingHeld() && h.journal());
		h.owner->closeWindow(u"window-B"_q, u"other-window"_q); CHECK(h.owner->busy());
		h.owner->closeWindow(u"window-A"_q, u"window-closed"_q); h.drain();
		CHECK(!h.owner->busy() && h.state.reason == u"window-closed"_q && h.transportCancels == 1 && !h.files.gate->postingHeld());
		CHECK(h.settled == std::vector{DesktopSyncChatCarryKind::Move});
		// Nothing was sent outward, so closing cancels the journal and leaves the old chat bound.
		CHECK(h.unchanged() && h.files.markWrites == 0 && h.files.cloud.empty());
		const auto before = h.sends;
		auto done = std::move(h.heldReply); done(std::move(h.held)); h.drain();
		CHECK(h.sends == before && h.state.reason == u"window-closed"_q && h.settled.size() == 1);
		h.holdReply = false;
		CHECK(CaptureSyncPrivateFile(h.files.scope.cacheFile, 64 * 1024).bytes == h.frame.cacheImage);
		CHECK(h.owner->move(h.draft(), u"window-A"_q)); h.drain();
		CHECK(h.done() && h.moved());
	}
	{
		auto h = OwnerHarness(); h.live = false;
		CHECK(!h.owner->resume() && !h.owner->busy() && h.resolves == 0);
	}
	// A closed move's reply arrives behind the next move's identical first step: it changes nothing.
	{
		auto h = OwnerHarness(); h.holdReply = true;
		CHECK(h.owner->move(h.draft(), u"window-A"_q)); h.drain(); Must(h.heldReply);
		auto stale = std::move(h.heldReply); auto staleReply = std::move(h.held); h.heldReply = nullptr;
		h.owner->closeWindow(u"window-A"_q, u"window-closed"_q); h.drain();
		CHECK(h.owner->move(h.draft(), u"window-A"_q)); h.drain(); Must(h.heldReply);
		CHECK(h.held.command.action == staleReply.command.action);
		const auto generation = h.state.generation;
		const auto sends = h.sends;
		stale(std::move(staleReply)); h.drain();
		CHECK(h.owner->busy() && h.state.generation == generation && h.sends == sends && !h.state.finished);
		h.holdReply = false;
		auto done = std::move(h.heldReply); h.heldReply = nullptr; done(std::move(h.held)); h.drain(); h.reconcile();
		CHECK(h.done() && h.moved() && h.files.markWrites == 1 && !h.wrongThread);
	}
	// A settle callback may start the next operation at once; the closed one's queued work is fenced.
	{
		auto h = OwnerHarness(); auto restarted = false;
		h.onSettled = [&] {
			if (!std::exchange(restarted, true)) { CHECK(!h.owner->busy() && h.owner->move(h.draft(), u"window-B"_q)); }
		};
		CHECK(h.owner->move(h.draft(), u"window-A"_q));
		h.owner->closeWindow(u"window-A"_q, u"window-closed"_q); h.drain(); h.reconcile();
		CHECK(restarted && h.done() && h.moved() && h.state.window == u"window-B"_q && h.files.markWrites == 1);
	}
}

void Restart() {
	// Crash at every reply boundary, then the startup resume finishes the move.
	for (auto steps = 0; steps != 16; ++steps) {
		auto h = OwnerHarness();
		if (Crash(h, steps)) { continue; }
		Must(h.journal());
		CHECK(h.owner->resume()); h.drain(); h.reconcile();
		if (h.state.reason == u"cancelled"_q) { CHECK(!h.files.gate->postingHeld()); continue; }
		if (!h.done() && !h.followFinished()) { std::fprintf(stderr, "Restart hold steps=%d: %s\n", steps, qPrintable(h.state.reason)); }
		CHECK((h.done() || h.followFinished()) && h.moved() && h.files.markWrites <= 1 && !h.files.gate->postingHeld());
		CHECK(!h.settled.empty() && h.settled.back() == DesktopSyncChatCarryKind::Resume && h.leases >= 1 && !h.wrongThread);
	}
	// The app quits mid-move: the owner is destroyed and cancels, as every carry controller does.
	// Core accepts the cancel only with no request in flight: the old chat stays bound and a
	// copy already delivered stays in the target. Otherwise the journal stays, and the next
	// startup finishes it.
	for (auto boundary = 1; boundary != 14; ++boundary) {
		auto h = OwnerHarness(); h.holdReply = true;
		CHECK(h.owner->move(h.draft(), u"window-A"_q)); h.drain();
		for (auto i = 0; i != boundary && h.heldReply; ++i) {
			auto done = std::move(h.heldReply); h.heldReply = nullptr; done(std::move(h.held)); h.drain();
		}
		if (h.state.finished) { CHECK(h.done()); continue; }
		h.owner.reset(); h.drain(); CHECK(!h.files.gate->postingHeld());
		if (auto done = std::move(h.heldReply)) { done(std::move(h.held)); h.drain(); }
		if (!h.journal()) {
			CHECK(h.unchanged() && h.files.markWrites == 0 && h.files.cloud.size() <= 1);
			continue;
		}
		h.holdReply = false; h.make();
		CHECK(h.owner->resume()); h.drain(); h.reconcile();
		if (h.state.reason == u"cancelled"_q) { CHECK(!h.files.gate->postingHeld()); continue; }
		if (!h.done() && !h.followFinished()) { std::fprintf(stderr, "Quit hold boundary=%d: %s\n", boundary, qPrintable(h.state.reason)); }
		CHECK((h.done() || h.followFinished()) && h.moved() && h.files.markWrites <= 1);
	}
	{
		auto h = OwnerHarness(); CHECK(h.owner->resume()); h.drain();
		CHECK(h.state.finished && h.state.carry && h.state.carry->absent && h.leases == 0 && h.transports == 0);
		CHECK(h.settled == std::vector{DesktopSyncChatCarryKind::Resume});
	}
	// An unreadable journal is not absent: the resume ends held, creates and sends nothing, and
	// does not run again by itself.
	{
		auto h = OwnerHarness(); CHECK(!Crash(h, 2)); Must(h.journal());
		Must(WriteSyncPrivateFile(h.files.root + u"/carry.json"_q, QByteArray("{\"version\":")) == SyncStoreStatus::Ready);
		CHECK(h.owner->resume()); h.drain();
		CHECK(h.state.finished && h.state.reason == u"carry-journal-unreadable"_q && h.state.carry && !h.state.carry->absent);
		CHECK(h.leases == 0 && h.transports == 0 && h.sends == 0 && h.journal() && !h.owner->busy());
		CHECK(h.settled == std::vector{DesktopSyncChatCarryKind::Resume});
	}
	{
		auto h = OwnerHarness(); CHECK(!Crash(h, 2)); Must(h.journal());
		h.on = {DesktopSyncChatCarrySwitch::FollowFill}; h.make();
		CHECK(h.owner->resume()); h.drain();
		CHECK(h.state.finished && h.state.reason == u"carry-resume-held"_q && h.leases == 0 && h.transports == 0 && h.journal());
		h.on.clear(); h.make(); CHECK(!h.owner->resume());
		h.on = {DesktopSyncChatCarrySwitch::Recovery}; h.make(); CHECK(h.owner->resume()); h.drain(); h.reconcile();
		CHECK((h.done() || h.followFinished()) && h.moved());
	}
}

void RecoveryTimers() {
	{
		auto h = OwnerHarness(); h.dropMark = true;
		h.onChanged = [&](const DesktopSyncChatCarryState &value) {
			if (value.reason != u"mark-recovery-waiting"_q) { return; }
			h.onChanged = nullptr;
			h.owner->close(u"timer-reentry"_q);
		};
		CHECK(h.owner->move(h.draft(), u"window-A"_q)); h.drain();
		CHECK(h.state.finished && !h.owner->busy() && h.timers.empty() && !h.files.gate->postingHeld());
		CHECK(h.owner->resume(u"window-B"_q)); h.drain();
		CHECK(h.state.carry && h.state.carry->markRecoveryAfter && !h.timers.empty());
	}
	auto h = OwnerHarness(); h.dropMark = true;
	CHECK(h.owner->move(h.draft(), u"window-A"_q)); h.drain();
	Must(!h.timers.empty() && h.files.progress.markRecoveryAfter);
	CHECK(h.timers.back().first == 30000 && MarkRecoveryImage(h.files).value(u"checks"_q).toInt() == 1);
	const auto stale = h.timers.back().second;
	h.holdReply = true; h.owner->retry(); h.drain(); Must(h.heldReply);
	const auto inFlight = h.sends;
	h.work.push_back(stale); h.drain();
	CHECK(h.sends == inFlight && h.heldReply);
	h.holdReply = false;
	auto done = std::move(h.heldReply); h.heldReply = nullptr; done(std::move(h.held)); h.drain();
	CHECK(MarkRecoveryImage(h.files).value(u"checks"_q).toInt() == 1);
	const auto sends = h.sends;
	h.work.push_back(stale); h.drain();
	CHECK(h.sends == sends && MarkRecoveryImage(h.files).value(u"checks"_q).toInt() == 1);
	h.files.clock += 30000;
	h.work.push_back(h.timers.back().second); h.drain();
	CHECK(MarkRecoveryImage(h.files).value(u"checks"_q).toInt() == 2 && h.timers.back().first == 120000);
	const auto oldOwnerTimer = h.timers.back().second;
	h.live = false; h.files.clock += 120000;
	h.work.push_back(oldOwnerTimer); h.drain();
	CHECK(MarkRecoveryImage(h.files).value(u"checks"_q).toInt() == 2);
	h.owner.reset(); h.drain(); CHECK(!h.files.gate->postingHeld());
	h.live = true; h.make(); CHECK(h.owner->resume(u"window-B"_q)); h.drain();
	CHECK(MarkRecoveryImage(h.files).value(u"checks"_q).toInt() == 3 && h.timers.back().first == 600000);
	const auto restartedSends = h.sends;
	h.work.push_back(oldOwnerTimer); h.drain();
	CHECK(h.sends == restartedSends && MarkRecoveryImage(h.files).value(u"checks"_q).toInt() == 3);
	const auto settled = h.settled.size();
	h.files.clock += 600000;
	h.work.push_back(h.timers.back().second); h.drain();
	Must(h.state.carry && h.state.carry->journal);
	CHECK(h.state.finished && !h.owner->busy() && h.state.carry->journal->phase == SyncCarryPhase::FinishedForFollow);
	CHECK(h.settled.size() == settled + 1 && h.files.markWrites == 0 && h.cleanupDeletes == 0);
	CHECK(!h.files.gate->postingHeld() && !h.wrongThread);
}

void FollowAndPause() {
	auto renewals = 0;
	for (const auto mode : {SyncCarryMode::Follow, SyncCarryMode::Fill}) {
		auto h = OwnerHarness(mode);
		auto action = DesktopSyncChatConsentAction(h.frame, u"window-A"_q, [] { return true; }, [] { return true; });
		const auto offer = action.offer(h.files.fresh.target.peer, u"config"_q); Must(offer);
		const auto selection = action.accept(*offer, h.frame, h.files.fresh.target.peer, DesktopSyncChatConsentChoice::Proceed, true); Must(selection);
		const auto start = DesktopSyncChatFollowStart(*selection); Must(start);
		CHECK(start->mode == mode && start->target == h.files.fresh.target.peer.chat && start->markRequestId.isEmpty());
		h.on.erase(DesktopSyncChatCarrySwitch::FollowFill); CHECK(!h.owner->follow(*selection));
		h.on.insert(DesktopSyncChatCarrySwitch::FollowFill);
		CHECK(h.owner->follow(*selection)); h.drain();
		if (!h.done()) { std::fprintf(stderr, "Follow hold mode=%d: %s\n", int(mode), qPrintable(h.state.reason)); }
		CHECK(h.done() && h.resolves == 2 && h.inspections == 2 && h.files.markWrites == 0 && !h.files.gate->postingHeld());
		CHECK(h.settled == std::vector{DesktopSyncChatCarryKind::Follow} && !h.wrongThread && h.unrenewed == 0);
		renewals += h.renewals;
	}
	std::printf("FOLLOW renewals=%d\n", renewals);
	CHECK(renewals > 0);
	{
		auto h = OwnerHarness(SyncCarryMode::Follow);
		auto action = DesktopSyncChatConsentAction(h.frame, u"window-A"_q, [] { return true; }, [] { return true; });
		const auto offer = action.offer(h.files.fresh.target.peer, u"config"_q); Must(offer);
		const auto selection = action.accept(*offer, h.frame, h.files.fresh.target.peer, DesktopSyncChatConsentChoice::Pause, true); Must(selection);
		CHECK(h.owner->follow(*selection)); h.drain();
		CHECK(h.state.finished && h.state.reason == u"sync-paused-here"_q && h.sends == 0 && h.leases == 0);
		const auto parsed = ParseDesktopSyncChatCache(CaptureSyncPrivateFile(h.files.scope.cacheFile, 65536).bytes, Origin.account);
		CHECK(parsed.paused);
	}
	{
		auto h = OwnerHarness(SyncCarryMode::Follow); h.transportReady = false;
		auto action = DesktopSyncChatConsentAction(h.frame, u"window-A"_q, [] { return true; }, [] { return true; });
		const auto offer = action.offer(h.files.fresh.target.peer, u"config"_q); Must(offer);
		const auto selection = action.accept(*offer, h.frame, h.files.fresh.target.peer, DesktopSyncChatConsentChoice::Proceed, true); Must(selection);
		CHECK(h.owner->follow(*selection)); h.drain();
		CHECK(h.state.finished && h.state.reason == u"carry-transport-held"_q && h.resolves == 0 && h.leases == 0);
	}
}

// A Follow from the late-post fixture cleans up only under the Cleanup the owner admitted it with:
// ON deletes exactly that copied post; OFF plans no cleanup stage, reads and deletes nothing and
// leaves nothing pending, so a held switch never strands the run in cleanup.
void LateFollow(bool held) {
	auto h = OwnerHarness(SyncCarryMode::Follow, false, true, true);
	if (held) { h.on.erase(DesktopSyncChatCarrySwitch::Cleanup); }
	const auto cleanup = h.on.contains(DesktopSyncChatCarrySwitch::Cleanup);
	CHECK(h.files.initial.chat != PeerToken{} && h.files.fresh.source.peer.chat == h.files.initial.chat);
	auto action = DesktopSyncChatConsentAction(h.frame, u"window-A"_q, [] { return true; }, [] { return true; });
	const auto offer = action.offer(h.files.fresh.target.peer, u"config"_q); Must(offer);
	const auto selection = action.accept(*offer, h.frame, h.files.fresh.target.peer, DesktopSyncChatConsentChoice::Proceed, true); Must(selection);
	CHECK(h.owner->follow(*selection)); h.drain();
	if (!h.done()) { std::fprintf(stderr, "Late Follow cleanup=%d: %s\n", int(cleanup), qPrintable(h.state.reason)); }
	CHECK(h.done() && !h.owner->busy() && !h.wrongThread && h.unrenewed == 0);
	CHECK(std::find(h.reasons.begin(), h.reasons.end(), u"cleanup-held"_q) == h.reasons.end());
	if (!h.done()) { return; }
	const auto &journal = *h.state.carry->journal;
	CHECK(journal.plan && journal.plan->cleanup == (cleanup ? SyncCarryCleanup::OwnLatePosts : SyncCarryCleanup::None));
	CHECK(!h.state.carry->cleanup.pending && !h.state.carry->cleanup.skipped);
	CHECK(!h.owner->latest() || !h.owner->latest()->cleanup.canAct);
	// Both runs pass the core's Cleanup phase; only the one with a cleanup stage ever reports it pending.
	CHECK(h.cleanupPhaseStates > 0 && (h.cleanupPendingStates > 0) == cleanup);
	if (cleanup) {
		CHECK(h.cleanupReads > 0 && h.cleanupDeletes == 1 && h.cleanupApplied == 1 && h.removed == std::set<int>{10});
	} else {
		CHECK(h.cleanupReads == 0 && h.cleanupDeletes == 0 && h.cleanupApplied == 0 && h.removed.empty());
	}
	// The transport guard is unchanged: under Cleanup OFF it refuses exactly the cleanup commands.
	auto removals = 0;
	for (const auto &command : h.commands) {
		const auto removal = bool(command.cleanupPermit || command.cleanupRead);
		removals += int(removal);
		CHECK(DesktopSyncChatCarryCommandAllowed(command, [&](DesktopSyncChatCarrySwitch value) { return h.on.contains(value); }));
		CHECK(DesktopSyncChatCarryCommandAllowed(command, [](DesktopSyncChatCarrySwitch value) {
			return value != DesktopSyncChatCarrySwitch::Cleanup;
		}) == !removal);
	}
	CHECK(!h.commands.empty() && (removals > 0) == cleanup);
	std::printf("LATE FOLLOW cleanup=%d reads=%d deletes=%d\n", int(cleanup), h.cleanupReads, h.cleanupDeletes);
}

void Recovery() {
	auto h = OwnerHarness(SyncCarryMode::RepairMark, true);
	CHECK(DesktopSyncChatRecoveryHeld(h.frame).isEmpty());
	h.files.fresh.target = h.files.fresh.source;
	h.files.fresh.target.peer.readerMembers = h.files.fresh.source.peer.readerMembers;
	auto selection = DesktopSyncChatRecoverySelection{h.frame, DesktopSyncChatRecoveryChoice::RepairMark,
		h.files.fresh.source.peer, u"window-A"_q, DesktopSyncChatMoveFingerprint(h.frame)};
	h.on.erase(DesktopSyncChatCarrySwitch::Recovery); CHECK(!h.owner->recover(selection));
	h.on.insert(DesktopSyncChatCarrySwitch::Recovery);
	CHECK(h.owner->recover(selection)); h.drain();
	if (!h.done()) { std::fprintf(stderr, "Recovery hold: %s\n", qPrintable(h.state.reason)); }
	CHECK(h.done() && h.files.markWrites == 1 && h.files.cloud.size() == 1 && !h.files.gate->postingHeld());
	CHECK(h.settled == std::vector{DesktopSyncChatCarryKind::Recovery} && !h.wrongThread);
	// The replacement mark is an effect: the recovery flow's own engine renewed it.
	CHECK(h.renewals > 0 && h.unrenewed == 0);
	// A renewal in flight when its window closes answers empty on the far side of the worker hop.
	for (const auto close : {false, true}) {
		auto r = OwnerHarness(SyncCarryMode::RepairMark, true); r.holdEffects = true;
		r.files.fresh.target = r.files.fresh.source;
		auto repair = DesktopSyncChatRecoverySelection{r.frame, DesktopSyncChatRecoveryChoice::RepairMark,
			r.files.fresh.source.peer, u"window-A"_q, DesktopSyncChatMoveFingerprint(r.frame)};
		CHECK(r.owner->recover(repair)); r.drain(); Must(r.pendingRenewal);
		auto answered = std::optional<bool>();
		r.pendingRenewal([&](bool renewed) { answered = renewed; });
		if (close) { r.owner->closeWindow(u"window-A"_q, u"window-closed"_q); }
		r.drain();
		CHECK(answered.has_value() && *answered == !close && !r.wrongThread);
	}
}

void Marks() {
	{
		auto h = OwnerHarness(); const auto route = h.files.fresh.source;
		const auto row = CaptureDesktopSyncChatStaleMark(route.peer, h.files.scope.loginAuthority, CarryMark(route, DesktopSyncChatMarkText(), 10)); Must(row);
		CHECK(h.owner->removeMark(*row, u"window-A"_q, h.frame)); h.drain();
		CHECK(h.state.finished && h.state.reason == u"mark-transport-held"_q && h.leases == 0 && h.resolves == 0);
	}
	auto h = OwnerHarness();
	CHECK(h.owner->move(h.draft(), u"window-A"_q)); h.drain(); Must(h.done());
	h.frame = MovedFrame(h.files);
	const auto route = h.files.fresh.source;
	auto message = NativeMessage();
	message.flags = MTP_flags(MTPDmessage::Flag::f_out | MTPDmessage::Flag::f_from_id);
	message.id = MTP_int(10); message.date = MTP_int(100);
	message.peer_id = Target(route.peer.chat); message.from_id = MTP_peerUser(MTP_long(Origin.account.userId));
	message.message = MTP_string(DesktopSyncChatMarkText());
	const auto row = CaptureDesktopSyncChatStaleMark(route.peer, h.files.scope.loginAuthority, message.Build());
	Must(row);
	const auto opening = h.frame;
	auto reads = 0, cancels = 0;
	auto native = DesktopSyncChatMarkPort();
	native.exact = [&](const DesktopSyncChatStaleMark &selected, Fn<void(std::optional<DesktopSyncChatMarkExact>)> done) {
		CHECK(!SyncStoreWorkerThread() && selected == *row); ++reads;
		done(DesktopSyncChatMarkExact{route, Response(route, {MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flags()), MTP_int(10), MTPPeer())}), true});
	};
	native.send = [&](DesktopSyncChatMarkPermit, DesktopSyncChatMarkRenewal, Fn<void(std::optional<MTPmessages_AffectedMessages>)>) { CHECK(false); };
	native.apply = [&](const DesktopSyncChatIo &, const MTPmessages_AffectedMessages &, Fn<void(bool)>) { CHECK(false); };
	native.cancel = [&] { CHECK(!SyncStoreWorkerThread()); ++cancels; };
	h.nativeMark = native;
	h.on.erase(DesktopSyncChatCarrySwitch::MarkRemoval); CHECK(!h.owner->removeMark(*row, u"window-A"_q, opening));
	h.on.insert(DesktopSyncChatCarrySwitch::MarkRemoval);
	auto foreign = *row; foreign.login = QString(64, u'c'); CHECK(!h.owner->removeMark(foreign, u"window-A"_q, opening));
	CHECK(h.owner->removeMark(*row, u"window-A"_q, opening)); h.drain();
	if (h.state.reason != u"mark-already-absent"_q) { std::fprintf(stderr, "Mark hold: %s\n", qPrintable(h.state.reason)); }
	CHECK(h.state.finished && h.state.removed && h.state.reason == u"mark-already-absent"_q && reads == 1 && cancels == 1);
	CHECK(!h.settled.empty() && h.settled.back() == DesktopSyncChatCarryKind::Mark && !h.files.gate->postingHeld() && !h.wrongThread);
}

void Builders() {
	auto h = OwnerHarness();
	const auto move = DesktopSyncChatMoveStart(h.draft(), h.files.fresh.target.peer, true); Must(move);
	CHECK(move->mode == SyncCarryMode::Switch && move->source == h.frame.scope.settingsChat && move->target == h.files.fresh.target.peer.chat);
	CHECK(move->readerConsentAccepted && move->operationConfirmed && move->markText == DesktopSyncChatMarkText());
	CHECK(!move->markRequestId.isEmpty() && move->markRequestId != move->ownRequestId && move->state);
	auto removing = h.draft(); removing.removeOldMark = true;
	CHECK(DesktopSyncChatMoveStart(removing, h.files.fresh.target.peer, true)->removeOldMarkAuthorized);
	CHECK(!DesktopSyncChatMoveStart(removing, h.files.fresh.target.peer, false)->removeOldMarkAuthorized);
	auto unconsented = h.draft(); unconsented.selection.readerConsent = false;
	CHECK(!DesktopSyncChatMoveStart(unconsented, h.files.fresh.target.peer, true));
	auto other = h.files.fresh.target.peer; other.chat = {PeerTokenKind::Channel, 999};
	CHECK(!DesktopSyncChatMoveStart(h.draft(), other, true));
	CHECK(!DesktopSyncChatMoveStart(h.draft(), h.files.fresh.source.peer, true));
	for (const auto mode : {SyncCarryMode::Switch, SyncCarryMode::RepairMark, SyncCarryMode::Follow, SyncCarryMode::Fill}) {
		CHECK(!DesktopSyncChatCarryResumeActivated(mode, [](DesktopSyncChatCarrySwitch) { return false; }));
		CHECK(DesktopSyncChatCarryResumeActivated(mode, [](DesktopSyncChatCarrySwitch) { return true; }));
	}
	CHECK(DesktopSyncChatCarryResumeActivated(SyncCarryMode::Switch, [](DesktopSyncChatCarrySwitch value) { return value == DesktopSyncChatCarrySwitch::Recovery; }));
	CHECK(!DesktopSyncChatCarryResumeActivated(SyncCarryMode::Follow, [](DesktopSyncChatCarrySwitch value) { return value == DesktopSyncChatCarrySwitch::Move; }));
	for (const auto value : {DesktopSyncChatCarrySwitch::Move, DesktopSyncChatCarrySwitch::FollowFill, DesktopSyncChatCarrySwitch::MarkRemoval,
			DesktopSyncChatCarrySwitch::Cleanup, DesktopSyncChatCarrySwitch::Recovery}) {
		CHECK(!DesktopSyncChatCarrySwitchActivated(value));
	}
}

}

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv); std::setbuf(stdout, nullptr);
	CHECK(!SyncStoreWorkerThread());
	StoreThreadTimers(); Builders(); Moves(); Cleanup(); MoveRefusals(); Revocation(); Restart(); RecoveryTimers(); FollowAndPause();
	LateFollow(false); LateFollow(true); Recovery(); Marks();
	std::printf("CARRY OWNER %d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
