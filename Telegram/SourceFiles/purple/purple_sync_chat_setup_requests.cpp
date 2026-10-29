#include "purple/purple_sync_chat_setup_requests.h"
#include "purple/purple_sync_chat_journal.h"
#include "purple/purple_sync_chat_carry_store.h"

#include <QtCore/QUuid>

#include <cstring>
#include <limits>

namespace Purple {

std::optional<MTPmessages_SendMessage> DesktopSyncChatMarkRequest(
		const DesktopSyncChatIo &route, uint64_t randomId) {
	if (!ValidDesktopSyncChatIo(route) || !randomId) {
		return {};
	}
	return MTPmessages_SendMessage(
		MTP_flags(MTPmessages_SendMessage::Flag::f_silent
			| MTPmessages_SendMessage::Flag::f_no_webpage),
		route.input, MTPInputReplyTo(), MTP_string(DesktopSyncChatMarkText()),
		MTP_long(randomId), MTPReplyMarkup(), MTP_vector<MTPMessageEntity>(),
		MTP_int(0), MTP_int(0), MTPInputPeer(), MTPInputQuickReplyShortcut(),
		MTP_long(0), MTP_long(0), MTPSuggestedPost(), MTPInputRichMessage());
}

std::optional<DesktopSyncChatDecoration> DesktopSyncChatDecorationRequest(
		const DesktopSyncChatCreateCommand &command,
		const DesktopSyncChatIo &route) {
	const auto &journal = command.journal;
	if (!journal.provisionSource.isEmpty()
		|| !ValidDesktopSyncChatIo(route) || !journal.created || !journal.chat
		|| *journal.chat != route.peer.chat || journal.markId <= 0
		|| route.peer.chat.kind != PeerTokenKind::Channel
		|| route.peer.origin != command.origin) {
		return {};
	}
	switch (command.step) {
	case DesktopSyncChatCreateStep::Pin:
		return MTPmessages_UpdatePinnedMessage(
			MTP_flags(MTPmessages_UpdatePinnedMessage::Flag::f_silent),
			route.input, MTP_int(journal.markId));
	case DesktopSyncChatCreateStep::Mute:
		return MTPaccount_UpdateNotifySettings(
			MTP_inputNotifyPeer(route.input), MTP_inputPeerNotifySettings(
				MTP_flags(MTPDinputPeerNotifySettings::Flag::f_mute_until
					| MTPDinputPeerNotifySettings::Flag::f_silent),
				MTPBool(), MTP_bool(true), MTP_int(std::numeric_limits<int32_t>::max()),
				MTPNotificationSound(), MTPBool(), MTPBool(), MTPNotificationSound()));
	case DesktopSyncChatCreateStep::Archive:
		return MTPfolders_EditPeerFolders(MTP_vector<MTPInputFolderPeer>(
			1, MTP_inputFolderPeer(route.input, MTP_int(1))));
	default:
		return {};
	}
}

auto DesktopSyncChatMoveDecorationRequest(
		const DesktopSyncChatSetupSnapshot &opening,
		const DesktopSyncChatCreateJournal &provision,
		const SyncCarryJournal &carry, const DesktopSyncChatIo &route,
		DesktopSyncChatCreateStep step)
-> std::optional<DesktopSyncChatDecoration> {
	const auto eligibility = DesktopSyncChatEligibility(route.peer);
	if (!SerializeDesktopSyncChatCreateJournal(provision)
		|| !SerializeSyncCarryJournal(carry)
		|| provision.provisionSource.isEmpty()
		|| provision.provisionSource != DesktopSyncChatMoveFingerprint(opening)
		|| provision.authority != DesktopSyncChatSetupAuthority(opening.scope)
		|| provision.account != opening.scope.origin.account
		|| provision.cancelled || provision.pending || !provision.created
		|| provision.step != DesktopSyncChatCreateStep::Done
		|| !provision.chat || provision.chat->kind != PeerTokenKind::Channel
		|| carry.mode != SyncCarryMode::Switch
		|| carry.account != provision.account
		|| carry.loginAuthority != DesktopSyncCarryLogin(opening.scope.loginBinding)
		|| carry.source != opening.scope.settingsChat || carry.target != *provision.chat
		|| carry.stateHash != SyncCarryRecordHash(opening.stateImage)
		|| !carry.stateCommitted || !carry.cacheCompleted || !carry.markVerified
		|| !carry.destinationVerified || !carry.targetMark
		|| (carry.phase != SyncCarryPhase::Cleanup
			&& carry.phase != SyncCarryPhase::CleanupSkipped
			&& carry.phase != SyncCarryPhase::Done)
		|| route.peer.origin != opening.scope.origin
		|| route.peer.chat != carry.target
		|| !eligibility || !*eligibility) {
		return {};
	}
	auto journal = provision;
	journal.provisionSource.clear();
	journal.markId = carry.targetMark->markId;
	journal.markDate = carry.targetMark->markDate;
	return DesktopSyncChatDecorationRequest({opening.scope.origin,
		provision.operation, 0, step, std::move(journal)}, route);
}

DesktopSyncChatMoveDecorationAction::DesktopSyncChatMoveDecorationAction(
		DesktopSyncChatSetupSnapshot opening, DesktopSyncChatCreateJournal provision,
		Fn<bool()> current)
: _opening(std::move(opening))
, _provision(std::move(provision))
, _current(std::move(current))
, _run(QUuid::createUuid().toString(QUuid::Id128)) {
}

auto DesktopSyncChatMoveDecorationAction::observe(
		const SyncCarryJournal &carry, const DesktopSyncChatIo &route)
-> std::optional<DesktopSyncChatMoveDecorationCommand> {
	if (!_current || !_current()) { cancel(); return {}; }
	if (_phase != DesktopSyncChatMoveDecorationPhase::NotStarted
		&& _phase != DesktopSyncChatMoveDecorationPhase::Ready) { return {}; }
	const auto request = DesktopSyncChatMoveDecorationRequest(
		_opening, _provision, carry, route, _next);
	if (!request) {
		if (_phase == DesktopSyncChatMoveDecorationPhase::Ready) {
			_phase = DesktopSyncChatMoveDecorationPhase::Held;
			_reason = u"decoration-admission-held"_q;
		}
		return {};
	}
	const auto receipt = carry.operationId + u':' + carry.nextStateHash
		+ u':' + QString::number(carry.targetMark->markId)
		+ u':' + QString::number(carry.targetMark->markDate);
	if ((!_receipt.isEmpty() && _receipt != receipt)
		|| _ticket == std::numeric_limits<uint64_t>::max()) {
		_phase = DesktopSyncChatMoveDecorationPhase::Held;
		_reason = u"decoration-receipt-held"_q;
		return {};
	}
	_receipt = receipt;
	_active = DesktopSyncChatMoveDecorationCommand{
		_run, carry.operationId, ++_ticket, _next, *request};
	_phase = DesktopSyncChatMoveDecorationPhase::Waiting;
	_reason.clear();
	return _active;
}

auto DesktopSyncChatMoveDecorationAction::retry(
		const SyncCarryJournal &carry, const DesktopSyncChatIo &route)
-> std::optional<DesktopSyncChatMoveDecorationCommand> {
	if (_phase != DesktopSyncChatMoveDecorationPhase::Held) { return {}; }
	_phase = DesktopSyncChatMoveDecorationPhase::Ready;
	return observe(carry, route);
}

bool DesktopSyncChatMoveDecorationAction::accepts(
		const DesktopSyncChatMoveDecorationCommand &command) const {
	return _phase == DesktopSyncChatMoveDecorationPhase::Waiting && _active
		&& command.run == _active->run && command.operation == _active->operation
		&& command.ticket == _active->ticket && command.step == _active->step;
}

void DesktopSyncChatMoveDecorationAction::complete(
		const DesktopSyncChatMoveDecorationCommand &command, bool okay) {
	if (!accepts(command)) { return; }
	if (!_current || !_current()) { cancel(); return; }
	if (!accepts(command)) { return; }
	_active.reset();
	if (!okay) {
		_phase = DesktopSyncChatMoveDecorationPhase::Held;
		_reason = command.step == DesktopSyncChatCreateStep::Pin ? u"pin-held"_q
			: command.step == DesktopSyncChatCreateStep::Mute ? u"mute-held"_q : u"archive-held"_q;
		return;
	}
	if (command.step == DesktopSyncChatCreateStep::Archive) {
		_phase = DesktopSyncChatMoveDecorationPhase::Complete;
	} else {
		_next = command.step == DesktopSyncChatCreateStep::Pin
			? DesktopSyncChatCreateStep::Mute : DesktopSyncChatCreateStep::Archive;
		_phase = DesktopSyncChatMoveDecorationPhase::Ready;
	}
}

void DesktopSyncChatMoveDecorationAction::cancel() {
	_active.reset();
	_phase = DesktopSyncChatMoveDecorationPhase::Cancelled;
	_reason = u"decoration-cancelled"_q;
}

auto DesktopSyncChatMoveDecorationAction::phase() const
-> DesktopSyncChatMoveDecorationPhase {
	return _phase;
}

QString DesktopSyncChatMoveDecorationAction::reason() const { return _reason; }

QByteArray EncodeDesktopSyncChatSetupUpdates(const MTPUpdates &updates) {
	auto words = mtpBuffer();
	updates.write(words);
	if (words.empty() || words.size() > 256 * 1024 / sizeof(mtpPrime)) {
		return {};
	}
	return QByteArray(reinterpret_cast<const char*>(words.constData()),
		words.size() * sizeof(mtpPrime));
}

std::optional<MTPUpdates> DecodeDesktopSyncChatSetupUpdates(const QByteArray &bytes) {
	if (bytes.isEmpty() || bytes.size() > 256 * 1024 || bytes.size() % sizeof(mtpPrime)) {
		return {};
	}
	auto words = mtpBuffer(bytes.size() / sizeof(mtpPrime));
	std::memcpy(words.data(), bytes.constData(), bytes.size());
	const auto begin = words.constData();
	auto cursor = begin;
	auto updates = MTPUpdates();
	return updates.read(cursor, begin + words.size()) && cursor == begin + words.size()
		? std::optional(updates) : std::nullopt;
}

DesktopSyncChatCreateOutcome DesktopSyncChatSetupError(const QString &type) {
	if (type == u"CHANNELS_TOO_MUCH"_q || type == u"CHANNELS_ADMIN_PUBLIC_TOO_MUCH"_q) {
		return DesktopSyncChatCreateOutcome::Limit;
	}
	if (type == u"USER_RESTRICTED"_q || type == u"USER_BANNED_IN_CHANNEL"_q) {
		return DesktopSyncChatCreateOutcome::Restricted;
	}
	for (const auto &permanent : { u"CHAT_WRITE_FORBIDDEN"_q, u"CHANNEL_PRIVATE"_q,
			u"PEER_ID_INVALID"_q, u"USER_ID_INVALID"_q, u"CHAT_ADMIN_REQUIRED"_q,
			u"CHAT_RESTRICTED"_q, u"MESSAGE_TOO_LONG"_q, u"PURPLE_EFFECT_RESUME_REFUSED"_q }) {
		if (type == permanent) {
			return DesktopSyncChatCreateOutcome::Rejected;
		}
	}
	return DesktopSyncChatCreateOutcome::Ambiguous;
}

}
