#include "purple/purple_sync_chat_carry_owner.h"

#include "purple/purple_sync_chat_store.h"

#include "base/random.h"
#include <QtCore/QDir>
#include <QtCore/QUuid>

namespace Purple {

// GUI-only holder of a native transport. Port lambdas that may run on the worker keep only a
// weak pointer, so the native closures are always released on the GUI thread.
struct DesktopSyncChatCarryTransportBox {
	DesktopSyncCarryTransport transport;
	void cancel() {
		auto taken = std::exchange(transport, {});
		if (taken.cancel) { taken.cancel(); }
	}
};

namespace {

uint64_t RequestId() {
	auto value = uint64_t(0);
	while (!value) { base::RandomFill(&value, sizeof(value)); value &= uint64_t(std::numeric_limits<int64_t>::max()); }
	return value;
}

bool SameMoveOpening(const DesktopSyncChatSetupSnapshot &opening, const DesktopSyncChatSetupSnapshot &fresh) {
	const auto fingerprint = DesktopSyncChatMoveFingerprint(opening);
	return !fingerprint.isEmpty() && fresh.scope == opening.scope
		&& DesktopSyncChatMoveFingerprint(fresh) == fingerprint
		&& fresh.stateImage == opening.stateImage && fresh.cacheImage == opening.cacheImage
		&& fresh.input.cacheRead == opening.input.cacheRead;
}

bool Provisioned(const std::optional<DesktopSyncChatCreateJournal> &journal,
		const std::optional<DesktopSyncChatPeer> &peer, const DesktopSyncChatSetupSnapshot &opening) {
	return journal && peer && journal->created && !journal->pending && !journal->cancelled && !journal->adopted
		&& journal->step == DesktopSyncChatCreateStep::Done && journal->chat && journal->chat == peer->chat
		&& journal->account == opening.scope.origin.account
		&& journal->authority == DesktopSyncChatSetupAuthority(opening.scope)
		&& journal->provisionSource == DesktopSyncChatMoveFingerprint(opening)
		&& !journal->markId && !journal->markDate && SerializeDesktopSyncChatCreateJournal(*journal)
		&& DesktopSyncChatMoveEligibility(opening, *peer) == DesktopSyncChatSetupRefusal::None;
}

// GUI: one command through the transport. Updates and affectedMessages are acknowledged through
// the transport's own hooks exactly as the session carry service did; a refused cleanup result
// becomes Unknown so the executor reads it back instead of trusting it.
bool SendCarry(const std::shared_ptr<DesktopSyncChatCarryTransportBox> &box, Fn<void(Fn<void()>)> main,
		const Fn<bool(DesktopSyncChatCarrySwitch)> &activated, DesktopSyncCarryCommand command,
		std::shared_ptr<std::atomic<bool>> live, Fn<void(DesktopSyncCarryReply)> done) {
	if (!box || !box->transport.send || !live->load() || !DesktopSyncChatCarryCommandAllowed(command, activated)) { return false; }
	const auto weak = std::weak_ptr<DesktopSyncChatCarryTransportBox>(box);
	box->transport.send(command, [=](DesktopSyncCarryReply reply) mutable {
		main([=]() mutable {
			const auto box = weak.lock();
			if (!box) { return; }
			const auto &transport = box->transport;
			if (transport.applyUpdates) { for (const auto &updates : reply.updates) { transport.applyUpdates(updates); } }
			try {
				if (reply.cleanupResult && (!command.cleanupPermit || !transport.applyCleanup
					|| !transport.applyCleanup(command.cleanupPermit->route(), *reply.cleanupResult))) {
					reply.cleanupResult.reset(); reply.outcome = SyncCarryOutcome::Unknown;
				}
			} catch (...) { reply.cleanupResult.reset(); reply.outcome = SyncCarryOutcome::Unknown; }
			if (!live->load()) { return; }
			reply.command = command;
			if (reply.fresh) { reply.fresh->current = [live] { return live->load(); }; }
			done(std::move(reply));
		});
	});
	return true;
}

// GUI access to a controller's own engine. `valid` is the GUI-side generation and liveness
// check; `engine` runs its work on the store worker, with nullptr once the operation is gone.
struct ControllerEngine {
	Fn<bool()> valid;
	Fn<void(Fn<void(DesktopSyncChatCarry *)>)> engine;
	Fn<void(Fn<void()>)> main;
	std::shared_ptr<std::atomic<bool>> live;
};

// GUI -> store worker -> GUI, as Run::renewOnWorker: an answer reaches the transport only while
// the same operation is live on both sides of the hop.
template <typename Result, typename Work>
void RenewOnController(const ControllerEngine &access, Work work, Fn<void(std::optional<Result>)> ready) {
	if (!access.valid()) { ready({}); return; }
	access.engine([main = access.main, valid = access.valid, work = std::move(work), ready = std::move(ready)](
			DesktopSyncChatCarry *engine) mutable {
		auto result = engine ? work(*engine) : std::optional<Result>();
		main([valid, ready = std::move(ready), result = std::move(result)]() mutable {
			ready(valid() ? std::move(result) : std::nullopt);
		});
	});
}

// Follow/Fill and recovery own their engines. T5K renews before every effect, the first one
// included, so their transport renews against that engine exactly as Run renews its own.
DesktopSyncChatCarryRenewal ControllerRenewal(ControllerEngine access) {
	auto result = DesktopSyncChatCarryRenewal();
	result.effect = [access](DesktopSyncCarryCommand command, DesktopSyncCarryPermit permit,
			DesktopSyncCarryFresh fresh, Fn<void(std::optional<DesktopSyncCarryPermit>)> ready) {
		fresh.current = [live = access.live] { return live->load(); };
		RenewOnController<DesktopSyncCarryPermit>(access, [command = std::move(command), permit = std::move(permit),
				fresh = std::move(fresh)](DesktopSyncChatCarry &engine) {
			return engine.renew(command, permit, fresh);
		}, std::move(ready));
	};
	result.beginCleanup = [access](DesktopSyncCarryCommand command, DesktopSyncCleanupPermit permit,
			Fn<void(std::optional<DesktopSyncCleanupRead>)> ready) {
		RenewOnController<DesktopSyncCleanupRead>(access, [command = std::move(command), permit = std::move(permit)](
				DesktopSyncChatCarry &engine) {
			return engine.beginCleanupRenew(command, permit);
		}, std::move(ready));
	};
	result.cleanup = [access](DesktopSyncCarryCommand command, DesktopSyncCleanupPermit permit, DesktopSyncCleanupRead read,
			DesktopSyncCarryReply reply, Fn<void(std::optional<DesktopSyncCleanupPermit>)> ready) {
		RenewOnController<DesktopSyncCleanupPermit>(access, [command = std::move(command), permit = std::move(permit),
				read = std::move(read), reply = std::move(reply)](DesktopSyncChatCarry &engine) {
			return engine.renewCleanup(command, permit, read, reply);
		}, std::move(ready));
	};
	return result;
}

template <typename Callback>
auto OnMain(Fn<void(Fn<void()>)> main, Callback callback) {
	return [main = std::move(main), callback = std::move(callback)](auto ...values) mutable {
		main([callback, values...]() mutable { callback(std::move(values)...); });
	};
}

}

bool DesktopSyncChatCarrySwitchActivated(DesktopSyncChatCarrySwitch value) {
	switch (value) {
	case DesktopSyncChatCarrySwitch::Move: return DesktopSyncChatMoveActivated();
	case DesktopSyncChatCarrySwitch::FollowFill: return DesktopSyncChatFollowFillActivated();
	case DesktopSyncChatCarrySwitch::MarkRemoval: return DesktopSyncChatMarkRemovalActivated();
	case DesktopSyncChatCarrySwitch::Cleanup: return DesktopSyncChatCleanupActivated();
	case DesktopSyncChatCarrySwitch::Recovery: return DesktopSyncChatRecoveryActivated();
	}
	return false;
}

bool DesktopSyncChatCarryCommandAllowed(const DesktopSyncCarryCommand &command,
		const Fn<bool(DesktopSyncChatCarrySwitch)> &activated) {
	return !(command.cleanupPermit || command.cleanupRead) || (activated && activated(DesktopSyncChatCarrySwitch::Cleanup));
}

SyncChatCarryStoreScope DesktopSyncChatCarryStoreScope(const DesktopSyncChatSetupSnapshot &opening) {
	const auto &scope = opening.scope;
	return {scope.origin.account, scope.binding, scope.loginBinding,
		QDir(scope.root).filePath(DesktopSyncChatCacheFileName(scope.origin.account))};
}

std::optional<SyncCarryJournalStart> DesktopSyncChatMoveStart(
		const DesktopSyncChatMoveDraft &draft, const DesktopSyncChatPeer &target, bool cleanup) {
	const auto &opening = draft.opening;
	const auto parsed = ParseSyncLocalState(opening.stateImage);
	const auto eligible = DesktopSyncChatEligibility(target);
	const auto expected = draft.selection.choice == DesktopSyncChatSetupChoice::Create
		|| (draft.selection.peer && SameDesktopSyncChatDisclosure(*draft.selection.peer, target));
	if (!parsed || !eligible || !*eligible || !expected || !draft.selection.readerConsent
		|| target.origin != opening.scope.origin || target.chat == opening.scope.settingsChat
		|| DesktopSyncChatMoveEligibility(opening, target) != DesktopSyncChatSetupRefusal::None) { return {}; }
	auto input = SyncCarryJournalStart();
	input.operationId = QUuid::createUuid().toString(QUuid::Id128);
	input.account = opening.scope.origin.account;
	input.source = opening.scope.settingsChat; input.target = target.chat;
	input.mode = SyncCarryMode::Switch;
	input.state = parsed.state; input.preferenceToken = opening.scope.binding;
	input.resolution = ResolveSyncChat(opening.input);
	input.oldRead = opening.input.cacheRead;
	input.sourceAccess = SyncCarrySourceAccess::Unknown;
	input.targetEligibility = *eligible;
	input.readerConsentAccepted = true; input.operationConfirmed = true;
	input.removeOldMarkAuthorized = cleanup && draft.removeOldMark;
	input.loginAuthority = DesktopSyncCarryLogin(opening.scope.loginBinding);
	input.markText = DesktopSyncChatMarkText();
	input.markRequestId = QString::number(RequestId()); input.ownRequestId = QString::number(RequestId());
	return input;
}

std::optional<SyncCarryJournalStart> DesktopSyncChatFollowStart(const DesktopSyncChatConsentSelection &selection) {
	const auto &opening = selection.opening;
	const auto &offer = selection.offer;
	const auto eligible = DesktopSyncChatEligibility(offer.peer);
	const auto resolution = ResolveSyncChat(opening.input);
	if ((offer.action != SyncChatAction::Follow && offer.action != SyncChatAction::Fill)
		|| !eligible || !*eligible || resolution.action != offer.action || resolution.winner != offer.winner) { return {}; }
	auto input = SyncCarryJournalStart();
	input.account = opening.scope.origin.account;
	input.source = offer.source; input.target = offer.winner.chat;
	input.mode = offer.action == SyncChatAction::Follow ? SyncCarryMode::Follow : SyncCarryMode::Fill;
	if (opening.stateImage.isEmpty()) {
		input.stateAbsentConfirmed = true;
	} else {
		const auto parsed = ParseSyncLocalState(opening.stateImage);
		if (!parsed) { return {}; }
		input.state = parsed.state;
	}
	input.preferenceToken = opening.scope.binding;
	input.resolution = resolution;
	input.oldRead = opening.input.cacheRead;
	input.sourceAccess = SyncCarrySourceAccess::Unknown;
	input.targetEligibility = *eligible;
	input.loginAuthority = DesktopSyncCarryLogin(opening.scope.loginBinding);
	input.markText = DesktopSyncChatMarkText();
	input.ownRequestId = QString::number(RequestId());
	return input;
}

std::optional<DesktopSyncChatCarryResume> DesktopSyncChatCarryResumeOpening(
		const DesktopSyncChatDiscoveryScope &scope, const QString &directory) {
	if (!SyncStoreWorkerThread() || directory.isEmpty() || scope.root.isEmpty() || scope.binding.isEmpty()
		|| SyncCarryStartupAdmission(directory, scope.binding) == SyncStoreStatus::Ready) { return {}; }
	const auto journalFile = CaptureSyncPrivateFile(directory + u"/carry.json"_q, kSyncCarryJournalStorageLimit);
	const auto journal = journalFile.status == SyncStoreStatus::Ready && !journalFile.absent()
		? InspectSyncCarryJournal(journalFile.bytes, scope.binding) : std::nullopt;
	if (!journal || journal->account != scope.origin.account) { return {}; }
	auto result = DesktopSyncChatCarryResume{{scope}, journal->mode};
	auto &opening = result.opening;
	const auto state = CaptureSyncPrivateFile(directory + u"/state.json"_q, kSyncCarryJournalStorageLimit);
	if (state.status != SyncStoreStatus::Ready) { return {}; }
	if (!state.absent()) {
		const auto parsed = ParseSyncLocalState(state.bytes);
		if (!parsed || CheckSyncAccountBinding(parsed.state, scope.binding) != SyncAccountBindingVerdict::Bound) { return {}; }
		opening.stateImage = state.bytes;
		opening.scope.settingsBound = true;
		opening.scope.settingsChat = parsed.state.chat;
		opening.scope.space = parsed.state.space;
	}
	const auto cache = CaptureSyncPrivateFile(DesktopSyncChatCarryStoreScope(opening).cacheFile, 64 * 1024);
	if (cache.status != SyncStoreStatus::Ready) { return {}; }
	if (!cache.absent()) {
		const auto parsed = ParseDesktopSyncChatCache(cache.bytes, scope.origin.account);
		if (parsed.status != DesktopSyncChatCacheStatus::Valid || parsed.paused
			|| (parsed.record && (parsed.record->account != scope.origin.account
				|| parsed.record->loginBinding != scope.loginBinding))) { return {}; }
		opening.cacheImage = cache.bytes;
		opening.cache = parsed.record;
		if (parsed.record) {
			opening.scope.mark = parsed.record->mark;
			opening.scope.carry = parsed.record->carry.has_value();
			opening.input.cache = parsed.record->mark;
		}
	}
	opening.input.selfUserId = scope.origin.account.userId;
	opening.input.settingsBound = opening.scope.settingsBound;
	opening.input.settingsChat = opening.scope.settingsChat;
	return result;
}

bool DesktopSyncChatCarryResumeActivated(SyncCarryMode mode, const Fn<bool(DesktopSyncChatCarrySwitch)> &activated) {
	if (!activated) { return false; }
	switch (mode) {
	case SyncCarryMode::Switch:
		return activated(DesktopSyncChatCarrySwitch::Move) || activated(DesktopSyncChatCarrySwitch::Recovery);
	case SyncCarryMode::RepairMark: return activated(DesktopSyncChatCarrySwitch::Recovery);
	case SyncCarryMode::Follow:
	case SyncCarryMode::Fill: return activated(DesktopSyncChatCarrySwitch::FollowFill);
	}
	return false;
}

// Move and startup resume: the owner drives the engine itself. The Run lives and dies on the GUI
// thread; worker tasks hold only the engine slot, so the engine is created, used and destroyed on
// the worker and never keeps the Run alive there.
class DesktopSyncChatCarryOwner::Run final : public std::enable_shared_from_this<Run> {
public:
	struct Slot { std::unique_ptr<DesktopSyncChatCarry> engine; };

