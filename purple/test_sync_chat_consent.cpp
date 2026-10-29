#define PURPLE_SYNC_CHAT_CARRY_EXECUTOR_EMBEDDED
#include "test_sync_chat_carry.cpp"
#include "purple/purple_sync_chat_consent.h"
#include <QJsonArray>
#include <deque>

namespace {

DesktopSyncChatSetupSnapshot Frame(ExecutorFiles &f) {
	auto frame = DesktopSyncChatSetupSnapshot();
	frame.scope.origin = f.fresh.target.peer.origin;
	frame.scope.root = f.temporary.path();
	frame.scope.binding = f.scope.binding;
	frame.scope.loginBinding = f.scope.loginAuthority;
	frame.scope.settingsBound = f.start.state.has_value();
	frame.scope.settingsChat = f.start.state ? f.start.state->chat : PeerToken{};
	frame.scope.space = f.start.state ? f.start.state->space : QString();
	frame.scope.mark = f.fresh.resolutionInput.cache;
	frame.input = f.fresh.resolutionInput;
	frame.status.complete = true;
	frame.status.phase = DesktopSyncChatPhase::Paused;
	frame.status.postingHeld = true;
	frame.status.resolution = ResolveSyncChat(frame.input);
	frame.stateImage = f.start.state ? SerializeSyncLocalState(*f.start.state).canonical : QByteArray();
	frame.run = 1;
	return frame;
}

void Readers(ExecutorFiles &f, uint64_t count, std::optional<std::vector<QString>> members) {
	f.fresh.target.peer.facts->otherMembers = count;
	f.fresh.target.peer.readerMembers = std::move(members);
	f.fresh.consentPeer = f.fresh.target.peer;
	for (auto &mark : f.fresh.resolutionInput.marks) { mark.eligibility = *DesktopSyncChatEligibility(f.fresh.target.peer); }
	f.start.targetEligibility = *DesktopSyncChatEligibility(f.fresh.target.peer);
	f.start.resolution = ResolveSyncChat(f.fresh.resolutionInput);
}

QJsonObject TransportReaders(ExecutorFiles &f) {
	const auto active = CaptureSyncPrivateFile(f.root + u"/carry.json"_q, kSyncCarryJournalStorageLimit);
	Must(active.status == SyncStoreStatus::Ready);
	auto bytes = active.bytes;
	if (active.absent()) {
		Must(f.progress.journal && !SyncCarryJournalHoldsWrites(*f.progress.journal));
		auto store = SyncChatCarryStore(f.root, f.scope); Must(store.Open());
		const auto retained = store.RetainedImages(); Must(retained);
		for (const auto &image : *retained) {
			const auto journal = InspectSyncCarryJournal(image, f.scope.binding); Must(journal);
			if (journal->operationId != f.progress.journal->operationId) { continue; }
			CHECK(SerializeSyncCarryJournal(*journal).canonical == SerializeSyncCarryJournal(*f.progress.journal).canonical);
			CHECK(bytes.isEmpty()); bytes = image;
		}
	}
	Must(!bytes.isEmpty());
	const auto journal = InspectSyncCarryJournal(bytes, f.scope.binding); Must(journal);
	CHECK(journal->account == f.scope.account && journal->operationId == f.progress.journal->operationId);
	const auto transport = DesktopSyncCarryTransportOf(QJsonDocument::fromJson(bytes).object(), *journal); Must(transport);
	Must(transport->value(u"readers"_q).isObject());
	return transport->value(u"readers"_q).toObject();
}

void MainActions(const DesktopSyncChatSetupSnapshot &frame, const DesktopSyncChatPeer &peer) {
	CHECK(!SyncStoreWorkerThread());
	CHECK(!DesktopSyncChatFollowFillActivated());
	auto live = true;
	auto foreground = true;
	for (const auto stream : {u"config"_q, u"library"_q}) {
		auto action = DesktopSyncChatConsentAction(frame, u"window-A"_q, [&] { return live; }, [&] { return foreground; });
		const auto offer = action.offer(peer, stream); Must(offer);
		CHECK(offer->askReaders && !offer->firstPost);
		CHECK(!action.accept(*offer, frame, peer, DesktopSyncChatConsentChoice::Proceed, false));
		for (auto mutation = 0; mutation != 13; ++mutation) {
			auto changed = frame;
			switch (mutation) {
			case 0: changed.scope.origin.account.userId++; break;
			case 1: changed.scope.origin.login += u'x'; break;
			case 2: changed.scope.loginBinding = QString(64, u'b'); break;
			case 3: changed.scope.root += u'x'; break;
			case 4: changed.scope.binding += 'x'; break;
			case 5: changed.scope.owner = AccountKey{0, 999}; break;
			case 6: changed.scope.mode = AccountWorkMode::Follow; break;
			case 7: changed.stateImage += ' '; break;
			case 8: changed.cacheImage += ' '; break;
			case 9: changed.status.complete = false; break;
			case 10: changed.input.marks.front().location.markId++; changed.status.resolution = ResolveSyncChat(changed.input); break;
			case 11: changed.scope.carry = true; break;
			case 12: changed.scope.mark->markDate++; break;
			}
			CHECK(!action.accept(*offer, changed, peer, DesktopSyncChatConsentChoice::Proceed, true));
		}
		auto changed = peer; changed.readerMembers = std::vector<QString>{QString(64, u'b')};
		CHECK(!action.accept(*offer, frame, changed, DesktopSyncChatConsentChoice::Proceed, true));
		foreground = false;
		CHECK(!action.offer(peer, stream));
		CHECK(!action.accept(*offer, frame, peer, DesktopSyncChatConsentChoice::Pause, false));
		foreground = true; live = false;
		CHECK(!action.accept(*offer, frame, peer, DesktopSyncChatConsentChoice::Proceed, true));
		live = true;
		auto forged = *offer; forged.window = u"window-B"_q;
		CHECK(!action.accept(forged, frame, peer, DesktopSyncChatConsentChoice::Proceed, true));
		const auto accepted = action.accept(*offer, frame, peer, DesktopSyncChatConsentChoice::Proceed, true); Must(accepted);
		CHECK(accepted->offer.stream == stream);
		CHECK(!action.accept(*offer, frame, peer, DesktopSyncChatConsentChoice::Proceed, true));
		action.cancel(); CHECK(!action.offer(peer, stream));
	}
	const auto path = frame.scope.root + u'/' + DesktopSyncChatCacheFileName(frame.scope.origin.account);
	CHECK(!WriteDesktopSyncChatPause(path, frame.scope.origin.account, frame.scope.loginBinding, {}, true, std::make_shared<std::atomic<bool>>(true)));
}

struct FlowHarness {
	ExecutorFiles files;
	DesktopSyncChatSetupSnapshot frame;
	DesktopSyncChatPeer peer;
	std::deque<Fn<void()>> queue;
	std::unique_ptr<DesktopSyncChatConsentFlow> flow;
	bool live = true;
	bool foreground = true;
	QString window = u"window-A"_q;
	QString login;
	DesktopSyncChatOrigin origin;
	QString reason;
	QString stream = u"config"_q;
	int starts = 0;
	int sends = 0;
	int cancels = 0;
	int resolves = 0;
	int inspections = 0;
	int stopAt = -1;
	int callbacks = 0;
	bool throwObserver = false;
	bool holdReply = false;
	Fn<void()> beforeOpen;
	Fn<void(DesktopSyncCarryReply)> reply;
	DesktopSyncCarryReply pending;

