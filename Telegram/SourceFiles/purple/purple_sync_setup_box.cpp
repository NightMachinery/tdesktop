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
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"

#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace Purple {
namespace {

struct AccountChoice {
	base::weak_ptr<Main::Account> account;
	base::weak_ptr<Main::Session> session;
};

struct State {
	std::vector<AccountChoice> choices;
	std::shared_ptr<Ui::RadiobuttonGroup> group;
	base::weak_ptr<Main::Account> selectedAccount;
	base::weak_ptr<Main::Session> selectedSession;
	std::unique_ptr<SyncAccountInventory> inventory;
	Ui::FlatLabel *status = nullptr;
	Ui::RoundButton *check = nullptr;
	Ui::RoundButton *cancel = nullptr;
	uint64_t scanned = 0;
	int selectedIndex = -1;
	bool running = false;
};

[[nodiscard]] QString AccountLabel(not_null<Main::Session*> session) {
	const auto user = session->user();
	const auto username = user->username();
	return username.isEmpty()
		? user->name()
		: u"%1 (@%2)"_q.arg(user->name(), username);
}

void RefreshButtons(State &state) {
	const auto account = state.selectedAccount.get();
	const auto session = state.selectedSession.get();
	const auto available = account
		&& session
		&& !account->loggingOut()
		&& account->maybeSession() == session;
	state.check->setDisabled(state.running || !available);
	state.cancel->setDisabled(!state.running);
}

void StopForLostAccount(State &state) {
	const auto wasRunning = state.running;
	state.selectedIndex = -1;
	state.selectedAccount = {};
	state.selectedSession = {};
	state.running = false;
	state.group->setValue(-1);
	if (wasRunning && state.inventory) {
		state.inventory->Cancel();
	}
	state.status->setText(
		u"Incomplete: the selected account is unavailable. Choose another "
		"account or reopen this box, then check again."_q);
	RefreshButtons(state);
}

void ShowResult(State &state, SyncAccountInventoryResult result) {
	state.running = false;
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

} // namespace

void SyncSetupBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"Sync across devices"_q));
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			u"Sync is not enabled here. A future setup would use the selected "
			"account's Saved Messages. Your settings and future playlist "
			"metadata would be stored in Telegram's cloud and readable by "
			"every signed-in session. state.toml stays on this device. This "
			"check only reads "
			"Saved Messages; it does not post, delete, or enable sync."_q,
			st::boxLabel),
		st::boxRowPadding);

	const auto state = box->lifetime().make_state<State>();
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
		if (state->running && state->inventory) {
			state->inventory->Cancel();
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

	state->check = box->addButton(
		rpl::single(u"Check Saved Messages"_q),
		[=] {
			const auto account = state->selectedAccount.get();
			const auto session = state->selectedSession.get();
			if (!account || !session || account->loggingOut()
				|| account->maybeSession() != session) {
				StopForLostAccount(*state);
				return;
			}
			state->inventory.reset();
			state->running = true;
			state->scanned = 0;
			state->status->setText(u"Checking Saved Messages: 0 scanned."_q);
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
				}));
			state->inventory->Start();
		});
	state->cancel = box->addButton(
		rpl::single(u"Cancel check"_q),
		[=] {
			if (state->running && state->inventory) {
				state->inventory->Cancel();
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
		if (state->inventory) {
			state->inventory->Cancel();
		}
	}, box->lifetime());
}

} // namespace Purple
