/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_settings_section.h"

#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "platform/platform_file_utilities.h"
#include "ui/delayed_activation.h"
#include <crl/crl_on_main.h>
#include "data/data_user.h"
#include "data/data_changes.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "purple/purple_config.h"
#include "purple/purple_archive_service.h"
#include "purple/purple_archive_purge_box.h"
#include "purple/purple_birthday_reminders.h"
#include "purple/purple_account_appwide_removal.h"
#include "purple/purple_device.h"
#include "purple/purple_diagnostics_ui.h"
#include "purple/purple_emoji_export.h"
#include "purple/purple_control.h"
#include "purple/purple_last_seen.h"
#include "purple/purple_keys.h"
#include "purple/purple_settings_customize.h"
#include "purple/purple_privacy_screen.h"
#include "purple/purple_media_playback.h"
#include "purple/purple_music_section.h"
#include "purple/purple_recording_settings.h"
#include "purple/purple_media_reply_settings.h"
#include "purple/purple_preset_box.h"
#include "purple/purple_schedule.h"
#include "purple/purple_schedule_box.h"
#include "purple/purple_screentime_box.h"
#include "purple/purple_sync.h"
#include "purple/purple_sync_account_binding.h"
#include "purple/purple_sync_auto_service.h"
#include "purple/purple_sync_magic_service.h"
#include "purple/purple_sync_setup_box.h"
#include "purple/purple_version.h"
#include "settings/settings_builder.h"
#include "settings/settings_common_session.h"
#include "settings/sections/settings_main.h"
#include "settings/sections/settings_local_storage.h"
#include "settings/settings_search.h"
#include "ui/layers/generic_box.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include <QtCore/QPointer>
#include <memory>
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace Purple {
namespace {

using namespace Settings::Builder;

rpl::producer<> AccountRowChanges() {
	auto sessions = rpl::single(rpl::empty)
		| rpl::then(Core::App().domain().accountsChanges())
		| rpl::map([] {
			rpl::producer<> events = rpl::single(rpl::empty);
			for (const auto &entry : Core::App().domain().accounts()) {
				events = rpl::merge(std::move(events), entry.account->sessionChanges() | rpl::to_empty);
			}
			return events;
		}) | rpl::flatten_latest();
	auto names = std::move(sessions) | rpl::map([] {
		rpl::producer<> events = rpl::single(rpl::empty);
		for (const auto &entry : Core::App().domain().accounts()) {
			if (const auto session = entry.account->maybeSession()) {
				events = rpl::merge(std::move(events), session->changes().realtimeNameUpdates() | rpl::to_empty);
			}
		}
		return events;
	}) | rpl::flatten_latest();
	return rpl::merge(SettingsChanges(), std::move(names));
}

void AddAccountScopeCaption(
		SectionBuilder &builder, std::optional<AccountSettingsRow> row = std::nullopt) {
	const auto weak = base::make_weak(builder.session().get());
	const auto caption = [=] {
		const auto live = weak.get();
		return live ? AccountSettingsScopeCaption(live, row) : QString();
	};
	builder.scope([&] {
		builder.addDividerText(AccountRowChanges() | rpl::map(caption));
	}, AccountRowChanges() | rpl::map([=] { return !caption().isEmpty(); }));
}

void AddSettingsOwnerLink(SectionBuilder &builder, const QString &id, AccountSettingsRow row) {
	const auto weakSession = base::make_weak(builder.session().get());
	const auto weakController = base::make_weak(builder.controller());
	const auto needsOwner = [=] {
		const auto live = weakSession.get();
		return live && AccountSettingsRowFor(live, row).availability
			== AccountSettingsRowAvailability::NeedsOwner;
	};
	builder.addControl({
		.factory = [=](not_null<Ui::VerticalLayout*> parent) -> object_ptr<Ui::RpWidget> {
			auto link = object_ptr<Ui::LinkButton>(parent, tr::lng_purple_settings_owner_box_choose(tr::now));
			link->setClickedCallback([=] {
				const auto live = weakSession.get();
				const auto window = weakController.get();
				if (live && window && &window->session() == live && needsOwner()) {
					window->show(Box(ChooseSettingsOwnerBox, std::make_optional(SessionAccountKey(live))));
				}
			});
			return std::move(link);
		},
		.id = id + u"_owner"_q,
		.title = tr::lng_purple_settings_owner_box_choose(),
		.margin = st::settingsButtonNoIcon.padding,
		.shown = AccountRowChanges() | rpl::map(needsOwner),
	});
}

void BuildAccountSettingsRow(
		SectionBuilder &builder,
		const QString &id,
		rpl::producer<QString> title,
		AccountSettingsRow row) {
	const auto refresh = std::make_shared<rpl::event_stream<>>();
	const auto weakSession = base::make_weak(builder.session().get());
	const auto controller = builder.controller();
	const auto weakController = controller ? base::make_weak(controller)
		: base::weak_ptr<Window::SessionController>();
	const auto state = [=] {
		const auto live = weakSession.get();
		return live ? AccountSettingsRowFor(live, row) : AccountSettingsRowState();
	};
	// A row that cannot be written keeps showing the value in effect, with the
	// switch locked and disabled, and says why in the caption below. It has no
	// right label: SettingsButton places one at the right edge, under the switch.
	const auto shown = [=] { return AccountSettingsRowShownValue(state()); };
	const auto toggle = builder.addButton({
		.id = id,
		.title = std::move(title),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::merge(AccountRowChanges(), refresh->events())
			| rpl::map(shown),
		.keywords = row == AccountSettingsRow::SendAfterSave
			? QStringList{ u"purple"_q, u"sync"_q, u"saved"_q, u"send"_q, u"backup"_q }
			: QStringList{ u"purple"_q, u"last"_q, u"seen"_q, u"privacy"_q },
	});
	if (toggle) {
		const auto button = QPointer<Ui::SettingsButton>(toggle);
		const auto windowCurrent = [=] {
			const auto live = weakSession.get();
			const auto window = weakController.get();
			return button && live && window && &window->session() == live;
		};
		AccountRowChanges() | rpl::on_next([=] {
			const auto writable = state().target.has_value();
			toggle->setToggleLocked(!writable);
			toggle->setEnabled(windowCurrent() && writable);
		}, toggle->lifetime());
		toggle->toggledValue() | rpl::filter([=](bool value) {
			return value != shown();
		}) | rpl::on_next([=](bool value) {
			const auto target = windowCurrent() ? state().target : std::nullopt;
			if (!target) { refresh->fire({}); return; }
			crl::on_main([=] {
				if (!windowCurrent()) { return; }
				const auto result = WriteAccountSettingsRow(weakSession.get(), *target, value, windowCurrent);
				if (windowCurrent() && !result.ok) {
					refresh->fire({});
					if (!windowCurrent()) { return; }
					weakController.get()->show(Ui::MakeInformBox(tr::lng_purple_settings_row_changed(tr::now)));
				}
			});
		}, toggle->lifetime());
	}
	AddAccountScopeCaption(builder, row);
	// The two last-seen rows sit together, so only the second offers it.
	if (row != AccountSettingsRow::LastSeenReasons) {
		AddSettingsOwnerLink(builder, id, row);
	}
	if (row == AccountSettingsRow::SendAfterSave) { return; }
	builder.addControl({
		.factory = [=](not_null<Ui::VerticalLayout*> parent) -> object_ptr<Ui::RpWidget> {
			auto link = object_ptr<Ui::LinkButton>(parent, QString());
			const auto button = QPointer<Ui::LinkButton>(link.data());
			const auto windowCurrent = [=] {
				const auto live = weakSession.get();
				const auto window = weakController.get();
				return button && live && window && &window->session() == live;
			};
			AccountRowChanges() | rpl::on_next([=] {
				if (!button) { return; }
				const auto live = weakSession.get();
				button->setText(live ? AccountSettingsResetCaption(live) : QString());
				button->setEnabled(windowCurrent() && state().target.has_value());
			}, link->lifetime());
			link->setClickedCallback([=] {
				const auto live = weakSession.get();
				const auto target = windowCurrent() && AccountSettingsResetVisible(live, row)
					? state().target : std::nullopt;
				if (!target) { return; }
				crl::on_main([=] {
					if (!windowCurrent()) { return; }
					const auto result = WriteAccountSettingsRow(
						weakSession.get(), *target, std::nullopt, windowCurrent);
					if (windowCurrent() && !result.ok) {
						weakController.get()->show(Ui::MakeInformBox(tr::lng_purple_settings_row_changed(tr::now)));
					}
				});
			});
			return std::move(link);
		},
		.id = id + u"_reset"_q,
		.title = AccountRowChanges() | rpl::map([=] {
			const auto live = weakSession.get();
			return live ? AccountSettingsResetCaption(live) : QString();
		}),
		.margin = st::settingsButtonNoIcon.padding,
		.shown = AccountRowChanges() | rpl::map([=] {
			const auto live = weakSession.get();
			return live && AccountSettingsResetVisible(live, row);
		}),
	});
}

void BuildPurpleSettingsFiles(SectionBuilder &builder) {
	const auto controller = builder.controller();
	const auto weakController = base::make_weak(controller);
	const auto weakSession = base::make_weak(builder.session().get());
	const auto windowCurrent = [=] {
		const auto window = weakController.get();
		const auto session = weakSession.get();
		return window && session && &window->session() == session;
	};
	builder.addSubsectionTitle({
		.id = u"advanced/purple_settings_files"_q,
		.title = tr::lng_purple_settings_files(),
		.keywords = { u"purple"_q, u"settings"_q, u"file"_q, u"account"_q },
	});
	builder.addButton({
		.id = u"advanced/purple_global_account"_q,
		.title = AccountRowChanges() | rpl::map([=] {
			const auto live = weakSession.get();
			return live ? GlobalAccountChoiceTitle(live) : QString();
		}),
		.label = tr::lng_purple_global_change(),
		.onClick = [=] {
			if (!windowCurrent()) { return; }
			weakController.get()->show(Box(ChooseGlobalAccountBox, weakController.get()));
		},
		.keywords = { u"purple"_q, u"account"_q, u"global"_q, u"app-wide"_q },
		.shown = AccountRowChanges() | rpl::map([=] {
			const auto live = weakSession.get();
			return live && GlobalAccountChoiceVisible(live);
		}),
	});
	builder.addDividerText(AccountRowChanges() | rpl::map([=] {
		return windowCurrent() ? AccountListFileWarnings(weakSession.get()) : QString();
	}));
	for (const auto role : { SettingsOpenRole::Own, SettingsOpenRole::AppWide }) {
		const auto target = [=] {
			const auto live = weakSession.get();
			return live ? AccountSettingsOpenTargetFor(live, role) : std::nullopt;
		};
		const auto label = [=] {
			const auto selected = target();
			return selected ? selected->file.path : tr::lng_purple_settings_open_unavailable(tr::now);
		};
		const auto title = [=] {
			const auto selected = target();
			const auto live = weakSession.get();
			if (role == SettingsOpenRole::Own) {
				const auto name = live ? live->user()->name() : QString();
				return selected && selected->requester == selected->global
					? tr::lng_purple_settings_open_appwide(tr::now, lt_account, name)
					: tr::lng_purple_settings_open_own(tr::now, lt_account, name);
			}
			const auto global = live ? AppWideSettingsAccount(live) : std::nullopt;
			const auto account = global
				? AccountKeyText(global->environment, global->userId) : std::nullopt;
			return tr::lng_purple_settings_open_appwide(tr::now, lt_account,
				account.value_or(QString()));
		};
		builder.addButton({
			.id = role == SettingsOpenRole::Own
				? u"advanced/purple_open_account_settings"_q : u"advanced/purple_open_appwide_settings"_q,
			.title = rpl::single(rpl::empty) | rpl::then(SettingsChanges()) | rpl::map(title),
			.label = rpl::single(rpl::empty) | rpl::then(SettingsChanges()) | rpl::map(label),
			.onClick = [=] {
				if (!windowCurrent()) { return; }
				const auto selected = target();
				if (!selected) {
					if (const auto window = weakController.get(); window && AccountSettingsNeedOwner()) {
						ShowAccountSettingsRefusal(window->uiShow(), AccountSettingsNeedsOwnerText(),
							&window->session(), &window->window());
					} else if (window) {
						window->show(Ui::MakeInformBox(tr::lng_purple_settings_open_unavailable(tr::now)));
					}
					return;
				}
				const auto result = PrepareAccountSettingsOpen(weakSession.get(), *selected, windowCurrent);
				if (!windowCurrent()) { return; }
				if (!result.target || !ValidateAccountSettingsOpenTarget(weakSession.get(), *result.target)) {
					weakController.get()->show(Ui::MakeInformBox(
						result.status == SettingsOpenStatus::ReadError
							? tr::lng_purple_settings_open_read_error(tr::now)
							: result.status == SettingsOpenStatus::CreateError
							? tr::lng_purple_settings_open_create_error(tr::now)
							: tr::lng_purple_settings_open_changed(tr::now)));
					return;
				}
				const auto ready = *result.target;
				crl::on_main([=] {
					if (!windowCurrent() || !ValidateAccountSettingsOpenTarget(weakSession.get(), ready)) {
						return;
					}
					Ui::PreventDelayedActivation();
					Platform::File::UnsafeLaunch(ready.file.path);
				});
			},
			.keywords = { u"purple"_q, u"settings"_q, u"file"_q, u"open"_q, u"account"_q },
			.shown = rpl::single(rpl::empty) | rpl::then(SettingsChanges()) | rpl::map([=] {
				const auto live = weakSession.get();
				const auto global = live ? AppWideSettingsAccount(live) : std::nullopt;
				return role == SettingsOpenRole::Own
					|| !global || (live && SessionAccountKey(live) != *global);
			}),
		});
	}
	builder.scope([&] {
		builder.addDividerText(AccountRowChanges() | rpl::map([=] {
			return windowCurrent() ? InactiveAppWideSettingsSummary(weakSession.get()) : QString();
		}));
		builder.addButton({
			.id = u"advanced/purple_remove_appwide_settings"_q,
			.title = AccountRowChanges() | rpl::map([=] {
				return RemoveAccountAppWideSettingsTitle(weakSession.get());
			}),
			.onClick = [=] {
				if (!windowCurrent()) { return; }
				const auto target = AccountAppWideRemovalFor(weakSession.get());
				if (!target || target->removed.empty()) { return; }
				weakController.get()->show(Box(RemoveAccountAppWideSettingsBox, weakController.get(), *target));
			},
			.keywords = { u"purple"_q, u"remove"_q, u"app-wide"_q, u"account"_q },
		});
	}, AccountRowChanges() | rpl::map([=] {
		const auto target = windowCurrent() ? AccountAppWideRemovalFor(weakSession.get()) : std::nullopt;
		return target && !target->removed.empty();
	}));
}

// Purple Telegram, not upstream. Why a coarse "last seen" is coarse, and the
// one-off peek that reads an exact one. See docs/purple/work_mode.md.
void BuildPurpleLastSeenSection(SectionBuilder &builder) {
	const auto window = base::make_weak(builder.controller());
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"advanced/purple_last_seen"_q,
		.title = tr::lng_purple_settings_last_seen(),
		.keywords = { u"purple"_q, u"last"_q, u"seen"_q, u"online"_q },
	});

	BuildAccountSettingsRow(builder, u"advanced/purple_last_seen_reasons"_q,
		tr::lng_purple_settings_last_seen_reasons(), AccountSettingsRow::LastSeenReasons);
	BuildAccountSettingsRow(builder, u"advanced/purple_last_seen_peek"_q,
		tr::lng_lastseen_peek_setting(), AccountSettingsRow::LastSeenPeek);

	const auto confirm = builder.addButton({
		.id = u"advanced/purple_last_seen_peek_confirm"_q,
		.title = tr::lng_lastseen_peek_confirm_setting(),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(!Purple::SkipLastSeenPeekConfirmation()),
		.keywords = {
			u"purple"_q,
			u"last"_q,
			u"seen"_q,
			u"peek"_q,
			u"confirm"_q,
		},
	});
	if (confirm) {
		confirm->toggledValue(
		) | rpl::on_next([](bool value) {
			const auto skip = !value;
			if (skip != Purple::SkipLastSeenPeekConfirmation()) {
				Purple::SetSkipLastSeenPeekConfirmation(skip);
			}
		}, confirm->lifetime());
	}

	{
		builder.addButton({
			.id = u"advanced/purple_last_seen_peeks"_q,
			.title = tr::lng_lastseen_peeks_title(),
			.st = &st::settingsButtonNoIcon,
			.onClick = [=] {
				if (const auto live = window.get()) {
					live->show(Box(Purple::LastSeenPeeksBox, &live->session()));
				}
			},
			.keywords = {
				u"purple"_q,
				u"last"_q,
				u"seen"_q,
				u"peek"_q,
				u"log"_q,
			},
		});
	}

	builder.addDividerText(tr::lng_lastseen_peek_settings_about());
	builder.addSkip();
}

