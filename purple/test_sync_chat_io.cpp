#define main AuthorMain
#include "test_sync_chat_discovery.cpp"
#undef main

#include "purple/purple_sync_chat_io.h"
#include "purple/purple_config_sync.h"
#include "purple/purple_sync_auto_transport.h"
#include "mtproto/mtproto_response.h"

namespace {

constexpr auto kMaximumRecordBytes = kSyncRecordMaximumBytes;

DesktopSyncChatIo Route(PeerToken token, bool broadcast = true, bool bot = false) {
	auto input = DesktopSyncChatPeerInput{Origin, token};
	input.canSendFiles = true;
	input.chosenSendAs = PeerToken{};
	auto endpoint = MTPInputPeer(MTP_inputPeerSelf());
	if (token == PeerToken{}) {
		return { Peer({}), endpoint };
	} else if (token.kind == PeerTokenKind::User) {
		auto user = NativeUser(); user.id = MTP_long(token.id);
		user.flags = MTP_flags(bot ? MTPDuser::Flag::f_bot : MTPDuser::Flag());
		auto full = NativeUserFull(); full.id = MTP_long(token.id);
		input.user = user.Build(); input.userFull = full.Build();
		endpoint = MTP_inputPeerUser(MTP_long(token.id), MTP_long(789));
	} else if (token.kind == PeerTokenKind::Chat) {
		auto chat = NativeChat(); chat.id = MTP_long(token.id);
		chat.flags = MTP_flags(MTPDchat::Flag::f_creator);
		auto full = NativeChatFull(); full.id = MTP_long(token.id);
		full.participants = MTP_chatParticipants(MTP_long(token.id),
			MTP_vector<MTPChatParticipant>({ MTP_chatParticipantCreator(
				MTP_flags(MTPDchatParticipantCreator::Flags()), MTP_long(123), MTP_string()) }), MTP_int(1));
		input.group = chat.Build(); input.groupFull = full.Build();
		endpoint = MTP_inputPeerChat(MTP_long(token.id));
	} else {
		auto chat = NativeChannel(); chat.id = MTP_long(token.id);
		chat.flags = MTP_flags(MTPDchannel::Flag::f_creator
			| (broadcast ? MTPDchannel::Flag::f_broadcast : MTPDchannel::Flag::f_megagroup));
		auto full = NativeChannelFull(); full.id = MTP_long(token.id);
		full.flags = MTP_flags(MTPDchannelFull::Flag::f_admins_count | MTPDchannelFull::Flag::f_participants_count);
		full.admins_count = MTP_int(1); full.participants_count = MTP_int(1);
		input.group = chat.Build(); input.groupFull = full.Build();
		endpoint = MTP_inputPeerChannel(MTP_long(token.id), MTP_long(789));
	}
	return { MapDesktopSyncChatPeer(Origin, input), endpoint };
}

MTPPeer Target(PeerToken token) {
	return token.kind == PeerTokenKind::Self ? MTPPeer(MTP_peerUser(MTP_long(123)))
		: token.kind == PeerTokenKind::User ? MTPPeer(MTP_peerUser(MTP_long(token.id)))
		: token.kind == PeerTokenKind::Chat ? MTPPeer(MTP_peerChat(MTP_long(token.id)))
		: MTPPeer(MTP_peerChannel(MTP_long(token.id)));
}

QByteArray RecordBytes(int seq = 1) {
	auto input = ConfigRecordBuildInput();
	input.text = "version = 1\n";
	input.space = u"sp-"_q + QString(26, u'a');
	input.install = u"in-"_q + QString(26, u'a');
	input.device = u"Synthetic"_q; input.platform = u"unit"_q;
	input.app = u"Purple unit"_q; input.seq = input.at = seq;
	const auto built = BuildConfigRecord(input);
	CHECK(built);
	return built.canonical;
}

MTPMessage DocumentMessage(const DesktopSyncChatIo &route, int id = 10) {
	auto document = NativeDocument(); document.id = MTP_long(99);
	document.access_hash = MTP_long(789); document.dc_id = MTP_int(2);
	document.size = MTP_long(RecordBytes().size());
	document.attributes = MTP_vector<MTPDocumentAttribute>({
		MTP_documentAttributeFilename(MTP_string(SyncSettingsRecordFileName())) });
	auto media = NativeMessageMediaDocument();
	media.flags = MTP_flags(MTPDmessageMediaDocument::Flag::f_document);
	media.document = document.Build();
	auto message = NativeMessage();
	message.flags = MTP_flags(MTPDmessage::Flag::f_media | MTPDmessage::Flag::f_from_id
		| (route.peer.chat == PeerToken{} ? MTPDmessage::Flag() : MTPDmessage::Flag::f_out));
	message.id = MTP_int(id); message.date = MTP_int(100);
	message.peer_id = Target(route.peer.chat); message.from_id = MTP_peerUser(MTP_long(123));
	message.message = MTP_string("#purplesync"); message.media = media.Build();
	return message.Build();
}

MTPmessages_Messages Response(const DesktopSyncChatIo &route,
		const QVector<MTPMessage> &messages) {
	if (route.peer.chat.kind == PeerTokenKind::Channel) {
		auto response = NativeChannelMessages(); response.count = MTP_int(messages.size());
		response.messages = MTP_vector<MTPMessage>(messages); return response.Build();
	}
	auto response = NativeMessages(); response.messages = MTP_vector<MTPMessage>(messages);
	return response.Build();
}

struct ScanProbe : base::has_weak_ptr {
	explicit ScanProbe(const DesktopSyncChatIo &route)
	: _modernHistory(route.peer.origin.account.userId, route.peer.chat)
	, _owned([peer = route.peer](const MTPMessage &message) {
		return GetSyncChatMessageOwnership(MapDesktopSyncChatMessage(peer.origin, peer, message));
	}) {}
	SyncHistoryPages _pages;
	DesktopSyncChatHistoryGuard _modernHistory;
	Fn<SyncChatMessageOwnership(const MTPMessage &)> _owned;
	Fn<void(uint64_t)> _progress;
	mtpRequestId _requestId = 1;
	bool _done = false;
	bool current = true;
	SyncHistoryScanStatus status = SyncHistoryScanStatus::InvalidResponse;
	int next = 0;
	bool Current() const { return current; }
	void Finish(SyncHistoryScanStatus value, QString = {}) { if (_done) return; status = value; _done = true; }
	void RequestNext() { ++next; }
	void Fail(const MTP::Error &error) {
		const auto weak = base::make_weak(this);
		const auto callback = [weak](const MTP::Error &error)
#include "typed-scan-failure.inc"
		;
		callback(error);
	}
	void Reply(const MTPmessages_Messages &result) {
		const auto weak = base::make_weak(this);
		const auto callback = [weak](const MTPmessages_Messages &result)
#include "typed-scan-reply.inc"
		;
		callback(result);
	}
};

struct DownloadProbe {
	QByteArray data;
	bool stopped = false;
	bool cancelled() const { return stopped; }
	const QByteArray &bytes() const { return data; }
};
struct StubUser { PeerId id = peerFromUser(UserId(123)); };
struct StubSession { StubUser self; StubUser *user() { return &self; } UserId userId() const { return UserId(123); } };

struct ReaderProbe : base::has_weak_ptr {
	explicit ReaderProbe(DesktopSyncChatIo route)
	: _route(std::move(route))
	, _owned([peer = _route->peer](const MTPMessage &message) {
		return GetSyncChatMessageOwnership(MapDesktopSyncChatMessage(peer.origin, peer, message));
	}) {}
	std::optional<DesktopSyncChatIo> _route;
	Fn<SyncChatMessageOwnership(const MTPMessage &)> _owned;
	StubSession session;
	StubSession *_session = &session;
	std::vector<int32_t> _candidateIds = {10};
	size_t _next = 0;
	mtpRequestId _requestId = 1;
	bool _done = false;
	bool current = true;
	bool downloading = false;
	bool _downloadFailed = false;
	uint64_t _documentId = 0;
	uint64_t _editDate = 0;
	int64_t _expectedBytes = 0;
	std::unique_ptr<DownloadProbe> _loader;
	std::optional<FullMsgId> origin;
	std::optional<SyncCandidateRecord> record;
	std::optional<SyncCandidateRecord> _downloaded;
	bool rereading = false;
	void RequestNext() { rereading = true; }
	bool Current() const { return current; }
	void Cancel() { _done = true; CompleteCurrent({ .id = 10, .status = SyncCandidateStatus::Cancelled }); }
	void CompleteCurrent(SyncCandidateRecord value) {
		value.documentId = _documentId; value.editDate = _editDate; record = std::move(value); _done = true;
	}
	void StartDownload(const MTPDdocument &document) {
		downloading = true; _expectedBytes = document.vsize().v;
		origin = DesktopSyncChatFileOrigin(*_route, 10);
	}
	void Reply(const MTPmessages_Messages &result) {
		const auto weak = base::make_weak(this);
		const auto id = _candidateIds[_next];
		const auto callback = [weak, id](const MTPmessages_Messages &result)
#include "typed-reader-reply.inc"
		;
		callback(result);
	}
	void Fail(const MTP::Error &error) {
		const auto weak = base::make_weak(this); const auto id = _candidateIds[_next];
		const auto callback = [weak, id](const MTP::Error &)
#include "typed-reader-failure.inc"
		;
		callback(error);
	}
	void OnDownload()
#include "typed-download.inc"
};

struct ExactReaderEndpoint {
	std::optional<DesktopSyncChatIo> route;
	Fn<void(SyncCandidateReadResult)> finished;
	ExactReaderEndpoint(StubSession *, std::vector<int32_t>,
		Fn<void(SyncCandidateReadResult)> done) : finished(std::move(done)) {}
	ExactReaderEndpoint(StubSession *, DesktopSyncChatIo value,
		std::vector<int32_t>, Fn<void(SyncCandidateReadResult)> done,
		Fn<bool()>) : route(std::move(value)), finished(std::move(done)) {}
	void Start() {}
};

struct ExactProbe : base::has_weak_ptr {
	explicit ExactProbe(DesktopSyncChatIo route) : _route(std::move(route)) {}
	std::optional<DesktopSyncChatIo> _route;
	int32_t _id = 10;
	StubSession session;
	StubSession *_session = &session;
	DesktopSyncExactMetadata _metadata;
	SyncCandidateRecord _record;
	std::unique_ptr<ExactReaderEndpoint> _reader;
	Fn<bool()> _current = [] { return true; };
	bool current = true;
	bool requestedReadback = false;
	std::optional<SyncAutoServerRead> result;
	bool Current() const { return current && !result; }
	void Finish(SyncAutoServerRead value) { result = std::move(value); }
	void Metadata(bool readback) { requestedReadback = readback; }
	void Reply(const MTPmessages_Messages &value, bool readback) {
		const auto weak = base::make_weak(this);
		const auto callback = [weak, readback](const MTPmessages_Messages &result)
#define SyncCandidateReader ExactReaderEndpoint
#include "typed-exact-reply.inc"
#undef SyncCandidateReader
		;
		callback(value);
	}
};

void ExactReadback() {
	for (const auto &route : {Route({}), Route({PeerTokenKind::User,456}),
		Route({PeerTokenKind::Chat,456}), Route({PeerTokenKind::Channel,456}),
		Route({PeerTokenKind::Channel,456},false), Route({PeerTokenKind::User,456},true,true)}) {
		const auto chat = route.peer.chat; const auto message = DocumentMessage(route);
		const auto metadata = InspectDesktopSyncExactMessage(Response(route,{message}),10,route);
		CHECK(metadata.presence == SyncAutoMessagePresence::Present && metadata.owned && metadata.documentId == 99);
		for (auto mode = 0; mode < 7; ++mode) {
			auto probe = ExactProbe(route); probe.Reply(Response(route,{message}),false);
			CHECK(probe._reader && probe._reader->route && probe._reader->route->peer.chat == chat);
			auto record = ClassifySyncCandidate(10,RecordBytes()); record.documentId = mode == 1 ? 100 : 99;
			if (mode == 2) record.editDate = 1;
			if (mode == 3) probe.current = false;
			probe._reader->finished({mode == 4 ? SyncCandidateReadStatus::Incomplete : SyncCandidateReadStatus::Complete,{record}});
			if (mode == 1 || mode == 2 || mode == 3 || mode == 4) {
				CHECK(probe.result && !probe.result->fresh && probe.result->record.isEmpty()); continue;
			}
			CHECK(probe.requestedReadback && !probe.result);
			if (mode == 5) probe.Reply(Response(route,{DocumentMessage(Route({PeerTokenKind::Channel,789}))}),true);
			else if (mode == 6) { probe.current = false; probe.Reply(Response(route,{message}),true); }
			else probe.Reply(Response(route,{message}),true);
			CHECK(probe.result && (mode == 0 ? probe.result->fresh && probe.result->record == RecordBytes()
				: !probe.result->fresh && probe.result->record.isEmpty()));
		}
		const auto missing = MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flag::f_peer_id),MTP_int(10),Target(chat));
		CHECK(InspectDesktopSyncExactMessage(Response(route,{missing}),10,route).presence == SyncAutoMessagePresence::Absent);
		const auto ambiguous = MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flags()),MTP_int(10),MTPPeer());
		CHECK((InspectDesktopSyncExactMessage(Response(route,{ambiguous}),10,route).presence == SyncAutoMessagePresence::Absent)
			== (chat == PeerToken{} || chat.kind == PeerTokenKind::Channel));
	}
}

