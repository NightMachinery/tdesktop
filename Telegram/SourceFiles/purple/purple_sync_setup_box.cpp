/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_setup_box.h"

#include "purple/purple_sync_chat_service.h"
#include "purple/purple_sync_chat_setup_box.h"
#include "purple/purple_sync_history_guard.h"

#include "core/application.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "purple/purple_sync_account_inventory.h"
#include "purple/purple_sync_auto_service.h"
#include "purple/purple_sync_account_recovery.h"
#include "purple/purple_config.h"
#include "purple/purple_sync_config_apply.h"
#include "purple/purple_sync_config_publish.h"
#include "purple/purple_sync_config_review.h"
#include "purple/purple_sync_config_text.h"
#include "purple/purple_sync_review_box.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"

#include <QtCore/QDateTime>
#include <QtCore/QPointer>

#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <memory>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace Purple {
namespace {

using Action = SyncConfigBoxAction;

struct AccountChoice {
	base::weak_ptr<Main::Account> account;
	base::weak_ptr<Main::Session> session;
};

enum class CheckFollowup {
	None,
	Publish,
	PublishChanges,
	FinishSending,
	Share,
	Recovery,
};

struct UndoInfo {
	QString historyId;
	QString device;
	std::optional<AccountOperationTarget> target;
};

std::map<QString, std::optional<UndoInfo>> UndoByAccount;

struct ShareInfo {
	SyncConfigPublishRequest request;
	QString prefix;
};

struct SetupBoxState {
	std::vector<AccountChoice> choices;
	std::shared_ptr<Ui::RadiobuttonGroup> group;
	base::weak_ptr<Main::Account> selectedAccount;
	base::weak_ptr<Main::Session> selectedSession;
	std::unique_ptr<SyncAccountInventory> inventory;
	std::unique_ptr<SyncConfigPublish> publisher;
	std::optional<SyncAccountInventoryResult> completed;
	SyncConfigSendQueue scanQueue = SyncConfigSendQueue::Empty;
	uint64_t scanPosts = 0;
	std::optional<SyncConfigReview> review;
	std::optional<AccountOperationTarget> scanTarget;
	std::optional<ShareInfo> share;
	std::optional<DesktopSyncRecovery> recovery;
	Ui::SlideWrap<Ui::SettingsButton> *recoveryButton = nullptr;
	rpl::variable<QString> actionText;
	rpl::variable<QString> checkText;
	QDateTime checkedAt;
	QString progressPrefix;
	QString publishPrefix;
	Ui::FlatLabel *status = nullptr;
	Ui::RoundButton *check = nullptr;
	Ui::RoundButton *close = nullptr;
	Ui::SlideWrap<Ui::SettingsButton> *action = nullptr;
	Ui::SlideWrap<Ui::SettingsButton> *undoButton = nullptr;
	Action currentAction = Action::None;
	uint64_t scanned = 0;
	uint64_t generation = 0;
	int selectedIndex = -1;
	bool running = false;
	bool publishing = false;
};

std::optional<UndoInfo> &LastUndoFor(SetupBoxState &state) {
	const auto session = state.selectedSession.get();
	const auto key = session ? AccountKeyText(SessionAccountKey(session).environment,
		SessionAccountKey(session).userId) : std::nullopt;
	return UndoByAccount[key.value_or(QString())];
}

[[nodiscard]] QString AccountLabel(not_null<Main::Session*> session) {
	const auto user = session->user();
	const auto username = user->username();
	return username.isEmpty()
		? user->name()
		: u"%1 (@%2)"_q.arg(user->name(), username);
}

[[nodiscard]] QString Joined(const QString &first, const QString &second) {
	return first.isEmpty()
		? second
		: second.isEmpty()
		? first
		: (first + u"\n\n"_q + second);
}

[[nodiscard]] bool SelectedAvailable(const SetupBoxState &state) {
	const auto account = state.selectedAccount.get();
	const auto session = state.selectedSession.get();
	return session && AccountSyncOperationsAllowed(session)
		&& account
		&& session
		&& !account->loggingOut()
		&& account->maybeSession() == session;
}

[[nodiscard]] bool Busy(const SetupBoxState &state) {
	return state.running || state.publishing;
}

[[nodiscard]] bool StillCurrent(
		const SetupBoxState &state,
		uint64_t generation) {
	return !Busy(state)
		&& state.generation == generation
		&& state.completed
		&& state.review
		&& SelectedAvailable(state);
}

[[nodiscard]] Action FollowupAction(CheckFollowup followup) {
	switch (followup) {
	case CheckFollowup::Publish: return Action::Publish;
	case CheckFollowup::PublishChanges: return Action::PublishChanges;
	case CheckFollowup::FinishSending: return Action::FinishSending;
	case CheckFollowup::None:
	case CheckFollowup::Share:
	case CheckFollowup::Recovery:
		break;
	}
	return Action::None;
}

[[nodiscard]] QString PublishResult(const SyncConfigPublishResult &result) {
	switch (result.status) {
	case SyncConfigPublishStatus::Confirmed:
		return u"Sent and confirmed: the settings record matches Saved "
			"Messages (message %1)."_q.arg(result.messageId);
	case SyncConfigPublishStatus::AlreadySynced:
		return u"Nothing to send: Saved Messages already has this device's "
			"current settings."_q;
	case SyncConfigPublishStatus::OutcomeUnknown:
		return u"Outcome unknown: Telegram may have accepted the document. "
			"Check again before trying anything else; nothing is retried "
			"automatically."_q;
	case SyncConfigPublishStatus::NeedsReview:
		return u"Not sent: a fresh check no longer allows this post, or the "
			"records are ambiguous. Check again."_q;
	case SyncConfigPublishStatus::AccountUnbound:
		return u"Account unbound: the local sync state no longer matches "
			"this account. Nothing was sent."_q;
	case SyncConfigPublishStatus::StoreError:
		return u"Store error: the local sync state could not be safely "
			"updated. Check again before trying anything else."_q;
	case SyncConfigPublishStatus::AccountUnavailable:
		return u"Account unavailable. Sending stopped; check again before "
			"trying anything else."_q;
	case SyncConfigPublishStatus::CloneDetected:
		return u"Needs review: this device's sync identity appears "
			"duplicated. Nothing was sent."_q;
	case SyncConfigPublishStatus::InvalidSettings:
		return u"settings.toml is missing, too large, or not valid TOML, so "
			"nothing was sent."_q;
	case SyncConfigPublishStatus::Incomplete:
		return u"The check before sending did not finish, so nothing was "
			"sent. Check again."_q;
	case SyncConfigPublishStatus::Cancelled:
		return u"Sending cancelled before anything was sent. Check again "
			"before trying again."_q;
	case SyncConfigPublishStatus::StillSending:
		return u"Not sent: an earlier copy of this settings post is still "
			"being sent, or failed to send, in Saved Messages. Wait until it "
			"arrives and check again, or delete the failed copy there "
			"first."_q;
	}
	return u"Sending stopped. Check again before trying again."_q;
}

[[nodiscard]] QString ConfirmText(
		Action action,
		const SyncConfigReview &review) {
	switch (action) {
	case Action::Publish:
		return (review.bound
			? u"Send this device's settings to this account's Saved "
				"Messages? "_q
			: u"Start sync with this device's settings? This sends one copy "
				"of settings.toml to this account's Saved Messages, and this "
				"device can publish later changes there. "_q)
			+ SyncCloudDisclosureText();
	case Action::PublishChanges:
		return u"Send this device's current settings to this account's Saved "
			"Messages so your other devices can use them? "_q
			+ SyncCloudDisclosureText();
	case Action::FinishSending:
		return u"Finish the earlier settings post? This may confirm a record "
			"already in Saved Messages without sending anything, or send the "
			"staged record once if a fresh check shows that is safe. "_q
			+ SyncCloudDisclosureText();
	case Action::Join: {
		if (review.local.status == SyncSettingsFileStatus::Absent && review.plan.offered.empty()) {
			return tr::lng_purple_sync_empty_join_confirm(tr::now) + u" "_q + SyncCloudDisclosureText();
		}
		auto names = std::vector<QString>();
		for (const auto &head : review.plan.same) {
			if (const auto record = FindSyncConfigHeadRecord(review, head)) {
				names.push_back(SyncDeviceName(*record));
			}
		}
		return u"Join sync for this account? %1 already has exactly these "
			"settings, so nothing is sent now. Settings you publish from this "
			"device later are sent to Saved Messages. "_q.arg(
				SyncDeviceList(names))
			+ SyncCloudDisclosureText();
	}
	case Action::None:
	case Action::ReviewUpdate:
	case Action::Choose:
		break;
	}
	return QString();
}

void RefreshButtons(SetupBoxState &state) {
	const auto available = SelectedAvailable(state);
	const auto busy = Busy(state);
	if (!available && !busy && state.selectedSession) {
		state.status->setText(tr::lng_purple_account_sync_file_held(tr::now));
	}
	state.checkText = state.running
		? u"Cancel check"_q
		: u"Check Saved Messages"_q;
	state.check->setDisabled(!state.running && (busy || !available));
	state.close->setDisabled(state.publishing);
	if (state.recoveryButton) {
		const auto recovery = state.recovery ? &*state.recovery : nullptr;
		state.recoveryButton->toggle(!busy && available && recovery
			&& (recovery->journalPresent || recovery->plan.offerStartAgain
				|| recovery->plan.status == SyncAccountRecoveryStatus::RootRelink), anim::type::instant);
	}
	state.actionText = SyncConfigBoxActionText(state.currentAction);
	state.action->toggle(
		!busy
			&& available
			&& state.review.has_value()
			&& state.currentAction != Action::None,
		anim::type::instant);
	state.undoButton->toggle(
		!busy && LastUndoFor(state).has_value(),
		anim::type::instant);
}

void ClearReview(SetupBoxState &state) {
	state.completed.reset();
	state.recovery.reset();
	state.review.reset();
	state.currentAction = Action::None;
	++state.generation;
}

void SetStatus(SetupBoxState &state, const QString &text) {
	state.status->setText(text);
	RefreshButtons(state);
}

[[nodiscard]] SyncConfigReview BuildReview(
		Main::Account &account,
		Main::Session &session,
		const SyncAccountInventoryResult &inventory,
		QString &failure) {
	auto review = ReviewSyncConfig(account, session, inventory);
	if (review.status != SyncConfigReviewStatus::Ready
		|| !review.bound
		|| review.plan.verdict != ConfigSyncVerdict::Adopt) {
		return review;
	}
	const auto adopted = ApplySyncConfigChoice(
		account,
		session,
		inventory,
		review,
		std::nullopt);
	if (adopted.status != SyncConfigApplyStatus::Applied) {
		failure = SyncConfigApplyFailureText(adopted);
		return review;
	}
	return ReviewSyncConfig(account, session, inventory);
}

void ShowReview(
		SetupBoxState &state,
		SyncAccountInventoryResult inventory,
		SyncConfigReview review,
		const QString &prefix) {
	const auto described = DescribeSyncConfigReview(review, state.checkedAt);
	state.completed = std::move(inventory);
	state.review = std::move(review);
	state.currentAction = described.action;
	if (state.review->status == SyncConfigReviewStatus::Ready && !state.review->bound
		&& state.review->local.status == SyncSettingsFileStatus::Absent
		&& !state.review->local.usingLastGood && state.review->plan.offered.empty()
		&& state.review->plan.verdict != ConfigSyncVerdict::Invalid) {
		state.currentAction = Action::Join;
	}
	++state.generation;
	SetStatus(state, Joined(prefix, described.text));
}

void StopForLostAccount(SetupBoxState &state) {
	const auto wasRunning = state.running;
	ClearReview(state);
	state.share.reset();
	state.publishPrefix = QString();
	state.selectedIndex = -1;
	state.selectedAccount = {};
	state.selectedSession = {};
	state.running = false;
	state.publishing = false;
	state.group->setValue(-1);
	if (wasRunning && state.inventory) {
		state.inventory->Cancel();
	}
	if (state.publisher) {
		state.publisher->Cancel();
	}
	SetStatus(
		state,
		u"Incomplete: the selected account is unavailable. Choose another "
			"account or reopen this box, then check again."_q);
}

void ReviewAgain(
		SetupBoxState &state,
		Fn<QString(bool staysLocal)> prefix) {
	const auto account = state.selectedAccount.get();
	const auto session = state.selectedSession.get();
	if (!state.completed || !SelectedAvailable(state)) {
		ClearReview(state);
		SetStatus(state, Joined(
			prefix(true),
			u"Check Saved Messages to see what to do next."_q));
		return;
	}
	auto inventory = *state.completed;
	auto failure = QString();
	auto review = BuildReview(*account, *session, inventory, failure);
	const auto staysLocal = !SyncSettingsMatchBase(review);
	ShowReview(
		state,
		std::move(inventory),
		std::move(review),
		Joined(prefix(staysLocal), failure));
}

void ReviewAgain(SetupBoxState &state, const QString &prefix) {
	ReviewAgain(state, [&](bool) { return prefix; });
}

void StartPublisher(
		not_null<Ui::GenericBox*> box,
		SetupBoxState *state,
		SyncAccountInventoryResult inventory,
		SyncConfigPublishRequest request,
		const QString &prefix,
		const QString &progress) {
	const auto account = state->selectedAccount.get();
	const auto session = state->selectedSession.get();
	if (!SelectedAvailable(*state)) {
		StopForLostAccount(*state);
		return;
	}
	state->publishPrefix = prefix;
	state->publishing = true;
	SetStatus(*state, Joined(state->publishPrefix, progress));
	state->publisher = std::make_unique<SyncConfigPublish>(
		*account,
		*session,
		std::move(inventory),
		state->scanQueue,
		state->scanPosts,
		crl::guard(box, [=](SyncConfigPublishResult result) {
			if (!state->publishing) {
				return;
			}
			state->publishing = false;
			SetStatus(
				*state,
				Joined(
					base::take(state->publishPrefix),
					PublishResult(result)));
		}),
		std::move(request));
	state->publisher->Start();
}

void StartCheck(
		not_null<Ui::GenericBox*> box,
		SetupBoxState *state,
		CheckFollowup followup,
		const QString &prefix = QString());

void ConfirmRecovery(not_null<Ui::GenericBox*> box, SetupBoxState *state) {
	if (!state->recovery || Busy(*state) || !SelectedAvailable(*state)) { return; }
	const auto review = *state->recovery;
	const auto origin = state->selectedSession;
	const auto relink = review.plan.status == SyncAccountRecoveryStatus::RootRelink && !review.journalPresent;
	const auto name = AccountLabel(origin.get());
	box->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> confirm) {
		confirm->setTitle(rpl::single(relink ? tr::lng_purple_sync_relink(tr::now) : tr::lng_purple_sync_start_again(tr::now)));
		confirm->addRow(object_ptr<Ui::FlatLabel>(confirm,
			(relink ? tr::lng_purple_sync_relink_confirm(tr::now) : tr::lng_purple_sync_reset_confirm(tr::now)).arg(name), st::boxLabel));
		confirm->addButton(rpl::single(relink ? tr::lng_purple_sync_relink(tr::now) : tr::lng_purple_sync_start_again(tr::now)), crl::guard(box, [=] {
			if (!origin || state->selectedSession.get() != origin.get() || Busy(*state)
				|| !SelectedAvailable(*state) || !ValidateAccountSettingsOpenTarget(origin.get(), review.opening)) { return; }
			const auto result = relink ? SyncArchiveResult{ .ready = RelinkSyncAccount(*origin.get(), review, true) }
				: StartAgainSyncAccount(*origin.get(), review, true, Busy(*state));
			confirm->closeBox();
			if (result.ready) {
				if (!relink) { LastUndoFor(*state).reset(); }
				ReviewAgain(*state, relink ? tr::lng_purple_sync_relinked(tr::now) : tr::lng_purple_sync_reset_done(tr::now));
			} else {
				SetStatus(*state, result.partial ? tr::lng_purple_sync_reset_partial(tr::now) : tr::lng_purple_transfer_unavailable(tr::now));
			}
		}));
		const auto cancel = confirm->addButton(tr::lng_cancel(), [=] { confirm->closeBox(); });
		confirm->setFocusCallback([=] { cancel->setFocus(); });
	}));
}

