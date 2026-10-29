#include "purple/purple_wait_key_filter.h"

#include <QtCore/QEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>

#include <utility>

namespace Purple {

void CloseWaitCover(
		QPointer<QWidget> cover,
		QPointer<QWidget> previousFocus) {
	if (!cover) {
		return;
	}
	if (previousFocus
		&& previousFocus->isVisible()
		&& previousFocus->isEnabled()
		&& previousFocus->window() == cover->window()
		&& previousFocus != cover
		&& !cover->isAncestorOf(previousFocus)) {
		previousFocus->setFocus(Qt::OtherFocusReason);
	}
	if (!cover) {
		return;
	}
	const auto focused = QPointer<QWidget>(QApplication::focusWidget());
	if (focused && (focused == cover || cover->isAncestorOf(focused))) {
		focused->clearFocus();
	}
	if (cover) {
		cover->hide();
	}
	if (cover) {
		cover->deleteLater();
	}
}

void WaitKeyFilter::arm(
		QWidget *host,
		QWidget *cover,
		std::function<void()> abort) {
	arm(host, cover, WaitKeyHandlers{ .abort = std::move(abort) });
}

void WaitKeyFilter::arm(
		QWidget *host,
		QWidget *cover,
		WaitKeyHandlers handlers) {
	disarm();
	_host = host;
	_cover = cover;
	_handlers = std::move(handlers);
	qApp->installEventFilter(this);
	_applicationState = QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged,
		this, [=](Qt::ApplicationState state) {
			if (!_handlers.abort) {
				return;
			} else if (_handlers.presence) {
				presence();
			} else if (state != Qt::ApplicationActive) {
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
	_handlers = {};
}

void WaitKeyFilter::abort() {
	const auto callback = _handlers.abort;
	disarm();
	callback();
}

void WaitKeyFilter::presence() {
	// May cancel and disarm reentrantly; nothing is used after it.
	const auto callback = _handlers.presence;
	callback();
}

bool WaitKeyFilter::eventFilter(QObject *watched, QEvent *event) {
	if (!_handlers.abort) {
		return false;
	}
	const auto suspended = _handlers.suspended && _handlers.suspended();
	if (!_host || !_cover || (!suspended && _cover->isHidden())) {
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
	if (ownActivation && (type == QEvent::Close || type == QEvent::Hide)) {
		abort();
		return false;
	} else if (type == QEvent::WindowActivate && anotherWindow && !suspended) {
		abort();
		return false;
	} else if (type == QEvent::ApplicationDeactivate
		|| type == QEvent::ApplicationActivate
		|| (type == QEvent::WindowActivate && (ownActivation || anotherWindow))
		|| (ownActivation && type == QEvent::WindowDeactivate)) {
		if (!_handlers.presence) {
			if (type != QEvent::ApplicationActivate && type != QEvent::WindowActivate) {
				abort();
			}
		} else {
			presence();
		}
		return false;
	}
	if (type != QEvent::KeyPress && type != QEvent::ShortcutOverride) {
		return false;
	} else if (suspended) {
		return false;
	}
	const auto receiver = qobject_cast<QWidget*>(watched);
	const auto owningWindow = watched == hostWindow->windowHandle();
	if (!owningWindow && (!receiver || receiver->window() != hostWindow)) {
		abort();
		return false;
	}
	event->accept();
	if (type == QEvent::KeyPress) {
		const auto key = static_cast<QKeyEvent*>(event);
		const auto enter = (key->key() == Qt::Key_Return)
			|| (key->key() == Qt::Key_Enter);
		const auto verdict = _handlers.classify
			? _handlers.classify(enter, key->isAutoRepeat())
			: WaitKeyVerdict::Abort;
		if (verdict == WaitKeyVerdict::Abort) {
			abort();
		} else if (verdict == WaitKeyVerdict::Confirm && _handlers.confirm) {
			// May apply, close the cover and disarm reentrantly.
			const auto confirm = _handlers.confirm;
			confirm();
		}
	}
	return true;
}

} // namespace Purple
