#include "purple/purple_sync_chat_requests.h"
#include "purple/purple_sync_chat_discovery.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include "purple/purple_sync_chat_store.h"
#include "purple/purple_sync_local_store.h"
#include "purple/purple_sync_history_scanner.h"
#include "sync-chat-native-meta.inc"
#include <unistd.h>
#include "sync-chat-native-cases.inc"

#include <cstdio>

using namespace Purple;
namespace {

auto Checks = 0;
auto Failures = 0;
#define CHECK(value) do { ++Checks; if (!(value)) { ++Failures; std::printf("FAIL line%d: %s\n", __LINE__, #value); } } while (false)

const auto Origin = DesktopSyncChatOrigin{{0, 123}, u"login-a"_q};

MTPMessage Message(int id, int64_t channel = 456) {
	auto message = NativeMessage();
	message.id = MTP_int(id);
	message.date = MTP_int(100);
	message.peer_id = MTP_peerChannel(MTP_long(channel));
	return message.Build();
}

DesktopSyncChatSearchPage Page(int start, int total, int count = 100) {
	auto result = DesktopSyncChatSearchPage();
	result.recognized = true;
	result.total = total;
	result.nextRate = 7;
	for (auto n = 0; n < count; ++n) {
		result.positions.push_back({{PeerTokenKind::Channel, 456}, start - n, 100});
	}
	return result;
}

template <typename Value>
Value Read(const mtpPrime *&from, const mtpPrime *end) {
	auto result = Value();
	CHECK(result.read(from, end));
	return result;
}

struct RequestFields {
	int flags = 0;
	QString query;
	MTPMessagesFilter filter;
	int limit = 0;
	int rate = 0;
	int offsetId = 0;
	MTPInputPeer offsetPeer;
};

RequestFields Inspect(const MTPmessages_SearchGlobal &request) {
	auto bytes = mtpBuffer(); request.write(bytes);
	auto from = bytes.constData(); const auto end = from + bytes.size();
	CHECK(Read<MTPint>(from, end).v == int(mtpc_messages_searchGlobal));
	auto fields = RequestFields(); fields.flags = Read<MTPint>(from, end).v;
	CHECK((fields.flags & ~2) == 0);
	fields.query = qs(Read<MTPstring>(from, end)); fields.filter = Read<MTPMessagesFilter>(from, end);
	CHECK(Read<MTPint>(from, end).v == 0); CHECK(Read<MTPint>(from, end).v == 0);
	fields.rate = Read<MTPint>(from, end).v; fields.offsetPeer = Read<MTPInputPeer>(from, end);
	fields.offsetId = Read<MTPint>(from, end).v; fields.limit = Read<MTPint>(from, end).v;
	CHECK(from == end);
	return fields;
}

RequestFields Inspect(const MTPmessages_Search &request) {
	auto bytes = mtpBuffer(); request.write(bytes);
	auto from = bytes.constData(); const auto end = from + bytes.size();
	CHECK(Read<MTPint>(from, end).v == int(mtpc_messages_search));
	auto fields = RequestFields(); fields.flags = Read<MTPint>(from, end).v;
	CHECK(fields.flags == 0);
	fields.offsetPeer = Read<MTPInputPeer>(from, end); fields.query = qs(Read<MTPstring>(from, end));
	fields.filter = Read<MTPMessagesFilter>(from, end);
	CHECK(Read<MTPint>(from, end).v == 0); CHECK(Read<MTPint>(from, end).v == 0);
	fields.offsetId = Read<MTPint>(from, end).v; CHECK(Read<MTPint>(from, end).v == 0);
	fields.limit = Read<MTPint>(from, end).v;
	CHECK(Read<MTPint>(from, end).v == 0); CHECK(Read<MTPint>(from, end).v == 0);
	CHECK(Read<MTPlong>(from, end).v == 0); CHECK(from == end);
	return fields;
}

void Requests() {
	for (const auto broadcasts : {false, true}) {
		const auto first = Inspect(DesktopSyncChatGlobalRequest(broadcasts, {}, MTP_inputPeerSelf()));
		CHECK((first.flags & 1) == 0);
		CHECK(bool(first.flags & 2) == broadcasts);
		CHECK(first.filter.type() == mtpc_inputMessagesFilterEmpty && first.limit == 100);
		CHECK(first.query == u"#purplechatsync"_q && first.offsetPeer.type() == mtpc_inputPeerEmpty);
		const auto cursor = DesktopSyncChatCursor{42, PeerToken{PeerTokenKind::Channel, 456}, 99};
		const auto next = Inspect(DesktopSyncChatGlobalRequest(broadcasts, cursor, MTP_inputPeerChannel(MTP_long(456), MTP_long(789))));
		CHECK(next.rate == 42 && next.offsetId == 99);
		CHECK(next.offsetPeer.type() == mtpc_inputPeerChannel && next.offsetPeer.c_inputPeerChannel().vchannel_id().v == 456);
	}
	for (const auto kind : {DesktopSyncChatSearch::Self, DesktopSyncChatSearch::Dialog, DesktopSyncChatSearch::SettingsHint}) {
		const auto request = Inspect(DesktopSyncChatSearchRequest(kind, MTP_inputPeerSelf(), 88));
		CHECK(request.flags == 0);
		CHECK(request.offsetId == 88 && request.offsetPeer.type() == mtpc_inputPeerSelf);
		CHECK(request.limit == (kind == DesktopSyncChatSearch::SettingsHint ? 1 : 100));
		CHECK(request.filter.type() == (kind == DesktopSyncChatSearch::SettingsHint ? mtpc_inputMessagesFilterDocument : mtpc_inputMessagesFilterEmpty));
		CHECK(request.query == (kind == DesktopSyncChatSearch::SettingsHint ? u"#purplesync"_q : u"#purplechatsync"_q));
	}
	CHECK(!DesktopSyncChatExactReadRequest({PeerTokenKind::Channel, 456}, 12, {}));
	CHECK(!DesktopSyncChatExactReadRequest({PeerTokenKind::Channel, 456}, 12, MTP_inputChannel(MTP_long(457), MTP_long(1))));
	CHECK(!DesktopSyncChatExactReadRequest({}, 0, {}));
	const auto channel = DesktopSyncChatExactReadRequest({PeerTokenKind::Channel, 456}, 12, MTP_inputChannel(MTP_long(456), MTP_long(1)));
	CHECK(channel && std::holds_alternative<MTPchannels_GetMessages>(*channel));
	const auto self = DesktopSyncChatExactReadRequest({}, 12, {});
	CHECK(self && std::holds_alternative<MTPmessages_GetMessages>(*self));
}

void Pagination() {
	for (const auto kind : {DesktopSyncChatSearch::Global, DesktopSyncChatSearch::Broadcasts, DesktopSyncChatSearch::Self, DesktopSyncChatSearch::Dialog}) {
		auto pages = DesktopSyncChatSearchPages(Origin, kind);
		CHECK(pages.Add(Page(1000, 201)) == DesktopSyncChatPageStatus::Next);
		CHECK(pages.cursor().rate == 7 && pages.cursor().id == 901);
		CHECK(pages.Add(Page(900, 201)) == DesktopSyncChatPageStatus::Next);
		CHECK(pages.Add(Page(800, 201, 1)) == DesktopSyncChatPageStatus::Complete);
		CHECK(pages.pages() == 3);
		CHECK(pages.Add(Page(799, 201, 0)) == DesktopSyncChatPageStatus::Incomplete);
	}
	{
		auto pages = DesktopSyncChatSearchPages(Origin, DesktopSyncChatSearch::Global);
		for (auto page = 0; page < 20; ++page) {
			CHECK(pages.Add(Page(3000 - page * 100, 2001)) == (page == 19 ? DesktopSyncChatPageStatus::Capped : DesktopSyncChatPageStatus::Next));
		}
	}
	{
		auto pages = DesktopSyncChatSearchPages(Origin, DesktopSyncChatSearch::Global);
		CHECK(pages.Add(Page(1000, 200)) == DesktopSyncChatPageStatus::Next);
		CHECK(pages.Add(Page(1000, 200)) == DesktopSyncChatPageStatus::Incomplete);
	}
	for (auto scenario = 0; scenario < 5; ++scenario) {
		auto pages = DesktopSyncChatSearchPages(Origin, DesktopSyncChatSearch::Global);
		CHECK(pages.Add(Page(1000, 200)) == DesktopSyncChatPageStatus::Next);
		auto next = Page(900, 200);
		if (scenario == 0) next.recognized = false;
		if (scenario == 1) next.total = 201;
		if (scenario == 2) next.positions.clear();
		if (scenario == 3) next.positions[0].markId = 0;
		if (scenario == 4) next.nextRate = -1;
		CHECK(pages.Add(next) == DesktopSyncChatPageStatus::Incomplete);
	}
}

void NativeReplies() {
	{
		auto full = NativeMessages(); CHECK(RecognizedDesktopSyncChatRead(full.Build(), {}));
		auto slice = NativeMessagesSlice(); slice.flags = MTP_flags(MTPDmessages_messagesSlice::Flag::f_inexact);
		CHECK(!RecognizedDesktopSyncChatRead(slice.Build(), {}));
		slice.flags = MTP_flags(MTPDmessages_messagesSlice::Flag::f_search_flood); slice.search_flood = MTP_searchPostsFlood(MTP_flags(MTPDsearchPostsFlood::Flags()), MTP_int(1), MTP_int(2), MTP_int(3), MTP_long(4));
		CHECK(!RecognizedDesktopSyncChatRead(slice.Build(), {}));
		auto channel = NativeChannelMessages(); CHECK(!RecognizedDesktopSyncChatRead(channel.Build(), {}));
		CHECK(RecognizedDesktopSyncChatRead(channel.Build(), {PeerTokenKind::Channel, 456}));
		CHECK(!RecognizedDesktopSyncChatRead(MTP_messages_messagesNotModified(MTP_int(0)), {}));
	}

	auto slice = NativeMessagesSlice();
	slice.flags = MTP_flags(MTPDmessages_messagesSlice::Flag::f_next_rate);
	slice.count = MTP_int(3); slice.next_rate = MTP_int(7);
	slice.messages = MTP_vector<MTPMessage>({Message(90), Message(80, 457)});
	auto page = MapDesktopSyncChatSearchPage(Origin, slice.Build(), DesktopSyncChatSearch::Global, {});
	CHECK(page.recognized && page.total == 3 && page.nextRate == 7 && page.positions.size() == 2);
	CHECK(page.positions.back().chat == (PeerToken{PeerTokenKind::Channel, 457}));
	CHECK(page.positions.back().markId == 80);
	CHECK(!MapDesktopSyncChatSearchPage(Origin, slice.Build(), DesktopSyncChatSearch::Self, PeerToken{}).recognized);
	slice.flags = MTP_flags(MTPDmessages_messagesSlice::Flag::f_inexact);
	CHECK(!MapDesktopSyncChatSearchPage(Origin, slice.Build(), DesktopSyncChatSearch::Global, {}).recognized);
	auto channel = NativeChannelMessages(); channel.count = MTP_int(1); channel.messages = MTP_vector<MTPMessage>({Message(90)});
	CHECK(MapDesktopSyncChatSearchPage(Origin, channel.Build(), DesktopSyncChatSearch::Dialog, PeerToken{PeerTokenKind::Channel, 456}).recognized);
	CHECK(!MapDesktopSyncChatSearchPage(Origin, channel.Build(), DesktopSyncChatSearch::Global, {}).recognized);
	CHECK(!MapDesktopSyncChatSearchPage(Origin, channel.Build(), DesktopSyncChatSearch::Dialog, PeerToken{PeerTokenKind::Channel, 457}).recognized);
	CHECK(!MapDesktopSyncChatSearchPage(Origin, MTP_messages_messagesNotModified(MTP_int(1)), DesktopSyncChatSearch::Global, {}).recognized);
	auto full = NativeMessages(); full.messages = MTP_vector<MTPMessage>({Message(1)});
	page = MapDesktopSyncChatSearchPage(Origin, full.Build(), DesktopSyncChatSearch::Global, {});
	CHECK(page.recognized && page.full && page.total == 1);
	auto pages = DesktopSyncChatSearchPages(Origin, DesktopSyncChatSearch::Global);
	CHECK(pages.Add(page) == DesktopSyncChatPageStatus::Complete);
}


DesktopSyncChatPeer Peer(PeerToken chat) {
	auto input = DesktopSyncChatPeerInput{Origin, chat}; input.canSendFiles = true; input.chosenSendAs = PeerToken{};
	if (chat == PeerToken{}) {
		auto user = NativeUser(); user.flags = MTP_flags(MTPDuser::Flag::f_self); user.id = MTP_long(123);
		auto full = NativeUserFull(); full.id = MTP_long(123); input.user = user.Build(); input.userFull = full.Build();
	} else {
		auto group = NativeChannel(); group.id = MTP_long(chat.id); group.flags = MTP_flags(MTPDchannel::Flag::f_broadcast | MTPDchannel::Flag::f_creator);
		auto full = NativeChannelFull(); full.id = MTP_long(chat.id); full.flags = MTP_flags(MTPDchannelFull::Flag::f_admins_count | MTPDchannelFull::Flag::f_participants_count);
		full.admins_count = MTP_int(1); full.participants_count = MTP_int(1); input.group = group.Build(); input.groupFull = full.Build();
	}
	return MapDesktopSyncChatPeer(Origin, input);
}

MTPMessage Mark(PeerToken chat, int id, int date = 100, const char *text = "#purplechatsync") {
	auto value = NativeMessage(); value.id = MTP_int(id); value.date = MTP_int(date); value.flags = MTP_flags(MTPDmessage::Flag::f_out);
	value.peer_id = chat == PeerToken{} ? MTP_peerUser(MTP_long(123)) : MTPPeer(MTP_peerChannel(MTP_long(chat.id)));
	value.message = MTP_string(text); return value.Build();
}

struct Scenario {
	std::vector<MTPMessage> hits;
	std::set<int> missing;
	bool failGlobal = false;
	bool failExact = false;
	bool readable = true;
	SyncChatSettingsHint hint = SyncChatSettingsHint::Absent;
	SyncChatRecordPresence inventory = SyncChatRecordPresence::Present;
	std::vector<DesktopSyncChatCommand> commands;
	std::optional<std::vector<MTPMessage>> exactOverride;
	bool unknownPeer = false;
};

void Drive(DesktopSyncChatDiscovery &discovery, Scenario &scenario, int64_t now) {
	for (auto step = 0; step < 100; ++step) {
		const auto command = discovery.command();
		if (command.step == DesktopSyncChatStep::Idle) return;
		scenario.commands.push_back(command);
		auto reply = DesktopSyncChatReply{Origin, command.ticket, true};
		if (command.step == DesktopSyncChatStep::Search) {
			if (scenario.failGlobal && command.search == DesktopSyncChatSearch::Global) reply.success = false;
			auto full = NativeMessages();
			auto messages = QVector<MTPMessage>();
			for (const auto &message : scenario.hits) {
				const auto target = DesktopSyncChatToken(message.c_message().vpeer_id(), 123);
				if (command.search == DesktopSyncChatSearch::Global || (target && *target == command.chat)) messages.push_back(message);
			}
			full.messages = MTP_vector<MTPMessage>(messages);
			const auto target = command.search == DesktopSyncChatSearch::Global || command.search == DesktopSyncChatSearch::Broadcasts
				? std::optional<PeerToken>() : std::optional(command.chat);
			reply.page = MapDesktopSyncChatSearchPage(Origin, full.Build(), command.search, target);
			reply.messages.assign(messages.begin(), messages.end());
		} else if (command.step == DesktopSyncChatStep::PeerInfo) {
			reply.peer = Peer(command.chat);
		} else if (command.step == DesktopSyncChatStep::ExactMark) {
			reply.success = !scenario.failExact; reply.chatReadable = scenario.readable; reply.peer = Peer(command.chat);
			if (command.mark->markId > 0) {
				if (scenario.missing.contains(command.mark->markId)) {
					reply.messages.push_back(MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flags()), MTP_int(command.mark->markId), MTPPeer()));
				} else {
					reply.messages.push_back(Mark(command.chat, command.mark->markId, command.mark->markDate));
				}
			}
			if (scenario.exactOverride) reply.messages = *scenario.exactOverride;
		} else if (command.step == DesktopSyncChatStep::SettingsHint) {
			reply.hint = scenario.hint;
		} else if (command.step == DesktopSyncChatStep::Inventory) {
			reply.records = scenario.inventory; reply.chatReadable = scenario.readable;
		}
		if (reply.peer && scenario.unknownPeer) reply.peer->facts.reset();
		CHECK(discovery.Complete(reply, now));
	}
	CHECK(false);
}

