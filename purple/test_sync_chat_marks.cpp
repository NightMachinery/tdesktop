#define PURPLE_SYNC_CHAT_CARRY_EXECUTOR_EMBEDDED
#include "test_sync_chat_carry.cpp"
#include "purple/purple_sync_chat_mark_actions.h"
#include <deque>

namespace {

struct MarkSenderProbe {
	Fn<void(const MTPmessages_AffectedMessages &)> done;
	Fn<void(const MTP::Error &)> fail;
	Fn<void(Fn<void(bool)>)> gate;
	mtpBuffer wire;
	int sends = 0;
	template <typename Request> struct Builder {
		MarkSenderProbe *owner;
		Builder &done(Fn<void(const MTPmessages_AffectedMessages &)> callback) { owner->done = std::move(callback); return *this; }
		Builder &fail(Fn<void(const MTP::Error &)> callback) { owner->fail = std::move(callback); return *this; }
		Builder &resumeEffect(Fn<void(Fn<void(bool)>)> callback) { owner->gate = std::move(callback); return *this; }
		void send() { ++owner->sends; }
	};
	template <typename Request> Builder<Request> request(Request value) {
		wire.clear(); value.write(wire); return {this};
	}
};

#include "mark-transport.inc"

MTPMessage MarkAt(const DesktopSyncChatIo &route, int id = 10, int date = 100, QString text = DesktopSyncChatMarkText()) {
	auto value = NativeMessage();
	value.flags = MTP_flags(MTPDmessage::Flag::f_out | MTPDmessage::Flag::f_from_id);
	value.id = MTP_int(id); value.date = MTP_int(date);
	value.peer_id = Target(route.peer.chat); value.from_id = MTP_peerUser(MTP_long(Origin.account.userId));
	value.message = MTP_string(text);
	return value.Build();
}

MTPMessage EmptyMark(int id, std::optional<PeerToken> peer = {}) {
	return MTP_messageEmpty(MTP_flags(peer ? MTPDmessageEmpty::Flag::f_peer_id : MTPDmessageEmpty::Flags()),
		MTP_int(id), peer ? Target(*peer) : MTPPeer());
}

DesktopSyncChatSetupSnapshot MarkFrame(ExecutorFiles &f) {
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
	return result;
}

struct MarkHarness {
	ExecutorFiles files;
	DesktopSyncChatIo route;
	DesktopSyncChatStaleMark row;
	DesktopSyncChatSetupSnapshot frame;
	DesktopSyncChatMarkExact read;
	DesktopSyncChatMarkProgress progress;
	std::unique_ptr<DesktopSyncChatMarkActions> action;
	std::deque<Fn<void()>> jobs;
	MarkSenderProbe sender;
	DesktopSyncChatMarkPermit sentPermit;
	Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)> resolveReply;
	Fn<void(std::optional<DesktopSyncChatMarkExact>)> exactReply;
	Fn<void(bool)> applyReply;
	Fn<void()> onCancel;
	QString login;
	QString window = u"window"_q;
	DesktopSyncChatOrigin origin = Origin;
	int resolutions = 0;
	int reads = 0;
	int applied = 0;
	int cancelled = 0;
	int senderDispatches = 0;
	bool current = true;
	bool foreground = true;
	bool holdResolve = false;
	bool holdRead = false;
	bool holdApply = false;
	bool removed = false;
	bool readable = true;
	bool appliedOkay = true;