// Purple Telegram, not upstream. How long the app has had you, out of a log
// this machine keeps and never sends. See docs/purple/work_mode.md.
void BuildPurpleScreenTimeSection(SectionBuilder &builder) {
	const auto window = base::make_weak(builder.controller());
	const auto session = builder.session();
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"advanced/purple_screen_time"_q,
		.title = tr::lng_purple_settings_screen_time(),
		.keywords = { u"purple"_q, u"screen"_q, u"time"_q },
	});

	// The digest on the row is the whole feature for most days: a line that
	// answers the question without opening anything.
	builder.addButton({
		.id = u"advanced/purple_screen_time_open"_q,
		.title = tr::lng_purple_settings_screen_time(),
		.st = &st::settingsButtonNoIcon,
		.label = Purple::ScreenTimeDigestValue(session),
		.onClick = [=] {
			if (const auto live = window.get()) {
				live->show(Box(Purple::ScreenTimeBox, &live->session()));
			}
		},
		.keywords = {
			u"purple"_q,
			u"screen"_q,
			u"time"_q,
			u"usage"_q,
			u"budget"_q,
			u"limit"_q,
		},
	});

	AddAccountScopeCaption(builder);
	builder.addDividerText(rpl::single(u"Off until you switch it on. Once on, "
		"one line per event - a chat opening, an action in the composer, the "
		"app leaving the front - is appended to screentime.log beside "
		"settings.toml, and everything on the screen is derived from it when "
		"you look, so changing a threshold re-reads the history you already "
		"have. The file never leaves this machine. Budgets are written in "
		"settings.toml: a soft one says so once at the limit, a hard one puts "
		"a cover over the chat with one snooze on it, and neither touches "
		"messages or notifications."_q));
	builder.addSkip();
}

