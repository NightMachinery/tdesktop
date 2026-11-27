#pragma once

#include <QtCore/QVariant>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QWidget>
#include <algorithm>

namespace Purple::ListFilter {

#ifdef Q_OS_MAC
inline constexpr auto kControl = Qt::MetaModifier;
#else
inline constexpr auto kControl = Qt::ControlModifier;
#endif

[[nodiscard]] inline int HalfPageDirection(int key, Qt::KeyboardModifiers mods) {
	if ((mods & ~Qt::KeypadModifier) != Qt::AltModifier) {
		return 0;
	}
	return (key == Qt::Key_Down) ? 1 : (key == Qt::Key_Up) ? -1 : 0;
}

[[nodiscard]] inline int HalfPagePixels(int height) {
	return std::max(1, height / 2);
}

[[nodiscard]] inline int HalfPageRows(int height, int rowHeight) {
	return std::max(1, HalfPagePixels(height) / std::max(1, rowHeight));
}

[[nodiscard]] inline int ListDelta(
		int direction,
		Qt::KeyboardModifiers mods,
		int height,
		int rowHeight) {
	return direction * ((mods == Qt::AltModifier) ? HalfPageRows(height, rowHeight) : 1);
}

[[nodiscard]] inline bool Enter(const QKeyEvent &event, int key, bool allowA = true) {
	const auto mods = event.modifiers() & ~Qt::KeypadModifier;
	if (!mods && (key == Qt::Key_I || (allowA && key == Qt::Key_A) || key == Qt::Key_Slash)) {
		return true;
	}
	return event.text() == QLatin1Char('/')
		&& !mods.testFlag(Qt::MetaModifier)
#ifdef Q_OS_MAC
		&& !mods.testFlag(Qt::ControlModifier);
#else
		&& (!mods.testFlag(Qt::ControlModifier) || mods.testFlag(Qt::AltModifier));
#endif
}

[[nodiscard]] inline bool Exit(int key, Qt::KeyboardModifiers mods, bool modal = true) {
	return (key == Qt::Key_Escape && !mods)
		|| (mods == kControl && (key == Qt::Key_G || (modal && key == Qt::Key_BracketLeft)));
}

[[nodiscard]] inline bool Submit(int key) {
	return key == Qt::Key_Return || key == Qt::Key_Enter;
}

[[nodiscard]] inline bool ProtectPreedit(
		int key,
		Qt::KeyboardModifiers mods,
		bool modal) {
	return Submit(key) || Exit(key, mods & ~Qt::KeypadModifier, modal);
}

inline constexpr auto kFieldProperty = "purpleListFilterField";
inline constexpr auto kHalfPageProperty = "purpleHalfPageList";

enum class HalfPageMode { Disabled, Modal, Always };

inline void MarkHalfPage(QWidget *widget, HalfPageMode mode = HalfPageMode::Always) {
	widget->setProperty(kHalfPageProperty, int(mode));
}

[[nodiscard]] inline bool HandlesHalfPage(const QWidget *widget, bool modal) {
	const auto mode = HalfPageMode(widget->property(kHalfPageProperty).toInt());
	return mode == HalfPageMode::Always || (modal && mode == HalfPageMode::Modal);
}

inline void Mark(QWidget *field) {
	field->setProperty(kFieldProperty, true);
}

[[nodiscard]] inline bool IsMarked(const QWidget *field) {
	return field && field->property(kFieldProperty).toBool();
}

} // namespace Purple::ListFilter
