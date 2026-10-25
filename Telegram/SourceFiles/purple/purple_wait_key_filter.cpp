#include "purple/purple_wait_key_filter.h"

#include <QtCore/QEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>

#include <utility>

namespace Purple {

void WaitKeyFilter::arm(
		QWidget *host,
		QWidget *cover,
		std::function<void()> abort) {
	disarm();
	_host = host;
	_cover = cover;
	_abort = std::move(abort);
	qApp->installEventFilter(this);
	_applicationState = QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged,
		this, [=](Qt::ApplicationState state) {
			if (state != Qt::ApplicationActive && _abort) {
				const auto abort = _abort;
				disarm();
				abort();
			}
		});
}

void WaitKeyFilter::disarm() {
	QObject::disconnect(_applicationState);
	_applicationState = {};
	qApp->removeEventFilter(this);
	_host = nullptr;
	_cover = nullptr;
	_abort = nullptr;
}

bool WaitKeyFilter::eventFilter(QObject *watched, QEvent *event) {
	if (!_abort) {
		return false;
	}
	const auto abort = _abort;
	if (!_host || !_cover || _cover->isHidden()) {
		disarm();
		abort();
		return false;
	}
	const auto type = event->type();
	const auto hostWindow = _host->window();
	const auto ownActivation = watched == _host || watched == hostWindow
		|| watched == hostWindow->windowHandle();
	const auto widget = qobject_cast<QWidget*>(watched);
	const auto window = qobject_cast<QWindow*>(watched);
	const auto anotherWindow = (widget && widget->isWindow() && widget != hostWindow)
		|| (window && window != hostWindow->windowHandle());
	if (type == QEvent::ApplicationDeactivate
		|| (ownActivation && (type == QEvent::WindowDeactivate
			|| type == QEvent::Close || type == QEvent::Hide))
		|| (type == QEvent::WindowActivate && anotherWindow)) {
		disarm();
		abort();
		return false;
	}
	if (type != QEvent::KeyPress && type != QEvent::ShortcutOverride) {
		return false;
	}
	const auto receiver = qobject_cast<QWidget*>(watched);
	const auto owningWindow = watched == hostWindow->windowHandle();
	if (!owningWindow && (!receiver || receiver->window() != hostWindow)) {
		disarm();
		abort();
		return false;
	}
	event->accept();
	if (type == QEvent::KeyPress) {
		disarm();
		abort();
	}
	return true;
}

} // namespace Purple