	explicit MarkHarness(std::optional<PeerToken> selected = {}) {
		files.engine->open(files.start); files.finish(); files.engine.reset();
		route = selected ? Route(*selected) : files.fresh.source; login = files.scope.loginAuthority;
		const auto captured = CaptureDesktopSyncChatStaleMark(route.peer, login, MarkAt(route)); Must(captured); row = *captured;
		frame = MarkFrame(files); read = {route, Response(route, {MarkAt(route)}), true};
		auto port = DesktopSyncChatMarkPort();
		port.origin = [&] { return origin; }; port.login = [&] { return login; }; port.window = [&] { return window; };
		port.current = [&] { return current; }; port.foreground = [&] { return foreground; };
		port.worker = [&](Fn<void()> job) { jobs.push_back(std::move(job)); };
		port.resolve = [&](auto done) { ++resolutions; resolveReply = std::move(done); if (!holdResolve) { resolveReply(frame); } };
		port.exact = [&](const auto &selected, auto done) {
			CHECK(selected == row); ++reads; exactReply = std::move(done);
			if (!holdRead) { exactReply(removed ? DesktopSyncChatMarkExact{route, Response(route, {EmptyMark(row.location.markId)}), readable} : read); }
		};
		port.send = [&](auto permit, auto renew, auto done) {
			sentPermit = permit; CHECK(permit.current());
			Must(ProbeMarkTransport(sender, permit, std::move(renew), [&](Fn<void()> task) { ++senderDispatches; jobs.push_back(std::move(task)); }, std::move(done)));
		};
		port.apply = [&](const auto &target, const auto &, auto done) {
			CHECK(target.peer.origin == row.origin && target.peer.chat == row.location.chat);
			++applied; removed = true; applyReply = std::move(done); if (!holdApply) { applyReply(appliedOkay); }
		};
		port.cancel = [&] { ++cancelled; if (onCancel) { onCancel(); } };
		port.status = [&](auto value) { progress = std::move(value); };
		action = std::make_unique<DesktopSyncChatMarkActions>(row, window, files.root, files.scope, std::move(port));
	}
	~MarkHarness() { action->close(); drain(); }
	void drain() {
		for (auto i = 0; !jobs.empty() && i < 200; ++i) { const auto job = std::move(jobs.front()); jobs.pop_front(); job(); }
		CHECK(jobs.empty());
	}
	void start() { action->start(); drain(); }
	void ack() { Must(sender.done); sender.done(MTP_messages_affectedMessages(MTP_int(7), MTP_int(1))); drain(); }
};

void ExactAndAuthority() {
	auto files = ExecutorFiles(); files.engine->open(files.start); files.finish(); files.engine.reset();
	const auto frame = MarkFrame(files);
	for (const auto token : {PeerToken{}, PeerToken{PeerTokenKind::User,456}, PeerToken{PeerTokenKind::Chat,456}, PeerToken{PeerTokenKind::Channel,456}}) {
		const auto route = Route(token); const auto message = MarkAt(route);
		const auto row = CaptureDesktopSyncChatStaleMark(route.peer, files.scope.loginAuthority, message); Must(row);
		const auto exact = DesktopSyncChatMarkExact{route, Response(route, {message}), true};
		CHECK(AuthorizeDesktopSyncChatStaleMark(*row, frame, exact));
		const auto request = DesktopSyncChatRecordRequest(route, 10); Must(request);
		CHECK(std::holds_alternative<MTPchannels_GetMessages>(*request) == (token.kind == PeerTokenKind::Channel));
		for (auto mode = 0; mode != 14; ++mode) {
			auto changed = exact; auto snapshot = frame;
			if (mode == 0) { changed.response = Response(route, {}); }
			if (mode == 1) { changed.response = Response(route, {message, message}); }
			if (mode == 2) { changed.response = Response(route, {MarkAt(route, 11)}); }
			if (mode == 3) { changed.response = Response(route, {MarkAt(route, 10, 101)}); }
			if (mode == 4) { changed.response = Response(route, {MarkAt(route, 10, 100, row->text + u"\n"_q)}); }
			if (mode == 5) { changed.route = Route({PeerTokenKind::Chat,789}); }
			if (mode == 6) { changed.route.peer.origin.login += u"stale"_q; }
			if (mode == 7) { changed.readable = false; }
			if (mode == 8) { snapshot.status.complete = false; }
			if (mode == 9) { snapshot.status.postingHeld = true; }
			if (mode == 10) { snapshot.scope.loginBinding = QString(64,u'b'); }
			if (mode == 11) { auto m = NativeMessage(); m.flags = MTP_flags(MTPDmessage::Flag::f_from_id); m.from_id = MTP_peerUser(MTP_long(999)); m.peer_id = Target(token); m.id = MTP_int(10); m.date = MTP_int(100); m.message = MTP_string(row->text); changed.response = Response(route,{m.Build()}); }
			if (mode == 12) { auto page = NativeMessagesSlice(); page.flags = MTP_flags(MTPDmessages_messagesSlice::Flag::f_inexact); page.count = MTP_int(1); page.messages = MTP_vector<MTPMessage>({message}); changed.response = page.Build(); }
			if (mode == 13) { auto page = NativeChannelMessages(); page.count = MTP_int(0); page.messages = MTP_vector<MTPMessage>({message}); changed.response = page.Build(); }
			CHECK(!AuthorizeDesktopSyncChatStaleMark(*row, snapshot, changed));
		}
		for (const auto explicitPeer : {false, true}) {
			const auto empty = EmptyMark(10, explicitPeer ? std::optional(token) : std::nullopt);
			const auto absence = DesktopSyncChatMarkExact{route, Response(route,{empty}),true};
			CHECK(DesktopSyncChatSelectedMarkAbsent(*row,absence));
			CHECK(!AuthorizeDesktopSyncChatStaleMark(*row,frame,absence));
			if (!explicitPeer) { CHECK(!DesktopSyncChatRecordMessage(route,absence.response,10)); }
		}
		for (const auto &value : {EmptyMark(11), EmptyMark(10,PeerToken{PeerTokenKind::Chat,789}), MarkAt(route,10,100,u"ordinary"_q)}) {
			CHECK(!DesktopSyncChatSelectedMarkAbsent(*row,{route,Response(route,{value}),true}));
		}
		CHECK(!DesktopSyncChatSelectedMarkAbsent(*row,{route,Response(route,{}),true}));
		CHECK(!DesktopSyncChatSelectedMarkAbsent(*row,{route,Response(route,{EmptyMark(10)}),false}));
		CHECK(!CaptureDesktopSyncChatStaleMark(route.peer, files.scope.loginAuthority, MarkAt(route,10,100,u"#purplechatsync v2"_q)));
	}
	const auto route = files.fresh.target;
	const auto row = CaptureDesktopSyncChatStaleMark(route.peer,files.scope.loginAuthority,MarkAt(route,90,500)); Must(row);
	CHECK(!AuthorizeDesktopSyncChatStaleMark(*row,frame,{route,Response(route,{MarkAt(route,90,500)}),true}));
}