	Run(DesktopSyncChatCarryHost host, std::shared_ptr<std::atomic<bool>> live,
		Fn<void(DesktopSyncCarryProgress)> report)
	: host(std::move(host)), live(std::move(live)), report(std::move(report)) {
	}
	~Run() { host.worker([slot = slot] { slot->engine.reset(); }); }
	bool gui() const { return live->load() && host.current && host.current(); }
	void halt(QString reason, SyncStoreStatus status = SyncStoreStatus::Ready) {
		if (live->load() && report) { report({.reason = std::move(reason), .storeStatus = status}); }
	}
	template <typename Callback>
	auto step(Callback callback) {
		const auto run = ++ticket;
		return OnMain(host.main, [weak = weak_from_this(), run, callback = std::move(callback)](auto ...values) mutable {
			const auto self = weak.lock();
			if (!self || self->ticket != run) { return; }
			++self->ticket;
			if (!self->gui()) { self->halt(u"carry-context-changed"_q); return; }
			callback(*self, std::move(values)...);
		});
	}

	void startMove() { resolve(false); }
	void resolve(bool final) {
		if (!gui() || !host.resolve) { halt(u"carry-context-changed"_q); return; }
		host.resolve(step([final](Run &self, std::optional<DesktopSyncChatSetupSnapshot> fresh) {
			if (!fresh || !SameMoveOpening(self.draft->opening, *fresh)) { self.halt(u"move-opening-changed"_q); return; }
			if (self.draft->selection.choice == DesktopSyncChatSetupChoice::Create && !self.provisioned) {
				self.provision();
			} else {
				self.inspect(final);
			}
		}));
	}
	void provision() {
		if (!host.provision) { halt(u"move-provision-held"_q); return; }
		const auto resume = std::exchange(provisionStarted, true);
		auto request = DesktopSyncChatMoveDraft{draft->opening, {DesktopSyncChatSetupChoice::Create, {}, true, true}, false, false};
		host.provision(std::move(request), resume, step([](Run &self,
				std::optional<DesktopSyncChatCreateJournal> journal, std::optional<DesktopSyncChatPeer> peer) {
			if (!Provisioned(journal, peer, self.draft->opening)) { self.halt(u"move-provision-held"_q); return; }
			self.provisioned = *peer;
			self.halt(u"move-provisioned"_q);
			self.resolve(false);
		}));
	}
	void inspect(bool final) {
		const auto expected = draft->selection.choice == DesktopSyncChatSetupChoice::Create ? provisioned : draft->selection.peer;
		if (!expected || !host.inspect) { halt(u"move-choice-held"_q); return; }
		host.inspect(expected->chat, step([final, expected](Run &self, std::optional<DesktopSyncChatPeer> actual) {
			if (!actual || !SameDesktopSyncChatDisclosure(*expected, *actual)
				|| DesktopSyncChatMoveEligibility(self.draft->opening, *actual) != DesktopSyncChatSetupRefusal::None) {
				self.halt(u"move-readers-changed"_q); return;
			}
			self.target = *actual;
			if (final) { self.begin(); } else { self.resolve(true); }
		}));
	}
	void begin() {
		auto input = DesktopSyncChatMoveStart(*draft, *target, cleanup);
		if (!input) { halt(u"move-opening-held"_q); return; }
		open(std::move(input), target, DesktopSyncCarryOpening{draft->opening.stateImage, draft->opening.cacheImage, cleanup && draft->cleanup});
	}