void RoutesAndWire() {
	for (const auto &route : {Route({}), Route({PeerTokenKind::User,456}),
		Route({PeerTokenKind::Chat,456}), Route({PeerTokenKind::Channel,456}),
		Route({PeerTokenKind::Channel,456},false), Route({PeerTokenKind::User,456},true,true)}) {
		const auto chat = route.peer.chat;
		CHECK(ValidDesktopSyncChatIo(route));
		const auto origin = DesktopSyncChatFileOrigin(route,10);
		CHECK(origin && origin->peer == (chat == PeerToken{} ? peerFromUser(UserId(123))
			: chat.kind == PeerTokenKind::User ? peerFromUser(UserId(456))
			: chat.kind == PeerTokenKind::Chat ? peerFromChat(ChatId(456)) : peerFromChannel(ChannelId(456))));
		CHECK(origin && origin->msg == MsgId(10));
		const auto history = DesktopSyncChatHistoryRequest(route,50);
		CHECK(history);
		auto wire = mtpBuffer(); history->write(wire);
		auto from = wire.constData(); const auto end = from + wire.size();
		CHECK(Read<MTPint>(from,end).v == int(mtpc_messages_getHistory));
		CHECK(Read<MTPInputPeer>(from,end).type() == route.input.type());
		CHECK(Read<MTPint>(from,end).v == 50);
		const auto exact = DesktopSyncChatRecordRequest(route,10);
		CHECK(exact && std::holds_alternative<MTPchannels_GetMessages>(*exact) == (chat.kind == PeerTokenKind::Channel));
		wire.clear(); std::visit([&](const auto &request) { request.write(wire); },*exact);
		from = wire.constData(); const auto limit = from + wire.size();
		CHECK(Read<MTPint>(from,limit).v == int(chat.kind == PeerTokenKind::Channel ? mtpc_channels_getMessages : mtpc_messages_getMessages));
		if (chat.kind == PeerTokenKind::Channel) {
			const auto channel = Read<MTPInputChannel>(from,limit);
			CHECK(channel.c_inputChannel().vchannel_id().v == 456 && channel.c_inputChannel().vaccess_hash().v == 789);
		}
		const auto ids = Read<MTPVector<MTPInputMessage>>(from,limit);
		CHECK(ids.v.size() == 1 && ids.v[0].c_inputMessageID().vid().v == 10 && from == limit);
		for (auto bad = 0; bad < 7; ++bad) {
			auto changed = route;
			if (bad == 0) changed.input = MTP_inputPeerEmpty();
			if (bad == 1) changed.peer.origin.login.clear();
			if (bad == 2) changed.peer.origin.account.environment = 2;
			if (bad == 3) changed.peer.facts.reset();
			if (bad == 4) changed.peer.chat.id = 999;
			if (bad == 5) changed.peer.kind = SyncChatKind::Unknown;
			if (bad == 6) changed.input = MTP_inputPeerUser(MTP_long(123), MTP_long(1));
			CHECK(!ValidDesktopSyncChatIo(changed));
			CHECK(!DesktopSyncChatHistoryRequest(changed,0) && !DesktopSyncChatRecordRequest(changed,10));
		}
		CHECK(!DesktopSyncChatRecordRequest(route,0));
		CHECK(!DesktopSyncChatHistoryRequest(route,-1));
	}
	CHECK(ValidDesktopSyncChatIo(Route({PeerTokenKind::Channel,456},false)));
	CHECK(ValidDesktopSyncChatIo(Route({PeerTokenKind::User,456},true,true)));
}

