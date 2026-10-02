/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_version.h"

#include "lang/lang_keys.h"
#include "purple/purple_build_info.h"
#include "settings/settings_builder.h"
#include "window/window_session_controller.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include "styles/style_settings.h"

namespace Purple {
namespace {

constexpr auto kVersion = "1.0.0";

void AddVersionButton(
		::Settings::Builder::SectionBuilder &builder,
		const QString &id) {
	const auto controller = builder.controller();
	builder.addButton({
		.id = id,
		.title = rpl::single(VersionText()),
		.st = &st::settingsButtonNoIcon,
		.onClick = [=] {
			QGuiApplication::clipboard()->setText(VersionText());
			if (controller) {
				controller->showToast(tr::lng_text_copied(tr::now));
			}
		},
		.keywords = {
			u"purple"_q,
			u"version"_q,
			u"commit"_q,
			u"build"_q,
		},
	});
}

} // namespace

QString VersionText() {
	return FormatVersion(
		QString::fromLatin1(kVersion),
		QString::fromLatin1(BuildInfo::kDesktopCommit),
		BuildInfo::kDesktopDirty,
		QString::fromLatin1(BuildInfo::kCoreCommit));
}

void AddVersionRow(::Settings::Builder::SectionBuilder &builder) {
	AddVersionButton(builder, u"advanced/purple_version"_q);
}

void AddVersionFooter(::Settings::Builder::SectionBuilder &builder) {
	builder.addDivider();
	builder.addSkip();
	AddVersionButton(builder, u"main/purple_version"_q);
	builder.addSkip();
}

} // namespace Purple