	void startResume() {
		const auto scope = host.scope ? host.scope() : std::nullopt;
		if (!scope || !host.resumeOpening) { halt(u"carry-context-changed"_q); return; }
		host.worker([live = live, scope = *scope, read = host.resumeOpening, directory = host.directory,
				done = step([](Run &self, std::optional<DesktopSyncChatCarryResume> found, bool unfinished) {
			// A carry marker whose journal cannot be read, or belongs to another account, is not
			// absent: it ends held for discovery to report and never starts anything.
			if (!found) {
				self.report(unfinished
					? DesktopSyncCarryProgress{.reason = u"carry-journal-unreadable"_q, .terminal = true}
					: DesktopSyncCarryProgress{.reason = u"no-carry-journal"_q, .absent = true});
				return;
			}
			if (!DesktopSyncChatCarryResumeActivated(found->mode, self.host.activated)) {
				self.report({.reason = u"carry-resume-held"_q, .terminal = true}); return;
			}
			self.resumed = found->opening;
			self.open(std::nullopt, std::nullopt, std::nullopt);
		})]() mutable {
			if (!live->load()) { return; }
			const auto path = directory ? directory(scope) : std::nullopt;
			const auto unfinished = path && SyncCarryStartupAdmission(*path, scope.binding) != SyncStoreStatus::Ready;
			done(read(scope), unfinished);
		});
	}