	explicit FlowHarness(SyncCarryMode mode = SyncCarryMode::Follow, bool pending = false)
	: files(false, mode, pending) {
		files.engine.reset();
		Readers(files, 1, std::vector<QString>{QString(64, u'a')});
		frame = Frame(files); peer = files.fresh.target.peer;
		login = files.scope.loginAuthority; origin = peer.origin;
	}
	void drain() {
		for (auto i = 0; !queue.empty() && i != 500; ++i) { auto task = std::move(queue.front()); queue.pop_front(); task(); }
		CHECK(queue.empty());
	}
	void start(DesktopSyncChatConsentChoice choice = DesktopSyncChatConsentChoice::Proceed) {
		auto action = DesktopSyncChatConsentAction(frame, window, [&] { return live; }, [&] { return foreground; });
		const auto offer = action.offer(peer, stream); Must(offer);
		const auto selection = action.accept(*offer, frame, peer, choice, true); Must(selection);
		auto port = DesktopSyncChatConsentPort();
		port.origin = [&] { return origin; }; port.login = [&] { return login; }; port.window = [&] { return window; };
		port.current = [&] { return live; }; port.foreground = [&] { return foreground; };
		port.worker = [&](Fn<void()> task) { queue.push_back(std::move(task)); };
		port.resolve = [&](Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)> done) { ++resolves; done(frame); };
		port.inspect = [&](PeerToken target, Fn<void(std::optional<DesktopSyncChatPeer>)> done) { CHECK(target == peer.chat); ++inspections; done(peer); };
		port.begin = [&](const DesktopSyncChatConsentSelection &) { return std::optional(files.start); };
		port.carry = [&](std::shared_ptr<std::atomic<bool>> admitted, Fn<void(DesktopSyncCarryProgress)> progress) {
			++starts; files.admitted = std::move(admitted);
			if (beforeOpen) { beforeOpen(); }
			const auto lease = files.gate->carry(); Must(lease);
			return std::make_unique<DesktopSyncChatCarry>(files.root, files.scope, files.fresh.target.peer.origin,
				files.admitted, lease, std::move(progress));
		};
		port.status = [&](DesktopSyncCarryProgress progress) {
			++callbacks;
			if (!progress.reason.isEmpty()) { reason = progress.reason; }
			if (progress.journal || progress.command) { files.progress = progress; }
			if (stopAt == callbacks && flow) { flow->close(); }
			if (throwObserver) { throw std::runtime_error("test observer"); }
		};
		port.send = [&](DesktopSyncCarryCommand command, Fn<void(DesktopSyncCarryReply)> done) {
			++sends; CHECK(files.progress.command && files.progress.command->ticket == command.ticket);
			auto result = files.reply();
			if (command.dispatch) {
				const auto baseline = TransportReaders(files);
				CHECK(baseline.value(u"stream"_q).toString() == stream);
				CHECK(baseline.value(u"settings_disclosed"_q).toBool());
			}
			if (holdReply) { reply = std::move(done); pending = std::move(result); }
			else { done(std::move(result)); }
		};
		port.pause = [&](const DesktopSyncChatConsentSelection &s, std::shared_ptr<std::atomic<bool>> admitted, Fn<void(bool)> done) {
			done(WriteDesktopSyncChatPause(files.scope.cacheFile, s.opening.scope.origin.account,
				s.opening.scope.loginBinding, s.opening.cacheImage, true, admitted));
		};
		port.cancel = [&] { ++cancels; };
		flow = std::make_unique<DesktopSyncChatConsentFlow>(*selection, std::move(port));
		flow->start();
	}
	~FlowHarness() { flow.reset(); drain(); }
};

