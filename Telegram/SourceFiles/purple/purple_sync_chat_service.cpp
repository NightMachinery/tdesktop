#include "purple/purple_sync_chat_service.h"

#include "apiwrap.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "crl/crl_async.h"
#include "crl/crl_on_main.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_chat_participant_status.h"
#include "data/data_folder.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/session/send_as_peers.h"
#include "mtproto/mtproto_auth_key.h"
#include "mtproto/sender.h"
#include "purple/purple_config.h"
#include "purple/purple_desktop_archive_store.h"
#include "purple/purple_sync_account_binding.h"
#include "purple/purple_sync_account_inventory.h"
#include "purple/purple_sync_chat_store.h"
#include "purple/purple_sync_chat_peer_request.h"
#include "storage/storage_account.h"
#include <QtCore/QCryptographicHash>

#include <QtCore/QDir>
#include <QtCore/QUuid>
#include <algorithm>
#include <map>

namespace Purple {
namespace {

rpl::event_stream<Main::Session*> Changes;

constexpr auto kLoginNoncePref = "purple.sync.chat.login_nonce.v1";
constexpr auto kLoginKeysPref = "purple.sync.chat.login_keys.v1";

QByteArray LoginKeys(Main::Session &session) {
	auto keys = std::set<QByteArray>();
	for (const auto &key : session.mtp().getKeysForWrite()) {
		if (key && key->type() != MTP::AuthKey::Type::Temporary
			&& key->type() != MTP::AuthKey::Type::Local) {
			keys.emplace(
				QByteArray::number(key->keyId(), 16).rightJustified(16, '0'));
		}
	}
	auto result = QByteArray();
	for (const auto &key : keys) {
		if (!result.isEmpty()) {
			result += ',';
		}
		result += key;
	}
	return result;
}

std::optional<DesktopArchiveLoginAuthority> LoginFor(Main::Session &session) {
	const auto nonce = session.local().readPref<QByteArray>(kLoginNoncePref);
	const auto keys = session.local().readPref<QByteArray>(kLoginKeysPref);
	const auto result =
		DesktopArchiveSelectLoginAuthority(nonce, keys, LoginKeys(session));
	if (result) {
		if (result->nonce != nonce) {
			session.local().writePref<QByteArray>(
				kLoginNoncePref, result->nonce);
		}
		if (result->authKeys != keys) {
			session.local().writePref<QByteArray>(
				kLoginKeysPref, result->authKeys);
		}
		if (session.local().readPref<QByteArray>(kLoginNoncePref)
				!= result->nonce
			|| session.local().readPref<QByteArray>(kLoginKeysPref)
				!= result->authKeys) {
			return {};
		}
	}
	return result;
}

std::optional<PeerToken> Token(PeerId id, uint64_t self) {
	return DesktopSyncChatToken(peerToMTP(id), self);
}

PeerData* Loaded(Main::Session &session, PeerToken chat) {
	if (chat == PeerToken{}) {
		return session.user();
	}
	const auto id = chat.kind == PeerTokenKind::User
		? peerFromUser(UserId(chat.id))
		: chat.kind == PeerTokenKind::Chat
		? peerFromChat(ChatId(chat.id))
		: peerFromChannel(ChannelId(chat.id));
	return session.data().peerLoaded(id);
}

std::vector<PeerToken> DialogsFor(Main::Session &session, uint64_t self) {
	struct Candidate {
		PeerToken token;
		int priority;
		TimeId date;
	};
	auto candidates = std::vector<Candidate>();
	for (const auto folder :
		 {static_cast<Data::Folder*>(nullptr),
		  session.data().folder(1).get()}) {
		for (const auto row : *session.data().chatsList(folder)->indexed()) {
			const auto history = row->key().history();
			if (!history
				|| (!history->peer->isChat() && !history->peer->isChannel())) {
				continue;
			}
			const auto token = Token(history->peer->id, self);
			if (!token) {
				continue;
			}
			const auto channel = history->peer->asChannel();
			const auto creator = channel && channel->amCreator();
			if (channel && !creator && !channel->isMegagroup()) {
				continue;
			}
			candidates.push_back(
				{*token,
				 creator && history->peer->name() == u"Purple sync"_q ? 0
					 : creator										  ? 1
																	  : 2,
				 history->chatListTimeId()});
		}
	}
	std::stable_sort(
		candidates.begin(),
		candidates.end(),
		[](const Candidate &a, const Candidate &b) {
			return a.priority != b.priority ? a.priority < b.priority
											: a.date > b.date;
		});
	auto result = std::vector<PeerToken>();
	for (const auto &candidate : candidates) {
		if (std::find(result.begin(), result.end(), candidate.token)
			== result.end()) {
			result.push_back(candidate.token);
		}
		if (result.size() == 50) {
			break;
		}
	}
	return result;
}

void RetireDisk(std::shared_ptr<DesktopSyncChatDisk> disk) {
	if (disk) {
		crl::async([disk = std::move(disk)] {
			disk->lock.reset();
		});
	}
}

class SyncChatSession final : public base::has_weak_ptr {
public:
	explicit SyncChatSession(not_null<Main::Session*> session);
	~SyncChatSession();
	void Ensure(
		bool force,
		Fn<void(DesktopSyncChatStatus)> done = {},
		DesktopSyncChatOperation operation = {},
		bool beforePost = false);
	void Cancel(QString reason);
	DesktopSyncChatStatus status() const;
	std::optional<DesktopSyncChatSetupSnapshot> Setup() const;
	bool Owns(Main::Session* session) const;
	std::optional<DesktopSyncChatLease> Lease(PeerToken chat);
	bool HasOrigin(const DesktopSyncChatIo &route) const;
	DesktopSyncChatDiscoveryScope ScopeFor(
		const DesktopSyncChatDisk &disk,
		const std::optional<DesktopSyncChatCacheRecord> &cache) const;

private:
	bool Current() const;
	bool Accept(const DesktopSyncChatCommand &command);
	void Begin(bool retry);
	void Run();
	void Complete(DesktopSyncChatReply reply);
	void Peer(
		const DesktopSyncChatCommand &command,
		Fn<void(std::optional<DesktopSyncChatPeer>)> done);
	void Exact(const DesktopSyncChatCommand &command);
	void Finish();
	void Publish();
	void Peers(const MTPmessages_Messages &response);
	std::vector<MTPMessage> Messages(
		const MTPmessages_Messages &response) const;