DesktopSyncChatCacheRecord Cache(PeerToken chat = {}, int id = 1, int date = 100) {
	auto value = DesktopSyncChatCacheRecord(); value.account = Origin.account; value.loginBinding = QString(64, u'a'); value.mark = {chat, id, date}; return value;
}

void CacheCases() {
	auto record = Cache(); record.title = u"Synthetic"_q; record.preserved.insert(u"extension"_q, QJsonObject{{u"keep"_q, true}});
	const auto bytes = SerializeDesktopSyncChatCache(record); CHECK(bytes);
	const auto parsed = ParseDesktopSyncChatCache(*bytes, Origin.account); CHECK(parsed.record && parsed.record->mark == record.mark && parsed.record->preserved.value(u"extension"_q).isObject());
	CHECK(ParseDesktopSyncChatCache(*bytes, {1,123}).status == DesktopSyncChatCacheStatus::Invalid);
	CHECK(ParseDesktopSyncChatCache(*bytes, {0,124}).status == DesktopSyncChatCacheStatus::Invalid);
	for (const auto &field : {u"version"_q, u"environment"_q, u"user"_q, u"chat"_q, u"mark_id"_q, u"mark_date"_q, u"title"_q, u"readers"_q, u"reader_consent"_q, u"login_binding"_q}) {
		auto object = QJsonDocument::fromJson(*bytes).object(); object.remove(field); CHECK(!ParseDesktopSyncChatCache(QJsonDocument(object).toJson(), Origin.account).record);
	}
	auto object = QJsonDocument::fromJson(*bytes).object(); object.insert(u"version"_q, u"2147483648"_q);
	CHECK(ParseDesktopSyncChatCache(QJsonDocument(object).toJson(), Origin.account).status == DesktopSyncChatCacheStatus::Newer);
	for (const auto &number : {u"-1"_q, u"01"_q, u"9223372036854775808"_q}) {
		object = QJsonDocument::fromJson(*bytes).object(); object.insert(u"mark_date"_q, number); CHECK(!ParseDesktopSyncChatCache(QJsonDocument(object).toJson(), Origin.account).record);
	}
	record.carry = QJsonObject(); CHECK(SerializeDesktopSyncChatCache(record));
	auto input = SyncChatResolveInput(); input.selfUserId = 123;
	auto resolution = SyncChatResolution(); resolution.action = SyncChatAction::Use; resolution.postingHeld = false; resolution.cache = record.mark;
	input.marks.push_back({record.mark, SyncChatMarkStatus::Supported, {SyncChatRefusal::None}, SyncChatRecordPresence::Present, true, true});
	CHECK(!DesktopSyncChatCacheCanCommit(record, input, resolution, true, true)); record.carry.reset();
	CHECK(DesktopSyncChatCacheCanCommit(record, input, resolution, true, true));
	CHECK(!DesktopSyncChatCacheCanCommit(record, input, resolution, false, true));
	CHECK(!DesktopSyncChatCacheCanCommit(record, input, resolution, true, false));
	for (const auto action : {SyncChatAction::Follow, SyncChatAction::Fill, SyncChatAction::ReadWinnerInventory}) {
		resolution.action = action; CHECK(!DesktopSyncChatCacheCanCommit(record, input, resolution, true, true));
	}
	resolution.action = SyncChatAction::Use; resolution.cache->chat = {PeerTokenKind::Channel, 456};
	CHECK(!DesktopSyncChatCacheCanCommit(record, input, resolution, true, true));
}


