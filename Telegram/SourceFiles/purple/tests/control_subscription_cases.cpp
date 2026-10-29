#include "purple/purple_control_server.h"

#include "base/basic_types.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtNetwork/QLocalSocket>
#include <cstdio>
#include <set>

namespace {
using namespace Purple;
auto checks = 0;
auto failures = 0;
void Check(bool value, const char *name) {
	++checks;
	if (!value) { ++failures; std::printf("FAIL: %s\n", name); }
}
bool Wait(const std::function<bool()> &condition, int milliseconds = 4000) {
	auto clock = QElapsedTimer();
	clock.start();
	while (!condition() && clock.elapsed() < milliseconds) {
		QCoreApplication::processEvents();
		QThread::msleep(1);
	}
	return condition();
}
void Pump(int milliseconds) {
	Wait([] { return false; }, milliseconds);
}
QByteArray Request(qint64 id, const QString &command, QJsonObject args = {}) {
	return QJsonDocument(QJsonObject{
		{u"v"_q, 1}, {u"id"_q, id}, {u"cmd"_q, command}, {u"args"_q, args},
	}).toJson(QJsonDocument::Compact) + '\n';
}
QJsonObject Topics(std::initializer_list<QString> names) {
	auto array = QJsonArray();
	for (const auto &name : names) array.append(name);
	return {{u"topics"_q, array}};
}
QJsonObject Receive(QLocalSocket &socket) {
	Check(Wait([&] { return socket.canReadLine(); }), "bounded socket line arrives");
	return QJsonDocument::fromJson(socket.readLine()).object();
}
QString Code(const QJsonObject &value) {
	return value.value(u"error"_q).toObject().value(u"code"_q).toString();
}
class Fixture final {
public:
	Fixture();
	std::unique_ptr<QLocalSocket> socket();
	void policyChanged();
	QTemporaryDir directory{u"/private/tmp/psub-XXXXXX"_q};
	ControlPolicy policy;
	ControlMediaSnapshot media{ .type = ControlMediaType::Music, .state = ControlMediaState::Playing,
		.name = u"song"_q, .title = u"Title"_q, .performer = u"Artist"_q };
	QString path;
	int snapshots = 0;
	int commands = 0;
	bool defer = false;
	bool deferCommands = false;
	bool padded = false;
	bool padCommands = false;
	int sequence = 0;
	int padding = 60000;
	QString eventError;
	std::vector<ControlReply> replies;
	std::vector<std::function<bool()>> active;
	std::unique_ptr<ControlServer> server;

};
Fixture::Fixture() {
	Check(directory.isValid(), "owned private fixture directory");
	policy.enabled = true;
	server = std::make_unique<ControlServer>([=, this](const auto &request, const auto &current, auto reply) {
		++commands;
		if (padCommands) {
			reply({QJsonObject{{u"padding"_q, QString(60000, 'x')}}});
			return;
		}
		if (!deferCommands) DispatchControlRequest(request, current, u"test"_q, {}, reply);
	}, [=, this](auto value, auto) { path = value; }, nullptr,
		[=, this](ControlTopic topic, const ControlPolicy &current, auto ticket, ControlReply reply) {
			Check(QThread::currentThread() == QCoreApplication::instance()->thread(), "snapshot adapter executes on main");
			++snapshots;
			if (defer) { replies.push_back(reply); active.push_back(ticket); return; }
			if (!eventError.isEmpty()) {
				reply(ControlError(eventError, u"Adapter failure."_q));
				return;
			}
			if (topic == ControlTopic::Lock) {
				auto value = QJsonObject{{u"locked"_q, current.locked}};
				if (padded) value.insert(u"padding"_q, QString(padding, 'x'));
				reply({value});
			} else {
				auto result = ControlMediaInfo(media, {});
				if (padded) {
					result.value.insert(u"sequence"_q, sequence);
					result.value.insert(u"padding"_q, QString(padding, 'x'));
				}
				reply(result);
			}
		});
	server->setPolicy(policy);
	server->configure({.configuredPath = directory.path() + u"/s"_q});
	Check(Wait([&] { return !path.isEmpty(); }), "worker socket ready");
}
std::unique_ptr<QLocalSocket> Fixture::socket() {
	auto result = std::make_unique<QLocalSocket>();
	result->connectToServer(path);
	Check(Wait([&] { return result->state() == QLocalSocket::ConnectedState; }), "private socket connected");
	return result;
}
void Fixture::policyChanged() { server->setPolicy(policy); }
void Admission() {
	auto f = Fixture(); auto socket = f.socket();
	socket->write(Request(1, u"hello"_q));
	const auto hello = Receive(*socket).value(u"result"_q).toObject();
	Check(hello.value(u"version"_q) == 1 && hello.value(u"minor"_q) == 1,
		"hello advertises v1.1 without changing wire version");
	for (const auto &args : {QJsonObject(), Topics({u"unknown"_q}), Topics({}),
		Topics({u"media.state"_q, u"media.state"_q}), QJsonObject{{u"topics"_q, 1}},
		QJsonObject{{u"topics"_q, QJsonArray{u"lock.state"_q}}, {u"extra"_q, true}}}) {
		socket->write(Request(2, u"subscribe"_q, args));
		Check(Code(Receive(*socket)) == u"bad-request"_q, "invalid topic set refused atomically");
	}
	f.policy.groups[2] = false; f.policyChanged();
	socket->write(Request(3, u"subscribe"_q, Topics({u"media.state"_q, u"lock.state"_q})));
	Check(Code(Receive(*socket)) == u"capability-off"_q, "each topic requires its own capability");
	f.policy.groups[2] = true; f.policy.enabled = false; f.policyChanged();
	socket->write(Request(4, u"subscribe"_q, Topics({u"lock.state"_q})));
	Check(Code(Receive(*socket)) == u"automation-disabled"_q, "subscribe requires master switch");
	f.policy.enabled = true; f.policyChanged();
	socket->write(Request(5, u"subscribe"_q, Topics({u"media.state"_q, u"lock.state"_q})));
	Check(Receive(*socket).value(u"result"_q).toObject().value(u"topics"_q).toArray().size() == 2,
		"acknowledgement lists accepted topics before events");
	auto seen = std::set<QString>();
	for (auto i = 0; i < 2; ++i) seen.insert(Receive(*socket).value(u"event"_q).toString());
	Check(seen == std::set<QString>{u"media.state"_q, u"lock.state"_q}, "initial snapshot for each topic");
	socket->write(Request(50, u"subscribe"_q, Topics({u"lock.state"_q})));
	Check(Receive(*socket).value(u"result"_q).toObject().value(u"topics"_q).toArray()
		== QJsonArray{u"lock.state"_q}, "resubscribe atomically replaces entire topic set");
	Check(Receive(*socket).value(u"event"_q) == u"lock.state"_q, "replacement publishes only its accepted topic");
	socket->write(Request(51, u"subscribe"_q, Topics({u"unknown"_q})));
	Check(Code(Receive(*socket)) == u"bad-request"_q, "invalid replacement preserves admitted set");
	f.server->notify(ControlTopic::Media); f.server->notify(ControlTopic::Lock);
	Check(Receive(*socket).value(u"event"_q) == u"lock.state"_q, "failed replacement keeps old topic and removes no authority");
	socket->write(Request(6, u"unsubscribe"_q, {{u"extra"_q, true}}));
	Check(Code(Receive(*socket)) == u"bad-request"_q, "invalid unsubscribe does not mutate topics");
	socket->write(Request(7, u"unsubscribe"_q));
	Check(Receive(*socket).value(u"result"_q).toObject().value(u"topics"_q).toArray().isEmpty(), "unsubscribe ends all topics");
	const auto count = f.snapshots;
	f.server->notify(ControlTopic::Media); f.server->notify(ControlTopic::Lock); Pump(250);
	Check(!socket->canReadLine() && f.snapshots == count, "unsubscribed client receives no snapshots");
}
void PrivacyAndPolicy() {
	auto f = Fixture(); auto socket = f.socket();
	f.media.hidden = true;
	socket->write(Request(1, u"subscribe"_q, Topics({u"media.state"_q, u"lock.state"_q})));
	Receive(*socket);
	auto hidden = QJsonObject();
	for (auto i = 0; i < 2; ++i) {
		auto event = Receive(*socket);
		if (event.value(u"event"_q) == u"media.state"_q) hidden = event.value(u"result"_q).toObject();
	}
	Check(hidden == ControlMediaInfo(f.media, {}).value && hidden.size() == 2,
		"media event uses identical Work Mode withholding as media.now");
	f.policy.locked = true; f.policyChanged();
	auto locked = QJsonObject(); auto lock = QJsonObject();
	for (auto i = 0; i < 2; ++i) {
		auto event = Receive(*socket);
		if (event.value(u"event"_q) == u"media.state"_q) locked = event;
		else lock = event;
	}
	Check(Code(locked) == u"locked"_q && !locked.contains(u"result"_q) && !locked.contains(u"ended"_q),
		"locked media event contains no metadata and keeps subscription");
	Check(lock.value(u"result"_q).toObject().value(u"locked"_q).toBool(), "lock topic publishes current lock state");
	f.policy.groups[2] = false; f.policyChanged();
	auto revoked = QJsonObject();
	for (auto i = 0; i < 2; ++i) {
		auto event = Receive(*socket);
		if (event.value(u"event"_q) == u"media.state"_q) revoked = event;
	}
	Check(Code(revoked) == u"capability-off"_q && revoked.value(u"ended"_q).toBool(), "capability revocation ends affected topic with reason");
	f.policy.enabled = false; f.policyChanged();
	const auto disabled = Receive(*socket);
	Check(Code(disabled) == u"automation-disabled"_q && disabled.value(u"ended"_q).toBool()
		&& disabled.value(u"event"_q) == u"lock.state"_q, "master revocation ends remaining topic");
}
void Fences() {
	auto f = Fixture(); auto socket = f.socket(); f.defer = true;
	socket->write(Request(1, u"subscribe"_q, Topics({u"media.state"_q}))); Receive(*socket);
	Check(Wait([&] { return f.replies.size() == 1; }), "first snapshot held");
	for (auto i = 0; i < 5000; ++i) f.server->notify(ControlTopic::Media);
	Pump(250);
	Check(f.replies.size() == 1 && !f.active[0](), "invalidations cancel but do not multiply unfinished snapshot work");
	f.replies[0]({QJsonObject{{u"stale"_q, true}}});
	Check(Wait([&] { return f.replies.size() == 2; }), "completion releases one latest replacement snapshot");
	f.replies[0]({QJsonObject{{u"stale"_q, true}}}); Pump(120);
	Check(!socket->canReadLine() && f.active[1](), "old revision cannot retire or publish newer snapshot");
	f.replies[1](ControlMediaInfo(f.media, {}));
	Check(Receive(*socket).value(u"result"_q).toObject() == ControlMediaInfo(f.media, {}).value, "newer snapshot publishes once");
	f.server->notify(ControlTopic::Media);
	Check(Wait([&] { return f.replies.size() == 3; }), "snapshot pending before policy change");
	f.policy.locked = true; f.policyChanged();
	f.replies[2]({QJsonObject{{u"private"_q, true}}});
	Check(Code(Receive(*socket)) == u"locked"_q && !f.active[2](), "policy generation fences old private snapshot");
	f.policy.locked = false; f.policyChanged();
	Check(Wait([&] { return f.replies.size() == 4; }), "unlock refreshes retained subscription");
	socket->write(Request(2, u"unsubscribe"_q)); Receive(*socket);
	f.replies[3]({QJsonObject{{u"stale"_q, true}}}); Pump(150);
	Check(!socket->canReadLine() && !f.active[3](), "unsubscribe cancels in-flight snapshot");
	socket->write(Request(3, u"subscribe"_q, Topics({u"lock.state"_q}))); Receive(*socket);
	Check(Wait([&] { return f.replies.size() == 5; }), "replacement topic has new serial");
	const auto old = f.replies[4];
	f.server->configure({.configuredPath = f.directory.path() + u"/s2"_q});
	Check(Wait([&] { return f.path.endsWith(u"s2"_q); }) && !f.active[4](), "reconfigure cancels subscriptions and tickets");
	old({QJsonObject{{u"locked"_q, true}}});
	auto replacement = f.socket(); replacement->write(Request(4, u"ping"_q));
	Check(Receive(*replacement).value(u"id"_q).toInteger() == 4 && !replacement->canReadLine(), "old epoch never reaches replacement client");
	replacement->write(Request(5, u"subscribe"_q, Topics({u"lock.state"_q}))); Receive(*replacement);
	Check(Wait([&] { return f.replies.size() == 6; }), "fresh subscription after reconfigure");
	replacement->abort(); Check(Wait([&] { return !f.active[5](); }), "disconnect retires snapshot ticket");
	f.server->stop();
	f.replies[5]({QJsonObject{{u"locked"_q, true}}});
	Check(!f.active[5](), "late callback after stop is inert");
}
void RapidRevocation() {
	for (const auto topic : { u"media.state"_q, u"lock.state"_q }) {
		for (const auto master : { false, true }) {
			auto f = Fixture(); auto socket = f.socket(); f.defer = true;
			socket->write(Request(1, u"subscribe"_q, Topics({ topic }))); Receive(*socket);
			Check(Wait([&] { return f.replies.size() == 1; }), "revocation starts with held snapshot");
			const auto group = topic == u"media.state"_q ? 2 : 3;
			if (master) f.policy.enabled = false; else f.policy.groups[group] = false;
			f.policyChanged();
			if (master) f.policy.enabled = true; else f.policy.groups[group] = true;
			f.policyChanged();
			f.replies[0]({ QJsonObject{{u"private"_q, true}} });
			const auto ended = Receive(*socket);
			Check(ended.value(u"ended"_q).toBool()
				&& Code(ended) == (master ? u"automation-disabled"_q : u"capability-off"_q)
				&& !ended.contains(u"result"_q), "rapid off/on terminates old subscription without leaking old snapshot");
			socket->write(Request(2, u"subscribe"_q, Topics({ topic }))); Receive(*socket);
			Check(Wait([&] { return f.replies.size() == 2; }), "fresh admission after reenable gets a new snapshot");
			f.replies[1](topic == u"media.state"_q ? ControlMediaInfo(f.media, {})
				: ControlResult{ QJsonObject{{u"locked"_q, false}} });
			Check(Receive(*socket).contains(u"result"_q), "earlier revocation cannot poison fresh admission");
		}
	}
}

void BoundedOperations() {
	auto f = Fixture(); f.defer = true;
	for (auto i = 0; i < kControlPending; ++i) {
		auto socket = f.socket();
		socket->write(Request(1, u"subscribe"_q, Topics({u"media.state"_q}))); Receive(*socket);
		Check(Wait([&] { return int(f.replies.size()) == i + 1; }), "one retained asynchronous operation per churned subscription");
		socket->abort();
		Check(Wait([&] { return !f.active[i](); }), "disconnect cancels ticket while underlying operation remains counted");
	}
	auto socket = f.socket();
	socket->write(Request(1, u"subscribe"_q, Topics({u"media.state"_q}))); Receive(*socket);
	Pump(250);
	Check(f.replies.size() == kControlPending, "global64 actual-work lease prevents disconnected-operation queue growth");
	f.replies[0]({QJsonObject{}});
	Check(Wait([&] { return f.replies.size() == kControlPending + 1; }), "old cancelled completion releases exactly one actual-work slot");
	f.replies[0]({QJsonObject{}}); Pump(150);
	Check(f.replies.size() == kControlPending + 1 && f.active.back()(), "duplicate completion cannot release a second lease or retire current ticket");
	f.replies.back()(ControlMediaInfo(f.media, {}));
	Check(Receive(*socket).contains(u"result"_q), "latest admitted work completes after lease recovery");
}

void ErrorsAndTimeouts() {
	for (const auto code : { u"unsupported"_q, u"not-found"_q }) {
		auto f = Fixture(); f.eventError = code; auto socket = f.socket();
		socket->write(Request(1, u"subscribe"_q, Topics({u"media.state"_q}))); Receive(*socket);
		const auto event = Receive(*socket);
		Check(Code(event) == code && event.value(u"ended"_q).toBool(), "adapter errors end topic with typed terminal reason");
	}
	{
		auto f = Fixture(); f.padded = true; f.padding = kControlLineBytes + 1; auto socket = f.socket();
		socket->write(Request(1, u"subscribe"_q, Topics({u"media.state"_q}))); Receive(*socket);
		Check(Wait([&] { return socket->state() == QLocalSocket::UnconnectedState; }), "oversize event drops connection before retaining or queueing it");
	}
	{
		auto f = Fixture(); f.defer = true; auto socket = f.socket();
		socket->write(Request(1, u"subscribe"_q, Topics({u"media.state"_q}))); Receive(*socket);
		Check(Wait([&] { return f.replies.size() == 1; }), "timeout starts with held asynchronous snapshot");
		Check(Wait([&] { return socket->canReadLine(); }, 12000), "unfinished snapshot has bounded ten-second deadline");
		const auto event = Receive(*socket);
		Check(Code(event) == u"busy"_q && event.value(u"ended"_q).toBool() && !f.active[0](), "snapshot timeout ends topic and cancels ticket");
		f.replies[0](ControlMediaInfo(f.media, {})); Pump(150);
		Check(!socket->canReadLine(), "timed out completion never publishes");
	}
}

void SlowReader() {
	auto f = Fixture(); auto socket = f.socket(); f.padded = true;
	socket->write(Request(1, u"subscribe"_q, Topics({u"media.state"_q}))); Receive(*socket);
	socket->setReadBufferSize(1);
	for (auto i = 1; i <= 30; ++i) {
		f.sequence = i;
		for (auto n = 0; n < 200; ++n) f.server->notify(ControlTopic::Media);
		Pump(25);
	}
	const auto calls = f.snapshots;
	f.sequence = 999; f.server->notify(ControlTopic::Media); Pump(400);
	Check(f.snapshots <= calls + 1 && socket->state() == QLocalSocket::ConnectedState,
		"stalled reader stops snapshot production without disconnect or growing event queue");
	socket->setReadBufferSize(0);
	auto latest = false; auto lines = 0;
	Check(Wait([&] {
		while (socket->canReadLine()) {
			const auto line = socket->readLine(); ++lines;
			Check(line.size() <= kControlLineBytes, "every pushed event stays within line cap");
			const auto value = QJsonDocument::fromJson(line).object();
			if (value.value(u"result"_q).toObject().value(u"sequence"_q).toInt() == 999) latest = true;
		}
		return latest;
	}), "resumed reader eventually receives newest coalesced state");
	Check(lines < 12 && f.snapshots < 12, "thousands of invalidations retain bounded snapshots rather than history");
}
void OutboundCap() {
	auto f = Fixture(); f.padded = true; f.padCommands = true;
	auto socket = f.socket();
	socket->write(Request(1, u"subscribe"_q, Topics({u"media.state"_q, u"lock.state"_q}))); Receive(*socket);
	socket->setReadBufferSize(1);
	Pump(300);
	for (auto id = 2; id < 10; ++id) socket->write(Request(id, u"ping"_q));
	Pump(500);
	socket->setReadBufferSize(0);
	auto lines = 0;
	Check(Wait([&] {
		while (socket->canReadLine()) {
			const auto line = socket->readLine(); ++lines;
			Check(line.size() <= kControlLineBytes, "mixed topic/command traffic respects line cap");
		}
		return socket->state() == QLocalSocket::UnconnectedState;
	}), "two large topic snapshots plus command traffic enforce aggregate outbound cap");
	Check(lines < 10, "outbound overflow drops instead of retaining unbounded command history");
}

void Limits() {
	auto f = Fixture(); auto socket = f.socket(); f.deferCommands = true;
	socket->write(Request(1, u"subscribe"_q, Topics({u"media.state"_q, u"lock.state"_q}))); Receive(*socket);
	Receive(*socket); Receive(*socket);
	for (auto id = 2; id <= 7; ++id) socket->write(Request(id, u"ping"_q));
	Check(Wait([&] { return f.commands == 6; }), "six requests plus two topics fill per-client slots");
	socket->write(Request(8, u"ping"_q));
	Check(Code(Receive(*socket)) == u"busy"_q && f.commands == 6, "subscriptions count against per-client pending cap");
	socket->write(Request(9, u"unsubscribe"_q)); Receive(*socket);
	socket->write(Request(10, u"ping"_q));
	Check(Wait([&] { return f.commands == 7; }), "unsubscribe releases slots immediately");
	socket->abort(); Pump(50);
	auto clients = std::vector<std::unique_ptr<QLocalSocket>>();
	for (auto n = 0; n < 16; ++n) {
		auto client = f.socket();
		client->write(Request(1, u"subscribe"_q, Topics({u"media.state"_q, u"lock.state"_q}))); Receive(*client);
		client->write(Request(2, u"ping"_q)); client->write(Request(3, u"ping"_q));
		clients.push_back(std::move(client));
	}
	Check(Wait([&] { return f.commands == 39; }), "16 clients with two topics and two requests fill total64 slots");
	for (auto &client : clients) while (client->canReadLine()) client->readLine();
	clients[0]->write(Request(4, u"ping"_q));
	auto response = QJsonObject();
	Check(Wait([&] {
		while (clients[0]->canReadLine()) {
			auto value = QJsonDocument::fromJson(clients[0]->readLine()).object();
			if (value.contains(u"id"_q)) response = value;
		}
		return !response.isEmpty();
	}) && Code(response) == u"busy"_q, "subscriptions count against total pending cap");
	clients[0]->abort(); Pump(100);
	auto fresh = f.socket(); fresh->write(Request(1, u"subscribe"_q, Topics({u"lock.state"_q})));
	Check(Receive(*fresh).value(u"ok"_q).toBool(), "disconnect releases total slots for a fresh subscription");
}
}
int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	Admission(); PrivacyAndPolicy(); Fences(); RapidRevocation(); BoundedOperations();
	ErrorsAndTimeouts(); SlowReader(); OutboundCap(); Limits();
	std::printf("purple subscriptions: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
