#pragma once

class MainWindow;
class QEvent;
class QObject;

namespace Test {

#ifdef PURPLE_BUILD_NOTICE_TEST_PROBES
[[nodiscard]] bool TryPurpleNoticeProbe(MainWindow *window, QObject *target, QEvent *event);
#else
[[nodiscard]] inline bool TryPurpleNoticeProbe(MainWindow *, QObject *, QEvent *) {
	return false;
}
#endif

} // namespace Test
