#include "purple/purple_control_endpoint.h"

#include "base/basic_types.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#ifndef Q_OS_WIN
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Purple {
namespace {

#ifndef Q_OS_WIN
class SocketDescriptor final {
public:
	explicit SocketDescriptor(int value);
	~SocketDescriptor();
	[[nodiscard]] int get() const;
	[[nodiscard]] int release();

private:
	int _value = -1;

};

SocketDescriptor::SocketDescriptor(int value) : _value(value) {
}

SocketDescriptor::~SocketDescriptor() {
	if (_value >= 0) ::close(_value);
}

int SocketDescriptor::get() const {
	return _value;
}

int SocketDescriptor::release() {
	return std::exchange(_value, -1);
}

bool Nonblocking(int fd) {
	const auto flags = fcntl(fd, F_GETFL);
	return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0
		&& fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

bool Same(const struct stat &a, const struct stat &b) {
	return a.st_dev == b.st_dev && a.st_ino == b.st_ino
		&& a.st_uid == b.st_uid && (a.st_mode & S_IFMT) == (b.st_mode & S_IFMT);
}

bool PrivateParent(const QString &path, struct stat &result) {
	const auto bytes = QFile::encodeName(path);
	return lstat(bytes.constData(), &result) == 0 && S_ISDIR(result.st_mode)
		&& result.st_uid == geteuid() && (result.st_mode & 0777) == 0700;
}

bool PrepareParent(const QString &path, struct stat &result) {
	const auto parts = path.split('/', Qt::SkipEmptyParts);
	auto current = QString();
	for (const auto &part : parts) {
		current += '/' + part;
		const auto bytes = QFile::encodeName(current);
		struct stat info = {};
		if (lstat(bytes.constData(), &info) != 0) {
			if (errno != ENOENT || mkdir(bytes.constData(), 0700) != 0
				|| lstat(bytes.constData(), &info) != 0) return false;
		}
		if (!S_ISDIR(info.st_mode)) return false;
	}
	return PrivateParent(path, result);
}

bool PeerMatches(int fd) {
#if defined Q_OS_MAC || defined Q_OS_FREEBSD || defined Q_OS_OPENBSD || defined Q_OS_NETBSD
	auto uid = uid_t(0);
	auto gid = gid_t(0);
	return getpeereid(fd, &uid, &gid) == 0 && ControlPeerAllowed(uid, geteuid());
#elif defined SO_PEERCRED
	auto credentials = ucred();
	auto size = socklen_t(sizeof(credentials));
	return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0
		&& size == sizeof(credentials) && ControlPeerAllowed(credentials.uid, geteuid());
#else
	return false;
#endif
}
#endif

}

struct ControlEndpoint::State {
	int fd = -1;
	QString path;
	QString error;
#ifndef Q_OS_WIN
	struct stat socket = {};
	struct stat parent = {};
	bool owned = false;
#endif
};

QString SelectControlSocketPath(const ControlEndpointOptions &options) {
	if (!options.configuredPath.isEmpty()) return options.configuredPath;
	const auto defaultDirectory = options.defaultDirectory
		? options.defaultDirectory() : QString();
	if (defaultDirectory.isEmpty()
		|| QDir::cleanPath(options.workingDirectory) != QDir::cleanPath(defaultDirectory)) return {};
	const auto path = QDir::cleanPath(options.workingDirectory) + u"/purple-control.sock"_q;
	return path.toUtf8().size() < 104 ? path : options.fallbackPath;
}

bool ValidControlSocketPath(const QString &path) {
	return path.isEmpty() || (QDir::isAbsolutePath(path) && QDir::cleanPath(path) == path
		&& !path.contains(QChar(0)) && path.toUtf8().size() < 104);
}

QString DecodeControlSocketPath(const QByteArray &bytes) {
	const auto path = QString::fromUtf8(bytes);
	return path.toUtf8() == bytes ? path : QString(QChar(0));
}

bool ControlPeerAllowed(quint64 peer, quint64 own) {
	return peer == own;
}

ControlEndpoint::ControlEndpoint() : _state(std::make_unique<State>()) {
}

ControlEndpoint::~ControlEndpoint() {
	close();
}

bool ControlEndpoint::open(const QString &path) {
	close();
	auto &state = *_state;
	state.error = u"A control socket needs an absolute path in an owned 0700 directory."_q;
#ifdef Q_OS_WIN
	state.error = u"The control socket is unavailable on this platform."_q;
	return false;
#else
	const auto bytes = QFile::encodeName(path);
	if (!QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path
		|| path.contains(QChar(0)) || bytes.isEmpty() || bytes.size() >= 104
		|| bytes.size() >= sizeof(sockaddr_un::sun_path)) return false;
	const auto parentPath = QFileInfo(path).absolutePath();
	if (!PrepareParent(parentPath, state.parent)) return false;
	auto address = sockaddr_un();
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, bytes.constData(), bytes.size() + 1);
	struct stat original = {};
	if (lstat(bytes.constData(), &original) == 0) {
		state.error = u"The control socket path is already occupied."_q;
		if (!S_ISSOCK(original.st_mode) || original.st_uid != geteuid()) return false;
		auto probe = SocketDescriptor(socket(AF_UNIX, SOCK_STREAM, 0));
		if (probe.get() < 0 || !Nonblocking(probe.get())) return false;
		if (::connect(probe.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) return false;
		if (errno != ECONNREFUSED && errno != ENOENT) return false;
		struct stat fresh = {};
		struct stat parent = {};
		if (lstat(bytes.constData(), &fresh) != 0 || !Same(original, fresh)
			|| !PrivateParent(parentPath, parent) || !Same(state.parent, parent)
			|| unlink(bytes.constData()) != 0) return false;
	} else if (errno != ENOENT) {
		return false;
	}
	struct stat parent = {};
	if (!PrivateParent(parentPath, parent) || !Same(state.parent, parent)) return false;
	auto listener = SocketDescriptor(socket(AF_UNIX, SOCK_STREAM, 0));
	state.error = u"Could not bind the control socket."_q;
	if (listener.get() < 0 || !Nonblocking(listener.get())
		|| bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) return false;
	state.path = path;
	state.fd = listener.release();
	if (lstat(bytes.constData(), &state.socket) != 0 || !S_ISSOCK(state.socket.st_mode)
		|| state.socket.st_uid != geteuid()) { close(); return false; }
	state.owned = true;
	if (!PrivateParent(parentPath, parent) || !Same(state.parent, parent)
		|| chmod(bytes.constData(), 0600) != 0 || listen(state.fd, 16) != 0) {
		close();
		return false;
	}
	state.error.clear();
	return true;
#endif
}

void ControlEndpoint::close() {
	auto &state = *_state;
#ifndef Q_OS_WIN
	if (state.fd >= 0) ::close(state.fd);
	state.fd = -1;
	if (state.owned) {
		struct stat socket = {};
		struct stat parent = {};
		const auto bytes = QFile::encodeName(state.path);
		if (lstat(bytes.constData(), &socket) == 0 && Same(state.socket, socket)
			&& PrivateParent(QFileInfo(state.path).absolutePath(), parent)
			&& Same(state.parent, parent)) unlink(bytes.constData());
	}
	state.owned = false;
#endif
	state.path.clear();
}

int ControlEndpoint::descriptor() const {
	return _state->fd;
}

int ControlEndpoint::accept() {
#ifdef Q_OS_WIN
	return -1;
#else
	auto peer = SocketDescriptor(::accept(_state->fd, nullptr, nullptr));
	if (peer.get() < 0 || !Nonblocking(peer.get()) || !PeerMatches(peer.get())) return -1;
	return peer.release();
#endif
}

QString ControlEndpoint::error() const {
	return _state->error;
}

}