[[nodiscard]] auto CurrentSettingsWindow(SectionBuilder &builder) {
	const auto window = base::make_weak(builder.controller());
	const auto session = base::make_weak(builder.session().get());
	return [=]() -> Window::SessionController* {
		const auto own = window.get();
		const auto live = session.get();
		return own && live && &own->session() == live
			&& own->window().sessionController() == own
			&& !live->account().loggingOut()
			&& live->account().maybeSession() == live
			&& !own->window().locked()
			&& !Core::App().screenIsLocked()
			&& !PrivacyScreenActiveAny()
			? own
			: nullptr;
	};
}

auto ArchiveSummaryValue(not_null<Main::Session*> session, bool compact = false) {
	return rpl::single(rpl::empty) | rpl::then(ArchiveStorageChanges())
		| rpl::map([weak = base::make_weak(session.get()), compact] {
			const auto live = weak.get();
			return !live ? QString()
				: compact ? ArchiveStorageSizeLabel(live) : ArchiveStorageSummary(live);
		});
}

void OpenArchiveCleanup(
		not_null<Window::SessionController*> window,
		ArchivePurgeKind kind,
		Fn<Window::SessionController*()> current) {
	ShowArchivePurgeBox(
		window,
		kind,
		{ SessionAccountKey(&window->session()) },
		tr::lng_purple_archive_all_chats(tr::now),
		[current] { return current() != nullptr; });
}