void FullReadPath() {
	for (const auto &route : {Route({}), Route({PeerTokenKind::User,456}),
		Route({PeerTokenKind::Chat,456}), Route({PeerTokenKind::Channel,456}),
		Route({PeerTokenKind::Channel,456},false), Route({PeerTokenKind::User,456},true,true)}) {
		const auto chat = route.peer.chat;
		const auto message = DocumentMessage(route);
		CHECK(GetSyncChatMessageOwnership(MapDesktopSyncChatMessage(Origin,route.peer,message)) == SyncChatMessageOwnership::Own);
		auto scan = ScanProbe(route); scan.Reply(Response(route,{message}));
		if (!scan._done) scan.Reply(Response(route,{}));
		CHECK(scan.status == SyncHistoryScanStatus::Complete && scan._pages.candidates() == std::vector<int32_t>{10});
		auto reader = ReaderProbe(route); reader.Reply(Response(route,{message}));
		CHECK(reader.downloading && reader.origin == DesktopSyncChatFileOrigin(route,10));
		reader._loader = std::make_unique<DownloadProbe>(); reader._loader->data = RecordBytes(); reader.OnDownload();
		CHECK(reader.rereading && !reader.record);
		reader.Reply(Response(route,{message}));
		CHECK(reader.record && reader.record->status == SyncCandidateStatus::Valid && reader.record->documentId == 99);
		auto read = SyncCandidateReadResult(); read.records = {*reader.record}; read.status = AggregateSyncCandidateRead(read.records);
		const auto raw = FinishSyncAccountInventory(123,{scan.status,scan._pages.candidates(),scan._pages.count()},read);
		const auto inventory = DesktopSyncChatInventory{route,raw};
		CHECK(DesktopSyncChatInventoryMatches(inventory,Origin,chat));
		CHECK(MapDesktopSyncChatInventoryReply(Origin,1,chat,inventory).records == SyncChatRecordPresence::Present);
		for (auto bad = 0; bad < 4; ++bad) {
			auto changed = inventory;
			if (bad == 0) changed.route.peer.origin.account.environment = 1;
			if (bad == 1) changed.route.peer.origin.login = u"other"_q;
			if (bad == 2) changed.route.peer.chat = {PeerTokenKind::Channel,789};
			if (bad == 3) changed.inventory.accountUserId = 124;
			CHECK(!MapDesktopSyncChatInventoryReply(Origin,1,chat,changed).success);
		}
		for (auto mode = 0; mode < 10; ++mode) {
			auto probe = ReaderProbe(route);
			auto response = Response(route,{message});
			if (mode == 0) { auto v = NativeMessagesSlice(); v.flags = MTP_flags(MTPDmessages_messagesSlice::Flag::f_inexact); v.messages = MTP_vector<MTPMessage>({message}); response = v.Build(); }
			if (mode == 1) { auto v = NativeMessagesSlice(); v.flags = MTP_flags(MTPDmessages_messagesSlice::Flag::f_search_flood); v.search_flood = MTP_searchPostsFlood(MTP_flags(MTPDsearchPostsFlood::Flags()),MTP_int(1),MTP_int(2),MTP_int(3),MTP_long(4)); v.messages = MTP_vector<MTPMessage>({message}); response = v.Build(); }
			if (mode == 2) response = Response(route,{});
			if (mode == 3) response = Response(route,{DocumentMessage(Route({PeerTokenKind::Channel,789}))});
			if (mode == 4) response = Response(route,{DocumentMessage(route,11)});
			if (mode == 5) response = MTP_messages_messagesNotModified(MTP_int(0));
			if (mode == 6) probe.current = false;
			if (mode == 7) response = Response(route,{message,message});
			if (mode == 9) { auto v = NativeMessagesSlice(); v.count = MTP_int(0); v.messages = MTP_vector<MTPMessage>({message}); response = v.Build(); }
			if (mode == 8) probe._owned = [](const MTPMessage &) { return SyncChatMessageOwnership::Unknown; };
			probe.Reply(response);
			CHECK(!probe.downloading && probe.record && probe.record->status != SyncCandidateStatus::Valid);
			CHECK(AggregateSyncCandidateRead({*probe.record}) != SyncCandidateReadStatus::Complete);
		}
		for (auto mode = 0; mode < 5; ++mode) {
			auto probe = ReaderProbe(route); probe.Reply(Response(route,{message}));
			probe._loader = std::make_unique<DownloadProbe>(); probe._loader->data = RecordBytes();
			if (mode == 0) probe._loader->data.chop(1);
			if (mode == 1) probe._loader->data += 'x';
			if (mode == 2) probe._downloadFailed = true;
			if (mode == 3) probe._loader->stopped = true;
			if (mode == 4) probe.current = false;
			probe.OnDownload(); CHECK(probe.record && probe.record->status != SyncCandidateStatus::Valid);
		}
		for (auto mode = 0; mode < 5; ++mode) {
			auto probe = ScanProbe(route);
			if (mode == 0) { probe.current = false; probe.Reply(Response(route,{})); }
			if (mode == 1) { auto v=NativeMessagesSlice(); v.count=MTP_int(2); v.flags=MTP_flags(MTPDmessages_messagesSlice::Flag::f_inexact); probe.Reply(v.Build()); }
			if (mode == 2) { auto v=NativeChannelMessages(); v.count=MTP_int(2); v.flags=MTP_flags(MTPDmessages_channelMessages::Flag::f_inexact); probe.Reply(v.Build()); }
			if (mode == 3) { auto v=NativeMessagesSlice(); v.count=MTP_int(2); probe.Reply(v.Build()); }
			if (mode == 4) probe.Reply(Response(route,{DocumentMessage(Route({PeerTokenKind::Channel,789}))}));
			CHECK(probe.status != SyncHistoryScanStatus::Complete);
		}
		{
			auto capped = ScanProbe(route);
			for (auto page = 0; page < 20; ++page) {
				if (chat.kind == PeerTokenKind::Channel) {
					auto response = NativeChannelMessages(); response.count = MTP_int(21);
					response.messages = MTP_vector<MTPMessage>({ DocumentMessage(route,100-page) }); capped.Reply(response.Build());
				} else {
					auto response = NativeMessagesSlice(); response.count = MTP_int(21);
					response.messages = MTP_vector<MTPMessage>({ DocumentMessage(route,100-page) }); capped.Reply(response.Build());
				}
			}
			CHECK(capped.status == SyncHistoryScanStatus::InvalidResponse && capped.next == 19);
			const auto bad = FinishSyncAccountInventory(123,{capped.status,{},capped._pages.count()},std::nullopt);
			CHECK(!MapDesktopSyncChatInventoryReply(Origin,1,chat,{route,bad}).success);
		}
		for (const auto &type : {u"FLOOD_WAIT_5"_q,u"SYNTHETIC_NETWORK_FAILURE"_q}) {
			const auto error = MTP::Error(MTP_rpc_error(MTP_int(type.startsWith(u"FLOOD"_q) ? 420 : 500),MTP_string(type)));
			CHECK(error.type() == type);
			auto probe = ScanProbe(route); probe.Fail(error);
			CHECK(probe.status == SyncHistoryScanStatus::RequestFailed);
			auto reader = ReaderProbe(route); reader.Fail(error);
			CHECK(reader.record && reader.record->status == SyncCandidateStatus::RequestFailed);
		}
		auto empty = ScanProbe(route); empty.Reply(Response(route,{}));
		CHECK(empty.status == SyncHistoryScanStatus::Complete);
		const auto absent = FinishSyncAccountInventory(123,{empty.status,{},0},SyncCandidateReadResult{SyncCandidateReadStatus::Complete,{}});
		CHECK(MapDesktopSyncChatInventoryReply(Origin,1,chat,{route,absent}).records == SyncChatRecordPresence::Empty);
	}
}

