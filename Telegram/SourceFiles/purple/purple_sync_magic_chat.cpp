#include "purple/purple_sync_magic_chat.h"

#include <algorithm>
#include <limits>

namespace Purple {
namespace {

constexpr auto kEditAttempts = 3;

std::optional<SyncChatBinding> Parse(const QByteArray &bytes) {
	return bytes.isEmpty() ? std::optional(SyncChatBinding()) : ParseSyncChatBinding(bytes);
}

const MagicChatCandidate *Find(const std::vector<MagicChatCandidate> &chats, uint64_t id) {
	const auto i = std::find_if(begin(chats), end(chats), [&](const MagicChatCandidate &chat) {
		return chat.channelId == id;
	});
	return (i != end(chats)) ? &*i : nullptr;
}

std::vector<MagicChatCandidate> Titled(const std::vector<MagicChatCandidate> &chats) {
	auto result = std::vector<MagicChatCandidate>();
	for (const auto &chat : chats) {
		if (IsMagicSyncChatTitle(chat.title)) {
			result.push_back(chat);
		}
	}
	return result;
}

int64_t Candidates(const SyncChatLookupInput &input) {
	return int64_t(input.visible.size())
		+ (input.allChats ? int64_t(input.allChats->size()) : 0);
}

SyncDiagField Method(const QString &method) {
	return SyncDiagCode(u"method"_q, method.toLower());
}

SyncMagicResult WaitUntil(int64_t retryAt) {
	auto result = SyncMagicResult();
	result.kind = SyncMagicResult::Kind::Wait;
	result.retryAt = retryAt;
	return result;
}

void SetCreateStep(
		SyncChatBinding &binding,
		SyncChatCreateStep step,
		int64_t at,
		std::optional<PeerToken> chat) {
	binding.createStep = step;
	binding.createAt = at;
	if (chat) {
		binding.created = chat;
	}
}

} // namespace

std::optional<int64_t> SyncMagicFloodSeconds(const QString &error) {
	for (const auto &prefix : { u"FLOOD_WAIT_"_q, u"FLOOD_PREMIUM_WAIT_"_q }) {
		if (!error.startsWith(prefix)) {
			continue;
		}
		const auto digits = error.mid(prefix.size());
		if (digits.isEmpty()) {
			return std::nullopt;
		}
		auto result = int64_t(0);
		for (const auto ch : digits) {
			if (ch < '0' || ch > '9') {
				return std::nullopt;
			}
			result = std::min(
				std::numeric_limits<int64_t>::max() / 20,
				result * 10 + (ch.unicode() - '0'));
		}
		return result;
	}
	return std::nullopt;
}

bool SyncMagicSpaceMismatch(
		const SyncDirectory &directory,
		const QString &space) {
	return directory.complete
		&& !directory.unreadableCandidate
		&& !space.isEmpty()
		&& !directory.groups.empty()
		&& std::none_of(
			directory.groups.begin(),
			directory.groups.end(),
			[&](const SyncDirectoryGroup &group) {
				return group.space == space;
			});
}

QString SyncMagicTriggerName(SyncMagicTrigger trigger) {
	switch (trigger) {
	case SyncMagicTrigger::Startup: return u"startup"_q;
	case SyncMagicTrigger::Screen: return u"screen"_q;
	case SyncMagicTrigger::Manual: return u"manual"_q;
	case SyncMagicTrigger::Auto: return u"auto"_q;
	case SyncMagicTrigger::Post: return u"post"_q;
	}
	return QString();
}

SyncMagicChat::SyncMagicChat(std::shared_ptr<SyncMagicPort> port)
: _port(std::move(port))
, _guard(std::make_shared<int>(0)) {
}

SyncMagicChat::~SyncMagicChat() = default;

const SyncMagicResult &SyncMagicChat::last() const {
	return _last;
}

const std::optional<SyncChatBinding> &SyncMagicChat::binding() const {
	return _binding;
}

std::optional<PeerToken> SyncMagicChat::hidden() const {
	if (_last.kind == SyncMagicResult::Kind::Unavailable
		|| _last.manual
		|| !_last.chat
		|| !_binding) {
		return std::nullopt;
	}
	return (_binding->mode == SyncChatBindingMode::Magic
		&& !_binding->listed
		&& _binding->chat == _last.chat)
		? _last.chat
		: std::nullopt;
}

void SyncMagicChat::setChanged(std::function<void()> changed) {
	_changed = std::move(changed);
}

void SyncMagicChat::changed() {
	if (_changed) {
		_changed();
	}
}

void SyncMagicChat::later(std::function<void()> task) {
	_port->post([weak = std::weak_ptr<int>(_guard), task = std::move(task)] {
		if (weak.lock()) {
			task();
		}
	});
}

bool SyncMagicChat::live(uint64_t run) const {
	return !_closed && run == _generation && _port->current();
}

void SyncMagicChat::choose(PeerToken chat, Done done) {
	userEdit(u"choose"_q, [=](SyncChatBinding &binding) {
		ClearSyncChatProblem(binding);
		binding.mode = SyncChatBindingMode::Manual;
		binding.chat = chat;
		binding.verifiedAt = 0;
		return true;
	}, std::move(done));
}

void SyncMagicChat::useDefault(Done done) {
	userEdit(u"default"_q, [](SyncChatBinding &binding) {
		auto next = SyncChatBinding();
		next.waits = binding.waits;
		next.login = binding.login;
		next.migration = binding.migration;
		next.listed = binding.listed;
		next.preserved = binding.preserved;
		binding = next;
		return true;
	}, std::move(done));
}

void SyncMagicChat::joined(Done done) {
	userEdit(u"join"_q, [](SyncChatBinding &binding) {
		ClearSyncChatProblem(binding);
		return true;
	}, std::move(done));
}

void SyncMagicChat::list(bool listed, std::function<void(bool)> done) {
	later([=] {
		const auto ok = !_closed && _port->current() && edit([&](SyncChatBinding &binding) {
			binding.listed = listed;
			return true;
		});
		if (ok) {
			_port->diag(u"listed"_q, { SyncDiagCode(u"value"_q, listed ? u"on"_q : u"off"_q) });
			changed();
		}
		if (done) {
			done(ok);
		}
	});
}

void SyncMagicChat::userEdit(const QString &action, Change change, Done done) {
	later([=] {
		if (_closed || !_port->current() || !edit(change)) {
			if (done) {
				done(SyncMagicResult());
			}
			return;
		}
		_port->diag(u"user_action"_q, { SyncDiagCode(u"action"_q, action) });
		++_wake;
		// WHY: a lookup still running read the old binding; its replies are dropped and its waiters get the new result.
		++_generation;
		_running = false;
		lookup(SyncMagicTrigger::Manual, true, done);
	});
}

void SyncMagicChat::lookup(SyncMagicTrigger trigger, bool fresh, Done done) {
	later([=] {
		if (_closed || !_port->current()) {
			if (done) {
				done(SyncMagicResult());
			}
			return;
		}
		if (done) {
			_waiters.push_back(done);
		}
		_fresh |= fresh;
		if (_running) {
			return;
		}
		_running = true;
		_created = false;
		_resumed = false;
		_allChats.reset();
		_retried.clear();
		_trigger = trigger;
		const auto run = ++_generation;
		_port->diag(u"lookup_start"_q, {
			SyncDiagCode(u"trigger"_q, SyncMagicTriggerName(trigger)),
		});
		step(run);
	});
}

void SyncMagicChat::shown() {
	later([=] {
		if (_closed || _last.kind != SyncMagicResult::Kind::Problem) {
			return;
		}
		const auto saved = edit([](SyncChatBinding &binding) {
			binding.problemShown = (binding.problem != SyncChatProblem::None);
			return true;
		});
		if (saved) {
			_port->diag(u"problem_shown"_q, {
				SyncDiagReason(SyncChatProblemCode(_last.problem)),
			});
			_last.show = false;
			changed();
		}
	});
}

void SyncMagicChat::raise(SyncChatProblem problem) {
	later([=] {
		if (_closed
			|| _last.kind != SyncMagicResult::Kind::Use
			|| problem == SyncChatProblem::None) {
			return;
		}
		const auto raised = edit([&](SyncChatBinding &binding) {
			RaiseSyncChatProblem(binding, problem);
			return true;
		});
		if (!raised) {
			return;
		}
		_port->diag(u"auto_paused"_q, { SyncDiagReason(SyncChatProblemCode(problem)) });
		_last.kind = SyncMagicResult::Kind::Problem;
		_last.problem = problem;
		_last.show = !raised->problemShown;
		_last.retryAt = 0;
		changed();
	});
}

void SyncMagicChat::close() {
	later([=] {
		_closed = true;
		++_generation;
		++_wake;
		finish(_generation, SyncMagicResult());
	});
}

void SyncMagicChat::step(uint64_t run) {
	if (!live(run)) {
		finish(run, SyncMagicResult());
		return;
	}
	const auto bytes = _port->readBinding();
	const auto parsed = bytes ? Parse(*bytes) : std::nullopt;
	if (!parsed) {
		unavailable(run, u"binding_unreadable"_q);
		return;
	}
	_binding = parsed;
	if (parsed->problem != SyncChatProblem::None
		&& _trigger == SyncMagicTrigger::Manual
		&& !_resumed) {
		_resumed = true;
		const auto cleared = edit([](SyncChatBinding &binding) {
			ClearSyncChatProblem(binding);
			return true;
		});
		if (!cleared) {
			unavailable(run, u"binding_unreadable"_q);
			return;
		}
		_port->diag(u"auto_resumed"_q, { SyncDiagReason(u"user_check"_q) });
		step(run);
		return;
	}
	const auto weak = std::weak_ptr<int>(_guard);
	_port->local(parsed->chat, [=](std::optional<SyncMagicLocal> local) {
		if (!weak.lock()) {
			return;
		}
		later([=] { planned(run, *parsed, local); });
	});
}

void SyncMagicChat::planned(
		uint64_t run,
		const SyncChatBinding &binding,
		std::optional<SyncMagicLocal> local) {
	if (!live(run)) {
		finish(run, SyncMagicResult());
		return;
	} else if (!local) {
		unavailable(run, u"lists_unreadable"_q);
		return;
	}
	_port->diag(u"lists"_q, {
		SyncDiagCode(u"main"_q, local->mainLoaded ? u"loaded"_q : u"partial"_q),
		SyncDiagCode(u"archive"_q, local->archiveLoaded ? u"loaded"_q : u"partial"_q),
	});
	auto input = SyncChatLookupInput();
	input.binding = binding;
	input.boundState = local->boundState;
	input.bound = local->bound;
	input.visible = local->visible;
	input.fresh = _fresh;
	input.now = _port->now();
	if (local->boundState == SyncChatBoundState::Unknown
		&& binding.chat
		&& binding.chat->kind == PeerTokenKind::Channel) {
		if (!_allChats) {
			proveAbsent(run, binding);
			return;
		}
		const auto found = Find(*_allChats, binding.chat->id);
		input.bound = found ? *found : MagicChatCandidate();
		input.boundState = found
			? SyncChatBoundState::Present
			: SyncChatBoundState::Gone;
	}
	if (_allChats) {
		input.allChats = Titled(*_allChats);
	}
	const auto plan = PlanSyncChatLookup(input);
	switch (plan.action) {
	case SyncChatLookupAction::Use: {
		auto result = SyncMagicResult();
		result.kind = SyncMagicResult::Kind::Use;
		result.chat = plan.chat;
		result.manual = (binding.mode == SyncChatBindingMode::Manual);
		done(run, result, _created ? u"created"_q : u"bound"_q, input);
	} return;
	case SyncChatLookupAction::Verify: verify(run, binding, plan, input); return;
	case SyncChatLookupAction::Wait: waiting(run, plan.retryAt, input); return;
	case SyncChatLookupAction::ProveAbsent: proveAbsent(run, binding); return;
	case SyncChatLookupAction::Create: create(run, binding); return;
	case SyncChatLookupAction::Problem:
		problem(run, plan.problem, plan.show, binding, &input);
		return;
	}
	unavailable(run, u"native_unavailable"_q);
}

void SyncMagicChat::proveAbsent(uint64_t run, const SyncChatBinding &binding) {
	using List = std::vector<MagicChatCandidate>;
	gated<List>(run, binding, QString::fromLatin1(kSyncChatAllChatsMethod), [=](auto done) {
		_port->allChats(std::move(done));
	}, [=](std::optional<List> list) {
		_allChats = list.value_or(List());
		step(run);
	}, [=](SyncMagicReply<List>) {
		unavailable(run, u"rpc"_q);
	});
}

void SyncMagicChat::verify(
		uint64_t run,
		const SyncChatBinding &binding,
		const SyncChatLookupPlan &plan,
		const SyncChatLookupInput &input) {
	if (!plan.chat || plan.chat->kind != PeerTokenKind::Channel) {
		unavailable(run, u"native_unavailable"_q);
		return;
	}
	const auto id = plan.chat->id;
	const auto rebind = plan.rebind;
	gated<MagicChatCandidate>(run, binding, QString::fromLatin1(kSyncChatFullInfoMethod), [=](auto done) {
		_port->fullInfo(id, std::move(done));
	}, [=](std::optional<MagicChatCandidate> chat) {
		if (!chat || chat->channelId != id) {
			unavailable(run, u"rpc"_q);
			return;
		}
		auto found = SyncChatProblem::None;
		const auto next = edit([&](SyncChatBinding &binding) {
			found = CompleteSyncChatVerify(binding, *chat, _port->now());
			return true;
		});
		if (!next) {
			unavailable(run, u"binding_unreadable"_q);
			return;
		} else if (found != SyncChatProblem::None) {
			problem(run, found, !next->problemShown, next, &input);
			return;
		}
		if (rebind && !_created) {
			_port->diag(u"lookup_result"_q, {
				SyncDiagCode(u"result"_q, input.binding.chat ? u"rebound"_q : u"found"_q),
				SyncDiagCount(u"count"_q, Candidates(input)),
			});
		}
		step(run);
	}, [=](SyncMagicReply<MagicChatCandidate>) {
		unavailable(run, u"rpc"_q);
	});
}

void SyncMagicChat::create(uint64_t run, const SyncChatBinding &binding) {
	if (_created) {
		unavailable(run, u"create_uncertain"_q);
		return;
	}
	const auto started = _port->now();
	const auto requested = edit([&](SyncChatBinding &binding) {
		SetCreateStep(binding, SyncChatCreateStep::Requested, started, std::nullopt);
		return true;
	});
	if (!requested) {
		unavailable(run, u"binding_unreadable"_q);
		return;
	}
	gated<MagicChatCandidate>(run, binding, QString::fromLatin1(kSyncChatCreateMethod), [=](auto done) {
		_port->create(std::move(done));
	}, [=](std::optional<MagicChatCandidate> chosen) {
		if (!chosen || !chosen->channelId) {
			uncertain(run, started);
			return;
		}
		const auto id = chosen->channelId;
		_created = true;
		auto known = _allChats.value_or(std::vector<MagicChatCandidate>());
		known.push_back(*chosen);
		_allChats = std::move(known);
		const auto chat = PeerToken{ PeerTokenKind::Channel, id };
		(void)edit([&](SyncChatBinding &binding) {
			SetCreateStep(binding, SyncChatCreateStep::Created, started, chat);
			return true;
		});
		cosmetic(run, QString::fromLatin1(kSyncMagicFoldersMethod), [=](auto done) {
			_port->archive(id, std::move(done));
		}, SyncChatCreateStep::Archived, started, chat, [=] {
			cosmetic(run, QString::fromLatin1(kSyncMagicNotifyMethod), [=](auto done) {
				_port->mute(id, std::move(done));
			}, SyncChatCreateStep::Muted, started, chat, [=] {
				step(run);
			});
		});
	}, [=](SyncMagicReply<MagicChatCandidate> failure) {
		notCreated();
		if (failure.error.contains(u"TOO_MUCH"_q)
			|| failure.error.startsWith(u"USER_RESTRICTED"_q)) {
			const auto bytes = _port->readBinding();
			problem(
				run,
				SyncChatProblem::CreateLimit,
				true,
				bytes ? Parse(*bytes) : std::nullopt,
				nullptr);
		} else {
			unavailable(run, u"rpc"_q);
		}
	}, [=] {
		uncertain(run, started);
	}, [=] {
		notCreated();
	});
}

void SyncMagicChat::notCreated() {
	(void)edit([](SyncChatBinding &binding) {
		SetCreateStep(binding, SyncChatCreateStep::None, 0, std::nullopt);
		return true;
	});
}

void SyncMagicChat::uncertain(uint64_t run, int64_t started) {
	(void)edit([&](SyncChatBinding &binding) {
		SetCreateStep(binding, SyncChatCreateStep::Uncertain, started, std::nullopt);
		return true;
	});
	_port->diag(u"lookup_result"_q, {
		SyncDiagCode(u"result"_q, u"absent_waiting"_q),
		SyncDiagReason(u"create_uncertain"_q),
	});
	finish(run, WaitUntil(started + kSyncChatCreateUncertainSeconds));
}

void SyncMagicChat::cosmetic(
		uint64_t run,
		const QString &method,
		Call<bool> call,
		SyncChatCreateStep step,
		int64_t started,
		PeerToken chat,
		std::function<void()> next) {
	const auto bytes = _port->readBinding();
	const auto binding = bytes ? Parse(*bytes) : std::nullopt;
	if (!binding) {
		next();
		return;
	}
	gatedQuiet<bool>(run, *binding, method, std::move(call), [=](std::optional<bool> ok) {
		if (ok.value_or(false)) {
			(void)edit([&](SyncChatBinding &binding) {
				SetCreateStep(binding, step, started, chat);
				return true;
			});
		}
		next();
	}, next);
}

void SyncMagicChat::problem(
		uint64_t run,
		SyncChatProblem problem,
		bool show,
		const std::optional<SyncChatBinding> &parsed,
		const SyncChatLookupInput *input) {
	const auto next = edit([&](SyncChatBinding &binding) {
		RaiseSyncChatProblem(binding, problem);
		return true;
	});
	const auto raised = next ? next : parsed;
	_port->diag(u"lookup_result"_q, {
		SyncDiagCode(u"result"_q, u"ineligible"_q),
		SyncDiagReason(SyncChatProblemCode(problem)),
		SyncDiagCount(u"count"_q, input ? Candidates(*input) : 0),
	});
	auto result = SyncMagicResult();
	result.kind = SyncMagicResult::Kind::Problem;
	result.chat = raised ? raised->chat : std::nullopt;
	result.problem = problem;
	result.show = raised ? !raised->problemShown : show;
	result.manual = raised && (raised->mode == SyncChatBindingMode::Manual);
	finish(run, result);
}

void SyncMagicChat::waiting(
		uint64_t run,
		int64_t retryAt,
		const SyncChatLookupInput &input) {
	_port->diag(u"lookup_result"_q, {
		SyncDiagCode(u"result"_q, u"absent_waiting"_q),
		SyncDiagCount(u"count"_q, Candidates(input)),
	});
	finish(run, WaitUntil(retryAt));
}

void SyncMagicChat::done(
		uint64_t run,
		SyncMagicResult result,
		const QString &code,
		const SyncChatLookupInput &input) {
	_port->diag(u"lookup_result"_q, {
		SyncDiagCode(u"result"_q, code),
		SyncDiagCount(u"count"_q, Candidates(input)),
	});
	_failures = 0;
	finish(run, std::move(result));
}

void SyncMagicChat::unavailable(uint64_t run, const QString &why) {
	_port->diag(u"lookup_result"_q, {
		SyncDiagCode(u"result"_q, u"absent_waiting"_q),
		SyncDiagReason(why),
	});
	finish(run, SyncMagicResult());
}

void SyncMagicChat::finish(uint64_t run, SyncMagicResult result) {
	if (run != _generation && !_closed) {
		return;
	}
	_running = false;
	_fresh = false;
	_allChats.reset();
	if (result.kind != SyncMagicResult::Kind::Unavailable || _closed) {
		_last = result;
	}
	auto waiters = std::exchange(_waiters, {});
	for (const auto &waiter : waiters) {
		waiter(result);
	}
	if (!_closed
		&& result.kind == SyncMagicResult::Kind::Wait
		&& result.retryAt > 0) {
		schedule(result.retryAt);
	}
	changed();
}

void SyncMagicChat::schedule(int64_t retryAt) {
	const auto wake = ++_wake;
	const auto weak = std::weak_ptr<int>(_guard);
	_port->delay([=] {
		if (weak.lock() && wake == _wake) {
			lookup(_trigger, false);
		}
	}, std::max(int64_t(1000), (retryAt - _port->now()) * 1000));
}

template <typename T>
void SyncMagicChat::gated(
		uint64_t run,
		const SyncChatBinding &binding,
		const QString &method,
		Call<T> call,
		std::function<void(std::optional<T>)> ok,
		std::function<void(SyncMagicReply<T>)> failed,
		std::function<void()> network,
		std::function<void()> beforeFlood) {
	const auto now = _port->now();
	const auto until = SyncChatWaitUntil(binding, method, now);
	if (until > 0) {
		_port->diag(u"wait_honored"_q, { Method(method), SyncDiagSeconds(until - now) });
		finish(run, WaitUntil(until));
		return;
	}
	const auto weak = std::weak_ptr<int>(_guard);
	call([=](SyncMagicReply<T> reply) {
		if (!weak.lock()) {
			return;
		}
		later([=] {
			if (!live(run)) {
				finish(run, SyncMagicResult());
				return;
			}
			const auto flood = reply.ok()
				? std::nullopt
				: SyncMagicFloodSeconds(reply.error);
			if (reply.ok()) {
				_port->diag(u"request"_q, { Method(method), SyncDiagCode(u"outcome"_q, u"ok"_q) });
				ok(reply.value);
			} else if (flood) {
				_port->diag(u"request"_q, {
					Method(method),
					SyncDiagCode(u"outcome"_q, u"flood"_q),
					SyncDiagSeconds(*flood),
				});
				if (*flood <= kSyncMagicShortFlood && _retried.insert(method).second) {
					_port->delay([=] {
						if (!weak.lock()) {
							return;
						}
						later([=] {
							if (live(run)) {
								gated<T>(run, binding, method, call, ok, failed, network, beforeFlood);
							} else {
								finish(run, SyncMagicResult());
							}
						});
					}, *flood * 1000 + 1000);
					return;
				}
				if (beforeFlood) {
					beforeFlood();
				}
				hold(run, method, now + *flood + 1, nullptr);
			} else if (reply.timeout
				|| reply.code < 0
				|| (reply.code >= 500 && reply.code < 600)) {
				_port->diag(u"request"_q, {
					Method(method),
					SyncDiagCode(u"outcome"_q, reply.timeout ? u"timeout"_q : u"network"_q),
					SyncDiagCount(u"code"_q, std::abs(reply.code)),
				});
				const auto backoff = std::min(
					int64_t(5) << std::min(_failures++, 4),
					int64_t(60));
				hold(run, method, now + backoff, network);
			} else {
				_port->diag(u"request"_q, {
					Method(method),
					SyncDiagCode(u"outcome"_q, u"error"_q),
					SyncDiagCount(u"code"_q, reply.code),
				});
				failed(reply);
			}
		});
	});
}

template <typename T>
void SyncMagicChat::gatedQuiet(
		uint64_t run,
		const SyncChatBinding &binding,
		const QString &method,
		Call<T> call,
		std::function<void(std::optional<T>)> ok,
		std::function<void()> skipped) {
	const auto now = _port->now();
	const auto until = SyncChatWaitUntil(binding, method, now);
	if (until != 0) {
		if (until > 0) {
			_port->diag(u"wait_honored"_q, { Method(method), SyncDiagSeconds(until - now) });
		}
		skipped();
		return;
	}
	const auto weak = std::weak_ptr<int>(_guard);
	call([=](SyncMagicReply<T> reply) {
		if (!weak.lock()) {
			return;
		}
		later([=] {
			if (!live(run)) {
				finish(run, SyncMagicResult());
				return;
			}
			const auto flood = reply.ok()
				? std::nullopt
				: SyncMagicFloodSeconds(reply.error);
			const auto outcome = reply.ok()
				? u"ok"_q
				: flood
				? u"flood"_q
				: reply.timeout
				? u"timeout"_q
				: (reply.code < 0)
				? u"network"_q
				: u"error"_q;
			_port->diag(u"request"_q, {
				Method(method),
				SyncDiagCode(u"outcome"_q, outcome),
				flood ? SyncDiagSeconds(*flood) : SyncDiagCount(u"code"_q, std::abs(reply.code)),
			});
			if (flood) {
				(void)edit([&](SyncChatBinding &binding) {
					RecordSyncChatWait(binding, method, now + *flood + 1);
					return true;
				});
			}
			if (reply.ok()) {
				ok(reply.value);
			} else {
				skipped();
			}
		});
	});
}

void SyncMagicChat::hold(
		uint64_t run,
		const QString &method,
		int64_t retryAt,
		std::function<void()> instead) {
	(void)edit([&](SyncChatBinding &binding) {
		RecordSyncChatWait(binding, method, retryAt);
		return true;
	});
	if (instead) {
		instead();
		return;
	}
	finish(run, WaitUntil(retryAt));
}

std::optional<SyncChatBinding> SyncMagicChat::edit(const Change &change) {
	for (auto i = 0; i != kEditAttempts; ++i) {
		const auto current = _port->readBinding();
		if (!current) {
			return std::nullopt;
		}
		auto next = Parse(*current);
		if (!next || !change(*next)) {
			return std::nullopt;
		}
		const auto bytes = SerializeSyncChatBinding(*next);
		if (!bytes) {
			return std::nullopt;
		}
		if (*bytes == *current || _port->writeBinding(*current, *bytes)) {
			_binding = next;
			return next;
		}
	}
	return std::nullopt;
}

} // namespace Purple
