#define PURPLE_SYNC_CHAT_WRITE_EMBEDDED
#include "test_sync_chat_write.cpp"

#include "purple/purple_sync_chat_carry_io.h"
#include "purple/purple_sync_json.h"

#include <QtCore/QCoreApplication>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace {

template <typename T> void Must(const T &value, int line = __builtin_LINE()) {
	CHECK(bool(value));
	if (!value) { std::fprintf(stderr, "Required fixture value missing at line %d\n", line); std::abort(); }
}

MTPMessage CarryDocument(const DesktopSyncChatIo &route, const QByteArray &bytes,
		int id = 10, int date = 100) {
	auto document = NativeDocument(); document.id = MTP_long(99);
	document.access_hash = MTP_long(789); document.dc_id = MTP_int(2);
	document.size = MTP_long(bytes.size());
	document.attributes = MTP_vector<MTPDocumentAttribute>({ MTP_documentAttributeFilename(MTP_string(SyncSettingsRecordFileName())) });
	auto media = NativeMessageMediaDocument(); media.flags = MTP_flags(MTPDmessageMediaDocument::Flag::f_document); media.document = document.Build();
	auto message = NativeMessage();
	message.flags = MTP_flags(MTPDmessage::Flag::f_media | MTPDmessage::Flag::f_from_id
		| (route.peer.chat == PeerToken{} ? MTPDmessage::Flag() : MTPDmessage::Flag::f_out));
	message.id = MTP_int(id); message.date = MTP_int(date);
	message.peer_id = Target(route.peer.chat); message.from_id = MTP_peerUser(MTP_long(Origin.account.userId));
	message.message = MTP_string("#purplesync"); message.media = media.Build();
	return message.Build();
}

MTPMessage CarryMark(const DesktopSyncChatIo &route, const QString &text, int id = 90) {
	auto message = NativeMessage();
	message.flags = MTP_flags(MTPDmessage::Flag::f_from_id | MTPDmessage::Flag::f_out);
	message.id = MTP_int(id); message.date = MTP_int(500);
	message.peer_id = Target(route.peer.chat); message.from_id = MTP_peerUser(MTP_long(Origin.account.userId));
	message.message = MTP_string(text);
	return message.Build();
}

DesktopSyncChatRecordReceipt Receipt(const DesktopSyncChatIo &route, const QByteArray &bytes, int id = 10, int date = 100) {
	const auto result = SealDesktopSyncChatRecord(route, Response(route, { CarryDocument(route, bytes, id, date) }), ClassifySyncCandidate(id, bytes), 99, 0);
	Must(result); return *result;
}

DesktopSyncChatInventory CarryInventory(const DesktopSyncChatIo &route, const QByteArray &bytes = {}, int id = 10) {
	auto read = SyncCandidateReadResult(); read.status = SyncCandidateReadStatus::Complete;
	auto scan = SyncHistoryScanResult(); scan.status = SyncHistoryScanStatus::Complete;
	auto receipts = std::vector<DesktopSyncChatRecordReceipt>();
	if (!bytes.isEmpty()) {
		receipts.push_back(Receipt(route, bytes, id)); read.records.push_back(receipts.front().record);
		read.status = AggregateSyncCandidateRead(read.records); scan.candidateIds.push_back(id); scan.scannedCount = 1;
	}
	return { route, FinishSyncAccountInventory(route.peer.origin.account.userId, scan, read), std::move(receipts), DesktopSyncChatReadStamp::Begin() };
}