auto RetiredDisks = std::vector<std::shared_ptr<DesktopSyncChatDisk>>();
void RetireDisk(std::shared_ptr<DesktopSyncChatDisk> disk) {
	if (disk) RetiredDisks.push_back(std::move(disk));
}
struct ProbeChanges {
	Fn<void()> next;
	void fire_copy(Main::Session *) { if (next) next(); }
} Changes;

struct ProbeSession : base::has_weak_ptr {
	struct Model {
		DesktopSyncChatCommand value;
		DesktopSyncChatCommand command() const { return value; }
		void Cancel() {}
		DesktopSyncChatStatus status() const { return {}; }
	};
	bool _busy = true;
	Model model;
	Model *_model = &model;
	uint64_t _run = 11;
	DesktopSyncChatOrigin _origin = Origin;
	bool current = true;
	int cancelled = 0;
	QString reason;
	DesktopSyncChatStatus _status;
	std::shared_ptr<std::atomic<bool>> _current = std::make_shared<std::atomic<bool>>(true);
	std::shared_ptr<DesktopSyncChatDisk> _disk;
	std::unique_ptr<int> _api;
	std::unique_ptr<int> _inventory;
	std::vector<Fn<void(DesktopSyncChatStatus)>> _callbacks;
	struct Timer { void cancel() {} } _timer, _requestTimeout;
	struct Session { Main::Session *value = nullptr; Main::Session *get() const { return value; } } _session;
	bool Current() const { return current; }
	void Cancel(QString value) { ++cancelled; reason = value; ProductionCancel(value); }
	void ProductionCancel(QString reason);
	void Publish();
	bool Accept(const DesktopSyncChatCommand &command);
};

