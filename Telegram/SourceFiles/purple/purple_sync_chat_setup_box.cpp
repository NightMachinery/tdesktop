#include "purple/purple_sync_chat_setup_box.h"

#include "base/weak_ptr.h"
#include "crl/crl_on_main.h"
#include "core/application.h"
#include <QtGui/QGuiApplication>
#include <QtCore/QPointer>
#include "data/data_channel.h"
#include "data/data_folder.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_sync_chat_carry_owner.h"
#include "purple/purple_sync_chat_peer_request.h"
#include "purple/purple_sync_chat_service.h"
#include "purple/purple_sync_chat_setup_service.h"
#include "purple/purple_sync_request_sender.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"

#include "styles/style_layers.h"
#include "styles/style_widgets.h"

namespace Purple {
namespace {

PeerData *Loaded(Main::Session &session, PeerToken chat) {
	if (chat == PeerToken{}) {
		return session.user();
	}
	const auto id = chat.kind == PeerTokenKind::User
		? peerFromUser(UserId(chat.id)) : chat.kind == PeerTokenKind::Chat
		? peerFromChat(ChatId(chat.id)) : peerFromChannel(ChannelId(chat.id));
	return session.data().peerLoaded(id);
}

QString Title(Main::Session &session, PeerToken chat) {
	if (chat == PeerToken{}) {
		return tr::lng_purple_chat_self(tr::now);
	}
	const auto peer = Loaded(session, chat);
	return peer ? peer->name()
		: PeerTokenText(chat, session.userId().bare).value_or(QString());
}

QString Refusal(SyncChatRefusal refusal) {
	switch (refusal) {
	case SyncChatRefusal::None: return {};
	case SyncChatRefusal::Secret: return tr::lng_purple_chat_refuse_secret(tr::now);
	case SyncChatRefusal::TopicRouting: return tr::lng_purple_chat_refuse_topic(tr::now);
	case SyncChatRefusal::Public: return tr::lng_purple_chat_refuse_public(tr::now);
	case SyncChatRefusal::Protected: return tr::lng_purple_chat_refuse_protected(tr::now);
	case SyncChatRefusal::Discussion: return tr::lng_purple_chat_refuse_discussion(tr::now);
	case SyncChatRefusal::AutoDelete: return tr::lng_purple_chat_refuse_ttl(tr::now);
	case SyncChatRefusal::Paid: return tr::lng_purple_chat_refuse_paid(tr::now);
	case SyncChatRefusal::CannotSendFiles: return tr::lng_purple_chat_refuse_files(tr::now);
	case SyncChatRefusal::SendAsOther: return tr::lng_purple_chat_refuse_sender(tr::now);
	case SyncChatRefusal::Migrated: return tr::lng_purple_chat_refuse_migrated(tr::now);
	case SyncChatRefusal::SlowMode: return tr::lng_purple_chat_refuse_slow(tr::now);
	case SyncChatRefusal::BroadcastNeedsCreatorAndSoleAdmin:
		return tr::lng_purple_chat_refuse_admin(tr::now);
	case SyncChatRefusal::UnknownKind:
	case SyncChatRefusal::BroadcastOwnershipUnselected:
		return tr::lng_purple_chat_refuse_unknown(tr::now);
	}
	return tr::lng_purple_chat_refuse_unknown(tr::now);
}

QString FollowFillDisclosure(const DesktopSyncChatFollowOffer &offer, const QString &chat) {
	auto text = (offer.firstPost ? tr::lng_purple_chat_fill_disclosure(tr::now)
		: tr::lng_purple_chat_follow_disclosure(tr::now)).arg(chat);
	text += u"\n"_q + tr::lng_purple_chat_settings_reader_warning(tr::now)
		+ u"\n"_q + tr::lng_purple_chat_settings_disclosure(tr::now).arg(chat)
		+ u"\n"_q + tr::lng_purple_chat_cloud_disclosure(tr::now);
	if (offer.stream == u"library"_q) { text += u"\n"_q + tr::lng_purple_chat_library_reader_warning(tr::now); }
	return text;
}

QString SettingsDisclosure(const QString &chat) {
	return tr::lng_purple_chat_settings_reader_warning(tr::now)
		+ u"\n"_q + tr::lng_purple_chat_settings_disclosure(tr::now).arg(chat);
}

QString ReaderCount(const SyncChatReaderFacts &readers) {
	return readers.populationUnknown
		? tr::lng_purple_chat_readers_unknown(tr::now)
		: readers.privateRecipient ? tr::lng_purple_chat_readers_private(tr::now)
		: readers.botOwner ? tr::lng_purple_chat_readers_bot(tr::now)
		: tr::lng_purple_chat_readers_count(tr::now).arg(
			QString::number(readers.otherMembers.value_or(0)),
			QString::number(readers.otherAdmins.value_or(0)));
}

QString Readers(const SyncChatReaderFacts &readers, const QString &chat) {
	return ReaderCount(readers) + u"\n"_q + SettingsDisclosure(chat)
		+ u"\n"_q + tr::lng_purple_chat_cloud_disclosure(tr::now);
}

QString RecoveryReason(const QString &reason) {
	if (reason == u"missing-local-version"_q) { return tr::lng_purple_chat_recovery_missing_base(tr::now); }
	if (reason == u"recovery-chat-gone"_q) { return tr::lng_purple_chat_recovery_gone(tr::now); }
	if (reason == u"recovery-mark-unproven"_q) { return tr::lng_purple_chat_recovery_unproven(tr::now); }
	if (reason == u"recovery-reader-consent"_q) { return tr::lng_purple_chat_recovery_consent(tr::now); }
	if (reason == u"recovery-pending-held"_q) { return tr::lng_purple_chat_recovery_pending(tr::now); }
	return tr::lng_purple_chat_recovery_refused(tr::now);
}

QString StatusText(Main::Session &session,
		const DesktopSyncChatStatus &status) {
	auto text = QString();
	switch (status.phase) {
	case DesktopSyncChatPhase::Unset:
		text = tr::lng_purple_chat_unset(tr::now); break;
	case DesktopSyncChatPhase::Searching:
		text = tr::lng_purple_chat_searching(tr::now); break;
	case DesktopSyncChatPhase::Use:
		text = tr::lng_purple_chat_use(tr::now).arg(Title(session,
			status.resolution.readChat.value_or(PeerToken{}))); break;
	case DesktopSyncChatPhase::Paused:
		text = tr::lng_purple_chat_paused(tr::now); break;
	case DesktopSyncChatPhase::Lost:
		text = tr::lng_purple_chat_lost(tr::now); break;
	}
	if (const auto pending = PendingSyncRequest(&session.mtp())) {
		text += u"\n"_q + SyncRequestWaitText(*pending);
	} else if (status.reason == u"telegram-transient"_q && status.retryAt > 0) {
		text += u"\n"_q + SyncRequestWaitText(
			{ SyncTransientKind::Timeout, status.retryAt });
	} else if (!status.reason.isEmpty()) {
		text += u"\n"_q + (status.reason.startsWith(u"recovery-"_q) || status.reason == u"missing-local-version"_q
			? RecoveryReason(status.reason) : status.reason == u"resolving"_q
			? tr::lng_purple_chat_searching(tr::now)
			: status.reason == u"newer-mark"_q
			? tr::lng_purple_chat_update_required(tr::now)
			: status.reason == u"unknown-readers"_q
			? tr::lng_purple_chat_readers_unknown(tr::now)
			: status.reason == u"sync-paused-here"_q
			? tr::lng_purple_chat_paused_here(tr::now)
			: status.reason == u"follow-required"_q || status.reason == u"fill-required"_q
			? tr::lng_purple_chat_change_disabled(tr::now)
			: tr::lng_purple_chat_check_failed(tr::now));
	}
	if (status.postingHeld) {
		text += u"\n"_q + tr::lng_purple_chat_posting_held(tr::now);
	}
	if (status.newReaders) {
		text += u"\n\n"_q + tr::lng_purple_chat_new_readers(tr::now)
			+ u"\n"_q + tr::lng_purple_chat_reader_blind_spot(tr::now);
		const auto peer = Loaded(session,
			status.resolution.readChat.value_or(PeerToken{}));
		const auto channel = peer ? peer->asChannel() : nullptr;
		text += u"\n"_q + (channel && channel->hiddenPreHistory()
			? tr::lng_purple_chat_future_readers(tr::now)
			: tr::lng_purple_chat_prior_readers(tr::now));
	}
	return text;
}

QString MoveNotice(DesktopSyncChatMoveNotice notice) {
	switch (notice) {
	case DesktopSyncChatMoveNotice::Held: return tr::lng_purple_chat_move_held(tr::now);
	case DesktopSyncChatMoveNotice::Retryable: return tr::lng_purple_chat_move_retry(tr::now);
	case DesktopSyncChatMoveNotice::Committed: return tr::lng_purple_chat_move_committed(tr::now);
	case DesktopSyncChatMoveNotice::Complete: return tr::lng_purple_chat_move_complete(tr::now);
	case DesktopSyncChatMoveNotice::Cancelled: return tr::lng_purple_chat_setup_cancelled(tr::now);
	}
	return tr::lng_purple_chat_move_stale(tr::now);
}

// With all five carry switches OFF the account's carry service is never constructed.
bool CarryOn() {
	using Switch = DesktopSyncChatCarrySwitch;
	for (const auto value : { Switch::Move, Switch::FollowFill, Switch::MarkRemoval,
			Switch::Cleanup, Switch::Recovery }) {
		if (DesktopSyncChatCarrySwitchActivated(value)) { return true; }
	}
	return false;
}

bool ResumeOn() {
	return DesktopSyncChatCarrySwitchActivated(DesktopSyncChatCarrySwitch::Move)
		|| DesktopSyncChatCarrySwitchActivated(DesktopSyncChatCarrySwitch::FollowFill)
		|| DesktopSyncChatCarrySwitchActivated(DesktopSyncChatCarrySwitch::Recovery);
}

QString CarryReason(const QString &reason) {
	return reason.isEmpty() ? QString()
		: u"\n"_q + tr::lng_purple_chat_setup_reason(tr::now).arg(reason);
}

QString CarryText(const DesktopSyncChatCarryState &state) {
	using Kind = DesktopSyncChatCarryKind;
	const auto &reason = state.reason;
	if (state.kind == Kind::Mark) {
		return state.removed ? tr::lng_purple_chat_remove_mark_done(tr::now)
			: state.running ? tr::lng_purple_chat_remove_mark_running(tr::now)
			: reason == u"mark-transport-held"_q ? tr::lng_purple_chat_carry_unavailable(tr::now)
			: tr::lng_purple_chat_remove_mark_stopped(tr::now) + CarryReason(reason);
	}
	const auto &carry = state.carry;
	const auto phase = carry && carry->journal
		? std::make_optional(carry->journal->phase) : std::nullopt;
	if (state.kind == Kind::None || (carry && carry->absent)) {
		// A startup resume that found nothing says nothing; an explicit Resume says so.
		return (state.kind == Kind::None || state.window.isEmpty()) ? QString()
			: tr::lng_purple_chat_carry_no_journal(tr::now);
	} else if (carry && carry->cleanup.pending) {
		return tr::lng_purple_chat_cleanup_pending(tr::now);
	} else if (phase == SyncCarryPhase::CleanupSkipped) {
		return tr::lng_purple_chat_cleanup_skipped(tr::now);
	} else if (phase == SyncCarryPhase::Done) {
		return state.kind == Kind::Follow ? tr::lng_purple_chat_follow_done(tr::now)
			: state.kind == Kind::Recovery ? tr::lng_purple_chat_recovery_done(tr::now)
			: tr::lng_purple_chat_move_complete(tr::now);
	} else if (reason == u"sync-paused-here"_q) {
		return tr::lng_purple_chat_paused_here(tr::now);
	} else if (reason == u"carry-retry-required"_q) {
		return tr::lng_purple_chat_move_retry(tr::now);
	} else if (reason == u"carry-journal-unreadable"_q) {
		return tr::lng_purple_chat_carry_unreadable(tr::now);
	} else if (reason == u"carry-writer-busy"_q) {
		return tr::lng_purple_chat_carry_busy(tr::now);
	} else if (reason == u"carry-transport-held"_q || reason == u"carry-unavailable"_q
			|| reason == u"cleanup-held"_q) {
		return tr::lng_purple_chat_carry_unavailable(tr::now);
	} else if (reason == u"mark-winner-held"_q || reason == u"recovery-own-mark-held"_q) {
		return tr::lng_purple_chat_carry_winner_held(tr::now);
	} else if (state.running) {
		return state.provisioned ? tr::lng_purple_chat_move_provisioned(tr::now)
			: tr::lng_purple_chat_carry_running(tr::now);
	} else if (!state.finished) {
		// Held, not stopped: the operation is still this window's, and Retry checks it again.
		return tr::lng_purple_chat_move_retry(tr::now) + CarryReason(reason);
	}
	return tr::lng_purple_chat_carry_stopped(tr::now) + CarryReason(reason);
}

struct Choice {
	DesktopSyncChatSetupSelection selection;
	QString title;
};

struct SetupState {
	base::weak_ptr<Main::Session> session;
	Fn<bool()> current;
	std::optional<AccountSettingsOpenTarget> target;
	std::optional<DesktopSyncChatSetupSnapshot> opening;
	std::shared_ptr<Ui::RadiobuttonGroup> group;
	std::vector<Choice> choices;
	std::vector<PeerToken> pending;
	std::unique_ptr<DesktopSyncChatPeerRequest> request;
	std::unique_ptr<DesktopSyncChatSetupService> service;
	std::unique_ptr<DesktopSyncChatMoveAction> move;
	std::unique_ptr<DesktopSyncChatConsentAction> follow;
	std::optional<DesktopSyncChatRecoveryChoice> recovery;
	std::optional<DesktopSyncChatCarryState> carry;
	QString windowId;
	QPointer<QWidget> window;
	DesktopSyncChatSetupProgress progress;
	Ui::RoundButton *retry = nullptr;
	Ui::RoundButton *self = nullptr;
	Ui::RoundButton *consent = nullptr;
	Ui::RoundButton *check = nullptr;
	Ui::RoundButton *cleanup = nullptr;
	Ui::RoundButton *resume = nullptr;
	Ui::FlatLabel *status = nullptr;
	Ui::RoundButton *accept = nullptr;
	size_t next = 0;
	uint64_t serviceGeneration = 0;
	bool busy = false;
	bool loading = false;
};

bool MoveContextCurrent(const SetupState &state) {
	const auto session = state.session.get();
	return session && state.current && state.current() && state.target && state.opening
		&& state.window && state.window->isActiveWindow()
		&& QGuiApplication::applicationState() == Qt::ApplicationActive
		&& !Core::App().passcodeLocked()
		&& ValidateAccountSettingsOpenTarget(session, *state.target)
		&& CachedSyncChatSetupContextCurrent(session, state.opening->scope);
}

bool Current(const SetupState &state) {
	const auto session = state.session.get();
	const auto fresh = session ? SyncChatSetupSnapshotOf(session) : std::nullopt;
	if (state.move || (state.opening && DesktopSyncChatRecoveryHeld(*state.opening).isEmpty())) {
		return MoveContextCurrent(state) && fresh && state.opening
			&& fresh->scope == state.opening->scope
			&& DesktopSyncChatMoveFingerprint(*fresh)
				== DesktopSyncChatMoveFingerprint(*state.opening);
	}
	return session && state.current && state.current() && state.target
		&& ValidateAccountSettingsOpenTarget(session, *state.target)
		&& state.opening && (state.service
			? SyncChatSetupContextCurrent(session, state.opening->scope)
			: fresh && fresh->run == state.opening->run
				&& fresh->scope == state.opening->scope);
}

DesktopSyncChatCarryOwner *CarryOwner(const SetupState &state) {
	const auto session = state.session.get();
	return (session && CarryOn()) ? &SyncChatCarry(session).owner() : nullptr;
}

// A box shows and controls the operations it started, and a resume started at startup.
bool OwnsCarry(const SetupState &state, const DesktopSyncChatCarryState &value) {
	return value.kind != DesktopSyncChatCarryKind::None && (value.window == state.windowId
		|| (value.kind == DesktopSyncChatCarryKind::Resume && value.window.isEmpty()));
}

bool CarryRunning(const SetupState &state) {
	const auto owner = CarryOwner(state);
	return owner && owner->busy();
}

void UpdateCarryButtons(SetupState *state) {
	const auto owner = CarryOwner(*state);
	const auto value = owner ? owner->state() : DesktopSyncChatCarryState();
	const auto own = owner && owner->busy() && OwnsCarry(*state, value);
	const auto latest = owner ? owner->latest() : std::nullopt;
	if (state->cleanup) {
		state->cleanup->setVisible(own && latest && latest->cleanup.pending && latest->cleanup.canAct);
	}
	if (state->resume) {
		state->resume->setVisible(owner && !owner->busy() && latest && latest->journal
			&& !latest->terminal && !latest->absent);
	}
	if (state->retry && own) { state->retry->setDisabled(false); }
	if (state->accept && owner && owner->busy()) { state->accept->setDisabled(true); }
}

void ShowCarry(SetupState *state, const DesktopSyncChatCarryState &value) {
	if (OwnsCarry(*state, value)) {
		if (const auto text = CarryText(value); !text.isEmpty()) {
			state->carry = value;
			state->status->setText(text);
		}
	}
	if (state->accept && value.finished && !state->service && !state->busy) {
		state->accept->setDisabled(!Current(*state));
	}
	UpdateCarryButtons(state);
}

bool StartMove(SetupState *state, const DesktopSyncChatMoveDraft &draft) {
	const auto owner = CarryOwner(*state);
	if (!owner) { return false; }
	if (!state->recovery) { return owner->move(draft, state->windowId); }
	// Lost recovery creates or chooses through the same review, under its own switch.
	auto selection = ReviewSyncChatRecovery(state->session.get(), *state->opening, *state->recovery,
		draft.selection.peer, state->windowId, true);
	return selection && owner->recover(std::move(*selection));
}

void OpenCleanup(not_null<Ui::GenericBox*> box, SetupState *state) {
	const auto owner = CarryOwner(*state);
	const auto opening = owner ? owner->latest() : std::nullopt;
	if (!DesktopSyncChatCleanupActivated() || !owner || !opening || !owner->busy()
		|| !OwnsCarry(*state, owner->state())) { return; }
	const auto session = state->session;
	const auto generation = owner->state().generation;
	const auto live = [session, generation]() -> DesktopSyncChatCarryOwner* {
		const auto strong = session.get();
		if (!strong || !CarryOn()) { return nullptr; }
		auto &owner = SyncChatCarry(strong).owner();
		return (owner.busy() && owner.state().generation == generation) ? &owner : nullptr;
	};
	box->uiShow()->showBox(Box(SyncChatCleanupBox, *opening,
		[=] { const auto owner = live(); return owner ? owner->latest() : std::nullopt; },
		[=] { return live() != nullptr; },
		[=] { if (const auto owner = live()) { owner->retry(); } },
		[=](bool confirmed) { if (const auto owner = live()) { owner->skipCleanup(confirmed); } }));
}

void LoadNext(not_null<Ui::GenericBox*> box, SetupState *state);

bool RestoreFreshSetup(SetupState *state, uint64_t generation, const QString &message) {
	if (!state->service || state->serviceGeneration != generation
		|| !state->progress.resumeAbsent) {
		return false;
	}
	++state->serviceGeneration;
	state->service.reset();
	state->progress = {};
	state->busy = false;
	state->status->setText(message);
	if (state->accept) { state->accept->setDisabled(!Current(*state)); }
	if (state->check) { state->check->setDisabled(false); }
	if (state->retry) { state->retry->setDisabled(!Current(*state)); }
	if (state->self) { state->self->hide(); }
	if (state->consent) { state->consent->hide(); }
	return true;
}

void SetupProgress(not_null<Ui::GenericBox*> box, SetupState *state,
		DesktopSyncChatSetupProgress progress) {
	state->progress = std::move(progress);
	if (state->progress.resumeAbsent) {
		const auto generation = state->serviceGeneration;
		const auto message = tr::lng_purple_chat_setup_no_journal(tr::now);
		state->status->setText(message);
		crl::on_main(box, [=] {
			if (RestoreFreshSetup(state, generation, message)) { LoadNext(box, state); }
		});
		return;
	}
	state->busy = state->progress.phase == DesktopSyncChatCreatePhase::Running;
	auto text = state->progress.provisioned
		? tr::lng_purple_chat_move_provisioned(tr::now)
		: state->progress.phase == DesktopSyncChatCreatePhase::Complete
		? tr::lng_purple_chat_setup_complete(tr::now)
		: state->progress.phase == DesktopSyncChatCreatePhase::Cancelled
		? tr::lng_purple_chat_setup_cancelled(tr::now)
		: state->progress.phase == DesktopSyncChatCreatePhase::Limit
		? tr::lng_purple_chat_setup_limit(tr::now)
		: state->busy ? tr::lng_purple_chat_setup_running(tr::now)
		: tr::lng_purple_chat_setup_held(tr::now);
	if (!state->progress.reason.isEmpty()) {
		text += u"\n"_q + tr::lng_purple_chat_setup_reason(tr::now).arg(state->progress.reason);
	}
	if (state->progress.reason == u"setup-resume-required"_q && state->progress.journal) {
		text += u"\n"_q + tr::lng_purple_chat_setup_resume(tr::now).arg(
			state->progress.journal->chat && state->session
				? Title(*state->session.get(), *state->progress.journal->chat)
				: tr::lng_purple_chat_create(tr::now));
	}
	if (state->session) {
		if (const auto pending = PendingSyncRequest(&state->session->mtp())) {
			text += u"\n"_q + SyncRequestWaitText(*pending);
		}
	}
	if (state->progress.journal && state->progress.journal->retryAt > 0) {
		text += u"\n"_q + SyncRequestWaitText({ SyncTransientKind::Flood,
			state->progress.journal->retryAt });
	}
	if (state->progress.journal && state->progress.journal->newReaders) {
		text += u"\n"_q + tr::lng_purple_chat_new_readers(tr::now);
	}
	state->status->setText(std::move(text));
	if (state->check) { state->check->setDisabled(state->service != nullptr); }
	if (state->retry) { state->retry->show(); state->retry->setDisabled(state->busy || !Current(*state)
		|| state->progress.phase == DesktopSyncChatCreatePhase::Complete); }
	if (state->self) { state->self->setVisible(state->progress.phase == DesktopSyncChatCreatePhase::Limit); state->self->setDisabled(!Current(*state)
		|| state->progress.phase != DesktopSyncChatCreatePhase::Limit); }
	if (state->consent) { state->consent->setVisible(state->progress.consent.has_value()); state->consent->setDisabled(!Current(*state) || !state->progress.consent); }
	if (state->accept) { state->accept->setDisabled(state->service || !Current(*state)); }
}

void LoadNext(not_null<Ui::GenericBox*> box, SetupState *state) {
	if (state->service || state->loading || !Current(*state) || state->next >= state->pending.size()) {
		return;
	}
	const auto chat = state->pending[state->next++];
	state->loading = true;
	const auto weak = QPointer<Ui::GenericBox>(box.get());
	state->request = std::make_unique<DesktopSyncChatPeerRequest>(
		state->session.get(), state->opening->scope.origin, chat,
		[weak, state] { return weak && Current(*state); },
		crl::guard(box, [=](std::optional<DesktopSyncChatPeer> peer) {
			state->loading = false;
			if (!Current(*state)) {
				return;
			}
			const auto title = Title(*state->session.get(), chat);
			const auto eligible = peer && (state->move
				? DesktopSyncChatMoveEligibility(*state->opening, *peer)
				: DesktopSyncChatSetupEligibility(*state->opening, *peer))
				== DesktopSyncChatSetupRefusal::None;
			const auto index = int(state->choices.size());
			state->choices.push_back({ { chat == PeerToken{}
				? DesktopSyncChatSetupChoice::Self
				: DesktopSyncChatSetupChoice::Existing, peer, false }, title });
			const auto predicate = peer ? DesktopSyncChatEligibility(*peer)
				: std::nullopt;
			const auto label = eligible ? title : title + u": "_q
				+ (predicate && !*predicate ? Refusal(predicate->refusal)
					: tr::lng_purple_chat_refuse_unknown(tr::now));
			const auto row = box->addRow(object_ptr<Ui::Radiobutton>(box,
				state->group, index, label, st::defaultCheckbox));
			row->setDisabled(!eligible);
			crl::on_main(box, [=] { LoadNext(box, state); });
		}));
}

void ConfirmStaleMark(not_null<Ui::GenericBox*> box, SetupState *state, DesktopSyncChatStaleMark row) {
	if (!DesktopSyncChatMarkRemovalActivated() || !Current(*state) || state->busy) { return; }
	const auto weak = QPointer<Ui::GenericBox>(box.get());
	box->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> confirmation) {
		if (!DesktopSyncChatMarkRemovalActivated() || !weak || !Current(*state)) { confirmation->closeBox(); return; }
		confirmation->setTitle(tr::lng_purple_chat_remove_mark());
		confirmation->addRow(object_ptr<Ui::FlatLabel>(confirmation,
			tr::lng_purple_chat_remove_mark_disclosure(tr::now).arg(Title(*state->session.get(), row.location.chat))
				+ u"\n\n"_q + row.text, st::boxLabel));
		confirmation->addButton(tr::lng_purple_chat_remove_mark(), [=] {
			confirmation->closeBox();
			if (!DesktopSyncChatMarkRemovalActivated() || !weak || !Current(*state) || state->busy) { return; }
			const auto current = [weak, state] { return weak && Current(*state) && !state->busy; };
			const auto reviewed = ReviewSyncChatStaleMark(state->session.get(), row, current, [=](DesktopSyncChatStaleMark value) {
				const auto owner = CarryOwner(*state);
				const auto opening = SyncChatSetupSnapshotOf(state->session.get());
				if (!owner || !opening || !owner->removeMark(std::move(value), state->windowId, *opening)) {
					state->status->setText(tr::lng_purple_chat_carry_busy(tr::now));
				}
			});
			if (!reviewed) { state->status->setText(tr::lng_purple_chat_stale(tr::now)); }
		});
		confirmation->addButton(tr::lng_cancel(), [=] { confirmation->closeBox(); });
	}));
}