	const DesktopSyncChatSetupSnapshot &opening() const { return draft ? draft->opening : *resumed; }
	void open(std::optional<SyncCarryJournalStart> input, std::optional<DesktopSyncChatPeer> peer,
			std::optional<DesktopSyncCarryOpening> images) {
		if (!gui()) { halt(u"carry-context-changed"_q); return; }
		const auto directory = host.directory ? host.directory(opening().scope) : std::nullopt;
		if (!directory || !host.lease || !host.transport) { halt(u"carry-unavailable"_q); return; }
		if (!box) {
			box = std::make_shared<DesktopSyncChatCarryTransportBox>();
			box->transport = host.transport(opening(), renewal(), [live = live, current = host.current] {
				return live->load() && current && current();
			});
		}
		if (!box->transport.send) { halt(u"carry-transport-held"_q); return; }
		opened = true;
		if (host.quiesce) { host.quiesce(); }
		const auto weak = weak_from_this();
		host.worker([slot = slot, live = live, lease = host.lease, main = host.main, directory = *directory,
				scope = DesktopSyncChatCarryStoreScope(opening()), origin = opening().scope.origin,
				receive = OnMain(host.main, [weak](DesktopSyncCarryProgress progress) {
					if (const auto self = weak.lock()) { self->receive(std::move(progress)); }
				}), busy = OnMain(host.main, [weak] {
					if (const auto self = weak.lock()) { self->halt(u"carry-writer-busy"_q, SyncStoreStatus::LockBusy); }
				}), input = std::move(input), peer = std::move(peer), images = std::move(images)]() mutable {
			if (!live->load()) { return; }
			if (!slot->engine) {
				auto taken = lease(origin.account);
				if (!taken) { busy(); return; }
				slot->engine = std::make_unique<DesktopSyncChatCarry>(directory, scope, origin, live, std::move(taken), receive);
			}
			slot->engine->open(std::move(input), std::move(peer), u"config"_q, std::move(images));
		});
	}
	void receive(DesktopSyncCarryProgress progress) {
		if (!live->load()) { return; }
		const auto self = shared_from_this();
		const auto command = progress.command;
		if (progress.journal) { journal = true; }
		rejected = !progress.journal && !command && progress.storeStatus != SyncStoreStatus::Ready;
		if (report) { report(std::move(progress)); }
		if (!command || !live->load()) { return; }
		const auto sent = SendCarry(box, host.main, host.activated, *command, live,
				[worker = host.worker, slot = slot, live = live, command = *command](DesktopSyncCarryReply reply) {
			worker([slot, live, command, reply = std::move(reply)]() mutable {
				if (!slot->engine || !live->load() || reply.command.ticket != command.ticket
					|| reply.command.run != command.run || reply.command.operation != command.operation
					|| reply.command.revision != command.revision) { return; }
				slot->engine->complete(std::move(reply));
			});
		});
		if (!sent) { halt(command->cleanupPermit || command->cleanupRead ? u"cleanup-held"_q : u"carry-transport-held"_q); }
	}
	void retry() {
		if (!gui()) { return; }
		if (!opened || (rejected && !journal)) {
			release();
			if (draft) { resolve(false); } else if (resumed) { open(std::nullopt, std::nullopt, std::nullopt); } else { startResume(); }
			return;
		}
		if (rejected) { release(); open(std::nullopt, std::nullopt, std::nullopt); return; }
		withEngine([](DesktopSyncChatCarry &engine) { engine.resume(); });
	}
	void release() {
		rejected = false; opened = false;
		host.worker([slot = slot] { slot->engine.reset(); });
	}
	void withEngine(Fn<void(DesktopSyncChatCarry &)> work) {
		host.worker([slot = slot, live = live, work = std::move(work)] {
			if (slot->engine && live->load()) { work(*slot->engine); }
		});
	}
	void close() {
		live->store(false);
		if (box) { box->cancel(); }
		host.worker([slot = slot] {
			try { if (slot->engine) { slot->engine->cancel(); } } catch (...) {}
			slot->engine.reset();
		});
	}
	template <typename Result, typename Work>
	void renewOnWorker(Work work, Fn<void(std::optional<Result>)> ready) {
		if (!gui()) { ready({}); return; }
		host.worker([slot = slot, live = live, work = std::move(work), done = OnMain(host.main,
				[weak = weak_from_this(), ready = std::move(ready)](std::optional<Result> result) {
			const auto self = weak.lock(); ready(self && self->gui() ? std::move(result) : std::nullopt);
		})]() mutable {
			done(slot->engine && live->load() ? work(*slot->engine) : std::optional<Result>());
		});
	}
	DesktopSyncChatCarryRenewal renewal() {
		auto result = DesktopSyncChatCarryRenewal();
		const auto weak = weak_from_this();
		result.effect = [weak](DesktopSyncCarryCommand command, DesktopSyncCarryPermit permit,
				DesktopSyncCarryFresh fresh, Fn<void(std::optional<DesktopSyncCarryPermit>)> ready) {
			const auto self = weak.lock();
			if (!self) { ready({}); return; }
			fresh.current = [live = self->live] { return live->load(); };
			self->renewOnWorker<DesktopSyncCarryPermit>([command = std::move(command), permit = std::move(permit), fresh = std::move(fresh)](DesktopSyncChatCarry &engine) {
				return engine.renew(command, permit, fresh);
			}, std::move(ready));
		};
		result.beginCleanup = [weak](DesktopSyncCarryCommand command, DesktopSyncCleanupPermit permit,
				Fn<void(std::optional<DesktopSyncCleanupRead>)> ready) {
			const auto self = weak.lock();
			if (!self) { ready({}); return; }
			self->renewOnWorker<DesktopSyncCleanupRead>([command = std::move(command), permit = std::move(permit)](DesktopSyncChatCarry &engine) {
				return engine.beginCleanupRenew(command, permit);
			}, std::move(ready));
		};
		result.cleanup = [weak](DesktopSyncCarryCommand command, DesktopSyncCleanupPermit permit, DesktopSyncCleanupRead read,
				DesktopSyncCarryReply reply, Fn<void(std::optional<DesktopSyncCleanupPermit>)> ready) {
			const auto self = weak.lock();
			if (!self) { ready({}); return; }
			self->renewOnWorker<DesktopSyncCleanupPermit>([command = std::move(command), permit = std::move(permit), read = std::move(read),
					reply = std::move(reply)](DesktopSyncChatCarry &engine) {
				return engine.renewCleanup(command, permit, read, reply);
			}, std::move(ready));
		};
		return result;
	}

