#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

#include <functional>

namespace Purple {

class WaitKeyFilter final : public QObject {
public:
	void arm(QWidget *host, QWidget *cover, std::function<void()> abort);
	void disarm();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	QPointer<QWidget> _host;
	QPointer<QWidget> _cover;
	std::function<void()> _abort;
	QMetaObject::Connection _applicationState;

};

} // namespace Purple