void AddStaleMarkRows(not_null<Ui::GenericBox*> box, SetupState *state) {
	if (!DesktopSyncChatMarkRemovalActivated() || !Current(*state)) { return; }
	for (const auto &row : SyncChatStaleMarksOf(state->session.get())) {
		const auto button = box->addRow(object_ptr<Ui::RoundButton>(box,
			rpl::single(tr::lng_purple_chat_remove_mark_row(tr::now).arg(Title(*state->session.get(), row.location.chat))), st::defaultBoxButton));
		button->setClickedCallback([=] {
			if (!DesktopSyncChatMarkRemovalActivated() || !Current(*state)) { return; }
			ConfirmStaleMark(box, state, row);
		});
	}
}

// Cleanup choices appear only where they are sent: an ordinary move with Cleanup ON. Lost
// recovery leaves the lost chat alone.
bool OffersCleanup(const SetupState &state) {
	return !state.recovery && DesktopSyncChatCleanupActivated();
}

void HideCleanupChoices(const SetupState &state, Ui::Checkbox *removeMark, not_null<Ui::Checkbox*> cleanup) {
	if (OffersCleanup(state)) { return; }
	if (removeMark) { removeMark->hide(); }
	cleanup->hide();
}

void AcceptMove(not_null<Ui::GenericBox*> box, SetupState *state) {
	if (!state->move || !Current(*state) || state->busy) { return; }
	const auto index = state->group->current();
	if (index < 0 || index >= int(state->choices.size())) { return; }
	const auto chosen = state->choices[index];
	const auto offer = state->move->choose(chosen.selection);
	if (!offer) {
		state->status->setText(tr::lng_purple_chat_move_stale(tr::now));
		return;
	}
	const auto disclosure = offer->selection.peer
		? Readers(DesktopSyncChatEligibility(*offer->selection.peer)->readers, chosen.title)
		: tr::lng_purple_chat_create_disclosure(tr::now)
			+ u"\n"_q + SettingsDisclosure(chosen.title);
	const auto weak = QPointer<Ui::GenericBox>(box.get());
	box->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> confirmation) {
		const auto submitted = std::make_shared<bool>(false);
		confirmation->boxClosing() | rpl::on_next([=] {
			if (!*submitted && weak && state->move) { state->move->dismiss(offer->ticket); }
		}, confirmation->lifetime());
		confirmation->setTitle(tr::lng_purple_chat_move_title());
		confirmation->setWidth(st::boxWideWidth);
		confirmation->addRow(object_ptr<Ui::FlatLabel>(confirmation,
			tr::lng_purple_chat_move_disclosure(tr::now).arg(
				Title(*state->session.get(), state->opening->scope.settingsChat), chosen.title)
				+ u"\n\n"_q + disclosure, st::boxLabel));
		const auto removeMark = DesktopSyncChatCleanupActivated()
			? confirmation->addRow(object_ptr<Ui::Checkbox>(confirmation,
				tr::lng_purple_chat_cleanup_remove_mark(tr::now), false, st::defaultCheckbox))
			: nullptr;
		if (removeMark) {
			removeMark->setDisabled(!state->opening->scope.mark || state->opening->scope.mark->markId<=0);
		}
		const auto cleanup = confirmation->addRow(object_ptr<Ui::Checkbox>(confirmation,
			tr::lng_purple_chat_move_cleanup(tr::now), offer->cleanup, st::defaultCheckbox));
		if (OffersCleanup(*state) && state->opening->scope.settingsChat == PeerToken{}) {
			confirmation->addRow(object_ptr<Ui::FlatLabel>(confirmation,
				tr::lng_purple_chat_move_cleanup_self_warning(tr::now), st::boxLabel));
		}
		HideCleanupChoices(*state, removeMark, cleanup);
		confirmation->addButton(tr::lng_continue(), [=] {
			const auto removeOld = OffersCleanup(*state) && cleanup->checked();
			const auto oldMark = removeMark && removeMark->checked();
			*submitted = true;
			confirmation->closeBox();
			if (!weak || !Current(*state) || state->busy) { return; }
			state->busy = true;
			state->accept->setDisabled(true);
			const auto finish = crl::guard(box, [=](std::optional<DesktopSyncChatPeer> peer) {
				state->busy = false;
				auto draft = std::optional<DesktopSyncChatMoveDraft>();
				const auto accepted = Current(*state) && ReviewSyncChatMove(
					state->session.get(), *state->move, *offer, std::move(peer), removeOld, true, oldMark,
					[&](DesktopSyncChatMoveDraft value) { draft = std::move(value); });
				// Move OFF holds an ordinary move at review; Lost recovery has its own switch.
				if (accepted && !draft && state->recovery) { draft = state->move->reviewed(); }
				if (!accepted || !draft || !StartMove(state, *draft)) {
					state->status->setText(!accepted ? tr::lng_purple_chat_move_stale(tr::now)
						: !draft ? MoveNotice(DesktopSyncChatMoveNotice::Held)
						: state->recovery ? tr::lng_purple_chat_recovery_refused(tr::now)
						: tr::lng_purple_chat_carry_busy(tr::now));
				}
				state->accept->setDisabled(!Current(*state) || CarryRunning(*state));
			});
			const auto context = [weak, state] { return weak && MoveContextCurrent(*state); };
			CheckSyncChatBeforePost(state->session.get(), crl::guard(box,
				[=](DesktopSyncChatStatus status) {
					if (!context() || !status.complete || !Current(*state)) {
						state->busy = false;
						state->accept->setDisabled(!Current(*state));
						state->status->setText(tr::lng_purple_chat_move_stale(tr::now));
						return;
					}
					if (offer->selection.choice == DesktopSyncChatSetupChoice::Create) {
						finish(std::optional<DesktopSyncChatPeer>());
					} else {
						state->request = std::make_unique<DesktopSyncChatPeerRequest>(
							state->session.get(), state->opening->scope.origin,
							offer->selection.peer->chat, context, finish);
					}
				}), DesktopSyncChatOperation{.current = context});
		});
		confirmation->addButton(tr::lng_cancel(), [=] {
			if (weak && state->move) { state->move->dismiss(offer->ticket); }
			confirmation->closeBox();
		});
	}));
}

