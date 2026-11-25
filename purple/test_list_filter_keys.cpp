#include "purple/purple_list_filter_keys.h"

#include <iostream>

int main() {
	using namespace Purple::ListFilter;
	const auto enters = [](int key, int physical, Qt::KeyboardModifiers mods, QString text = {}) {
		return Enter(QKeyEvent(QEvent::KeyPress, key, mods, text), physical);
	};
	const auto slash = QStringLiteral("/");
	const auto checks = {
		enters(Qt::Key_I, Qt::Key_I, Qt::NoModifier),
		enters(Qt::Key_A, Qt::Key_A, Qt::NoModifier),
		enters(Qt::Key_unknown, Qt::Key_Slash, Qt::NoModifier),
		enters(Qt::Key_7, Qt::Key_7, Qt::ShiftModifier, slash),
		enters(Qt::Key_7, Qt::Key_7, Qt::AltModifier, slash),
#ifdef Q_OS_MAC
		!enters(Qt::Key_7, Qt::Key_7, Qt::ControlModifier | Qt::AltModifier, slash),
#else
		enters(Qt::Key_7, Qt::Key_7, Qt::ControlModifier | Qt::AltModifier, slash),
#endif
		!enters(Qt::Key_Slash, Qt::Key_Slash, Qt::MetaModifier, slash),
		!enters(Qt::Key_Slash, Qt::Key_Slash, Qt::ControlModifier, slash),
		!enters(Qt::Key_I, Qt::Key_I, Qt::ShiftModifier),
		!enters(Qt::Key_7, Qt::Key_7, Qt::ShiftModifier, QStringLiteral("&")),
		!Enter(QKeyEvent(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier), Qt::Key_A, false),
		Exit(Qt::Key_Escape, Qt::NoModifier),
		Exit(Qt::Key_G, kControl),
		Exit(Qt::Key_BracketLeft, kControl),
		!Exit(Qt::Key_BracketLeft, kControl, false),
		!Exit(Qt::Key_G, Qt::NoModifier),
		!Exit(Qt::Key_BracketLeft, Qt::NoModifier),
		Submit(Qt::Key_Return),
		Submit(Qt::Key_Enter),
		!Submit(Qt::Key_Space),
		ProtectPreedit(Qt::Key_Escape, Qt::NoModifier, true),
		ProtectPreedit(Qt::Key_Escape, Qt::NoModifier, false),
		ProtectPreedit(Qt::Key_Enter, Qt::KeypadModifier, true),
		!ProtectPreedit(Qt::Key_E, Qt::NoModifier, true),
	};
	auto failures = 0;
	for (const auto check : checks) failures += !check;
	std::cout << checks.size() << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