void NativeReceiptsAndReads() {
	for (const auto &route : {Route({}),Route({PeerTokenKind::User,456}),Route({PeerTokenKind::Chat,456}),Route({PeerTokenKind::Channel,456})}) {
		auto protectedRoute = route; protectedRoute.peer.facts->protectedContent = true;
		CHECK(ValidDesktopSyncChatReadIo(protectedRoute) && !ValidDesktopSyncChatIo(protectedRoute));
		CHECK(DesktopSyncChatHistoryRequest(protectedRoute, 0));
		CHECK(DesktopSyncChatRecordRequest(protectedRoute, 10));
		CHECK(DesktopSyncChatFileOrigin(protectedRoute, 10));
		CHECK(!DesktopSyncChatSendMediaRequest(protectedRoute, MTPInputMedia(), 20));
		CHECK(!DesktopSyncChatForwardRequest(protectedRoute, Route({PeerTokenKind::Chat,789}), 10, 20));
		for (const auto &bytes : { RecordBytes(), QByteArray("{\"purple_sync\":2,\"opaque\":true}") }) {
			auto probe = ReaderProbe(route); const auto message = CarryDocument(route, bytes);
			probe.Reply(Response(route, {message}));
			probe._loader = std::make_unique<DownloadProbe>(); probe._loader->data = bytes; probe.OnDownload();
			CHECK(probe.rereading && !probe.record && probe._receipts.empty());
			probe.Reply(Response(route, {message}));
			CHECK(probe.record && probe._receipts.size() == 1);
			if (probe._receipts.empty()) { continue; }
			CHECK(probe._receipts.front().record.bytes == bytes);
			auto inventory = CarryInventory(route, bytes);
			const auto converted = DesktopSyncChatCarryInventory(inventory); Must(converted);
			CHECK(converted->messages.front().date == 100);
			for (auto mode = 0; mode != 10; ++mode) {
				auto bad = inventory;
				if (mode == 0) { bad.receipts.front().route.peer.chat = {PeerTokenKind::Chat,789}; }
				if (mode == 1) { bad.receipts.front().route.peer.origin.login = u"other-login"_q; }
				if (mode == 2) { bad.receipts.front().record.bytes += ' '; }
				if (mode == 3) { bad.receipts.front().record.documentId = 100; }
				if (mode == 4) { bad.receipts.front().record.editDate = 1; }
				if (mode == 5) { bad.receipts.front().response = Response(route, {message,message}); }
				if (mode == 6) { bad.inventory.scan.status = SyncHistoryScanStatus::InvalidResponse; }
				if (mode == 7) { bad.inventory.read->status = SyncCandidateReadStatus::Incomplete; }
				if (mode == 8) { bad.inventory.scan.candidateIds.push_back(10); }
				if (mode == 9) { bad.receipts.clear(); }
				CHECK(!DesktopSyncChatCarryInventory(bad));
			}
		}
		const auto source = route.peer.chat == PeerToken{} ? Route({PeerTokenKind::Chat,789}) : Route({});
		const auto forward = DesktopSyncChatForwardRequest(source, route, 10, std::numeric_limits<uint64_t>::max()); Must(forward);
		auto wire = mtpBuffer(); forward->write(wire); auto cursor = wire.constData(); const auto end = cursor + wire.size();
		CHECK(Read<MTPint>(cursor,end).v == int(mtpc_messages_forwardMessages));
		const auto flags = Read<MTPint>(cursor,end).v;
		CHECK(flags == int(MTPmessages_ForwardMessages::Flag::f_silent | MTPmessages_ForwardMessages::Flag::f_drop_author));
		CHECK(Read<MTPInputPeer>(cursor,end).type() == source.input.type());
		const auto ids = Read<MTPVector<MTPint>>(cursor,end);
		const auto randoms = Read<MTPVector<MTPlong>>(cursor,end);
		CHECK(ids.v.size() == 1 && ids.v.front().v == 10 && randoms.v.size() == 1 && uint64_t(randoms.v.front().v) == std::numeric_limits<uint64_t>::max());
		CHECK(Read<MTPInputPeer>(cursor,end).type() == route.input.type() && cursor == end);
		cursor = wire.constData(); const auto decoded = Read<MTPmessages_ForwardMessages>(cursor,end); auto again = mtpBuffer(); decoded.write(again); CHECK(again == wire);
		const auto deletion = DesktopSyncChatDeleteRequest(route, 10); Must(deletion);
		CHECK(std::holds_alternative<MTPchannels_DeleteMessages>(*deletion) == (route.peer.chat.kind == PeerTokenKind::Channel));
		CHECK(!DesktopSyncChatDeleteRequest(route, 0));
	}
	const auto channel = Route({PeerTokenKind::Channel,456});
	const auto chat = Route({PeerTokenKind::Chat,456});
	CHECK(!DesktopSyncChatRecordMessage(chat, Response(channel, {CarryDocument(channel, RecordBytes())}), 10));
}

struct CarryWorkerState {
	QTemporaryDir directory;
	SyncChatCarryStoreScope scope;
	DesktopSyncCarryFresh fresh;
	std::shared_ptr<std::atomic<bool>> current = std::make_shared<std::atomic<bool>>(true);
	std::unique_ptr<SyncChatCarryStore> store;
	std::unique_ptr<DesktopSyncChatCarryIo> io;
	QByteArray original;
	bool fault = false;
	SyncDurablePoint failAt = SyncDurablePoint::AfterRename;

