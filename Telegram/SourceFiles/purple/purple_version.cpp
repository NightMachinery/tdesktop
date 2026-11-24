/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_version.h"

#include "base/platform/base_platform_info.h"
#include "base/weak_ptr.h"
#include "boxes/about_box.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "core/update_channel.h"
#include "lang/lang_keys.h"
#include "purple/purple_build_info.h"
#include "purple/purple_notices.h"
#include "settings/settings_builder.h"
#include "ui/basic_click_handlers.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
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
				const auto msg = tr::lng_text_copied(tr::now);
				const auto weakCtrl = base::make_weak(controller);
				ShowPurpleAck({
					.session = &controller->session(),
					.window = &controller->window(),
					.scope = u"Purple version"_q,
					.text = msg,
					.severity = ModalNotices::NoticeSeverity::Info,
					.fallbackToast = [=] { if (const auto c = weakCtrl.get()) { c->showToast(msg); } },
				});
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
		QString::fromLatin1(BuildInfo::kCoreCommit),
		BuildInfo::kCoreDirty);
}

QString ShortVersionText() {
	return FormatShortVersion(QString::fromLatin1(kVersion));
}

QString VersionTooltip(
		const QString &buildDate,
		const QString &qtVersion) {
	return FormatVersionTooltip(
		VersionText(),
		buildDate,
		qtVersion,
		Platform::IsLinux());
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

void SetupMainMenuVersion(
		Ui::FlatLabel *label,
		Window::SessionController *controller) {
	if (!label || !controller) {
		return;
	}
	const auto upstream = Core::BuildIsCanary
		? currentVersionShortText()
		: tr::lng_settings_current_version(
			tr::now,
			lt_version,
			currentVersionShortText());
	label->setMarkedText(
		tr::link(upstream, 1)
		.append(u" \u00B7 "_q)
		.append(tr::link(ShortVersionText(), 3))
		.append(QChar(' '))
		.append(QChar(8211))
		.append(QChar(' '))
		.append(tr::link(tr::lng_menu_about(tr::now), 2)));
	label->setLink(
		1,
		std::make_shared<UrlClickHandler>(Core::App().changelogLink()));
	const auto weak = base::make_weak(controller);
	label->setLink(
		2,
		std::make_shared<LambdaClickHandler>([=] {
			if (const auto strong = weak.get()) {
				strong->show(Box(AboutBox));
			}
		}));
	label->setLink(
		3,
		std::make_shared<LambdaClickHandler>([=] {
			QGuiApplication::clipboard()->setText(VersionText());
			if (const auto strong = weak.get()) {
				strong->showToast(tr::lng_text_copied(tr::now));
			}
		}));
}

void AddAboutVersion(
		Ui::VerticalLayout *layout,
		Ui::GenericBox *box) {
	if (!layout || !box) {
		return;
	}
	const auto purpleVersion = layout->add(
		object_ptr<Ui::LinkButton>(
			box,
			VersionText(),
			st::aboutVersionLink),
		QMargins(
			st::boxRowPadding.left(),
			0,
			st::boxRowPadding.right(),
			0));
	purpleVersion->setClickedCallback([=] {
		QGuiApplication::clipboard()->setText(VersionText());
		box->uiShow()->showToast(tr::lng_text_copied(tr::now));
	});
}

} // namespace Purple
