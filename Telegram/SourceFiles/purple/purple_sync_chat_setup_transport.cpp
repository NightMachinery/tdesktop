#include "purple/purple_sync_chat_setup_transport.h"

#include "apiwrap.h"
#include "base/unixtime.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "purple/purple_sync_chat_write.h"

namespace Purple {

DesktopSyncChatSetupTransport::DesktopSyncChatSetupTransport(
		not_null<Main::Session*> session, DesktopSyncChatDiscoveryScope scope,
		DesktopSyncChatOperation operation, Fn<bool()> current, DesktopSyncChatSetupRenewal renew)
: _session(session)
, _scope(std::move(scope))
, _operation(std::move(operation))
, _current(std::move(current))
, _renew(std::move(renew))
, _api(&session->mtp(), [this] { return this->current(_generation); }) {
}

bool DesktopSyncChatSetupTransport::current(uint64_t generation) const {
	const auto session = _session.get();
	return session && _command && _done && generation == _generation
		&& _current && _current() && _command->origin == _scope.origin
		&& SyncChatSetupContextCurrent(session, _scope);
}

void DesktopSyncChatSetupTransport::cancel() {
	++_generation;
	_command.reset();
	_done = nullptr;
	_resumeReady = nullptr;
	_resumeProof = {};
	_api.cancelAll();
	_peerRequest.reset();
	_inventory.reset();
}

std::optional<DesktopSyncChatIo> DesktopSyncChatSetupTransport::route() const {
	if (!_session || !_command || !_command->journal.chat || !_peer
		|| _peer->origin != _scope.origin || _peer->chat != *_command->journal.chat) {
		return {};
	}
	const auto chat = _peer->chat;
	const auto id = chat.kind == PeerTokenKind::User ? peerFromUser(UserId(chat.id))
		: chat.kind == PeerTokenKind::Chat ? peerFromChat(ChatId(chat.id))
		: peerFromChannel(ChannelId(chat.id));
	const auto peer = chat == PeerToken{} ? _session->user().get()
		: _session->data().peerLoaded(id);
	if (!peer) {
		return {};
	}
	auto result = DesktopSyncChatIo{ *_peer, chat == PeerToken{}
		? MTPInputPeer(MTP_inputPeerSelf()) : peer->input() };
	return ValidDesktopSyncChatIo(result) ? std::optional(result) : std::nullopt;
}

void DesktopSyncChatSetupTransport::reply(
		DesktopSyncChatCreateReply result, uint64_t generation) {
	if (!current(generation)) {
		return;
	}
	result.origin = _command->origin;
	result.operation = _command->operation;
	result.ticket = _command->ticket;
	const auto callback = std::move(_done);
	callback(std::move(result));
}

void DesktopSyncChatSetupTransport::fail(const QString &type, uint64_t generation) {
	auto result = DesktopSyncChatCreateReply();
	result.outcome = DesktopSyncChatSetupError(type);
	reply(std::move(result), generation);
}

void DesktopSyncChatSetupTransport::readMark(
		int32_t id, bool reconcile, uint64_t generation) {
	const auto target = route();
	const auto request = target ? DesktopSyncChatRecordRequest(*target, id) : std::nullopt;
	if (!current(generation) || !request) {
		fail(u"PEER_ID_INVALID"_q, generation);
		return;
	}
	const auto weak = base::make_weak(this);
	std::visit([&](auto value) {
		_api.request(std::move(value)).done([weak, generation, id, reconcile](
				const MTPmessages_Messages &response) {
			const auto self = weak.get();
			if (!self || !self->current(generation)) {
				return;
			}
			auto result = DesktopSyncChatCreateReply();
			result.outcome = DesktopSyncChatCreateOutcome::Okay;
			result.readback = response;
			result.markId = id;
			result.randomId = reconcile ? self->_command->journal.randomId : 0;
			self->reply(std::move(result), generation);
		}).fail([weak, generation](const MTP::Error &error) {
			if (const auto self = weak.get()) { self->fail(error.type(), generation); }
		}).send();
	}, *request);
}

void DesktopSyncChatSetupTransport::mark(bool reconcile, uint64_t generation) {
	const auto target = route();
	const auto randomId = _command->journal.randomId;
	const auto request = target ? DesktopSyncChatMarkRequest(*target, randomId) : std::nullopt;
	if (!current(generation) || !request) {
		fail(u"PEER_ID_INVALID"_q, generation);
		return;
	}
	const auto weak = base::make_weak(this);
	_api.request(std::move(*request)).done([weak, generation, target, randomId, reconcile](
			const MTPUpdates &updates) {
		const auto self = weak.get();
		if (!self || !self->current(generation)) {
			return;
		}
		const auto id = DesktopSyncChatUploadReceipt(*target, updates, randomId);
		self->_session->api().applyUpdates(updates, randomId);
		if (!weak) { return; }
		if (!self->current(generation) || !id) {
			self->fail(u"UNRECOGNIZED_RECEIPT"_q, generation);
			return;
		}
		if (reconcile) {
			self->readMark(*id, true, generation);
		} else {
			auto result = DesktopSyncChatCreateReply();
			result.outcome = DesktopSyncChatCreateOutcome::Okay;
			result.randomId = randomId;
			result.markId = *id;
			self->reply(std::move(result), generation);
		}
	}).resumeEffect([weak, generation](Fn<void(bool)> ready) {
		if (const auto self = weak.get()) { self->renewEffect(std::move(ready), generation); }
		else { ready(false); }
	}).fail([weak, generation](const MTP::Error &error) {
		if (const auto self = weak.get()) { self->fail(error.type(), generation); }
	}).send();
}

void DesktopSyncChatSetupTransport::finishRenewal(bool allowed, uint64_t generation) {
	if (!current(generation) || !_resumeReady) { return; }
	const auto ready = std::move(_resumeReady);
	ready(allowed);
}

void DesktopSyncChatSetupTransport::approveRenewal(uint64_t generation) {
	if (!current(generation) || !_resumeReady || !_renew) {
		finishRenewal(false, generation);
		return;
	}
	const auto weak = base::make_weak(this);
	_renew(*_command, _resumeProof, [weak, generation](bool allowed) {
		if (const auto self = weak.get()) { self->finishRenewal(allowed, generation); }
	});
}

void DesktopSyncChatSetupTransport::renewEffect(Fn<void(bool)> ready, uint64_t generation) {
	if (!current(generation) || _resumeReady) { ready(false); return; }
	_resumeReady = std::move(ready);
	_resumeProof = {};
	const auto weak = base::make_weak(this);
	CheckSyncChatBeforePost(_session.get(), [weak, generation](DesktopSyncChatStatus) {
		const auto self = weak.get();
		if (!self || !self->current(generation)) { return; }
		self->_resumeProof.resolution = SyncChatSetupSnapshotOf(self->_session.get());
		if (self->_command->step == DesktopSyncChatCreateStep::Create) {
			self->approveRenewal(generation);
		} else { self->renewPeer(generation, false); }
	}, _operation);
}

void DesktopSyncChatSetupTransport::renewPeer(uint64_t generation, bool afterInventory) {
	if (!current(generation) || !_command->journal.chat) {
		finishRenewal(false, generation); return;
	}
	const auto weak = base::make_weak(this);
	_peerRequest = std::make_unique<DesktopSyncChatPeerRequest>(_session.get(), _scope.origin,
		*_command->journal.chat, [weak, generation] {
			const auto self = weak.get(); return self && self->current(generation);
		}, [weak, generation, afterInventory](std::optional<DesktopSyncChatPeer> peer) {
			const auto self = weak.get();
			if (!self || !self->current(generation)) { return; }
			if (!peer) { self->finishRenewal(false, generation); return; }
			self->_peer = peer;
			self->_resumeProof.peers.push_back(*peer);
			if (afterInventory || self->_command->step == DesktopSyncChatCreateStep::ReconcileMark) {
				self->approveRenewal(generation);
			} else if (self->_command->step == DesktopSyncChatCreateStep::PostMark) {
				const auto target = self->route();
				if (!target) { self->finishRenewal(false, generation); return; }
				self->_inventory = std::make_unique<SyncAccountInventory>(self->_session.get(), *target, nullptr,
					[weak, generation](DesktopSyncChatInventory inventory) {
						const auto self = weak.get();
						if (!self || !self->current(generation)) { return; }
						self->_resumeProof.inventory = std::move(inventory);
						self->renewPeer(generation, true);
					}, [weak, generation] { const auto self = weak.get(); return self && self->current(generation); });
				self->_inventory->Start();
			} else { self->renewMark(generation); }
		});
}

void DesktopSyncChatSetupTransport::renewMark(uint64_t generation) {
	const auto target = route();
	const auto request = target ? DesktopSyncChatRecordRequest(*target, _command->journal.markId) : std::nullopt;
	if (!current(generation) || !request) { finishRenewal(false, generation); return; }
	const auto weak = base::make_weak(this);
	std::visit([&](auto value) {
		_api.request(std::move(value)).done([weak, generation](const MTPmessages_Messages &response) {
			const auto self = weak.get();
			if (!self || !self->current(generation)) { return; }
			self->_resumeProof.readback = response;
			self->approveRenewal(generation);
		}).fail([weak, generation](const MTP::Error &) {
			if (const auto self = weak.get()) { self->finishRenewal(false, generation); }
		}).send();
	}, *request);
}

void DesktopSyncChatSetupTransport::send(DesktopSyncChatCreateCommand command,
		Fn<void(DesktopSyncChatCreateReply)> done) {
	cancel();
	_command = std::move(command);
	_done = std::move(done);
	const auto generation = _generation;
	if (!current(generation)) {
		return;
	}
	const auto weak = base::make_weak(this);
	const auto failed = [weak, generation](const MTP::Error &error) {
		if (const auto self = weak.get()) { self->fail(error.type(), generation); }
	};
	switch (_command->step) {
	case DesktopSyncChatCreateStep::Resolve: {
		auto operation = _operation;
		operation.current = [weak, generation] {
			const auto self = weak.get();
			return self && self->current(generation);
		};
		CheckSyncChatBeforePost(_session.get(), [weak, generation](DesktopSyncChatStatus) {
			const auto self = weak.get();
			if (!self || !self->current(generation)) { return; }
			auto result = DesktopSyncChatCreateReply();
			result.outcome = DesktopSyncChatCreateOutcome::Okay;
			result.resolution = SyncChatSetupSnapshotOf(self->_session.get());
			self->reply(std::move(result), generation);
		}, std::move(operation));
	} break;
	case DesktopSyncChatCreateStep::Create:
		_api.request(DesktopSyncChatCreateRequest()).done([weak, generation](const MTPUpdates &updates) {
			const auto self = weak.get();
			if (!self || !self->current(generation)) { return; }
			auto result = DesktopSyncChatCreateReply();
			result.outcome = DesktopSyncChatCreateOutcome::Okay;
			result.updates = EncodeDesktopSyncChatSetupUpdates(updates);
			if (DesktopSyncChatCreatedChannel(result.updates, self->_scope.origin.account.userId)) {
				self->_createdReceipt = std::pair(self->_command->operation, result.updates);
				result.creationProof = DesktopSyncChatCreationProof::MatchedRequest;
			}
			self->reply(std::move(result), generation);
		}).resumeEffect([weak, generation](Fn<void(bool)> ready) {
			if (const auto self = weak.get()) { self->renewEffect(std::move(ready), generation); }
			else { ready(false); }
		}).fail(failed).send();
		break;
	case DesktopSyncChatCreateStep::ReconcileCreate: {
		auto result = DesktopSyncChatCreateReply();
		if (_createdReceipt && _createdReceipt->first == _command->operation) {
			result.outcome = DesktopSyncChatCreateOutcome::Okay;
			result.creationProof = DesktopSyncChatCreationProof::MatchedRequest;
			result.updates = _createdReceipt->second;
		}
		reply(std::move(result), generation);
	} break;
	case DesktopSyncChatCreateStep::ApplyUpdates: {
		const auto updates = DecodeDesktopSyncChatSetupUpdates(_command->journal.updates);
		if (updates) {
			_session->api().applyUpdates(*updates);
			if (!weak) { return; }
			auto result = DesktopSyncChatCreateReply();
			result.outcome = DesktopSyncChatCreateOutcome::Okay;
			reply(std::move(result), generation);
		} else { fail(u"INVALID_UPDATES"_q, generation); }
	} break;
	case DesktopSyncChatCreateStep::PeerInfo:
		_peerRequest = std::make_unique<DesktopSyncChatPeerRequest>(_session.get(), _scope.origin,
			_command->journal.chat.value_or(PeerToken{}), [weak, generation] {
				const auto self = weak.get(); return self && self->current(generation);
			}, [weak, generation](std::optional<DesktopSyncChatPeer> peer) {
				const auto self = weak.get();
				if (!self || !self->current(generation)) { return; }
				self->_peer = peer;
				auto result = DesktopSyncChatCreateReply();
				result.outcome = DesktopSyncChatCreateOutcome::Okay;
				result.peer = std::move(peer);
				self->reply(std::move(result), generation);
			});
		break;
	case DesktopSyncChatCreateStep::Inventory: {
		const auto target = route();
		if (!target) { fail(u"PEER_ID_INVALID"_q, generation); break; }
		_inventory = std::make_unique<SyncAccountInventory>(_session.get(), *target, nullptr,
			[weak, generation](DesktopSyncChatInventory inventory) {
				const auto self = weak.get();
				if (!self || !self->current(generation)) { return; }
				auto result = DesktopSyncChatCreateReply();
				result.outcome = DesktopSyncChatCreateOutcome::Okay;
				result.inventory = std::move(inventory);
				self->reply(std::move(result), generation);
			}, [weak, generation] {
				const auto self = weak.get(); return self && self->current(generation);
			});
		_inventory->Start();
	} break;
	case DesktopSyncChatCreateStep::PostMark: mark(false, generation); break;
	case DesktopSyncChatCreateStep::ReconcileMark: mark(true, generation); break;
	case DesktopSyncChatCreateStep::ReadMark: readMark(_command->journal.markId, false, generation); break;
	case DesktopSyncChatCreateStep::Pin:
	case DesktopSyncChatCreateStep::Mute:
	case DesktopSyncChatCreateStep::Archive: {
		const auto target = route();
		const auto request = target ? DesktopSyncChatDecorationRequest(*_command, *target) : std::nullopt;
		if (!request) { fail(u"PEER_ID_INVALID"_q, generation); break; }
		std::visit([&](auto value) {
			_api.request(std::move(value)).done([weak, generation](const auto &response) {
				const auto self = weak.get();
				if (!self || !self->current(generation)) { return; }
				auto result = DesktopSyncChatCreateReply();
				result.outcome = DesktopSyncChatCreateOutcome::Okay;
				if constexpr (std::is_same_v<std::decay_t<decltype(response)>, MTPBool>) {
					if (mtpIsFalse(response)) { result.outcome = DesktopSyncChatCreateOutcome::Rejected; }
				} else {
					self->_session->api().applyUpdates(response);
					if (!weak) { return; }
				}
				self->reply(std::move(result), generation);
			}).resumeEffect([weak, generation](Fn<void(bool)> ready) {
				if (const auto self = weak.get()) { self->renewEffect(std::move(ready), generation); }
				else { ready(false); }
			}).fail(failed).send();
		}, *request);
	} break;
	default: fail(u"UNEXPECTED_STEP"_q, generation); break;
	}
}

}
