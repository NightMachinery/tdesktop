#pragma once

#include <QtCore/QEvent>
#include <QtWidgets/QWidget>

namespace Purple {

[[nodiscard]] inline bool WindowHintGeometryEvent(QEvent::Type type) {
	// Showing the hint overlay posts LayoutRequest to its own parent.
	return type == QEvent::Show || type == QEvent::Hide
		|| type == QEvent::Move || type == QEvent::Resize;
}

[[nodiscard]] inline QRect WindowHintClip(QWidget *widget) {
	if (!widget || !widget->isVisible() || !widget->isEnabled()) {
		return {};
	}
	auto clip = QRect(widget->mapToGlobal(QPoint()), widget->size());
	for (auto parent = widget->parentWidget(); parent; parent = parent->parentWidget()) {
		clip &= QRect(parent->mapToGlobal(QPoint()), parent->size());
		if (parent->isWindow() || clip.isEmpty()) {
			break;
		}
	}
	return clip;
}

[[nodiscard]] inline bool WindowHintExposed(QWidget *widget, QPoint globalPoint) {
	if (!WindowHintClip(widget).contains(globalPoint)) {
		return false;
	}
	const auto window = widget->window();
	const auto hit = window->childAt(window->mapFromGlobal(globalPoint));
	return hit && (hit == widget || widget->isAncestorOf(hit));
}

} // namespace Purple