struct ProbeOwnership {
	Fn<SyncChatMessageOwnership(const MTPMessage &)> _owned;
	SyncHistoryScanStatus status = SyncHistoryScanStatus::Complete;
	SyncCandidateStatus read = SyncCandidateStatus::Valid;
	bool accepted = false;
	void Finish(SyncHistoryScanStatus value) { status = value; }
	void CompleteCurrent(SyncCandidateRecord value) { read = value.status; }
};

#include "sync-chat-production-ports.inc"

void CallbackCases() {
	{
		auto self = std::make_unique<ProbeSession>(); self->_status.postingHeld = false;
		self->_session.value = reinterpret_cast<Main::Session*>(uintptr_t(1));
		auto delivered = DesktopSyncChatStatus();
		self->_callbacks.push_back([&](DesktopSyncChatStatus value) { delivered = value; });
		Changes.next = [&] { self.reset(); };
		self->Publish();
		CHECK(!self && delivered.postingHeld && delivered.reason == u"completion-superseded"_q);
		Changes.next = {};
	}

	for (const auto wasHeld : {false, true}) {
		auto self = ProbeSession(); self._status.postingHeld = wasHeld; self._status.reason = u"original-held"_q;
		self._session.value = reinterpret_cast<Main::Session*>(uintptr_t(1));
		auto delivered = DesktopSyncChatStatus();
		self._callbacks.push_back([&](DesktopSyncChatStatus status) { delivered = status; });
		Changes.next = [&] { ++self._run; self.current = false; };
		self.Publish();
		CHECK(delivered.postingHeld);
		CHECK(delivered.reason == (wasHeld ? u"original-held"_q : u"completion-superseded"_q));
		Changes.next = {};
	}
	{
		auto root = QTemporaryDir(); root.setAutoRemove(false); CHECK(root.isValid());
		auto current = std::make_shared<std::atomic<bool>>(true);
		auto disk = OpenDesktopSyncChatDisk(root.path(), root.path() + u"/sync"_q,
			root.path() + u"/sync/state.json"_q, Origin.account, {}, QString(64, u'a'), current);
		CHECK(disk->error.isEmpty());
		auto self = ProbeSession(); self._disk = disk;
		self.model.value = {Origin, 5}; auto command = self.model.value; command.run = self._run;
		self.current = false; CHECK(!self.Accept(command));
		CHECK(!self._current->load() && !self._busy && !self._disk && !RetiredDisks.empty());
		CHECK(disk->lock != nullptr);
		for (const auto &retired : RetiredDisks) retired->lock.reset();
		RetiredDisks.clear();
		CHECK(disk->lock == nullptr);
		auto replacement = QLockFile(root.path() + u"/sync/lock"_q); CHECK(replacement.tryLock(0));
	}

	for (const auto &changed : {u"work-mode"_q, u"app-wide-source"_q, u"logout"_q}) {
		auto self = ProbeSession();
		self.model.value = {Origin, 7};
		auto command = self.model.value; command.run = self._run;
		CHECK(self.Accept(command) && self.cancelled == 0);
		self.current = false;
		CHECK(!self.Accept(command) && self.cancelled == 1 && !self._busy);
		CHECK(self.reason == u"origin-retired"_q);
	}
	for (auto obsolete = 0; obsolete < 4; ++obsolete) {
		auto self = ProbeSession(); self.current = false; self.model.value = {Origin, 7};
		auto command = self.model.value; command.run = self._run;
		if (obsolete == 0) --command.run;
		if (obsolete == 1) --command.ticket;
		if (obsolete == 2) command.origin.login = u"old-login"_q;
		if (obsolete == 3) command.origin.account.environment = 1;
		CHECK(!self.Accept(command) && self.cancelled == 0 && self._busy);
	}
}