	DesktopSyncChatCarryHost host;
	std::shared_ptr<std::atomic<bool>> live;
	Fn<void(DesktopSyncCarryProgress)> report;
	std::shared_ptr<Slot> slot = std::make_shared<Slot>();
	std::optional<DesktopSyncChatMoveDraft> draft;
	std::optional<DesktopSyncChatSetupSnapshot> resumed;
	std::optional<DesktopSyncChatPeer> provisioned;
	std::optional<DesktopSyncChatPeer> target;
	std::shared_ptr<DesktopSyncChatCarryTransportBox> box;
	uint64_t ticket = 0;
	bool cleanup = false;
	bool provisionStarted = false;
	bool opened = false;
	bool journal = false;
	bool rejected = false;

};

struct DesktopSyncChatCarryOwner::Active {
	DesktopSyncChatCarryKind kind = DesktopSyncChatCarryKind::None;
	uint64_t generation = 0;
	QString window;
	std::shared_ptr<std::atomic<bool>> live = std::make_shared<std::atomic<bool>>(true);
	std::shared_ptr<DesktopSyncChatCarryTransportBox> transport;
	std::shared_ptr<DesktopSyncChatMarkPort> mark;
	std::shared_ptr<Run> run;
	std::unique_ptr<DesktopSyncChatConsentFlow> consent;
	std::unique_ptr<DesktopSyncChatRecovery> recovery;
	std::unique_ptr<DesktopSyncChatMarkActions> marks;

