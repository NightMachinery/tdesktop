#include "purple/purple_sync_chat_mark_actions.h"

#include <QtCore/QJsonDocument>
#include <algorithm>

namespace Purple {
namespace {

bool Authority(const QString &value) {
	return value.size() == 64 && std::all_of(value.begin(), value.end(), [](QChar c) {
		return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f');
	});
}

QByteArray Author(const MTPDmessage &message) {
	auto wire = mtpBuffer();
	if (const auto from = message.vfrom_id()) { from->write(wire); }
	auto result = QByteArray(reinterpret_cast<const char*>(wire.constData()), wire.size() * sizeof(mtpPrime));
	result += message.is_out() ? ":out:" : ":in:";
	result += message.is_post() ? "post:" : "message:";
	if (const auto signature = message.vpost_author()) { result += signature->v; }
	return result;
}

std::optional<MTPMessage> ExactMessage(
		const DesktopSyncChatStaleMark &row, const DesktopSyncChatMarkExact &exact) {
	if (exact.route.peer.origin != row.origin || exact.route.peer.chat != row.location.chat
		|| !exact.readable || !ValidDesktopSyncChatReadIo(exact.route)
		|| !DesktopSyncChatRecordRequest(exact.route, row.location.markId)
		|| !RecognizedDesktopSyncChatRead(exact.response, row.location.chat)) { return {}; }
	const auto consistent = exact.response.match([](const MTPDmessages_messagesSlice &data) {
		return data.vcount().v >= data.vmessages().v.size();
	}, [](const MTPDmessages_channelMessages &data) {
		return data.vcount().v >= data.vmessages().v.size();
	}, [](const auto &) { return true; });
	if (!consistent) { return {}; }
	const auto messages = exact.response.match([](const MTPDmessages_messagesNotModified &) {
		return static_cast<const QVector<MTPMessage>*>(nullptr);
	}, [](const auto &data) { return &data.vmessages().v; });
	if (!messages || messages->size() != 1) { return {}; }
	return messages->front();
}

bool Resolution(const DesktopSyncChatStaleMark &row, const DesktopSyncChatSetupSnapshot &fresh) {
	const auto resolved = ResolveSyncChat(fresh.input);
	return fresh.scope.origin == row.origin && fresh.scope.loginBinding == row.login
		&& fresh.status.complete && !fresh.status.postingHeld && !fresh.scope.carry
		&& resolved.action == SyncChatAction::Use && !resolved.postingHeld
		&& fresh.status.resolution.action == resolved.action
		&& fresh.status.resolution.winner == resolved.winner
		&& fresh.status.resolution.cache == resolved.cache
		&& fresh.input.selfUserId == row.origin.account.userId
		&& resolved.winner && resolved.cache == resolved.winner
		&& SyncChatMarkOutranks(*resolved.winner, row.location);
}

}

bool DesktopSyncChatMarkRemovalActivated() { return false; }

std::optional<DesktopSyncChatStaleMark> CaptureDesktopSyncChatStaleMark(
		const DesktopSyncChatPeer &peer, const QString &login, const MTPMessage &message) {
	if (!ValidDesktopSyncChatOrigin(peer.origin) || !Authority(login) || !peer.facts
		|| peer.facts->kind != peer.kind || message.type() != mtpc_message) { return {}; }
	const auto &data = message.c_message();
	const auto own = GetSyncChatMessageOwnership(MapDesktopSyncChatMessage(peer.origin, peer, message));
	const auto media = data.vmedia();
	const auto hasMedia = media && media->type() != mtpc_messageMediaEmpty;
	const auto text = qs(data.vmessage());
	if (data.vid().v <= 0 || data.vdate().v <= 0
		|| DesktopSyncChatToken(data.vpeer_id(), peer.origin.account.userId) != peer.chat
		|| own != SyncChatMessageOwnership::Own
		|| ParseSyncChatMark(text, hasMedia, own).status != SyncChatMarkStatus::Supported) { return {}; }
	return DesktopSyncChatStaleMark{peer.origin, login,
		{peer.chat, data.vid().v, data.vdate().v}, text, Author(data)};
}

bool DesktopSyncChatSelectedMarkAbsent(
		const DesktopSyncChatStaleMark &row, const DesktopSyncChatMarkExact &exact) {
	const auto message = ExactMessage(row, exact);
	if (!message || message->type() != mtpc_messageEmpty) { return false; }
	const auto &empty = message->c_messageEmpty();
	return empty.vid().v == row.location.markId && (!empty.vpeer_id()
		|| DesktopSyncChatToken(*empty.vpeer_id(), row.origin.account.userId) == row.location.chat);
}

bool AuthorizeDesktopSyncChatStaleMark(
		const DesktopSyncChatStaleMark &row, const DesktopSyncChatSetupSnapshot &fresh,
		const DesktopSyncChatMarkExact &exact) {
	const auto message = ExactMessage(row, exact);
	const auto captured = message ? CaptureDesktopSyncChatStaleMark(exact.route.peer, row.login, *message) : std::nullopt;
	if (!Resolution(row, fresh) || !captured || *captured != row || !ValidDesktopSyncChatIo(exact.route)) { return false; }
	const auto readback = SyncCarryMarkReadback{SyncCarryMarkOutcome::Valid, row.origin.account,
		row.location.chat, row.location.markId, row.location.markDate, row.text, false, SyncChatMessageOwnership::Own};
	return CheckStaleMarkRemoval(row.origin.account, row.location, readback, ResolveSyncChat(fresh.input), true);
}

bool DesktopSyncChatMarkReferencesClear(SyncChatCarryStore &store, const DesktopSyncChatStaleMark &row) {
	if (!SyncStoreWorkerThread() || store.foreignLogin() || store.journal()) { return false; }
	const auto history = store.RetainedEffects();
	if (!history) { return false; }
	const auto refers = [&](const std::optional<SyncChatCache> &mark) {
		return mark && mark->chat == row.location.chat && mark->markId == row.location.markId;
	};
	for (const auto &journal : *history) {
		if (journal.account != row.origin.account) { return false; }
		if (journal.phase == SyncCarryPhase::Done) { continue; }
		if (SyncCarryJournalHoldsWrites(journal) || refers(journal.oldMark)
			|| refers(journal.targetMark) || refers(journal.expectedWinner)
			|| (journal.markAttempted && journal.target == row.location.chat && journal.markText == row.text)) { return false; }
	}
	return true;
}

bool DesktopSyncChatMarkPermit::current() const {
	if (!_admitted || !_admitted->load() || !_active || !_active->load() || !_current) { return false; }
	return _current() && _admitted->load() && _active->load();
}
const DesktopSyncChatStaleMark &DesktopSyncChatMarkPermit::row() const { return _row; }
const DesktopSyncChatIo &DesktopSyncChatMarkPermit::route() const { return _route; }
std::optional<DesktopSyncChatDelete> DesktopSyncChatMarkPermit::request() const {
	if (!current() || !_claimed || _claimed->exchange(true)) { return {}; }
	return DesktopSyncChatDeleteRequest(_route, _row.location.markId);
}

class DesktopSyncChatMarkActions::State final : public std::enable_shared_from_this<State> {
public:
	State(DesktopSyncChatStaleMark row, QString window, QString directory,
		SyncChatCarryStoreScope scope, DesktopSyncChatMarkPort port)
	: row(std::move(row)), window(std::move(window)), directory(std::move(directory)), scope(std::move(scope)), port(std::move(port)) {
	}
	bool owner() const {
		return port.current && port.current() && port.foreground && port.foreground()
			&& port.origin && port.origin() == row.origin && port.login && port.login() == row.login
			&& port.window && !window.isEmpty() && port.window() == window;
	}
	bool live() const { return !closed.load() && admitted->load() && owner() && !closed.load(); }
	void post(Fn<void(State &)> work) {
		const auto weak = weak_from_this();
		port.worker([weak, work = std::move(work)]() mutable {
			const auto self = weak.lock();
			if (!self || !SyncStoreWorkerThread()) { return; }
			try { if (self->live()) { work(*self); } else { self->close(); } }
			catch (...) { self->finish(u"mark-unavailable"_q); }
		});
	}
	void release() {
		if (released) { return; }
		released = true;
		admitted->store(false);
		if (active) { active->store(false); }
		store.reset(); lease.reset();
		try { if (port.cancel) { port.cancel(); } } catch (...) {}
	}
	void close() {
		if (closed.exchange(true)) { return; }
		admitted->store(false);
		const auto self = shared_from_this();
		port.worker([self] { self->release(); });
	}
	void finish(QString reason, bool absent = false) {
		if (finished || closed.load()) { return; }
		finished = true; ++ticket; release();
		if (owner() && !closed.load() && port.status) {
			try { port.status({std::move(reason), true, absent}); } catch (...) {}
		}
	}
	void start() {
		post([](State &self) {
			if (self.started) { return; }
			self.started = true;
			if (self.scope.account != self.row.origin.account || self.scope.loginAuthority != self.row.login) {
				self.finish(u"mark-opening-changed"_q); return;
			}
			const auto gate = DesktopSyncCarryGateFor(self.row.origin.account);
			self.lease = gate ? gate->carry() : nullptr;
			if (!self.lease || !self.lease->quiescent()) { self.finish(u"mark-account-busy"_q); return; }
			self.store = std::make_unique<SyncChatCarryStore>(self.directory, self.scope);
			if (!self.store->Open()) { self.finish(u"mark-store-held"_q); return; }
			self.authorize([weak = self.weak_from_this()](std::optional<DesktopSyncChatMarkPermit> permit) {
				if (const auto state = weak.lock(); state && state->live() && permit) { state->send(*permit); }
			});
		});
	}
	void authorize(Fn<void(std::optional<DesktopSyncChatMarkPermit>)> ready) {
		if (!live() || !store || !lease || !lease->quiescent() || !port.resolve || !port.exact) { finish(u"mark-admission-held"_q); return; }
		if (active) { active->store(false); }
		const auto run = ++ticket;
		const auto weak = weak_from_this();
		port.resolve([weak, run, ready = std::move(ready)](std::optional<DesktopSyncChatSetupSnapshot> fresh) mutable {
			if (const auto self = weak.lock()) { self->post([run, fresh = std::move(fresh), ready = std::move(ready)](State &state) mutable {
				if (state.ticket != run) { return; }
				++state.ticket;
				if (!fresh || !Resolution(state.row, *fresh)) { state.finish(u"mark-resolution-held"_q); if (ready) { ready({}); } return; }
				if (!DesktopSyncChatMarkReferencesClear(*state.store, state.row)) { state.finish(u"mark-move-reference"_q); if (ready) { ready({}); } return; }
				state.read(*fresh, std::move(ready));
			}); }
		});
	}
	void read(DesktopSyncChatSetupSnapshot fresh, Fn<void(std::optional<DesktopSyncChatMarkPermit>)> ready) {
		const auto run = ++ticket;
		const auto weak = weak_from_this();
		port.exact(row, [weak, run, fresh = std::move(fresh), ready = std::move(ready)](std::optional<DesktopSyncChatMarkExact> exact) mutable {
			if (const auto self = weak.lock()) { self->post([run, fresh = std::move(fresh), exact = std::move(exact), ready = std::move(ready)](State &state) mutable {
				if (state.ticket != run) { return; }
				++state.ticket;
				if (exact && DesktopSyncChatSelectedMarkAbsent(state.row, *exact)) { state.finish(u"mark-already-absent"_q, true); if (ready) { ready({}); } return; }
				if (!exact || !AuthorizeDesktopSyncChatStaleMark(state.row, fresh, *exact)
					|| !DesktopSyncChatMarkReferencesClear(*state.store, state.row)) { state.finish(u"mark-changed"_q); if (ready) { ready({}); } return; }
				auto permit = DesktopSyncChatMarkPermit();
				permit._row = state.row; permit._route = exact->route; permit._admitted = state.admitted;
				permit._active = state.active = std::make_shared<std::atomic<bool>>(true);
				permit._claimed = std::make_shared<std::atomic<bool>>(false);
				permit._current = [weak = state.weak_from_this(), gate = std::weak_ptr<DesktopSyncCarryGate::Lease>(state.lease)] {
					const auto self = weak.lock(); const auto lease = gate.lock();
					return self && self->live() && lease && lease->quiescent();
				};
				if (ready) { ready(std::move(permit)); }
			}); }
		});
	}
	void send(DesktopSyncChatMarkPermit permit) {
		if (!live() || !port.send || !port.apply || !permit.current()) { finish(u"mark-transport-held"_q); return; }
		const auto weak = weak_from_this();
		const auto run = ++sendTicket;
		port.send(permit, [weak, run](Fn<void(std::optional<DesktopSyncChatMarkPermit>)> ready) {
			if (const auto self = weak.lock()) { self->post([run, ready = std::move(ready)](State &state) mutable {
				if (state.sendTicket == run && !state.acknowledged) { state.authorize(std::move(ready)); }
			}); }
		}, [weak, run, route = permit.route()](std::optional<MTPmessages_AffectedMessages> affected) {
			if (const auto self = weak.lock()) { self->post([run, route, affected = std::move(affected)](State &state) mutable {
				if (state.sendTicket != run || state.acknowledged) { return; }
				state.acknowledged = true;
				if (!affected || affected->data().vpts().v < 0 || affected->data().vpts_count().v < 0) { state.finish(u"mark-delete-uncertain"_q); return; }
				state.apply(route, *affected);
			}); }
		});
	}
	void apply(DesktopSyncChatIo route, MTPmessages_AffectedMessages affected) {
		const auto run = ++ticket;
		const auto weak = weak_from_this();
		port.apply(route, affected, [weak, run](bool applied) {
			if (const auto self = weak.lock()) { self->post([run, applied](State &state) {
				if (state.ticket != run) { return; }
				++state.ticket;
				if (!applied) { state.finish(u"mark-updates-held"_q); return; }
				state.verify();
			}); }
		});
	}
	void verify() {
		const auto run = ++ticket;
		const auto weak = weak_from_this();
		port.exact(row, [weak, run](std::optional<DesktopSyncChatMarkExact> exact) {
			if (const auto self = weak.lock()) { self->post([run, exact = std::move(exact)](State &state) {
				if (state.ticket != run) { return; }
				++state.ticket;
				const auto absent = exact && DesktopSyncChatSelectedMarkAbsent(state.row, *exact);
				state.finish(absent ? u"mark-removed"_q : u"mark-absence-unknown"_q, absent);
			}); }
		});
	}
	DesktopSyncChatStaleMark row;
	QString window;
	QString directory;
	SyncChatCarryStoreScope scope;
	DesktopSyncChatMarkPort port;
	std::atomic<bool> closed = false;
	std::shared_ptr<std::atomic<bool>> admitted = std::make_shared<std::atomic<bool>>(true);
	std::shared_ptr<std::atomic<bool>> active;
	std::shared_ptr<DesktopSyncCarryGate::Lease> lease;
	std::unique_ptr<SyncChatCarryStore> store;
	uint64_t ticket = 0;
	uint64_t sendTicket = 0;
	bool started = false;
	bool finished = false;
	bool acknowledged = false;
	bool released = false;
};

DesktopSyncChatMarkActions::DesktopSyncChatMarkActions(DesktopSyncChatStaleMark row, QString window,
		QString directory, SyncChatCarryStoreScope scope, DesktopSyncChatMarkPort port)
: _state(std::make_shared<State>(std::move(row), std::move(window), std::move(directory), std::move(scope), std::move(port))) {
}
DesktopSyncChatMarkActions::~DesktopSyncChatMarkActions() { close(); }
void DesktopSyncChatMarkActions::start() { _state->start(); }
void DesktopSyncChatMarkActions::close() { _state->close(); }

}
