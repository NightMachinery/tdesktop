#pragma once

#include <QtCore/QString>
#include <QtCore/QByteArray>
#include <functional>
#include <memory>

namespace Purple {

struct ControlEndpointOptions {
	QString workingDirectory;
	QString configuredPath;
	std::function<QString()> defaultDirectory;
	QString fallbackPath;
};

[[nodiscard]] QString SelectControlSocketPath(const ControlEndpointOptions &options);
[[nodiscard]] bool ControlPeerAllowed(quint64 peer, quint64 own);
[[nodiscard]] bool ValidControlSocketPath(const QString &path);
[[nodiscard]] QString DecodeControlSocketPath(const QByteArray &bytes);

class ControlEndpoint final {
public:
	ControlEndpoint();
	~ControlEndpoint();
	ControlEndpoint(const ControlEndpoint &) = delete;
	ControlEndpoint &operator=(const ControlEndpoint &) = delete;
	[[nodiscard]] bool open(const QString &path);
	void close();
	[[nodiscard]] int descriptor() const;
	[[nodiscard]] int accept();
	[[nodiscard]] QString error() const;

private:
	struct State;
	std::unique_ptr<State> _state;

};

}
