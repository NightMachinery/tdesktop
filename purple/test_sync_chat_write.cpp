#define PURPLE_SYNC_CHAT_IO_EMBEDDED
#include "test_sync_chat_io.cpp"
#include "purple/purple_sync_chat_write.h"
#include "purple/purple_sync_chat_service.h"
#include "purple/purple_sync_config_post.h"
#include "base/random.h"
#include <QtCore/QCryptographicHash>
#include <deque>
#include <limits>

namespace base {
void RandomFill(bytes::span data) {
	static auto value = uint64_t(700);
	++value;
	for (auto i = size_t(0); i < data.size(); ++i) {
		data[i] = std::byte((value >> ((i % sizeof(value)) * 8)) & 255);
	}
}
}

namespace {

constexpr auto kUploadPartBytes = 32 * 1024;

struct PendingApi {
	mtpBuffer wire;
	Fn<void(const MTPBool &)> boolean;
	Fn<void(const MTPUpdates &)> updates;
	Fn<void(const MTP::Error &)> failure;
	bool handled = false;
	bool uploadDc = false;
};

struct ControlledApi {
	std::deque<PendingApi> pending;
	std::vector<mtpBuffer> sent;
	int applied = 0;
	uint64_t appliedRandom = 0;
	Fn<void()> onApply;
	template <typename Request>
	struct Builder {
		ControlledApi *api;
		PendingApi call;
		template <typename Callback> Builder &done(Callback callback) {
			if constexpr (std::is_same_v<typename Request::ResponseType, MTPBool>) {
				call.boolean = std::move(callback);
			} else {
				call.updates = std::move(callback);
			}
			return *this;
		}
		template <typename Callback> Builder &fail(Callback callback) { call.failure = std::move(callback); return *this; }
		Builder &handleAllErrors() { call.handled = true; return *this; }
		Builder &toDC(int) { call.uploadDc = true; return *this; }
		void send() { api->sent.push_back(call.wire); api->pending.push_back(std::move(call)); }
	};
	template <typename Request> Builder<Request> request(Request value) {
		auto result = Builder<Request>{this}; value.write(result.call.wire); return result;
	}
	PendingApi Take() { CHECK(!pending.empty()); auto result = std::move(pending.front()); pending.pop_front(); return result; }
	void applyUpdates(const MTPUpdates &, uint64_t random) { ++applied; appliedRandom = random; if (onApply) onApply(); }
};

struct UploadSession;
struct UploadAccount {
	UploadSession *session = nullptr;
	bool leaving = false;
	bool loggingOut() const { return leaving; }
	UploadSession *maybeSession() const { return session; }
};
struct UploadSession : base::has_weak_ptr {
	UploadAccount owner{this};
	AccountKey key = Origin.account;
	UploadAccount &account() { return owner; }
	ControlledApi endpoint;
	ControlledApi &api() { return endpoint; }
	UserId userId() const { return UserId(key.userId); }
};

AccountKey SessionAccountKey(UploadSession *session) { return session->key; }

struct PostCounters {
	base::weak_ptr<UploadSession> session;
	uint64_t started=0;
	int inFlight=0;
};
std::map<UploadSession*,std::map<QString,std::shared_ptr<PostCounters>>> Posts;
std::shared_ptr<PostCounters> PostsFor(not_null<UploadSession*> session,PeerToken chat={})
#include "post-counters.inc"
std::function<void()> BeginNetwork(not_null<UploadSession*> session,PeerToken chat)
#include "post-network.inc"

void CounterScopes() {
	auto first=std::optional<UploadSession>(); first.emplace();
	auto second=UploadSession();
	auto completions=std::vector<std::function<void()>>();
	for (const auto chat:{PeerToken{},PeerToken{PeerTokenKind::User,456},PeerToken{PeerTokenKind::Chat,456},PeerToken{PeerTokenKind::Channel,456},PeerToken{PeerTokenKind::Channel,789}}) {
		CHECK(PostsFor(&*first,chat)->inFlight==0);
		completions.push_back(BeginNetwork(&*first,chat));
		CHECK(PostsFor(&*first,chat)->inFlight==1 && PostsFor(&*first,chat)->started==1);
		CHECK(PostsFor(&second,chat)->inFlight==0);
	}
	completions.front()();completions.front()();
	CHECK(PostsFor(&*first,{})->inFlight==0 && PostsFor(&*first,{PeerTokenKind::Channel,456})->inFlight==1);
	first.reset();first.emplace();
	CHECK(PostsFor(&*first,{})->started==0 && PostsFor(&*first,{PeerTokenKind::Channel,456})->inFlight==0);
	auto newer=BeginNetwork(&*first,{PeerTokenKind::Channel,456});
	for(auto &done:completions) done();
	CHECK(PostsFor(&*first,{PeerTokenKind::Channel,456})->inFlight==1);
	newer();newer();CHECK(PostsFor(&*first,{PeerTokenKind::Channel,456})->inFlight==0);
}

Fn<void(DesktopSyncChatStatus)> BeforePost;
std::optional<DesktopSyncChatLease> NextLease;
int GateCalls = 0;
bool BorrowedProof = false;

void CheckSyncChatBeforePost(UploadSession *, Fn<void(DesktopSyncChatStatus)> done,
		DesktopSyncChatOperation operation) {
	++GateCalls;
	BorrowedProof = operation.current && operation.current();
	BeforePost = std::move(done);
}
std::optional<DesktopSyncChatLease> SyncChatIoForPost(UploadSession *, PeerToken) { return NextLease; }

struct UploadProbe : std::enable_shared_from_this<UploadProbe> {
	base::weak_ptr<UploadSession> _session;
	AccountKey _account = Origin.account;
	std::optional<DesktopSyncChatIo> _route;
	std::optional<DesktopSyncChatLease> _lease;
	DesktopSyncChatOperation _operation;
	QByteArray _content;
	Fn<void(std::optional<MsgId>)> _finished;
	Fn<void()> _editRefused;
	Fn<bool()> _stillCurrent;
	MsgId _editId;
	uint64_t _fileId = 900;
	uint64_t _randomId = 0;
	qsizetype _offset = 0;
	int _part = 0;
	int _parts = 0;
	bool _done = false;
	bool _checking = false;
	bool current = true;
	rpl::lifetime _lifetime;
	int completions = 0;
	int refusals = 0;
	std::optional<MsgId> receipt;
	explicit UploadProbe(UploadSession &session, DesktopSyncChatIo route,
		QByteArray content = RecordBytes(), int edit = 0)
	: _session(base::make_weak(&session)), _route(std::move(route)), _content(std::move(content)), _editId(edit) {
		_stillCurrent = [this]{ return current; };
		_parts = int((_content.size() + kUploadPartBytes - 1) / kUploadPartBytes);
		_operation = { u"synthetic"_q, Origin.account, [this] { return current && !_done; } };
		_finished = [this](std::optional<MsgId> id) { ++completions; receipt = id; };
		_editRefused = [this] { ++refusals; };
	}
	bool Current() const
#include "upload-Current.inc"
	std::optional<DesktopSyncChatIo> route() const { return _lease ? std::optional(_lease->route) : std::nullopt; }
	Fn<bool()> readbackCurrent() const { return _lease ? _lease->current : Fn<bool()>(); }
	void SendNextPart()
#include "upload-SendNextPart.inc"
	void SendMedia()
#include "upload-SendMedia.inc"
	void DispatchMedia()
#include "upload-DispatchMedia.inc"
	void OnMedia(const MTPUpdates &result)
#include "upload-OnMedia.inc"
	void Finish(std::optional<MsgId> messageId)
#include "upload-Finish.inc"
	void Cancel()
#include "upload-Cancel.inc"
};

DesktopSyncChatStatus Usable(PeerToken chat) {
	auto status = DesktopSyncChatStatus(); status.complete = true; status.postingHeld = false;
	status.phase = DesktopSyncChatPhase::Use; status.resolution.readChat = chat; return status;
}

void Gate(const DesktopSyncChatIo &route, Fn<bool()> current = [] { return true; }) {
	CHECK(bool(BeforePost));
	NextLease = DesktopSyncChatLease{route, std::move(current)};
	auto done = std::move(BeforePost); done(Usable(route.peer.chat));
}

MTPUpdates Receipt(const DesktopSyncChatIo &route, uint64_t random, int id = 10,
		bool edit = false, bool combined = false, bool rawBroadcast = false) {
	auto message = DocumentMessage(route,id);
	if (rawBroadcast) {
		auto data = NativeMessage();
		data.flags = MTP_flags(MTPDmessage::Flag::f_from_id | MTPDmessage::Flag::f_out);
		data.peer_id = Target(route.peer.chat); data.from_id = Target(route.peer.chat);
		data.id = MTP_int(id); data.date = MTP_int(100); message = data.Build();
	}
	auto values = QVector<MTPUpdate>();
	if (!edit) values.push_back(MTP_updateMessageID(MTP_int(id),MTP_long(random)));
	if (route.peer.chat.kind == PeerTokenKind::Channel) {
		values.push_back(edit ? MTPUpdate(MTP_updateEditChannelMessage(message,MTP_int(1),MTP_int(1)))
			: MTPUpdate(MTP_updateNewChannelMessage(message,MTP_int(1),MTP_int(1))));
	} else {
		values.push_back(edit ? MTPUpdate(MTP_updateEditMessage(message,MTP_int(1),MTP_int(1)))
			: MTPUpdate(MTP_updateNewMessage(message,MTP_int(1),MTP_int(1))));
	}
	if (combined) { auto result = NativeUpdatesCombined(); result.updates = MTP_vector<MTPUpdate>(values); return result.Build(); }
	auto result = NativeUpdates(); result.updates = MTP_vector<MTPUpdate>(values); return result.Build();
}

void ReceiptMatrix() {
	for (const auto &route : {Route({}), Route({PeerTokenKind::User,456}), Route({PeerTokenKind::Chat,456}),
		Route({PeerTokenKind::Channel,456}), Route({PeerTokenKind::Channel,456},false), Route({PeerTokenKind::User,456},true,true)}) {
		for (const auto combined : {false,true}) {
			CHECK(DesktopSyncChatUploadReceipt(route,Receipt(route,5,10,false,combined),5) == 10);
			CHECK(DesktopSyncChatUploadReceipt(route,Receipt(route,5,10,true,combined),0,10) == 10);
			CHECK(!DesktopSyncChatUploadReceipt(route,Receipt(route,5,10,false,combined),6));
			CHECK(!DesktopSyncChatUploadReceipt(route,Receipt(route,5,10,true,combined),0,11));
			CHECK(!DesktopSyncChatUploadReceipt(route,Receipt(route,5,10,true,combined),5));
			CHECK(!DesktopSyncChatUploadReceipt(route,Receipt(route,5,10,false,combined),0,10));
		}
		auto shortSent = NativeShortSent(); shortSent.flags = MTP_flags(MTPDupdateShortSentMessage::Flag::f_out);
		shortSent.id = MTP_int(10); shortSent.date = MTP_int(100);
		CHECK(bool(DesktopSyncChatUploadReceipt(route,shortSent.Build(),5)) == (route.peer.chat.kind != PeerTokenKind::Channel));
		CHECK(!DesktopSyncChatUploadReceipt(route,shortSent.Build(),5,10));
		shortSent.flags = MTP_flags(MTPDupdateShortSentMessage::Flag::f_ttl_period);
		shortSent.ttl_period = MTP_int(1); CHECK(!DesktopSyncChatUploadReceipt(route,shortSent.Build(),5));
		CHECK(!DesktopSyncChatUploadReceipt(route,MTP_updatesTooLong(),5));
		CHECK(!DesktopSyncChatUploadReceipt(route,Receipt(route,5),0));
		CHECK(!DesktopSyncChatUploadReceipt(route,Receipt(route,5),5,-1));
		const auto foreign = Route({PeerTokenKind::Channel,789});
		CHECK(!DesktopSyncChatUploadReceipt(route,Receipt(foreign,5),5));
		auto updates = NativeUpdates(); updates.updates = MTP_vector<MTPUpdate>({MTP_updateMessageID(MTP_int(10),MTP_long(5))});
		CHECK(!DesktopSyncChatUploadReceipt(route,updates.Build(),5));
		auto base = Receipt(route,5).c_updates().vupdates().v;
		base.push_back(MTP_updateMessageID(MTP_int(11),MTP_long(5))); updates.updates = MTP_vector<MTPUpdate>(base);
		CHECK(!DesktopSyncChatUploadReceipt(route,updates.Build(),5));
		CHECK(CompatibleDesktopSyncChatIo(route,route));
		for (auto mode = 0; mode < 5; ++mode) {
			auto changed = route;
			if (mode == 0) changed.peer.origin.login = u"other"_q;
			if (mode == 1) changed.peer.origin.account.environment = 1;
			if (mode == 2) changed.peer.origin.account.userId = 124;
			if (mode == 3) changed.peer.facts.reset();
			if (mode == 4) changed.peer.kind = SyncChatKind::Unknown;
			CHECK(!CompatibleDesktopSyncChatIo(route,changed));
		}
		if (route.peer.chat.kind == PeerTokenKind::Chat || route.peer.chat.kind == PeerTokenKind::Channel) {
			auto readers=route; readers.peer.facts->otherMembers=5;
			CHECK(!CompatibleDesktopSyncChatIo(route,readers));
			auto unknown=route; unknown.peer.facts->otherMembers.reset();
			CHECK(!CompatibleDesktopSyncChatIo(route,unknown));
		}
		for (auto mode=0;mode<3;++mode) {
			auto changed=route;
			if (mode==0) changed.peer.facts->canSendFiles=false;
			if (mode==1) changed.peer.facts->postsAsOther=true;
			if (mode==2) changed.peer.facts->protectedContent=true;
			CHECK(!DesktopSyncChatSendMediaRequest(changed,MTP_inputMediaEmpty(),5));
			CHECK(!DesktopSyncChatEditRequest(changed,MTP_inputMediaEmpty(),10));
		}
		const auto send = DesktopSyncChatSendMediaRequest(route,MTP_inputMediaEmpty(),5);
		CHECK(send); auto wire = mtpBuffer(); send->write(wire);
		auto from = wire.constData(); const auto end = from + wire.size();
		CHECK(Read<MTPint>(from,end).v == int(mtpc_messages_sendMedia));
		CHECK(Read<MTPint>(from,end).v == int(MTPmessages_SendMedia::Flag::f_silent));
		CHECK(Read<MTPInputPeer>(from,end).type() == route.input.type());
		CHECK(Read<MTPInputMedia>(from,end).type() == mtpc_inputMediaEmpty);
		CHECK(qs(Read<MTPstring>(from,end)) == u"#purplesync"_q);
		CHECK(Read<MTPlong>(from,end).v == 5 && from == end);
		const auto edit = DesktopSyncChatEditRequest(route,MTP_inputMediaEmpty(),10); CHECK(edit);
		wire.clear(); edit->write(wire); from = wire.constData(); const auto limit = from + wire.size();
		CHECK(Read<MTPint>(from,limit).v == int(mtpc_messages_editMessage));
		CHECK(Read<MTPint>(from,limit).v == int(MTPmessages_EditMessage::Flag::f_media | MTPmessages_EditMessage::Flag::f_message));
		CHECK(Read<MTPInputPeer>(from,limit).type() == route.input.type());
		CHECK(Read<MTPint>(from,limit).v == 10);
		CHECK(qs(Read<MTPstring>(from,limit)) == u"#purplesync"_q);
		CHECK(Read<MTPInputMedia>(from,limit).type() == mtpc_inputMediaEmpty && from == limit);
		CHECK(!DesktopSyncChatSendMediaRequest(route,MTP_inputMediaEmpty(),0));
		CHECK(!DesktopSyncChatEditRequest(route,MTP_inputMediaEmpty(),0));
	}
	const auto channel = Route({PeerTokenKind::Channel,456});
	CHECK(DesktopSyncChatUploadReceipt(channel,Receipt(channel,5,10,false,false,true),5) == 10);
}

void UploadSchedules() {
	for (const auto &route : {Route({}), Route({PeerTokenKind::User,456}), Route({PeerTokenKind::Chat,456}),
		Route({PeerTokenKind::Channel,456}), Route({PeerTokenKind::Channel,456},false)}) {
		for (const auto edit : {false,true}) {
			for (auto mode = 0; mode < 16; ++mode) {
				auto session = UploadSession(); const auto calls = GateCalls;
				auto upload = std::make_shared<UploadProbe>(session,route,QByteArray(2*kUploadPartBytes+1,'x'),edit?10:0);
				upload->SendNextPart();
				auto collected = QByteArray();
				for (auto part = 0; part < 3; ++part) {
					CHECK(session.endpoint.pending.size() == 1 && GateCalls == calls);
					auto request = session.endpoint.Take(); CHECK(request.handled && request.uploadDc);
					auto from = request.wire.constData(); const auto end = from + request.wire.size();
					CHECK(Read<MTPint>(from,end).v == int(mtpc_upload_saveFilePart));
					CHECK(Read<MTPlong>(from,end).v == 900); CHECK(Read<MTPint>(from,end).v == part);
					collected += Read<MTPbytes>(from,end).v; CHECK(from == end);
					request.boolean(MTP_boolTrue());
				}
				CHECK(collected == upload->_content && GateCalls == calls+1 && BorrowedProof);
				CHECK(session.endpoint.pending.empty());
				if (mode == 1) upload->current = false;
				if (mode == 2) upload->Cancel();
				if (mode == 11) session.owner.leaving = true;
				if (mode == 12) session.owner.session = nullptr;
				if (mode == 13) session.key.environment = 1;
				if (mode == 14) upload->_route->peer.origin.account.userId = 124;
				if (mode == 3) { auto status=Usable(route.peer.chat); status.postingHeld=true; auto done=std::move(BeforePost); done(status); }
				else if (mode == 4) { auto bad=route; bad.peer.origin.login=u"other"_q; Gate(bad); }
				else if (mode == 5) Gate(route,[]{return false;});
				else Gate(route);
				if ((mode >= 1 && mode <= 5) || (mode >= 11 && mode <= 14)) {
					CHECK(upload->_done && !upload->receipt && session.endpoint.pending.empty()); continue;
				}
				CHECK(session.endpoint.pending.size()==1 && !upload->_done);
				auto request=session.endpoint.Take(); CHECK(request.handled && !request.uploadDc);
				auto from=request.wire.constData(); const auto end=from+request.wire.size();
				CHECK(Read<MTPint>(from,end).v==int(edit?mtpc_messages_editMessage:mtpc_messages_sendMedia));
				const auto flags=Read<MTPint>(from,end).v;
				CHECK(flags==int(edit?(MTPmessages_EditMessage::Flag::f_media|MTPmessages_EditMessage::Flag::f_message).value():int(MTPmessages_SendMedia::Flag::f_silent)));
				CHECK(Read<MTPInputPeer>(from,end).type()==route.input.type());
				if(edit) { CHECK(Read<MTPint>(from,end).v==10); CHECK(qs(Read<MTPstring>(from,end))==u"#purplesync"_q); }
				const auto mediaValue=Read<MTPInputMedia>(from,end);
				const auto &media=mediaValue.c_inputMediaUploadedDocument();
				CHECK(media.is_force_file() && qs(media.vmime_type())==u"application/json"_q);
				const auto &file=media.vfile().c_inputFile();
				CHECK(file.vid().v==900 && file.vparts().v==3 && file.vmd5_checksum().v==QCryptographicHash::hash(collected,QCryptographicHash::Md5).toHex());
				if(!edit) { CHECK(qs(Read<MTPstring>(from,end))==u"#purplesync"_q); CHECK(uint64_t(Read<MTPlong>(from,end).v)==upload->_randomId); }
				CHECK(from==end);
				if(mode==6) upload->current=false;
				if(mode==7) session.endpoint.onApply=[&]{upload->current=false;};
				if(mode==8) upload->Cancel();
				if(mode==15) {
					upload->Cancel();
					request.failure(MTP::Error(MTP_rpc_error(MTP_int(400),MTP_string("MESSAGE_AUTHOR_REQUIRED"))));
					CHECK(upload->completions==1 && upload->refusals==0); continue;
				}
				if(mode==9 || mode==10) {
					const auto type=mode==9?u"MESSAGE_NOT_MODIFIED"_q:u"MESSAGE_AUTHOR_REQUIRED"_q;
					request.failure(MTP::Error(MTP_rpc_error(MTP_int(400),MTP_string(type))));
					if(edit) CHECK(mode==9?bool(upload->receipt):upload->refusals==1);
					else CHECK(upload->_done && !upload->receipt);
					continue;
				}
				request.updates(Receipt(route,upload->_randomId,10,edit));
				CHECK(session.endpoint.applied==1 && session.endpoint.appliedRandom==(edit?0:upload->_randomId));
				CHECK(upload->_done && upload->completions==1 && bool(upload->receipt)==(mode==0));
				upload->Cancel(); CHECK(upload->completions==1);
			}
		}
	}
}

struct PostExactEndpoint {
	DesktopSyncChatIo route;
	int id;
	Fn<bool()> current;
	Fn<void(SyncAutoServerRead)> done;
	PostExactEndpoint(UploadSession *, DesktopSyncChatIo value, int target,
		Fn<bool()> proof, Fn<void(SyncAutoServerRead)> callback)
	: route(std::move(value)), id(target), current(std::move(proof)), done(std::move(callback)) {}
	void Start() {}
};

struct PostProbe : base::has_weak_ptr {
	UploadSession *_session;
	std::optional<DesktopSyncChatIo> _route;
	Fn<bool()> _readbackCurrent;
	std::shared_ptr<UploadProbe> _upload;
	std::unique_ptr<PostExactEndpoint> _reader;
	MsgId _messageId;
	QByteArray _staged=RecordBytes();
	bool _done=false;
	bool _posted=true;
	bool current=true;
	Fn<void(SyncConfigPostResult)> _finished;
	std::optional<SyncConfigPostResult> result;
	explicit PostProbe(UploadSession &session,const DesktopSyncChatIo &route)
	: _session(&session),_route(route),_upload(std::make_shared<UploadProbe>(session,route)) {
		_upload->_lease=DesktopSyncChatLease{route,[this]{return current;}};
		_finished=[this](SyncConfigPostResult value){result=std::move(value);};
	}
	bool ContextCurrent() const {return current && (!_readbackCurrent || _readbackCurrent());}
	void OnReceipt(std::optional<MsgId> messageId)
#define DesktopSyncExactRead PostExactEndpoint
#include "post-OnReceipt.inc"
#undef DesktopSyncExactRead
	void OnReadback(SyncAutoServerRead result)
#include "post-OnReadback.inc"
	void Finish(SyncConfigPostResult result)
#include "post-Finish.inc"
	void Cancel()
#include "post-Cancel.inc"
};

struct ProofStore {
	SyncLocalState value;
	bool current = true;
	bool valid = true;
	ProofStore &local() {return *this;}
	const SyncLocalState *state() const {return valid?&value:nullptr;}
	QString directory() const {return u"synthetic"_q;}
	bool Current() const {return current;}
};
struct AutoProofProbe : base::has_weak_ptr {
	SyncAutoPlan _plan;
	QString _sessionId=u"owner-a"_q;
	AccountKey _account=Origin.account;
	std::unique_ptr<ProofStore> _store=std::make_unique<ProofStore>();
	bool current=true;
	bool Current() const {return current;}
	DesktopSyncChatOperation OperationFor(const SyncAutoPlan &plan)
#include "auto-operation.inc"
};
void BorrowedRunProof() {
	for(auto mode=0;mode<8;++mode) {
		auto owner=AutoProofProbe(); owner._plan.ticket=1; owner._store->value.version=1;
		auto proof=owner.OperationFor(owner._plan); CHECK(proof.current && proof.current());
		owner._plan.ticket=2; CHECK(proof.current());
		if(mode==1) owner._sessionId=u"owner-b"_q;
		if(mode==2) owner._store=std::make_unique<ProofStore>();
		if(mode==3) owner.current=false;
		if(mode==4) owner._store->current=false;
		if(mode==5) owner._store->valid=false;
		if(mode==6) owner._store->value.version=2;
		if(mode==7) owner._store->value.chat={PeerTokenKind::Channel,456};
		CHECK(proof.current()==(mode==0));
	}
}

void PostReadbackSchedules() {
	for(const auto &route:{Route({}),Route({PeerTokenKind::User,456}),Route({PeerTokenKind::Chat,456}),Route({PeerTokenKind::Channel,456})}) {
		for(auto mode=0;mode<12;++mode) {
			auto directory=QTemporaryDir(); directory.setAutoRemove(false); CHECK(directory.isValid());
			auto state=SyncLocalState(); state.install=u"in-"_q+QString(26,u'a'); state.space=u"sp-"_q+QString(26,u'a');
			state.createdDevice=u"Synthetic"_q; state.bindingToken=QString(32,u'a'); state.chat=route.peer.chat;
			state.version=state.chat==PeerToken{}?1:2;
			auto store=SyncLocalStore(directory.path()); CHECK(store.Open(true).status==SyncStoreStatus::Uninitialized); CHECK(store.Initialize(state));
			auto next=state.configData; next.pending=InspectConfigPayload(ParseSyncEnvelope(RecordBytes())).version.key;
			CHECK(store.StageConfig(RecordBytes(),next,state.bindingToken.toLatin1()));
			auto session=UploadSession(); auto post=PostProbe(session,route);
			if(mode==1) post.current=false;
			if(mode==2) post._upload->_lease.reset();
			if(mode==3) post._upload->_lease->route.peer.origin.login=u"other"_q;
			post.OnReceipt(mode==4?std::nullopt:std::optional(MsgId(10)));
			if(mode>=1 && mode<=4) {CHECK(post.result && post.result->status==SyncConfigPostStatus::OutcomeUnknown); continue;}
			CHECK(post._reader && post._reader->route.peer.origin==Origin && post._reader->route.peer.chat==route.peer.chat && post._reader->id==10);
			auto exact=ExactProbe(post._reader->route); exact.Reply(Response(route,{DocumentMessage(route)}),false);
			auto candidate=ClassifySyncCandidate(10,RecordBytes());candidate.documentId=99;
			exact._reader->finished({SyncCandidateReadStatus::Complete,{candidate}});
			exact.Reply(Response(route,{DocumentMessage(route)}),true);
			CHECK(exact.result && exact.result->fresh && exact.result->owned);
			auto read=*exact.result;
			if(mode==5) read.record=RecordBytes(2);
			if(mode==6) read.messageId=11;
			if(mode==7) read.fresh=false;
			if(mode==8) read.owned=false;
			if(mode==9) read.presence=SyncAutoMessagePresence::Absent;
			if(mode==10) post.current=false;
			if(mode==11) { post.Cancel(); CHECK(post.result && post.result->status==SyncConfigPostStatus::OutcomeUnknown); continue; }
			post._reader->done(std::move(read));
			CHECK(post.result && post.result->status==(mode==0?SyncConfigPostStatus::Confirmed:SyncConfigPostStatus::OutcomeUnknown));
			CHECK((post.result->readback==RecordBytes())==(mode==0));
			if (post.result->status==SyncConfigPostStatus::Confirmed) {
				CHECK(store.ConfirmConfigReadBack(post.result->readback,state.createdDevice,int32_t(post.result->messageId.bare),route.peer.chat));
				CHECK(store.state()->ownMessages.size()==1 && store.state()->ownMessages.front().messageId==10);
			} else {
				CHECK(store.state()->ownMessages.empty() && store.state()->config.pendingSeq!=0);
				CHECK(store.ReadPendingConfig().staged==RecordBytes());
			}
		}
	}
}

}

int main() {
	AuthorMain(); RoutesAndWire(); FullReadPath(); ExactReadback(); CandidateSeal(); LedgerScope();
	ReceiptMatrix(); UploadSchedules(); PostReadbackSchedules(); BorrowedRunProof(); CounterScopes();
	std::printf("TYPED WRITE %d checks, %d failures\n",Checks,Failures);
	return Failures?1:0;
}