void OwnershipPorts() {
	auto native = NativeMessage(); native.id = MTP_int(9); native.date = MTP_int(100);
	native.peer_id = MTP_peerUser(MTP_long(123)); native.message = MTP_string("#purplesync");
	native.flags = MTP_flags(MTPDmessage::Flag::f_media);
	auto media = NativeMessageMediaDocument(); media.flags = MTP_flags(MTPDmessageMediaDocument::Flag::f_document);
	media.document = MTP_document(MTP_flags(MTPDdocument::Flags()), MTP_long(10), MTP_long(20), MTP_bytes(QByteArray()), MTP_int(100), MTP_string("application/json"), MTP_long(10), MTP_vector<MTPPhotoSize>(), MTP_vector<MTPVideoSize>(), MTP_int(1), MTP_vector<MTPDocumentAttribute>(1, MTP_documentAttributeFilename(MTP_string("purple.json"))));
	native.media = media.Build();
	const auto message = native.Build();
	CHECK(IsSyncHistoryCandidate(message));
	for (const auto verdict : {SyncChatMessageOwnership::Own, SyncChatMessageOwnership::Other, SyncChatMessageOwnership::Unknown}) {
		auto self = ProbeOwnership(); self._owned = [=](const MTPMessage &) { return verdict; };
		auto page = std::vector<SyncHistoryPageItem>(); const auto messages = QVector<MTPMessage>{message};
		ProbeScan(&self, &messages, page);
		CHECK(self.status == (verdict == SyncChatMessageOwnership::Unknown ? SyncHistoryScanStatus::InvalidResponse : SyncHistoryScanStatus::Complete));
		CHECK(verdict == SyncChatMessageOwnership::Unknown ? page.empty() : page.size() == 1 && page[0].candidate == (verdict == SyncChatMessageOwnership::Own));
		ProbeReadOwnership(&self, message, 9);
		CHECK(self.accepted == (verdict == SyncChatMessageOwnership::Own));
		CHECK(self.read == (verdict == SyncChatMessageOwnership::Unknown ? SyncCandidateStatus::Inaccessible : verdict == SyncChatMessageOwnership::Other ? SyncCandidateStatus::Changed : SyncCandidateStatus::Valid));
	}
}

