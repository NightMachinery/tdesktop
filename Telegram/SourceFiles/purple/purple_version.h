/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_types.h"

#include <QtCore/QString>

namespace Settings::Builder {
class SectionBuilder;
} // namespace Settings::Builder

namespace Purple {

[[nodiscard]] inline QString FormatVersion(
		const QString &version,
		const QString &desktopCommit,
		bool desktopDirty,
		const QString &coreCommit) {
	return u"Purple %1 (%2%3, core %4)"_q.arg(
		version,
		desktopCommit,
		desktopDirty ? u"+dirty"_q : QString(),
		coreCommit);
}

[[nodiscard]] QString VersionText();

void AddVersionRow(::Settings::Builder::SectionBuilder &builder);
void AddVersionFooter(::Settings::Builder::SectionBuilder &builder);

} // namespace Purple
