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

struct ControlRevocation {
	quint64 generation = 0;
	ControlResult reason;
};

constexpr auto kSnapshotTimeout = qint64(10000);

struct ControlAuthority {
	std::shared_ptr<const ControlPolicy> policy
		= std::make_shared<const ControlPolicy>();
	std::atomic<int> queued = 0;
	std::atomic<int> eventWork = 0;
	std::array<std::shared_ptr<const ControlRevocation>, 2> revocations = {};
	std::array<std::atomic<quint64>, 2> revisions = {};
};

struct ControlLockTransition {
	ControlPolicy before;
	std::atomic<quint64> allowedGeneration = 0;
};

class ControlDispatchLease final {
public:
	explicit ControlDispatchLease(std::shared_ptr<ControlAuthority> authority);
	~ControlDispatchLease();
	[[nodiscard]] bool acquired() const;

private:
	std::shared_ptr<ControlAuthority> _authority;

};

class ControlEventLease final {
public:
	explicit ControlEventLease(std::shared_ptr<ControlAuthority> authority);
	~ControlEventLease();
	[[nodiscard]] bool acquired() const;
	[[nodiscard]] std::shared_ptr<std::atomic<bool>> finished() const;
	bool complete();

private:
	std::shared_ptr<ControlAuthority> _authority;
	std::shared_ptr<std::atomic<bool>> _finished
		= std::make_shared<std::atomic<bool>>(false);

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
	using EventDispatch = std::function<void(
		quint64, quint64, quint64, quint64, quint64, ControlTopic, Ticket,
		std::shared_ptr<ControlDispatchLease>, std::shared_ptr<ControlEventLease>)>;
	using Status = std::function<void(quint64, QString, QString)>;
	ControlWorker(std::shared_ptr<ControlAuthority> authority, Dispatch dispatch, Status status, EventDispatch events);
	void configure(quint64 epoch, ControlEndpointOptions options);
	void stop();
	void reply(quint64 epoch, quint64 client, quint64 serial, quint64 lockGeneration, ControlResult result);
	void eventReply(quint64 epoch, quint64 client, quint64 serial, quint64 generation,
		quint64 revision, ControlTopic topic, ControlResult result);

private:
	struct Pending {
		quint64 serial = 0;
		quint64 generation = 0;
		QString command;
		qint64 started = 0;
		Ticket active;
	};
	struct Subscription {
		quint64 serial = 0;
		quint64 generation = 0;
		quint64 revision = 0;
		Ticket active;
		Ticket finished;
		QByteArray latest;
		quint64 admitted = 0;
		qint64 started = 0;
		bool dirty = true;
		bool ended = false;
	};
	struct Client {
		QLocalSocket *socket = nullptr;
		QByteArray input;
		ControlRateLimit rate;
		std::map<qint64, Pending> pending;
		std::map<ControlTopic, Subscription> subscriptions;
		qint64 activity = 0;
		qint64 terminalStarted = 0;
	};
	void acceptReady();
	void addConnection();
	void read(quint64 client);
	void request(quint64 client, const QByteArray &line);
	void send(quint64 client, QJsonValue id, ControlResult result);
	void drop(quint64 client);
	void expire();
	void subscriptions();
	void subscribe(quint64 client, const ControlRequest &request);
	void clearSubscriptions(Client &client);
	void write(quint64 client, const QByteArray &bytes);
	void stage(quint64 client, ControlTopic topic, ControlResult result, bool ended);
	[[nodiscard]] qint64 stagedBytes(const Client &client) const;

