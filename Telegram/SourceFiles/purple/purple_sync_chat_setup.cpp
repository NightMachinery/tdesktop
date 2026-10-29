#include "purple/purple_sync_chat_setup.h"
#include "purple/purple_sync_chat_journal.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>

namespace Purple {

bool SameDesktopSyncChatDisclosure(
		const DesktopSyncChatPeer &opening, const DesktopSyncChatPeer &fresh) {
	const auto first = DesktopSyncChatEligibility(opening);
	const auto second = DesktopSyncChatEligibility(fresh);
	return opening.origin == fresh.origin && opening.chat == fresh.chat
		&& opening.kind == fresh.kind && first && second && *first && *second
		&& !second->readers.populationUnknown
		&& first->readers == second->readers
		&& opening.creator == fresh.creator && opening.soleAdmin == fresh.soleAdmin
		&& opening.readerMembers == fresh.readerMembers;
}

bool DesktopSyncChatFreshSetup(const DesktopSyncChatSetupSnapshot &snapshot) {
	return ValidDesktopSyncChatOrigin(snapshot.scope.origin)
		&& snapshot.run != 0 && snapshot.scope.loginBinding.size() == 64
		&& !snapshot.scope.settingsBound && snapshot.scope.space.isEmpty()
		&& !snapshot.scope.mark && !snapshot.scope.carry && !snapshot.cache
		&& !snapshot.input.settingsBound && !snapshot.input.cache
		&& snapshot.input.settingsHint == SyncChatSettingsHint::Absent
		&& snapshot.input.marks.empty() && snapshot.status.complete
		&& snapshot.status.phase == DesktopSyncChatPhase::Unset
		&& snapshot.status.resolution.action == SyncChatAction::Unset;
}

DesktopSyncChatSetupRefusal DesktopSyncChatSetupEligibility(
		const DesktopSyncChatSetupSnapshot &snapshot,
		const DesktopSyncChatPeer &peer) {
	if (peer.origin != snapshot.scope.origin) {
		return DesktopSyncChatSetupRefusal::Stale;
	} else if (snapshot.scope.settingsBound || snapshot.scope.mark
		|| snapshot.scope.carry || !snapshot.scope.space.isEmpty()
		|| snapshot.cache || snapshot.input.settingsBound || snapshot.input.cache) {
		return DesktopSyncChatSetupRefusal::Bound;
	} else if (!DesktopSyncChatFreshSetup(snapshot)) {
		return DesktopSyncChatSetupRefusal::Incomplete;
	}
	const auto token = PeerTokenText(peer.chat, peer.origin.account.userId);
	if (!peer.facts || peer.facts->kind != peer.kind
		|| !token || ParsePeerToken(*token, peer.origin.account.userId) != peer.chat
		|| (peer.chat == PeerToken{}) != (peer.kind == SyncChatKind::Self)) {
		return DesktopSyncChatSetupRefusal::Ineligible;
	}
	const auto eligible = DesktopSyncChatEligibility(peer);
	if (!eligible || !*eligible) {
		return DesktopSyncChatSetupRefusal::Ineligible;
	} else if (eligible->readers.populationUnknown) {
		return DesktopSyncChatSetupRefusal::UnknownReaders;
	}
	return DesktopSyncChatSetupRefusal::None;
}

DesktopSyncChatSetupRefusal AcceptDesktopSyncChatSetup(
		const DesktopSyncChatSetupSnapshot &opening,
		const DesktopSyncChatSetupSnapshot &fresh,
		const DesktopSyncChatSetupSelection &selection) {
	if (selection.provisionOnly) { return DesktopSyncChatSetupRefusal::Bound; }
	if (opening.scope != fresh.scope || opening.run != fresh.run) {
		return DesktopSyncChatSetupRefusal::Stale;
	} else if (!DesktopSyncChatFreshSetup(fresh)) {
		return fresh.scope.settingsBound || fresh.scope.mark || fresh.cache
			? DesktopSyncChatSetupRefusal::Bound
			: DesktopSyncChatSetupRefusal::Incomplete;
	} else if (selection.choice == DesktopSyncChatSetupChoice::Create) {
		return selection.peer ? DesktopSyncChatSetupRefusal::Ineligible
			: DesktopSyncChatSetupRefusal::None;
	} else if (!selection.peer
		|| (selection.choice == DesktopSyncChatSetupChoice::Self)
			!= (selection.peer->chat == PeerToken{})) {
		return DesktopSyncChatSetupRefusal::Ineligible;
	}
	const auto result = DesktopSyncChatSetupEligibility(fresh, *selection.peer);
	if (result != DesktopSyncChatSetupRefusal::None) {
		return result;
	}
	const auto eligible = DesktopSyncChatEligibility(*selection.peer);
	return eligible->readers.warningRequired && !selection.readerConsent
		? DesktopSyncChatSetupRefusal::ConsentRequired
		: DesktopSyncChatSetupRefusal::None;
}

QString DesktopSyncChatMoveFingerprint(const DesktopSyncChatSetupSnapshot &snapshot) {
	const auto &scope = snapshot.scope;
	const auto &resolution = snapshot.status.resolution;
	const auto state = ParseSyncLocalState(snapshot.stateImage);
	if (!state || state.state.bindingToken.toLatin1() != scope.binding
		|| state.state.chat != scope.settingsChat || state.state.space != scope.space) {
		return {};
	}
	if (!ValidDesktopSyncChatOrigin(scope.origin) || !snapshot.run
		|| scope.loginBinding.size() != 64 || scope.binding.isEmpty()
		|| scope.root.isEmpty() || !scope.settingsBound || scope.space.isEmpty()
		|| scope.carry || !snapshot.input.settingsBound
		|| snapshot.input.settingsChat != scope.settingsChat || !snapshot.status.complete
		|| snapshot.status.phase != DesktopSyncChatPhase::Use
		|| resolution.action != SyncChatAction::Use
		|| resolution.readChat != scope.settingsChat
		|| resolution.readers.populationUnknown || snapshot.status.postingHeld
		|| resolution.postingHeld) {
		return {};
	}
	const auto cache = snapshot.cache
		? SerializeDesktopSyncChatCache(*snapshot.cache) : std::optional(QByteArray());
	if (!cache || (snapshot.cache && (snapshot.cache->account != scope.origin.account
		|| snapshot.cache->loginBinding != scope.loginBinding
		|| snapshot.cache->carry))) {
		return {};
	}
	const auto mark = [](const std::optional<SyncChatCache> &value) {
		return value ? QJsonObject{{u"chat"_q, PeerTokenText(value->chat).value_or(QString())},
			{u"id"_q, value->markId}, {u"date"_q, QString::number(value->markDate)}}
			: QJsonObject();
	};
	const auto object = QJsonObject{
		{u"account"_q, AccountKeyText(scope.origin.account.environment,
			scope.origin.account.userId).value_or(QString())},
		{u"login"_q, scope.origin.login}, {u"authority"_q, scope.loginBinding},
		{u"root"_q, scope.root}, {u"binding"_q, QString::fromLatin1(scope.binding.toHex())},
		{u"space"_q, scope.space}, {u"source"_q, PeerTokenText(scope.settingsChat).value_or(QString())},
		{u"mode"_q, int(scope.mode)}, {u"owner"_q, scope.owner
			? AccountKeyText(scope.owner->environment, scope.owner->userId).value_or(QString()) : QString()},
		{u"cache"_q, QString::fromLatin1(cache->toBase64())},
		{u"state"_q, QString::fromLatin1(QCryptographicHash::hash(
			snapshot.stateImage, QCryptographicHash::Sha256).toHex())},
		{u"scope_mark"_q, mark(scope.mark)}, {u"input_mark"_q, mark(snapshot.input.cache)},
		{u"winner"_q, mark(resolution.winner)}, {u"resolved_cache"_q, mark(resolution.cache)},
		{u"read"_q, int(snapshot.input.cacheRead)}};
	return QString::fromLatin1(QCryptographicHash::hash(
		QJsonDocument(object).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

DesktopSyncChatSetupRefusal DesktopSyncChatMoveEligibility(
		const DesktopSyncChatSetupSnapshot &snapshot, const DesktopSyncChatPeer &peer) {
	if (DesktopSyncChatMoveFingerprint(snapshot).isEmpty()) {
		return DesktopSyncChatSetupRefusal::Incomplete;
	} else if (peer.chat == snapshot.scope.settingsChat) {
		return DesktopSyncChatSetupRefusal::Bound;
	}
	auto fresh = snapshot;
	fresh.scope.settingsBound = false;
	fresh.scope.settingsChat = {};
	fresh.scope.space.clear();
	fresh.scope.mark.reset();
	fresh.cache.reset();
	fresh.input = {};
	fresh.input.settingsHint = SyncChatSettingsHint::Absent;
	fresh.status = {};
	fresh.status.complete = true;
	fresh.status.phase = DesktopSyncChatPhase::Unset;
	fresh.status.resolution.action = SyncChatAction::Unset;
	return DesktopSyncChatSetupEligibility(fresh, peer);
}

DesktopSyncChatMoveNotice DesktopSyncChatMoveNoticeOf(
		const SyncCarryJournal &journal, bool storeReady, bool retryable) {
	if (!storeReady) { return DesktopSyncChatMoveNotice::Held; }
	if (journal.stateCommitted && journal.cacheCompleted && journal.markVerified
		&& journal.destinationVerified) {
		if (journal.phase == SyncCarryPhase::Done) {
			return DesktopSyncChatMoveNotice::Complete;
		} else if (journal.phase == SyncCarryPhase::Cleanup
			|| journal.phase == SyncCarryPhase::CleanupSkipped) {
			return DesktopSyncChatMoveNotice::Committed;
		}
	}
	if (journal.phase == SyncCarryPhase::Aborted && !journal.stateCommitted) {
		return DesktopSyncChatMoveNotice::Cancelled;
	}
	return retryable ? DesktopSyncChatMoveNotice::Retryable : DesktopSyncChatMoveNotice::Held;
}

bool DesktopSyncChatMoveActivated() { return false; }

DesktopSyncChatMoveAction::DesktopSyncChatMoveAction(
		DesktopSyncChatSetupSnapshot opening, Fn<bool()> current)
: _opening(std::move(opening))
, _current(std::move(current)) {
}

std::optional<DesktopSyncChatMoveOffer> DesktopSyncChatMoveAction::choose(
		DesktopSyncChatSetupSelection selection) {
	++_ticket;
	_offer.reset();
	_reviewed.reset();
	const auto fingerprint = DesktopSyncChatMoveFingerprint(_opening);
	if (!_current || !_current() || _phase == DesktopSyncChatMovePhase::Cancelled
		|| fingerprint.isEmpty()
		|| selection.provisionOnly
		|| selection.choice < DesktopSyncChatSetupChoice::Create
		|| selection.choice > DesktopSyncChatSetupChoice::Existing
		|| (selection.choice == DesktopSyncChatSetupChoice::Create ? selection.peer.has_value()
			: !selection.peer || (selection.choice == DesktopSyncChatSetupChoice::Self)
				!= (selection.peer->chat == PeerToken{})
				|| DesktopSyncChatMoveEligibility(_opening, *selection.peer)
					!= DesktopSyncChatSetupRefusal::None)) {
		_phase = DesktopSyncChatMovePhase::Held;
		_reason = u"move-choice-held"_q;
		return {};
	}
	selection.readerConsent = false;
	_offer = DesktopSyncChatMoveOffer{_ticket, std::move(selection), fingerprint,
		_opening.scope.settingsChat != PeerToken{}};
	_phase = DesktopSyncChatMovePhase::Confirming;
	_reason.clear();
	return _offer;
}

bool DesktopSyncChatMoveAction::confirm(const DesktopSyncChatMoveOffer &offer,
		const DesktopSyncChatSetupSnapshot &fresh, std::optional<DesktopSyncChatPeer> peer,
		bool cleanup, bool consent, Fn<void(DesktopSyncChatMoveDraft)> start) {
	const auto same = _offer && offer.ticket == _offer->ticket
		&& offer.sourceFingerprint == _offer->sourceFingerprint
		&& offer.selection.choice == _offer->selection.choice
		&& offer.selection.provisionOnly == _offer->selection.provisionOnly
		&& (offer.selection.peer.has_value() == _offer->selection.peer.has_value())
		&& (!offer.selection.peer || SameDesktopSyncChatDisclosure(
			*offer.selection.peer, *_offer->selection.peer));
	if (_phase != DesktopSyncChatMovePhase::Confirming || !same) { return false; }
	_offer.reset();
	++_ticket;
	_phase = DesktopSyncChatMovePhase::Held;
	if (!_current || !_current() || !consent || _opening.scope != fresh.scope
		|| offer.sourceFingerprint != DesktopSyncChatMoveFingerprint(fresh)
		|| (offer.selection.choice == DesktopSyncChatSetupChoice::Create ? peer.has_value()
			: !peer || !SameDesktopSyncChatDisclosure(*offer.selection.peer, *peer)
				|| DesktopSyncChatMoveEligibility(fresh, *peer) != DesktopSyncChatSetupRefusal::None)) {
		_reason = u"move-confirmation-stale"_q;
		return false;
	}
	if (_phase != DesktopSyncChatMovePhase::Held || _ticket != offer.ticket + 1) {
		return false;
	}
	auto selected = offer.selection;
	selected.peer = std::move(peer);
	selected.readerConsent = true;
	selected.provisionOnly = selected.choice == DesktopSyncChatSetupChoice::Create;
	_reviewed = DesktopSyncChatMoveDraft{fresh, std::move(selected), cleanup};
	if (!DesktopSyncChatMoveActivated()) {
		_reason = u"move-qualification-held"_q;
		return true;
	}
	if (start) { start(*_reviewed); }
	return true;
}

void DesktopSyncChatMoveAction::dismiss(uint64_t ticket) {
	if (_offer && _offer->ticket == ticket) {
		_offer.reset();
		++_ticket;
		_phase = DesktopSyncChatMovePhase::Choosing;
		_reason.clear();
	}
}

void DesktopSyncChatMoveAction::cancel() {
	_offer.reset();
	_reviewed.reset();
	++_ticket;
	_phase = DesktopSyncChatMovePhase::Cancelled;
	_reason = u"move-cancelled"_q;
}

DesktopSyncChatMovePhase DesktopSyncChatMoveAction::phase() const { return _phase; }
QString DesktopSyncChatMoveAction::reason() const { return _reason; }
const std::optional<DesktopSyncChatMoveDraft> &DesktopSyncChatMoveAction::reviewed() const {
	return _reviewed;
}

}