void ShowCleanupReview(
		not_null<Window::SessionController*> window,
		Fn<Window::SessionController*()> current) {
	window->show(Box<Ui::GenericBox>([=](not_null<Ui::GenericBox*> box) {
		const auto live = current();
		if (!live) { return; }
		box->setTitle(tr::lng_purple_settings_archive_cleanup());
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			ArchiveSummaryValue(&live->session()),
			st::boxLabel));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box, tr::lng_purple_archive_purge_scope_notice(), st::boxLabel));
		const auto guard = QPointer<Ui::GenericBox>(box);
		for (const auto kind : { ArchivePurgeKind::Trashbin, ArchivePurgeKind::EditHistory }) {
			box->addButton(
				kind == ArchivePurgeKind::Trashbin
					? tr::lng_purple_archive_empty_trash()
					: tr::lng_purple_archive_clear_edits(),
				[guard, current, kind] {
					const auto resolve = current;
					if (guard) { guard->closeBox(); }
					if (const auto own = resolve()) {
						OpenArchiveCleanup(own, kind, resolve);
					}
				});
		}
		box->addLeftButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void BuildArchiveSettings(SectionBuilder &builder) {
	const auto current = CurrentSettingsWindow(builder);
	const auto session = builder.session();
	builder.addSkip();
	builder.addSubsectionTitle(tr::lng_purple_settings_archive_account(
		lt_account, rpl::single(session->user()->name())));
	builder.addDividerText(ArchiveSummaryValue(session));
	builder.addButton({
		.id = u"purple/archive_cleanup"_q,
		.title = tr::lng_purple_settings_archive_cleanup(),
		.icon = { &st::menuIconStorage },
		.onClick = [=] {
			if (const auto own = current()) { ShowCleanupReview(own, current); }
		},
		.keywords = { u"disk"_q, u"archive"_q, u"cleanup"_q, u"GC"_q, u"reclaim"_q },
	});
	for (const auto kind : { ArchivePurgeKind::Trashbin, ArchivePurgeKind::EditHistory }) {
		const auto trash = (kind == ArchivePurgeKind::Trashbin);
		builder.addButton({
			.id = trash ? u"advanced/purple_archive_trash"_q : u"advanced/purple_archive_edits"_q,
			.title = trash ? tr::lng_purple_archive_empty_trash() : tr::lng_purple_archive_clear_edits(),
			.onClick = [=] {
				if (const auto own = current()) { OpenArchiveCleanup(own, kind, current); }
			},
			.keywords = trash
				? QStringList{ u"archive"_q, u"trash"_q, u"deleted"_q }
				: QStringList{ u"archive"_q, u"edited"_q, u"versions"_q },
		});
	}
	builder.addSectionButton({
		.title = tr::lng_purple_settings_archive_cache(),
		.targetSection = ::Settings::LocalStorageId(),
		.icon = { &st::menuIconStorage },
	});
	builder.addDividerText(tr::lng_purple_settings_archive_cache_about());
	builder.addSkip();
	builder.addButton({
		.id = u"advanced/purple_archive_discard"_q,
		.title = tr::lng_purple_archive_discard(),
		.onClick = [=] {
			const auto own = current();
			if (!own) { return; }
			const auto discard = ArchiveDiscardAction(&own->session());
			const auto confirm = own->show(Ui::MakeConfirmBox({
				.text = tr::lng_purple_archive_discard_confirm(tr::now),
				.confirmed = [=](Fn<void()> close) {
					close();
					if (current()) { discard(); }
				},
				.confirmText = tr::lng_purple_archive_discard_button(tr::now),
			}));
			if (confirm) {
				own->window().sessionControllerValue() | rpl::skip(1)
					| rpl::on_next([=] {
						if (confirm && !current()) { confirm->closeBox(); }
					}, confirm->lifetime());
			}
		},
		.keywords = { u"archive"_q, u"discard"_q, u"remove"_q, u"disk"_q },
	});
	builder.addSkip();
}