void CandidateSeal() {
	for (const auto &route : {Route({}),Route({PeerTokenKind::User,456}),Route({PeerTokenKind::Chat,456}),Route({PeerTokenKind::Channel,456})}) {
		for (auto mode = 0; mode < 9; ++mode) {
			auto probe = ReaderProbe(route); const auto message = DocumentMessage(route);
			probe.Reply(Response(route,{message}));
			probe._loader = std::make_unique<DownloadProbe>(); probe._loader->data = RecordBytes(); probe.OnDownload();
			CHECK(probe.rereading && !probe.record);
			if (mode == 1) probe._documentId = 100;
			if (mode == 2) probe._editDate = 1;
			if (mode == 3) ++probe._expectedBytes;
			if (mode == 4) probe.current = false;
			if (mode == 5) probe._owned = [](const MTPMessage &){return SyncChatMessageOwnership::Unknown;};
			if (mode == 6) probe.Fail(MTP::Error(MTP_rpc_error(MTP_int(420),MTP_string("FLOOD_WAIT_5"))));
			else if (mode == 7) probe.Reply(Response(route,{DocumentMessage(Route({PeerTokenKind::Channel,789}))}));
			else if (mode == 8) {auto value=NativeMessagesSlice();value.flags=MTP_flags(MTPDmessages_messagesSlice::Flag::f_inexact);value.count=MTP_int(1);value.messages=MTP_vector<MTPMessage>({message});probe.Reply(value.Build());}
			else probe.Reply(Response(route,{message}));
			CHECK(probe.record && (probe.record->status == SyncCandidateStatus::Valid) == (mode == 0));
		}
	}
}