	explicit CarryWorkerState(bool own = false, int padding = 0, SyncCarryMode mode = SyncCarryMode::Switch) {
		Must(directory.isValid()); directory.setAutoRemove(false);
		auto state = SyncLocalState(); state.version = own ? 2 : 1;
		state.chat = own ? PeerToken{PeerTokenKind::Chat,17} : PeerToken{};
		state.install = u"in-"_q + QString(26,u'a'); state.space = u"sp-"_q + QString(26,u'a');
		state.createdDevice = u"Synthetic"_q; state.bindingToken = QString(32,u'a');
		scope = { Origin.account, state.bindingToken.toLatin1(), QString(64,u'a'), directory.path()+u"/cache.json"_q };
		const auto root = directory.path()+u"/sync"_q; Must(EnsureSyncPrivateDirectory(root) == SyncStoreStatus::Ready);
		original = RecordBytes();
		if (mode != SyncCarryMode::Fill) {
			auto local = SyncLocalStore(root); CHECK(local.Open(true).status == SyncStoreStatus::Uninitialized); Must(local.Initialize(state));
			if (padding) {
				auto input = ConfigRecordBuildInput(); input.text = "version = 1\n# " + QByteArray(padding,'a') + '\n';
				input.space = state.space; input.install = state.install; input.device = state.createdDevice;
				input.platform = u"unit"_q; input.app = u"T5b"_q; input.seq = input.at = 1;
				const auto built = BuildConfigRecord(input); Must(built); original = built.canonical;
			}
			auto next = state.configData; next.pending = InspectConfigPayload(ParseSyncEnvelope(original)).version.key;
			Must(local.StageConfig(original,next,scope.binding)); Must(local.ConfirmConfigReadBack(original,state.createdDevice,10,state.chat));
		}
		fresh.source = Route(state.chat); fresh.target = Route({PeerTokenKind::Channel,456});
		fresh.fullResolutionComplete = true;
		fresh.source.peer.facts->protectedContent = own;
		fresh.current = [alive=current] { return alive->load(); }; fresh.loginAuthority = scope.loginAuthority; fresh.readerConsentAccepted = true;
		fresh.resolutionInput.cache = SyncChatCache{state.chat,10,100}; fresh.resolutionInput.settingsBound = true;
		fresh.resolutionInput.settingsChat = state.chat; fresh.resolutionInput.cacheRead = SyncChatDirectRead::Valid;
		fresh.resolutionInput.selfUserId = Origin.account.userId; fresh.resolutionInput.readerWarningAccepted = true;
		fresh.resolutionInput.cachedEligibility = *DesktopSyncChatEligibility(fresh.source.peer);
		if (!own) { fresh.sourceRecord = Receipt(fresh.source,original); }
		if (mode != SyncCarryMode::Switch) {
			fresh.resolutionInput.marks.push_back({
				{fresh.target.peer.chat,20,200},SyncChatMarkStatus::Supported,
				*DesktopSyncChatEligibility(fresh.target.peer),
				mode == SyncCarryMode::Fill ? SyncChatRecordPresence::Empty : SyncChatRecordPresence::Present,true,true});
		}
		if (mode == SyncCarryMode::Fill) {
			fresh.resolutionInput.cache.reset(); fresh.resolutionInput.settingsBound = false;
			fresh.resolutionInput.settingsHint = SyncChatSettingsHint::Found;
		}
		store = std::make_unique<SyncChatCarryStore>(root,scope,[this](SyncDurablePoint point) { return !fault || point != failAt; });
		Must(store->Open());
		auto start = SyncCarryJournalStart(); start.operationId = *FormatSyncBindingToken(QByteArray(16,'o'));
		start.account = scope.account; start.source = state.chat; start.target = fresh.target.peer.chat;
		start.mode = mode; start.state = store->state() ? std::optional(*store->state()) : std::nullopt; start.stateAbsentConfirmed = mode == SyncCarryMode::Fill; start.preferenceToken = scope.binding; start.operationConfirmed = true;
		start.targetEligibility = *DesktopSyncChatEligibility(fresh.target.peer); start.readerConsentAccepted = true;
		start.resolution = ResolveSyncChat(fresh.resolutionInput); start.markText = DesktopSyncChatMarkText();
		start.markRequestId = mode == SyncCarryMode::Switch ? u"41"_q : QString(); start.ownRequestId = u"42"_q;
		Must(store->Begin(start));
		auto event = SyncCarryEvent(*store->journal(),SyncCarryEventKind::Resolution); event.resolution = start.resolution; event.outcome = SyncCarryOutcome::Success; Must(store->Advance(event));
		auto input = SyncCarryInput(); input.account = scope.account; input.source = state.chat; input.target = start.target;
		input.mode = mode; if (mode != SyncCarryMode::Switch) { input.winner = start.resolution.winner; }
		input.state = start.state; input.preferenceToken = scope.binding;
		input.access = own ? SyncCarrySourceAccess::Protected : SyncCarrySourceAccess::Readable;
		input.targetEligibility = start.targetEligibility; input.readerConsentAccepted = true;
		input.oldInventory = {scope.account,state.chat,true,{}}; input.newInventory = {scope.account,start.target,true,{}};
		if (!own) { input.oldInventory.messages.push_back(*MakeSyncCarryMessage(scope.account,state.chat,10,100,original,SyncChatMessageOwnership::Own)); }
		const auto payload = InspectConfigPayload(ParseSyncEnvelope(original));
		if (mode != SyncCarryMode::Fill) { input.ownVersion = SyncCarryOwnVersion{payload.text,payload.version,u"unit"_q,u"T5b"_q,1000}; }
		const auto plan = PlanCarry(input); Must(plan);
		event = SyncCarryEvent(*store->journal(),SyncCarryEventKind::PlanPrepared); event.plan = plan.plan; event.outcome = SyncCarryOutcome::Success; Must(store->Advance(event));
		if (own) { Must(store->StageOwn()); }
		CHECK(store->journal()->phase == SyncCarryPhase::Deliver);
		io = std::make_unique<DesktopSyncChatCarryIo>(*store);
	}

	void Prepared() {
		auto event = SyncCarryEvent(*store->journal(),SyncCarryEventKind::DeliveryPrepared);
		event.outcome = SyncCarryOutcome::Success; Must(store->Advance(event));
	}
	QByteArray Payload() const { return store->journal()->plan->ownRecord.isEmpty() ? original : store->journal()->plan->ownRecord; }
	void MarkPhase() {
		auto destination = SyncCarryInventory{scope.account,fresh.target.peer.chat,true,{}};
		destination.messages.push_back(*MakeSyncCarryMessage(scope.account,fresh.target.peer.chat,20,300,Payload(),SyncChatMessageOwnership::Own));
		auto event = SyncCarryEvent(*store->journal(),SyncCarryEventKind::DestinationRead); event.destination = destination;
		event.outcome = SyncCarryOutcome::Success; Must(store->Advance(event)); CHECK(store->journal()->phase == SyncCarryPhase::Mark);
	}
};

class Worker final {
public:
	explicit Worker(bool own = false, int padding = 0, SyncCarryMode mode = SyncCarryMode::Switch) : thread([this,own,padding,mode] {
		state = std::make_unique<CarryWorkerState>(own,padding,mode);
		initialized.set_value();
		for (;;) {
			auto task = std::function<void()>();
			{
				auto lock = std::unique_lock(mutex); wake.wait(lock,[this] { return stopped || !tasks.empty(); });
				if (stopped && tasks.empty()) { break; }
				task = std::move(tasks.front()); tasks.pop_front();
			}
			task();
		}
		state.reset();
	}) {
		Must(initialized.get_future().wait_for(std::chrono::seconds(120)) == std::future_status::ready);
	}
	~Worker() {
		{ auto lock = std::lock_guard(mutex); stopped = true; }
		wake.notify_one(); thread.join();
	}
	template <typename Function> auto Run(Function function, int line = __builtin_LINE()) {
		using Result = decltype(function(*state));
		auto task = std::make_shared<std::packaged_task<Result()>>([this,function=std::move(function)] { return function(*state); });
		auto result = task->get_future();
		{ auto lock = std::lock_guard(mutex); tasks.push_back([task] { (*task)(); }); }
		wake.notify_one();
		Must(result.wait_for(std::chrono::seconds(120)) == std::future_status::ready, line);
		return result.get();
	}

private:
	std::mutex mutex;
	std::condition_variable wake;
	bool stopped = false;
	std::deque<std::function<void()>> tasks;
	std::unique_ptr<CarryWorkerState> state;
	std::promise<void> initialized;
	std::thread thread;

};