void RunAction(
		not_null<Ui::GenericBox*> box,
		SetupBoxState *state,
		Action action) {
	const auto account = state->selectedAccount.get();
	const auto session = state->selectedSession.get();
	if (!SelectedAvailable(*state)) {
		StopForLostAccount(*state);
		return;
	}
	if (action == Action::FinishSending) {
		auto inventory = *state->completed;
		ClearReview(*state);
		StartPublisher(
			box,
			state,
			std::move(inventory),
			SyncConfigPublishRequest{ .pendingOnly = true },
			QString(),
			u"Finishing the earlier settings post."_q);
		return;
	}
	if (action == Action::Join && !state->review->bound
		&& state->review->local.status == SyncSettingsFileStatus::Absent
		&& !state->review->local.usingLastGood && state->review->plan.offered.empty()) {
		const auto result = InitializeSyncAccountLocally(*account, *session, *state->completed);
		ReviewAgain(*state, result.status == SyncAccountSetupStatus::Ready
			? tr::lng_purple_sync_empty_join_done(tr::now)
			: tr::lng_purple_transfer_unavailable(tr::now));
		return;
	}
	const auto applied = ApplySyncConfigChoice(
		*account,
		*session,
		*state->completed,
		*state->review,
		std::nullopt);
	if (applied.status != SyncConfigApplyStatus::Applied) {
		ClearReview(*state);
		SetStatus(*state, SyncConfigApplyFailureText(applied));
		return;
	} else if (action == Action::Join) {
		ReviewAgain(*state, u"Joined sync. Nothing was sent."_q);
		return;
	} else if (!applied.publishNeeded) {
		ReviewAgain(*state, u"Nothing needs to be sent."_q);
		return;
	}
	auto inventory = *state->completed;
	ClearReview(*state);
	StartPublisher(
		box,
		state,
		std::move(inventory),
		SyncConfigPublishRequest{
			.expectedFingerprint = applied.fingerprint,
			.expectedParents = applied.expectedParents,
		},
		QString(),
		u"Publishing this device's settings to Saved Messages."_q);
}