void Accept(not_null<Ui::GenericBox*> box, SetupState *state) {
	if (state->move) { AcceptMove(box, state); return; }
	if (!Current(*state) || state->busy || state->service) {
		state->status->setText(tr::lng_purple_chat_stale(tr::now));
		return;
	}
	const auto index = state->group->current();
	if (index < 0 || index >= int(state->choices.size())) {
		return;
	}
	state->busy = true;
	state->accept->setDisabled(true);
	const auto chosen = state->choices[index];
	const auto confirm = [=](std::optional<DesktopSyncChatPeer> peer) {
		state->busy = false;
		state->accept->setDisabled(false);
		if (!Current(*state)) {
			state->status->setText(tr::lng_purple_chat_stale(tr::now));
			return;
		}
		auto selection = chosen.selection;
		selection.peer = peer;
		const auto snapshot = *SyncChatSetupSnapshotOf(state->session.get());
		const auto result = AcceptDesktopSyncChatSetup(
			*state->opening, snapshot, selection);
		if (result != DesktopSyncChatSetupRefusal::None
			&& result != DesktopSyncChatSetupRefusal::ConsentRequired) {
			state->status->setText(tr::lng_purple_chat_refuse_unknown(tr::now));
			return;
		}
		const auto disclosure = peer
			? Readers(DesktopSyncChatEligibility(*peer)->readers, chosen.title)
			: tr::lng_purple_chat_create_disclosure(tr::now)
				+ u"\n"_q + SettingsDisclosure(chosen.title);
		box->uiShow()->showBox(Ui::MakeConfirmBox({
			.text = chosen.title + u"\n\n"_q + disclosure,
			.confirmed = crl::guard(box, [=](Fn<void()> close) {
				close();
				if (!Current(*state) || state->busy || state->service) {
					state->status->setText(tr::lng_purple_chat_stale(tr::now));
					return;
				}
				state->busy = true;
				state->accept->setDisabled(true);
				const auto finish = crl::guard(box,
					[=](std::optional<DesktopSyncChatPeer> acceptedPeer) {
						state->busy = false;
						const auto fresh = state->session
							? SyncChatSetupSnapshotOf(state->session.get()) : std::nullopt;
						const auto same = !peer || (acceptedPeer
							&& SameDesktopSyncChatDisclosure(*peer, *acceptedPeer));
						auto accepted = selection;
						accepted.peer = acceptedPeer;
						accepted.readerConsent = true;
						if (!Current(*state) || !fresh || !same
							|| AcceptDesktopSyncChatSetup(*state->opening,
								*fresh, accepted) != DesktopSyncChatSetupRefusal::None) {
							state->accept->setDisabled(!Current(*state));
							state->status->setText(tr::lng_purple_chat_stale(tr::now));
							return;
						}
						++state->serviceGeneration;
						state->service = std::make_unique<DesktopSyncChatSetupService>(
							state->session.get(), *fresh, accepted,
							[weak = QPointer<Ui::GenericBox>(box.get()), state] {
								return weak && state->session && state->current && state->current()
									&& state->target && ValidateAccountSettingsOpenTarget(
										state->session.get(), *state->target);
							}, crl::guard(box, [=](DesktopSyncChatSetupProgress progress) {
								SetupProgress(box, state, std::move(progress));
							}));
						state->request.reset();
						state->service->start();
					});
				if (selection.choice == DesktopSyncChatSetupChoice::Create) {
					finish(std::optional<DesktopSyncChatPeer>());
				} else {
					const auto weak = QPointer<Ui::GenericBox>(box.get());
					state->request = std::make_unique<DesktopSyncChatPeerRequest>(
						state->session.get(), state->opening->scope.origin,
						selection.peer->chat,
						[weak, state] { return weak && Current(*state); }, finish);
				}
			}),
			.confirmText = tr::lng_continue(),
		}));
	};
	if (chosen.selection.choice == DesktopSyncChatSetupChoice::Create) {
		confirm({});
	} else {
		const auto weak = QPointer<Ui::GenericBox>(box.get());
		state->request = std::make_unique<DesktopSyncChatPeerRequest>(
			state->session.get(), state->opening->scope.origin,
			chosen.selection.peer->chat,
			[weak, state] { return weak && Current(*state); },
			crl::guard(box, confirm));
	}
}