void BuildWorkActivitySettings(SectionBuilder &builder) {
	const auto current = CurrentSettingsWindow(builder);
	const auto origin = base::make_weak(builder.session().get());
	builder.addSkip();
	builder.addButton({
		.id = u"advanced/purple_work_mode"_q,
		.title = tr::lng_purple_settings_work_mode(),
		.icon = { &st::menuIconTagFilter },
		.onClick = [=] {
			if (const auto own = current()) {
				own->show(Box(Purple::PresetBox, &own->session()));
			}
		},
		.keywords = { u"purple"_q, u"work"_q, u"mode"_q, u"preset"_q, u"focus"_q, u"hide"_q },
	});
	builder.addButton({
		.id = u"advanced/purple_account_work_mode"_q,
		.title = AccountRowChanges() | rpl::map([=] {
			const auto live = origin.get();
			return live ? AccountWorkModeTitle(live) : QString();
		}),
		.label = AccountRowChanges() | rpl::map([=] {
			const auto live = origin.get();
			return live ? AccountListsSummary(live) : QString();
		}),
		.onClick = [=] {
			if (const auto own = current()) {
				own->show(Box(ChooseAccountWorkModeBox, not_null{ own }));
			}
		},
		.keywords = { u"purple"_q, u"lists"_q, u"stock"_q, u"follow"_q },
		.shown = AccountRowChanges() | rpl::map([=] {
			const auto live = origin.get();
			return live && (AccountWorkModeChoiceVisible(live)
				|| AccountWorkModeForSession(live) == AccountWorkMode::Stock);
		}),
	});
	AddAccountScopeCaption(builder);
	builder.addButton({
		.id = u"advanced/purple_schedule"_q,
		.title = tr::lng_purple_settings_schedule(),
		.icon = { &st::menuIconSchedule },
		.onClick = [=] {
			if (const auto own = current()) { own->show(Box(Purple::ScheduleBox)); }
		},
		.keywords = { u"purple"_q, u"schedule"_q, u"work"_q, u"hours"_q, u"ruleset"_q, u"timetable"_q },
	});
	BuildPurpleScreenTimeSection(builder);
	AddBirthdayReminderSettings(builder);
	builder.addSkip();
}