void Flows() {
	for (const auto mode : {SyncCarryMode::Follow, SyncCarryMode::Fill}) {
		auto h = FlowHarness(mode); h.start(); h.drain();
		if (!h.files.progress.terminal) { std::fprintf(stderr, "Consent flow held: %s\n", qPrintable(h.reason)); }
		CHECK(h.files.progress.terminal); CHECK(h.starts == 1 && h.resolves == 2 && h.inspections == 2);
		CHECK(!h.files.gate->postingHeld()); CHECK(h.files.markWrites == 0);
		const auto parsed = ParseDesktopSyncChatCache(CaptureSyncPrivateFile(h.files.scope.cacheFile, 65536).bytes, Origin.account);
		Must(parsed.record); CHECK(parsed.record->readerConsent && !parsed.record->paused);
		CHECK(parsed.record->disclosedStreams == std::vector<QString>{u"config"_q});
		if (mode == SyncCarryMode::Fill) { CHECK(CaptureSyncPrivateFile(h.files.root + u"/state.json"_q, kSyncCarryJournalStorageLimit).absent()); }
	}
	{
		auto h = FlowHarness(SyncCarryMode::Follow, true); h.start(); h.drain();
		CHECK(h.files.progress.terminal && h.files.pendingPosts == 0);
		CHECK(h.files.markWrites == 0);
	}
	for (auto mutation = 0; mutation != 5; ++mutation) {
		auto h = FlowHarness(); h.start();
		switch (mutation) {
		case 0: h.foreground = false; break;
		case 1: h.window = u"window-B"_q; break;
		case 2: h.login = QString(64, u'b'); break;
		case 3: h.origin.account.userId++; break;
		case 4: h.live = false; break;
		}
		h.drain(); CHECK(h.starts == 0 && h.sends == 0); h.flow->close(); h.drain(); CHECK(!h.files.gate->postingHeld());
	}
	for (auto stop = 1; stop != 9; ++stop) {
		auto h = FlowHarness(); h.stopAt = stop; h.start(); h.drain(); h.flow->close(); h.drain();
		CHECK(!h.files.gate->postingHeld()); CHECK(h.cancels == 1);
	}
	{
		auto h = FlowHarness(); h.throwObserver = true; h.start(); h.drain();
		CHECK(!h.files.gate->postingHeld()); CHECK(h.cancels == 1);
	}
	{
		auto h = FlowHarness(); h.holdReply = true; h.start(); h.drain(); Must(h.reply);
		h.flow->close(); h.drain(); const auto sent = h.sends; h.reply(h.pending); h.drain();
		CHECK(h.sends == sent && !h.files.gate->postingHeld());
	}
	{
		auto h = FlowHarness(); h.holdReply = true; h.start(); h.drain();
		for (auto i = 0; !h.pending.command.dispatch && i != 20; ++i) {
			const auto callback = h.reply; const auto reply = h.pending; Must(callback); callback(reply); h.drain();
		}
		Must(h.pending.command.dispatch && !h.pending.command.effects.empty());
		const auto permit = h.pending.command.effects.front(); CHECK(permit.current());
		h.flow->close(); CHECK(!permit.current()); CHECK(!h.files.admitted->load());
		h.drain(); CHECK(!h.files.gate->postingHeld());
	}
	for (const auto changeState : {false, true}) {
		auto h = FlowHarness();
		h.beforeOpen = [&] {
			if (changeState) {
				const auto path = h.files.root + u"/state.json"_q;
				const auto before = CaptureSyncPrivateFile(path, kSyncCarryJournalStorageLimit);
				Must(WriteSyncPrivateFile(path, before.bytes + ' ') == SyncStoreStatus::Ready);
			} else {
				const auto image = DesktopSyncChatPauseImage({}, Origin.account, h.login, false, true); Must(image);
				Must(WriteSyncPrivateFile(h.files.scope.cacheFile, *image) == SyncStoreStatus::Ready);
			}
		};
		h.start(); h.drain();
		CHECK(h.reason == u"opening-images-held"_q && h.sends == 0 && h.files.outward == 0);
		CHECK(CaptureSyncPrivateFile(h.files.root + u"/carry.json"_q, kSyncCarryJournalStorageLimit).absent());
		CHECK(!h.files.gate->postingHeld());
	}
	{
		auto h = FlowHarness(); h.start(DesktopSyncChatConsentChoice::Pause); h.drain();
		CHECK(h.starts == 0 && h.sends == 0 && h.reason == u"sync-paused-here"_q);
		const auto parsed = ParseDesktopSyncChatCache(CaptureSyncPrivateFile(h.files.scope.cacheFile, 65536).bytes, Origin.account);
		CHECK(parsed.paused && parsed.pauseOnly && !parsed.record);
	}
}

