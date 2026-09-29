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
#include "purple/purple_config.h"
#include "purple/purple_sync_account_inventory.h"
#include "purple/purple_sync_account_setup.h"
#include "purple/purple_sync_config_publish.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"

#include <QtCore/QFileInfo>

#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace Purple {
namespace {

struct AccountChoice {
	base::weak_ptr<Main::Account> account;
	base::weak_ptr<Main::Session> session;
};

struct SetupBoxState {
	std::vector<AccountChoice> choices;
	std::shared_ptr<Ui::RadiobuttonGroup> group;
	base::weak_ptr<Main::Account> selectedAccount;
	base::weak_ptr<Main::Session> selectedSession;
	std::unique_ptr<SyncAccountInventory> inventory;
	std::unique_ptr<SyncConfigPublish> publisher;
	std::optional<SyncAccountInventoryResult> completed;
	Ui::FlatLabel *status = nullptr;
	Ui::RoundButton *check = nullptr;
	Ui::RoundButton *cancel = nullptr;
	Ui::RoundButton *publish = nullptr;
	uint64_t scanned = 0;
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

void RefreshButtons(SetupBoxState &state) {
	const auto account = state.selectedAccount.get();
	const auto session = state.selectedSession.get();
	const auto available = account
		&& session
		&& !account->loggingOut()
		&& account->maybeSession() == session;
	state.check->setDisabled(state.running || state.publishing || !available);
	state.cancel->setDisabled(!state.running);
	if (state.publish) {
		state.publish->setDisabled(state.running
			|| state.publishing
			|| !available
			|| !state.completed
			|| state.completed->accountUserId
				!= peerToUser(session->user()->id).bare
			|| state.completed->status != SyncAccountInventoryStatus::Complete
			|| !state.completed->scan.complete()
			|| !state.completed->read
			|| !state.completed->read->complete()
			|| !state.completed->directory.complete
			|| !state.completed->directory.canCreateSpace
			|| state.completed->directory.selectedSpace
			|| !state.completed->directory.groups.empty()
			|| QFileInfo::exists(
				ConfigDirectory() + u"/sync/state.json"_q));
	}
}

void StopForLostAccount(SetupBoxState &state) {
	const auto wasRunning = state.running;
	state.completed.reset();
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
	state.status->setText(
		u"Incomplete: the selected account is unavailable. Choose another "
		"account or reopen this box, then check again."_q);
	RefreshButtons(state);
}

void ShowResult(SetupBoxState &state, SyncAccountInventoryResult result) {
	state.running = false;
	state.completed.reset();
	state.scanned = result.scan.scannedCount;
	const auto scanned = QString::number(state.scanned);
	const auto candidates = QString::number(result.scan.candidateIds.size());
	auto valid = 0;
	auto future = 0;
	if (result.read) {
		for (const auto &record : result.read->records) {
			if (record.status == SyncCandidateStatus::Valid) {
				++valid;
			} else if (record.header
				&& (record.status == SyncCandidateStatus::UnsupportedStream
					|| record.status
						== SyncCandidateStatus::UnsupportedEncoding
					|| record.status
						== SyncCandidateStatus::UnsupportedLibrary)) {
				++future;
			}
		}
	}
	if (result.status == SyncAccountInventoryStatus::Complete) {
		state.status->setText(future
			? u"Complete: found %1 sync record(s), including %2 this "
				"version cannot read. Checked %3 messages."_q.arg(
				QString::number(valid + future),
				QString::number(future),
				scanned)
			: valid
			? u"Complete: found %1 sync record(s). Checked %2 messages."_q.arg(
				QString::number(valid), scanned)
			: u"Complete: no sync records found after checking %1 messages."_q.arg(
				scanned));
		if (result.directory.complete
			&& result.directory.canCreateSpace
			&& !result.directory.selectedSpace
			&& result.directory.groups.empty()) {
			state.completed = std::move(result);
			if (QFileInfo::exists(
					ConfigDirectory() + u"/sync/state.json"_q)) {
				state.status->setText(
					u"A local sync state already exists. Resuming or retrying "
					"a prior publish is not available in this box."_q);
			}
		} else {
			state.status->setText(
				u"Complete: existing sync data found. Joining existing "
				"sync data is not available yet."_q);
		}
	} else if (result.status == SyncAccountInventoryStatus::NeedsReview) {
		state.status->setText(
			u"Needs review: %1 candidate(s), %2 valid. Cannot safely decide. "
			"Checked %3 messages."_q.arg(
				candidates,
				QString::number(valid),
				scanned));
	} else {
		state.status->setText(
			u"Incomplete: check unfinished or failed. %1 candidate(s) "
			"seen in %2 messages. Cannot decide."_q.arg(candidates, scanned));
	}
	RefreshButtons(state);
}

[[nodiscard]] QString SetupFailure(SyncAccountSetupResult result) {
	switch (result.status) {
	case SyncAccountSetupStatus::AccountUnavailable:
		return u"Account unavailable. Select the account and check again."_q;
	case SyncAccountSetupStatus::IncompleteInventory:
		return u"Inventory incomplete. Check Saved Messages again."_q;
	case SyncAccountSetupStatus::NeedsReview:
		return u"Needs review: local state or Saved Messages changed. "
			"No document was sent."_q;
	case SyncAccountSetupStatus::AccountUnbound:
		return u"Account unbound: local sync state does not match this "
			"account. No document was sent."_q;
	case SyncAccountSetupStatus::StoreError:
		return u"Store error: local sync state could not be opened or "
			"written. No document was sent."_q;
	case SyncAccountSetupStatus::InvalidGeneratedState:
		return u"Could not create a valid local sync identity. "
			"No document was sent."_q;
	case SyncAccountSetupStatus::AlreadyBound:
		return u"Local sync state already exists. Resuming a prior "
			"publish is not available in this box."_q;
	default:
		return u"Local sync setup did not complete. No document was sent."_q;
	}
}

[[nodiscard]] QString PublishResult(SyncConfigPublishResult result) {
	switch (result.status) {
	case SyncConfigPublishStatus::Confirmed:
		return u"Confirmed: one settings record was posted to Saved "
			"Messages and read back (message %1). Continuous sync is off."_q.arg(
				result.messageId);
	case SyncConfigPublishStatus::AlreadySynced:
		return u"The existing own settings record is already confirmed. "
			"Continuous sync is off."_q;
	case SyncConfigPublishStatus::OutcomeUnknown:
		return u"Outcome unknown: Telegram may have accepted the document. "
			"Check Saved Messages again before any retry."_q;
	case SyncConfigPublishStatus::NeedsReview:
		return u"Needs review: the local or cloud record is ambiguous. "
			"No automatic retry was made."_q;
	case SyncConfigPublishStatus::AccountUnbound:
		return u"Account unbound: the local sync state no longer matches "
			"this account. Publishing stopped."_q;
	case SyncConfigPublishStatus::StoreError:
		return u"Store error: the local sync state could not be safely "
			"updated. Check Saved Messages before any retry."_q;
	case SyncConfigPublishStatus::AccountUnavailable:
		return u"Account unavailable. Publishing stopped; check Saved "
			"Messages before any retry."_q;
	case SyncConfigPublishStatus::CloneDetected:
		return u"Needs review: this install's sync identity appears "
			"duplicated. Publishing stopped."_q;
	case SyncConfigPublishStatus::InvalidSettings:
		return u"Current settings are missing, too large, or invalid. "
			"No document was sent."_q;
	case SyncConfigPublishStatus::Incomplete:
		return u"Inventory incomplete. Check Saved Messages again."_q;
	case SyncConfigPublishStatus::Cancelled:
		return u"Publishing cancelled. Check Saved Messages before "
			"trying again."_q;
	}
	return u"Publishing stopped. Check Saved Messages before retrying."_q;
}

void ConfirmPublish(not_null<Ui::GenericBox*> box, SetupBoxState *state) {
	RefreshButtons(*state);
	if (state->publish->isDisabled() || !state->completed) {
		return;
	}
	const auto chosenAccount = state->selectedAccount;
	const auto chosenSession = state->selectedSession;
	box->uiShow()->showBox(Ui::MakeConfirmBox({
		.text = u"Send one copy of the current settings to this "
			"account's Saved Messages? The file may contain chat IDs "
			"and names. Telegram stores Saved Messages in its cloud; "
			"it is not end-to-end encrypted, and every signed-in session "
			"can read it. This does not enable continuous sync."_q,
		.confirmed = crl::guard(box, [=](Fn<void()> close) {
			close();
			RefreshButtons(*state);
			if (state->publish->isDisabled()
				|| !state->completed
				|| chosenAccount.get() != state->selectedAccount.get()
				|| chosenSession.get() != state->selectedSession.get()) {
				return;
			}
			const auto account = chosenAccount.get();
			const auto session = chosenSession.get();
			if (!account || !session || account->loggingOut()
				|| account->maybeSession() != session) {
				StopForLostAccount(*state);
				return;
			}
			auto inventory = std::move(*state->completed);
			state->completed.reset();
			const auto setup = InitializeSyncAccountLocally(
				*account,
				*session,
				inventory);
			if (setup.status != SyncAccountSetupStatus::Ready) {
				state->status->setText(SetupFailure(setup));
				RefreshButtons(*state);
				return;
			}
			state->publishing = true;
			state->status->setText(
				u"Publishing one settings record to Saved Messages."_q);
			RefreshButtons(*state);
			state->publisher = std::make_unique<SyncConfigPublish>(
				*account,
				*session,
				std::move(inventory),
				crl::guard(box, [=](SyncConfigPublishResult result) {
					if (!state->publishing) {
						return;
					}
					state->publishing = false;
					state->status->setText(PublishResult(result));
					RefreshButtons(*state);
				}));
			state->publisher->Start();
		}),
		.confirmText = u"Send one record"_q,
	}));
}

} // namespace

void SyncSetupBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"Sync across devices"_q));
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			u"This is a manual one-time settings publish, not continuous sync. "
			"Checking reads the selected account's Saved Messages. After an "
			"empty, complete check, you can choose to send one settings record. "
			"The settings file may contain chat IDs and names. Saved Messages "
			"is a Telegram cloud chat, not end-to-end encrypted, and every "
			"signed-in session can read the record. state.toml stays local. "
			"Nothing is sent without a separate confirmation."_q,
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

	group->setChangedCallback([=](int value) {
		if (value < 0 || value >= int(state->choices.size())) {
			return;
		}
		state->completed.reset();
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
		state->status->setText(u"Ready to check Saved Messages."_q);
		RefreshButtons(*state);
	});

	const auto startCheck = [=](bool publishAfterCheck) {
		const auto account = state->selectedAccount.get();
		const auto session = state->selectedSession.get();
		if (!account || !session || account->loggingOut()
			|| account->maybeSession() != session) {
			StopForLostAccount(*state);
			return;
		}
		state->completed.reset();
		state->inventory.reset();
		state->running = true;
		state->scanned = 0;
		state->status->setText(publishAfterCheck
			? u"Rechecking Saved Messages before publishing: 0 scanned."_q
			: u"Checking Saved Messages: 0 scanned."_q);
		RefreshButtons(*state);
		const auto chosenAccount = state->selectedAccount;
		const auto chosenSession = state->selectedSession;
		state->inventory = std::make_unique<SyncAccountInventory>(
			session,
			crl::guard(box, [=](uint64_t count) {
				if (state->running) {
					state->scanned = count;
					state->status->setText(
						u"Checking Saved Messages: %1 scanned."_q.arg(
							QString::number(count)));
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
				ShowResult(*state, std::move(result));
				if (publishAfterCheck && state->completed) {
					ConfirmPublish(box, state);
				}
			}));
		state->inventory->Start();
	};
	state->check = box->addButton(
		rpl::single(u"Check Saved Messages"_q),
		[=] { startCheck(false); });
	state->cancel = box->addButton(
		rpl::single(u"Cancel check"_q),
		[=] {
			if (state->running && state->inventory) {
				state->completed.reset();
				state->running = false;
				state->inventory->Cancel();
				state->status->setText(
					u"Check cancelled. Check Saved Messages again."_q);
				RefreshButtons(*state);
			}
		});
	state->publish = box->addButton(
		rpl::single(u"Publish settings once"_q),
		[=] {
			RefreshButtons(*state);
			if (state->publish->isDisabled() || !state->completed) {
				return;
			}
			startCheck(true);
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
		state->completed.reset();
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
