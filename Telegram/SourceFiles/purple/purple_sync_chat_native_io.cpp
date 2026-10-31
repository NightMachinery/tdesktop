#include "purple/purple_sync_chat_native.h"

#include "apiwrap.h"
#include "base/weak_ptr.h"
#include "crl/crl_async.h"
#include "crl/crl_on_main.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_sync_account_inventory.h"
#include "purple/purple_sync_chat_mark_transport.h"
#include "purple/purple_sync_chat_peer_request.h"
#include "purple/purple_sync_chat_service.h"
#include "purple/purple_sync_config_upload.h"
#include "purple/purple_sync_request_sender.h"

#include <QtCore/QDir>
#include <QtCore/QTimer>

namespace Purple {
namespace {

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

struct ReadJob {
	std::unique_ptr<DesktopSyncChatPeerRequest> peer;
	std::unique_ptr<SyncAccountInventory> inventory;
	std::shared_ptr<SyncConfigUpload> upload;
};

class NativeIo final : public std::enable_shared_from_this<NativeIo> {
public:
	NativeIo(not_null<Main::Session*> session,
	    DesktopSyncChatSetupSnapshot opening,
	    Fn<bool()> current);
	DesktopSyncNativeIo port();

private:
	bool identity() const;
	bool current() const;
	std::optional<MTPInputPeer> input(PeerToken chat) const;
	void peer(PeerToken chat,
	    Fn<void(std::optional<DesktopSyncChatIo>)> done);
	void inventory(DesktopSyncChatIo route,
	    Fn<void(std::optional<DesktopSyncChatInventory>)> done);
	void request(DesktopSyncNativeRequest request,
	    Fn<void(Fn<void(bool)>)> gate,
	    Fn<void(DesktopSyncNativeReply)> done);
	void upload(DesktopSyncCarryPermit permit,
	    DesktopSyncCarryRenew renew,
	    Fn<void(std::optional<MTPUpdates>)> done);
	bool affected(const DesktopSyncChatIo &route,
	    const MTPmessages_AffectedMessages &result);
	void localText(Fn<void(std::optional<QByteArray>)> done);
	void retire(uint64_t id);
	void cancel();
	base::weak_ptr<Main::Session> _session;
	DesktopSyncChatSetupSnapshot _opening;
	Fn<bool()> _current;
	SyncRequestSender _api;
	std::map<uint64_t, std::shared_ptr<ReadJob>> _jobs;
	uint64_t _job = 0;
	uint64_t _generation = 0;
	bool _closed = false;

};

NativeIo::NativeIo(not_null<Main::Session*> session,
    DesktopSyncChatSetupSnapshot opening,
    Fn<bool()> current)
: _session(session)
, _opening(std::move(opening))
, _current(std::move(current))
, _api(&session->mtp(), [this] { return identity(); }) {}

bool NativeIo::identity() const {
	const auto session = _session.get();
	return session
	    && SessionAccountKey(session) == _opening.scope.origin.account
	    && session->account().maybeSession() == session
	    && !session->account().loggingOut();
}

bool NativeIo::current() const {
	return !_closed && identity() && _current && _current()
	    && CachedSyncChatSetupContextCurrent(_session.get(), _opening.scope);
}

std::optional<MTPInputPeer> NativeIo::input(PeerToken chat) const {
	if (!current()
	    || !PeerTokenText(chat, _opening.scope.origin.account.userId)) {
		return {};
	}
	const auto peer = Loaded(*_session.get(), chat);
	return peer ? std::optional(chat == PeerToken{}
	                      ? MTPInputPeer(MTP_inputPeerSelf())
	                      : peer->input())
	            : std::nullopt;
}

void NativeIo::retire(uint64_t id) {
	const auto self = shared_from_this();
	crl::on_main([self, id] { self->_jobs.erase(id); });
}

void NativeIo::peer(PeerToken chat,
    Fn<void(std::optional<DesktopSyncChatIo>)> done) {
	if (!current()) {
		done({});
		return;
	}
	const auto self = shared_from_this();
	const auto id = ++_job;
	const auto generation = _generation;
	const auto job = std::make_shared<ReadJob>();
	_jobs.emplace(id, job);
	job->peer = std::make_unique<DesktopSyncChatPeerRequest>(
	    _session.get(),
	    _opening.scope.origin,
	    chat,
	    [self, generation] {
		    return generation == self->_generation && self->current();
	    },
	    [self, id, generation, done = std::move(done)](auto facts) {
		    self->retire(id);
		    const auto input
		        = self->input(facts ? facts->chat : PeerToken{});
		    if (generation != self->_generation || !facts || !input) {
			    done({});
			    return;
		    }
		    auto route = DesktopSyncChatIo{*facts, *input};
		    done(ValidDesktopSyncChatReadIo(route) ? std::optional(route)
		                                           : std::nullopt);
	    });
}

void NativeIo::inventory(DesktopSyncChatIo route,
    Fn<void(std::optional<DesktopSyncChatInventory>)> done) {
	if (!current() || !ValidDesktopSyncChatReadIo(route)
	    || route.peer.origin != _opening.scope.origin) {
		done({});
		return;
	}
	const auto self = shared_from_this();
	const auto id = ++_job;
	const auto generation = _generation;
	const auto job = std::make_shared<ReadJob>();
	_jobs.emplace(id, job);
	job->inventory = std::make_unique<SyncAccountInventory>(
	    _session.get(),
	    std::move(route),
	    Fn<void(uint64_t)>(),
	    [self, id, generation, done = std::move(done)](auto result) {
		    self->retire(id);
		    done(generation == self->_generation && self->current()
		            ? std::optional(std::move(result))
		            : std::nullopt);
	    },
	    [self, generation] {
		    return generation == self->_generation && self->current();
	    });
	job->inventory->Start();
}

void NativeIo::request(DesktopSyncNativeRequest request,
    Fn<void(Fn<void(bool)>)> gate,
    Fn<void(DesktopSyncNativeReply)> done) {
	if (!current()) {
		done({});
		return;
	}
	const auto self = shared_from_this();
	const auto generation = _generation;
	std::visit(
	    [&](auto value) {
		    using Response = typename decltype(value)::ResponseType;
		    constexpr auto effect = std::is_same_v<Response, MTPUpdates>;
		    if (effect && !gate) {
			    done({});
			    return;
		    }
		    _api.request(std::move(value))
		        .done([self, generation, done](const Response &response) {
			        if (generation != self->_generation && !effect) {
				        done({});
				        return;
			        }
			        auto result = DesktopSyncNativeReply();
			        if constexpr (effect) {
				        result.updates = response;
			        } else {
				        response.match(
				            [](const MTPDmessages_messagesNotModified &) {},
				            [&](const auto &data) {
					            if (const auto session
					                = self->_session.get()) {
						            session->data().processUsers(
						                data.vusers());
						            session->data().processChats(
						                data.vchats());
					            }
				            });
				        if (generation == self->_generation
				            && self->current()) {
					        result.messages = response;
				        }
			        }
			        done(std::move(result));
		        })
		        .fail([done](const MTP::Error &error) {
			        auto result = DesktopSyncNativeReply();
			        result.error = error.type();
			        done(std::move(result));
		        })
		        .resumeEffect(
		            [self, generation, gate](Fn<void(bool)> ready) {
			            if (generation != self->_generation
			                || !self->current()) {
				            ready(false);
			            } else if (gate) {
				            gate(std::move(ready));
			            } else {
				            ready(true);
			            }
		            })
		        .send();
	    },
	    std::move(request));
}

void NativeIo::upload(DesktopSyncCarryPermit permit,
    DesktopSyncCarryRenew renew,
    Fn<void(std::optional<MTPUpdates>)> done) {
	if (!current() || !permit.current() || !renew) {
		done({});
		return;
	}
	const auto self = shared_from_this();
	const auto id = ++_job;
	const auto generation = _generation;
	const auto job = std::make_shared<ReadJob>();
	_jobs.emplace(id, job);
	job->upload = std::make_shared<SyncConfigUpload>(
	    *_session.get(),
	    std::move(permit),
	    [self, id, job, done = std::move(done)](auto) {
		    const auto result = job->upload->carryResult();
		    self->retire(id);
		    done(result);
	    },
	    [self, generation] {
		    return generation == self->_generation && self->current();
	    },
	    std::move(renew));
	job->upload->Start();
}

bool NativeIo::affected(const DesktopSyncChatIo &route,
    const MTPmessages_AffectedMessages &result) {
	if (!identity() || route.peer.origin != _opening.scope.origin
	    || !ValidDesktopSyncChatReadIo(route)) {
		return false;
	}
	const auto peer = Loaded(*_session.get(), route.peer.chat);
	if (!peer) {
		return false;
	}
	_session->api().applyAffectedMessages(peer, result);
	return true;
}

void NativeIo::localText(Fn<void(std::optional<QByteArray>)> done) {
	const auto files = ResolveAccountFiles(_opening.scope.origin.account,
	    _opening.scope.owner,
	    AccountFileLayout::Desktop);
	if (!current() || !files) {
		done({});
		return;
	}
	const auto path = QDir(_opening.scope.root).filePath(files->settings);
	const auto weak = weak_from_this();
	const auto generation = _generation;
	crl::async([weak, generation, path, done = std::move(done)] {
		const auto file
		    = CaptureSyncPrivateFile(path, kSyncRecordMaximumBytes);
		crl::on_main([weak, generation, file, done] {
			const auto self = weak.lock();
			done(self && self->current() && self->_generation == generation
			            && file.status == SyncStoreStatus::Ready
			            && !file.absent()
			        ? std::optional(file.bytes)
			        : std::nullopt);
		});
	});
}

void NativeIo::cancel() {
	if (_closed) {
		return;
	}
	_closed = true;
	++_generation;
	_api.cancelAll();
	auto jobs = std::move(_jobs);
	for (const auto &[id, job] : jobs) {
		if (job->inventory) {
			job->inventory->Cancel();
		}
		if (job->upload) {
			job->upload->Cancel();
		}
	}
}

DesktopSyncNativeIo NativeIo::port() {
	const auto self = shared_from_this();
	auto result = DesktopSyncNativeIo();
	result.onMain = [](Fn<void()> task) { crl::on_main(std::move(task)); };
	result.delay = [](int ms, Fn<void()> task) {
		QTimer::singleShot(ms, std::move(task));
	};
	result.current = [self] { return self->current(); };
	result.input = [self](auto chat) { return self->input(chat); };
	result.peer = [self](auto chat, auto done) {
		self->peer(chat, std::move(done));
	};
	result.inventory = [self](auto route, auto done) {
		self->inventory(std::move(route), std::move(done));
	};
	result.request = [self](auto request, auto gate, auto done) {
		self->request(std::move(request), std::move(gate), std::move(done));
	};
	result.upload = [self](auto permit, auto renew, auto done) {
		self->upload(std::move(permit), std::move(renew), std::move(done));
	};
	result.apply = [self](const auto &updates, auto id) {
		if (self->identity()) {
			self->_session->api().applyUpdates(updates, id);
		}
	};
	result.affected = [self](const auto &route, const auto &value) {
		return self->affected(route, value);
	};
	const auto onMain
	    = [](Fn<void()> task) { crl::on_main(std::move(task)); };
	result.cleanup = [self, onMain](auto permit, auto renew, auto done) {
		if (!self->current()
		    || !SendDesktopSyncChatCleanup(
		        self->_api,
		        std::move(permit),
		        std::move(renew),
		        onMain,
		        [](int ms, Fn<void()> task) {
			        QTimer::singleShot(ms, std::move(task));
		        },
		        done)) {
			done({});
		}
	};
	result.removeMark = [self, onMain](auto permit, auto renew, auto done) {
		if (!self->current()
		    || !SendDesktopSyncChatMarkDelete(self->_api,
		        std::move(permit),
		        std::move(renew),
		        onMain,
		        done)) {
			done({});
		}
	};
	result.localText
	    = [self](auto done) { self->localText(std::move(done)); };
	result.cancel = [self] { self->cancel(); };
	result.dialogs = SyncChatLoadedDialogs(_session.get());
	return result;
}

}

DesktopSyncNativeIo MakeDesktopSyncNativeIo(not_null<Main::Session*> session,
    DesktopSyncChatSetupSnapshot opening,
    Fn<bool()> current) {
	return std::make_shared<NativeIo>(session,
	    std::move(opening),
	    std::move(current))
	    ->port();
}

DesktopSyncCarryTransport MakeDesktopSyncCarryTransport(
    not_null<Main::Session*> session,
    Fn<bool()> current,
    DesktopSyncNativeContext context) {
	auto io = MakeDesktopSyncNativeIo(session,
	    context.opening,
	    std::move(current));
	const auto owner
	    = std::make_shared<DesktopSyncChatNative>(std::move(context.opening),
	        std::move(io),
	        std::move(context.renewal));
	auto result = owner->transport();
	const auto send = result.send;
	const auto cancel = result.cancel;
	result.send = [owner, send](auto command, auto done) {
		send(std::move(command), std::move(done));
	};
	result.cancel = [owner, cancel] { cancel(); };
	return result;
}

DesktopSyncChatMarkPort MakeDesktopSyncMarkNativePort(
    not_null<Main::Session*> session,
    Fn<bool()> current,
    DesktopSyncChatSetupSnapshot opening) {
	auto io = MakeDesktopSyncNativeIo(session, opening, std::move(current));
	const auto owner
	    = std::make_shared<DesktopSyncChatNative>(std::move(opening),
	        std::move(io),
	        DesktopSyncNativeRenewal());
	auto result = owner->markPort({});
	const auto exact = result.exact;
	const auto send = result.send;
	const auto apply = result.apply;
	const auto cancel = result.cancel;
	result.exact = [owner, exact](auto row, auto done) {
		exact(std::move(row), std::move(done));
	};
	result.send = [owner, send](auto permit, auto renew, auto done) {
		send(std::move(permit), std::move(renew), std::move(done));
	};
	result.apply = [owner, apply](auto route, auto value, auto done) {
		apply(std::move(route), std::move(value), std::move(done));
	};
	result.cancel = [owner, cancel] { cancel(); };
	return result;
}

}