DesktopSyncChatCacheRecord CacheRecord(const DesktopSyncChatPeer &peer, const QString &login) {
	auto record = DesktopSyncChatCacheRecord(); record.account = peer.origin.account;
	record.mark = {peer.chat, 20, 200}; record.loginBinding = login;
	record.readers = DesktopSyncChatEligibility(peer)->readers; record.readerMembers = peer.readerMembers;
	record.readerCreator = peer.creator; record.readerConsent = true;
	record.disclosedStreams = {u"config"_q, u"library"_q}; return record;
}

void PauseLogout() {
	for (const auto admitted : {false, true}) {
		auto f = ExecutorFiles(); f.engine.reset();
		const auto login = f.scope.loginAuthority; const auto newLogin = QString(64, u'b');
		auto record = CacheRecord(f.fresh.target.peer, login);
		const auto original = admitted ? *SerializeDesktopSyncChatCache(record) : QByteArray();
		if (admitted) { Must(WriteSyncPrivateFile(f.scope.cacheFile, original) == SyncStoreStatus::Ready); }
		CHECK(WriteDesktopSyncChatPause(f.scope.cacheFile, Origin.account, login, original, true, f.admitted));
		const auto paused = CaptureSyncPrivateFile(f.scope.cacheFile, 65536).bytes;
		const auto parsed = ParseDesktopSyncChatCache(paused, Origin.account);
		CHECK(parsed.paused && (parsed.record.has_value() == admitted));
		ForgetDesktopSyncChatLogin(f.scope.cacheFile, Origin.account, login);
		const auto negative = CaptureSyncPrivateFile(f.scope.cacheFile, 65536).bytes;
		const auto after = ParseDesktopSyncChatCache(negative, Origin.account);
		CHECK(after.paused && after.pauseOnly && !after.record);
		CHECK(!negative.contains("login_binding") && !negative.contains("readers") && !negative.contains("chat"));
		const auto disk = OpenDesktopSyncChatDisk(f.temporary.path(), f.root, f.root + u"/state.json"_q,
			Origin.account, f.scope.binding, newLogin, f.admitted);
		CHECK(disk->error == u"sync-paused-here"_q && !disk->cache && !disk->lock);
		CHECK(!DesktopSyncChatPauseImage(negative, Origin.account, newLogin, false, false));
		CHECK(!DesktopSyncChatPauseImage(negative, AccountKey{0,999}, newLogin, false, true));
		f.admitted->store(false); CHECK(!WriteDesktopSyncChatPause(f.scope.cacheFile, Origin.account, newLogin, negative, false, f.admitted));
		f.admitted->store(true); CHECK(WriteDesktopSyncChatPause(f.scope.cacheFile, Origin.account, newLogin, negative, false, f.admitted));
		const auto resumed = ParseDesktopSyncChatCache(CaptureSyncPrivateFile(f.scope.cacheFile, 65536).bytes, Origin.account);
		CHECK(!resumed.paused && resumed.pauseOnly && !resumed.record);
		record.loginBinding = newLogin; const auto fresh = *SerializeDesktopSyncChatCache(record);
		Must(WriteSyncPrivateFile(f.scope.cacheFile, fresh) == SyncStoreStatus::Ready);
		ForgetDesktopSyncChatLogin(f.scope.cacheFile, Origin.account, login);
		CHECK(CaptureSyncPrivateFile(f.scope.cacheFile, 65536).bytes == fresh);
		CHECK(!WriteDesktopSyncChatPause(f.scope.cacheFile, Origin.account, newLogin, negative, true, f.admitted));
	}
}