void BuildSetupBox(not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session, Fn<bool()> current, bool moving,
	std::optional<DesktopSyncChatRecoveryChoice> recovery = std::nullopt);

void MoveBox(not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session, Fn<bool()> current) {
	BuildSetupBox(box, session, std::move(current), true);
}

void RecoveryMoveBox(not_null<Ui::GenericBox*> box, not_null<Main::Session*> session,
		Fn<bool()> current, DesktopSyncChatRecoveryChoice choice) {
	if (!DesktopSyncChatRecoveryActivated()) { box->closeBox(); return; }
	BuildSetupBox(box, session, std::move(current), true, choice);
}

QString RecoveryTitle(DesktopSyncChatRecoveryChoice choice) {
	switch (choice) {
	case DesktopSyncChatRecoveryChoice::Create: return tr::lng_purple_chat_recovery_create(tr::now);
	case DesktopSyncChatRecoveryChoice::Choose: return tr::lng_purple_chat_recovery_choose(tr::now);
	case DesktopSyncChatRecoveryChoice::RepairMark: return tr::lng_purple_chat_recovery_repair(tr::now);
	}
	return tr::lng_purple_chat_move_held(tr::now);
}

void BuildRecoveryConfirmation(not_null<Ui::GenericBox*> confirmation, QString disclosure, Fn<void()> proceed) {
	confirmation->setTitle(tr::lng_purple_chat_recovery_repair());
	if (!DesktopSyncChatRecoveryActivated()) {
		confirmation->addButton(tr::lng_close(), [=] { confirmation->closeBox(); });
		return;
	}
	confirmation->addRow(object_ptr<Ui::FlatLabel>(confirmation, disclosure, st::boxLabel));
	confirmation->addButton(tr::lng_continue(), [=] {
		confirmation->closeBox();
		if (DesktopSyncChatRecoveryActivated() && proceed) { proceed(); }
	});
	confirmation->addButton(tr::lng_cancel(), [=] { confirmation->closeBox(); });
}

