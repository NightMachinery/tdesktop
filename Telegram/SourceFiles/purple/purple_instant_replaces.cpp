/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_instant_replaces.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "ui/widgets/fields/input_field.h"

namespace Purple {
namespace {

const Ui::InstantReplaces &WithoutDashes(InstantReplacesType type) {
	static const auto defaultReplaces = [] {
		auto result = Ui::InstantReplaces::Default();
		result.add(u"--"_q, QString());
		return result;
	}();
	static const auto textOnlyReplaces = [] {
		auto result = Ui::InstantReplaces::TextOnly();
		result.add(u"--"_q, QString());
		return result;
	}();
	return (type == InstantReplacesType::Default)
		? defaultReplaces
		: textOnlyReplaces;
}

const Ui::InstantReplaces &WithDashes(InstantReplacesType type) {
	return (type == InstantReplacesType::Default)
		? Ui::InstantReplaces::Default()
		: Ui::InstantReplaces::TextOnly();
}

} // namespace

void InstallInstantReplaces(
		not_null<Ui::InputField*> field,
		InstantReplacesType type) {
	Core::App().settings().replaceDashesValue(
	) | rpl::on_next([=](bool replaceDashes) {
		field->setInstantReplaces(replaceDashes
			? WithDashes(type)
			: WithoutDashes(type));
	}, field->lifetime());
}

} // namespace Purple