void Baselines() {
	for (const auto mode : {SyncCarryMode::Follow, SyncCarryMode::Fill}) {
		for (const auto known : {false, true}) {
			for (const auto addition : {false, true}) {
				auto f = ExecutorFiles(false, mode); Readers(f, 2, known ? std::optional(std::vector<QString>{QString(64,u'a'),QString(64,u'b')}) : std::nullopt);
				f.engine->open(f.start, f.fresh.target.peer, u"library"_q);
				const auto baseline = TransportReaders(f);
				CHECK(baseline.value(u"stream"_q).toString() == u"library"_q);
				CHECK(baseline.value(u"members_known"_q).toBool() == known);
				CHECK(f.progress.journal && !f.progress.journal->deliveryAttempted && f.outward == 0);
				while (f.progress.command && !f.progress.command->dispatch) { f.step(); }
				Must(f.progress.command && f.progress.command->dispatch);
				f.step();
				Readers(f, addition ? 3 : 1, std::vector<QString>{QString(64,u'a')});
				f.finish();
				const auto cache = ParseDesktopSyncChatCache(CaptureSyncPrivateFile(f.scope.cacheFile,65536).bytes,Origin.account); Must(cache.record);
				CHECK(cache.record->newReaders == addition);
				CHECK(cache.record->readerMembers.has_value() == known);
				CHECK(cache.record->readers.otherMembers == 2);
				CHECK(cache.record->disclosedStreams.size() == 2);
				CHECK(TransportReaders(f) == baseline);
			}
		}
		{
			auto f=ExecutorFiles(false,mode); Readers(f,1,std::vector<QString>{QString(64,u'a')}); f.engine->open(f.start,f.fresh.target.peer);
			const auto baseline=TransportReaders(f); Readers(f,1,std::vector<QString>{QString(64,u'b')}); f.step();
			CHECK(f.outward==0 && !f.progress.command); CHECK(TransportReaders(f)==baseline);
			f.engine.reset(); f.newEngine(true); Must(f.progress.command); f.step();
			CHECK(f.outward==0 && !f.progress.command); CHECK(TransportReaders(f)==baseline);
		}
		{
			auto f=ExecutorFiles(false,mode); Readers(f,1,std::vector<QString>{QString(64,u'a')}); f.engine->open(f.start,f.fresh.target.peer);
			while(f.progress.command && !f.progress.command->dispatch){f.step();} Must(f.progress.command); f.step();
			Readers(f,1,std::vector<QString>{QString(64,u'b')}); f.finish();
			const auto cache=ParseDesktopSyncChatCache(CaptureSyncPrivateFile(f.scope.cacheFile,65536).bytes,Origin.account); Must(cache.record); CHECK(cache.record->newReaders);
		}
		{
			auto f=ExecutorFiles(false,mode); Readers(f,1,std::vector<QString>{QString(64,u'a')}); f.engine->open(f.start,f.fresh.target.peer);
			f.fresh.target.peer.readerMembers.reset(); f.step(); CHECK(f.outward==0 && !f.progress.command);
		}
		{
			auto f=ExecutorFiles(false,mode); f.engine.reset();
			{auto store=SyncChatCarryStore(f.root,f.scope); Must(store.Open()); Must(store.Begin(f.start,true));}
			f.newEngine(true); Must(f.progress.command); f.step(); CHECK(f.outward==0 && !f.progress.command);
		}
	}
}