void DiskCases() {
	{
		auto directory = QTemporaryDir(); directory.setAutoRemove(false); CHECK(directory.isValid());
		const auto root = directory.path(); const auto sync = root + u"/sync"_q;
		CHECK(EnsureSyncPrivateDirectory(sync) == SyncStoreStatus::Ready);
		auto state = SyncLocalState(); state.install = u"in-"_q + QString(26, u'a');
		state.space = u"sp-"_q + QString(26, u'a'); state.createdDevice = u"Synthetic"_q; state.bindingToken = QString(32, u'a');
		const auto serialized = SerializeSyncLocalState(state); CHECK(serialized);
		const auto path = sync + u"/state.json"_q;
		CHECK(WriteSyncPrivateFile(path, serialized.canonical) == SyncStoreStatus::Ready);
		auto current = std::make_shared<std::atomic<bool>>(true);
		auto disk = OpenDesktopSyncChatDisk(root, sync, path, Origin.account, state.bindingToken.toLatin1(), QString(64, u'a'), current);
		CHECK(disk->error.isEmpty() && disk->settingsBound && disk->settingsChat == PeerToken{});
		CHECK(SaveDesktopSyncChatDisk(disk, Cache(), current));
		auto file = QFile(path); CHECK(file.open(QIODevice::Append)); CHECK(file.write(" ") == 1); file.close();
		CHECK(SyncPathIdentity(path) == disk->stateIdentity);
		CHECK(!SaveDesktopSyncChatDisk(disk, {}, current)); disk->lock.reset();
		CHECK(!OpenDesktopSyncChatDisk(root, sync, path, Origin.account, QByteArray(32, 'b'), QString(64, u'a'), current)->error.isEmpty());
		CHECK(WriteSyncPrivateFile(path + u".good"_q, serialized.canonical) == SyncStoreStatus::Ready);
		CHECK(WriteSyncPrivateFile(path, "{\"version\":3}") == SyncStoreStatus::Ready);
		CHECK(!OpenDesktopSyncChatDisk(root, sync, path, Origin.account, state.bindingToken.toLatin1(), QString(64, u'a'), current)->error.isEmpty());
	}

	const auto login = QString(64, u'a');
	const auto open = [&](const QString &root, const QString &sync, const std::shared_ptr<std::atomic<bool>> &current, bool borrowed = false, QString authority = QString(64, u'a')) {
		return OpenDesktopSyncChatDisk(root, sync, sync + u"/state.json"_q, Origin.account, {}, authority, current, borrowed);
	};
	for (auto n = 0; n < 11; ++n) {
		auto directory = QTemporaryDir(); CHECK(directory.isValid()); directory.setAutoRemove(false);
		const auto root = directory.path(); const auto sync = root + u"/sync"_q;
		auto current = std::make_shared<std::atomic<bool>>(true);
		auto disk = open(root, sync, current); CHECK(disk->error.isEmpty() && disk->identity == "missing" && !disk->settingsBound);
		auto record = Cache();
		if (n == 0) {
			CHECK(SaveDesktopSyncChatDisk(disk, record, current)); disk->lock.reset();
			auto again = open(root, sync, current); CHECK(again->cache && again->cache->mark == record.mark);
			CHECK(!open(root, sync, current)->error.isEmpty());
			CHECK(open(root, sync, current, true)->error.isEmpty());
			current->store(false); CHECK(!SaveDesktopSyncChatDisk(again, record, current));
		} else if (n == 1) {
			current->store(false); CHECK(!SaveDesktopSyncChatDisk(disk, record, current));
			CHECK(SyncPathIdentity(disk->cachePath) == "missing");
		} else if (n == 2) {
			CHECK(WriteSyncPrivateFile(disk->statePath, "corrupt") == SyncStoreStatus::Ready);
			CHECK(!SaveDesktopSyncChatDisk(disk, record, current));
		} else if (n == 3) {
			record.account.environment = 1; CHECK(!SaveDesktopSyncChatDisk(disk, record, current));
		} else if (n == 4) {
			record.loginBinding = QString(64, u'b'); CHECK(!SaveDesktopSyncChatDisk(disk, record, current));
		} else {
			auto bytes = *SerializeDesktopSyncChatCache(record);
			if (n == 5) bytes = "corrupt";
			if (n == 6) bytes = "{\"version\":\"2147483648\"}";
			if (n == 7) bytes = QByteArray(64 * 1024 + 1, 'a');
			if (n == 8) {
				const auto target = root + u"/target"_q; CHECK(WriteSyncPrivateFile(target, bytes) == SyncStoreStatus::Ready);
				CHECK(::symlink(QFile::encodeName(target).constData(), QFile::encodeName(disk->cachePath).constData()) == 0);
			} else CHECK(WriteSyncPrivateFile(disk->cachePath, bytes) == SyncStoreStatus::Ready);
			disk->lock.reset();
			const auto held = open(root, sync, current, false, n == 9 ? QString(64, u'b') : login);
			if (n != 10) {
				CHECK(!held->error.isEmpty());
				if (n != 8) { auto after = QByteArray(); CHECK(ReadSyncPrivateFile(disk->cachePath, 70 * 1024, after) == SyncStoreStatus::Ready && after == bytes); }
			} else {
				CHECK(held->error.isEmpty());
				CHECK(SaveDesktopSyncChatDisk(held, {}, current));
				CHECK(WriteSyncPrivateFile(held->cachePath, bytes + ' ') == SyncStoreStatus::Ready);
				CHECK(!SaveDesktopSyncChatDisk(held, record, current));
			}
		}
	}
}

