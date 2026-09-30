/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_setup_box.h"

#include "core/application.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "purple/purple_sync_account_inventory.h"
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
};

struct UndoInfo {
	QString historyId;
	QString device;
};

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
	std::optional<SyncConfigReview> review;
	std::optional<ShareInfo> share;
	std::optional<UndoInfo> undo;
	rpl::variable<QString> actionText;
	QDateTime checkedAt;
	QString progressPrefix;
	QString publishPrefix;
	Ui::FlatLabel *status = nullptr;
	Ui::RoundButton *check = nullptr;
	Ui::RoundButton *cancel = nullptr;
	Ui::SlideWrap<Ui::SettingsButton> *action = nullptr;
	Ui::SlideWrap<Ui::SettingsButton> *undoButton = nullptr;
	Action currentAction = Action::None;
	uint64_t scanned = 0;
	uint64_t generation = 0;
	int selectedIndex = -1;
	bool running = false;
	bool publishing = false;
};

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
	return account
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
	state.check->setDisabled(busy || !available);
	state.cancel->setDisabled(!state.running);
	state.actionText = SyncConfigBoxActionText(state.currentAction);
	state.action->toggle(
		!busy
			&& available
			&& state.review.has_value()
			&& state.currentAction != Action::None,
		anim::type::instant);
	state.undoButton->toggle(
		!busy && state.undo.has_value(),
		anim::type::instant);
}

void ClearReview(SetupBoxState &state) {
	state.completed.reset();
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

void ReviewAgain(SetupBoxState &state, const QString &prefix) {
	const auto account = state.selectedAccount.get();
	const auto session = state.selectedSession.get();
	if (!state.completed || !SelectedAvailable(state)) {
		ClearReview(state);
		SetStatus(state, Joined(
			prefix,
			u"Check Saved Messages to see what to do next."_q));
		return;
	}
	auto inventory = *state.completed;
	auto failure = QString();
	auto review = BuildReview(*account, *session, inventory, failure);
	ShowReview(
		state,
		std::move(inventory),
		std::move(review),
		Joined(prefix, failure));
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
	box->uiShow()->showBox(Ui::MakeConfirmBox({
		.text = ConfirmText(action, *state->review),
		.confirmed = crl::guard(box, [=](Fn<void()> close) {
			close();
			if (!SelectedAvailable(*state)) {
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
	ShowReview(*state, std::move(result), std::move(review), prefix);
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
				|| chosenAccount->maybeSession() != chosenSession.get()) {
				StopForLostAccount(*state);
				return;
			}
			OnInventory(box, state, std::move(result), followup);
		}));
	state->scanQueue = SyncConfigSendQueueOf(not_null{ session });
	state->inventory->Start();
}

void ApplyChoice(
		not_null<Ui::GenericBox*> box,
		SetupBoxState *state,
		uint64_t generation,
		std::optional<QString> key) {
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
		key);
	const SyncConfigHeadRecord *record = nullptr;
	for (const auto &head : review.plan.offered) {
		if (key && head.key == *key) {
			record = FindSyncConfigHeadRecord(review, head);
		}
	}
	const auto device = record ? SyncDeviceName(*record) : u"another device"_q;
	if (applied.wroteFile) {
		state->undo = applied.undoAvailable
			? std::make_optional(UndoInfo{ applied.historyId, device })
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
	box->uiShow()->showBox(Box(SyncConfigReviewBox, SyncConfigReviewBoxArgs{
		.review = *state->review,
		.choose = crl::guard(box, [=](std::optional<QString> key) {
			ApplyChoice(box, state, generation, std::move(key));
		}),
	}));
}

void ConfirmUndo(not_null<Ui::GenericBox*> box, SetupBoxState *state) {
	if (!state->undo || Busy(*state)) {
		return;
	}
	const auto undo = *state->undo;
	box->uiShow()->showBox(Ui::MakeConfirmBox({
		.text = u"Put back the settings this device had before the update "
			"from %1? The current file is kept in History. The change stays "
			"on this device until you publish it."_q.arg(undo.device),
		.confirmed = crl::guard(box, [=](Fn<void()> close) {
			close();
			if (Busy(*state)
				|| !state->undo
				|| state->undo->historyId != undo.historyId) {
				return;
			}
			const auto entries = ListSyncConfigHistory();
			const auto entry = std::find_if(
				entries.begin(),
				entries.end(),
				[&](const SyncConfigHistoryEntry &entry) {
					return entry.id == undo.historyId;
				});
			const auto result = RestoreSyncConfigHistory(
				undo.historyId,
				SyncConfigHistoryReason::BeforeUndo);
			if (SyncConfigUndoFinished(result)) {
				state->undo.reset();
			}
			if (result.status == SyncConfigRestoreStatus::Restored) {
				box->uiShow()->showToast(u"Previous settings are back."_q);
				ReviewAgain(*state, u"Your previous settings are back on this "
					"device. They stay local until you publish them."_q);
			} else {
				SetStatus(*state, (entry != entries.end())
					? SyncConfigRestoreText(result, *entry)
					: SyncConfigRestoreText(result, {}));
			}
		}),
		.confirmText = u"Undo"_q,
	}));
}

[[nodiscard]] bool RestoreFromHistory(
		not_null<Ui::GenericBox*> box,
		SetupBoxState *state,
		const SyncConfigHistoryEntry &entry) {
	if (Busy(*state)) {
		box->uiShow()->showToast(
			u"Wait for the check or post to finish, then try again."_q);
		return false;
	}
	const auto result = RestoreSyncConfigHistory(
		entry.id,
		SyncConfigHistoryReason::BeforeRestore);
	const auto text = SyncConfigRestoreText(result, entry);
	if (result.status == SyncConfigRestoreStatus::Restored) {
		state->undo.reset();
		box->uiShow()->showToast(u"Settings restored."_q);
		ReviewAgain(*state, text);
	} else {
		SetStatus(*state, text);
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

void SyncSetupBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"Sync across devices"_q));
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			u"Sync is manual in this version. Check Saved Messages reads "
			"the selected account's Saved Messages and shows what to do "
			"next. Nothing changes on this device without your review, and "
			"nothing is sent without a fresh check and your confirmation. "
			"Settings that sync replaces are kept in History. The settings "
			"file may contain chat IDs and names. Saved Messages is a "
			"Telegram cloud chat, not end-to-end encrypted, and every "
			"signed-in session can read it. state.toml stays local."_q,
			st::boxLabel),
		st::boxRowPadding);

	const auto state = box->lifetime().make_state<SetupBoxState>();
	const auto group = std::make_shared<Ui::RadiobuttonGroup>();
	state->group = group;
	const auto &domain = Core::App().domain();
	for (const auto &[index, account] : domain.accounts()) {
		if (const auto session = account->maybeSession()) {
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
		const auto weak = QPointer<Ui::GenericBox>(box.get());
		box->uiShow()->showBox(Box(
			SyncConfigHistoryBox,
			[=](const SyncConfigHistoryEntry &entry) {
				return weak && RestoreFromHistory(box, state, entry);
			}));
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
	});

	state->check = box->addButton(
		rpl::single(u"Check Saved Messages"_q),
		[=] {
			if (!Busy(*state)) {
				StartCheck(box, state, CheckFollowup::None);
			}
		});
	state->cancel = box->addButton(
		rpl::single(u"Cancel check"_q),
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
			}
		});
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
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
