#include "purple/purple_sync_chat_native.h"

namespace Purple {
namespace {

bool SameRoute(const DesktopSyncChatIo &route,
    const DesktopSyncChatOrigin &origin,
    PeerToken chat) {
	return route.peer.origin == origin && route.peer.chat == chat
	    && ValidDesktopSyncChatReadIo(route);
}

mtpBuffer Wire(const DesktopSyncNativeRequest &request) {
	auto result = mtpBuffer();
	std::visit([&](const auto &value) { value.write(result); }, request);
	return result;
}

std::optional<DesktopSyncNativeRequest> Effect(
    const DesktopSyncCarryPermit &permit) {
	if (const auto forward = DesktopSyncCarryForwardRequest(permit)) {
		return *forward;
	}
	if (const auto mark = DesktopSyncCarryMarkRequest(permit)) {
		return *mark;
	}
	return {};
}

struct Gathering {
	DesktopSyncCarryCommand command;
	DesktopSyncCarryReply reply;
	Fn<void(DesktopSyncCarryReply)> done;
	uint64_t epoch = 0;
};

}

class DesktopSyncChatNative::State final
: public std::enable_shared_from_this<State> {
public:
	State(DesktopSyncChatSetupSnapshot opening,
	    DesktopSyncNativeIo io,
	    DesktopSyncNativeRenewal renewal);
	bool live(uint64_t epoch) const;
	void send(DesktopSyncCarryCommand command,
	    Fn<void(DesktopSyncCarryReply)> done);
	void cancel();
	bool cleanupApplied(const DesktopSyncChatIo &route,
	    const MTPmessages_AffectedMessages &result);
	DesktopSyncChatMarkPort markPort(DesktopSyncChatMarkPort context);

private:
	void gather(DesktopSyncCarryCommand command,
	    Fn<void(DesktopSyncCarryReply)> done);
	void target(std::shared_ptr<Gathering> work,
	    DesktopSyncChatSetupSnapshot fresh);
	void source(std::shared_ptr<Gathering> work);
	void inventories(std::shared_ptr<Gathering> work);
	void exact(std::shared_ptr<Gathering> work);
	void finish(std::shared_ptr<Gathering> work, bool success);
	void effects(std::shared_ptr<Gathering> work, size_t index);
	void renew(DesktopSyncCarryCommand command,
	    DesktopSyncCarryPermit permit,
	    Fn<void(std::optional<DesktopSyncCarryPermit>)> done);
	void cleanup(std::shared_ptr<Gathering> work);
	DesktopSyncChatSetupSnapshot _opening;
	DesktopSyncNativeIo _io;
	DesktopSyncNativeRenewal _renewal;
	uint64_t _epoch = 1;
	std::optional<std::pair<DesktopSyncChatIo, MTPmessages_AffectedMessages>>
	    _appliedCleanup;
	std::optional<std::pair<DesktopSyncChatIo, MTPmessages_AffectedMessages>>
	    _appliedMark;
	bool _closed = false;
	bool _busy = false;
	std::shared_ptr<std::atomic<bool>> _admitted
	    = std::make_shared<std::atomic<bool>>(true);

};

DesktopSyncChatNative::State::State(DesktopSyncChatSetupSnapshot opening,
    DesktopSyncNativeIo io,
    DesktopSyncNativeRenewal renewal)
: _opening(std::move(opening))
, _io(std::move(io))
, _renewal(std::move(renewal)) {}

bool DesktopSyncChatNative::State::live(uint64_t epoch) const {
	if (_closed || epoch != _epoch || !_admitted->load()) {
		return false;
	}
	if (!_io.current || !_io.current()) {
		_admitted->store(false);
		return false;
	}
	return !_closed && epoch == _epoch && _admitted->load();
}

void DesktopSyncChatNative::State::cancel() {
	if (_closed) {
		return;
	}
	_closed = true;
	_admitted->store(false);
	++_epoch;
	_busy = false;
	_appliedCleanup.reset();
	_appliedMark.reset();
	if (_io.cancel) {
		_io.cancel();
	}
}

void DesktopSyncChatNative::State::gather(DesktopSyncCarryCommand command,
    Fn<void(DesktopSyncCarryReply)> done) {
	const auto work = std::make_shared<Gathering>();
	work->command = std::move(command);
	work->reply.command = work->command;
	work->epoch = _epoch;
	work->done = std::move(done);
	if (!live(work->epoch) || !_io.peer || !_io.inventory || !_io.request
	    || work->command.origin != _opening.scope.origin) {
		finish(work, false);
		return;
	}
	const auto self = shared_from_this();
	auto io = _io;
	io.current = [weak = weak_from_this(), epoch = work->epoch] {
		const auto self = weak.lock();
		return self && self->live(epoch);
	};
	ResolveDesktopSyncNative(_opening,
	    std::move(io),
	    [self, work](auto fresh) {
		    if (!self->live(work->epoch)) {
			    self->finish(work, false);
			    return;
		    }
		    if (!fresh || !fresh->status.complete
		        || fresh->scope.origin != work->command.origin
		        || fresh->input.selfUserId
		            != work->command.origin.account.userId) {
			    self->finish(work, false);
			    return;
		    }
		    self->target(work, *fresh);
	    });
}

void DesktopSyncChatNative::State::target(std::shared_ptr<Gathering> work,
    DesktopSyncChatSetupSnapshot fresh) {
	const auto self = shared_from_this();
	_io.peer(work->command.target, [self, work, fresh](auto route) {
		if (!self->live(work->epoch) || !route
		    || !SameRoute(*route, work->command.origin, work->command.target)
		    || !ValidDesktopSyncChatIo(*route)) {
			self->finish(work, false);
			return;
		}
		auto facts = DesktopSyncCarryFresh();
		facts.target = *route;
		facts.resolutionInput = fresh.input;
		facts.loginAuthority = self->_opening.scope.loginBinding;
		facts.current
		    = [admitted = self->_admitted] { return admitted->load(); };
		work->reply.fresh = std::move(facts);
		work->reply.oldRead = fresh.input.cacheRead;
		const auto gone = fresh.input.cache
		    && fresh.input.cache->chat == work->command.source
		    && fresh.input.cacheRead == SyncChatDirectRead::ChatGone;
		if (gone) {
			work->reply.access = SyncCarrySourceAccess::ChatGone;
			work->reply.fresh->source.peer.origin = work->command.origin;
			work->reply.fresh->source.peer.chat = work->command.source;
			self->inventories(work);
		} else {
			self->source(work);
		}
	});
}

void DesktopSyncChatNative::State::source(std::shared_ptr<Gathering> work) {
	const auto self = shared_from_this();
	_io.peer(work->command.source, [self, work](auto route) {
		if (!self->live(work->epoch) || !route
		    || !SameRoute(*route,
		        work->command.origin,
		        work->command.source)) {
			self->finish(work, false);
			return;
		}
		work->reply.fresh->source = *route;
		work->reply.access = route->peer.facts->protectedContent
		    ? SyncCarrySourceAccess::Protected
		    : SyncCarrySourceAccess::Readable;
		self->inventories(work);
	});
}

void DesktopSyncChatNative::State::inventories(
    std::shared_ptr<Gathering> work) {
	if (!live(work->epoch)) {
		finish(work, false);
		return;
	}
	const auto self = shared_from_this();
	const auto destination = [self, work] {
		if (!self->live(work->epoch)) {
			self->finish(work, false);
			return;
		}
		self->_io.inventory(work->reply.fresh->target,
		    [self, work](auto inventory) {
			    if (!self->live(work->epoch) || !inventory
			        || !SameRoute(inventory->route,
			            work->command.origin,
			            work->command.target)
			        || !DesktopSyncChatCarryInventory(*inventory)) {
				    self->finish(work, false);
				    return;
			    }
			    work->reply.destination = std::move(inventory);
			    self->exact(work);
		    });
	};
	const auto needsSource
	    = work->reply.access == SyncCarrySourceAccess::Readable
	    || (work->reply.access == SyncCarrySourceAccess::Protected
	        && (work->command.action
	                == SyncCarryJournalAction::ResolvePending
	            || work->command.cleanupRead));
	if (!needsSource) {
		destination();
		return;
	}
	_io.inventory(work->reply.fresh->source,
	    [self, work, destination](auto inventory) {
		    if (!self->live(work->epoch) || !inventory
		        || !SameRoute(inventory->route,
		            work->command.origin,
		            work->command.source)
		        || !DesktopSyncChatCarryInventory(*inventory)) {
			    self->finish(work, false);
			    return;
		    }
		    work->reply.source = std::move(inventory);
		    destination();
	    });
}

void DesktopSyncChatNative::State::exact(std::shared_ptr<Gathering> work) {
	if (!live(work->epoch)) {
		finish(work, false);
		return;
	}
	const auto &command = work->command;
	if (command.action == SyncCarryJournalAction::ResolvePending) {
		work->reply.sendIntent = command.pendingIntent;
		if (!_io.localText) {
			finish(work, false);
			return;
		}
		const auto self = shared_from_this();
		_io.localText([self, work](auto text) {
			if (text) {
				work->reply.retainedLocalText = *text;
			}
			self->finish(work, text.has_value());
		});
		return;
	}
	const auto cleanup = command.cleanupRead.has_value();
	if (!cleanup && command.action != SyncCarryJournalAction::ReadMark) {
		finish(work, true);
		return;
	}
	const auto route
	    = cleanup ? work->reply.fresh->source : work->reply.fresh->target;
	const auto id = cleanup ? command.cleanupRead->item.reference.messageId
	                        : command.markId;
	const auto request = DesktopSyncChatRecordRequest(route, id);
	if (!request) {
		finish(work, false);
		return;
	}
	const auto self = shared_from_this();
	std::visit(
	    [&](auto value) {
		    _io.request(std::move(value),
		        {},
		        [self, work, cleanup, route, id](auto result) {
			        if (!self->live(work->epoch) || !result.messages
			            || !DesktopSyncChatRecordMessage(route,
			                *result.messages,
			                id)) {
				        self->finish(work, false);
				        return;
			        }
			        if (cleanup) {
				        work->reply.cleanupResponse = result.messages;
				        if (work->reply.source) {
					        for (const auto &receipt :
					            work->reply.source->receipts) {
						        if (receipt.record.id != id) {
							        continue;
						        }
						        work->reply.fresh->sourceRecord
						            = SealDesktopSyncChatRecord(route,
						                *result.messages,
						                receipt.record,
						                receipt.record.documentId,
						                receipt.record.editDate);
					        }
				        }
			        } else {
				        work->reply.markResponse = result.messages;
				        work->reply.markId = id;
			        }
			        self->finish(work, true);
		        });
	    },
	    *request);
}

void DesktopSyncChatNative::State::finish(std::shared_ptr<Gathering> work,
    bool success) {
	if (!work->done) {
		return;
	}

	work->reply.outcome = success && live(work->epoch)
	    ? SyncCarryOutcome::Success
	    : SyncCarryOutcome::Unknown;
	auto done = std::move(work->done);
	done(std::move(work->reply));
}

void DesktopSyncChatNative::State::renew(DesktopSyncCarryCommand command,
    DesktopSyncCarryPermit permit,
    Fn<void(std::optional<DesktopSyncCarryPermit>)> done) {
	if (!live(_epoch) || !_renewal.effect || !permit.current()) {
		done({});
		return;
	}
	const auto self = shared_from_this();
	auto read = command;
	read.dispatch = false;
	read.effects.clear();
	gather(std::move(read), [self, command, permit, done](auto reply) {
		if (reply.outcome != SyncCarryOutcome::Success || !reply.fresh) {
			done({});
			return;
		}
		if (permit.sourceMessageId() > 0) {
			if (!reply.source) {
				done({});
				return;
			}
			const auto id = permit.sourceMessageId();
			const auto found = std::find_if(reply.source->receipts.begin(),
			    reply.source->receipts.end(),
			    [id](const auto &receipt) {
				    return receipt.record.id == id;
			    });
			if (found == reply.source->receipts.end()) {
				done({});
				return;
			}
			reply.fresh->sourceRecord = *found;
		}
		self->_renewal.effect(command, permit, *reply.fresh, done);
	});
}

void DesktopSyncChatNative::State::effects(std::shared_ptr<Gathering> work,
    size_t index) {
	if (!live(work->epoch)) {
		finish(work, false);
		return;
	}
	if (index == work->command.effects.size()) {
		finish(work, true);
		return;
	}
	const auto permit = work->command.effects[index];
	const auto self = shared_from_this();
	renew(work->command, permit, [self, work, index, permit](auto fresh) {
		if (!self->live(work->epoch) || !fresh
		    || !permit.compatible(*fresh)) {
			self->finish(work, false);
			return;
		}
		const auto active = std::make_shared<DesktopSyncCarryPermit>(*fresh);
		const auto renew
		    = [self, work, active](
		          Fn<void(std::optional<DesktopSyncCarryPermit>)> done) {
			      self->renew(work->command,
			          *active,
			          [active, done](auto next) {
				          if (!next || !active->compatible(*next)) {
					          done({});
					          return;
				          }
				          *active = *next;
				          done(std::move(next));
			          });
		      };
		const auto completedOnce
		    = std::make_shared<std::atomic<bool>>(false);
		const auto completed
		    = [self, work, index, active, completedOnce](auto updates) {
			      if (completedOnce->exchange(true)) {
				      return;
			      }
			      if (updates) {
				      work->reply.updates.push_back(*updates);
			      }
			      if (!self->live(work->epoch) || !updates
			          || !DesktopSyncChatUploadReceipt(active->target(),
			              *updates,
			              active->randomId())) {
				      self->finish(work, false);
				      return;
			      }
			      self->effects(work, index + 1);
		      };
		const auto request = Effect(*active);
		if (!request) {
			if (!self->_io.upload || active->uploadContent().isEmpty()) {
				self->finish(work, false);
				return;
			}
			self->_io.upload(*active, renew, completed);
			return;
		}
		const auto wire = Wire(*request);
		self->_io.request(
		    *request,
		    [renew, wire](auto ready) {
			    renew([wire, ready](auto next) {
				    const auto request = next ? Effect(*next) : std::nullopt;
				    ready(request && Wire(*request) == wire
				        && next->current());
			    });
		    },
		    [self,
		        active,
		        completed,
		        once = std::make_shared<std::atomic<bool>>(false)](
		        auto reply) {
			    if (once->exchange(true)) {
				    return;
			    }
			    if (reply.updates && self->_io.apply) {
				    self->_io.apply(*reply.updates, active->randomId());
			    }
			    completed(std::move(reply.updates));
		    });
	});
}

void DesktopSyncChatNative::State::cleanup(std::shared_ptr<Gathering> work) {
	const auto permit = work->command.cleanupPermit;
	if (!permit || !_io.cleanup || !_renewal.beginCleanup
	    || !_renewal.cleanup) {
		finish(work, false);
		return;
	}
	const auto self = shared_from_this();
	const auto renew = [self, work, permit](auto done) {
		self->_renewal.beginCleanup(work->command,
		    *permit,
		    [self, work, permit, done](auto read) {
			    if (!self->live(work->epoch) || !read) {
				    done({});
				    return;
			    }
			    auto command = work->command;
			    command.dispatch = false;
			    command.cleanupRead = *read;
			    self->gather(command,
			        [self, work, permit, read, done](auto reply) {
				        if (!self->live(work->epoch)
				            || reply.outcome != SyncCarryOutcome::Success) {
					        done({});
					        return;
				        }
				        self->_renewal.cleanup(work->command,
				            *permit,
				            *read,
				            std::move(reply),
				            done);
			        });
		    });
	};
	_io.cleanup(*permit, renew, [self, work, permit](auto result) {
		const auto applied = result && self->_io.affected
		    && self->_io.affected(permit->route(), *result);
		if (applied) {
			work->reply.cleanupResult = result;
			self->_appliedCleanup = std::make_pair(permit->route(), *result);
		}
		self->finish(work, applied);
	});
}

void DesktopSyncChatNative::State::send(DesktopSyncCarryCommand command,
    Fn<void(DesktopSyncCarryReply)> done) {
	const auto epoch = _epoch;
	if (!live(epoch)) {
		return;
	}
	const auto supported
	    = command.action == SyncCarryJournalAction::ResolvePending
	    || command.action == SyncCarryJournalAction::ResolveAgain
	    || command.action == SyncCarryJournalAction::Plan
	    || command.action == SyncCarryJournalAction::Deliver
	    || command.action == SyncCarryJournalAction::PostMark
	    || command.action == SyncCarryJournalAction::VerifyDestination
	    || command.action == SyncCarryJournalAction::ReadMark
	    || command.action == SyncCarryJournalAction::PrepareCache
	    || command.action == SyncCarryJournalAction::CompleteCache
	    || command.action == SyncCarryJournalAction::Cleanup;
	if (_busy || !live(epoch) || !supported || !command.ticket
	    || !command.run || command.operation.isEmpty()
	    || command.origin != _opening.scope.origin
	    || (command.dispatch
	        && command.action != SyncCarryJournalAction::Deliver
	        && command.action != SyncCarryJournalAction::PostMark
	        && command.action != SyncCarryJournalAction::ResolvePending
	        && command.action != SyncCarryJournalAction::Cleanup)) {
		auto reply = DesktopSyncCarryReply();
		reply.command = std::move(command);
		done(reply);
		return;
	}
	_busy = true;
	const auto self = shared_from_this();
	const auto complete = [self, epoch, done = std::move(done)](auto reply) {
		if (epoch == self->_epoch) {
			self->_busy = false;
		}
		if (!self->live(epoch)) {
			return;
		}
		done(std::move(reply));
	};
	if (!command.dispatch) {
		gather(std::move(command), complete);
		return;
	}
	const auto work = std::make_shared<Gathering>();
	work->command = std::move(command);
	work->reply.command = work->command;
	work->done = complete;
	work->epoch = epoch;
	for (const auto &permit : work->command.effects) {
		const auto target
		    = work->command.action == SyncCarryJournalAction::ResolvePending
		    ? work->command.source
		    : work->command.target;
		if (!permit.current()
		    || permit.source().peer.origin != work->command.origin
		    || permit.source().peer.chat != work->command.source
		    || permit.target().peer.origin != work->command.origin
		    || permit.target().peer.chat != target) {
			finish(work, false);
			return;
		}
	}
	if (work->command.action == SyncCarryJournalAction::Cleanup) {
		cleanup(work);
	} else if (work->command.effects.empty()) {
		finish(work, false);
	} else {
		effects(work, 0);
	}
}

DesktopSyncChatMarkPort DesktopSyncChatNative::State::markPort(
    DesktopSyncChatMarkPort context) {
	const auto self = shared_from_this();
	context.resolve = [self](auto done) {
		self->_io.onMain([self, done] {
			ResolveDesktopSyncNative(self->_opening, self->_io, done);
		});
	};
	context.exact = [self](auto row, auto done) {
		self->_io.onMain([self, row, done] {
			if (!self->live(self->_epoch)
			    || row.origin != self->_opening.scope.origin
			    || row.login != self->_opening.scope.loginBinding) {
				done({});
				return;
			}
			self->_io.peer(row.location.chat, [self, row, done](auto route) {
				if (!self->live(self->_epoch)) {
					return;
				}
				const auto request = route
				    ? DesktopSyncChatRecordRequest(*route,
				          row.location.markId)
				    : std::nullopt;
				if (!request
				    || !SameRoute(*route, row.origin, row.location.chat)) {
					done({});
					return;
				}
				std::visit(
				    [&](auto value) {
					    self->_io.request(std::move(value),
					        {},
					        [self, route, done](auto result) {
						        if (!self->live(self->_epoch)) {
							        return;
						        }
						        done(result.messages
						                ? std::optional(
						                      DesktopSyncChatMarkExact{
						                          *route,
						                          *result.messages,
						                          true})
						                : std::nullopt);
					        });
				    },
				    *request);
			});
		});
	};
	context.send = [self](auto permit, auto renew, auto done) {
		self->_io.onMain([self, permit, renew, done] {
			if (!self->live(self->_epoch) || !self->_io.removeMark) {
				done({});
				return;
			}
			self->_io.removeMark(permit,
			    renew,
			    [self,
			        permit,
			        done,
			        once = std::make_shared<std::atomic<bool>>(false)](
			        auto result) {
				    if (once->exchange(true)) {
					    return;
				    }
				    const auto applied = result && self->_io.affected
				        && self->_io.affected(permit.route(), *result);
				    if (applied) {
					    self->_appliedMark
					        = std::make_pair(permit.route(), *result);
				    }
				    if (!self->live(self->_epoch)) {
					    return;
				    }
				    done(applied ? result : std::nullopt);
			    });
		});
	};
	context.apply = [self](auto route, auto affected, auto done) {
		self->_io.onMain([self, route, affected, done] {
			auto accepted = false;
			if (self->_appliedMark
			    && route.peer.origin == self->_appliedMark->first.peer.origin
			    && route.peer.chat == self->_appliedMark->first.peer.chat) {
				auto expected = mtpBuffer(), actual = mtpBuffer();
				self->_appliedMark->second.write(expected);
				affected.write(actual);
				accepted = expected == actual;
				if (accepted) {
					self->_appliedMark.reset();
				}
			}
			if (self->live(self->_epoch)) {
				done(accepted);
			}
		});
	};
	context.cancel
	    = [self] { self->_io.onMain([self] { self->cancel(); }); };
	return context;
}

bool DesktopSyncChatNative::State::cleanupApplied(
    const DesktopSyncChatIo &route,
    const MTPmessages_AffectedMessages &result) {
	if (!_appliedCleanup
	    || route.peer.origin != _appliedCleanup->first.peer.origin
	    || route.peer.chat != _appliedCleanup->first.peer.chat) {
		return false;
	}
	auto expected = mtpBuffer(), actual = mtpBuffer();
	_appliedCleanup->second.write(expected);
	result.write(actual);
	if (expected != actual) {
		return false;
	}
	_appliedCleanup.reset();
	return true;
}

DesktopSyncChatNative::DesktopSyncChatNative(
    DesktopSyncChatSetupSnapshot opening,
    DesktopSyncNativeIo io,
    DesktopSyncNativeRenewal renewal)
: _state(std::make_shared<State>(std::move(opening),
      std::move(io),
      std::move(renewal))) {}

DesktopSyncChatNative::~DesktopSyncChatNative() {
	cancel();
}

DesktopSyncCarryTransport DesktopSyncChatNative::transport() const {
	const auto state = _state;
	auto result = DesktopSyncCarryTransport();
	result.send = [state](auto command, auto done) {
		state->send(std::move(command), std::move(done));
	};
	result.cancel = [state] { state->cancel(); };
	result.applyUpdates = [](const MTPUpdates &) {};
	result.applyCleanup = [state](const DesktopSyncChatIo &route,
	                          const MTPmessages_AffectedMessages &result) {
		return state->cleanupApplied(route, result);
	};
	return result;
}

DesktopSyncChatMarkPort DesktopSyncChatNative::markPort(
    DesktopSyncChatMarkPort context) const {
	return _state->markPort(std::move(context));
}

void DesktopSyncChatNative::cancel() {
	_state->cancel();
}

}
