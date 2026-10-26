#include "purple/purple_archive_purge_box.h"

#include "base/weak_ptr.h"
#include "data/data_user.h"
#include "data/data_changes.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "purple/purple_archive_service.h"
#include "purple/purple_config.h"
#include "purple/purple_gate.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QPointer>

#include "styles/style_layers.h"

namespace Purple {
namespace {

void BindArchivePurgeScopeChanges(not_null<Main::Session*> session,
		std::function<bool()> current, std::shared_ptr<std::atomic<bool>> permitted,
		Fn<void()> close, rpl::lifetime &lifetime) {
	session->changes().realtimePeerUpdates(Data::PeerUpdate::Flag::Migration)
		| rpl::on_next([=] {
			if (!current()) {
				permitted->store(false);
				close();
			}
		}, lifetime);
}

}

void ShowArchivePurgeBox(not_null<Window::SessionController *> controller,
	ArchivePurgeKind kind,
	ArchivePurgeScope scope,
	QString scopeName,
	std::function<bool()> scopeCurrent) {
	const auto session = base::make_weak(&controller->session());
	const auto window = base::make_weak(controller.get());
	const auto source = CachedAppWideSettingsAccount(&controller->session());
	const auto mode = AccountWorkModeForSession(&controller->session());
	const auto account = scope.account;
	if (!scopeCurrent || !scopeCurrent()
		|| account != SessionAccountKey(&controller->session())) {
		return;
	}
	controller->show(Box<Ui::GenericBox>([=](not_null<Ui::GenericBox *>
												 box) {
		box->setTitle(kind == ArchivePurgeKind::Trashbin
						  ? tr::lng_purple_archive_empty_trash()
						  : tr::lng_purple_archive_clear_edits());
		box->setWidth(st::boxWideWidth);
		box->addRow(object_ptr<Ui::FlatLabel>(box,
			tr::lng_purple_archive_deleted_scope(tr::now,
				lt_account,
				session.get()->user()->name(),
				lt_chat,
				scopeName),
			st::boxLabel));
		box->addRow(object_ptr<Ui::FlatLabel>(box,
			kind == ArchivePurgeKind::Trashbin
				? tr::lng_purple_archive_empty_trash_notice(tr::now)
				: tr::lng_purple_archive_clear_edits_notice(tr::now),
			st::boxLabel));
		box->addRow(object_ptr<Ui::FlatLabel>(box,
			tr::lng_purple_archive_purge_scope_notice(tr::now),
			st::boxLabel));
		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
			box, tr::lng_purple_archive_opening(tr::now), st::boxLabel));
		struct State {
			std::optional<ArchivePurgeConfirmation> confirmation;
			std::optional<ArchiveStoreEpoch> epoch;
			std::shared_ptr<std::atomic<bool>> permitted
				= std::make_shared<std::atomic<bool>>(true);
			QPointer<Ui::RoundButton> submit;
			uint64_t ticket = 0;
			bool pending = false;
			bool committed = false;
			bool closed = false;
			Fn<void()> review;
		};
		const auto state = box->lifetime().make_state<State>();
		const auto guard = QPointer<Ui::GenericBox>(box);
		const auto current = [=] {
			const auto live = session.get();
			const auto own = window.get();
			return guard && !state->closed && live && own
				   && &own->session() == live
				   && own->window().sessionController() == own
				   && !live->account().loggingOut()
				   && live->account().maybeSession() == live
				   && SessionAccountKey(live) == account
				   && CachedAppWideSettingsAccount(live) == source
				   && AccountWorkModeForSession(live) == mode
				   && scopeCurrent();
		};
		const auto permitted = state->permitted;
		BindArchivePurgeScopeChanges(session.get(), current, permitted, [guard] {
			if (guard) guard->closeBox();
		}, box->lifetime());
		box->lifetime().add([permitted] { permitted->store(false); });
		box->boxClosing()
			| rpl::on_next(
				[=] {
					state->closed = true;
					permitted->store(false);
					++state->ticket;
				},
				box->lifetime());
		rpl::merge(SettingsChanges(), ActiveChanges())
			| rpl::on_next(
				[=] {
					if (!current()) {
						permitted->store(false);
						box->closeBox();
					}
				},
				box->lifetime());
		controller->window().sessionControllerChanges()
			| rpl::on_next(
				[=](Window::SessionController *now) {
					if (now != window.get()) {
						permitted->store(false);
						box->closeBox();
					}
				},
				box->lifetime());
		session.get()->lifetime().add([guard, permitted] {
			permitted->store(false);
			if (guard) {
				guard->closeBox();
			}
		});
		state->review = [=] {
			if (!current() || state->pending || state->committed) {
				return;
			}
			state->confirmation.reset();
			state->submit->setDisabled(true);
			state->pending = true;
			const auto ticket = ++state->ticket;
			PrepareArchivePurge(session.get(),
				kind,
				scope,
				[=](DesktopArchivePurgePreview preview) {
					if (!current() || ticket != state->ticket) {
						return;
					}
					state->pending = false;
					if (CanRunArchiveWork(preview.epoch, preview.epoch)) {
						state->epoch = preview.epoch;
					}
					if (preview.maintenance) {
						state->committed = true;
						label->setText(
							tr::lng_purple_archive_purge_resume_notice(
								tr::now)
							+ u"\n"_q
							+ ArchiveStorageSummary(session.get()));
						return;
					}
					if (preview.status != ArchiveQueryStatus::Ready
						|| !preview.confirmation) {
						label->setText(
							preview.status == ArchiveQueryStatus::Busy
								? tr::lng_purple_archive_query_busy(tr::now)
							: preview.status == ArchiveQueryStatus::ReadOnly
								? tr::lng_purple_archive_query_future(
									  tr::now)
							: preview.status
									== ArchiveQueryStatus::SchemaMismatch
								? tr::lng_purple_archive_query_schema(
									  tr::now)
								: tr::lng_purple_archive_purge_refused(
									  tr::now));
						return;
					}
					state->confirmation = *preview.confirmation;
					label->setText(
						tr::lng_purple_archive_purge_counts(tr::now,
							lt_messages,
							QString::number(preview.counts[0]),
							lt_deleted,
							QString::number(preview.counts[1]),
							lt_versions,
							QString::number(preview.counts[2]),
							lt_prior,
							QString::number(preview.counts[3])));
					state->submit->setDisabled(false);
				});
		};
		state->submit = box->addButton(
			tr::lng_purple_archive_purge_confirm(), [=] {
				if (!current() || state->pending || state->committed
					|| !state->confirmation) {
					return;
				}
				state->pending = true;
				state->submit->setDisabled(true);
				const auto ticket = ++state->ticket;
				ExecuteArchivePurge(session.get(),
					*state->confirmation,
					permitted,
					[=](DesktopArchiveStatus status) {
						if (!current() || ticket != state->ticket) {
							return;
						}
						state->pending = false;
						state->confirmation.reset();
						state->committed
							= status == DesktopArchiveStatus::Maintenance
							  || status
									 == DesktopArchiveStatus::
										 MaintenanceHeld;
						label->setText(
							state->committed
								? status == DesktopArchiveStatus::Maintenance
									  ? tr::lng_purple_archive_purge_running(
											tr::now)
									  : tr::lng_purple_archive_purge_held(
											tr::now)
							: status == DesktopArchiveStatus::Busy
								? tr::lng_purple_archive_query_busy(tr::now)
								: tr::lng_purple_archive_purge_stale(
									  tr::now));
					});
			});
		box->addButton(
			tr::lng_purple_archive_purge_review(), [=] { state->review(); });
		box->addButton(tr::lng_purple_archive_purge_retry(), [=] {
			if (!current() || state->pending || !state->epoch) {
				return;
			}
			state->pending = true;
			state->confirmation.reset();
			state->submit->setDisabled(true);
			const auto ticket = ++state->ticket;
			RetryArchiveMaintenance(session.get(),
				*state->epoch,
				permitted,
				[=](DesktopArchiveStatus status) {
					if (!current() || ticket != state->ticket) {
						return;
					}
					state->pending = false;
					state->committed
						= status != DesktopArchiveStatus::Stale
						  && status != DesktopArchiveStatus::Busy;
					label->setText(
						state->committed
							? ArchiveStorageSummary(session.get())
							: tr::lng_purple_archive_purge_stale(tr::now));
				});
		});
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		ArchiveStorageChanges()
			| rpl::on_next(
				[=] {
					if (current() && state->committed) {
						label->setText(ArchiveStorageSummary(session.get()));
					}
				},
				box->lifetime());
		state->review();
	}));
}

}