void StaleAbsenceReuse() {
	for (const auto own : {false, true}) {
		auto worker = Worker(own);
		worker.Run([&](CarryWorkerState &s) {
			const auto identity = s.io->Reserve(own ? DesktopSyncCarryEffect::OwnRecord : DesktopSyncCarryEffect::Forward,
				SyncCarryRecordHash(s.Payload()), s.fresh, 901, 902); Must(identity);
			s.Prepared();
			const auto first = s.io->Authorize(identity->slot, s.fresh); Must(first);
			Must(s.io->OutcomeUnknown(*first));
			const auto inventory = CarryInventory(s.fresh.target);
			CHECK(s.io->Reconcile(identity->slot, inventory, s.fresh) == DesktopSyncCarryReconcile::Absent);
			const auto second = s.io->Authorize(identity->slot, s.fresh); Must(second);
			CHECK(first->randomId() == second->randomId() && first->fileId() == second->fileId());
			Must(s.io->OutcomeUnknown(*second));
			auto observer = DesktopSyncChatCarryIo(*s.store);
			CHECK(observer.Reconcile(identity->slot, inventory, s.fresh) == DesktopSyncCarryReconcile::Held);
			const auto staleRetry = s.io->Authorize(identity->slot, s.fresh);
			CHECK(!staleRetry);
			if (staleRetry) { Must(s.io->OutcomeUnknown(*staleRetry)); }
			const auto fresh = CarryInventory(s.fresh.target);
			CHECK(observer.Reconcile(identity->slot, fresh, s.fresh) == DesktopSyncCarryReconcile::Absent);
			CHECK(s.io->Reconcile(identity->slot, fresh, s.fresh) == DesktopSyncCarryReconcile::Held);
			const auto path = s.directory.path() + u"/sync/carry.json"_q;
			const auto saved = QJsonDocument::fromJson(CaptureSyncPrivateFile(path, 4 * 1024 * 1024).bytes).object();
			const auto generation = saved.value(u"transport"_q).toObject().value(u"entries"_q).toObject()
				.value(identity->slot).toObject().value(u"generation"_q).toString();
			CHECK(generation == u"6"_q);
			s.store->Close(); Must(s.store->Open());
			CHECK(observer.Reconcile(identity->slot, fresh, s.fresh) == DesktopSyncCarryReconcile::Held);
			const auto after = QJsonDocument::fromJson(CaptureSyncPrivateFile(path, 4 * 1024 * 1024).bytes).object();
			CHECK(after.value(u"transport"_q).toObject().value(u"entries"_q).toObject()
				.value(identity->slot).toObject().value(u"generation"_q).toString() == generation);
			CHECK(observer.Reconcile(identity->slot, CarryInventory(s.fresh.target), s.fresh) == DesktopSyncCarryReconcile::Absent);
			const auto resumed = s.io->Authorize(identity->slot, s.fresh); Must(resumed);
			CHECK(resumed->randomId() == first->randomId() && resumed->fileId() == first->fileId());
		});
	}
}

void DurableIdentityAndReconcile() {
	for (auto own : {false,true}) {
		auto worker = Worker(own);
		const auto identity = worker.Run([=](CarryWorkerState &s) {
			CHECK(SyncStoreWorkerThread() && !s.io->cleanupEnabled());
			const auto effect = own ? DesktopSyncCarryEffect::OwnRecord : DesktopSyncCarryEffect::Forward;
			const auto hash = SyncCarryRecordHash(s.Payload());
			CHECK(!s.io->Authorize(u"missing"_q,s.fresh));
			const auto result = s.io->Reserve(effect,hash,s.fresh,std::numeric_limits<uint64_t>::max(),901); Must(result);
			const auto repeated = s.io->Reserve(effect,hash,s.fresh,902,903); Must(repeated);
			CHECK(result->randomId == repeated->randomId && result->fileId == repeated->fileId);
			CHECK(result->randomId == (own ? 42 : std::numeric_limits<uint64_t>::max()));
			CHECK(!s.io->Authorize(result->slot,s.fresh)); s.Prepared(); return *result;
		});
		const auto permit = worker.Run([&](CarryWorkerState &s) { return s.io->Authorize(identity.slot,s.fresh); }); Must(permit);
		CHECK(permit->current()); CHECK(permit->randomId() == identity.randomId);
		CHECK(own ? (permit->uploadMatches(worker.Run([](CarryWorkerState &s) {return s.Payload();})) && permit->uploadContent() == worker.Run([](CarryWorkerState &s) {return s.Payload();})) : (bool(DesktopSyncCarryForwardRequest(*permit)) && permit->uploadContent().isEmpty()));
		worker.Run([&](CarryWorkerState &s) {
			CHECK(!s.io->Authorize(identity.slot,s.fresh));
			auto observer = DesktopSyncChatCarryIo(*s.store);
			CHECK(observer.Reconcile(identity.slot,CarryInventory(s.fresh.target),s.fresh) == DesktopSyncCarryReconcile::Held);
			const auto renewed = s.io->Renew(*permit,s.fresh); Must(renewed); CHECK(permit->compatible(*renewed));
			for (auto mode = 0; mode != 8; ++mode) {
				auto bad = s.fresh;
				if (mode == 0) { bad.target.peer.origin.login = u"different"_q; }
				if (mode == 1) { bad.loginAuthority = QString(64,u'b'); }
				if (mode == 2) { bad.target.peer.facts->canSendFiles = false; }
				if (mode == 3) { bad.target.peer.facts->otherMembers.reset(); }
				if (mode == 4) { bad.resolutionInput.cacheRead = SyncChatDirectRead::Unknown; }
				if (mode == 5) { bad.target.peer.chat = {PeerTokenKind::Chat,456}; }
				if (mode == 6) { bad.current = [] {return false;}; }
				if (mode == 7) { bad.resolutionInput.cache->markDate++; }
				CHECK(!s.io->Renew(*permit,bad));
			}
			CHECK(s.io->Reconcile(identity.slot,CarryInventory(s.fresh.target),s.fresh) == DesktopSyncCarryReconcile::Held);
			Must(s.io->OutcomeUnknown(*permit)); CHECK(!permit->current());
			CHECK(!s.io->Authorize(identity.slot,s.fresh));
			auto incomplete = CarryInventory(s.fresh.target); incomplete.inventory.scan.status = SyncHistoryScanStatus::InvalidResponse;
			CHECK(s.io->Reconcile(identity.slot,incomplete,s.fresh) == DesktopSyncCarryReconcile::Held);
			auto successor = std::make_unique<DesktopSyncChatCarryIo>(*s.store);
			CHECK(successor->Reconcile(identity.slot,CarryInventory(s.fresh.target),s.fresh) == DesktopSyncCarryReconcile::Absent);
			const auto next = successor->Authorize(identity.slot,s.fresh); Must(next); CHECK(next->randomId() == identity.randomId && next->fileId() == identity.fileId);
			s.io.reset(); CHECK(next->current()); s.io = std::move(successor);
			s.store->Close(); CHECK(!next->current()); Must(s.store->Open());
			CHECK(!s.io->Authorize(identity.slot,s.fresh));
			CHECK(s.io->Reconcile(identity.slot,CarryInventory(s.fresh.target,s.Payload(),20),s.fresh) == DesktopSyncCarryReconcile::Present);
			CHECK(!s.io->Authorize(identity.slot,s.fresh));
		});
	}
}

