/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_settings_section.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "purple/purple_config.h"
#include "purple/purple_device.h"
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
#include "ui/widgets/buttons.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Purple {
namespace {

using namespace Settings::Builder;

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

	const auto flag = [&](
			const QString &id,
			const QString &title,
			bool (*read)(),
			const QString &key,
			const QString &what) {
		const auto toggle = builder.addButton({
			.id = id,
			.title = rpl::single(title),
			.st = &st::settingsButtonNoIcon,
			.toggled = rpl::single(
				rpl::empty
			) | rpl::then(
				Purple::SettingsChanges()
			) | rpl::map(read),
			.keywords = {
				u"purple"_q,
				u"last"_q,
				u"seen"_q,
				u"privacy"_q,
			},
		});
		if (!toggle) {
			return;
		}
		toggle->toggledValue(
		) | rpl::filter([=](bool value) {
			return (value != read());
		}) | rpl::on_next([=](bool value) {
			Purple::WriteSettings([=](const QString &text) {
				return Purple::SetTableBool(
					text,
					Purple::SettingsFilePath(),
					u"last_seen"_q,
					key,
					value);
			}, what);
		}, toggle->lifetime());
	};

	flag(
		u"advanced/purple_last_seen_reasons"_q,
		u"Say why a last seen is hidden"_q,
		[] { return Purple::ActiveSettings().lastSeen.reasons; },
		u"reasons_p"_q,
		u"the last seen reasons"_q);
	flag(
		u"advanced/purple_last_seen_peek"_q,
		tr::lng_lastseen_peek_setting(tr::now),
		[] { return Purple::ActiveSettings().lastSeen.trade; },
		u"trade_p"_q,
		u"Last Seen Peek"_q);

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
					controller->uiShow());
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
	const auto autoSend = builder.addButton({
		.id = u"advanced/purple_send_after_save"_q,
		.title = rpl::single(u"Send to Saved Messages after every save"_q),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(
			rpl::empty
		) | rpl::then(
			Purple::SettingsChanges()
		) | rpl::map([] {
			return Purple::ActiveSettings().sync.sendAfterSave;
		}),
		.keywords = {
			u"purple"_q,
			u"sync"_q,
			u"saved"_q,
			u"send"_q,
			u"backup"_q,
		},
	});

	if (autoSend) {
		autoSend->toggledValue(
		) | rpl::filter([=](bool value) {
			return (value != Purple::ActiveSettings().sync.sendAfterSave);
		}) | rpl::on_next([=](bool value) {
			Purple::WriteSettings([=](const QString &text) {
				return Purple::SetTableBool(
					text,
					Purple::SettingsFilePath(),
					u"sync"_q,
					u"send_after_save_p"_q,
					value);
			}, u"the automatic send"_q);
		}, autoSend->lifetime());
	}

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

	builder.addDividerText(rpl::single(u"Unlocks the Premium features that "
		"Telegram Desktop gates on the client alone: no sponsored messages, "
		"exact \"last seen\" times, real-time chat translation, and up to six "
		"accounts instead of three. Features the server enforces stay locked. "
		"Stored in "_q + Purple::SettingsFilePath() + '.'));
	builder.addSkip();

	BuildPurpleLastSeenSection(builder);
	BuildPurpleScreenTimeSection(builder);
}

} // namespace

void BuildAdvancedSection(::Settings::Builder::SectionBuilder &builder) {
	BuildPurpleSection(builder);
}

} // namespace Purple
