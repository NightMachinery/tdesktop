#include "purple/purple_chat_background.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "lang/lang_keys.h"
#include "ui/chat/chat_theme.h"
#include "ui/rp_widget.h"
#include "ui/style/style_core_palette.h"
#include "ui/toast/toast.h"
#include "styles/palette.h"

#include <QtGui/QPainter>

namespace Purple {
namespace {

constexpr auto kVisiblePref = "purple-chat-backgrounds-visible";
rpl::event_stream<> VisibilityChanges;

bool ChatBackgroundsVisible() {
	return Core::App().settings().readPref<bool>(kVisiblePref, true);
}

} // namespace

void WatchChatBackgroundVisibility(not_null<Ui::RpWidget*> widget) {
	VisibilityChanges.events() | rpl::on_next([=] {
		widget->update();
	}, widget->lifetime());
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
	Ui::Toast::Show(window, visible
		? tr::lng_purple_keys_backgrounds_shown(tr::now)
		: tr::lng_purple_keys_backgrounds_hidden(tr::now));
}

} // namespace Purple