	void stop() {
		live->store(false);
		if (transport) { transport->cancel(); }
		if (mark && mark->cancel) { auto cancel = std::exchange(mark->cancel, {}); cancel(); }
		if (run) { run->close(); }
		if (consent) { consent->close(); }
		if (recovery) { recovery->close(); }
		if (marks) { marks->close(); }
		consent.reset(); recovery.reset(); marks.reset(); run.reset(); transport.reset(); mark.reset();
	}
};

DesktopSyncChatCarryOwner::DesktopSyncChatCarryOwner(DesktopSyncChatCarryHost host)
: _host(std::move(host))
, _self(std::make_shared<DesktopSyncChatCarryOwner*>(this)) {
}

DesktopSyncChatCarryOwner::~DesktopSyncChatCarryOwner() {
	*_self = nullptr;
	if (_active) { _active->stop(); }
}

bool DesktopSyncChatCarryOwner::admit(DesktopSyncChatCarryKind kind, QString window, DesktopSyncChatCarrySwitch gate) {
	if (busy() || !_host.activated || !_host.current || !_host.current() || !_host.worker || !_host.main) { return false; }
	if (kind != DesktopSyncChatCarryKind::Resume && !_host.activated(gate)) { return false; }
	if (_active) { _active->stop(); }
	_active = std::make_unique<Active>();
	_active->kind = kind; _active->generation = ++_generation; _active->window = window;
	_state = {kind, _generation, std::move(window)};
	_state.running = true;
	publish();
	return true;
}

bool DesktopSyncChatCarryOwner::resume(QString window) {
	if (!_host.activated || !(_host.activated(DesktopSyncChatCarrySwitch::Move) || _host.activated(DesktopSyncChatCarrySwitch::Recovery)
		|| _host.activated(DesktopSyncChatCarrySwitch::FollowFill))
		|| !admit(DesktopSyncChatCarryKind::Resume, std::move(window), DesktopSyncChatCarrySwitch::Move)) { return false; }
	const auto generation = _generation;
	const auto self = std::weak_ptr<DesktopSyncChatCarryOwner*>(_self);
	_active->run = std::make_shared<Run>(_host, _active->live, [self, generation](DesktopSyncCarryProgress value) {
		if (const auto owner = self.lock(); owner && *owner) { (*owner)->progress(generation, std::move(value)); }
	});
	_active->run->startResume();
	return true;
}

bool DesktopSyncChatCarryOwner::move(DesktopSyncChatMoveDraft draft, QString window) {
	if (window.isEmpty() || !admit(DesktopSyncChatCarryKind::Move, std::move(window), DesktopSyncChatCarrySwitch::Move)) { return false; }
	const auto generation = _generation;
	const auto self = std::weak_ptr<DesktopSyncChatCarryOwner*>(_self);
	_active->run = std::make_shared<Run>(_host, _active->live, [self, generation](DesktopSyncCarryProgress value) {
		if (const auto owner = self.lock(); owner && *owner) { (*owner)->progress(generation, std::move(value)); }
	});
	_active->run->cleanup = _host.activated(DesktopSyncChatCarrySwitch::Cleanup);
	_active->run->draft = std::move(draft);
	_active->run->startMove();
	return true;
}

namespace {

// Worker-safe pieces shared by the controller ports: plain values, the operation's live flag and
// the host's thread hops. GUI entries are always called through `main`.
struct PortParts {
	DesktopSyncChatOrigin origin;
	QString login;
	QString window;
	std::shared_ptr<std::atomic<bool>> live;
	Fn<void(Fn<void()>)> worker;
	Fn<void(Fn<void()>)> main;
	Fn<void(Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)>)> resolve;
	Fn<void(PeerToken, Fn<void(std::optional<DesktopSyncChatPeer>)>)> inspect;
};

template <typename Port>
void FillPort(Port &port, const PortParts &parts) {
	port.origin = [origin = parts.origin] { return origin; };
	port.login = [login = parts.login] { return login; };
	port.window = [window = parts.window] { return window; };
	port.current = port.foreground = [live = parts.live] { return live->load(); };
	port.worker = parts.worker;
	port.resolve = [main = parts.main, resolve = parts.resolve](Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)> done) {
		main([=] { if (resolve) { resolve(done); } });
	};
	if constexpr (requires { port.inspect; }) {
		port.inspect = [main = parts.main, inspect = parts.inspect](PeerToken chat, Fn<void(std::optional<DesktopSyncChatPeer>)> done) {
			main([=] { if (inspect) { inspect(chat, done); } });
		};
	}
}

// Worker: the engine for a Follow/Fill or recovery controller, under the account's carry lease.
auto CarryFactory(const DesktopSyncChatCarryHost &host, QString directory, const DesktopSyncChatSetupSnapshot &opening) {
	return [main = host.main, quiesce = host.quiesce, lease = host.lease, directory = std::move(directory),
			scope = DesktopSyncChatCarryStoreScope(opening), origin = opening.scope.origin](
			std::shared_ptr<std::atomic<bool>> admitted, Fn<void(DesktopSyncCarryProgress)> progress) -> std::unique_ptr<DesktopSyncChatCarry> {
		if (quiesce) { main(quiesce); }
		auto taken = lease ? lease(origin.account) : nullptr;
		if (!taken) {
			if (progress) { progress({.reason = u"carry-writer-busy"_q, .storeStatus = SyncStoreStatus::LockBusy}); }
			return nullptr;
		}
		return std::make_unique<DesktopSyncChatCarry>(directory, scope, origin, std::move(admitted), std::move(taken), std::move(progress));
	};
}

auto SendPort(Fn<void(Fn<void()>)> main, Fn<bool(DesktopSyncChatCarrySwitch)> activated,
		std::weak_ptr<DesktopSyncChatCarryTransportBox> weak, std::shared_ptr<std::atomic<bool>> live, Fn<void(QString)> held) {
	return [=](DesktopSyncCarryCommand command, Fn<void(DesktopSyncCarryReply)> done) {
		main([=] {
			if (!SendCarry(weak.lock(), main, activated, command, live, done) && live->load() && held) {
				held(command.cleanupPermit || command.cleanupRead ? u"cleanup-held"_q : u"carry-transport-held"_q);
			}
		});
	};
}

}

std::shared_ptr<DesktopSyncChatCarryTransportBox> DesktopSyncChatCarryOwner::transportFor(const DesktopSyncChatSetupSnapshot &opening,
		const std::shared_ptr<std::atomic<bool>> &live) {
	auto box = std::make_shared<DesktopSyncChatCarryTransportBox>();
	if (_host.transport) {
		box->transport = _host.transport(opening, controllerRenewal(_generation, live), [live, current = _host.current] {
			return live->load() && current && current();
		});
	}
	return box;
}

DesktopSyncChatCarryRenewal DesktopSyncChatCarryOwner::controllerRenewal(uint64_t generation,
		std::shared_ptr<std::atomic<bool>> live) {
	const auto owner = [self = std::weak_ptr<DesktopSyncChatCarryOwner*>(_self), generation]() -> DesktopSyncChatCarryOwner* {
		const auto strong = self.lock();
		const auto result = strong ? *strong : nullptr;
		return (result && result->_active && result->_active->generation == generation && result->_active->live->load()
			&& result->_host.current && result->_host.current()) ? result : nullptr;
	};
	return ControllerRenewal({
		.valid = [owner] { return owner() != nullptr; },
		.engine = [owner](Fn<void(DesktopSyncChatCarry *)> work) {
			const auto current = owner();
			const auto active = current ? current->_active.get() : nullptr;
			if (active && active->consent) { active->consent->withEngine(std::move(work)); }
			else if (active && active->recovery) { active->recovery->withEngine(std::move(work)); }
			else { work(nullptr); }
		},
		.main = _host.main,
		.live = std::move(live),
	});
}

