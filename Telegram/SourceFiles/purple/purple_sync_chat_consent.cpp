#include "purple/purple_sync_chat_consent.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>

namespace Purple {
namespace {

bool Stream(const QString &stream) {
	return stream == u"config"_q || stream == u"library"_q;
}

std::optional<PeerToken> Source(const SyncChatResolution &resolution) {
	return resolution.action == SyncChatAction::Follow && resolution.cache
		? std::optional(resolution.cache->chat) : resolution.source;
}

}

bool DesktopSyncChatFollowFillActivated() { return false; }

QString DesktopSyncChatConsentFingerprint(const DesktopSyncChatSetupSnapshot &snapshot) {
	const auto &scope = snapshot.scope;
	const auto actual = ResolveSyncChat(snapshot.input);
	if (!snapshot.status.complete || !ValidDesktopSyncChatOrigin(scope.origin)
		|| scope.root.isEmpty() || scope.loginBinding.size() != 64 || scope.binding.isEmpty()
		|| scope.carry || (snapshot.cache && (snapshot.cache->paused || snapshot.cache->carry
			|| snapshot.cache->account != scope.origin.account || snapshot.cache->loginBinding != scope.loginBinding))
		|| (actual.action != SyncChatAction::Follow && actual.action != SyncChatAction::Fill)
		|| actual.action != snapshot.status.resolution.action || !actual.winner || !Source(actual)
		|| actual.winner != snapshot.status.resolution.winner || actual.cache != snapshot.status.resolution.cache
		|| actual.source != snapshot.status.resolution.source || actual.readers.populationUnknown
		|| snapshot.input.selfUserId != scope.origin.account.userId) { return {}; }
	if (scope.settingsBound) {
		const auto state = ParseSyncLocalState(snapshot.stateImage);
		if (!state || state.state.chat != scope.settingsChat || state.state.space != scope.space
			|| state.state.bindingToken.toLatin1() != scope.binding) { return {}; }
	} else if (!snapshot.stateImage.isEmpty()) { return {}; }
	const auto cache = snapshot.cache ? SerializeDesktopSyncChatCache(*snapshot.cache) : std::optional(QByteArray());
	if (!cache) { return {}; }
	const auto mark = [](const std::optional<SyncChatCache> &value) {
		return value ? QJsonObject{{u"chat"_q, PeerTokenText(value->chat).value_or(QString())},
			{u"id"_q, value->markId}, {u"date"_q, QString::number(value->markDate)}} : QJsonObject();
	};
	const auto value = QJsonObject{{u"account"_q, AccountKeyText(scope.origin.account.environment, scope.origin.account.userId).value_or(QString())},
		{u"login"_q, scope.origin.login}, {u"authority"_q, scope.loginBinding}, {u"root"_q, scope.root},
		{u"binding"_q, QString::fromLatin1(scope.binding.toHex())}, {u"owner"_q, scope.owner ? AccountKeyText(scope.owner->environment, scope.owner->userId).value_or(QString()) : QString()},
		{u"mode"_q, int(scope.mode)}, {u"space"_q, scope.space}, {u"bound"_q, scope.settingsBound},
		{u"state_chat"_q, PeerTokenText(scope.settingsChat).value_or(QString())},
		{u"action"_q, int(actual.action)}, {u"source"_q, PeerTokenText(*Source(actual)).value_or(QString())},
		{u"winner"_q, mark(actual.winner)}, {u"old"_q, mark(actual.cache)}, {u"scope_mark"_q, mark(scope.mark)},
		{u"cache"_q, QString::fromLatin1(cache->toBase64())}, {u"cache_image"_q, QString::fromLatin1(snapshot.cacheImage.toBase64())},
		{u"state"_q, QString::fromLatin1(snapshot.stateImage.toBase64())}};
	return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

bool DesktopSyncChatConsentMatches(const DesktopSyncChatConsentSelection &selection,
		const DesktopSyncChatSetupSnapshot &fresh, const DesktopSyncChatPeer &peer) {
	const auto &offer = selection.offer;
	const auto eligible = DesktopSyncChatEligibility(peer);
	return Stream(offer.stream) && !offer.window.isEmpty() && offer.ticket
		&& !offer.fingerprint.isEmpty() && DesktopSyncChatConsentFingerprint(fresh) == offer.fingerprint
		&& peer.origin == fresh.scope.origin && peer.chat == offer.winner.chat && eligible && *eligible
		&& !eligible->readers.populationUnknown && SameDesktopSyncChatDisclosure(offer.peer, peer)
		&& offer.peer.facts && peer.facts && offer.peer.facts->admin == peer.facts->admin;
}

DesktopSyncChatConsentAction::DesktopSyncChatConsentAction(DesktopSyncChatSetupSnapshot opening,
		QString window, Fn<bool()> current, Fn<bool()> foreground)
: _opening(std::move(opening)), _window(std::move(window)), _current(std::move(current)), _foreground(std::move(foreground)) {
}

bool DesktopSyncChatConsentAction::current() const {
	return !_closed && !_window.isEmpty() && _current && _current() && !_closed && _foreground && _foreground();
}

std::optional<DesktopSyncChatFollowOffer> DesktopSyncChatConsentAction::offer(
		const DesktopSyncChatPeer &peer, QString stream) {
	if (!current() || !Stream(stream)) { return {}; }
	const auto fingerprint = DesktopSyncChatConsentFingerprint(_opening);
	const auto resolution = ResolveSyncChat(_opening.input);
	const auto eligible = DesktopSyncChatEligibility(peer);
	if (fingerprint.isEmpty() || !resolution.winner || peer.chat != resolution.winner->chat
		|| peer.origin != _opening.scope.origin || !eligible || !*eligible || eligible->readers.populationUnknown) { return {}; }
	_offer = DesktopSyncChatFollowOffer{++_ticket, _window, fingerprint, std::move(stream), resolution.action,
		*Source(resolution), *resolution.winner, peer, eligible->readers.warningRequired, resolution.action == SyncChatAction::Fill};
	return _offer;
}

std::optional<DesktopSyncChatConsentSelection> DesktopSyncChatConsentAction::accept(
		const DesktopSyncChatFollowOffer &offer, const DesktopSyncChatSetupSnapshot &fresh,
		const DesktopSyncChatPeer &peer, DesktopSyncChatConsentChoice choice, bool disclosed) {
	if (!current() || !_offer || _offer->ticket != offer.ticket || offer.window != _window
		|| offer.fingerprint != _offer->fingerprint || offer.stream != _offer->stream
		|| (choice == DesktopSyncChatConsentChoice::Proceed && (_offer->askReaders || _offer->firstPost) && !disclosed)) { return {}; }
	auto selection = DesktopSyncChatConsentSelection{_opening, *_offer, choice};
	if (!DesktopSyncChatConsentMatches(selection, fresh, peer) || !current()) { return {}; }
	_offer.reset();
	return selection;
}

void DesktopSyncChatConsentAction::cancel() { _closed = true; _offer.reset(); ++_ticket; }

class DesktopSyncChatConsentFlow::State final : public std::enable_shared_from_this<State> {
public:
	State(DesktopSyncChatConsentSelection selection, DesktopSyncChatConsentPort port)
	: selection(std::move(selection)), port(std::move(port)) {
	}
	bool live() const {
		return !closed.load() && admitted->load() && port.current && port.current() && !closed.load()
			&& port.foreground && port.foreground() && port.origin && port.origin() == selection.opening.scope.origin
			&& port.login && port.login() == selection.opening.scope.loginBinding
			&& port.window && port.window() == selection.offer.window;
	}
	void post(Fn<void(State &)> work) {
		const auto weak = weak_from_this();
		port.worker([weak, work = std::move(work)]() mutable {
			const auto self = weak.lock();
			if (!self || !SyncStoreWorkerThread()) { return; }
			try { if (self->live()) { work(*self); } else { self->close(); } }
			catch (...) { self->close(); }
		});
	}
	void report(QString reason) {
		if (!live() || !port.status) { return; }
		try { port.status({.reason = std::move(reason)}); }
		catch (...) { close(); }
	}
	void close() {
		if (closed.exchange(true)) { return; }
		admitted->store(false);
		const auto self = shared_from_this();
		port.worker([self] {
			try { if (self->engine) { self->engine->cancel(); } } catch (...) {}
			self->engine.reset();
			try { if (self->port.cancel) { self->port.cancel(); } } catch (...) {}
		});
	}
	void start() {
		post([](State &self) {
			if (self.started) { return; }
			self.started = true;
			self.resolve(false, {});
		});
	}
	void resolve(bool final, Fn<void()> ready) {
		if (!live() || !port.resolve) { report(u"follow-resolution-held"_q); return; }
		const auto run = ++ticket;
		const auto weak = weak_from_this();
		port.resolve([weak, run, final, ready = std::move(ready)](std::optional<DesktopSyncChatSetupSnapshot> fresh) mutable {
			if (const auto self = weak.lock()) { self->post([run, final, fresh = std::move(fresh), ready = std::move(ready)](State &state) mutable {
				if (state.ticket != run) { return; }
				++state.ticket;
				if (!fresh || DesktopSyncChatConsentFingerprint(*fresh) != state.selection.offer.fingerprint) { state.report(u"follow-opening-changed"_q); return; }
				state.inspect(final, std::move(ready));
			}); }
		});
	}
	void inspect(bool final, Fn<void()> ready) {
		if (!live() || !port.inspect) { report(u"reader-consent-changed"_q); return; }
		const auto run = ++ticket;
		const auto weak = weak_from_this();
		port.inspect(selection.offer.winner.chat, [weak, run, final, ready = std::move(ready)](std::optional<DesktopSyncChatPeer> peer) mutable {
			if (const auto self = weak.lock()) { self->post([run, final, peer = std::move(peer), ready = std::move(ready)](State &state) mutable {
				if (state.ticket != run) { return; }
				++state.ticket;
				if (!peer || !DesktopSyncChatConsentMatches(state.selection, state.selection.opening, *peer)) { state.report(u"reader-consent-changed"_q); return; }
				if (final) { if (state.live() && ready) { ready(); } return; }
				state.begin(*peer);
			}); }
		});
	}
	void begin(const DesktopSyncChatPeer &peer) {
		if (!live()) { return; }
		if (selection.choice == DesktopSyncChatConsentChoice::Pause) {
			if (!port.pause) { report(u"pause-held"_q); return; }
			const auto weak = weak_from_this();
			port.pause(selection, admitted, [weak](bool saved) {
				if (const auto self = weak.lock()) { self->post([saved](State &state) { state.report(saved ? u"sync-paused-here"_q : u"pause-held"_q); state.close(); }); }
			});
			return;
		}
		if (!port.begin || !port.carry) { report(u"carry-unavailable"_q); return; }
		auto input = port.begin(selection);
		if (!live() || !input || input->account != selection.opening.scope.origin.account
			|| input->preferenceToken != selection.opening.scope.binding
			|| input->loginAuthority != DesktopSyncCarryLogin(selection.opening.scope.loginBinding)
			|| input->source != selection.offer.source || input->target != selection.offer.winner.chat
			|| input->mode != (selection.offer.action == SyncChatAction::Follow ? SyncCarryMode::Follow : SyncCarryMode::Fill)
			|| input->resolution.winner != selection.offer.winner || input->resolution.action != selection.offer.action
			|| input->resolution.cache != selection.opening.status.resolution.cache
			|| input->stateAbsentConfirmed != selection.opening.stateImage.isEmpty()
			|| (input->state ? SerializeSyncLocalState(*input->state).canonical : QByteArray()) != selection.opening.stateImage) { report(u"carry-opening-held"_q); return; }
		input->operationId = QUuid::createUuid().toString(QUuid::Id128);
		input->readerConsentAccepted = true;
		input->operationConfirmed = true;
		const auto prepared = std::make_shared<SyncCarryJournalStart>(std::move(*input));
		const auto weak = weak_from_this();
		resolve(true, [weak, prepared, peer] {
			if (const auto self = weak.lock(); self && self->live()) { self->startCarry(*prepared, peer); }
		});
	}
	void startCarry(SyncCarryJournalStart input, const DesktopSyncChatPeer &peer) {
		const auto weak = weak_from_this();
		engine = port.carry(admitted, [weak](DesktopSyncCarryProgress progress) {
			if (const auto self = weak.lock(); self && self->live()) {
				const auto command = progress.command;
				try { if (self->port.status) { self->port.status(std::move(progress)); } } catch (...) { self->close(); return; }
				if (!self->live() || !command) { return; }
				self->dispatch(*command);
			}
		});
		if (!live()) { close(); return; }
		if (engine) { engine->open(std::move(input), peer, selection.offer.stream,
			DesktopSyncCarryOpening{selection.opening.stateImage, selection.opening.cacheImage}); }
	}
	void dispatch(DesktopSyncCarryCommand command) {
		if (!live()) { close(); return; }
		if (!port.send) { report(u"carry-transport-held"_q); return; }
		const auto weak = weak_from_this();
		port.send(command, [weak, command](DesktopSyncCarryReply reply) mutable {
			if (const auto self = weak.lock()) { self->post([command, reply = std::move(reply)](State &state) mutable {
				if (!state.engine || reply.command.ticket != command.ticket || reply.command.run != command.run
					|| reply.command.operation != command.operation || reply.command.revision != command.revision) { return; }
				state.engine->complete(std::move(reply));
			}); }
		});
	}
	DesktopSyncChatConsentSelection selection;
	DesktopSyncChatConsentPort port;
	std::shared_ptr<std::atomic<bool>> admitted = std::make_shared<std::atomic<bool>>(true);
	std::atomic<bool> closed = false;
	bool started = false;
	uint64_t ticket = 0;
	std::unique_ptr<DesktopSyncChatCarry> engine;
};

DesktopSyncChatConsentFlow::DesktopSyncChatConsentFlow(DesktopSyncChatConsentSelection selection, DesktopSyncChatConsentPort port)
: _state(std::make_shared<State>(std::move(selection), std::move(port))) {
}
DesktopSyncChatConsentFlow::~DesktopSyncChatConsentFlow() { close(); }
void DesktopSyncChatConsentFlow::start() { _state->start(); }
void DesktopSyncChatConsentFlow::retry() { _state->post([](State &state) { if (state.engine) { state.engine->resume(); } }); }
void DesktopSyncChatConsentFlow::close() { if (_state) { _state->close(); } }
void DesktopSyncChatConsentFlow::withEngine(Fn<void(DesktopSyncChatCarry *)> work) {
	if (!_state || !_state->port.worker) { work(nullptr); return; }
	_state->port.worker([weak = std::weak_ptr<State>(_state), work = std::move(work)] {
		const auto self = weak.lock();
		work(self && SyncStoreWorkerThread() && self->live() ? self->engine.get() : nullptr);
	});
}

}