void ConfirmAction(
		not_null<Ui::GenericBox*> box,
		SetupBoxState *state,
		Action action) {
	if (!state->review) {
		return;
	}
	const auto generation = state->generation;
	const auto origin = state->selectedSession;
	const auto target = origin ? AccountSyncInspectionTarget(origin.get()) : std::nullopt;
	if (!target) { return; }
	box->uiShow()->showBox(Ui::MakeConfirmBox({
		.text = ConfirmText(action, *state->review),
		.confirmed = crl::guard(box, [=](Fn<void()> close) {
			close();
			if (!origin || origin.get() != state->selectedSession.get()
				|| !ValidateAccountSyncTarget(origin.get(), *target) || !SelectedAvailable(*state)) {
				StopForLostAccount(*state);
			} else if (!StillCurrent(*state, generation)) {
				SetStatus(*state, u"The check result changed, so nothing "
					"was done. Check again."_q);
			} else {
				RunAction(box, state, action);
			}
		}),
		.confirmText = SyncConfigBoxActionText(action),
	}));
}

void OnInventory(
		not_null<Ui::GenericBox*> box,
		SetupBoxState *state,
		SyncAccountInventoryResult result,
		CheckFollowup followup) {
	state->running = false;
	state->scanned = result.scan.scannedCount;
	state->checkedAt = QDateTime::currentDateTime();
	const auto account = state->selectedAccount.get();
	const auto session = state->selectedSession.get();
	if (followup == CheckFollowup::Share) {
		const auto share = base::take(state->share);
		ClearReview(*state);
		if (!share) {
			SetStatus(*state, u"Nothing to share. Check again."_q);
			return;
		}
		StartPublisher(
			box,
			state,
			std::move(result),
			share->request,
			share->prefix,
			u"Sharing the chosen settings to Saved Messages."_q);
		return;
	}
	auto recovery = InspectSyncAccountRecovery(*session, &result);
	if (recovery && recovery->plan.status == SyncAccountRecoveryStatus::AutoRelink
		&& !recovery->journalPresent && RelinkSyncAccount(*session, *recovery, false)) {
		recovery = InspectSyncAccountRecovery(*session, &result);
	}
	auto failure = QString();
	auto review = BuildReview(*account, *session, result, failure);
	const auto described = DescribeSyncConfigReview(review, state->checkedAt);
	const auto expected = FollowupAction(followup);
	const auto matches = (expected != Action::None)
		&& (described.action == expected);
	const auto prefix = (expected == Action::None || matches)
		? failure
		: Joined(
			(review.status == SyncConfigReviewStatus::Ready)
				? u"Saved Messages or this device changed since the last "
					"check, so nothing was sent."_q
				: u"Nothing was sent."_q,
			failure);
	state->recovery = std::move(recovery);
	ShowReview(*state, std::move(result), std::move(review), prefix);
	if (followup == CheckFollowup::Recovery && state->recovery) { ConfirmRecovery(box, state); }
	if (matches) {
		ConfirmAction(box, state, expected);
	}
}