void FollowAndFill() {
	for (auto mode : {SyncCarryMode::Follow,SyncCarryMode::Fill}) {
		auto worker = Worker(false,0,mode);
		worker.Run([](CarryWorkerState &s) {
			const auto identity = s.io->Reserve(DesktopSyncCarryEffect::Forward,SyncCarryRecordHash(s.Payload()),s.fresh,900); Must(identity);
			s.Prepared(); const auto permit = s.io->Authorize(identity->slot,s.fresh); Must(permit);
			CHECK(DesktopSyncCarryForwardRequest(*permit));
			CHECK(!DesktopSyncCarryMarkRequest(*permit));
			const auto renewed = s.io->Renew(*permit,s.fresh); Must(renewed); CHECK(permit->compatible(*renewed));
		});
	}
}

void MarkAndWorkerBoundary() {
	auto mainOnly = SyncChatCarryStore(QString(),{}); CHECK(mainOnly.Open().status == SyncStoreStatus::WrongThread);
	for (auto acknowledged : {false,true}) {
		auto worker = Worker();
		worker.Run([=](CarryWorkerState &s) {
			const auto forward = s.io->Reserve(DesktopSyncCarryEffect::Forward,SyncCarryRecordHash(s.Payload()),s.fresh,900); Must(forward);
			s.Prepared(); s.MarkPhase();
			const auto identity = s.io->Reserve(DesktopSyncCarryEffect::Mark,QString(),s.fresh,902); Must(identity); CHECK(identity->randomId == 41);
			CHECK(!s.io->Authorize(identity->slot,s.fresh));
			auto event = SyncCarryEvent(*s.store->journal(),SyncCarryEventKind::MarkPostPrepared); event.outcome = SyncCarryOutcome::Success; Must(s.store->Advance(event));
			const auto permit = s.io->Authorize(identity->slot,s.fresh); Must(permit);
			const auto request = DesktopSyncCarryMarkRequest(*permit); Must(request);
			auto wire = mtpBuffer(); request->write(wire); auto cursor = wire.constData(); const auto end = cursor + wire.size();
			CHECK(Read<MTPint>(cursor,end).v == int(mtpc_messages_sendMessage));
			CHECK(Read<MTPint>(cursor,end).v == int(MTPmessages_SendMessage::Flag::f_silent | MTPmessages_SendMessage::Flag::f_no_webpage));
			CHECK(Read<MTPInputPeer>(cursor,end).type() == s.fresh.target.input.type());
			CHECK(qs(Read<MTPstring>(cursor,end)) == s.store->journal()->markText && Read<MTPlong>(cursor,end).v == 41 && cursor == end);
			if (acknowledged) { CHECK(s.io->RecordReceipt(*permit,Receipt(s.fresh.target,41,90)) == 90); }
			Must(s.io->OutcomeUnknown(*permit));
			CHECK(s.io->Reconcile(identity->slot,CarryInventory(s.fresh.target),s.fresh) == DesktopSyncCarryReconcile::Held);
			CHECK(s.io->ReconcileMark(identity->slot,s.fresh.target,90,Response(s.fresh.target,{}),s.fresh) == DesktopSyncCarryReconcile::Held);
			CHECK(s.io->ReconcileMark(identity->slot,s.fresh.target,90,Response(s.fresh.target,{CarryMark(s.fresh.target,s.store->journal()->markText)}),s.fresh) == (acknowledged ? DesktopSyncCarryReconcile::Present : DesktopSyncCarryReconcile::Held));
			CHECK(!s.io->Authorize(identity->slot,s.fresh));
		});
	}
}

