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
#include "purple/purple_account_appwide_removal.h"
#include "purple/purple_device.h"
#include "purple/purple_emoji_export.h"
#include "purple/purple_last_seen.h"
#include "purple/purple_preset_box.h"
#include "purple/purple_schedule.h"
#include "purple/purple_schedule_box.h"
#include "purple/purple_screentime_box.h"
#include "purple/purple_sync.h"
#include "purple/purple_sync_setup_box.h"
#include "purple/purple_version.h"
#include "settings/settings_builder.h"
#include "ui/layers/generic_box.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include <QtCore/QPointer>
#include <memory>
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
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
	if (controller && row != AccountSettingsRow::LastSeenReasons) {
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
	if (!controller) { return; }
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
			return windowCurrent() ? AccountSettingsOpenTargetFor(live, role) : std::nullopt;
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
			const auto global = windowCurrent() ? AppWideSettingsAccount(live) : std::nullopt;
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
				const auto selected = target();
				if (!selected) {
					if (const auto window = weakController.get(); window && AccountSettingsNeedOwner()) {
						ShowAccountSettingsRefusal(window->uiShow(), AccountSettingsNeedsOwnerText(),
							SessionAccountKey(&window->session()));
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
				const auto global = windowCurrent() ? AppWideSettingsAccount(live) : std::nullopt;
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
	const auto controller = builder.controller();
	const auto session = builder.session();
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"advanced/purple_last_seen"_q,
		.title = rpl::single(u"Last seen"_q),
		.keywords = { u"purple"_q, u"last"_q, u"seen"_q, u"online"_q },
	});

	BuildAccountSettingsRow(builder, u"advanced/purple_last_seen_reasons"_q,
		rpl::single(u"Say why a last seen is hidden"_q), AccountSettingsRow::LastSeenReasons);
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

	if (controller) {
		builder.addButton({
			.id = u"advanced/purple_last_seen_peeks"_q,
			.title = tr::lng_lastseen_peeks_title(),
			.st = &st::settingsButtonNoIcon,
			.onClick = [=] {
				controller->show(Box(Purple::LastSeenPeeksBox, session));
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
	const auto controller = builder.controller();
	const auto session = builder.session();
	if (!controller) {
		return;
	}
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"advanced/purple_screen_time"_q,
		.title = rpl::single(u"Screen time"_q),
		.keywords = { u"purple"_q, u"screen"_q, u"time"_q },
	});

	// The digest on the row is the whole feature for most days: a line that
	// answers the question without opening anything.
	builder.addButton({
		.id = u"advanced/purple_screen_time_open"_q,
		.title = rpl::single(u"Screen time"_q),
		.st = &st::settingsButtonNoIcon,
		.label = Purple::ScreenTimeDigestValue(session),
		.onClick = [=] {
			controller->show(Box(Purple::ScreenTimeBox, session));
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

// Purple Telegram, not upstream. See docs/purple/premium.md and
// docs/purple/work_mode.md.
void BuildPurpleSection(SectionBuilder &builder) {
	const auto controller = builder.controller();
	const auto session = builder.session();
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"advanced/purple"_q,
		.title = rpl::single(u"Purple"_q),
		.keywords = { u"purple"_q, u"premium"_q, u"work"_q },
	});
	Purple::AddVersionRow(builder);
	const auto archiveSession = base::make_weak(session.get());
	builder.addDividerText(rpl::single(rpl::empty) | rpl::then(ArchiveStorageChanges()) | rpl::map([archiveSession] {
		const auto live = archiveSession.get();
		return live ? tr::lng_purple_archive_storage(tr::now) + u"\n"_q + ArchiveStorageSummary(live) : QString();
	}));
	if (controller) {
		const auto window = base::make_weak(controller);
		for (const auto kind : {ArchivePurgeKind::Trashbin, ArchivePurgeKind::EditHistory}) {
			builder.addButton({
				.id = kind == ArchivePurgeKind::Trashbin ? u"advanced/purple_archive_trash"_q : u"advanced/purple_archive_edits"_q,
				.title = kind == ArchivePurgeKind::Trashbin ? tr::lng_purple_archive_empty_trash() : tr::lng_purple_archive_clear_edits(),
				.onClick = [archiveSession, window, kind] {
					const auto live = archiveSession.get();
					const auto own = window.get();
					if (live && own && &own->session() == live) {
						ShowArchivePurgeBox(own, kind, {SessionAccountKey(live)},
							tr::lng_purple_archive_all_chats(tr::now), [] { return true; });
					}
				},
			});
		}
		builder.addButton({
			.id = u"advanced/purple_archive_discard"_q,
			.title = tr::lng_purple_archive_discard(),
			.onClick = [archiveSession, window] {
				const auto live = archiveSession.get();
				const auto ownWindow = window.get();
				if (!live || !ownWindow || &ownWindow->session() != live) {
					return;
				}
				const auto discard = ArchiveDiscardAction(live);
				ownWindow->show(Ui::MakeConfirmBox({
					.text = tr::lng_purple_archive_discard_confirm(tr::now),
					.confirmed = [archiveSession, window, discard](Fn<void()> close) {
						close();
						const auto live = archiveSession.get();
						const auto ownWindow = window.get();
						if (live && ownWindow && &ownWindow->session() == live) {
							discard();
						}
					},
					.confirmText = tr::lng_purple_archive_discard_button(tr::now),
				}));
			},
		});
	}
	BuildPurpleSettingsFiles(builder);
	if (controller) {
		const auto window = base::make_weak(controller);
		const auto origin = base::make_weak(session.get());
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
				const auto live = origin.get();
				const auto ownWindow = window.get();
				if (live && ownWindow && &ownWindow->session() == live) {
					ownWindow->show(Box(ChooseAccountWorkModeBox, not_null{ ownWindow }));
				}
			},
			.keywords = { u"purple"_q, u"lists"_q, u"stock"_q, u"follow"_q },
			.shown = AccountRowChanges() | rpl::map([=] {
				const auto live = origin.get();
				return live && (AccountWorkModeChoiceVisible(live)
					|| AccountWorkModeForSession(live) == AccountWorkMode::Stock);
			}),
		});
	}
	if (controller) {
		builder.addButton({
			.id = u"advanced/purple_settings_owner"_q,
			.title = tr::lng_purple_settings_file_owner(),
			.label = rpl::single(SettingsOwnerStatus()),
			.onClick = [=] { controller->show(Box(ChooseSettingsOwnerBox, std::make_optional(SessionAccountKey(&controller->session())))); },
		});
	}

	// The main menu carries it too, and that is where it belongs for something
	// switched several times a day. But Settings is where people look for a
	// feature they have heard of and cannot find, and searching "work mode"
	// finding nothing is how a feature stays unused.
	if (controller) {
		builder.addButton({
			.id = u"advanced/purple_work_mode"_q,
			.title = rpl::single(u"Work Mode"_q),
			.icon = { &st::menuIconTagFilter },
			.onClick = [=] {
				controller->show(Box(Purple::PresetBox, session));
			},
			.keywords = {
				u"purple"_q,
				u"work"_q,
				u"mode"_q,
				u"preset"_q,
				u"focus"_q,
				u"hide"_q,
			},
		});
	}

	AddAccountScopeCaption(builder);

	// The schedule was file-only until now, which meant the one feature that
	// changes the app without being asked was also the one with nothing on
	// screen to read it back from. The status line under it is what the
	// picker shows too, so the two cannot drift.
	if (controller) {
		builder.addButton({
			.id = u"advanced/purple_schedule"_q,
			.title = rpl::single(u"Schedule"_q),
			.icon = { &st::menuIconSchedule },
			.onClick = [=] {
				controller->show(Box(Purple::ScheduleBox));
			},
			.keywords = {
				u"purple"_q,
				u"schedule"_q,
				u"work"_q,
				u"hours"_q,
				u"ruleset"_q,
				u"timetable"_q,
			},
		});
	}

	// One settings.toml is meant to travel between machines, so each one has to
	// be able to say which it is. The id is what a ruleset names; the name is
	// what every device reading the file calls it.
	if (controller) {
		builder.addButton({
			.id = u"advanced/purple_this_device"_q,
			.title = rpl::single(u"This device"_q),
			.st = &st::settingsButtonNoIcon,
			.onClick = [=] {
				controller->show(Box(Purple::DeviceLabelBox));
			},
			.keywords = { u"purple"_q, u"device"_q, u"name"_q, u"id"_q },
		});
	}

	// The other half of settings.toml being a plain file: it is per-install,
	// and Saved Messages is the one place both machines can already see.
	// See docs/purple/sync.md.
	if (controller) {
		builder.addButton({
			.id = u"advanced/purple_send_settings"_q,
			.title = rpl::single(u"Send settings to Saved Messages"_q),
			.icon = { &st::menuIconSavedMessages },
			.onClick = [=] {
				Purple::SendSettingsToSavedMessages(
					session,
					controller->uiShow(),
					[weak = base::make_weak(controller), session] {
						return weak && &weak->session() == session;
					});
			},
			.keywords = {
				u"purple"_q,
				u"sync"_q,
				u"settings"_q,
				u"backup"_q,
				u"saved"_q,
			},
		});
	}
	if (controller) {
		builder.addButton({
			.id = u"advanced/purple_share_settings"_q,
			.title = tr::lng_purple_transfer_share(),
			.icon = { &st::menuIconDownload },
			.onClick = [=] {
				Purple::ShareAccountSettingsFile(session, controller->uiShow(),
					[weak = base::make_weak(controller), session] {
						return weak && &weak->session() == session;
					});
			},
			.keywords = { u"purple"_q, u"share"_q, u"settings"_q },
		});
	}
	if (controller) {
		builder.addButton({
			.id = u"advanced/purple_sync_setup"_q,
			.title = rpl::single(u"Sync across devices"_q),
			.icon = { &st::menuIconSavedMessages },
			.label = rpl::single(u"Off"_q),
			.onClick = [=] {
				controller->show(Box(Purple::SyncSetupBox));
			},
			.keywords = {
				u"purple"_q,
				u"sync"_q,
				u"saved"_q,
				u"devices"_q,
			},
		});
	}

	// Opt-in, and it stays opt-in: this posts a document to a real chat, and
	// an upgrade that started doing that on somebody's behalf would be doing
	// it behind their back. See ShouldAutoSend() for what stops two machines
	// from handing the same file back and forth.
	BuildAccountSettingsRow(builder, u"advanced/purple_send_after_save"_q,
		rpl::single(u"Send to Saved Messages after every save"_q), AccountSettingsRow::SendAfterSave);

	const auto toggle = builder.addButton({
		.id = u"advanced/purple_premium_toggle"_q,
		.title = rpl::single(u"Local Premium features"_q),
		.st = &st::settingsButtonNoIcon,
		.toggled = Purple::LocalPremiumValue(),
		.keywords = { u"purple"_q, u"premium"_q, u"ads"_q, u"translate"_q },
	});

	if (toggle) {
		toggle->toggledValue(
		) | rpl::filter([=](bool value) {
			return (value != Purple::LocalPremium());
		}) | rpl::on_next([=](bool value) {
			Purple::SetLocalPremium(value);
		}, toggle->lifetime());
	}

	AddAccountScopeCaption(builder);
	builder.addDividerText(rpl::single(u"Unlocks the Premium features that "
		"Telegram Desktop gates on the client alone: no sponsored messages, "
		"exact \"last seen\" times, real-time chat translation, and up to six "
		"accounts instead of three. Features the server enforces stay locked. "
		"Stored in "_q + Purple::SettingsFilePath() + '.'));
	builder.addSkip();

	BuildEmojiExportSetting(builder);
	BuildPurpleLastSeenSection(builder);
	BuildPurpleScreenTimeSection(builder);
}

} // namespace

void BuildAdvancedSection(::Settings::Builder::SectionBuilder &builder) {
	BuildPurpleSection(builder);
}

} // namespace Purple