void ConfirmRecovery(not_null<Ui::GenericBox*> box, SetupState *state, DesktopSyncChatRecoveryChoice choice) {
	if (!DesktopSyncChatRecoveryActivated() || !Current(*state) || !MoveContextCurrent(*state)
		|| state->busy || !state->opening || !DesktopSyncChatRecoveryHeld(*state->opening).isEmpty()) { return; }
	if (choice != DesktopSyncChatRecoveryChoice::RepairMark) {
		CheckSyncChatBeforePost(state->session.get(), crl::guard(box, [=](DesktopSyncChatStatus) {
			if (!DesktopSyncChatRecoveryActivated() || !Current(*state) || !MoveContextCurrent(*state)) { return; }
			box->uiShow()->showBox(Box(RecoveryMoveBox, state->session.get(), state->current, choice));
		}), DesktopSyncChatOperation{.current = [weak = QPointer<Ui::GenericBox>(box.get()), state] {
			return weak && DesktopSyncChatRecoveryActivated() && MoveContextCurrent(*state);
		}});
		return;
	}
	if (state->opening->input.cacheRead != SyncChatDirectRead::MarkGone) {
		state->status->setText(RecoveryReason(state->opening->input.cacheRead == SyncChatDirectRead::ChatGone
			? u"recovery-chat-gone"_q : u"recovery-mark-unproven"_q));
		return;
	}
	const auto weak = QPointer<Ui::GenericBox>(box.get());
	state->request = std::make_unique<DesktopSyncChatPeerRequest>(state->session.get(), state->opening->scope.origin,
		state->opening->scope.settingsChat, [weak, state] { return weak && DesktopSyncChatRecoveryActivated() && MoveContextCurrent(*state); },
		crl::guard(box, [=](std::optional<DesktopSyncChatPeer> peer) {
			if (!DesktopSyncChatRecoveryActivated() || !Current(*state)) { return; }
			if (!peer) { state->status->setText(RecoveryReason(u"recovery-readers-held"_q)); return; }
			const auto held = DesktopSyncChatRepairHeld(*state->opening, *peer);
			if (!held.isEmpty()) { state->status->setText(RecoveryReason(held)); return; }
			box->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> confirmation) {
				BuildRecoveryConfirmation(confirmation, tr::lng_purple_chat_recovery_repair_disclosure(tr::now)
					+ u"\n"_q + Readers(DesktopSyncChatEligibility(*peer)->readers, Title(*state->session.get(), peer->chat)), [=] {
					if (!weak || !DesktopSyncChatRecoveryActivated() || !Current(*state) || !MoveContextCurrent(*state)) { return; }
					CheckSyncChatBeforePost(state->session.get(), crl::guard(box, [=](DesktopSyncChatStatus) {
						if (!DesktopSyncChatRecoveryActivated() || !Current(*state) || !MoveContextCurrent(*state)) { return; }
						auto accepted = ReviewSyncChatRecovery(state->session.get(), *state->opening, choice, peer,
							state->windowId, true);
						const auto owner = CarryOwner(*state);
						if (!accepted || !owner || !owner->recover(std::move(*accepted))) {
							state->status->setText(accepted ? tr::lng_purple_chat_carry_busy(tr::now)
								: tr::lng_purple_chat_recovery_refused(tr::now));
						}
					}), DesktopSyncChatOperation{.current = [weak, state] { return weak && MoveContextCurrent(*state); }});
				});
			}));
		}));
}