void CarryUploadSchedules() {
	for (auto mode = 0; mode != 8; ++mode) {
		auto worker = Worker(true,40000);
		const auto opened = worker.Run([](CarryWorkerState &s) {
			const auto identity = s.io->Reserve(DesktopSyncCarryEffect::OwnRecord,SyncCarryRecordHash(s.Payload()),s.fresh,800,901); Must(identity);
			s.Prepared(); const auto permit = s.io->Authorize(identity->slot,s.fresh); Must(permit);
			return std::pair(*permit,s.Payload());
		});
		auto session = UploadSession(); session.login = opened.first.target().peer.origin.login;
		auto upload = std::make_shared<UploadProbe>(session,opened.first.target(),opened.second);
		upload->_carry = opened.first; upload->_carryContentValid = opened.first.uploadMatches(opened.second);
		upload->_fileId = opened.first.fileId(); upload->_randomId = opened.first.randomId();
		Fn<void(std::optional<DesktopSyncCarryPermit>)> awaiting;
		upload->_renewCarry = [&](Fn<void(std::optional<DesktopSyncCarryPermit>)> ready) { awaiting = std::move(ready); };
		upload->SendNextPart();
		auto offset = qsizetype(0); auto part = 0;
		while (!session.endpoint.pending.empty()) {
			auto call = session.endpoint.Take(); Must(call.boolean);
			auto cursor = call.wire.constData(); const auto end = cursor+call.wire.size();
			CHECK(Read<MTPint>(cursor,end).v == int(mtpc_upload_saveFilePart));
			CHECK(uint64_t(Read<MTPlong>(cursor,end).v) == opened.first.fileId());
			CHECK(Read<MTPint>(cursor,end).v == part++);
			const auto bytes = Read<MTPbytes>(cursor,end).v;
			CHECK(bytes == opened.second.mid(offset,bytes.size()) && cursor == end); offset += bytes.size();
			CHECK(!call.resume && upload->RequestCurrent());
			upload->current = false;
			CHECK(!upload->RequestCurrent());
			upload->current = true;
			call.boolean(MTP_bool(true));
		}
		CHECK(offset == opened.second.size() && part > 1 && awaiting && upload->completions == 0);
		auto oldReady = awaiting;
		if (mode == 2) {
			awaiting(opened.first); CHECK(upload->completions == 1 && !upload->receipt && session.endpoint.pending.empty()); continue;
		}
		if (mode == 3 || mode == 4) {
			if (mode == 3) { upload->Cancel(); } else { session.owner.leaving = true; }
			const auto fresh = worker.Run([&](CarryWorkerState &s) { return s.io->Renew(opened.first,s.fresh); });
			awaiting(fresh); CHECK(upload->completions == 1 && !upload->receipt && session.endpoint.pending.empty()); continue;
		}
		const auto renewed = worker.Run([&](CarryWorkerState &s) { return s.io->Renew(opened.first,s.fresh); }); Must(renewed);
		CHECK(renewed->randomId() == opened.first.randomId() && renewed->fileId() == opened.first.fileId());
		awaiting(renewed); CHECK(session.endpoint.pending.size() == 1);
		oldReady(renewed); CHECK(session.endpoint.pending.size() == 1);
		auto call = session.endpoint.Take(); CHECK(call.resume && call.updates);
		if (mode == 1 || mode == 6) {
			auto resumes = 0; auto allowed = false;
			call.resume([&](bool value) { ++resumes; allowed = value; });
			CHECK(awaiting && upload->_checking);
			if (mode == 6) { oldReady(renewed); CHECK(upload->_checking && resumes == 0); }
			const auto next = worker.Run([&](CarryWorkerState &s) { return s.io->Renew(*renewed,s.fresh); }); Must(next);
			awaiting(next); CHECK(allowed && resumes == 1 && !upload->_checking);
			CHECK(upload->_fileId == opened.first.fileId() && upload->_randomId == opened.first.randomId());
		}
		const auto updates = Receipt(opened.first.target(),opened.first.randomId(),80);
		if (mode == 5) { upload->Cancel(); CHECK(upload->completions == 1); }
		if (mode == 7) {
			upload->current = false;
			CHECK(!upload->Current() && upload->RequestCurrent());
			session.owner.leaving = true;
			CHECK(!upload->RequestCurrent());
			session.owner.leaving = false;
		}
		call.updates(updates);
		CHECK(upload->_carryResult.has_value());
		CHECK(session.endpoint.applied == 1 && session.endpoint.appliedRandom == opened.first.randomId());
		CHECK(upload->completions == 1 && bool(upload->receipt) == (mode != 5 && mode != 7));
		worker.Run([&](CarryWorkerState &s) {
			CHECK(s.store->journal()->phase == SyncCarryPhase::VerifyDestination);
			CHECK(s.store->state()->config.pendingSeq != 0);
		});
	}
}

QByteArray CarryEnvelope(QJsonObject envelope) {
	const auto parsed = ParseSyncCarryJournal(QJsonDocument(envelope.value(u"journal"_q).toObject()).toJson()); Must(parsed);
	const auto &journal = parsed.journal;
	const auto scoped = QJsonObject{
		{u"domain"_q,u"purple-sync-carry-transport-v1"_q},
		{u"operation"_q,journal.operationId}, {u"mode"_q,int(journal.mode)},
		{u"source"_q,*PeerTokenText(journal.source,journal.account.userId)},
		{u"target"_q,*PeerTokenText(journal.target,journal.account.userId)},
		{u"account"_q,envelope.value(u"account"_q)},
		{u"binding_sha256"_q,envelope.value(u"binding_sha256"_q)},
		{u"login_authority"_q,envelope.value(u"login_authority"_q)},
		{u"transport"_q,envelope.value(u"transport"_q)} };
	const auto encoded = CanonicalizeSyncJson(QJsonDocument(scoped).toJson()); Must(encoded);
	envelope.insert(u"transport_sha256"_q,SyncCarryRecordHash(encoded.canonical));
	const auto result = CanonicalizeSyncJson(QJsonDocument(envelope).toJson(QJsonDocument::Compact)); Must(result);
	return result.canonical;
}