void StartCheck(
		not_null<Ui::GenericBox*> box,
		SetupBoxState *state,
		CheckFollowup followup,
		const QString &prefix) {
	const auto session = state->selectedSession.get();
	if (!SelectedAvailable(*state)) {
		StopForLostAccount(*state);
		return;
	}
	ClearReview(*state);
	state->inventory.reset();
	state->running = true;
	state->scanned = 0;
	state->progressPrefix = prefix;
	const auto progressText = [=](uint64_t count) {
		return Joined(state->progressPrefix, (followup != CheckFollowup::None)
			? u"Checking Saved Messages again before sending: %1 "
				"scanned."_q.arg(QString::number(count))
			: u"Checking Saved Messages: %1 scanned."_q.arg(
				QString::number(count)));
	};
	SetStatus(*state, progressText(0));
	const auto chosenAccount = state->selectedAccount;
	const auto chosenSession = state->selectedSession;
	state->scanTarget = AccountSyncInspectionTarget(session);
	if (!state->scanTarget) { StopForLostAccount(*state); return; }
	const auto scanTarget = *state->scanTarget;
	state->inventory = std::make_unique<SyncAccountInventory>(
		session,
		crl::guard(box, [=](uint64_t count) {
			if (state->running) {
				state->scanned = count;
				state->status->setText(progressText(count));
			}
		}),
		crl::guard(box, [=](SyncAccountInventoryResult result) {
			if (!state->running) {
				return;
			}
			if (chosenAccount.get() != state->selectedAccount.get()
				|| chosenSession.get() != state->selectedSession.get()) {
				return;
			}
			if (!chosenAccount || !chosenSession
				|| chosenAccount->loggingOut()
				|| chosenAccount->maybeSession() != chosenSession.get()
				|| !ValidateAccountSyncTarget(chosenSession.get(), scanTarget)) {
				StopForLostAccount(*state);
				return;
			}
			OnInventory(box, state, std::move(result), followup);
		}));
	state->scanQueue = SyncConfigSendQueueOf(not_null{ session });
	state->scanPosts = SyncConfigPostsStarted(not_null{ session });
	state->inventory->Start();
}

