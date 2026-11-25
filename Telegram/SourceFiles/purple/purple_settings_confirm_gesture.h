#pragma once

#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtGui/QStyleHints>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

namespace Purple {

class SettingsConfirmGesture final : public QObject {
public:
	static void Guard(QWidget *window, const QMouseEvent &click) {
		new SettingsConfirmGesture(window, click.globalPosition().toPoint());
	}

private:
	SettingsConfirmGesture(QWidget *window, QPoint position)
	: QObject(window)
	, _window(window)
	, _position(position)
	, _distance(QApplication::styleHints()->mouseDoubleClickDistance()) {
		qApp->installEventFilter(this);
		QTimer::singleShot(QApplication::doubleClickInterval(), this, &QObject::deleteLater);
	}

	bool eventFilter(QObject *object, QEvent *event) override {
		const auto type = event->type();
		if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonDblClick
			&& type != QEvent::MouseButtonRelease) {
			return false;
		}
		const auto target = qobject_cast<QWidget*>(object);
		const auto mouse = static_cast<QMouseEvent*>(event);
		if (!target || target->window() != _window || mouse->button() != Qt::LeftButton) {
			return false;
		}
		if (type == QEvent::MouseButtonRelease) {
			if (!_continuing) return false;
			qApp->removeEventFilter(this);
			deleteLater();
		} else if ((mouse->globalPosition().toPoint() - _position).manhattanLength() <= _distance) {
			_continuing = true;
		} else {
			return false;
		}
		event->accept();
		return true;
	}

	QWidget *_window;
	QPoint _position;
	int _distance;
	bool _continuing = false;
};

} // namespace Purple
