#include "purple/purple_control_server.h"

#include "base/basic_types.h"
#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtCore/QSocketNotifier>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>
#include <atomic>
#include <map>
#include <set>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

namespace Purple {
namespace {

struct ControlAuthority {
	std::shared_ptr<const ControlPolicy> policy
		= std::make_shared<const ControlPolicy>();
	std::atomic<int> queued = 0;
};

class ControlDispatchLease final {
public:
	explicit ControlDispatchLease(std::shared_ptr<ControlAuthority> authority);
	~ControlDispatchLease();
	[[nodiscard]] bool acquired() const;

private:
	std::shared_ptr<ControlAuthority> _authority;

};

class PendingConnections final : public QLocalServer {
public:
	explicit PendingConnections(QObject *parent);
	void enqueue(QLocalSocket *socket);
	bool hasPendingConnections() const override;
	QLocalSocket *nextPendingConnection() override;

private:
	QLocalSocket *_pending = nullptr;

};

class ControlWorker final : public QObject {
public:
	using Ticket = std::shared_ptr<std::atomic<bool>>;
	using Dispatch = std::function<void(
		quint64, quint64, quint64, quint64, Ticket,
		std::shared_ptr<ControlDispatchLease>, ControlRequest)>;
	using Status = std::function<void(quint64, QString, QString)>;
	ControlWorker(std::shared_ptr<ControlAuthority> authority, Dispatch dispatch, Status status);
	void configure(quint64 epoch, ControlEndpointOptions options);
	void stop();
	void reply(quint64 epoch, quint64 client, quint64 serial, ControlResult result);

private:
	struct Pending {
		quint64 serial = 0;
		quint64 generation = 0;
		QString command;
		qint64 started = 0;
		Ticket active;
	};
	struct Client {
		QLocalSocket *socket = nullptr;
		QByteArray input;
		ControlRateLimit rate;
		std::map<qint64, Pending> pending;
		qint64 activity = 0;
	};
	void acceptReady();
	void addConnection();
	void read(quint64 client);
	void request(quint64 client, const QByteArray &line);
	void send(quint64 client, QJsonValue id, ControlResult result);
	void drop(quint64 client);
	void expire();