	base::weak_ptr<Main::Session> _session;
	DesktopSyncChatOrigin _origin;
	DesktopSyncChatStatus _status;
	DesktopSyncChatOperation _operation;
	std::unique_ptr<DesktopSyncChatDiscovery> _model;
	std::unique_ptr<MTP::Sender> _api;
	std::unique_ptr<DesktopSyncChatPeerRequest> _peerRequest;
	std::optional<DesktopSyncChatDiscoveryScope> _setupScope;
	std::unique_ptr<SyncAccountInventory> _inventory;
	std::shared_ptr<DesktopSyncChatDisk> _disk;
	std::shared_ptr<std::atomic<bool>> _current;
	std::vector<Fn<void(DesktopSyncChatStatus)>> _callbacks;
	uint64_t _run = 0;
	QByteArray _expectedCacheBytes;
	QByteArray _expectedStateIdentity;
	QByteArray _expectedStateBytes;
	QString _root;
	QByteArray _binding;
	QByteArray _loginNonce;
	QByteArray _loginKeys;
	QString _loginBinding;
	std::optional<AccountKey> _owner;
	AccountWorkMode _mode = AccountWorkMode::Own;
	int64_t _lastStarted = 0;
	std::optional<DesktopSyncChatDiscoveryReceipt> _recent;
	bool _beforePost = false;
	bool _directBeforePost = false;
	bool _busy = false;
	base::Timer _timer;
	base::Timer _requestTimeout;
	rpl::lifetime _lifetime;

};

SyncChatSession::SyncChatSession(not_null<Main::Session*> session)
: _session(session)
, _origin{SessionAccountKey(session), QUuid::createUuid().toString(QUuid::WithoutBraces)}
, _timer([=] {
	if (!_busy && _model && _status.phase == DesktopSyncChatPhase::Searching) {
		Begin(true);
	}
})
, _requestTimeout([=] {
	if (_busy) {
		Cancel(u"request-timeout"_q);
	}
}) {
	const auto changed = [=] {
		if (_current && !Current()) {
			Cancel(u"account-context-changed"_q);
		}
	};
	SettingsChanges() | rpl::on_next(changed, _lifetime);
	StateChanges() | rpl::on_next(changed, _lifetime);
	const auto weak = base::make_weak(this);
	session->lifetime().add([weak] {
		if (const auto self = weak.get()) {
			self->Cancel(u"session-ended"_q);
		}
	});
}

SyncChatSession::~SyncChatSession() {
	if (_current) {
		_current->store(false);
	}
	_timer.cancel();
	_requestTimeout.cancel();
	_peerRequest.reset();
	_api.reset();
	_inventory.reset();
	RetireDisk(std::move(_disk));
}

bool SyncChatSession::Owns(Main::Session* session) const {
	return _session.get() == session;
}

bool SyncChatSession::HasOrigin(const DesktopSyncChatIo &route) const {
	return _session && ValidDesktopSyncChatIo(route)
		&& route.peer.origin == _origin
		&& SessionAccountKey(_session.get()) == _origin.account;
}

std::optional<DesktopSyncChatLease> SyncChatSession::Lease(PeerToken chat) {
	if (_busy || !Current() || !_model || !_status.complete
		|| _status.phase != DesktopSyncChatPhase::Use || _status.postingHeld
		|| _status.resolution.readChat != chat) {
		return {};
	}
	const auto peer = _model->peer(chat);
	const auto native = Loaded(*_session.get(), chat);
	if (!peer || !native) {
		return {};
	}
	const auto route = DesktopSyncChatIo{ *peer, chat == PeerToken{}
		? MTPInputPeer(MTP_inputPeerSelf()) : native->input() };
	const auto eligible = DesktopSyncChatEligibility(*peer);
	if (!ValidDesktopSyncChatIo(route) || !eligible
		|| eligible->readers.populationUnknown
		|| eligible->readers != _status.resolution.readers) {
		return {};
	}
	const auto weak = base::make_weak(this);
	return DesktopSyncChatLease{ route,
		[weak, run = _run, origin = _origin, chat] {
			const auto self = weak.get();
			if (!self || self->_busy || self->_run != run
				|| self->_origin != origin || !self->Current()
				|| !self->_status.complete || self->_status.postingHeld
				|| self->_status.phase != DesktopSyncChatPhase::Use
				|| self->_status.resolution.readChat != chat) {
				return false;
			}
			const auto native = Loaded(*self->_session.get(), chat);
			return native && Data::CanSend(native, ChatRestriction::SendFiles)
				&& Token(self->_session->sendAsPeers().resolveChosen(native)->id,
					origin.account.userId) == PeerToken{};
		} };
}

DesktopSyncChatDiscoveryScope SyncChatSession::ScopeFor(
		const DesktopSyncChatDisk &disk,
		const std::optional<DesktopSyncChatCacheRecord> &cache) const {
	return { _origin, _root, _binding, _loginBinding, _owner, _mode,
		disk.settingsSpace, disk.settingsBound, disk.settingsChat,
		cache ? std::optional(cache->mark) : std::nullopt,
		cache && cache->carry.has_value() };
}

bool SyncChatSession::Current() const {
	const auto session = _session.get();
	return session && _current && _current->load()
		&& !session->account().loggingOut()
		&& session->account().maybeSession() == session
		&& SessionAccountKey(session) == _origin.account
		&& AccountSyncOperationsAllowed(session) && ConfigDirectory() == _root
		&& SettingsStartupOwner() == _owner
		&& AccountWorkModeForSession(session) == _mode
		&& AccountSyncBindingToken(session->account()) == _binding
		&& session->local().readPref<QByteArray>(kLoginNoncePref) == _loginNonce
		&& DesktopArchiveLoginKeysOverlap(_loginKeys, LoginKeys(*session))
		&& (!_operation.current || _operation.current());
}

void SyncChatSession::Ensure(
	bool force,
	Fn<void(DesktopSyncChatStatus)> done,
	DesktopSyncChatOperation operation,
	bool beforePost) {
	if (_busy) {
		if (operation.current || beforePost) {
			auto held = status();
			held.postingHeld = true;
			held.phase = DesktopSyncChatPhase::Paused;
			held.reason = u"resolution-busy"_q;
			if (done) {
				done(held);
			}
		} else if (done) {
			_callbacks.push_back(std::move(done));
		}
		return;
	}
	const auto now = base::unixtime::now();
	if (Current()
		&& ((_status.phase == DesktopSyncChatPhase::Searching
			 && now < _status.retryAt)
			|| (!force && _status.complete && now - _lastStarted < 900))) {
		if (done) {
			done(_status);
		}
		return;
	}
	_beforePost = beforePost;
	_operation = std::move(operation);
	if (done) {
		_callbacks.push_back(std::move(done));
	}
	Begin(false);
}

void SyncChatSession::Cancel(QString reason) {
	_recent.reset();
	if (_current) {
		_current->store(false);
	}
	++_run;
	_timer.cancel();
	_requestTimeout.cancel();
	_peerRequest.reset();
	_api.reset();
	_inventory.reset();
	if (_model) {
		_model->Cancel();
		_status.resolution = _model->status().resolution;
	}
	RetireDisk(std::move(_disk));
	_busy = false;
	_status.phase = DesktopSyncChatPhase::Paused;
	_status.postingHeld = true;
	_status.complete = false;
	_status.retryAt = 0;
	_status.reason = std::move(reason);
	Publish();
}

bool SyncChatSession::Accept(const DesktopSyncChatCommand &command) {
	if (!_busy || !_model || command.run != _run
		|| _model->command().ticket != command.ticket
		|| command.origin != _origin) {
		return false;
	}
	if (!Current()) {
		Cancel(u"origin-retired"_q);
		return false;
	}
	return true;
}

void SyncChatSession::Begin(bool retry) {
	const auto session = _session.get();
	if (!session || session->account().loggingOut()
		|| !AccountSyncOperationsAllowed(session)) {
		Cancel(u"account-not-available"_q);
		return;
	}
	if (_current) {
		_current->store(false);
	}
	_timer.cancel();
	_requestTimeout.cancel();
	_peerRequest.reset();
	_api.reset();
	_inventory.reset();
	RetireDisk(std::move(_disk));
	_current = std::make_shared<std::atomic<bool>>(true);
	_root = ConfigDirectory();
	_binding = AccountSyncBindingToken(session->account());
	const auto login = LoginFor(*session);
	if (!login) {
		Cancel(u"login-authority-held"_q);
		return;
	}
	_loginNonce = login->nonce;
	_loginKeys = login->authKeys;
	_loginBinding = QString::fromLatin1(
		QCryptographicHash::hash(
			"purple-sync-chat-v1:" + _loginNonce, QCryptographicHash::Sha256)
			.toHex());
	_owner = SettingsStartupOwner();
	_mode = AccountWorkModeForSession(session);
	const auto files = ResolveAccountFiles(
		_origin.account, _owner, AccountFileLayout::Desktop);
	if (!files
		|| (_operation.current
			&& (_operation.account != _origin.account
				|| _operation.directory
					!= QDir(_root).filePath(files->syncDirectory)
				|| !_operation.current()))) {
		Cancel(u"account-lock-scope-held"_q);
		return;
	}
	++_run;
	_busy = true;
	_lastStarted = base::unixtime::now();
	_status = {};
	_status.reason = u"resolving"_q;
	_requestTimeout.callOnce(30 * 1000);
	const auto weak = base::make_weak(this);
	const auto current = _current;
	const auto root = _root;
	const auto token = _binding;
	const auto account = _origin.account;
	const auto directory = QDir(root).filePath(files->syncDirectory);
	const auto state = QDir(root).filePath(files->syncStore);
	const auto borrowed = bool(_operation.current);
	const auto loginBinding = _loginBinding;
	crl::async([=] {
		const auto disk = OpenDesktopSyncChatDisk(
			root,
			directory,
			state,
			account,
			token,
			loginBinding,
			current,
			borrowed);
		crl::on_main([=] {
			const auto self = weak.get();
			if (!self || self->_current != current) {
				RetireDisk(disk);
				return;
			}
			if (!self->Current()) {
				RetireDisk(disk);
				self->Cancel(u"origin-retired"_q);
				return;
			}
			if (!disk->error.isEmpty()) {
				const auto error = disk->error;
				RetireDisk(disk);
				self->Cancel(error);
				return;
			}
			if (retry
				&& (disk->bytes != self->_expectedCacheBytes
					|| disk->stateIdentity != self->_expectedStateIdentity
					|| disk->stateBytes != self->_expectedStateBytes)) {
				RetireDisk(disk);
				self->Cancel(u"cache-changed-during-search"_q);
				return;
			}
			self->_disk = disk;
			self->_api = std::make_unique<MTP::Sender>(&self->_session->mtp());
			if (!retry || !self->_model) {
				self->_model = std::make_unique<DesktopSyncChatDiscovery>(
					self->_origin,
					disk->cache,
					disk->settingsBound,
					disk->settingsChat,
					DialogsFor(*self->_session, self->_origin.account.userId),
					disk->loginBinding);
				const auto now = int64_t(base::unixtime::now());
				const auto scope = self->ScopeFor(*disk, disk->cache);
				self->_directBeforePost = self->_beforePost && self->_recent
					&& ReuseDesktopSyncChatDiscovery(*self->_recent, scope, now);
				if (self->_directBeforePost) {
					self->_model->StartBeforePost(now);
				} else {
					self->_recent.reset();
					self->_model->Start(now);
				}
			} else {
				self->_directBeforePost = false;
				self->_recent.reset();
				self->_model->Tick(base::unixtime::now());
			}
			self->Run();
		});
	});
}

void SyncChatSession::Peers(const MTPmessages_Messages &response) {
	response.match(
		[](const MTPDmessages_messagesNotModified &) {
		},
		[&](const auto &data) {
			_session->data().processUsers(data.vusers());
			_session->data().processChats(data.vchats());
		});
}

std::vector<MTPMessage> SyncChatSession::Messages(
		const MTPmessages_Messages &response) const {
	auto result = std::vector<MTPMessage>();
	response.match(
		[](const MTPDmessages_messagesNotModified &) {
		},
		[&](const auto &data) {
			result.assign(data.vmessages().v.begin(), data.vmessages().v.end());
		});
	return result;
}

void SyncChatSession::Peer(
		const DesktopSyncChatCommand &command,
		Fn<void(std::optional<DesktopSyncChatPeer>)> done) {
	const auto weak = base::make_weak(this);
	_peerRequest = std::make_unique<DesktopSyncChatPeerRequest>(
		_session.get(), _origin, command.chat, [weak, command] {
			const auto self = weak.get();
			return self && self->Accept(command);
		}, std::move(done));
}

void SyncChatSession::Exact(const DesktopSyncChatCommand &command) {
	const auto weak = base::make_weak(this);
	const auto messages = std::make_shared<std::vector<MTPMessage>>();
	const auto exactKnown = std::make_shared<bool>(command.mark->markId == 0);
	const auto exactGone = std::make_shared<bool>(false);
	const auto evidence = [=] {
		const auto self = weak.get();
		if (!self || !self->Accept(command)) {
			return;
		}
		self->Peer(command, [=](std::optional<DesktopSyncChatPeer> facts) {
			const auto self = weak.get();
			if (!self || !self->Accept(command)) {
				return;
			}
			const auto peer = Loaded(*self->_session, command.chat);
			if (!peer) {
				self->Complete({command.origin, command.ticket});
				return;
			}
			self->_api
				->request(MTPmessages_GetHistory(
					peer->input(),
					MTP_int(0),
					MTP_int(0),
					MTP_int(0),
					MTP_int(1),
					MTP_int(0),
					MTP_int(0),
					MTP_long(0)))
				.done([=](const MTPmessages_Messages &response) {
					const auto self = weak.get();
					if (!self || !self->Accept(command)) {
						return;
					}
					auto reply = DesktopSyncChatReply{
						command.origin, command.ticket, *exactKnown};
					reply.peer = facts;
					reply.messages = *messages;
					reply.chatReadable =
						RecognizedDesktopSyncChatRead(response, command.chat);
					self->Complete(std::move(reply));
				})
				.fail([=](const MTP::Error &error) {
					const auto self = weak.get();
					if (!self || !self->Accept(command)) {
						return;
					}
					auto reply =
						DesktopSyncChatReply{command.origin, command.ticket};
					reply.confirmedChatGone = *exactGone
						&& command.chat.kind == PeerTokenKind::Channel
						&& error.type() == u"CHANNEL_PRIVATE"_q;
					if (reply.confirmedChatGone) {
						reply.chatReadable = false;
					}
					self->Complete(std::move(reply));
				})
				.handleAllErrors()
				.send();
		});
	};
	if (command.mark->markId == 0) {
		evidence();
		return;
	}
	const auto peer = Loaded(*_session, command.chat);
	if (!peer) {
		Complete({command.origin, command.ticket});
		return;
	}
	auto channel = std::optional<MTPInputChannel>();
	if (peer->input().type() == mtpc_inputPeerChannel) {
		const auto &input = peer->input().c_inputPeerChannel();
		channel = MTP_inputChannel(input.vchannel_id(), input.vaccess_hash());
	}
	const auto request = DesktopSyncChatExactReadRequest(
		command.chat, command.mark->markId, channel);
	if (!request) {
		Complete({command.origin, command.ticket});
		return;
	}
	std::visit(
		[&](const auto &value) {
			auto copy = value;
			_api->request(std::move(copy))
				.done([=](const MTPmessages_Messages &response) {
					const auto self = weak.get();
					if (!self || !self->Accept(command)) {
						return;
					}
					self->Peers(response);
					*exactKnown =
						RecognizedDesktopSyncChatRead(response, command.chat);
					*messages = self->Messages(response);
					evidence();
				})
				.fail([=](const MTP::Error &error) {
					const auto self = weak.get();
					if (!self || !self->Accept(command)) {
						return;
					}
					*exactGone = error.type() == u"CHANNEL_PRIVATE"_q;
					evidence();
				})
				.handleAllErrors()
				.send();
		},
		*request);
}

void SyncChatSession::Run() {
	if (!Current()) {
		Cancel(u"origin-retired"_q);
		return;
	}
	auto command = _model->command();
	command.run = _run;
	if (command.step == DesktopSyncChatStep::Idle) {
		Finish();
		return;
	}
	_requestTimeout.callOnce(30 * 1000);
	const auto weak = base::make_weak(this);
	if (command.step == DesktopSyncChatStep::PeerInfo) {
		Peer(command, [=](std::optional<DesktopSyncChatPeer> peer) {
			if (const auto self = weak.get(); self && self->Accept(command)) {
				auto reply = DesktopSyncChatReply{
					command.origin, command.ticket, bool(peer)};
				reply.peer = std::move(peer);
				self->Complete(std::move(reply));
			}
		});
	} else if (command.step == DesktopSyncChatStep::ExactMark) {
		Exact(command);
	} else if (command.step == DesktopSyncChatStep::Inventory) {
		Peer(command, [=](std::optional<DesktopSyncChatPeer> peer) {
			const auto live = weak.get();
			if (!live || !live->Accept(command)) {
				return;
			}
			if (!peer || !DesktopSyncChatEligibility(*peer)
				|| !bool(*DesktopSyncChatEligibility(*peer))) {
				live->Complete({command.origin, command.ticket});
				return;
			}
			const auto loaded = Loaded(*live->_session.get(), command.chat);
			if (!loaded) {
				live->Complete({command.origin, command.ticket});
				return;
			}
			const auto route = DesktopSyncChatIo{ *peer, command.chat == PeerToken{}
				? MTPInputPeer(MTP_inputPeerSelf()) : loaded->input() };
			if (!ValidDesktopSyncChatIo(route)) {
				live->Complete({command.origin, command.ticket});
				return;
			}
			live->_inventory = std::make_unique<SyncAccountInventory>(
				live->_session.get(), route,
				Fn<void(uint64_t)>(),
				[=](DesktopSyncChatInventory result) {
					if (const auto self = weak.get();
						self && self->Accept(command)) {
						self->Complete(MapDesktopSyncChatInventoryReply(
							command.origin, command.ticket, command.chat, result));
					}
				},
				[=] {
					const auto self = weak.get();
					return self && self->Current() && self->_model
						&& self->_origin == command.origin && self->_run == command.run
						&& self->_model->command().ticket == command.ticket;
				});
			live->_inventory->Start();
		});
	} else {
		const auto completed = [=](const MTPmessages_Messages &response) {
			const auto self = weak.get();
			if (!self || !self->Accept(command)) {
				return;
			}
			self->Peers(response);
			auto reply =
				DesktopSyncChatReply{command.origin, command.ticket, true};
			reply.messages = self->Messages(response);
			if (command.step == DesktopSyncChatStep::SettingsHint) {
				self->Peer(
					command,
					[=](std::optional<DesktopSyncChatPeer> peer) mutable {
						const auto live = weak.get();
						if (!live || !live->Accept(command)) {
							return;
						}
						reply.success = peer
							&& DesktopSyncChatEligibility(*peer)
							&& bool(*DesktopSyncChatEligibility(*peer))
							&& RecognizedDesktopSyncChatRead(
											response, command.chat)
							&& reply.messages.size() <= 1
							&& MapDesktopSyncChatSearchPage(
								   command.origin,
								   response,
								   DesktopSyncChatSearch::SettingsHint,
								   PeerToken{})
								   .recognized;
						const auto page = MapDesktopSyncChatSearchPage(
							command.origin,
							response,
							DesktopSyncChatSearch::SettingsHint,
							PeerToken{});
						if (page.total > 0 && reply.messages.empty()) {
							reply.success = false;
						}
						reply.hint = reply.success
							? SyncChatSettingsHint::Absent
							: SyncChatSettingsHint::Unknown;
						if (reply.success) {
							for (const auto &message : reply.messages) {
								const auto ownership =
									GetSyncChatMessageOwnership(
										MapDesktopSyncChatMessage(
											command.origin, *peer, message));
								if (ownership
									== SyncChatMessageOwnership::Unknown) {
									reply.success = false;
									reply.hint = SyncChatSettingsHint::Unknown;
								} else if (
									ownership == SyncChatMessageOwnership::Own
									&& IsSyncHistoryCandidate(message)) {
									reply.hint = SyncChatSettingsHint::Found;
								}
							}
						}
						live->Complete(std::move(reply));
					});
				return;
			} else {
				const auto target =
					command.search == DesktopSyncChatSearch::Global
						|| command.search == DesktopSyncChatSearch::Broadcasts
					? std::optional<PeerToken>()
					: std::optional(command.chat);
				reply.page = MapDesktopSyncChatSearchPage(
					command.origin, response, command.search, target);
			}
			self->Complete(std::move(reply));
		};
		const auto failed = [=](const MTP::Error &) {
			if (const auto self = weak.get(); self && self->Accept(command)) {
				self->Complete({command.origin, command.ticket});
			}
		};
		if (command.step == DesktopSyncChatStep::SettingsHint) {
			_api->request(DesktopSyncChatSearchRequest(
							  DesktopSyncChatSearch::SettingsHint,
							  _session->user()->input()))
				.done(std::move(completed))
				.fail(failed)
				.handleAllErrors()
				.send();
		} else if (
			command.search == DesktopSyncChatSearch::Global
			|| command.search == DesktopSyncChatSearch::Broadcasts) {
			const auto offset = command.cursor.peer
				? Loaded(*_session, *command.cursor.peer)
				: nullptr;
			if (command.cursor.peer && !offset) {
				Complete({command.origin, command.ticket});
				return;
			}
			_api
				->request(DesktopSyncChatGlobalRequest(
					command.search == DesktopSyncChatSearch::Broadcasts,
					command.cursor,
					offset ? offset->input()
						   : MTPInputPeer(MTP_inputPeerEmpty())))
				.done(std::move(completed))
				.fail(failed)
				.handleAllErrors()
				.send();
		} else {
			const auto peer = Loaded(*_session, command.chat);
			if (!peer) {
				Complete({command.origin, command.ticket});
				return;
			}
			_api->request(DesktopSyncChatSearchRequest(
							  command.search, peer->input(), command.cursor.id))
				.done(std::move(completed))
				.fail(failed)
				.handleAllErrors()
				.send();
		}
	}
}

void SyncChatSession::Complete(DesktopSyncChatReply reply) {
	if (_model && Current() && _model->Complete(reply, base::unixtime::now())) {
		Run();
	}
}

void SyncChatSession::Finish() {
	_status = _model->status();
	const auto disk = base::take(_disk);
	const auto current = _current;
	auto cache = _status.phase == DesktopSyncChatPhase::Use && _status.complete
			&& !_status.postingHeld
		? _model->cache()
		: std::optional<DesktopSyncChatCacheRecord>();
	const auto weak = base::make_weak(this);
	if (cache) {
		cache->loginBinding = _loginBinding;
		if (const auto peer = Loaded(*_session.get(), cache->mark.chat)) {
			cache->title = peer->name();
		}
	}
	crl::async([=] {
		const auto saved = SaveDesktopSyncChatDisk(disk, cache, current);
		const auto bytes = cache
			? SerializeDesktopSyncChatCache(*cache).value_or(disk->bytes)
			: disk->bytes;
		disk->lock.reset();
		crl::on_main([=] {
			const auto self = weak.get();
			if (!self || self->_current != current) {
				return;
			}
			if (!self->Current()) {
				self->Cancel(u"origin-retired"_q);
				return;
			}
			self->_busy = false;
			self->_requestTimeout.cancel();
			self->_api.reset();
			self->_inventory.reset();
			self->_setupScope = self->ScopeFor(*disk, cache ? cache : disk->cache);
			self->_expectedCacheBytes = bytes;
			self->_expectedStateIdentity = disk->stateIdentity;
			self->_expectedStateBytes = disk->stateBytes;
			if (saved && cache && self->_status.complete
				&& self->_status.phase == DesktopSyncChatPhase::Use
				&& !self->_status.postingHeld) {
				const auto now = int64_t(base::unixtime::now());
				const auto scope = self->ScopeFor(*disk, cache);
				if (self->_directBeforePost && (!self->_recent
					|| !ReuseDesktopSyncChatDiscovery(*self->_recent, scope, now))) {
					self->_recent.reset();
					self->Begin(false);
					return;
				}
				if (!self->_directBeforePost) {
					self->_recent = DesktopSyncChatDiscoveryReceipt{
						scope, now };
				}
			} else {
				self->_recent.reset();
			}
			if (!saved) {
				self->_status.phase = DesktopSyncChatPhase::Paused;
				self->_status.postingHeld = true;
				self->_status.reason = u"cache-write-held"_q;
			}
			if (self->_status.phase == DesktopSyncChatPhase::Searching) {
				self->_timer.callOnce(
					std::max(
						int64_t(1),
						self->_status.retryAt - base::unixtime::now())
					* 1000);
			}
			self->Publish();
		});
	});
}

void SyncChatSession::Publish() {
	const auto callbacks = base::take(_callbacks);
	const auto current = _current;
	const auto run = _run;
	const auto status = _status;
	const auto weak = base::make_weak(this);
	if (const auto session = _session.get()) {
		Changes.fire_copy(session);
	}
	for (const auto &callback : callbacks) {
		auto delivered = status;
		const auto self = weak.get();
		if (!status.postingHeld
			&& (!self || self->_current != current || self->_run != run || !self->Current())) {
			delivered.phase = DesktopSyncChatPhase::Paused;
			delivered.postingHeld = true;
			delivered.reason = u"completion-superseded"_q;
		}
		callback(delivered);
	}
}

DesktopSyncChatStatus SyncChatSession::status() const {
	if (_busy) {
		auto status = _status;
		status.postingHeld = true;
		status.reason = u"resolving"_q;
		return status;
	}
	if (_current && !Current()) {
		auto status = _status;
		status.phase = DesktopSyncChatPhase::Paused;
		status.postingHeld = true;
		status.reason = u"origin-retired"_q;
		return status;
	}
	return _status;
}

std::optional<DesktopSyncChatSetupSnapshot> SyncChatSession::Setup() const {
	if (_busy || !Current() || !_model || !_setupScope) {
		return {};
	}
	return DesktopSyncChatSetupSnapshot{
		*_setupScope, status(), _model->input(), _model->cache(), _run };
}

std::map<Main::Session*, std::unique_ptr<SyncChatSession>> Sessions;
SyncChatSession &For(not_null<Main::Session*> session) {
	auto &entry = Sessions[session];
	if (!entry || !entry->Owns(session)) {
		entry = std::make_unique<SyncChatSession>(session);
	}
	return *entry;
}

}

bool SyncChatIoBelongsToSession(
		not_null<Main::Session*> session,
		const DesktopSyncChatIo &route) {
	return For(session).HasOrigin(route);
}

std::optional<DesktopSyncChatLease> SyncChatIoForPost(
		not_null<Main::Session*> session,
		PeerToken chat) {
	return For(session).Lease(chat);
}

std::optional<DesktopSyncChatSetupSnapshot> SyncChatSetupSnapshotOf(
		not_null<Main::Session*> session) {
	return For(session).Setup();
}

void EnsureSyncChatResolved(not_null<Main::Session*> session, bool force) {
	For(session).Ensure(force);
}
void CheckSyncChatBeforePost(
	not_null<Main::Session*> session,
	Fn<void(DesktopSyncChatStatus)> done,
	DesktopSyncChatOperation operation) {
	For(session).Ensure(true, std::move(done), std::move(operation), true);
}
DesktopSyncChatStatus SyncChatStatusOf(not_null<Main::Session*> session) {
	return For(session).status();
}
bool SyncChatSelfResolved(not_null<Main::Session*> session) {
	const auto status = SyncChatStatusOf(session);
	return status.phase == DesktopSyncChatPhase::Use && !status.postingHeld
		&& status.resolution.readChat == PeerToken{};
}
rpl::producer<Main::Session*> SyncChatStatusChanges() {
	return Changes.events();
}

void StartSyncChatDiscovery() {
	static auto started = false;
	if (std::exchange(started, true)) {
		return;
	}
	static auto lifetime = rpl::lifetime();
	static auto accounts = rpl::lifetime();
	const auto refresh = [] {
		accounts.destroy();
		auto live = std::set<Main::Session*>();
		for (const auto &entry : Core::App().domain().accounts()) {
			const auto ready = [](Main::Session* session) {
				if (session) {
					crl::on_main(session, [=] {
						EnsureSyncChatResolved(session);
					});
				}
			};
			entry.account->sessionChanges() | rpl::on_next(ready, accounts);
			if (const auto session = entry.account->maybeSession()) {
				live.emplace(session);
				ready(session);
			}
		}
		std::erase_if(Sessions, [&](const auto &entry) {
			return !live.contains(entry.first);
		});
	};
	Core::App().domain().accountsChanges() | rpl::on_next(refresh, lifetime);
	refresh();
}

}