void BuildPurpleMediaSettings(SectionBuilder &builder) {
	const auto current = CurrentSettingsWindow(builder);
	BuildMediaPlaybackSettings(builder);
	BuildMediaRecordingSettings(builder);
	BuildMediaReplySettings(builder);
	builder.addSkip();
	builder.addButton({
		.id = u"advanced/purple_music_source_folder"_q,
		.title = tr::lng_purple_music_source(),
		.onClick = [=] {
			if (const auto own = current()) { ChooseMusicSourceFolder(own); }
		},
		.keywords = { u"music"_q, u"playlist"_q, u"folder"_q },
	});
	const auto hideChats = builder.addButton({
		.id = u"advanced/purple_music_hide_chats"_q,
		.title = tr::lng_purple_music_hide_chats(),
		.toggled = MusicHideChatsValue(),
		.keywords = { u"music"_q, u"sidebar"_q },
	});
	if (hideChats) {
		hideChats->toggledValue() | rpl::on_next(SetMusicHideChats, hideChats->lifetime());
	}
	BuildEmojiExportSetting(builder);
	builder.addSkip();
}

void BuildPurplePrivacySettings(SectionBuilder &builder) {
	builder.addSkip();
	BuildPrivacyScreenSetting(builder);
	BuildPurpleLastSeenSection(builder);
	const auto toggle = builder.addButton({
		.id = u"advanced/purple_premium_toggle"_q,
		.title = tr::lng_purple_settings_local_premium(),
		.st = &st::settingsButtonNoIcon,
		.toggled = Purple::LocalPremiumValue(),
		.keywords = { u"purple"_q, u"premium"_q, u"ads"_q, u"translate"_q },
	});
	if (toggle) {
		toggle->toggledValue() | rpl::filter([](bool value) {
			return value != Purple::LocalPremium();
		}) | rpl::on_next(Purple::SetLocalPremium, toggle->lifetime());
	}
	AddAccountScopeCaption(builder);
	builder.addDividerText(rpl::single(u"Unlocks the Premium features that "
		"Telegram Desktop gates on the client alone: no sponsored messages, "
		"exact \"last seen\" times, real-time chat translation, and up to six "
		"accounts instead of three. Features the server enforces stay locked. "
		"Stored in "_q + Purple::SettingsFilePath() + '.'));
	builder.addSkip();
	BuildModalKeysSetting(builder);
	BuildControlSettings(builder);
}

