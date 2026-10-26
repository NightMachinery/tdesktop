#include "purple/purple_sync_chat_peer_request.h"

#include "apiwrap.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "crl/crl_on_main.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "main/session/send_as_peers.h"
#include "mtproto/sender.h"
#include "purple/purple_config.h"

namespace Purple {
namespace {

PeerData *Loaded(Main::Session &session, PeerToken chat) {
	if (chat == PeerToken{}) {
		return session.user();
	}
	const auto id = chat.kind == PeerTokenKind::User
		? peerFromUser(UserId(chat.id)) : chat.kind == PeerTokenKind::Chat
		? peerFromChat(ChatId(chat.id)) : peerFromChannel(ChannelId(chat.id));
	return session.data().peerLoaded(id);
}

std::optional<PeerToken> Token(PeerId id, uint64_t self) {
	return DesktopSyncChatToken(peerToMTP(id), self);
}

}

class DesktopSyncChatPeerRequest::Impl final : public base::has_weak_ptr {
public:
	Impl(not_null<Main::Session*> session, DesktopSyncChatOrigin origin,
		PeerToken chat, Fn<bool()> current,
		Fn<void(std::optional<DesktopSyncChatPeer>)> done);
	bool Current() const;
	void Start(Fn<void(std::optional<DesktopSyncChatPeer>)> done);

private:
	base::weak_ptr<Main::Session> _session;
	DesktopSyncChatOrigin _origin;
	PeerToken _chat;
	Fn<bool()> _current;
	MTP::Sender _api;
	base::Timer _timeout;
	bool _finished = false;

};

DesktopSyncChatPeerRequest::Impl::Impl(
		not_null<Main::Session*> session, DesktopSyncChatOrigin origin,
		PeerToken chat, Fn<bool()> current,
		Fn<void(std::optional<DesktopSyncChatPeer>)> done)
: _session(session)
, _origin(std::move(origin))
, _chat(chat)
, _current(std::move(current))
, _api(&session->mtp()) {
	const auto weak = base::make_weak(this);
	const auto complete = [weak, done = std::move(done)](
			std::optional<DesktopSyncChatPeer> peer) {
		crl::on_main([weak, done, peer = std::move(peer)]() mutable {
			const auto self = weak.get();
			if (!self || self->_finished) {
				return;
			}
			const auto current = self->Current();
			self->_finished = true;
			self->_timeout.cancel();
			done(current ? std::move(peer) : std::nullopt);
		});
	};
	_timeout.setCallback([complete] { complete({}); });
	_timeout.callOnce(30000);
	Start(complete);
}

bool DesktopSyncChatPeerRequest::Impl::Current() const {
	const auto session = _session.get();
	return !_finished && session && ValidDesktopSyncChatOrigin(_origin)
		&& SessionAccountKey(session) == _origin.account
		&& session->account().maybeSession() == session
		&& !session->account().loggingOut() && _current && _current();
}

void DesktopSyncChatPeerRequest::Impl::Start(
		Fn<void(std::optional<DesktopSyncChatPeer>)> done) {
	if (!Current()) {
		done({});
		return;
	}

	const auto weak = base::make_weak(this);
	const auto _chat = this->_chat;
	const auto peer = Loaded(*_session, _chat);
	auto prepare = [=](DesktopSyncChatPeerInput input) mutable {
		if (const auto self = weak.get(); self && self->Current()) {
			const auto native = Loaded(*self->_session, _chat);
			if (!native) {
				done({});
				return;
			}
			input.origin = self->_origin;
			input.chat = _chat;
			input.canSendFiles =
				Data::CanSend(native, ChatRestriction::SendFiles);
			input.chosenSendAs = Token(
				self->_session->sendAsPeers().resolveChosen(native)->id,
				self->_origin.account.userId);
			done(MapDesktopSyncChatPeer(self->_origin, input));
		}
	};
	if (!peer) {
		done({});
		return;
	}
	if (_chat.kind == PeerTokenKind::Self
		|| _chat.kind == PeerTokenKind::User) {
		const auto user = peer->asUser();
		if (!user) {
			done({});
			return;
		}
		_api.request(MTPusers_GetFullUser(user->inputUser()))
			.done([=](const MTPusers_UserFull &response) mutable {
				const auto self = weak.get();
				if (!self || !self->Current()) {
					return;
				}
				const auto &data = response.data();
				self->_session->data().processUsers(data.vusers());
				auto input = DesktopSyncChatPeerInput();
				input.userFull = data.vfull_user();
				const auto id = _chat == PeerToken{}
					? self->_origin.account.userId
					: int64_t(_chat.id);
				for (const auto &value : data.vusers().v) {
					if (value.type() == mtpc_user
						&& value.c_user().vid().v == id) {
						input.user = value;
					}
				}
				prepare(std::move(input));
			})
			.fail([=](const MTP::Error &) {
				if (const auto self = weak.get();
					self && self->Current()) {
					done({});
				}
			})
			.handleAllErrors()
			.send();
	} else {
		auto completed = [=](const MTPmessages_ChatFull &response) mutable {
			const auto self = weak.get();
			if (!self || !self->Current()) {
				return;
			}
			const auto &data = response.data();
			auto input = DesktopSyncChatPeerInput();
			input.groupFull = data.vfull_chat();
			for (const auto &value : data.vchats().v) {
				if (value.type() == mtpc_channel
					&& _chat.kind == PeerTokenKind::Channel
					&& value.c_channel().vid().v == int64_t(_chat.id)) {
					input.group = value;
				} else if (
					value.type() == mtpc_chat
					&& _chat.kind == PeerTokenKind::Chat
					&& value.c_chat().vid().v == int64_t(_chat.id)) {
					input.group = value;
				}
			}
			if (!input.group) {
				done({});
				return;
			}
			self->_session->api().processFullPeer(peer, response);
			prepare(std::move(input));
		};
		const auto failed = [=](const MTP::Error &) {
			if (const auto self = weak.get(); self && self->Current()) {
				done({});
			}
		};
		if (_chat.kind == PeerTokenKind::Chat) {
			_api.request(MTPmessages_GetFullChat(MTP_long(_chat.id)))
				.done(std::move(completed))
				.fail(failed)
				.handleAllErrors()
				.send();
		} else if (peer->input().type() == mtpc_inputPeerChannel) {
			const auto &input = peer->input().c_inputPeerChannel();
			_api.request(MTPchannels_GetFullChannel(MTP_inputChannel(
							  input.vchannel_id(), input.vaccess_hash())))
				.done(std::move(completed))
				.fail(failed)
				.handleAllErrors()
				.send();
		} else {
			done({});
		}
	}
}

DesktopSyncChatPeerRequest::DesktopSyncChatPeerRequest(
		not_null<Main::Session*> session, DesktopSyncChatOrigin origin,
		PeerToken chat, Fn<bool()> current,
		Fn<void(std::optional<DesktopSyncChatPeer>)> done)
: _impl(std::make_unique<Impl>(session, std::move(origin), chat,
	std::move(current), std::move(done))) {
}

DesktopSyncChatPeerRequest::~DesktopSyncChatPeerRequest() = default;

}
