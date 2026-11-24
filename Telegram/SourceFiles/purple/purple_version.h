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

namespace Ui {
class FlatLabel;
class GenericBox;
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Purple {

[[nodiscard]] inline QString FormatVersion(
		const QString &version,
		const QString &desktopCommit,
		bool desktopDirty,
		const QString &coreCommit,
		bool coreDirty) {
	const auto dirty = [](bool value) {
		return value ? u"+dirty"_q : QString();
	};
	return u"Purple %1 (%2%3, core %4%5)"_q.arg(
		version,
		desktopCommit,
		dirty(desktopDirty),
		coreCommit,
		dirty(coreDirty));
}

[[nodiscard]] inline QString FormatShortVersion(const QString &version) {
	return u"Purple %1"_q.arg(version);
}

[[nodiscard]] inline QString FormatVersionTooltip(
		const QString &purpleVersion,
		const QString &buildDate,
		const QString &qtVersion,
		bool isLinux) {
	return isLinux
		? u"%1\nQt version: %2."_q.arg(purpleVersion, qtVersion)
		: u"%1\nBuild date: %2.\nQt version: %3."_q.arg(
			purpleVersion,
			buildDate,
			qtVersion);
}

[[nodiscard]] QString VersionText();
[[nodiscard]] QString ShortVersionText();
[[nodiscard]] QString VersionTooltip(
	const QString &buildDate,
	const QString &qtVersion);

void AddVersionRow(::Settings::Builder::SectionBuilder &builder);
void AddVersionFooter(::Settings::Builder::SectionBuilder &builder);
void SetupMainMenuVersion(
	Ui::FlatLabel *label,
	Window::SessionController *controller);
void AddAboutVersion(
	Ui::VerticalLayout *layout,
	Ui::GenericBox *box);

} // namespace Purple