void ReceiptStorageBounds() {
	auto worker = Worker();
	worker.Run([](CarryWorkerState &s) {
		const auto identity = s.io->Reserve(DesktopSyncCarryEffect::Forward,SyncCarryRecordHash(s.Payload()),s.fresh,900); Must(identity);
		s.Prepared(); s.store->Close();
		const auto path = s.directory.path()+u"/sync/carry.json"_q;
		auto raw = QByteArray(); Must(ReadSyncPrivateFile(path,kSyncCarryJournalStorageLimit,raw) == SyncStoreStatus::Ready);
		auto envelope = QJsonDocument::fromJson(raw).object();
		auto transport = envelope.value(u"transport"_q).toObject();
		transport.insert(u"future_padding"_q,QString()); envelope.insert(u"transport"_q,transport);
		const auto empty = CarryEnvelope(envelope);
		transport.insert(u"future_padding"_q,QString(kSyncCarryJournalStorageLimit-empty.size()-1,u'x'));
		envelope.insert(u"transport"_q,transport);
		const auto bounded = CarryEnvelope(envelope); CHECK(bounded.size() == kSyncCarryJournalStorageLimit-1);
		Must(WriteSyncPrivateFile(path,bounded) == SyncStoreStatus::Ready);
		const auto opened=s.store->Open();
		if (!opened) { std::fprintf(stderr,"Bounded receipt reopen status=%d\n",int(opened.status)); }
		Must(opened);
		CHECK(!s.io->Authorize(identity->slot,s.fresh));
		auto after = QByteArray(); Must(ReadSyncPrivateFile(path,kSyncCarryJournalStorageLimit,after) == SyncStoreStatus::Ready); CHECK(after == bounded);
	});
}

void ConsentFaultsAndAuthority() {
	{
		auto worker = Worker();
		worker.Run([](CarryWorkerState &s) {
			auto fresh = s.fresh; fresh.target.peer.facts->otherMembers = 1; fresh.target.peer.readerMembers = std::vector<QString>{u"unit-member"_q};
			const auto added=s.io->Reserve(DesktopSyncCarryEffect::Forward,SyncCarryRecordHash(s.Payload()),fresh,900);
			CHECK(!added); CHECK(s.store->journal()->readerBaseline->otherMembers.value_or(0)==0);
			const auto identity = s.io->Reserve(DesktopSyncCarryEffect::Forward,SyncCarryRecordHash(s.Payload()),s.fresh,900); Must(identity);
			s.fresh = fresh; s.Prepared(); const auto permit = s.io->Authorize(identity->slot,fresh); Must(permit);
			CHECK(permit->readerWarning() && !permit->compatible(*permit));
			auto before = QByteArray(); Must(ReadSyncPrivateFile(s.directory.path()+u"/sync/carry.json"_q,4*1024*1024,before) == SyncStoreStatus::Ready);
			const auto baseline = QJsonDocument::fromJson(before).object().value(u"transport"_q).toObject().value(u"readers"_q);
			auto changed = fresh; changed.target.peer.facts->otherMembers = 2; changed.target.peer.readerMembers->push_back(u"later-member"_q);
			changed.readerConsentAccepted = false;
			const auto next = s.io->Renew(*permit,changed); Must(next); CHECK(next->readerWarning() && next->renewal() > permit->renewal());
			auto after = QByteArray(); Must(ReadSyncPrivateFile(s.directory.path()+u"/sync/carry.json"_q,4*1024*1024,after) == SyncStoreStatus::Ready);
			const auto transport = QJsonDocument::fromJson(after).object().value(u"transport"_q).toObject();
			CHECK(transport.value(u"readers"_q) == baseline && transport.value(u"reader_warning"_q).toBool());
			auto unknown = changed; unknown.target.peer.facts->otherAdmins.reset(); CHECK(!s.io->Renew(*next,unknown));
			auto edited = changed; edited.sourceRecord = Receipt(edited.source,s.original,10,101); CHECK(!s.io->Renew(*next,edited));
			s.store->Close(); CHECK(!next->current());
			auto extension = QJsonDocument::fromJson(after).object();
			auto extended = extension.value(u"transport"_q).toObject(); extended.insert(u"future_member"_q,u"preserve"_q);
			extension.insert(u"transport"_q,extended);
			Must(WriteSyncPrivateFile(s.directory.path()+u"/sync/carry.json"_q,CarryEnvelope(extension)) == SyncStoreStatus::Ready);
			Must(s.store->Open());
			CHECK(!s.io->Authorize(identity->slot,changed));
			CHECK(s.io->Reconcile(identity->slot,CarryInventory(changed.target),changed) == DesktopSyncCarryReconcile::Absent);
			const auto again = s.io->Authorize(identity->slot,changed); Must(again); CHECK(again->readerWarning());
			auto preserved = QByteArray(); Must(ReadSyncPrivateFile(s.directory.path()+u"/sync/carry.json"_q,kSyncCarryJournalStorageLimit,preserved) == SyncStoreStatus::Ready);
			CHECK(QJsonDocument::fromJson(preserved).object().value(u"transport"_q).toObject().value(u"future_member"_q).toString() == u"preserve"_q);
		});
	}
	for (auto point : {SyncDurablePoint::BeforeWrite,SyncDurablePoint::AfterRename,SyncDurablePoint::FileBarrier,SyncDurablePoint::DirectoryBarrier}) {
		auto worker = Worker();
		worker.Run([=](CarryWorkerState &s) {
			s.failAt = point; s.fault = true;
			CHECK(!s.io->Reserve(DesktopSyncCarryEffect::Forward,SyncCarryRecordHash(s.Payload()),s.fresh,900));
			s.fault = false; s.store->Close(); Must(s.store->Open());
			const auto saved = s.io->Reserve(DesktopSyncCarryEffect::Forward,SyncCarryRecordHash(s.Payload()),s.fresh,900); Must(saved); CHECK(saved->randomId == 900);
			s.Prepared(); const auto permit = s.io->Authorize(saved->slot,s.fresh); Must(permit);
			s.current->store(false); CHECK(!permit->current() && !DesktopSyncCarryForwardRequest(*permit));
			s.current->store(true); s.store->Close(); CHECK(!permit->current());
		});
	}
	for (auto corruption : {0,1,2}) {
		auto worker = Worker();
		worker.Run([=](CarryWorkerState &s) {
			CHECK(!s.io->Reserve(DesktopSyncCarryEffect::Forward,SyncCarryRecordHash(s.Payload()),s.fresh,41));
			const auto id = s.io->Reserve(DesktopSyncCarryEffect::Forward,SyncCarryRecordHash(s.Payload()),s.fresh,900); Must(id);
			s.store->Close();
			const auto path = s.directory.path()+u"/sync/carry.json"_q; auto raw = QByteArray();
			Must(ReadSyncPrivateFile(path,4*1024*1024,raw) == SyncStoreStatus::Ready);
			auto envelope = QJsonDocument::fromJson(raw).object(); auto transport = envelope.value(u"transport"_q).toObject();
			if (corruption == 0) { transport.insert(u"version"_q,2147483648.0); }
			if (corruption == 2) {
				auto entries = transport.value(u"entries"_q).toObject(); auto entry = entries.value(id->slot).toObject();
				entry.insert(u"random_id"_q,u"901"_q); entries.insert(id->slot,entry); transport.insert(u"entries"_q,entries);
			}
			envelope.insert(u"transport"_q,transport);
			if (corruption == 1) { envelope.remove(u"transport_sha256"_q); }
			const auto edited = corruption == 0 ? CarryEnvelope(envelope)
				: CanonicalizeSyncJson(QJsonDocument(envelope).toJson()).canonical;
			Must(!edited.isEmpty());
			Must(WriteSyncPrivateFile(path,edited) == SyncStoreStatus::Ready);
			const auto reopened = s.store->Open();
			CHECK(reopened.status == SyncStoreStatus::InvalidState);
			CHECK(!s.io->Reserve(DesktopSyncCarryEffect::Forward,SyncCarryRecordHash(s.Payload()),s.fresh,901));
			CHECK(!s.io->Authorize(id->slot,s.fresh));
			auto after = QByteArray(); Must(ReadSyncPrivateFile(path,4*1024*1024,after) == SyncStoreStatus::Ready); CHECK(after == edited);
		});
	}
}

