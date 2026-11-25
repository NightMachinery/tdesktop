#include "base/integration.h"
#include "lang/lang_keys.h"
#include "purple/purple_archive_v2_ui.h"
#include "ui/effects/animations.h"
#include "ui/integration.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/layer_widget.h"
#include "ui/style/style_core.h"
#include "ui/style/style_core_direction.h"
#include "ui/style/style_core_palette.h"
#include "styles/style_layers.h"
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtGui/QFontDatabase>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>
#include <cstdio>

namespace crl {
rpl::producer<> on_main_update_requests() { return rpl::never<>(); }
}
namespace Lang::details {
QString Current(ushort key) { return GetOriginalValue(key); }
rpl::producer<QString> Value(ushort key) { return rpl::single(Current(key)); }
}
namespace Purple {
bool ModalKeysEnabled() { return true; }
QString CopyTextAsMarkdown(const TextForMimeData &text) { return text.rich.text; }
}

namespace {
using namespace Purple::ArchiveV2;

class BaseIntegration final : public base::Integration {
public:
	using base::Integration::Integration;
	void enterFromEventLoop(FnMut<void()> &&method) override { method(); }
	bool logSkipDebug() override { return true; }
	void logMessageDebug(const QString &) override {}
	void logMessage(const QString &) override {}
};

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

bool Render(const QString &out, int width, const char *name,
		Fn<void(not_null<Ui::GenericBox*>)> fill) {
	auto window = Canvas();
	window.resize(width, 720);
	auto layers = Ui::LayerStackWidget(&window, {});
	layers.setGeometry(window.rect());
	window.show();
	auto content = Box<Ui::GenericBox>(std::move(fill));
	const auto box = QPointer<Ui::GenericBox>(content.data());
	layers.showBox(std::move(content), Ui::LayerOption::CloseOther, anim::type::instant);
	qApp->processEvents();
	const auto image = window.grab().toImage();
	if (!box || !image.save(out + '/' + QString::fromLatin1(name) + u".png"_q)) return false;
	const auto geometry = box->mapTo(&window, QPoint(0, 0));
	if (geometry.x() < 0 || geometry.x() + box->width() > width) {
		std::fprintf(stderr, "archive box %s is clipped at width %d\n", name, width);
		return false;
	}
	const auto layout = box->verticalLayout();
	for (auto child : layout->children()) {
		const auto row = dynamic_cast<QWidget*>(child);
		if (!row || !row->isVisible()) continue;
		const auto left = row->mapTo(box, QPoint()).x();
		if (left < 0 || left + row->width() > static_cast<QWidget*>(box.data())->width()) {
			std::fprintf(stderr, "archive box %s has a clipped row at width %d (x=%d, row=%d, box=%d, layout=%d)\n",
				name, width, left, row->width(), static_cast<QWidget*>(box.data())->width(), layout->width());
			return false;
		}
	}
	return true;
}
}

int main(int argc, char **argv) {
	if (argc != 5) return 2;
	auto app = QApplication(argc, argv);
	auto base = BaseIntegration(argc, argv);
	base::Integration::Set(&base);
	auto integration = Integration();
	Ui::Integration::Set(&integration);
	auto animations = Ui::Animations::Manager();
	const auto width = QByteArray(argv[3]).toInt();
	style::SetRightToLeft(QByteArray(argv[4]) == "rtl");
	QFontDatabase::addApplicationFont(u"Telegram/lib_ui/fonts/OpenSans-Regular.ttf"_q);
	QFontDatabase::addApplicationFont(u"Telegram/lib_ui/fonts/OpenSans-SemiBold.ttf"_q);
	style::SetScale(100);
	style::StartManager(100);
	style::main_palette::reset();
	if (*argv[2]) ApplyNightPalette(QString::fromLocal8Bit(argv[2]));
	const auto directory = QString::fromLocal8Bit(argv[1]);
	const auto current = [] { return true; };
	const auto decode = [](const QByteArray &bytes) {
		return TextForMimeData::Simple(QString::fromUtf8(bytes));
	};
	const auto ok = Render(directory, width, "history", [&](not_null<Ui::GenericBox*> box) {
		FillHistoryBox(box, { MessageStatus::Deleted, true, {
			{ 2, { 1760000200, "A long fixture caption breaks across multiple lines near the narrow edge. العربية." },
				{ { 0, "fixture", 32768, {} }, { 1, "fixture", 0, {} }, { 2, "fixture", 0, u"expired"_q } } },
			{ 1, { 1760000000, "Earlier fixture caption" }, {} },
		}, 1 }, decode, current, [](int64) {}, {
			.saving = [](int64 seq, int64 ordinal) { return seq == 2 && ordinal == 1; },
			.openSaved = [](int64, int64) {},
		});
	}) && Render(directory, width, "deleted", [&](not_null<Ui::GenericBox*> box) {
		FillTimelineBox(box, {}, true, decode, current, {});
	}) && Render(directory, width, "storage", [&](not_null<Ui::GenericBox*> box) {
		FillStorageBox(box, StorageSummary{ { 90 * kMiB, 8 * kMiB, 0, 0, 65 * kMiB, 24 * kMiB },
			2 * kMiB, 512 * kMiB, 256 * kMiB, 16 * kMiB, 64 * kMiB }, 24 * kMiB,
			current, [](CleanupKind) {}, [] {});
	}) && Render(directory, width, "previous-confirm", [&](not_null<Ui::GenericBox*> box) {
		FillPreviousArchiveConfirm(box, u"Fixture account"_q, 24 * kMiB, current, [] {});
	});
	style::StopManager();
	return ok ? 0 : 1;
}
