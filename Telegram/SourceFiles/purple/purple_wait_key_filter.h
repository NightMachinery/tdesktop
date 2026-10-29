#pragma once

#include "purple/purple_wait_gate.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

#include <functional>

namespace Purple {

void CloseWaitCover(
	QPointer<QWidget> cover,
	QPointer<QWidget> previousFocus);

struct WaitKeyHandlers {
	std::function<void()> abort;
	// The host window or the application gained or lost activation. Without
	// it, losing activation aborts at once.
	std::function<void()> presence;
	// The cover is hidden behind a lock: every event passes through, so a
	// passcode can be typed, and nothing aborts but closing the host.
	std::function<bool()> suspended;
	// What a key press means now. Without it, every key aborts.
	std::function<WaitKeyVerdict(bool enter, bool autoRepeat)> classify;
	std::function<void()> confirm;
};

class WaitKeyFilter final : public QObject {
public:
	void arm(QWidget *host, QWidget *cover, std::function<void()> abort);
	void arm(QWidget *host, QWidget *cover, WaitKeyHandlers handlers);
	void disarm();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void abort();
	void presence();

	QPointer<QWidget> _host;
	QPointer<QWidget> _cover;
	WaitKeyHandlers _handlers;
	QMetaObject::Connection _applicationState;

};

} // namespace Purple