bool DesktopSyncChatCarryOwner::follow(DesktopSyncChatConsentSelection selection) {
	const auto window = selection.offer.window;
	if (window.isEmpty() || !admit(DesktopSyncChatCarryKind::Follow, window, DesktopSyncChatCarrySwitch::FollowFill)) { return false; }
	const auto generation = _generation;
	const auto live = _active->live;
	const auto scope = _host.scope ? _host.scope() : std::nullopt;
	const auto directory = _host.directory ? _host.directory(selection.opening.scope) : std::nullopt;
	const auto box = _active->transport = transportFor(selection.opening, live);
	if (!scope || !directory || !box->transport.send) {
		progress(generation, {.reason = u"carry-transport-held"_q, .terminal = true});
		return true;
	}
	auto port = DesktopSyncChatConsentPort();
	FillPort(port, {scope->origin, scope->loginBinding, window, live, _host.worker, _host.main, _host.resolve, _host.inspect});
	port.begin = [](const DesktopSyncChatConsentSelection &selection) { return DesktopSyncChatFollowStart(selection); };
	port.carry = CarryFactory(_host, *directory, selection.opening);
	port.send = SendPort(_host.main, _host.activated, box, live, held(generation));
	port.pause = [cacheFile = DesktopSyncChatCarryStoreScope(selection.opening).cacheFile](
			const DesktopSyncChatConsentSelection &selection, std::shared_ptr<std::atomic<bool>> admitted, Fn<void(bool)> done) {
		done(WriteDesktopSyncChatPause(cacheFile, selection.opening.scope.origin.account,
			selection.opening.scope.loginBinding, selection.opening.cacheImage, true, admitted));
	};
	port.status = reporter(generation);
	port.cancel = [main = _host.main, weak = std::weak_ptr<DesktopSyncChatCarryTransportBox>(box)] {
		main([weak] { if (const auto box = weak.lock()) { box->cancel(); } });
	};
	port.cleanup = _host.activated(DesktopSyncChatCarrySwitch::Cleanup);
	_active->consent = std::make_unique<DesktopSyncChatConsentFlow>(std::move(selection), std::move(port));
	_active->consent->start();
	return true;
}

bool DesktopSyncChatCarryOwner::recover(DesktopSyncChatRecoverySelection selection) {
	const auto window = selection.window;
	if (window.isEmpty() || !admit(DesktopSyncChatCarryKind::Recovery, window, DesktopSyncChatCarrySwitch::Recovery)) { return false; }
	const auto generation = _generation;
	const auto live = _active->live;
	const auto scope = _host.scope ? _host.scope() : std::nullopt;
	const auto directory = _host.directory ? _host.directory(selection.opening.scope) : std::nullopt;
	const auto box = _active->transport = transportFor(selection.opening, live);
	if (!scope || !directory || !box->transport.send) {
		progress(generation, {.reason = u"carry-transport-held"_q, .terminal = true});
		return true;
	}
	auto port = DesktopSyncChatRecoveryPort();
	FillPort(port, {scope->origin, scope->loginBinding, window, live, _host.worker, _host.main, _host.resolve, _host.inspect});
	port.activated = [activated = _host.activated] { return activated && activated(DesktopSyncChatCarrySwitch::Recovery); };
	port.provision = [main = _host.main, provision = _host.provision](DesktopSyncChatMoveDraft draft, DesktopSyncChatProvisionDone done) {
		main([=] { if (provision) { provision(draft, false, done); } });
	};
	port.resumeProvision = [main = _host.main, provision = _host.provision, opening = selection.opening](DesktopSyncChatProvisionDone done) {
		main([=] {
			if (provision) { provision(DesktopSyncChatMoveDraft{opening, {DesktopSyncChatSetupChoice::Create, {}, true, true}, false, false}, true, done); }
		});
	};
	port.carry = CarryFactory(_host, *directory, selection.opening);
	port.send = SendPort(_host.main, _host.activated, box, live, held(generation));
	port.status = reporter(generation);
	port.cancel = [main = _host.main, weak = std::weak_ptr<DesktopSyncChatCarryTransportBox>(box)] {
		main([weak] { if (const auto box = weak.lock()) { box->cancel(); } });
	};
	_active->recovery = std::make_unique<DesktopSyncChatRecovery>(std::move(selection), std::move(port));
	_active->recovery->start();
	return true;
}

bool DesktopSyncChatCarryOwner::removeMark(DesktopSyncChatStaleMark row, QString window, DesktopSyncChatSetupSnapshot opening) {
	if (window.isEmpty() || row.origin != opening.scope.origin || row.login != opening.scope.loginBinding
		|| !admit(DesktopSyncChatCarryKind::Mark, window, DesktopSyncChatCarrySwitch::MarkRemoval)) { return false; }
	const auto generation = _generation;
	const auto self = std::weak_ptr<DesktopSyncChatCarryOwner*>(_self);
	const auto live = _active->live;
	const auto scope = _host.scope ? _host.scope() : std::nullopt;
	const auto directory = _host.directory ? _host.directory(opening.scope) : std::nullopt;
	const auto native = _active->mark = std::make_shared<DesktopSyncChatMarkPort>(_host.mark
		? _host.mark(opening, [live, current = _host.current] { return live->load() && current && current(); })
		: DesktopSyncChatMarkPort());
	if (!scope || !directory || !native->exact || !native->send || !native->apply) {
		marked(generation, {u"mark-transport-held"_q, true});
		return true;
	}
	const auto weak = std::weak_ptr<DesktopSyncChatMarkPort>(native);
	auto port = DesktopSyncChatMarkPort();
	FillPort(port, {scope->origin, scope->loginBinding, window, live, _host.worker, _host.main, _host.resolve, {}});
	port.exact = [main = _host.main, weak](const DesktopSyncChatStaleMark &row, Fn<void(std::optional<DesktopSyncChatMarkExact>)> done) {
		main([=] { if (const auto native = weak.lock(); native && native->exact) { native->exact(row, done); } });
	};
	port.send = [main = _host.main, weak](DesktopSyncChatMarkPermit permit, DesktopSyncChatMarkRenewal renew,
			Fn<void(std::optional<MTPmessages_AffectedMessages>)> done) {
		main([=] { if (const auto native = weak.lock(); native && native->send) { native->send(permit, renew, done); } });
	};
	port.apply = [main = _host.main, weak](const DesktopSyncChatIo &route, const MTPmessages_AffectedMessages &affected, Fn<void(bool)> done) {
		main([=] { if (const auto native = weak.lock(); native && native->apply) { native->apply(route, affected, done); } });
	};
	port.cancel = [main = _host.main, weak] {
		main([weak] { if (const auto native = weak.lock(); native && native->cancel) { auto cancel = std::exchange(native->cancel, {}); cancel(); } });
	};
	port.status = OnMain(_host.main, [self, generation](DesktopSyncChatMarkProgress value) {
		if (const auto owner = self.lock(); owner && *owner) { (*owner)->marked(generation, std::move(value)); }
	});
	if (_host.quiesce) { _host.quiesce(); }
	_active->marks = std::make_unique<DesktopSyncChatMarkActions>(std::move(row), window, *directory,
		DesktopSyncChatCarryStoreScope(opening), std::move(port));
	_active->marks->start();
	return true;
}