void LedgerScope() {
	auto state = SyncLocalState(); state.version = 1;
	CHECK(DesktopSyncChatLedgerMatches(state,{}));
	CHECK(!DesktopSyncChatLedgerMatches(state,{PeerTokenKind::Channel,456}));
	state.version = 2; state.chat = {PeerTokenKind::Channel,456};
	CHECK(!DesktopSyncChatLedgerMatches(state,{}));
	CHECK(!DesktopSyncChatLedgerMatches(state,{PeerTokenKind::Chat,456}));
	CHECK(!DesktopSyncChatLedgerMatches(state,{PeerTokenKind::Channel,457}));
	CHECK(DesktopSyncChatLedgerMatches(state,{PeerTokenKind::Channel,456}));
	state.version = 3; CHECK(!DesktopSyncChatLedgerMatches(state,state.chat));
	for (const auto chat : {PeerToken{}, PeerToken{PeerTokenKind::User,456},
		PeerToken{PeerTokenKind::Chat,456}, PeerToken{PeerTokenKind::Channel,456}}) {
		auto directory = QTemporaryDir(); directory.setAutoRemove(false); CHECK(directory.isValid());
		auto initial = SyncLocalState(); initial.install = u"in-"_q + QString(26,u'a');
		initial.space = u"sp-"_q + QString(26,u'a'); initial.createdDevice = u"Synthetic"_q;
		initial.bindingToken = QString(32,u'a'); initial.version = chat == PeerToken{} ? 1 : 2;
		initial.chat = chat;
		auto store = SyncLocalStore(directory.path());
		CHECK(store.Open(true).status == SyncStoreStatus::Uninitialized);
		CHECK(store.Initialize(initial));
		const auto bytes = RecordBytes();
		auto next = initial.configData;
		next.pending = InspectConfigPayload(ParseSyncEnvelope(bytes)).version.key;
		CHECK(store.StageConfig(bytes,next,initial.bindingToken.toLatin1()));
		auto before = QByteArray();
		CHECK(ReadSyncPrivateFile(directory.path()+u"/state.json"_q,1024*1024,before) == SyncStoreStatus::Ready);
		const auto wrong = chat == PeerToken{} ? PeerToken{PeerTokenKind::Channel,456} : PeerToken{};
		CHECK(store.ConfirmConfigReadBack(bytes,initial.createdDevice,10,wrong).status == SyncStoreStatus::InvalidTransition);
		auto after = QByteArray();
		CHECK(ReadSyncPrivateFile(directory.path()+u"/state.json"_q,1024*1024,after) == SyncStoreStatus::Ready && before == after);
		if (chat != PeerToken{}) CHECK(store.ConfirmConfigReadBack(bytes,initial.createdDevice,10).status == SyncStoreStatus::InvalidTransition);
		CHECK(store.ConfirmConfigReadBack(bytes,initial.createdDevice,10,chat));
		CHECK(store.state()->chat == chat && store.state()->ownMessages.size() == 1 && store.state()->ownMessages[0].messageId == 10);
		CHECK(store.RemoveAbsentConfigMessage(10,bytes,initial.bindingToken.toLatin1(),wrong).status == SyncStoreStatus::InvalidTransition);
		CHECK(store.state()->ownMessages.size() == 1);
		CHECK(store.RemoveAbsentConfigMessage(10,bytes,initial.bindingToken.toLatin1(),chat).status == SyncStoreStatus::InvalidTransition);
		const auto successor = RecordBytes(2);
		next = store.state()->configData;
		next.pending = InspectConfigPayload(ParseSyncEnvelope(successor)).version.key;
		CHECK(store.StageConfig(successor,next,initial.bindingToken.toLatin1()));
		CHECK(store.ConfirmConfigReadBack(successor,initial.createdDevice,11,chat));
		CHECK(store.RemoveAbsentConfigMessage(10,bytes,initial.bindingToken.toLatin1(),wrong).status == SyncStoreStatus::InvalidTransition);
		CHECK(store.RemoveAbsentConfigMessage(10,bytes,initial.bindingToken.toLatin1(),chat));
		CHECK(store.state()->ownMessages.size() == 1 && store.state()->ownMessages[0].messageId == 11);
	}

}

}

#ifndef PURPLE_SYNC_CHAT_IO_EMBEDDED
int main() {
	AuthorMain();
	RoutesAndWire(); FullReadPath(); ExactReadback(); CandidateSeal(); LedgerScope();
	std::printf("TYPED IO %d checks, %d failures\n",Checks,Failures);
	return Failures ? 1 : 0;
}

#endif
