/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common.h"

namespace Settings::Builder {
class SectionBuilder;
} // namespace Settings::Builder

namespace Purple {

void AddReplaceDashesSearchEntry(::Settings::Builder::SectionBuilder &builder);
void AddReplaceDashesCheckbox(
	not_null<Ui::VerticalLayout*> inner,
	::Settings::HighlightRegistry *highlights);

} // namespace Purple