void ApplyChoice(
		not_null<Ui::GenericBox*> box,
		SetupBoxState *state,
		uint64_t generation,
		std::optional<QString> key,
		const AccountTransferReview &replacement) {
	if (!SelectedAvailable(*state)) {
		StopForLostAccount(*state);
		return;
	} else if (!StillCurrent(*state, generation)) {
		SetStatus(*state, u"The check result changed or a check is running, "
			"so nothing was done. Check again."_q);
		return;
	}
	const auto account = state->selectedAccount.get();
	const auto session = state->selectedSession.get();
	const auto review = *state->review;
	const auto applied = ApplySyncConfigChoice(
		*account,
		*session,
		*state->completed,
		review,
		key,
		&replacement);
	const SyncConfigHeadRecord *record = nullptr;
	for (const auto &head : review.plan.offered) {
		if (key && head.key == *key) {
			record = FindSyncConfigHeadRecord(review, head);
		}
	}
	const auto device = record ? SyncDeviceName(*record) : u"another device"_q;
	if (applied.wroteFile) {
		LastUndoFor(*state) = applied.undoAvailable
			? std::make_optional(UndoInfo{ applied.historyId, device, AccountSyncTarget(state->selectedSession.get()) })
			: std::nullopt;
	}
	if (applied.status != SyncConfigApplyStatus::Applied) {
		ClearReview(*state);
		SetStatus(*state, SyncConfigApplyFailureText(applied));
		return;
	}
	auto prefix = applied.joined ? u"Joined sync."_q : QString();
	if (applied.wroteFile) {
		box->uiShow()->showToast(u"Settings updated from %1."_q.arg(device));
		prefix = Joined(prefix, applied.undoAvailable
			? u"Settings updated from %1. The previous file is in History, "
				"and Undo puts it back."_q.arg(device)
			: (review.local.status == SyncSettingsFileStatus::Absent)
			? u"Settings updated from %1. This device had no settings.toml "
				"before, so there is nothing to undo."_q.arg(device)
			: u"Settings updated from %1. The previous file is in History, "
				"but it is not valid UTF-8 text, so Undo cannot put it "
				"back."_q.arg(device));
	}
	const auto choosing = (review.plan.verdict == ConfigSyncVerdict::Choose)
		|| (review.plan.verdict == ConfigSyncVerdict::Conflict);
	if (choosing && applied.publishNeeded) {
		state->share = ShareInfo{
			.request = {
				.expectedFingerprint = applied.fingerprint,
				.expectedParents = applied.expectedParents,
			},
			.prefix = prefix,
		};
		StartCheck(box, state, CheckFollowup::Share, prefix);
		return;
	}
	ReviewAgain(*state, prefix);
}

