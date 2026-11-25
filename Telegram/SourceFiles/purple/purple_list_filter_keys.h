#pragma once

#include <QtCore/QVariant>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QWidget>

namespace Purple::ListFilter {

#ifdef Q_OS_MAC
inline constexpr auto kControl = Qt::MetaModifier;
#else
inline constexpr auto kControl = Qt::ControlModifier;
#endif

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

inline constexpr auto kFieldProperty = "purpleListFilterField";

inline void Mark(QWidget *field) {
	field->setProperty(kFieldProperty, true);
}

[[nodiscard]] inline bool IsMarked(const QWidget *field) {
	return field && field->property(kFieldProperty).toBool();
}

} // namespace Purple::ListFilter
