#include "test/test_purple_notices.h"

#include "base/weak_ptr.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "purple/purple_notices.h"
#include "window/window_controller.h"

#include <QtGui/QKeyEvent>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>

namespace Test {
namespace {

void SubmitProbe(Window::Controller &controller, Main::Session *session, bool dialog, bool delayed) {
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
			.coalesceKey = delayed ? u"test:delayed-dialog"_q : u"test:dialog"_q,
			.background = delayed,
			.rows = { { .label = u"Fixture"_q, .detail = u"Local notice probe"_q } },
			.fallbackToast = fallback,
		});
	} else {
		Purple::ShowPurpleAck({
			.session = session,
			.window = &controller,
			.scope = u"Notice test"_q,
			.text = text,
			.coalesceKey = delayed ? u"test:delayed-ack"_q : u"test:ack"_q,
			.background = delayed,
			.fallbackToast = fallback,
		});
	}
}

} // namespace

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
	const auto delayed = (key->modifiers() == (kModifiers | Qt::AltModifier));
	if ((!delayed && key->modifiers() != kModifiers)
		|| (key->key() != Qt::Key_F9 && key->key() != Qt::Key_F10)) {
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
	if (delayed) {
		const auto origin = base::make_weak(session);
		QTimer::singleShot(6000, window, [window, origin, dialog] {
			auto &live = window->controller();
			const auto session = origin.get();
			if (session && live.maybeSession() == session) {
				SubmitProbe(live, session, dialog, true);
			}
		});
	} else {
		SubmitProbe(controller, session, dialog, false);
	}
	return true;
}

} // namespace Test