void BuildPurpleAccountsSettings(SectionBuilder &builder) {
	const auto current = CurrentSettingsWindow(builder);
	const auto live = base::make_weak(builder.session().get());
	builder.addSkip();
	BuildPurpleSettingsFiles(builder);
	builder.addButton({
		.id = u"advanced/purple_settings_owner"_q,
		.title = tr::lng_purple_settings_file_owner(),
		.label = rpl::single(SettingsOwnerStatus()),
		.onClick = [=] {
			if (const auto own = current()) {
				own->show(Box(ChooseSettingsOwnerBox,
					std::make_optional(SessionAccountKey(&own->session()))));
			}
		},
		.keywords = { u"settings"_q, u"owner"_q, u"app-wide"_q, u"account"_q },
	});
	AddAccountScopeCaption(builder);
	builder.addButton({
		.id = u"advanced/purple_this_device"_q,
		.title = tr::lng_purple_settings_this_device(),
		.st = &st::settingsButtonNoIcon,
		.onClick = [=] {
			if (const auto own = current()) { own->show(Box(Purple::DeviceLabelBox)); }
		},
		.keywords = { u"purple"_q, u"device"_q, u"name"_q, u"id"_q },
	});
	const auto currentWindow = [=] { return current() != nullptr; };
	builder.addButton({
		.id = u"advanced/purple_send_settings"_q,
		.title = tr::lng_purple_settings_send_saved(),
		.icon = { &st::menuIconSavedMessages },
		.onClick = [=] {
			if (const auto own = current()) {
				Purple::SendSettingsToSavedMessages(&own->session(), own->uiShow(), currentWindow);
			}
		},
		.keywords = { u"purple"_q, u"sync"_q, u"settings"_q, u"backup"_q, u"saved"_q },
	});
	builder.addButton({
		.id = u"advanced/purple_share_settings"_q,
		.title = tr::lng_purple_transfer_share(),
		.icon = { &st::menuIconDownload },
		.onClick = [=] {
			if (const auto own = current()) {
				Purple::ShareAccountSettingsFile(&own->session(), own->uiShow(), currentWindow);
			}
		},
		.keywords = { u"purple"_q, u"share"_q, u"settings"_q },
	});
	builder.addButton({
		.id = u"advanced/purple_sync_setup"_q,
		.title = tr::lng_purple_settings_sync_devices(),
		.icon = { &st::menuIconSavedMessages },
		.label = rpl::single(rpl::empty)
			| rpl::then(AutomaticSettingsSyncChanges())
			| rpl::map([=] {
				const auto session = live.get();
				return session && AutomaticSettingsSyncEnabled(session)
					? tr::lng_settings_cloud_password_on(tr::now)
					: tr::lng_settings_cloud_password_off(tr::now);
			}),
		.onClick = [=] {
			if (const auto own = current()) { own->show(Box(Purple::SyncSetupBox)); }
		},
		.keywords = { u"purple"_q, u"sync"_q, u"saved"_q, u"devices"_q },
	});
	builder.addButton({
		.id = u"advanced/purple_sync_join"_q,
		.title = rpl::single(u"Not linked to sync"_q),
		.label = rpl::single(u"Join"_q),
		.onClick = [=] {
			if (const auto own = current(); own && SyncMagicSettingsUnlinked(&own->session())) {
				own->show(Box(SyncSetupBoxForAccount, &own->session()));
			}
		},
		.keywords = { u"sync"_q, u"join"_q, u"link"_q },
		.shown = rpl::merge(
			AccountRowChanges(),
			AutomaticSettingsSyncChanges(),
			AccountSyncBindingChanges()
		) | rpl::map([=] {
			const auto session = live.get();
			return session && SyncMagicSettingsUnlinked(session);
		}) | rpl::distinct_until_changed(),
	});
	BuildAccountSettingsRow(builder, u"advanced/purple_send_after_save"_q,
		tr::lng_purple_settings_send_after_save(), AccountSettingsRow::SendAfterSave);
	builder.addSkip();
}

void BuildPurpleDiagnosticsSettings(SectionBuilder &builder) {
	const auto current = CurrentSettingsWindow(builder);
	builder.addSkip();
	builder.addDividerText(tr::lng_purple_settings_diagnostics_scope());
	builder.addButton({
		.id = u"advanced/purple_log_export"_q,
		.title = tr::lng_purple_settings_diagnostics_export(),
		.onClick = [=] {
			if (const auto live = current()) { Diagnostics::ShowExport(&live->window()); }
		},
		.keywords = { u"purple"_q, u"log"_q, u"diagnostics"_q, u"export"_q, u"debug"_q, u"notices"_q },
	});
	builder.addButton({
		.id = u"advanced/purple_log_clear"_q,
		.title = tr::lng_purple_settings_diagnostics_clear(),
		.onClick = [=] {
			if (const auto live = current()) { Diagnostics::ShowClear(&live->window()); }
		},
		.keywords = { u"purple"_q, u"log"_q, u"diagnostics"_q, u"clear"_q, u"debug"_q },
	});
	builder.addSkip();
}

class PurpleSettings final : public ::Settings::Search {
public:
	PurpleSettings(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] static ::Settings::Type Id() {
		return ::Settings::SectionFactory<PurpleSettings>::Instance();
	}

	[[nodiscard]] rpl::producer<QString> title() override {
		return tr::lng_purple_settings_title();
	}
};

template <void (*Build)(SectionBuilder &), const tr::phrase<> *Title>
class PurpleSubsection final : public ::Settings::Section<PurpleSubsection<Build, Title>> {
	using Section = ::Settings::Section<PurpleSubsection<Build, Title>>;

public:
	PurpleSubsection(
		QWidget *parent,
		not_null<Window::SessionController*> controller)
	: Section(parent, controller) {
		const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
		this->build(content, meta().build);
		Ui::ResizeFitChild(this, content);
	}

	[[nodiscard]] rpl::producer<QString> title() override {
		return (*Title)();
	}

	static const BuildHelper &meta();
};