	std::shared_ptr<ControlAuthority> _authority;
	Dispatch _dispatch;
	Status _status;
	EventDispatch _events;
	ControlEndpoint _endpoint;
	PendingConnections _connections{ this };
	std::unique_ptr<QSocketNotifier> _notifier;
	QTimer _expiry{ this };
	QTimer _updates{ this };
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

ControlEventLease::ControlEventLease(std::shared_ptr<ControlAuthority> authority)
: _authority(std::move(authority)) {
	if (_authority->eventWork.fetch_add(1) >= kControlPending) {
		_authority->eventWork.fetch_sub(1);
		_authority.reset();
		_finished->store(true);
	}
}

ControlEventLease::~ControlEventLease() {
	complete();
}

bool ControlEventLease::acquired() const {
	return _authority != nullptr;
}

std::shared_ptr<std::atomic<bool>> ControlEventLease::finished() const {
	return _finished;
}

bool ControlEventLease::complete() {
	if (!_authority || _finished->exchange(true)) return false;
	_authority->eventWork.fetch_sub(1);
	return true;
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
		std::shared_ptr<ControlAuthority> authority, Dispatch dispatch, Status status, EventDispatch events)
: _authority(std::move(authority))
, _dispatch(std::move(dispatch))
, _status(std::move(status))
, _events(std::move(events)) {
	_clock.start();
	_updates.setInterval(100);
	connect(&_updates, &QTimer::timeout, this, [=] { subscriptions(); });
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
	_updates.start();
	_status(epoch, path, {});
}

void ControlWorker::stop() {
	++_epoch;
	_expiry.stop();
	_updates.stop();
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
	clearSubscriptions(i->second);
	_clients.erase(i);
	socket->disconnect(this);
	socket->abort();
	socket->deleteLater();
}

void ControlWorker::send(quint64 client, QJsonValue id, ControlResult result) {
	write(client, EncodeControlReply(id, std::move(result)));
}

void ControlWorker::write(quint64 client, const QByteArray &bytes) {
	const auto i = _clients.find(client);
	if (i == _clients.end()) return;
	const auto socket = i->second.socket;
	if (bytes.size() > kControlLineBytes || socket->bytesToWrite() + stagedBytes(i->second) + bytes.size() > 2 * kControlLineBytes
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
	if (entry.pending.contains(request.id)) {
		send(client, request.id, ControlError(u"busy"_q, u"Duplicate pending id."_q));
		return;
	}
	if (request.command == u"subscribe"_q || request.command == u"unsubscribe"_q) {
		subscribe(client, request);
		return;
	}
	if (entry.pending.size() + entry.subscriptions.size() >= kControlClientPending
		|| _pending >= kControlPending) {
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

void ControlWorker::reply(quint64 epoch, quint64 client, quint64 serial, quint64 lockGeneration, ControlResult result) {
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
		auto authorization = AuthorizeControlCommand(p->second.command, *policy);
		const auto ownLockTransition = p->second.command == u"lock"_q
			&& lockGeneration == policy->generation && lockGeneration == p->second.generation + 1;
		if (!authorization.code.isEmpty()) result = std::move(authorization);
		else if (!ownLockTransition) result = ControlError(u"busy"_q, u"Automation settings changed; retry."_q);
	}
	const auto id = p->first;
	p->second.active->store(false);
	pending.erase(p);
	--_pending;
	send(client, id, std::move(result));
}

void ControlWorker::clearSubscriptions(Client &client) {
	for (const auto &[topic, subscription] : client.subscriptions) {
		if (subscription.active) subscription.active->store(false);
	}
	_pending -= client.subscriptions.size();
	client.subscriptions.clear();
}

void ControlWorker::subscribe(quint64 client, const ControlRequest &request) {
	auto &entry = _clients.at(client);
	if (request.command == u"unsubscribe"_q) {
		if (!request.arguments.isEmpty()) {
			send(client, request.id, ControlError(u"bad-request"_q, u"unsubscribe takes no arguments."_q));
			return;
		}
		clearSubscriptions(entry);
		send(client, request.id, { QJsonObject{ { u"topics"_q, QJsonArray() } } });
		return;
	}
	const auto topics = ParseControlTopics(request.arguments);
	if (!topics) {
		send(client, request.id, ControlError(u"bad-request"_q, u"Expected unique media.state or lock.state topics."_q));
		return;
	}
	const auto policy = std::atomic_load(&_authority->policy);
	for (const auto topic : *topics) {
		auto error = AuthorizeControlTopic(topic, *policy);
		if (!error.code.isEmpty()) { send(client, request.id, std::move(error)); return; }
	}
	const auto extra = int(topics->size()) - int(entry.subscriptions.size());
	if (entry.pending.size() + topics->size() > kControlClientPending || _pending + extra > kControlPending) {
		send(client, request.id, ControlError(u"busy"_q, u"Subscription limit exceeded."_q));
		return;
	}
	clearSubscriptions(entry);
	auto names = QJsonArray();
	for (const auto topic : *topics) {
		entry.subscriptions.emplace(topic, Subscription{
			.serial = ++_nextSerial, .admitted = policy->generation });
		++_pending;
		names.append(ControlTopicName(topic));
	}
	send(client, request.id, { QJsonObject{ { u"topics"_q, names } } });
}

qint64 ControlWorker::stagedBytes(const Client &client) const {
	auto bytes = qint64(0);
	for (const auto &[topic, subscription] : client.subscriptions) {
		bytes += subscription.latest.size();
	}
	return bytes;
}

void ControlWorker::stage(quint64 client, ControlTopic topic, ControlResult result, bool ended) {
	const auto i = _clients.find(client);
	if (i == _clients.end()) return;
	const auto p = i->second.subscriptions.find(topic);
	if (p == i->second.subscriptions.end()) return;
	auto &subscription = p->second;
	const auto bytes = EncodeControlEvent(topic, std::move(result), ended);
	if (bytes.size() > kControlLineBytes
		|| i->second.socket->bytesToWrite() + stagedBytes(i->second)
			- subscription.latest.size() + bytes.size() > 2 * kControlLineBytes) {
		drop(client);
		return;
	}
	if (subscription.active) subscription.active->store(false);
	subscription.active.reset();
	subscription.latest = bytes;
	subscription.dirty = false;
	subscription.ended = ended;
	if (ended) subscription.started = _clock.elapsed();
	else if (!subscription.finished || subscription.finished->load()) subscription.started = 0;
}

void ControlWorker::subscriptions() {
	auto clients = std::vector<quint64>();
	for (const auto &[client, entry] : _clients) clients.push_back(client);
	for (const auto client : clients) {
		const auto currentClient = _clients.find(client);
		if (currentClient == _clients.end()) continue;
		if (!currentClient->second.socket->bytesToWrite()) currentClient->second.terminalStarted = 0;
		if (currentClient->second.terminalStarted
			&& _clock.elapsed() - currentClient->second.terminalStarted >= kSnapshotTimeout) {
			drop(client);
			continue;
		}
		for (const auto topic : { ControlTopic::Media, ControlTopic::Lock }) {
			const auto i = _clients.find(client);
			if (i == _clients.end()) break;
			auto &entry = i->second;
			const auto p = entry.subscriptions.find(topic);
			if (p == entry.subscriptions.end()) continue;
			auto &subscription = p->second;
			const auto now = _clock.elapsed();
			if (subscription.ended) {
				if (now - subscription.started >= kSnapshotTimeout) drop(client);
				continue;
			}
			const auto policy = std::atomic_load(&_authority->policy);
			const auto revision = _authority->revisions[int(topic)].load();
			const auto revocation = std::atomic_load(&_authority->revocations[int(topic)]);
			if (revocation && revocation->generation > subscription.admitted) {
				stage(client, topic, revocation->reason, true);
				continue;
			}
			if (subscription.generation != policy->generation || subscription.revision != revision) {
				if (subscription.active) subscription.active->store(false);
				subscription.active.reset();
				subscription.latest.clear();
				subscription.dirty = true;
				subscription.generation = policy->generation;
				subscription.revision = revision;
			}
			auto authorization = AuthorizeControlTopic(topic, *policy);
			if (!authorization.code.isEmpty()) {
				stage(client, topic, std::move(authorization), true);
				continue;
			}
			if (!subscription.active && !subscription.dirty
				&& (!subscription.finished || subscription.finished->load())) subscription.started = 0;
			if (subscription.started && now - subscription.started >= kSnapshotTimeout) {
				stage(client, topic, ControlError(u"busy"_q, u"State snapshot timed out."_q), true);
				continue;
			}
			if (!subscription.dirty || subscription.active) continue;
			auto snapshotAuthorization = AuthorizeControlEvent(topic, *policy);
			if (!snapshotAuthorization.code.isEmpty()) {
				const auto ended = snapshotAuthorization.code != u"locked"_q;
				stage(client, topic, std::move(snapshotAuthorization), ended);
				continue;
			}
			if (subscription.finished && !subscription.finished->load()) continue;
			if (entry.socket->bytesToWrite()) {
				subscription.started = 0;
				continue;
			}
			if (!subscription.started) subscription.started = now;
			const auto lease = std::make_shared<ControlDispatchLease>(_authority);
			if (!lease->acquired()) continue;
			const auto operation = std::make_shared<ControlEventLease>(_authority);
			if (!operation->acquired()) continue;
			subscription.active = std::make_shared<std::atomic<bool>>(true);
			subscription.finished = operation->finished();
			subscription.started = now;
			subscription.dirty = false;
			_events(_epoch, client, subscription.serial, policy->generation, revision,
				topic, subscription.active, lease, operation);
		}
		for (const auto topic : { ControlTopic::Media, ControlTopic::Lock }) {
			const auto i = _clients.find(client);
			if (i == _clients.end() || i->second.socket->bytesToWrite()) break;
			auto &topics = i->second.subscriptions;
			const auto p = topics.find(topic);
			if (p == topics.end() || p->second.latest.isEmpty()) continue;
			auto &subscription = p->second;
			auto authorization = ControlResult();
			if (!subscription.ended) {
				const auto current = std::atomic_load(&_authority->policy);
				const auto revoked = std::atomic_load(&_authority->revocations[int(topic)]);
				if (subscription.generation != current->generation
					|| subscription.revision != _authority->revisions[int(topic)].load()
					|| (revoked && revoked->generation > subscription.admitted)) continue;
				authorization = AuthorizeControlEvent(topic, *current);
			}
			auto bytes = std::exchange(subscription.latest, {});
			auto ended = subscription.ended;
			if (!ended && !authorization.code.isEmpty()) {
				ended = authorization.code != u"locked"_q;
				bytes = EncodeControlEvent(topic, std::move(authorization), ended);
			}
			if (ended) { topics.erase(p); --_pending; }
			write(client, bytes);
			const auto remaining = _clients.find(client);
			if (ended && remaining != _clients.end() && remaining->second.socket->bytesToWrite()
				&& !remaining->second.terminalStarted) remaining->second.terminalStarted = _clock.elapsed();
		}
	}
}

void ControlWorker::eventReply(quint64 epoch, quint64 client, quint64 serial, quint64 generation,
		quint64 revision, ControlTopic topic, ControlResult result) {
	if (epoch != _epoch) return;
	const auto i = _clients.find(client);
	if (i == _clients.end()) return;
	const auto p = i->second.subscriptions.find(topic);
	if (p == i->second.subscriptions.end() || p->second.serial != serial || !p->second.active
		|| p->second.generation != generation || p->second.revision != revision) return;
	auto &subscription = p->second;
	if (!subscription.active->exchange(false)) return;
	subscription.active.reset();
	const auto policy = std::atomic_load(&_authority->policy);
	const auto revoked = std::atomic_load(&_authority->revocations[int(topic)]);
	if (generation != policy->generation || revision != _authority->revisions[int(topic)].load()
		|| (revoked && revoked->generation > subscription.admitted)) {
		subscription.dirty = true;
		return;
	}
	const auto ended = !result.code.isEmpty()
		&& (topic != ControlTopic::Media || result.code != u"locked"_q);
	stage(client, topic, std::move(result), ended);
}

void ControlWorker::expire() {
	auto expired = std::vector<quint64>();
	const auto now = _clock.elapsed();
	for (const auto &[id, client] : _clients) {
		const auto overdue = std::any_of(client.pending.begin(), client.pending.end(), [&](const auto &entry) {
			return now - entry.second.started >= 10000;
		});
		if (overdue || (client.subscriptions.empty() && now - client.activity >= 30000)) expired.push_back(id);
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
	ControlEventHandler events;
	quint64 epoch = 0;
	bool stopped = false;
	std::shared_ptr<ControlLockTransition> lockTransition;
};

ControlServer::ControlServer(ControlHandler handler, ControlStatus status, QObject *parent,
		ControlEventHandler events)
: QObject(parent)
, _state(std::make_unique<State>()) {
	auto &state = *_state;
	state.handler = std::move(handler);
	state.events = std::move(events);
	state.status = std::move(status);
	state.worker = new ControlWorker(state.authority,
		[this](quint64 epoch, quint64 client, quint64 serial, quint64 generation,
				ControlWorker::Ticket active, std::shared_ptr<ControlDispatchLease> lease,
				ControlRequest request) {
			QMetaObject::invokeMethod(this, [=, this]() mutable {
				(void)lease;
				auto &state = *_state;
				if (state.stopped || state.epoch != epoch || !active->load()) return;
				const auto weak = QPointer<ControlServer>(this);
				const auto mainThread = thread();
				const auto transition = std::make_shared<ControlLockTransition>();
				const auto reply = [=](ControlResult result) {
					if (QThread::currentThread() != mainThread || !active->load()
						|| !weak || weak->_state->stopped || weak->_state->epoch != epoch) return;
					const auto worker = weak->_state->worker;
					const auto lockGeneration = transition->allowedGeneration.load();
					QMetaObject::invokeMethod(worker, [=] { worker->reply(epoch, client, serial, lockGeneration, result); });
				};
				const auto policy = std::atomic_load(&state.authority->policy);
				auto error = AuthorizeControlCommand(request.command, *policy);
				if (error.code.isEmpty() && generation != policy->generation) {
					error = ControlError(u"busy"_q, u"Automation settings changed; retry."_q);
				}
				if (!error.code.isEmpty()) reply(std::move(error));
				else {
					request.active = [active] { return active->load(); };
					const auto previous = state.lockTransition;
					const auto restore = gsl::finally([=] {
						if (weak) weak->_state->lockTransition = previous;
					});
					transition->before = *policy;
					state.lockTransition = request.command == u"lock"_q ? transition : nullptr;
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
		}, [this](quint64 epoch, quint64 client, quint64 serial, quint64 generation, quint64 revision,
				ControlTopic topic, ControlWorker::Ticket active, std::shared_ptr<ControlDispatchLease> lease,
				std::shared_ptr<ControlEventLease> operation) {
			QMetaObject::invokeMethod(this, [=, this] {
				(void)lease;
				if (_state->stopped || _state->epoch != epoch || !active->load()) return;
				const auto weak = QPointer<ControlServer>(this);
				const auto mainThread = thread();
				const auto reply = [=](ControlResult result) {
					if (QThread::currentThread() != mainThread || !operation->complete() || !active->load()
						|| !weak || weak->_state->stopped || weak->_state->epoch != epoch) return;
					const auto worker = weak->_state->worker;
					QMetaObject::invokeMethod(worker, [=] {
						worker->eventReply(epoch, client, serial, generation, revision, topic, result);
					});
				};
				const auto policy = std::atomic_load(&_state->authority->policy);
				if (generation != policy->generation) {
					reply(ControlError(u"busy"_q, u"Policy changed."_q));
					return;
				}
				auto error = AuthorizeControlEvent(topic, *policy);
				const auto handler = _state->events;
				if (!error.code.isEmpty()) reply(std::move(error));
				else if (handler) handler(topic, *policy, [active] { return active->load(); }, reply);
				else reply(ControlError(u"unsupported"_q, u"State adapter unavailable."_q));
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
	const auto before = std::atomic_load(&state.authority->policy);
	policy.generation = before->generation + 1;
	if (const auto transition = state.lockTransition) {
		if (transition->before.generation == before->generation
			&& !before->locked && policy.locked
			&& before->enabled == policy.enabled && before->groups == policy.groups) {
			transition->allowedGeneration.store(policy.generation);
		}
	}
	for (const auto topic : { ControlTopic::Media, ControlTopic::Lock }) {
		auto reason = AuthorizeControlTopic(topic, policy);
		if (!reason.code.isEmpty()) {
			std::atomic_store(&state.authority->revocations[int(topic)],
				std::make_shared<const ControlRevocation>(ControlRevocation{
					policy.generation, std::move(reason) }));
		}
	}
	std::atomic_store(&state.authority->policy, std::make_shared<const ControlPolicy>(policy));
}

void ControlServer::notify(ControlTopic topic) {
	_state->authority->revisions[int(topic)].fetch_add(1);
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