void DiscoveryCases() {
	{
		auto cache = Cache(); cache.loginBinding = QString(64, u'b');
		auto discovery = DesktopSyncChatDiscovery(Origin, cache, true, {}, {}, QString(64, u'a'));
		discovery.Start(1000);
		CHECK(discovery.command().step == DesktopSyncChatStep::Idle && discovery.status().postingHeld);
		CHECK(discovery.cache()->loginBinding == cache.loginBinding);
	}

	for (auto mode = 0; mode < 6; ++mode) {
		auto cache = Cache(); auto discovery = DesktopSyncChatDiscovery(Origin, cache, true, {}, {}, QString(64, u'a'));
		discovery.Start(1000); auto scenario = Scenario{{Mark({}, 1)}};
		if (mode == 0) scenario.exactOverride = std::vector<MTPMessage>{Mark({}, 2)};
		if (mode == 1) scenario.exactOverride = std::vector<MTPMessage>{Mark({}, 1, 999)};
		if (mode == 2) scenario.exactOverride = std::vector<MTPMessage>{Mark({PeerTokenKind::Channel, 456}, 1)};
		if (mode == 3) scenario.exactOverride = std::vector<MTPMessage>{Mark({}, 1), Mark({}, 1)};
		if (mode == 4) scenario.readable = false;
		if (mode == 5) scenario.unknownPeer = true;
		Drive(discovery, scenario, 1000);
		CHECK(discovery.status().phase == DesktopSyncChatPhase::Paused && discovery.status().postingHeld);
		CHECK(discovery.cache()->mark == cache.mark);
		CHECK(SerializeDesktopSyncChatCache(*discovery.cache()) == SerializeDesktopSyncChatCache(cache));
	}

	{
		auto discovery = DesktopSyncChatDiscovery(Origin, {}, false, {}, {}, QString(64, u'a')); discovery.Start(1000);
		const auto opening = discovery.command();
		auto wrong = DesktopSyncChatReply{Origin, opening.ticket, true}; wrong.origin.login = u"new-login"_q;
		CHECK(!discovery.Complete(wrong, 1000) && discovery.command().ticket == opening.ticket);
		wrong.origin = Origin; ++wrong.ticket; CHECK(!discovery.Complete(wrong, 1000));
		auto scenario = Scenario{{Mark({}, 10)}}; Drive(discovery, scenario, 1000);
		CHECK(discovery.status().phase == DesktopSyncChatPhase::Use && !discovery.status().postingHeld && discovery.cache()->mark.markId == 10);
		CHECK(std::any_of(scenario.commands.begin(), scenario.commands.end(), [](const auto &command) { return command.step == DesktopSyncChatStep::Search && command.search == DesktopSyncChatSearch::Self; }));
		discovery.Start(1100); const auto fresh = discovery.command(); CHECK(fresh.step == DesktopSyncChatStep::Search);
		CHECK(!discovery.Complete({Origin, opening.ticket, true}, 1100) && discovery.command().ticket == fresh.ticket);
		discovery.Cancel(); CHECK(discovery.status().postingHeld && discovery.command().step == DesktopSyncChatStep::Idle);
		CHECK(!discovery.Complete({Origin, fresh.ticket, true}, 1100));
	}
	{
		auto cache = Cache(); auto discovery = DesktopSyncChatDiscovery(Origin, cache, true, {}, {}, QString(64, u'a'));
		discovery.Start(1000); auto scenario = Scenario{{Mark({}, 1), Mark({PeerTokenKind::Channel, 456}, 2, 200)}}; Drive(discovery, scenario, 1000);
		CHECK(discovery.status().phase == DesktopSyncChatPhase::Paused && discovery.status().reason == u"follow-required"_q);
		CHECK(discovery.cache()->mark == cache.mark && discovery.status().postingHeld);
	}
	for (const auto failSearch : {false, true}) {
		auto cache = Cache(); auto discovery = DesktopSyncChatDiscovery(Origin, cache, true, {}, {}, QString(64, u'a')); discovery.Start(1000);
		auto scenario = Scenario(); scenario.failGlobal = failSearch; scenario.failExact = !failSearch; Drive(discovery, scenario, 1000);
		CHECK(discovery.status().postingHeld && discovery.cache()->mark == cache.mark && discovery.status().phase == DesktopSyncChatPhase::Paused);
	}
	{
		auto cache = Cache(); auto discovery = DesktopSyncChatDiscovery(Origin, cache, true, {}, {}, QString(64, u'a')); discovery.Start(1000);
		auto scenario = Scenario(); scenario.missing.insert(1);
		for (const auto now : {1000,1030,1150,1750}) {
			if (now != 1000) discovery.Tick(now);
			Drive(discovery, scenario, now);
			CHECK(discovery.status().phase == (now == 1750 ? DesktopSyncChatPhase::Lost : DesktopSyncChatPhase::Searching));
			CHECK(discovery.status().postingHeld && discovery.cache()->mark == cache.mark);
			if (now == 1000) { const auto count = scenario.commands.size(); discovery.Tick(1029); Drive(discovery, scenario, 1029); CHECK(scenario.commands.size() == count); }
		}
	}
	{
		auto discovery = DesktopSyncChatDiscovery(Origin, {}, false, {}, {}, QString(64, u'a')); discovery.Start(1000);
		auto scenario = Scenario(); scenario.hint = SyncChatSettingsHint::Found; scenario.inventory = SyncChatRecordPresence::Unknown;
		Drive(discovery, scenario, 1000); CHECK(discovery.status().phase == DesktopSyncChatPhase::Paused && !discovery.cache());
	}
	{
		auto discovery = DesktopSyncChatDiscovery(Origin, {}, false, {}, {}, QString(64, u'a')); discovery.Start(1000);
		auto scenario = Scenario{{Mark({PeerTokenKind::Channel, 456}, 1, 100), Mark({PeerTokenKind::Channel, 457}, 2, 200)}}; scenario.inventory = SyncChatRecordPresence::Empty;
		Drive(discovery, scenario, 1000); CHECK(discovery.status().reason == u"fill-required"_q && !discovery.cache());
	}
}

}

int main() {
	Requests();
	Pagination();
	NativeReplies();
	CacheCases();
	DiscoveryCases();
	CallbackCases();
	OwnershipPorts();
	DiskCases();
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
