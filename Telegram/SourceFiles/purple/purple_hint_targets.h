#pragma once

#include "base/basic_types.h"

#include <QtCore/QPointer>
#include <QtCore/QRect>
#include <QtCore/QString>

#include <vector>

class QWidget;

namespace Purple {

struct WindowHintTarget {
	QPointer<QWidget> widget;
	QRect bounds;
	QRect bubble;
	QString name;
	bool mainList = false;
	bool reply = false;
	Fn<bool()> current;
	Fn<void()> activate;
};

class WindowHintSource {
public:
	virtual ~WindowHintSource() = default;
	virtual void collectWindowHints(
		const QRect &clip,
		std::vector<WindowHintTarget> &out) = 0;
};

[[nodiscard]] QRect WindowHintClip(QWidget *widget);
[[nodiscard]] bool WindowHintExposed(QWidget *widget, QPoint globalPoint);
[[nodiscard]] std::vector<WindowHintTarget> CollectWindowHintTargets(QWidget *root);
void ClickWindowHint(QWidget *widget, QPoint localPoint);

} // namespace Purple