	std::shared_ptr<ControlAuthority> _authority;
	Dispatch _dispatch;
	Status _status;
	ControlEndpoint _endpoint;
	PendingConnections _connections{ this };
	std::unique_ptr<QSocketNotifier> _notifier;
	QTimer _expiry{ this };
	QElapsedTimer _clock;
	std::map<quint64, Client> _clients;
	quint64 _epoch = 0;
	quint64 _nextClient = 0;
	quint64 _nextSerial = 0;
	int _pending = 0;

};

ControlDispatchLease::ControlDispatchLease(std::shared_ptr<ControlAuthority> authority)
: _authority(std::move(authority)) {
	if (_authority->queued.fetch_add(1) >= kControlPending) {
		_authority->queued.fetch_sub(1);
		_authority.reset();
	}
}

ControlDispatchLease::~ControlDispatchLease() {
	if (_authority) _authority->queued.fetch_sub(1);
}

bool ControlDispatchLease::acquired() const {
	return _authority != nullptr;
}

PendingConnections::PendingConnections(QObject *parent) : QLocalServer(parent) {
}

void PendingConnections::enqueue(QLocalSocket *socket) {
	if (_pending) { socket->abort(); socket->deleteLater(); return; }
	_pending = socket;
	newConnection();
}

bool PendingConnections::hasPendingConnections() const {
	return _pending != nullptr;
}

QLocalSocket *PendingConnections::nextPendingConnection() {
	return std::exchange(_pending, nullptr);
}

ControlWorker::ControlWorker(
		std::shared_ptr<ControlAuthority> authority, Dispatch dispatch, Status status)
: _authority(std::move(authority))
, _dispatch(std::move(dispatch))
, _status(std::move(status)) {
	_clock.start();
	connect(&_connections, &QLocalServer::newConnection, this, [=] { addConnection(); });
	_expiry.setInterval(1000);
	connect(&_expiry, &QTimer::timeout, this, [=] { expire(); });
}

void ControlWorker::configure(quint64 epoch, ControlEndpointOptions options) {
	stop();
	_epoch = epoch;
	const auto path = SelectControlSocketPath(options);
	if (path.isEmpty()) { _status(epoch, {}, {}); return; }
	if (!_endpoint.open(path)) { _status(epoch, {}, _endpoint.error()); return; }
	_notifier = std::make_unique<QSocketNotifier>(_endpoint.descriptor(), QSocketNotifier::Read, this);
	connect(_notifier.get(), &QSocketNotifier::activated, this, [=] { acceptReady(); });
	_expiry.start();
	_status(epoch, path, {});
}

void ControlWorker::stop() {
	++_epoch;
	_expiry.stop();
	_notifier.reset();
	while (!_clients.empty()) drop(_clients.begin()->first);
	_endpoint.close();
}

void ControlWorker::acceptReady() {
	for (auto i = 0; i < kControlClients; ++i) {
		auto fd = _endpoint.accept();
		if (fd < 0) return;
		const auto release = gsl::finally([&] {
#ifndef Q_OS_WIN
			if (fd >= 0) ::close(fd);
#endif
		});
		if (_clients.size() >= kControlClients) continue;
		auto socket = new QLocalSocket(&_connections);
		if (!socket->setSocketDescriptor(std::exchange(fd, -1))) {
			delete socket;
			continue;
		}
		_connections.enqueue(socket);
	}
}

void ControlWorker::addConnection() {
	const auto socket = _connections.nextPendingConnection();
	if (!socket) return;
	const auto id = ++_nextClient;
	_clients.emplace(id, Client{ .socket = socket, .activity = _clock.elapsed() });
	socket->setReadBufferSize(kControlLineBytes + 1);
	connect(socket, &QLocalSocket::readyRead, this, [=] { read(id); });
	connect(socket, &QLocalSocket::disconnected, this, [=] { drop(id); });
	if (socket->bytesAvailable()) read(id);
}

void ControlWorker::drop(quint64 client) {
	const auto i = _clients.find(client);
	if (i == _clients.end()) return;
	const auto socket = i->second.socket;
	for (const auto &[id, pending] : i->second.pending) pending.active->store(false);
	_pending -= i->second.pending.size();
	_clients.erase(i);
	socket->disconnect(this);
	socket->abort();
	socket->deleteLater();
}

void ControlWorker::send(quint64 client, QJsonValue id, ControlResult result) {
	const auto i = _clients.find(client);
	if (i == _clients.end()) return;
	const auto bytes = EncodeControlReply(id, std::move(result));
	const auto socket = i->second.socket;
	if (socket->bytesToWrite() + bytes.size() > 2 * kControlLineBytes
		|| socket->write(bytes) != bytes.size()) drop(client);
}

void ControlWorker::read(quint64 client) {
	for (auto lines = 0; lines < 32; ++lines) {
		const auto i = _clients.find(client);
		if (i == _clients.end()) return;
		auto &entry = i->second;
		entry.input += entry.socket->read(kControlLineBytes + 1 - entry.input.size());
		entry.activity = _clock.elapsed();
		const auto newline = entry.input.indexOf('\n');
		if (newline < 0) {
			if (entry.input.size() > kControlLineBytes) drop(client);
			return;
		}
		const auto line = entry.input.first(newline);
		entry.input.remove(0, newline + 1);
		request(client, line);
		const auto remaining = _clients.find(client);
		if (remaining == _clients.end()) return;
		if (remaining->second.input.isEmpty() && !remaining->second.socket->bytesAvailable()) return;
	}
	QTimer::singleShot(0, this, [=] { read(client); });
}

void ControlWorker::request(quint64 client, const QByteArray &line) {
	const auto i = _clients.find(client);
	if (i == _clients.end()) return;
	auto &entry = i->second;
	const auto parsed = ParseControlRequest(line);
	if (!entry.rate.take(_clock.elapsed())) {
		send(client, parsed.id, ControlError(u"busy"_q, u"Request rate exceeded."_q));
		drop(client);
		return;
	}
	if (!parsed.request) { send(client, parsed.id, parsed.error); return; }
	const auto &request = *parsed.request;
	const auto policy = std::atomic_load(&_authority->policy);
	auto error = AuthorizeControlCommand(request.command, *policy);
	if (!error.code.isEmpty()) { send(client, request.id, std::move(error)); return; }
	if (entry.pending.size() >= kControlClientPending || _pending >= kControlPending
		|| entry.pending.contains(request.id)) {
		send(client, request.id, ControlError(u"busy"_q, u"Too many pending requests or duplicate id."_q));
		return;
	}
	const auto lease = std::make_shared<ControlDispatchLease>(_authority);
	if (!lease->acquired()) {
		send(client, request.id, ControlError(u"busy"_q, u"The main command queue is full."_q));
		return;
	}
	const auto serial = ++_nextSerial;
	const auto active = std::make_shared<std::atomic<bool>>(true);
	entry.pending.emplace(request.id, Pending{
		serial, policy->generation, request.command, _clock.elapsed(), active });
	++_pending;
	_dispatch(_epoch, client, serial, policy->generation, active, lease, request);
}

void ControlWorker::reply(quint64 epoch, quint64 client, quint64 serial, ControlResult result) {
	if (epoch != _epoch) return;
	const auto i = _clients.find(client);
	if (i == _clients.end()) return;
	auto &pending = i->second.pending;
	const auto p = std::find_if(pending.begin(), pending.end(), [&](const auto &p) {
		return p.second.serial == serial;
	});
	if (p == pending.end()) return;
	const auto policy = std::atomic_load(&_authority->policy);
	if (policy->generation != p->second.generation) {
		result = AuthorizeControlCommand(p->second.command, *policy);
		if (result.code.isEmpty()) result = ControlError(u"busy"_q, u"Automation settings changed; retry."_q);
	}
	const auto id = p->first;
	p->second.active->store(false);
	pending.erase(p);
	--_pending;
	send(client, id, std::move(result));
}

void ControlWorker::expire() {
	auto expired = std::vector<quint64>();
	const auto now = _clock.elapsed();
	for (const auto &[id, client] : _clients) {
		const auto overdue = std::any_of(client.pending.begin(), client.pending.end(), [&](const auto &entry) {
			return now - entry.second.started >= 10000;
		});
		if (overdue || now - client.activity >= 30000) expired.push_back(id);
	}
	for (const auto id : expired) drop(id);
}

}

struct ControlServer::State {
	std::shared_ptr<ControlAuthority> authority = std::make_shared<ControlAuthority>();
	QThread thread;
	ControlWorker *worker = nullptr;
	ControlHandler handler;
	ControlStatus status;
	quint64 epoch = 0;
	bool stopped = false;
};

ControlServer::ControlServer(ControlHandler handler, ControlStatus status, QObject *parent)
: QObject(parent)
, _state(std::make_unique<State>()) {
	auto &state = *_state;
	state.handler = std::move(handler);
	state.status = std::move(status);
	state.worker = new ControlWorker(state.authority,
		[this](quint64 epoch, quint64 client, quint64 serial, quint64 generation,
				ControlWorker::Ticket active, std::shared_ptr<ControlDispatchLease> lease,
				ControlRequest request) {
			QMetaObject::invokeMethod(this, [=, this] {
				(void)lease;
				auto &state = *_state;
				if (state.stopped || state.epoch != epoch || !active->load()) return;
				const auto weak = QPointer<ControlServer>(this);
				const auto mainThread = thread();
				const auto reply = [=](ControlResult result) {
					if (QThread::currentThread() != mainThread || !active->load()
						|| !weak || weak->_state->stopped || weak->_state->epoch != epoch) return;
					const auto worker = weak->_state->worker;
					QMetaObject::invokeMethod(worker, [=] { worker->reply(epoch, client, serial, result); });
				};
				const auto policy = std::atomic_load(&state.authority->policy);
				auto error = AuthorizeControlCommand(request.command, *policy);
				if (error.code.isEmpty() && generation != policy->generation) {
					error = ControlError(u"busy"_q, u"Automation settings changed; retry."_q);
				}
				if (!error.code.isEmpty()) reply(std::move(error));
				else {
					const auto handler = state.handler;
					handler(request, *policy, reply);
				}
			});
		}, [this](quint64 epoch, QString path, QString error) {
			QMetaObject::invokeMethod(this, [=, this] {
				if (!_state->stopped && _state->epoch == epoch) {
					const auto status = _state->status;
					status(path, error);
				}
			});
		});
	state.worker->moveToThread(&state.thread);
	connect(&state.thread, &QThread::finished, state.worker, &QObject::deleteLater);
	state.thread.start();
}

ControlServer::~ControlServer() {
	stop();
	_state->thread.quit();
	_state->thread.wait();
}

void ControlServer::configure(ControlEndpointOptions options) {
	auto &state = *_state;
	if (state.stopped) return;
	const auto epoch = ++state.epoch;
	const auto worker = state.worker;
	const auto weak = QPointer<ControlServer>(this);
	const auto status = state.status;
	status({}, {});
	if (!weak || weak->_state->stopped || weak->_state->epoch != epoch) return;
	QMetaObject::invokeMethod(worker, [=] { worker->configure(epoch, options); });
}

void ControlServer::setPolicy(ControlPolicy policy) {
	auto &state = *_state;
	policy.generation = std::atomic_load(&state.authority->policy)->generation + 1;
	std::atomic_store(&state.authority->policy, std::make_shared<const ControlPolicy>(policy));
}

void ControlServer::stop() {
	auto &state = *_state;
	if (std::exchange(state.stopped, true)) return;
	++state.epoch;
	const auto worker = state.worker;
	QMetaObject::invokeMethod(worker, [=] { worker->stop(); }, Qt::BlockingQueuedConnection);
	const auto status = state.status;
	status({}, {});
}

}