void AddRecoveryRows(not_null<Ui::GenericBox*> box, SetupState *state) {
	if (!state->opening || !DesktopSyncChatRecoveryActivated()) { return; }
	for (const auto choice : DesktopSyncChatRecoveryChoices(*state->opening)) {
		box->addButton(rpl::single(RecoveryTitle(choice)), [=] {
			if (!DesktopSyncChatRecoveryActivated() || !Current(*state) || !MoveContextCurrent(*state)) { return; }
			ConfirmRecovery(box, state, choice);
		});
	}
	if (DesktopSyncChatRecoveryHeld(*state->opening).isEmpty()) {
		box->addButton(tr::lng_purple_chat_recovery_retry(), [=] {
			if (!DesktopSyncChatRecoveryActivated() || !Current(*state) || !MoveContextCurrent(*state)) { return; }
			EnsureSyncChatResolved(state->session.get(), true);
			box->closeBox();
		});
	}
}

void OfferFollow(not_null<Ui::GenericBox*> box, SetupState *state) {
	if (!DesktopSyncChatFollowFillActivated() || !state->follow || !Current(*state) || state->busy) { return; }
	const auto &winner = state->opening->status.resolution.winner;
	if (!winner) { state->status->setText(tr::lng_purple_chat_stale(tr::now)); return; }
	const auto weak = QPointer<Ui::GenericBox>(box.get());
	state->request = std::make_unique<DesktopSyncChatPeerRequest>(state->session.get(), state->opening->scope.origin,
		winner->chat, [weak, state] { return weak && Current(*state); },
		crl::guard(box, [=](std::optional<DesktopSyncChatPeer> peer) {
			const auto offer = (peer && state->follow && Current(*state))
				? state->follow->offer(*peer, u"config"_q) : std::nullopt;
			if (!offer) { state->status->setText(tr::lng_purple_chat_stale(tr::now)); return; }
			const auto title = Title(*state->session.get(), peer->chat);
			box->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> confirmation) {
				confirmation->setTitle(tr::lng_purple_chat_title());
				confirmation->setWidth(st::boxWideWidth);
				const auto eligible = DesktopSyncChatEligibility(offer->peer);
				confirmation->addRow(object_ptr<Ui::FlatLabel>(confirmation,
					(offer->askReaders && eligible ? ReaderCount(eligible->readers) + u"\n"_q : QString())
						+ FollowFillDisclosure(*offer, title), st::boxLabel));
				const auto choose = [=](DesktopSyncChatConsentChoice choice) {
					confirmation->closeBox();
					if (!weak || !state->follow || !Current(*state)) { return; }
					auto selection = ReviewSyncChatFollowFill(state->session.get(), *state->follow, *offer, *peer,
						choice, true);
					const auto owner = CarryOwner(*state);
					if (!selection || !owner || !owner->follow(std::move(*selection))) {
						state->status->setText(selection ? tr::lng_purple_chat_carry_busy(tr::now)
							: tr::lng_purple_chat_stale(tr::now));
					}
				};
				confirmation->addButton(tr::lng_purple_chat_follow_action(), [=] {
					choose(DesktopSyncChatConsentChoice::Proceed);
				});
				confirmation->addButton(tr::lng_purple_chat_pause_here_action(), [=] {
					choose(DesktopSyncChatConsentChoice::Pause);
				});
				confirmation->addButton(tr::lng_cancel(), [=] { confirmation->closeBox(); });
			}));
		}));
}

// Follow and Fill offer the resolved winner with its disclosure, or Pause sync here.
void AddFollowRows(not_null<Ui::GenericBox*> box, SetupState *state) {
	if (!DesktopSyncChatFollowFillActivated() || !state->opening || !Current(*state)) { return; }
	const auto action = state->opening->status.resolution.action;
	if (action != SyncChatAction::Follow && action != SyncChatAction::Fill) { return; }
	const auto weak = QPointer<Ui::GenericBox>(box.get());
	state->follow = std::make_unique<DesktopSyncChatConsentAction>(*state->opening, state->windowId,
		[weak, state] { return weak && Current(*state); },
		[weak, state] {
			return weak && state->window && state->window->isActiveWindow()
				&& QGuiApplication::applicationState() == Qt::ApplicationActive
				&& !Core::App().passcodeLocked();
		});
	box->addButton(tr::lng_purple_chat_follow_action(), [=] { OfferFollow(box, state); });
}