Fn<void(DesktopSyncCarryProgress)> DesktopSyncChatCarryOwner::reporter(uint64_t generation) {
	return OnMain(_host.main, [self = std::weak_ptr<DesktopSyncChatCarryOwner*>(_self), generation](DesktopSyncCarryProgress value) {
		if (const auto owner = self.lock(); owner && *owner) { (*owner)->progress(generation, std::move(value)); }
	});
}

Fn<void(QString)> DesktopSyncChatCarryOwner::held(uint64_t generation) {
	return [self = std::weak_ptr<DesktopSyncChatCarryOwner*>(_self), generation](QString reason) {
		if (const auto owner = self.lock(); owner && *owner) { (*owner)->progress(generation, {.reason = std::move(reason)}); }
	};
}

void DesktopSyncChatCarryOwner::retry() {
	if (!_active || _state.finished || !_host.current || !_host.current()) { return; }
	if (_active->run) { _active->run->retry(); }
	else if (_active->consent) { _active->consent->retry(); }
	else if (_active->recovery) { _active->recovery->retry(); }
}

void DesktopSyncChatCarryOwner::skipCleanup(bool confirmed) {
	if (_active && _active->run && !_state.finished && confirmed && _host.current && _host.current()) {
		_active->run->withEngine([](DesktopSyncChatCarry &engine) { engine.skipCleanup(true); });
	}
}

void DesktopSyncChatCarryOwner::discardInterrupted(bool confirmed) {
	if (_active && _active->run && !_state.finished && confirmed && _host.current && _host.current()) {
		_active->run->withEngine([](DesktopSyncChatCarry &engine) { engine.discardInterrupted(true); });
	}
}

void DesktopSyncChatCarryOwner::close(QString reason) {
	if (!_active) { return; }
	const auto kind = _active->kind;
	const auto running = !_state.finished;
	_active->stop();
	_active.reset();
	if (!running) { return; }
	_state.finished = true; _state.running = false;
	_state.reason = std::move(reason);
	publish();
	if (_host.settled) { _host.settled(kind); }
}

void DesktopSyncChatCarryOwner::closeWindow(const QString &window, QString reason) {
	if (_active && !window.isEmpty() && _active->window == window) { close(std::move(reason)); }
}

bool DesktopSyncChatCarryOwner::busy() const { return _active && !_state.finished; }
const DesktopSyncChatCarryState &DesktopSyncChatCarryOwner::state() const { return _state; }
std::optional<DesktopSyncCarryProgress> DesktopSyncChatCarryOwner::latest() const { return _state.carry; }

void DesktopSyncChatCarryOwner::progress(uint64_t generation, DesktopSyncCarryProgress value) {
	if (!_active || _active->generation != generation || _state.finished) { return; }
	if (!value.reason.isEmpty()) { _state.reason = value.reason; }
	if (value.reason == u"move-provisioned"_q) { _state.provisioned = true; }
	_state.running = value.command.has_value()
		|| value.reason == u"move-provisioned"_q || value.reason == u"new-readers"_q;
	const auto ended = value.terminal || value.absent
		|| (_active->kind == DesktopSyncChatCarryKind::Follow
			&& (value.reason == u"sync-paused-here"_q || value.reason == u"pause-held"_q));
	if (value.journal || value.command || ended) { _state.carry = std::move(value); }
	else if (_state.carry) { _state.carry->command.reset(); _state.carry->reason = _state.reason; }
	publish();
	if (ended) { finish(generation); }
}

void DesktopSyncChatCarryOwner::marked(uint64_t generation, DesktopSyncChatMarkProgress value) {
	if (!_active || _active->generation != generation || _state.finished) { return; }
	_state.reason = value.reason;
	_state.removed = value.absent;
	_state.running = !value.terminal;
	publish();
	if (value.terminal) { finish(generation); }
}

void DesktopSyncChatCarryOwner::finish(uint64_t generation) {
	if (!_active || _active->generation != generation) { return; }
	_state.finished = true; _state.running = false;
	publish();
	const auto self = std::weak_ptr<DesktopSyncChatCarryOwner*>(_self);
	const auto kind = _active->kind;
	_host.main([self, generation, kind] {
		const auto owner = self.lock();
		if (!owner || !*owner) { return; }
		const auto that = *owner;
		if (that->_active && that->_active->generation == generation) {
			that->_active->stop();
			that->_active.reset();
			if (that->_host.settled) { that->_host.settled(kind); }
		}
	});
}

void DesktopSyncChatCarryOwner::publish() {
	if (_host.changed) { _host.changed(_state); }
}

}