using ArchiveSettings = PurpleSubsection<BuildArchiveSettings, &tr::lng_purple_settings_archive>;
using WorkActivitySettings = PurpleSubsection<BuildWorkActivitySettings, &tr::lng_purple_settings_work_activity>;
using MediaSettings = PurpleSubsection<BuildPurpleMediaSettings, &tr::lng_purple_settings_media>;
using PrivacySettings = PurpleSubsection<BuildPurplePrivacySettings, &tr::lng_purple_settings_privacy_controls>;
using AccountsSettings = PurpleSubsection<BuildPurpleAccountsSettings, &tr::lng_purple_settings_accounts_sync>;
using DiagnosticsSettings = PurpleSubsection<BuildPurpleDiagnosticsSettings, &tr::lng_purple_settings_diagnostics>;

struct PageInfo {
	::Settings::Type id;
	const tr::phrase<> *title;
	const style::icon *icon;
	QStringList keywords;
};

const auto kPages = std::array{
	PageInfo{ ArchiveSettings::Id(), &tr::lng_purple_settings_archive, &st::menuIconStorage,
		{ u"disk"_q, u"usage"_q, u"archive"_q, u"storage"_q } },
	PageInfo{ WorkActivitySettings::Id(), &tr::lng_purple_settings_work_activity, &st::menuIconTagFilter,
		{ u"focus"_q, u"schedule"_q, u"usage"_q } },
	PageInfo{ MediaSettings::Id(), &tr::lng_purple_settings_media, &st::menuIconVideoChat,
		{ u"playback"_q, u"recording"_q, u"music"_q, u"reply"_q } },
	PageInfo{ PrivacySettings::Id(), &tr::lng_purple_settings_privacy_controls, &st::menuIconLock,
		{ u"privacy"_q, u"keybindings"_q, u"automation"_q } },
	PageInfo{ AccountsSettings::Id(), &tr::lng_purple_settings_accounts_sync, &st::menuIconDevices,
		{ u"owner"_q, u"toml"_q, u"sync"_q } },
	PageInfo{ DiagnosticsSettings::Id(), &tr::lng_purple_settings_diagnostics, &st::menuIconGroupLog,
		{ u"diagnostic"_q, u"debug"_q, u"logs"_q } },
};

template <void (*Build)(SectionBuilder &), const tr::phrase<> *Title>
const BuildHelper &PurpleSubsection<Build, Title>::meta() {
	static const auto meta = [] {
		const auto page = ranges::find(kPages, Title, &PageInfo::title);
		Assert(page != kPages.end());
		return BuildHelper({
			.id = page->id,
			.parentId = PurpleSettings::Id(),
			.title = page->title,
			.icon = page->icon,
			.keywords = page->keywords,
			.tabStops = true,
		}, Build);
	}();
	return meta;
}

const auto kPageMetadata = std::array{
	&ArchiveSettings::meta(),
	&WorkActivitySettings::meta(),
	&MediaSettings::meta(),
	&PrivacySettings::meta(),
	&AccountsSettings::meta(),
	&DiagnosticsSettings::meta(),
};

void BuildPurpleSection(SectionBuilder &builder) {
	const auto current = CurrentSettingsWindow(builder);
	builder.addSkip();
	const auto customize = builder.addButton({
		.id = u"advanced/purple_customize_settings"_q,
		.title = tr::lng_purple_settings_customize(),
		.icon = { &st::menuIconSettings },
		.keywords = { u"purple"_q, u"settings"_q, u"customize"_q, u"file"_q },
	});
	if (customize) {
		customize->addClickHandler([=] {
			if (const auto own = current()) {
				ShowCustomizeSettings(&own->window(), customize);
			}
		});
	}
	builder.addDivider();
	builder.addSkip();
	for (const auto &page : kPages) {
		builder.addSectionButton({
			.title = (*page.title)(),
			.targetSection = page.id,
			.icon = { page.icon },
			.label = page.id == ArchiveSettings::Id()
				? ArchiveSummaryValue(builder.session(), true)
				: rpl::producer<QString>(),
		});
	}
	builder.addDivider();
	builder.addSkip();
	Purple::AddVersionRow(builder);
	builder.addSkip();
}

const auto PurpleSettingsMeta = BuildHelper({
	.id = PurpleSettings::Id(),
	.parentId = ::Settings::MainId(),
	.title = &tr::lng_purple_settings_title,
	.icon = &st::menuIconManage,
	.keywords = { u"purple"_q, u"work mode"_q, u"sync"_q },
	.tabStops = true,
}, BuildPurpleSection);

PurpleSettings::PurpleSettings(
		QWidget *parent,
		not_null<Window::SessionController*> controller)
: Search(parent, controller, { Id(), PurpleSettingsMeta.build }) {
}

} // namespace

void AddSettingsSection(::Settings::Builder::SectionBuilder &builder) {
	builder.addSectionButton({
		.title = tr::lng_purple_settings_title(),
		.targetSection = PurpleSettings::Id(),
		.icon = { &st::menuIconManage },
		.keywords = { u"purple"_q, u"work mode"_q, u"sync"_q },
	});
}

} // namespace Purple
