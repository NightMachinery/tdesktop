#pragma once

#include "purple/purple_control_commands.h"
#include "purple/purple_control_endpoint.h"
#include <QtCore/QObject>
#include <memory>

namespace Purple {

using ControlHandler = std::function<void(
	const ControlRequest &, const ControlPolicy &, ControlReply)>;
using ControlStatus = std::function<void(QString, QString)>;

class ControlServer final : public QObject {
public:
	ControlServer(ControlHandler handler, ControlStatus status, QObject *parent = nullptr);
	~ControlServer();
	void configure(ControlEndpointOptions options);
	void setPolicy(ControlPolicy policy);
	void stop();

private:
	struct State;
	std::unique_ptr<State> _state;

};

}
