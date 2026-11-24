#include "purple/purple_chat_background.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "lang/lang_keys.h"
#include "purple/purple_notices.h"
#include "ui/chat/chat_style.h"
#include "ui/chat/chat_theme.h"
#include "ui/color_contrast.h"
#include "ui/rp_widget.h"
#include "ui/style/style_core_palette.h"
#include "ui/toast/toast.h"
#include "styles/palette.h"

#include <QtGui/QPainter>

namespace Purple {
namespace {

constexpr auto kVisiblePref = "purple-chat-backgrounds-visible";
constexpr auto kServiceTextContrast = 4.5;
rpl::event_stream<> VisibilityChanges;

bool ChatBackgroundsVisible() {
	return Core::App().settings().readPref<bool>(kVisiblePref, true);
}

QColor Mix(const QColor &from, const QColor &to, float64 fraction) {
	const auto channel = [&](int a, int b) {
		return qRound(a + (b - a) * fraction);
	};
	return QColor(
		channel(from.red(), to.red()),
		channel(from.green(), to.green()),
		channel(from.blue(), to.blue()));
}

QColor Composite(const QColor &over, const QColor &under) {
	return Mix(under, over, over.alphaF());
}

QColor ReadableServiceBackground(
		const QColor &chip,
		const QColor &canvas,
		const QColor &text) {
	const auto visible = Composite(chip, canvas);
	if (Ui::CountContrast(text, visible) >= kServiceTextContrast) {
		return chip;
	}
	const auto black = QColor(Qt::black);
	const auto white = QColor(Qt::white);
	const auto toward = (Ui::CountContrast(text, black)
		> Ui::CountContrast(text, white)) ? black : white;
	auto low = 0.;
	auto high = 1.;
	for (auto i = 0; i != 10; ++i) {
		const auto mid = (low + high) / 2.;
		if (Ui::CountContrast(text, Mix(visible, toward, mid))
			>= kServiceTextContrast) {
			high = mid;
		} else {
			low = mid;
		}
	}
	return Mix(visible, toward, high);
}

} // namespace

void WatchChatBackgroundVisibility(not_null<Ui::RpWidget*> widget) {
	VisibilityChanges.events() | rpl::on_next([=] {
		widget->update();
	}, widget->lifetime());
}

void WatchLiveChatBackgroundStyle(
		not_null<Ui::ChatStyle*> style,
		Fn<std::shared_ptr<Ui::ChatTheme>()> currentTheme,
		rpl::lifetime &lifetime) {
	rpl::merge(
		rpl::single(),
		VisibilityChanges.events(),
		style::PaletteChanged() | rpl::filter([] {
			return !ChatBackgroundsVisible();
		})
	) | rpl::on_next([=] {
		if (const auto theme = currentTheme()) {
			ApplyLiveChatBackgroundStyle(style, theme.get());
		}
	}, lifetime);
}

void ApplyLiveChatBackgroundStyle(
		not_null<Ui::ChatStyle*> style,
		not_null<Ui::ChatTheme*> theme) {
	if (ChatBackgroundsVisible()) {
		style->apply(theme);
		return;
	}
	auto palette = style::palette();
	const auto base = theme->palette();
	palette = *(base ? base : style::main_palette::get().get());
	const auto canvas = palette.windowBg()->c;
	auto text = palette.msgServiceFg()->c;
	const auto normal = palette.msgServiceBg()->c;
	const auto selected = palette.msgServiceBgSelected()->c;
	const auto lowContrast = [&](const QColor &chip) {
		const auto visible = Composite(chip, canvas);
		return Ui::CountContrast(Composite(text, visible), visible)
			< kServiceTextContrast;
	};
	if (lowContrast(normal) || lowContrast(selected)) {
		text.setAlpha(255);
		palette.setColor(QLatin1String("msgServiceFg"), text);
		palette.setColor(QLatin1String("msgServiceBg"),
			ReadableServiceBackground(normal, canvas, text));
		palette.setColor(QLatin1String("msgServiceBgSelected"),
			ReadableServiceBackground(selected, canvas, text));
	}
	style->applyCustomPalette(&palette);
}

bool PaintHiddenChatBackground(
		not_null<QWidget*> widget,
		not_null<Ui::ChatTheme*> theme,
		const QRect &clip) {
	if (ChatBackgroundsVisible()) {
		return false;
	}
	auto painter = QPainter(widget);
	const auto palette = theme->palette();
	auto color = (palette ? palette->windowBg() : st::windowBg)->c;
	color.setAlpha(255);
	painter.fillRect(clip, color);
	return true;
}

void ToggleChatBackgroundVisibility(not_null<QWidget*> window) {
	const auto visible = !ChatBackgroundsVisible();
	Core::App().settings().writePref<bool>(kVisiblePref, visible);
	VisibilityChanges.fire({});
	const auto msg = visible
		? tr::lng_purple_keys_backgrounds_shown(tr::now)
		: tr::lng_purple_keys_backgrounds_hidden(tr::now);
	const auto weakWin = QPointer<QWidget>(window.get());
	ShowPurpleAck({
		.scope = u"Chat backgrounds"_q,
		.text = msg,
		.severity = ModalNotices::NoticeSeverity::Info,
		.fallbackToast = [=] { if (weakWin) { Ui::Toast::Show(weakWin.data(), msg); } },
	});
}

} // namespace Purple