void OpenReview(not_null<Ui::GenericBox*> box, SetupBoxState *state) {
	if (!state->review || Busy(*state)) {
		return;
	}
	const auto generation = state->generation;
	const auto origin = state->selectedSession;
	const auto target = origin ? AccountSettingsOpenTargetFor(origin.get(), SettingsOpenRole::Own) : std::nullopt;
	if (!target) { return; }
	const auto proposed = state->review->local.status == SyncSettingsFileStatus::Present
		? state->review->local.text : QByteArray("version = 2\n");
	const auto replacement = ReviewAccountTransfer(origin.get(), proposed);
	if (!replacement) {
		SetStatus(*state, tr::lng_purple_transfer_unavailable(tr::now));
		return;
	}
	box->uiShow()->showBox(Box(SyncConfigReviewBox, SyncConfigReviewBoxArgs{
		.review = *state->review,
		.accountCaption = AccountLabel(origin.get()),
		.listNotes = CaptureOwnListPreviewNotes(origin.get()),
		.replacement = *replacement,
		.choose = crl::guard(box, [=](std::optional<QString> key, AccountTransferReview chosen) {
			if (!origin || state->selectedSession.get() != origin.get()
				|| !ValidateAccountSettingsOpenTarget(origin.get(), *target)) { return; }
			ApplyChoice(box, state, generation, std::move(key), chosen);
		}),
	}));
}

[[nodiscard]] SyncHistoryReview HistoryReviewFor(
		not_null<Ui::GenericBox*> box,
		SetupBoxState *state) {
	const auto origin = state->selectedSession;
	const auto opening = origin ? AccountSettingsOpenTargetFor(origin.get(), SettingsOpenRole::Own) : std::nullopt;
	return crl::guard(box, [=](const QByteArray &bytes) -> std::optional<AccountTransferReview> {
		if (!opening || !origin || origin.get() != state->selectedSession.get()
			|| !ValidateAccountSettingsOpenTarget(origin.get(), *opening)) { return std::nullopt; }
		return ReviewAccountTransfer(origin.get(), bytes);
	});
}

void ConfirmUndo(not_null<Ui::GenericBox*> box, SetupBoxState *state) {
	if (!LastUndoFor(*state) || Busy(*state) || !SelectedAvailable(*state)) { return; }
	const auto undo = *LastUndoFor(*state);
	const auto origin = state->selectedSession;
	if (!undo.target || !ValidateAccountSyncTarget(origin.get(), *undo.target)) { return; }
	const auto access = AccountSyncHistoryAccess(origin.get(), *undo.target);
	const auto entries = ListSyncConfigHistory(access);
	const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto &value) {
		return value.id == undo.historyId;
	});
	if (entry == entries.end()) { return; }
	box->uiShow()->showBox(Box(SyncConfigHistoryEntryBox, access, *entry,
		HistoryReviewFor(box, state),
		crl::guard(box, [=](const SyncConfigHistoryEntry &entry, const AccountTransferReview &replacement) {
			if (!origin || origin.get() != state->selectedSession.get() || Busy(*state)
				|| !SelectedAvailable(*state) || !LastUndoFor(*state)
				|| LastUndoFor(*state)->historyId != undo.historyId
				|| !ValidateAccountSyncTarget(origin.get(), *undo.target)) { return false; }
			const auto result = RestoreSyncConfigHistory(*origin.get(), entry.id,
				SyncConfigHistoryReason::BeforeUndo, replacement);
			if (SyncConfigUndoFinished(result)) { LastUndoFor(*state).reset(); }
			if (result.status == SyncConfigRestoreStatus::Restored) {
				box->uiShow()->showToast(u"Previous settings are back."_q);
				ReviewAgain(*state, SyncConfigUndoneText);
			} else {
				SetStatus(*state, SyncConfigRestoreText(result, entry));
			}
			return true;
		}), Fn<void()>(), CaptureOwnListPreviewNotes(origin.get()), u"Undo"_q));
}

[[nodiscard]] bool RestoreFromHistory(
		not_null<Ui::GenericBox*> box,
		SetupBoxState *state,
		const SyncConfigHistoryEntry &entry,
		const AccountTransferReview &replacement) {
	if (Busy(*state) || !SelectedAvailable(*state)) {
		box->uiShow()->showToast(
			u"Wait for the check or post to finish, then try again."_q);
		return false;
	}
	const auto result = RestoreSyncConfigHistory(
		*state->selectedSession.get(), entry.id,
		SyncConfigHistoryReason::BeforeRestore, replacement);
	if (result.status == SyncConfigRestoreStatus::Restored) {
		LastUndoFor(*state).reset();
		box->uiShow()->showToast(u"Settings restored."_q);
		ReviewAgain(*state, [&](bool staysLocal) {
			return SyncConfigRestoreText(result, entry, staysLocal);
		});
	} else {
		SetStatus(*state, SyncConfigRestoreText(result, entry));
	}
	return true;
}

