#include "ui/effects/animations.h"
#include "ui/integration.h"
#include "ui/style/style_core.h"
#include "ui/style/style_core_direction.h"
#include "ui/style/style_core_palette.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtGui/QFontDatabase>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>

#include <iostream>

namespace crl {
rpl::producer<> on_main_update_requests() {
	return rpl::never<>();
}
} // namespace crl

namespace {

class Integration final : public Ui::Integration {
public:
	void postponeCall(FnMut<void()> &&callable) override {
		QTimer::singleShot(0, qApp, std::move(callable));
	}
	void registerLeaveSubscription(not_null<QWidget*>) override {}
	void unregisterLeaveSubscription(not_null<QWidget*>) override {}
	QString emojiCacheFolder() override { return {}; }
	QString fontsCacheFolder() override { return {}; }
	QString openglCheckFilePath() override { return {}; }
	QString angleBackendFilePath() override { return {}; }
	void touchCounterIncrement() override {}
	int touchCounterNow() override { return 0; }
};

class Canvas final : public QWidget {
protected:
	void paintEvent(QPaintEvent *) override {
		QPainter(this).fillRect(rect(), st::windowBg->c);
	}
};

QByteArray Read(const QString &path) {
	auto file = QFile(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void ApplyNightPalette(const QString &path) {
	const auto source = QString::fromUtf8(Read(path));
	auto palette = style::palette();
	const auto pattern = QRegularExpression(
		QStringLiteral(R"(^([a-zA-Z][a-zA-Z0-9_]*):\s*([#a-zA-Z0-9_]+);)"),
		QRegularExpression::MultilineOption);
	for (const auto &match : pattern.globalMatch(source)) {
		const auto key = match.captured(1).toLatin1();
		const auto value = match.captured(2);
		if (value.startsWith(u'#')) {
			const auto rgb = value.mid(1, 6).toUInt(nullptr, 16);
			const auto alpha = value.size() == 9 ? value.mid(7).toUInt(nullptr, 16) : 255;
			palette.setColor(QLatin1String(key), rgb >> 16, rgb >> 8, rgb, alpha);
		} else {
			palette.setColor(QLatin1String(key), QLatin1String(value.toLatin1()));
		}
	}
	palette.finalize();
	style::main_palette::apply(palette);
}

Ui::SettingsButton *Row(
		not_null<Ui::VerticalLayout*> layout,
		const QString &text) {
	Expects(!text.isEmpty());
	const auto button = layout->add(object_ptr<Ui::SettingsButton>(
		layout, rpl::single(text), st::settingsButton));
	button->setFocusPolicy(Qt::StrongFocus);
	return button;
}

bool Render(
		not_null<Canvas*> canvas,
		not_null<Ui::VerticalLayout*> layout,
		const QString &out,
		int width) {
	layout->resizeToWidth(style::ConvertScale(width));
	canvas->resize(layout->size());
	canvas->show();
	layout->show();
	qApp->processEvents();
	const auto image = canvas->grab().toImage();
	return !image.isNull() && image.save(out)
		&& image.pixelColor(1, image.height() - 2).rgba() == st::windowBg->c.rgba();
}

} // namespace

int main(int argc, char **argv) {
	if (argc != 6) {
		return 2;
	}
	auto app = QApplication(argc, argv);
	auto integration = Integration();
	Ui::Integration::Set(&integration);
	auto animations = Ui::Animations::Manager();
	const auto scale = QString::fromLatin1(argv[1]).toInt();
	const auto mode = QString::fromLatin1(argv[5]);
	const auto rtl = (mode == QStringLiteral("rtl"));
	const auto narrow = (mode != QStringLiteral("wide"));
	style::SetRightToLeft(rtl);
	QApplication::setLayoutDirection(style::LayoutDirection());
	for (const auto *file : { "OpenSans-Regular.ttf", "OpenSans-SemiBold.ttf" }) {
		QFontDatabase::addApplicationFont(
			QStringLiteral("Telegram/lib_ui/fonts/") + QString::fromLatin1(file));
	}
	style::SetScale(scale);
	style::StartManager(scale);
	style::main_palette::reset();
	const auto theme = QString::fromLocal8Bit(argv[2]);
	if (!theme.isEmpty()) {
		ApplyNightPalette(theme);
		Expects(st::boxDividerBg->c == st::windowBgOver->c);
	}
	const auto strings = QJsonDocument::fromJson(Read(QString::fromLocal8Bit(argv[3]))).object();
	const auto tr = [&](const char *key) { return strings.value(QString::fromLatin1(key)).toString(); };
	const auto out = QString::fromLocal8Bit(argv[4]);

	auto landingCanvas = Canvas();
	auto landing = Ui::VerticalLayout(&landingCanvas);
	Ui::AddSkip(&landing);
	Row(&landing, tr("lng_purple_settings_customize"));
	Ui::AddDivider(&landing);
	Ui::AddSkip(&landing);
	const auto archiveText = rtl
		? QStringLiteral("أرشيف ومساحة التخزين")
		: tr("lng_purple_settings_archive");
	const auto archive = Row(&landing, archiveText);
	const auto &buttonStyle = st::settingsButton;
	const auto usage = Ui::CreateChild<Ui::FlatLabel>(
		archive,
		st::wrap_rtl(narrow ? QStringLiteral("1.2 GiB+") : QStringLiteral("1.2 GiB")),
		buttonStyle.rightLabel);
	usage->show();
	archive->widthValue() | rpl::on_next([=](int width) {
		if (rtl) {
			auto padding = buttonStyle.padding;
			padding.setRight(std::max(
				padding.right(),
				width - padding.left() - archive->fullTextWidth()));
			archive->setPaddingOverride(padding);
		}
		usage->resizeToNaturalWidth(width - buttonStyle.padding.left()
			- buttonStyle.padding.right() - buttonStyle.style.font->width(archiveText)
			- st::settingsButtonRightSkip);
		usage->moveToRight(st::settingsButtonRightSkip, buttonStyle.padding.top());
	}, usage->lifetime());
	Row(&landing, tr("lng_purple_settings_work_activity"));
	Row(&landing, tr("lng_purple_settings_media"));
	Row(&landing, tr("lng_purple_settings_privacy_controls"));
	Row(&landing, tr("lng_purple_settings_accounts_sync"));
	Row(&landing, tr("lng_purple_settings_diagnostics"));
	Ui::AddDivider(&landing);
	Ui::AddSkip(&landing);
	Row(&landing, QStringLiteral("Purple 1.0 · build fixture"));
	Ui::AddSkip(&landing);

	auto mediaCanvas = Canvas();
	auto media = Ui::VerticalLayout(&mediaCanvas);
	Row(&media, tr("lng_purple_video_message_float_keep"));
	const auto help = Ui::AddDividerText(
		&media, rpl::single(tr("lng_purple_video_message_size_about")));
	Ui::AddSkip(&media);
	const auto recording = Ui::AddSubsectionTitle(
		&media, rpl::single(tr("lng_purple_media_recording")));
	Row(&media, tr("lng_purple_recording_keep_chats"));
	Ui::AddSkip(&media);
	const auto landingSaved = Render(
		&landingCanvas, &landing, out + QStringLiteral("-landing.png"), narrow ? 320 : 540);
	const auto mediaSaved = Render(&mediaCanvas, &media, out + QStringLiteral("-media.png"), 540);
	const auto saved = landingSaved && mediaSaved;
	const auto divider = help->parentWidget();
	const auto gap = recording->y() - divider->geometry().bottom() - 1;
	const auto inset = st::defaultVerticalListSkip + st::defaultSubsectionTitlePadding.top();
	const auto title = style::rtlrect(buttonStyle.padding.left(), 0,
		buttonStyle.style.font->width(archiveText), archive->height(), archive->width());
	const auto fits = archive->rect().contains(usage->geometry())
		&& !title.intersects(usage->geometry());
	const auto geometry = gap == inset && fits;
	std::cout << "Scale " << scale << ": Media heading inset " << gap
		<< " px (stock " << inset << "), archive label " << (fits ? "fits" : "overlaps")
		<< ", PNGs "
		<< (saved ? "saved" : "failed") << '\n';
	return (saved && geometry) ? 0 : 1;
}
