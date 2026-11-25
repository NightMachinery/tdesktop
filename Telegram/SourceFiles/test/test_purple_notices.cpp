#include "test/test_purple_notices.h"

#include "base/weak_ptr.h"
#include "mainwindow.h"
#include "purple/purple_notices.h"
#include "window/window_controller.h"

#include <QtGui/QKeyEvent>
#include <QtWidgets/QApplication>

namespace Test {

bool TryPurpleNoticeProbe(MainWindow *window, QObject *target, QEvent *event) {
	const auto type = event->type();
	if (type != QEvent::ShortcutOverride && type != QEvent::KeyPress) {
		return false;
	}
	static const auto enabled = (qEnvironmentVariableIntValue("PURPLE_TEST_NOTICE_PROBES") == 1);
	if (!enabled || !target->isWidgetType()
		|| static_cast<QWidget*>(target)->window() != window
		|| QApplication::activeWindow() != window) {
		return false;
	}
#ifdef Q_OS_MAC
	constexpr auto kModifiers = Qt::MetaModifier | Qt::ShiftModifier;
#else
	constexpr auto kModifiers = Qt::ControlModifier | Qt::ShiftModifier;
#endif
	const auto key = static_cast<QKeyEvent*>(event);
	if (key->modifiers() != kModifiers || (key->key() != Qt::Key_F9 && key->key() != Qt::Key_F10)) {
		return false;
	}
	event->accept();
	if (type == QEvent::ShortcutOverride || key->isAutoRepeat()) {
		return true;
	}
	auto &controller = window->controller();
	const auto session = controller.maybeSession();
	if (!session) {
		return true;
	}
	const auto dialog = (key->key() == Qt::Key_F9);
	const auto text = dialog ? u"Test dialog notice"_q : u"Test acknowledgement"_q;
	const auto weak = base::make_weak(&controller);
	const auto fallback = [weak, text] {
		if (const auto live = weak.get()) {
			live->showToast(u"%1 (stock toast)"_q.arg(text));
		}
	};
	if (dialog) {
		Purple::ShowPurpleNotice({
			.session = session,
			.window = &controller,
			.scope = u"Notice test"_q,
			.summary = text,
			.coalesceKey = u"test:dialog"_q,
			.rows = { { .label = u"Fixture"_q, .detail = u"Local notice probe"_q } },
			.fallbackToast = fallback,
		});
	} else {
		Purple::ShowPurpleAck({
			.session = session,
			.window = &controller,
			.scope = u"Notice test"_q,
			.text = text,
			.coalesceKey = u"test:ack"_q,
			.fallbackToast = fallback,
		});
	}
	return true;
}

} // namespace Test
