/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_instant_replaces.h"

#include "purple/purple_settings_chat.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "lang/lang_keys.h"
#include "settings/settings_builder.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace Purple {
namespace {

constexpr auto kReplaceDashesPref = "purple-replace-dashes"_cs;

} // namespace

bool ReplaceDashes() {
	return Core::App().settings().readPref<bool>(kReplaceDashesPref, false);
}

void SetReplaceDashes(bool value) {
	Core::App().settings().writePref<bool>(kReplaceDashesPref, value);
	Core::App().saveSettingsDelayed();
}

bool InstantReplaceAllowed(const QString &what) {
	return (what != u"--"_q) || ReplaceDashes();
}

void TextOnlyFollowToggles(not_null<Ui::InputField*> field) {
	field->setInstantReplaces(Ui::InstantReplaces::TextOnly());
	field->setInstantReplacesEnabled(
		Core::App().settings().replaceEmojiValue(),
		Core::App().settings().systemTextReplaceValue());
}

void AddReplaceDashesSearchEntry(::Settings::Builder::SectionBuilder &builder) {
	using namespace ::Settings::Builder;
	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"chat/replace-dashes"_q,
			.title = tr::lng_settings_replace_dashes(tr::now),
			.keywords = { u"replace"_q, u"hyphens"_q, u"dash"_q },
			.checkIcon = ReplaceDashes()
				? SearchEntryCheckIcon::Checked
				: SearchEntryCheckIcon::Unchecked,
		};
	});
}

void AddReplaceDashesCheckbox(
		not_null<Ui::VerticalLayout*> inner,
		::Settings::HighlightRegistry *highlights) {
	const auto replaceDashes = inner->add(
		object_ptr<Ui::Checkbox>(
			inner,
			tr::lng_settings_replace_dashes(tr::now),
			ReplaceDashes(),
			st::settingsCheckbox),
		st::settingsCheckboxPadding);
	replaceDashes->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		SetReplaceDashes(checked);
	}, inner->lifetime());
	Core::App().settings().replaceEmojiValue(
	) | rpl::on_next([=](bool enabled) {
		replaceDashes->setDisabled(!enabled);
	}, replaceDashes->lifetime());
	if (highlights) {
		highlights->push_back({ u"chat/replace-dashes"_q, {
			replaceDashes,
			{ .radius = st::boxRadius },
		} });
	}
}

} // namespace Purple