void ReadStartAndGenerationControls() {
	CHECK(!DesktopSyncChatReadStamp().valid());
	const auto first = DesktopSyncChatReadStamp::Begin();
	const auto second = DesktopSyncChatReadStamp::Begin();
	CHECK(second.after(first) && !first.after(second) && !second.after(second));
	for (const auto bad : {QJsonValue(0),QJsonValue(u"00"_q),QJsonValue(u"-1"_q),
		QJsonValue(u"1.0"_q),QJsonValue(u"18446744073709551616"_q)}) {
		CHECK(!DesktopSyncCarryTransportGeneration({{u"generation"_q,bad}}));
	}
	CHECK(DesktopSyncCarryTransportGeneration({{u"generation"_q,u"18446744073709551615"_q}})
		== std::numeric_limits<uint64_t>::max());
	auto worker = Worker();
	worker.Run([&](CarryWorkerState &s) {
		const auto id = s.io->Reserve(DesktopSyncCarryEffect::Forward,SyncCarryRecordHash(s.Payload()),s.fresh,901); Must(id);
		s.Prepared(); const auto permit = s.io->Authorize(id->slot,s.fresh); Must(permit);
		const auto started = DesktopSyncChatReadStamp::Begin();
		Must(s.io->OutcomeUnknown(*permit));
		auto finished = CarryInventory(s.fresh.target); finished.readStarted = started;
		CHECK(s.io->Reconcile(id->slot,finished,s.fresh) == DesktopSyncCarryReconcile::Held);
		finished.readStarted = {};
		CHECK(s.io->Reconcile(id->slot,finished,s.fresh) == DesktopSyncCarryReconcile::Held);
		CHECK(s.io->Reconcile(id->slot,CarryInventory(s.fresh.target),s.fresh) == DesktopSyncCarryReconcile::Absent);
		const auto path = s.directory.path()+u"/sync/carry.json"_q;
		auto envelope = QJsonDocument::fromJson(CaptureSyncPrivateFile(path,4*1024*1024).bytes).object();
		auto transport = envelope.value(u"transport"_q).toObject(); auto entries = transport.value(u"entries"_q).toObject();
		auto entry = entries.value(id->slot).toObject(); entry.insert(u"generation"_q,u"18446744073709551615"_q);
		entries.insert(id->slot,entry);transport.insert(u"entries"_q,entries);envelope.insert(u"transport"_q,transport);
		s.store->Close(); Must(WriteSyncPrivateFile(path,CarryEnvelope(envelope))==SyncStoreStatus::Ready);Must(s.store->Open());
		CHECK(!s.io->Authorize(id->slot,s.fresh));
		CHECK(s.io->Reconcile(id->slot,CarryInventory(s.fresh.target),s.fresh) == DesktopSyncCarryReconcile::Held);
		CHECK(QJsonDocument::fromJson(CaptureSyncPrivateFile(path,4*1024*1024).bytes).object()
			.value(u"transport"_q).toObject().value(u"entries"_q).toObject().value(id->slot).toObject()
			.value(u"generation"_q).toString()==u"18446744073709551615"_q);
	});
}

}

#ifndef PURPLE_SYNC_CHAT_CARRY_EMBEDDED
int main(int argc, char **argv) {
	auto app = QCoreApplication(argc,argv);
	std::setbuf(stdout,nullptr);
	std::puts("Native receipts"); NativeReceiptsAndReads();
	std::puts("Durable identities"); DurableIdentityAndReconcile();
	std::puts("Stale absence"); StaleAbsenceReuse(); ReadStartAndGenerationControls();
	std::puts("Follow/Fill"); FollowAndFill();
	std::puts("Mark boundary"); MarkAndWorkerBoundary();
	std::puts("Uploads"); CarryUploadSchedules();
	std::puts("Consent/faults"); ConsentFaultsAndAuthority();
	std::puts("Receipt capacity"); ReceiptStorageBounds();
	std::printf("CARRY IO %d checks, %d failures\n",Checks,Failures);
	return Failures ? 1 : 0;
}

#endif