void Flows() {
	{
		auto h = MarkHarness(); h.start(); CHECK(h.sender.sends == 1 && h.sentPermit.current());
		CHECK(!h.sentPermit.request());
		auto cursor = h.sender.wire.constData(); const auto end = cursor + h.sender.wire.size();
		CHECK(Read<MTPint>(cursor,end).v == int(mtpc_messages_deleteMessages));
		CHECK(Read<MTPint>(cursor,end).v == int(MTPmessages_DeleteMessages::Flag::f_revoke));
		const auto ids = Read<MTPVector<MTPint>>(cursor,end); CHECK(ids.v.size() == 1 && ids.v.front().v == 10 && cursor == end);
		h.ack(); CHECK(h.progress.reason == u"mark-removed"_q && h.progress.absent && h.applied == 1);
		CHECK(!h.sentPermit.current() && !h.files.gate->postingHeld());
		const auto callback = h.sender.done; callback(MTP_messages_affectedMessages(MTP_int(7),MTP_int(1))); h.drain(); CHECK(h.applied == 1);
	}
	{
		auto h = MarkHarness(); h.removed = true; h.start();
		CHECK(h.progress.reason == u"mark-already-absent"_q && h.progress.absent && h.sender.sends == 0 && h.applied == 0);
	}
	for (auto mode = 0; mode != 6; ++mode) {
		auto h = MarkHarness();
		if (mode == 0) { h.frame.status.complete = false; }
		if (mode == 1) { h.frame.status.postingHeld = true; }
		if (mode == 2) { h.read.response = Response(h.route,{}); }
		if (mode == 3) { h.read.readable = false; }
		if (mode == 4) { h.read.route.peer.chat = {PeerTokenKind::Channel,789}; }
		if (mode == 5) { h.foreground = false; }
		h.start(); CHECK(h.sender.sends == 0 && !h.files.gate->postingHeld());
	}
	for (auto phase = 0; phase != 4; ++phase) {
		auto h = MarkHarness(); h.holdResolve = phase == 0; h.holdRead = phase == 1; h.holdApply = phase == 3;
		h.start(); if (phase == 3) { h.ack(); }
		const auto permit = h.sentPermit; h.action->close(); CHECK(!permit.current()); h.drain();
		CHECK(!h.files.gate->postingHeld()); const auto lease = h.files.gate->writer(); Must(lease);
		if (h.resolveReply) { h.resolveReply(h.frame); } if (h.exactReply) { h.exactReply(h.read); }
		if (h.applyReply) { h.applyReply(true); } h.drain(); CHECK(h.applied <= 1 && h.cancelled == 1);
	}
	for (auto mode = 0; mode != 4; ++mode) {
		auto h = MarkHarness(); h.start(); Must(h.sender.gate); const auto original = h.sentPermit;
		if (mode == 1) { h.frame.status.complete = false; }
		if (mode == 2) { h.read.response = Response(h.route,{}); }
		if (mode == 3) { h.login = QString(64,u'b'); }
		auto allowed = false; const auto before = h.resolutions;
		h.sender.gate([&](bool value) { allowed = value; }); h.drain();
		CHECK(allowed == (mode == 0)); CHECK(!original.current());
		if (mode == 0) { CHECK(h.resolutions == before + 1 && h.senderDispatches == 1); h.ack(); CHECK(h.progress.absent); }
	}
	{
		auto h = MarkHarness(); h.start(); h.sender.fail(MTP::Error::Local(u"TIMEOUT"_q, QString())); h.drain();
		CHECK(h.progress.reason == u"mark-delete-uncertain"_q && h.sender.sends == 1 && !h.files.gate->postingHeld());
	}
	{
		auto h = MarkHarness(); h.onCancel = [&] { Must(h.files.gate->writer()); throw std::runtime_error("cancel"); };
		h.start(); h.action->close(); h.drain(); CHECK(!h.files.gate->postingHeld() && h.cancelled == 1);
	}
	{
		auto h = MarkHarness(); h.appliedOkay = false; h.start(); h.ack();
		CHECK(h.progress.reason == u"mark-updates-held"_q && h.reads == 1 && !h.files.gate->postingHeld());
	}
}