void BuildSetupBox(not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session, Fn<bool()> current, bool moving,
		std::optional<DesktopSyncChatRecoveryChoice> recovery) {
	box->setTitle(tr::lng_purple_chat_title());
	box->setWidth(st::boxWideWidth);
	const auto state = box->lifetime().make_state<SetupState>();
	state->session = session;
	state->current = std::move(current);
	state->window = box->window();
	state->target = AccountSettingsOpenTargetFor(session, SettingsOpenRole::Own);
	state->opening = SyncChatSetupSnapshotOf(session);
	state->recovery = recovery;
	static auto Windows = uint64_t();
	state->windowId = u"setup-"_q + QString::number(++Windows);
	if (moving && state->opening && DesktopSyncChatRecoveryHeld(*state->opening).isEmpty()
		&& !DesktopSyncChatRecoveryActivated()) {
		state->status = box->addRow(object_ptr<Ui::FlatLabel>(box, tr::lng_purple_chat_recovery_off(tr::now), st::boxLabel));
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		return;
	}
	if (moving && state->opening) {
		const auto weak = QPointer<Ui::GenericBox>(box.get());
		state->move = std::make_unique<DesktopSyncChatMoveAction>(*state->opening,
			[weak, state] { return weak && Current(*state); });
		box->setTitle(tr::lng_purple_chat_move_title());
		QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged, box, [=] {
			if (!MoveContextCurrent(*state)) { state->move->cancel(); }
		});
		QObject::connect(qGuiApp, &QGuiApplication::focusWindowChanged, box, [=] {
			if (!MoveContextCurrent(*state)) { state->move->cancel(); }
		});
		Core::App().passcodeLockChanges() | rpl::on_next([=] {
			if (!MoveContextCurrent(*state)) { state->move->cancel(); }
		}, box->lifetime());
	}
	state->status = box->addRow(object_ptr<Ui::FlatLabel>(box,
		StatusText(*session, SyncChatStatusOf(session)), st::boxLabel));
	const auto refresh = [=] {
		if (state->carry && !state->service) {
			auto text = CarryText(*state->carry);
			if (state->carry->finished && Current(*state)) {
				text += u"\n\n"_q + StatusText(*session, SyncChatStatusOf(session));
			}
			state->status->setText(text);
		} else if (state->move && !state->recovery && state->move->reason() == u"move-qualification-held"_q) {
			state->status->setText(tr::lng_purple_chat_move_held(tr::now));
		} else if (!state->service) {
			state->status->setText(Current(*state)
				? StatusText(*session, SyncChatStatusOf(session))
				: tr::lng_purple_chat_stale(tr::now));
		} else {
			SetupProgress(box, state, state->progress);
		}
		if (state->accept) {
			state->accept->setDisabled(!Current(*state) || state->service
				|| state->busy || CarryRunning(*state));
		}
	};
	if (CarryOn()) {
		// The box owns what it starts: closing it stops that operation like any controller.
		SyncChatCarry(session).changes() | rpl::on_next([=](const DesktopSyncChatCarryState &value) {
			ShowCarry(state, value);
		}, box->lifetime());
		const auto closed = [weak = state->session, id = state->windowId] {
			const auto live = weak.get();
			if (live && CarryOn()) { SyncChatCarry(live).owner().closeWindow(id, u"window-closed"_q); }
		};
		box->boxClosing() | rpl::on_next(closed, box->lifetime());
		box->lifetime().add([=] { crl::on_main(closed); });
	}
	SyncRequestChanges() | rpl::on_next([=](MTP::Instance *instance) {
		if (state->session && &state->session->mtp() == instance) {
			refresh();
		}
	}, box->lifetime());
	SyncChatStatusChanges() | rpl::on_next([=](Main::Session *changed) {
		if (changed == state->session.get()) {
			refresh();
		}
	}, box->lifetime());
	SettingsChanges() | rpl::on_next(refresh, box->lifetime());
	StateChanges() | rpl::on_next(refresh, box->lifetime());
	if (state->opening) {
		const auto &snapshot = *state->opening;
		if (snapshot.status.newReaders && snapshot.cache) {
			box->addRow(object_ptr<Ui::FlatLabel>(box,
				tr::lng_purple_chat_original_readers(tr::now).arg(
					snapshot.cache->readers.otherMembers
						? QString::number(*snapshot.cache->readers.otherMembers)
						: tr::lng_purple_chat_unknown_count(tr::now),
					snapshot.cache->readers.otherAdmins
						? QString::number(*snapshot.cache->readers.otherAdmins)
						: tr::lng_purple_chat_unknown_count(tr::now)), st::boxLabel));
		}
		if (snapshot.status.resolution.readChat) {
			box->addRow(object_ptr<Ui::FlatLabel>(box,
				Readers(snapshot.status.resolution.readers,
					Title(*session, *snapshot.status.resolution.readChat)), st::boxLabel));
		}
		for (const auto &mark : snapshot.input.marks) {
			if (!mark.eligibility) {
				box->addRow(object_ptr<Ui::FlatLabel>(box,
					tr::lng_purple_chat_refused_mark(tr::now).arg(
						Title(*session, mark.location.chat), Refusal(mark.eligibility.refusal)),
					st::boxLabel));
			}
		}
	}
	if (Current(*state) && (moving
		? !DesktopSyncChatMoveFingerprint(*state->opening).isEmpty()
		: DesktopSyncChatFreshSetup(*state->opening))) {
		box->addRow(object_ptr<Ui::FlatLabel>(box,
			moving ? tr::lng_purple_chat_move_choices(tr::now)
				: tr::lng_purple_chat_fresh_choices(tr::now), st::boxLabel));
		state->group = std::make_shared<Ui::RadiobuttonGroup>();
		if (!recovery || *recovery != DesktopSyncChatRecoveryChoice::Choose) {
			state->choices.push_back({ { DesktopSyncChatSetupChoice::Create },
				tr::lng_purple_chat_create(tr::now) });
			box->addRow(object_ptr<Ui::Radiobutton>(box, state->group, 0,
				state->choices.front().title, st::defaultCheckbox));
			state->group->setValue(0);
		}
		if (!recovery || *recovery != DesktopSyncChatRecoveryChoice::Create) {
		state->pending.push_back({});
		for (const auto folder : {static_cast<Data::Folder*>(nullptr),
			session->data().folder(1).get()}) {
			for (const auto row : *session->data().chatsList(folder)->indexed()) {
				const auto history = row->key().history();
				const auto token = history ? DesktopSyncChatToken(
					peerToMTP(history->peer->id), session->userId().bare) : std::nullopt;
				if (token && std::find(state->pending.begin(), state->pending.end(),
					*token) == state->pending.end() && state->pending.size() < 51) {
					state->pending.push_back(*token);
				}
			}
		}
		}
		state->accept = box->addButton(tr::lng_continue(), [=] { Accept(box, state); });
		LoadNext(box, state);
	} else if (!moving && state->opening && state->opening->status.phase == DesktopSyncChatPhase::Use
		&& !DesktopSyncChatMoveFingerprint(*state->opening).isEmpty()) {
		box->addButton(tr::lng_purple_chat_move_title(), [=] {
			if (!Current(*state)) { return; }
			box->uiShow()->showBox(Box(MoveBox, session, state->current));
		});
	} else {
		box->addRow(object_ptr<Ui::FlatLabel>(box,
			tr::lng_purple_chat_change_disabled(tr::now), st::boxLabel));
	}
	if (!moving && state->opening && DesktopSyncChatRecoveryHeld(*state->opening).isEmpty()) {
		if (!DesktopSyncChatRecoveryActivated()) {
			box->addRow(object_ptr<Ui::FlatLabel>(box, tr::lng_purple_chat_recovery_off(tr::now), st::boxLabel));
		}
		AddRecoveryRows(box, state);
	}
	if (!moving) { AddFollowRows(box, state); }
	AddStaleMarkRows(box, state);
	state->retry = box->addRow(object_ptr<Ui::RoundButton>(box,
		tr::lng_purple_chat_setup_retry(), st::defaultBoxButton));
	state->retry->setClickedCallback([=] {
		// A running operation retries on its own authority: a committed move changes the
		// fingerprint this box opened with.
		if (const auto owner = CarryOwner(*state); owner && owner->busy() && OwnsCarry(*state, owner->state())) {
			owner->retry();
			return;
		}
		if (!Current(*state)) { return; }
		if (state->move) {
			if (DesktopSyncChatMoveActivated() || state->recovery) { refresh(); } else {
				state->status->setText(tr::lng_purple_chat_move_held(tr::now));
			}
			return;
		}
		if (state->service) { state->service->retry(); return; }
		if (state->loading) {
			state->loading = false;
			--state->next;
		}
		state->request.reset();
		++state->serviceGeneration;
		state->service = std::make_unique<DesktopSyncChatSetupService>(
			state->session.get(), *state->opening, DesktopSyncChatSetupSelection{},
			[weak = QPointer<Ui::GenericBox>(box.get()), state] {
				return weak && state->session && state->current && state->current()
					&& state->target && ValidateAccountSettingsOpenTarget(
						state->session.get(), *state->target);
			}, crl::guard(box, [=](DesktopSyncChatSetupProgress progress) {
				SetupProgress(box, state, std::move(progress));
			}), true);
		state->service->start();
	});
	state->retry->setDisabled(!Current(*state));
	if (DesktopSyncChatCleanupActivated()) {
		state->cleanup = box->addRow(object_ptr<Ui::RoundButton>(box,
			tr::lng_purple_chat_cleanup_title(), st::defaultBoxButton));
		state->cleanup->setClickedCallback([=] { OpenCleanup(box, state); });
		state->cleanup->hide();
	}
	if (ResumeOn()) {
		state->resume = box->addRow(object_ptr<Ui::RoundButton>(box,
			tr::lng_purple_chat_carry_resume(), st::defaultBoxButton));
		state->resume->setClickedCallback([=] {
			const auto owner = CarryOwner(*state);
			if (owner && !owner->resume(state->windowId)) {
				state->status->setText(tr::lng_purple_chat_carry_busy(tr::now));
			}
		});
		state->resume->hide();
	}
	if (const auto owner = CarryOwner(*state)) {
		if (owner->busy()) { ShowCarry(state, owner->state()); } else { UpdateCarryButtons(state); }
	}
	state->self = box->addRow(object_ptr<Ui::RoundButton>(box,
		tr::lng_purple_chat_setup_self(), st::defaultBoxButton));
	state->self->setClickedCallback([=] { if (state->service) { state->service->useSelf(); } });
	state->self->hide();
	state->consent = box->addRow(object_ptr<Ui::RoundButton>(box,
		tr::lng_purple_chat_setup_consent(), st::defaultBoxButton));
	state->consent->setClickedCallback([=] {
		if (!state->service || !state->progress.consent || !Current(*state)) { return; }
		const auto offer = *state->progress.consent;
		box->uiShow()->showBox(Ui::MakeConfirmBox({
			.text = Readers(DesktopSyncChatEligibility(offer.peer)->readers,
				Title(*state->session.get(), offer.peer.chat)),
			.confirmed = crl::guard(box, [=](Fn<void()> close) {
				close();
				if (state->service && Current(*state)) { state->service->reconsent(offer); }
			}),
			.confirmText = tr::lng_continue(),
		}));
	});
	state->consent->hide();
	state->check = box->addButton(tr::lng_purple_chat_check_again(), [=] {
		// Check again closes this box, which would stop a running change it owns.
		if (state->service || (CarryRunning(*state) && OwnsCarry(*state, CarryOwner(*state)->state()))) { return; }
		const auto live = state->session.get();
		if (!live || !state->current || !state->current() || !state->target
			|| !ValidateAccountSettingsOpenTarget(live, *state->target)) {
			state->status->setText(tr::lng_purple_chat_stale(tr::now));
			return;
		}
		EnsureSyncChatResolved(live, true);
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] {
		if (state->service) { state->service->cancel(); }
		if (state->move) { state->move->cancel(); }
		box->closeBox();
	});
}

}