void DeferredOpeningControls() {
	for (const auto mode : {SyncCarryMode::Follow, SyncCarryMode::Fill}) {
		for (const auto stream : {u"config"_q, u"library"_q}) {
			auto h = FlowHarness(mode); h.stream = stream; h.holdReply = true;
			auto writer = h.files.gate->writer(); Must(writer); h.start(); h.drain();
			CHECK(h.reason == u"publishers-not-quiescent"_q && h.sends == 0);
			writer.reset(); h.flow->retry(); h.drain();
			const auto baseline = TransportReaders(h.files);
			CHECK(baseline.value(u"stream"_q).toString() == stream);
			CHECK(baseline.value(u"members_known"_q).toBool() && baseline.value(u"members"_q).toArray() == QJsonArray{QString(64,u'a')});
			h.holdReply = false; const auto callback = h.reply; const auto reply = h.pending; Must(callback); callback(reply); h.drain();
			CHECK(h.files.progress.terminal && TransportReaders(h.files) == baseline);
		}
		if (mode == SyncCarryMode::Follow) {
			auto h = FlowHarness(mode); auto writer = h.files.gate->writer(); Must(writer);
			h.start(); h.drain(); CHECK(h.reason == u"publishers-not-quiescent"_q);
			const auto path = h.files.root + u"/state.json"_q;
			const auto before = CaptureSyncPrivateFile(path,kSyncCarryJournalStorageLimit); Must(!before.absent());
			Must(WriteSyncPrivateFile(path,before.bytes + ' ') == SyncStoreStatus::Ready);
			writer.reset(); h.flow->retry(); h.drain();
			CHECK(h.reason == u"opening-images-held"_q && h.files.outward == 0 && !h.files.progress.terminal);
		}
		for (auto loss = 0; loss != 2; ++loss) {
			auto f = ExecutorFiles(false,mode); f.fresh.target = Route({PeerTokenKind::Chat,456}); f.start.target = f.fresh.target.peer.chat;
			for (auto &mark : f.fresh.resolutionInput.marks) { mark.location.chat = f.start.target; }
			Readers(f,1,std::vector<QString>{QString(64,u'a')}); f.engine->open(f.start,f.fresh.target.peer,u"library"_q);
			for (auto i = 0; f.progress.command && f.progress.command->action != SyncCarryJournalAction::ReadMark && i != 30; ++i) { f.step(); }
			Must(f.progress.command && f.progress.command->action == SyncCarryJournalAction::ReadMark);
			const auto reply = f.reply(); Must(reply.markResponse);
			const auto read = DesktopSyncChatCarryMarkReadback(f.fresh.target,reply.markId,*reply.markResponse,f.progress.journal->markText); Must(read);
			f.engine.reset(); auto store = SyncChatCarryStore(f.root,f.scope); Must(store.Open());
			auto event = SyncCarryEvent(*store.journal(),SyncCarryEventKind::MarkRead); event.outcome = SyncCarryOutcome::Success; event.mark = *read; event.noRequestInFlight = true;
			Must(store.Advance(event));
			auto changed = f.fresh.target.peer;
			if (loss == 0) { changed.readerMembers.reset(); changed.facts->otherMembers = 200; }
			if (loss == 1) { changed.creator = false; changed.facts->creator = false; changed.soleAdmin = false; changed.facts->soleAdmin = false; changed.facts->otherAdmins = 1; }
			CHECK(!store.WriteCache(changed,false)); CHECK(!store.journal()->cachePrepared && !store.journal()->stateCommitted);
			CHECK(store.WriteCache(f.fresh.target.peer,false));
		}
	}
}


}

int main(int argc,char **argv) {
	auto app=QCoreApplication(argc,argv); std::setbuf(stdout,nullptr);
	DesktopSyncChatSetupSnapshot frame; DesktopSyncChatPeer peer;
	auto prepare=std::thread([&] {auto f=ExecutorFiles(false,SyncCarryMode::Follow); Readers(f,1,std::vector<QString>{QString(64,u'a')});frame=Frame(f);peer=f.fresh.target.peer;});prepare.join();
	MainActions(frame,peer);
	auto worker=std::thread([] {Flows();PauseLogout();Baselines();DeferredOpeningControls();});worker.join();
	std::printf("T5E CONSENT %d checks, %d failures\n",Checks,Failures);
	return Failures ? 1 : 0;
}