void RunCurrentAction(not_null<Ui::GenericBox*> box, SetupBoxState *state) {
	if (!state->review || Busy(*state)) {
		return;
	} else if (!SelectedAvailable(*state)) {
		StopForLostAccount(*state);
		return;
	}
	switch (state->currentAction) {
	case Action::Publish:
		StartCheck(box, state, CheckFollowup::Publish);
		break;
	case Action::PublishChanges:
		StartCheck(box, state, CheckFollowup::PublishChanges);
		break;
	case Action::FinishSending:
		StartCheck(box, state, CheckFollowup::FinishSending);
		break;
	case Action::Join:
		ConfirmAction(box, state, Action::Join);
		break;
	case Action::ReviewUpdate:
	case Action::Choose:
		OpenReview(box, state);
		break;
	case Action::None:
		break;
	}
}

} // namespace

void NoteAutomaticSyncApply(not_null<Main::Session*> session, const SyncConfigApplyResult &result) {
	const auto key = AccountKeyText(SessionAccountKey(session).environment, SessionAccountKey(session).userId);
	if (key && result.wroteFile) {
		UndoByAccount[*key] = result.undoAvailable
			? std::make_optional(UndoInfo{ result.historyId, tr::lng_purple_auto_remote(tr::now), AccountSyncTarget(session) })
			: std::nullopt;
	}
}

void SyncSetupBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"Sync across devices"_q));
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			tr::lng_purple_auto_description(tr::now),
			st::boxLabel),
		st::boxRowPadding);

	const auto state = box->lifetime().make_state<SetupBoxState>();
	const auto group = std::make_shared<Ui::RadiobuttonGroup>();
	state->group = group;
	const auto &domain = Core::App().domain();
	for (const auto &[index, account] : domain.accounts()) {
		if (const auto session = account->maybeSession()) {
			EnsureSyncChatResolved(session, true);
			const auto choice = int(state->choices.size());
			state->choices.push_back({
				base::make_weak(account.get()),
				base::make_weak(session),
			});
			container->add(
				object_ptr<Ui::Radiobutton>(
					container,
					group,
					choice,
					AccountLabel(session),
					st::defaultCheckbox),
				st::boxRowPadding);
		}
	}

	if (state->choices.size() == 1) {
		group->setValue(0);
		state->selectedIndex = 0;
		state->selectedAccount = state->choices[0].account;
		state->selectedSession = state->choices[0].session;
	} else if (state->choices.size() > 1) {
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				u"Choose an account to check. No account is selected yet."_q,
				st::boxDividerLabel),
			st::boxRowPadding);
	}

	const auto chatStatus = container->add(object_ptr<Ui::FlatLabel>(container,
		QString(), st::boxLabel), st::boxRowPadding);
	const auto chatButton = ::Settings::AddButtonWithIcon(container,
		tr::lng_purple_chat_title(), st::settingsButtonNoIcon);
	const auto updateChat = [=] {
		const auto session = state->selectedSession.get();
		chatButton->setDisabled(!session || Busy(*state) || !SelectedAvailable(*state));
		if (!session) {
			chatStatus->setText(tr::lng_purple_chat_select_account(tr::now));
			return;
		}
		const auto status = SyncChatStatusOf(session);
		chatStatus->setText(status.newReaders
			? tr::lng_purple_chat_new_readers(tr::now)
			: status.phase == DesktopSyncChatPhase::Use
			? tr::lng_purple_chat_status_open(tr::now)
			: status.phase == DesktopSyncChatPhase::Searching
			? tr::lng_purple_chat_searching(tr::now)
			: status.phase == DesktopSyncChatPhase::Unset
			? tr::lng_purple_chat_unset(tr::now)
			: tr::lng_purple_chat_posting_held(tr::now));
	};
	SyncChatStatusChanges() | rpl::on_next([=](Main::Session*) {
		updateChat();
	}, box->lifetime());
	chatButton->setClickedCallback([=] {
		if (Busy(*state) || !SelectedAvailable(*state)) {
			return;
		}
		const auto origin = state->selectedSession;
		const auto weak = QPointer<Ui::GenericBox>(box.get());
		box->uiShow()->showBox(Box(SyncChatSetupBox, origin.get(), [=] {
			return weak && origin && state->selectedSession.get() == origin.get()
				&& SelectedAvailable(*state) && !Busy(*state);
		}));
	});
	updateChat();

	const auto automatic = container->add(object_ptr<Ui::Checkbox>(container,
		tr::lng_purple_auto_enable(tr::now), false), st::boxRowPadding);
	const auto automaticStatus = container->add(object_ptr<Ui::FlatLabel>(container,
		tr::lng_purple_auto_select(tr::now), st::boxLabel), st::boxRowPadding);
	const auto updateAutomatic = [=] {
		const auto session = state->selectedSession.get();
		automatic->setEnabled(session && !state->running && !state->publishing);
		automatic->setChecked(session && AutomaticSettingsSyncEnabled(session), Ui::Checkbox::NotifyAboutChange::DontNotify);
		automaticStatus->setText(session ? AutomaticSettingsSyncStatus(session) : tr::lng_purple_auto_select(tr::now));
	};
	AutomaticSettingsSyncChanges() | rpl::on_next(updateAutomatic, box->lifetime());
	automatic->checkedChanges() | rpl::on_next([=](bool enabled) {
		const auto session = state->selectedSession.get();
		const auto changed = session && !state->running && !state->publishing && SetAutomaticSettingsSync(session, enabled);
		updateAutomatic();
		if (!changed && session) { automaticStatus->setText(tr::lng_purple_auto_invalid(tr::now)); }
	}, box->lifetime());
	updateAutomatic();

	state->status = container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			state->choices.empty()
				? u"Sign in to an account to check Saved Messages."_q
				: state->choices.size() > 1
				? u"Select an account before checking."_q
				: u"Ready to check Saved Messages."_q,
			st::boxLabel),
		st::boxRowPadding);
	Ui::AddSkip(container);
	state->action = container->add(
		object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
			container,
			::Settings::CreateButtonWithIcon(
				container,
				state->actionText.value(),
				st::settingsButtonNoIcon)));
	state->undoButton = container->add(
		object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
			container,
			::Settings::CreateButtonWithIcon(
				container,
				rpl::single(u"Undo last update"_q),
				st::settingsButtonNoIcon)));
	state->recoveryButton = container->add(object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
		container, ::Settings::CreateButtonWithIcon(container, tr::lng_purple_sync_recovery(), st::settingsButtonNoIcon)));
	state->recoveryButton->hide(anim::type::instant);
	state->recoveryButton->entity()->setClickedCallback([=] {
		StartCheck(box, state, CheckFollowup::Recovery);
	});
	const auto history = ::Settings::AddButtonWithIcon(
		container,
		rpl::single(u"History"_q),
		st::settingsButtonNoIcon);
	state->action->hide(anim::type::instant);
	state->undoButton->hide(anim::type::instant);
	state->action->entity()->setClickedCallback([=] {
		RunCurrentAction(box, state);
	});
	state->undoButton->entity()->setClickedCallback([=] {
		ConfirmUndo(box, state);
	});
	history->setClickedCallback([=] {
		if (!SelectedAvailable(*state)) {
			return;
		}
		const auto origin = state->selectedSession;
		const auto target = AccountSyncInspectionTarget(origin.get());
		if (!target) { return; }
		box->uiShow()->showBox(Box(
			SyncConfigHistoryBox,
			AccountSyncHistoryAccess(state->selectedSession.get(), *target),
			HistoryReviewFor(box, state),
			crl::guard(box, [=](const SyncConfigHistoryEntry &entry, const AccountTransferReview &replacement) {
				const auto live = state->selectedSession.get();
				return origin && live == origin.get() && ValidateAccountSyncTarget(live, *target)
					&& RestoreFromHistory(box, state, entry, replacement);
			}),
			CaptureOwnListPreviewNotes(state->selectedSession.get())));
	});

	group->setChangedCallback([=](int value) {
		if (value < 0 || value >= int(state->choices.size())) {
			return;
		}
		ClearReview(*state);
		state->share.reset();
		const auto wasRunning = state->running;
		const auto wasPublishing = state->publishing;
		state->running = false;
		state->publishing = false;
		if (wasRunning && state->inventory) {
			state->inventory->Cancel();
		}
		if (wasPublishing && state->publisher) {
			state->publisher->Cancel();
		}
		const auto account = state->choices[value].account.get();
		const auto session = state->choices[value].session.get();
		if (!account || !session || account->maybeSession() != session) {
			StopForLostAccount(*state);
			return;
		}
		state->selectedIndex = value;
		state->selectedAccount = state->choices[value].account;
		state->selectedSession = state->choices[value].session;
		SetStatus(*state, u"Ready to check Saved Messages."_q);
		updateAutomatic();
		updateChat();
	});

	state->checkText = u"Check Saved Messages"_q;
	state->check = box->addButton(
		state->checkText.value(),
		[=] {
			if (state->running && state->inventory) {
				const auto sharing = state->share.has_value();
				ClearReview(*state);
				state->share.reset();
				state->running = false;
				state->inventory->Cancel();
				SetStatus(*state, sharing
					? u"Sharing cancelled: the chosen settings are on this "
						"device but were not sent. Check again to share "
						"them."_q
					: u"Check cancelled. Check Saved Messages again."_q);
			} else if (!Busy(*state)) {
				StartCheck(box, state, CheckFollowup::None);
			}
		});
	state->close = box->addButton(tr::lng_close(), [=] {
		if (!state->publishing) {
			box->closeBox();
		}
	});
	RefreshButtons(*state);

	for (const auto &choice : state->choices) {
		if (const auto account = choice.account.get()) {
			account->sessionChanges(
			) | rpl::on_next(crl::guard(box, [=](Main::Session *session) {
				if (account == state->selectedAccount.get()
					&& session != state->selectedSession.get()) {
					StopForLostAccount(*state);
				}
			}), box->lifetime());
		}
	}
	domain.accountsChanges(
	) | rpl::on_next(crl::guard(box, [=] {
		const auto account = state->selectedAccount.get();
		const auto session = state->selectedSession.get();
		if (state->selectedIndex >= 0
			&& (!account || !session || account->maybeSession() != session)) {
			StopForLostAccount(*state);
		}
	}), box->lifetime());
	box->boxClosing(
	) | rpl::on_next([=] {
		ClearReview(*state);
		state->share.reset();
		state->running = false;
		state->publishing = false;
		if (state->inventory) {
			state->inventory->Cancel();
		}
		if (state->publisher) {
			state->publisher->Cancel();
		}
	}, box->lifetime());
}

} // namespace Purple