void SyncChatCleanupBox(not_null<Ui::GenericBox*> box, DesktopSyncCarryProgress opening,
		Fn<std::optional<DesktopSyncCarryProgress>()> latest, Fn<bool()> current, Fn<void()> retry, Fn<void(bool)> skip) {
	box->setTitle(tr::lng_purple_chat_cleanup_title());
	box->setWidth(st::boxWideWidth);
	if (!DesktopSyncChatCleanupActivated() || !opening.journal || !current || !current()) {
		box->addRow(object_ptr<Ui::FlatLabel>(box,DesktopSyncChatCleanupActivated() ? tr::lng_purple_chat_stale(tr::now)
			: tr::lng_purple_chat_move_held(tr::now),st::boxLabel));
		box->addButton(tr::lng_close(),[=] { box->closeBox(); }); return;
	}
	const auto text=opening.cleanup.skipped ? tr::lng_purple_chat_cleanup_skipped(tr::now)
		: opening.cleanup.pending ? tr::lng_purple_chat_cleanup_pending(tr::now) : tr::lng_purple_chat_move_complete(tr::now);
	box->addRow(object_ptr<Ui::FlatLabel>(box,text,st::boxLabel));
	if (opening.cleanup.uncopied) { box->addRow(object_ptr<Ui::FlatLabel>(box,tr::lng_purple_chat_cleanup_uncopied(tr::now),st::boxLabel)); }
	const auto weak=QPointer<Ui::GenericBox>(box.get());
	const auto allowed=[weak,current,latest,opening] {
		if (!weak || !current || !current() || !latest) { return false; }
		const auto fresh=latest();
		return fresh && fresh->journal && fresh->cleanup.pending && fresh->cleanup.canAct
			&& fresh->journal->operationId==opening.journal->operationId && fresh->journal->revision==opening.journal->revision
			&& fresh->journal->account==opening.journal->account && fresh->journal->loginAuthority==opening.journal->loginAuthority
			&& fresh->journal->source==opening.journal->source && fresh->journal->target==opening.journal->target;
	};
	if (opening.cleanup.pending && opening.cleanup.canAct) {
		box->addButton(tr::lng_purple_chat_cleanup_retry(),[allowed,retry] { if (allowed() && retry) { retry(); } });
		box->addButton(tr::lng_purple_chat_cleanup_skip(),[=] {
			if (!allowed()) { return; }
			box->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> confirmation) {
				confirmation->setTitle(tr::lng_purple_chat_cleanup_skip());
				confirmation->addRow(object_ptr<Ui::FlatLabel>(confirmation,tr::lng_purple_chat_cleanup_skip_confirm(tr::now),st::boxLabel));
				confirmation->addButton(tr::lng_continue(),[=] { confirmation->closeBox(); if (allowed() && skip) { skip(true); } });
				confirmation->addButton(tr::lng_cancel(),[=] { confirmation->closeBox(); });
			}));
		});
	}
	box->addButton(tr::lng_close(),[=] { box->closeBox(); });
}

void SyncChatSetupBox(not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session, Fn<bool()> current) {
	BuildSetupBox(box, session, std::move(current), false);
}

}
