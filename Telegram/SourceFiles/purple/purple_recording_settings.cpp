/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_recording_settings.h"

#include "purple/purple_config.h"
#include "purple/purple_recording_bin_box.h"
#include "purple/purple_recording_registry.h"
#include "purple/purple_recording_store.h"

#include "lang/lang_keys.h"
#include "settings/settings_builder.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "window/window_session_controller.h"
#include "styles/style_settings.h"

namespace Purple {
namespace {

using namespace RecordingRegistry;

[[nodiscard]] QString KeepText(int seconds) {
	if (!seconds) {
		return tr::lng_purple_recording_bin_keep_never(tr::now);
	} else if (seconds % (24 * 3600) == 0 && seconds >= 7 * 24 * 3600) {
		return tr::lng_days(tr::now, lt_count, seconds / (24 * 3600));
	} else if (seconds % 3600 == 0) {
		return tr::lng_hours(tr::now, lt_count, seconds / 3600);
	}
	return tr::lng_seconds(tr::now, lt_count, seconds);
}

[[nodiscard]] rpl::producer<int> KeepValue() {
	return rpl::single(rpl::empty) | rpl::then(
		SettingsChanges()
	) | rpl::map([] {
		return ActiveSettings().recording.binKeepSeconds;
	}) | rpl::distinct_until_changed();
}

void WriteRecording(const QString &key, const QString &value, const QString &what) {
	WriteSettings([=](const QString &text) {
		return SetTableString(
			text,
			SettingsFilePath(),
			u"recording"_q,
			key,
			value);
	}, what);
}

void ChooseKeepBox(not_null<Ui::GenericBox*> box) {
	auto options = std::vector<QString>();
	for (const auto &choice : kBinKeepChoices) {
		options.push_back(KeepText(choice.seconds));
	}
	const auto current = ChoiceIndex(
		kBinKeepChoices,
		ActiveSettings().recording.binKeepSeconds);
	SingleChoiceBox(box, {
		.title = tr::lng_purple_recording_bin_keep(),
		.options = options,
		.initialSelection = current,
		.callback = [=](int index) {
			WriteRecording(
				u"bin_keep"_q,
				QString::fromLatin1(kBinKeepChoices[index].setting),
				u"the recording bin keep time"_q);
		},
	});
}

void SetKeepDiscarded(bool value) {
	WriteSettings([=](const QString &text) {
		return SetTableBool(
			text,
			SettingsFilePath(),
			u"recording"_q,
			u"keep_discarded_p"_q,
			value);
	}, u"keeping discarded recordings"_q);
}

} // namespace

void BuildMediaRecordingSettings(::Settings::Builder::SectionBuilder &builder) {
	const auto controller = builder.controller();
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"advanced/purple_media_recording"_q,
		.title = tr::lng_purple_media_recording(),
		.keywords = { u"purple"_q, u"recording"_q, u"voice"_q, u"round"_q },
	});
	builder.addButton({
		.id = u"advanced/purple_recording_bin_keep"_q,
		.title = tr::lng_purple_recording_bin_keep(),
		.st = &st::settingsButtonNoIcon,
		.label = KeepValue() | rpl::map(KeepText),
		.onClick = [=] {
			if (controller) {
				controller->show(Box(ChooseKeepBox));
			}
		},
		.keywords = { u"purple"_q, u"recording"_q, u"bin"_q, u"keep"_q },
	});
	const auto keep = builder.addButton({
		.id = u"advanced/purple_recording_keep_discarded"_q,
		.title = tr::lng_purple_recording_keep_discarded(),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(rpl::empty) | rpl::then(
			SettingsChanges()
		) | rpl::map([] { return ActiveSettings().recording.keepDiscarded; }),
		.keywords = { u"purple"_q, u"recording"_q, u"discard"_q, u"bin"_q },
	});
	if (keep) {
		keep->toggledValue() | rpl::filter([](bool value) {
			return value != ActiveSettings().recording.keepDiscarded;
		}) | rpl::on_next(SetKeepDiscarded, keep->lifetime());
	}
	builder.addButton({
		.id = u"advanced/purple_recording_bin"_q,
		.title = tr::lng_purple_recording_bin(),
		.st = &st::settingsButtonNoIcon,
		.label = rpl::single(rpl::empty) | rpl::then(
			RecordingBinChanges()
		) | rpl::map([] {
			const auto count = RecordingBinCount();
			return count ? QString::number(count) : QString();
		}),
		.onClick = [=] {
			if (controller) {
				controller->show(Box(RecordingBinBox, controller));
			}
		},
		.keywords = { u"purple"_q, u"recording"_q, u"bin"_q, u"crash"_q },
	});
	builder.addDividerText(tr::lng_purple_recording_bin_about());
	builder.addSkip();
}

} // namespace Purple