void TypedTransport() {
	for (const auto token : {PeerToken{PeerTokenKind::User,456}, PeerToken{PeerTokenKind::Chat,456}, PeerToken{PeerTokenKind::Channel,456}}) {
		auto h = MarkHarness(token); h.start(); CHECK(h.sender.sends == 1);
		auto cursor = h.sender.wire.constData(); const auto end = cursor + h.sender.wire.size();
		if (token.kind == PeerTokenKind::Channel) {
			CHECK(Read<MTPint>(cursor,end).v == int(mtpc_channels_deleteMessages));
			const auto channel = Read<MTPInputChannel>(cursor,end); CHECK(channel.type() == mtpc_inputChannel);
			CHECK(channel.c_inputChannel().vchannel_id().v == token.id && channel.c_inputChannel().vaccess_hash().v != 0);
		} else {
			CHECK(Read<MTPint>(cursor,end).v == int(mtpc_messages_deleteMessages));
			CHECK(Read<MTPint>(cursor,end).v == int(MTPmessages_DeleteMessages::Flag::f_revoke));
		}
		const auto ids = Read<MTPVector<MTPint>>(cursor,end); CHECK(ids.v.size() == 1 && ids.v.front().v == h.row.location.markId && cursor == end);
		h.ack(); CHECK(h.progress.absent && h.applied == 1 && !h.files.gate->postingHeld());
	}
}

void MoreFlows() {
	for (auto phase = 0; phase != 4; ++phase) {
		for (auto change = 0; change != 4; ++change) {
			auto h = MarkHarness(); h.holdResolve = phase == 0; h.holdRead = phase == 1; h.holdApply = phase == 3;
			h.start(); if (phase == 3) { h.ack(); }
			if (change == 0) { ++h.origin.account.userId; }
			if (change == 1) { h.login = QString(64,u'b'); }
			if (change == 2) { h.window = u"replacement"_q; }
			if (change == 3) { h.foreground = false; }
			if (phase == 0) { h.resolveReply(h.frame); }
			if (phase == 1) { h.exactReply(h.read); }
			if (phase == 2) { h.sender.done(MTP_messages_affectedMessages(MTP_int(7),MTP_int(1))); }
			if (phase == 3) { h.applyReply(true); }
			h.drain(); CHECK(!h.sentPermit.current() && !h.files.gate->postingHeld());
			CHECK(h.cancelled == 1 && !h.progress.absent);
		}
	}
	{
		auto h = MarkHarness(); h.start(); const auto original = h.sentPermit;
		h.frame.input.cache = h.row.location; h.frame.input.settingsChat = h.row.location.chat;
		h.frame.input.cachedEligibility = *DesktopSyncChatEligibility(h.route.peer);
		h.frame.input.marks = {{h.row.location, SyncChatMarkStatus::Supported, h.frame.input.cachedEligibility, SyncChatRecordPresence::Present, true, true}};
		h.frame.status.resolution = ResolveSyncChat(h.frame.input);
		auto allowed = true; h.sender.gate([&](bool value) { allowed = value; }); h.drain();
		CHECK(!allowed && !original.current() && h.progress.reason == u"mark-resolution-held"_q);
	}
	{
		auto h = MarkHarness(); h.start(); h.readable = false; h.ack();
		CHECK(h.progress.reason == u"mark-absence-unknown"_q && !h.progress.absent && h.applied == 1);
	}
	{
		auto h = MarkHarness(); h.holdRead = true; h.start(); const auto old = h.exactReply;
		h.exactReply(h.read); h.drain(); Must(h.sender.gate);
		h.sender.gate([](bool) {}); h.drain(); const auto before = h.sender.sends;
		old(h.read); h.drain(); CHECK(h.sender.sends == before && h.resolutions == 2);
		h.action->close(); h.drain(); CHECK(!h.files.gate->postingHeld());
	}
}

