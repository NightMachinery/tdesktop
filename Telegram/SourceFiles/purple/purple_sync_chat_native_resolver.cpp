#include "purple/purple_sync_chat_native.h"

#include "base/unixtime.h"
#include "purple/purple_sync_history_scanner.h"

namespace Purple {
namespace {

std::vector<MTPMessage> Messages(const MTPmessages_Messages &response) {
	return response.match(
	    [](const MTPDmessages_messagesNotModified &) {
		    return std::vector<MTPMessage>();
	    },
	    [](const auto &data) {
		    return std::vector<MTPMessage>(data.vmessages().v.begin(),
		        data.vmessages().v.end());
	    });
}

std::optional<DesktopSyncChatCacheRecord> ReadOnlyCache(
    const DesktopSyncChatSetupSnapshot &opening) {
	auto cache = opening.cache;
	if (cache) {
		cache->carry.reset();
	}
	return cache;
}

class Resolution final : public std::enable_shared_from_this<Resolution> {
public:
	Resolution(DesktopSyncChatSetupSnapshot opening,
	    DesktopSyncNativeIo io,
	    Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)> done);
	void start();

private:
	bool live() const;
	void run();
	void complete(DesktopSyncChatReply reply);
	void finish();
	void exact(DesktopSyncChatCommand command);
	void evidence(DesktopSyncChatCommand command,
	    DesktopSyncNativeReply result);
	void search(DesktopSyncChatCommand command);
	DesktopSyncChatSetupSnapshot _opening;
	DesktopSyncNativeIo _io;
	Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)> _done;
	DesktopSyncChatDiscovery _model;
	uint64_t _ticket = 0;
	bool _finished = false;

};

Resolution::Resolution(DesktopSyncChatSetupSnapshot opening,
    DesktopSyncNativeIo io,
    Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)> done)
: _opening(std::move(opening))
, _io(std::move(io))
, _done(std::move(done))
, _model(_opening.scope.origin,
      ReadOnlyCache(_opening),
      _opening.scope.settingsBound,
      _opening.scope.settingsChat,
      _io.dialogs,
      _opening.scope.loginBinding) {}

bool Resolution::live() const {
	return !_finished && _io.current && _io.current();
}

void Resolution::start() {
	_model.Start(base::unixtime::now());
	run();
}

void Resolution::complete(DesktopSyncChatReply reply) {
	if (!live() || reply.ticket != _ticket) {
		return;
	}
	if (!_model.Complete(reply, base::unixtime::now())) {
		finish();
		return;
	}
	run();
}

void Resolution::finish() {
	if (_finished) {
		return;
	}
	const auto valid = live() && _model.status().complete;
	_finished = true;
	auto done = std::move(_done);
	if (!done) {
		return;
	}
	if (!valid) {
		done({});
		return;
	}
	_opening.status = _model.status();
	_opening.input = _model.input();
	done(std::move(_opening));
}

void Resolution::run() {
	if (!live()) {
		finish();
		return;
	}
	const auto command = _model.command();
	_ticket = command.ticket;
	if (command.step == DesktopSyncChatStep::Idle) {
		const auto status = _model.status();
		if (status.complete
		    && status.phase == DesktopSyncChatPhase::Searching
		    && status.retryAt > 0 && _io.delay) {
			const auto self = shared_from_this();
			const auto ticket = _ticket;
			const auto seconds = std::max(int64_t(1),
			    status.retryAt - base::unixtime::now());
			_io.delay(int(std::min(seconds, int64_t(60))) * 1000,
			    [self, ticket] {
				    if (!self->live() || self->_ticket != ticket) {
					    return;
				    }
				    self->_model.Tick(base::unixtime::now());
				    self->run();
			    });
			return;
		}
		finish();
		return;
	}
	const auto self = shared_from_this();
	if (command.step == DesktopSyncChatStep::PeerInfo) {
		_io.peer(command.chat, [self, command](auto route) {
			auto reply = DesktopSyncChatReply{command.origin,
			    command.ticket,
			    bool(route)};
			if (route) {
				reply.peer = route->peer;
			} else if (self->_opening.cache
			    && self->_opening.cache->mark.chat == command.chat) {
				auto identity = DesktopSyncChatPeer();
				identity.origin = command.origin;
				identity.chat = command.chat;
				reply.peer = identity;
				reply.success = true;
			}
			self->complete(std::move(reply));
		});
	} else if (command.step == DesktopSyncChatStep::Inventory) {
		_io.peer(command.chat, [self, command](auto route) {
			if (!self->live()) {
				return;
			}
			if (!route || !ValidDesktopSyncChatIo(*route)) {
				self->complete({command.origin, command.ticket});
				return;
			}
			self->_io.inventory(*route, [self, command](auto inventory) {
				self->complete(inventory
				        ? MapDesktopSyncChatInventoryReply(command.origin,
				              command.ticket,
				              command.chat,
				              *inventory)
				        : DesktopSyncChatReply{command.origin,
				              command.ticket});
			});
		});
	} else if (command.step == DesktopSyncChatStep::ExactMark) {
		exact(command);
	} else {
		search(command);
	}
}

