#include "purple/purple_sync_chat_recovery.h"

#include "base/random.h"
#include <QtCore/QUuid>

namespace Purple {
namespace {

uint64_t RequestId() {
	auto value = uint64_t(0);
	while (!value) { base::RandomFill(&value, sizeof(value)); value &= uint64_t(std::numeric_limits<int64_t>::max()); }
	return value;
}

bool RepairReaders(const DesktopSyncChatCacheRecord &cache, const DesktopSyncChatPeer &peer) {
	const auto eligible = DesktopSyncChatEligibility(peer);
	const auto roles = peer.kind == SyncChatKind::BasicGroup || peer.kind == SyncChatKind::Supergroup || peer.kind == SyncChatKind::Broadcast;
	return eligible && *eligible && !eligible->readers.populationUnknown && peer.facts
		&& !cache.readers.populationUnknown && cache.readerKind == peer.kind
		&& (!roles || (cache.readerCreator.has_value() && cache.readerAdmin.has_value()))
		&& (cache.readerCreator != true || peer.creator == true)
		&& (cache.readerAdmin != true || peer.facts->admin)
		&& (!cache.readerMembers || peer.readerMembers);
}

bool Matches(const DesktopSyncChatRecoverySelection &selection, const DesktopSyncChatSetupSnapshot &fresh) {
	return DesktopSyncChatRecoveryHeld(fresh).isEmpty()
		&& DesktopSyncChatMoveFingerprint(fresh) == selection.fingerprint
		&& selection.opening.stateImage == fresh.stateImage
		&& selection.opening.cacheImage == fresh.cacheImage
		&& selection.opening.input.cacheRead == fresh.input.cacheRead;
}

}

bool DesktopSyncChatRecoveryActivated() { return false; }

QString DesktopSyncChatRecoveryHeld(const DesktopSyncChatSetupSnapshot &snapshot) {
	if (!snapshot.status.complete) { return u"recovery-searching"_q; }
	if (snapshot.cache && snapshot.cache->paused) { return u"sync-paused-here"_q; }
	const auto action = ResolveSyncChat(snapshot.input).action;
	if ((action != SyncChatAction::Searching && action != SyncChatAction::Lost)
		|| DesktopSyncChatMoveFingerprint(snapshot).isEmpty() || !snapshot.cache
		|| snapshot.cache->mark != snapshot.status.resolution.cache
		|| snapshot.cache->mark != snapshot.scope.mark
		|| snapshot.cache->mark.chat != snapshot.scope.settingsChat) { return u"recovery-opening-changed"_q; }
	return {};
}

QString DesktopSyncChatRepairHeld(const DesktopSyncChatSetupSnapshot &snapshot, const DesktopSyncChatPeer &peer) {
	const auto held = DesktopSyncChatRecoveryHeld(snapshot);
	if (!held.isEmpty()) { return held; }
	if (snapshot.input.cacheRead != SyncChatDirectRead::MarkGone) {
		return snapshot.input.cacheRead == SyncChatDirectRead::ChatGone ? u"recovery-chat-gone"_q : u"recovery-mark-unproven"_q;
	}
	if (peer.origin != snapshot.scope.origin || peer.chat != snapshot.cache->mark.chat) { return u"recovery-wrong-chat"_q; }
	if (!snapshot.cache->readerConsent || std::find(snapshot.cache->disclosedStreams.begin(),
		snapshot.cache->disclosedStreams.end(), u"config"_q) == snapshot.cache->disclosedStreams.end()) { return u"recovery-reader-consent"_q; }
	if (!RepairReaders(*snapshot.cache, peer)) { return u"recovery-readers-held"_q; }
	const auto state = ParseSyncLocalState(snapshot.stateImage);
	if (!state || SerializeSyncLocalState(state.state).canonical != snapshot.stateImage || state.state.config.pendingSeq || state.state.library.pendingSeq
		|| !state.state.configData.pending.isEmpty()) { return u"recovery-pending-held"_q; }
	return {};
}

std::optional<DesktopSyncChatPeer> DesktopSyncChatRepairBaseline(
		const DesktopSyncChatSetupSnapshot &snapshot, const DesktopSyncChatPeer &peer) {
	if (!DesktopSyncChatRepairHeld(snapshot, peer).isEmpty()) { return {}; }
	auto baseline = peer;
	const auto &cache = *snapshot.cache;
	baseline.readerMembers = cache.readerMembers;
	baseline.creator = cache.readerCreator;
	baseline.facts->creator = cache.readerCreator.value_or(false);
	baseline.facts->admin = cache.readerAdmin.value_or(false);
	baseline.facts->otherMembers = cache.readers.otherMembers;
	baseline.facts->otherAdmins = cache.readers.otherAdmins;
	const auto eligibility = DesktopSyncChatEligibility(baseline);
	return eligibility && *eligibility && eligibility->readers == cache.readers ? std::optional(baseline) : std::nullopt;
}

std::vector<DesktopSyncChatRecoveryChoice> DesktopSyncChatRecoveryChoices(const DesktopSyncChatSetupSnapshot &snapshot) {
	if (!DesktopSyncChatRecoveryActivated() || !DesktopSyncChatRecoveryHeld(snapshot).isEmpty()) { return {}; }
	auto choices = std::vector{DesktopSyncChatRecoveryChoice::Create, DesktopSyncChatRecoveryChoice::Choose};
	if (snapshot.input.cacheRead == SyncChatDirectRead::MarkGone && snapshot.cache->mark.markId > 0) {
		choices.push_back(DesktopSyncChatRecoveryChoice::RepairMark);
	}
	return choices;
}

auto SelectDesktopSyncChatRecovery(
		const DesktopSyncChatSetupSnapshot &snapshot, DesktopSyncChatRecoveryChoice choice,
		std::optional<DesktopSyncChatPeer> peer, QString window, bool disclosed)
-> std::optional<DesktopSyncChatRecoverySelection> {
	if (!DesktopSyncChatRecoveryActivated() || !disclosed || window.isEmpty()
		|| !DesktopSyncChatRecoveryHeld(snapshot).isEmpty()) { return {}; }
	if (choice == DesktopSyncChatRecoveryChoice::RepairMark
		? !peer || !DesktopSyncChatRepairHeld(snapshot, *peer).isEmpty()
		: choice == DesktopSyncChatRecoveryChoice::Create ? peer.has_value()
		: choice != DesktopSyncChatRecoveryChoice::Choose || !peer
			|| DesktopSyncChatMoveEligibility(snapshot, *peer) != DesktopSyncChatSetupRefusal::None) { return {}; }
	return DesktopSyncChatRecoverySelection{snapshot, choice, std::move(peer), std::move(window), DesktopSyncChatMoveFingerprint(snapshot)};
}

class DesktopSyncChatRecovery::State final : public std::enable_shared_from_this<State> {
public:
	State(DesktopSyncChatRecoverySelection selection, DesktopSyncChatRecoveryPort port)
	: selection(std::move(selection)), port(std::move(port)) {
	}
	bool live() const {
		return !closed.load() && admitted->load() && port.activated && port.activated()
			&& port.current && port.current() && port.foreground && port.foreground()
			&& port.origin && port.origin() == selection.opening.scope.origin
			&& port.login && port.login() == selection.opening.scope.loginBinding
			&& port.window && port.window() == selection.window && !closed.load();
	}
	void close() {
		if (closed.exchange(true)) { return; }
		admitted->store(false);
		const auto self = shared_from_this();
		if (port.worker) { port.worker([self] {
			try { if (self->engine) { self->engine->cancel(); } } catch (...) {}
			self->engine.reset();
			try { if (self->port.cancel) { self->port.cancel(); } } catch (...) {}
		}); }
	}
	void post(Fn<void(State &)> task) {
		if (!port.worker) { close(); return; }
		const auto weak = weak_from_this();
		port.worker([weak, task = std::move(task)]() mutable {
			const auto self = weak.lock();
			if (!self || !SyncStoreWorkerThread()) { return; }
			try { if (self->live()) { task(*self); } else { self->close(); } }
			catch (...) { self->close(); }
		});
	}
	void report(QString reason) {
		if (live() && port.status) { port.status({.reason = std::move(reason)}); }
	}
	void start() {
		post([](State &self) { if (!self.started) { self.started = true; self.resolve(false); } });
	}
	void resolve(bool final) {
		if (!live() || !port.resolve || busy || engine) { return; }
		if (!Matches(selection, selection.opening) || selection.window.isEmpty()) { report(u"recovery-opening-changed"_q); return; }
		busy = true;
		const auto run = ++ticket;
		const auto weak = weak_from_this();
		port.resolve([weak, run, final](std::optional<DesktopSyncChatSetupSnapshot> fresh) {
			if (const auto self = weak.lock()) { self->post([run, final, fresh = std::move(fresh)](State &state) {
				if (state.ticket != run) { return; }
				++state.ticket; state.busy = false;
				if (!fresh || !Matches(state.selection, *fresh)) { state.report(u"recovery-opening-changed"_q); return; }
				if (state.selection.choice == DesktopSyncChatRecoveryChoice::Create && !state.provisioned) {
					state.provision(*fresh, state.provisionStarted);
					return;
				}
				state.inspect(final);
			}); }
		});
	}
	void provision(const DesktopSyncChatSetupSnapshot &fresh, bool resume) {
		if ((!resume && !port.provision) || (resume && !port.resumeProvision) || !live()) { report(u"recovery-provision-held"_q); return; }
		provisionStarted = true; busy = true;
		const auto run = ++ticket;
		const auto weak = weak_from_this();
		auto draft = DesktopSyncChatMoveDraft{fresh, {DesktopSyncChatSetupChoice::Create, {}, true, true}, false, false};
		auto done = [weak, run](std::optional<DesktopSyncChatCreateJournal> journal, std::optional<DesktopSyncChatPeer> peer) {
			if (const auto self = weak.lock()) { self->post([run, journal = std::move(journal), peer = std::move(peer)](State &state) {
				if (state.ticket != run) { return; }
				++state.ticket; state.busy = false;
				if (!journal || !peer || !journal->created || journal->pending || journal->cancelled || journal->adopted
					|| journal->step != DesktopSyncChatCreateStep::Done || !journal->chat || journal->chat != peer->chat
					|| journal->account != state.selection.opening.scope.origin.account
					|| journal->authority != DesktopSyncChatSetupAuthority(state.selection.opening.scope)
					|| journal->provisionSource != state.selection.fingerprint || journal->markId || journal->markDate
					|| !SerializeDesktopSyncChatCreateJournal(*journal)
					|| DesktopSyncChatMoveEligibility(state.selection.opening, *peer) != DesktopSyncChatSetupRefusal::None) {
					state.report(u"recovery-provision-held"_q); return;
				}
				state.provisioned = *peer;
				state.resolve(false);
			}); }
		};
		if (resume) { port.resumeProvision(std::move(done)); } else { port.provision(std::move(draft), std::move(done)); }
	}
	void inspect(bool final) {
		if (!live() || !port.inspect) { report(u"recovery-readers-held"_q); return; }
		const auto peer = selection.choice == DesktopSyncChatRecoveryChoice::Create ? provisioned : selection.peer;
		const auto chat = selection.choice == DesktopSyncChatRecoveryChoice::RepairMark
			? selection.opening.scope.settingsChat : peer ? peer->chat : PeerToken{};
		if (!peer) { report(u"recovery-choice-held"_q); return; }
		busy = true;
		const auto run = ++ticket;
		const auto weak = weak_from_this();
		port.inspect(chat, [weak, run, final, peer](std::optional<DesktopSyncChatPeer> actual) {
			if (const auto self = weak.lock()) { self->post([run, final, peer, actual = std::move(actual)](State &state) {
				if (state.ticket != run) { return; }
				++state.ticket; state.busy = false;
				const auto repair = state.selection.choice == DesktopSyncChatRecoveryChoice::RepairMark;
				if (!actual || (repair ? !DesktopSyncChatRepairHeld(state.selection.opening, *actual).isEmpty()
					: !SameDesktopSyncChatDisclosure(*peer, *actual)
						|| DesktopSyncChatMoveEligibility(state.selection.opening, *actual) != DesktopSyncChatSetupRefusal::None)) {
					state.report(u"recovery-readers-held"_q); return;
				}
				if (repair && NewDesktopSyncChatReaders(*state.selection.opening.cache, *actual)) { state.report(u"new-readers"_q); }
				if (!state.live()) { state.close(); return; }
				state.target = *actual;
				if (!final) { state.resolve(true); } else { state.begin(); }
			}); }
		});
	}
	void begin() {
		if (!live() || !target || !port.carry) { report(u"carry-unavailable"_q); return; }
		const auto &opening = selection.opening;
		const auto repair = selection.choice == DesktopSyncChatRecoveryChoice::RepairMark;
		const auto baseline = repair ? DesktopSyncChatRepairBaseline(opening, *target) : target;
		const auto parsed = ParseSyncLocalState(opening.stateImage);
		if (!baseline || !parsed) { report(u"recovery-readers-held"_q); return; }
		auto input = SyncCarryJournalStart();
		input.operationId = QUuid::createUuid().toString(QUuid::Id128);
		input.account = opening.scope.origin.account;
		input.source = opening.scope.settingsChat; input.target = target->chat;
		input.mode = repair ? SyncCarryMode::RepairMark : SyncCarryMode::Switch;
		input.state = parsed.state; input.preferenceToken = opening.scope.binding;
		input.resolution = ResolveSyncChat(opening.input);
		input.oldRead = opening.input.cacheRead;
		input.sourceAccess = repair ? SyncCarrySourceAccess::Readable : SyncCarrySourceAccess::Unknown;
		input.targetEligibility = *DesktopSyncChatEligibility(*baseline);
		input.readerConsentAccepted = true; input.operationConfirmed = true;
		input.loginAuthority = DesktopSyncCarryLogin(opening.scope.loginBinding);
		input.markText = DesktopSyncChatMarkText(); input.markRequestId = QString::number(RequestId()); input.ownRequestId = QString::number(RequestId());
		const auto weak = weak_from_this();
		engine = port.carry(admitted, [weak](DesktopSyncCarryProgress progress) {
			if (const auto self = weak.lock(); self && self->live()) {
				const auto command = progress.command;
				self->openingRejected = !progress.journal && !command
					&& progress.storeStatus != SyncStoreStatus::Ready && progress.storeStatus != SyncStoreStatus::LockBusy;
				try { if (self->port.status) { self->port.status(std::move(progress)); } }
				catch (...) { self->close(); return; }
				if (self->live() && command) { self->dispatch(*command); }
			}
		});
		if (!live()) { close(); return; }
		if (engine) { engine->open(std::move(input), baseline, u"config"_q, DesktopSyncCarryOpening{opening.stateImage, opening.cacheImage}); }
	}
	void dispatch(DesktopSyncCarryCommand command) {
		if (!live() || !port.send) { report(u"carry-transport-held"_q); return; }
		const auto weak = weak_from_this();
		port.send(command, [weak, command](DesktopSyncCarryReply reply) {
			if (const auto self = weak.lock()) { self->post([command, reply = std::move(reply)](State &state) mutable {
				if (!state.engine || reply.command.ticket != command.ticket || reply.command.run != command.run
					|| reply.command.operation != command.operation || reply.command.revision != command.revision) { return; }
				if ((command.action == SyncCarryJournalAction::ResolvePending || command.action == SyncCarryJournalAction::Plan)
					&& reply.access == SyncCarrySourceAccess::ChatGone && (!reply.fresh
					|| reply.fresh->resolutionInput.cacheRead != SyncChatDirectRead::ChatGone
					|| reply.fresh->resolutionInput.cache != state.selection.opening.input.cache)) {
					reply.outcome = SyncCarryOutcome::Unknown;
				}
				state.engine->complete(std::move(reply));
			}); }
		});
	}
	DesktopSyncChatRecoverySelection selection;
	DesktopSyncChatRecoveryPort port;
	std::unique_ptr<DesktopSyncChatCarry> engine;
	std::optional<DesktopSyncChatPeer> provisioned;
	std::optional<DesktopSyncChatPeer> target;
	std::shared_ptr<std::atomic<bool>> admitted = std::make_shared<std::atomic<bool>>(true);
	std::atomic<bool> closed = false;
	uint64_t ticket = 0;
	bool started = false;
	bool busy = false;
	bool provisionStarted = false;
	bool openingRejected = false;

};

DesktopSyncChatRecovery::DesktopSyncChatRecovery(DesktopSyncChatRecoverySelection selection, DesktopSyncChatRecoveryPort port)
: _state(std::make_shared<State>(std::move(selection), std::move(port))) {
}
DesktopSyncChatRecovery::~DesktopSyncChatRecovery() { close(); }
void DesktopSyncChatRecovery::start() { _state->start(); }
void DesktopSyncChatRecovery::retry() {
	_state->post([](State &state) {
		if (state.engine && !state.openingRejected) { state.engine->resume(); }
		else { state.engine.reset(); state.openingRejected = false; state.resolve(false); }
	});
}
void DesktopSyncChatRecovery::close() { if (_state) { _state->close(); } }

}