void References() {
	{
		auto h = MarkHarness(); auto store = SyncChatCarryStore(h.files.root,h.files.scope); Must(store.Open());
		CHECK(DesktopSyncChatMarkReferencesClear(store,h.row));
	}
	{
		auto f = ExecutorFiles(); f.engine->open(f.start); f.step(); f.engine.reset();
		auto store = SyncChatCarryStore(f.root,f.scope); Must(store.Open());
		const auto row = CaptureDesktopSyncChatStaleMark(f.fresh.source.peer,f.scope.loginAuthority,MarkAt(f.fresh.source)); Must(row);
		CHECK(!DesktopSyncChatMarkReferencesClear(store,*row));
	}
	{
		auto h = MarkHarness(); Must(WriteSyncPrivateFile(h.files.root+u"/carry-history/invalid.json"_q,"{}") == SyncStoreStatus::Ready);
		h.start(); CHECK(h.sender.sends == 0 && h.progress.reason == u"mark-store-held"_q);
	}
	{
		auto f = ExecutorFiles(); f.start.removeOldMarkAuthorized = true;
		f.engine->open(f.start); f.run(); Must(f.progress.journal && f.progress.journal->phase == SyncCarryPhase::Cleanup);
		f.engine->skipCleanup(true); Must(f.progress.terminal); f.engine.reset();
		auto store = SyncChatCarryStore(f.root,f.scope); Must(store.Open());
		const auto row = CaptureDesktopSyncChatStaleMark(f.fresh.source.peer,f.scope.loginAuthority,MarkAt(f.fresh.source)); Must(row);
		CHECK(!DesktopSyncChatMarkReferencesClear(store,*row));
		auto edited = *row; ++edited.location.markDate; CHECK(!DesktopSyncChatMarkReferencesClear(store,edited));
		auto unrelated = *row; unrelated.location.markId = 123; CHECK(DesktopSyncChatMarkReferencesClear(store,unrelated));
	}

}

}

void MainThreadBoundaries() {
	auto queue = std::deque<Fn<void()>>(); auto cancels = 0;
	auto port = DesktopSyncChatMarkPort();
	port.worker = [&](Fn<void()> job) { queue.push_back(std::move(job)); };
	port.cancel = [&] { ++cancels; };
	auto action = DesktopSyncChatMarkActions({}, u"window"_q, u"/not-opened"_q, {}, std::move(port));
	action.start(); CHECK(queue.size() == 1); action.close(); CHECK(queue.size() == 2);
	auto worker = std::thread([&] { while (!queue.empty()) { const auto task = std::move(queue.front()); queue.pop_front(); task(); } }); worker.join();
	CHECK(cancels == 1 && queue.empty());
}

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc,argv); std::setbuf(stdout,nullptr);
	CHECK(!DesktopSyncChatMarkRemovalActivated()); MainThreadBoundaries();
	auto worker = std::thread([] { ExactAndAuthority(); Flows(); TypedTransport(); MoreFlows(); References(); }); worker.join();
	std::printf("T5F MARKS %d checks, %d failures\n",Checks,Failures);
	return Failures ? 1 : 0;
}