void Resolution::exact(DesktopSyncChatCommand command) {
	const auto input = _io.input(command.chat);
	auto channel = std::optional<MTPInputChannel>();
	if (input && input->type() == mtpc_inputPeerChannel) {
		const auto &data = input->c_inputPeerChannel();
		channel = MTP_inputChannel(data.vchannel_id(), data.vaccess_hash());
	}
	if (!command.mark || !input) {
		complete({command.origin, command.ticket});
		return;
	}
	if (command.mark->markId == 0) {
		evidence(command, {});
		return;
	}
	const auto request = DesktopSyncChatExactReadRequest(command.chat,
	    command.mark->markId,
	    channel);
	if (!request) {
		complete({command.origin, command.ticket});
		return;
	}
	const auto self = shared_from_this();
	std::visit(
	    [&](auto value) {
		    _io.request(std::move(value), {}, [self, command](auto result) {
			    if (self->live() && self->_ticket == command.ticket) {
				    self->evidence(command, std::move(result));
			    }
		    });
	    },
	    *request);
}

void Resolution::evidence(DesktopSyncChatCommand command,
    DesktopSyncNativeReply result) {
	const auto self = shared_from_this();
	_io.peer(command.chat, [self, command, result](auto route) {
		if (!self->live() || self->_ticket != command.ticket) {
			return;
		}
		const auto input = self->_io.input(command.chat);
		if (!input) {
			self->complete({command.origin, command.ticket});
			return;
		}
		self->_io.request(MTPmessages_GetHistory(*input,
		                      MTP_int(0),
		                      MTP_int(0),
		                      MTP_int(0),
		                      MTP_int(1),
		                      MTP_int(0),
		                      MTP_int(0),
		                      MTP_long(0)),
		    {},
		    [self, command, route, result](auto history) {
			    auto reply
			        = DesktopSyncChatReply{command.origin, command.ticket};
			    reply.success = command.mark->markId == 0
			        || (result.messages
			            && RecognizedDesktopSyncChatRead(*result.messages,
			                command.chat));
			    if (route) {
				    reply.peer = route->peer;
			    }
			    if (result.messages) {
				    reply.messages = Messages(*result.messages);
			    }
			    if (history.messages) {
				    reply.chatReadable
				        = RecognizedDesktopSyncChatRead(*history.messages,
				            command.chat);
			    } else if (command.chat.kind == PeerTokenKind::Channel
			        && result.error == u"CHANNEL_PRIVATE"_q
			        && history.error == u"CHANNEL_PRIVATE"_q) {
				    reply.confirmedChatGone = true;
				    reply.chatReadable = false;
			    }
			    self->complete(std::move(reply));
		    });
	});
}

void Resolution::search(DesktopSyncChatCommand command) {
	const auto global = command.search == DesktopSyncChatSearch::Global
	    || command.search == DesktopSyncChatSearch::Broadcasts;
	const auto input = global
	    ? (command.cursor.peer
	              ? _io.input(*command.cursor.peer)
	              : std::optional<MTPInputPeer>(MTP_inputPeerEmpty()))
	    : _io.input(command.chat);
	if (!input) {
		complete({command.origin, command.ticket});
		return;
	}
	const auto request = global
	    ? DesktopSyncNativeRequest(DesktopSyncChatGlobalRequest(
	          command.search == DesktopSyncChatSearch::Broadcasts,
	          command.cursor,
	          *input))
	    : DesktopSyncNativeRequest(
	          DesktopSyncChatSearchRequest(command.search,
	              *input,
	              command.cursor.id));
	const auto self = shared_from_this();
	_io.request(request, {}, [self, command, global](auto result) {
		if (!self->live() || self->_ticket != command.ticket) {
			return;
		}
		if (!result.messages) {
			self->complete({command.origin, command.ticket});
			return;
		}
		auto reply
		    = DesktopSyncChatReply{command.origin, command.ticket, true};
		reply.messages = Messages(*result.messages);
		const auto page = MapDesktopSyncChatSearchPage(command.origin,
		    *result.messages,
		    command.search,
		    global ? std::nullopt : std::optional(command.chat));
		if (command.step != DesktopSyncChatStep::SettingsHint) {
			reply.page = page;
			self->complete(std::move(reply));
			return;
		}
		self->_io.peer(command.chat,
		    [self, command, page, reply](auto route) mutable {
			    reply.success = route && ValidDesktopSyncChatIo(*route)
			        && page.recognized && reply.messages.size() <= 1
			        && (page.total == 0 || !reply.messages.empty());
			    reply.hint = reply.success ? SyncChatSettingsHint::Absent
			                               : SyncChatSettingsHint::Unknown;
			    if (reply.success) {
				    for (const auto &message : reply.messages) {
					    const auto own = GetSyncChatMessageOwnership(
					        MapDesktopSyncChatMessage(command.origin,
					            route->peer,
					            message));
					    if (own == SyncChatMessageOwnership::Unknown) {
						    reply.success = false;
						    reply.hint = SyncChatSettingsHint::Unknown;
					    } else if (own == SyncChatMessageOwnership::Own
					        && IsSyncHistoryCandidate(message)) {
						    reply.hint = SyncChatSettingsHint::Found;
					    }
				    }
			    }
			    self->complete(std::move(reply));
		    });
	});
}

}

void ResolveDesktopSyncNative(DesktopSyncChatSetupSnapshot opening,
    DesktopSyncNativeIo io,
    Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)> done) {
	if (!io.current || !io.current() || !io.peer || !io.inventory
	    || !io.input || !io.request) {
		done({});
		return;
	}
	std::make_shared<Resolution>(std::move(opening),
	    std::move(io),
	    std::move(done))
	    ->start();
}

}
