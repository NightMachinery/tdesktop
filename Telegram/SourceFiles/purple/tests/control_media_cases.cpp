#include "purple/purple_control_commands.h"
#include "purple/purple_control_media.h"
#include "purple/purple_control_server.h"

#include "base/basic_types.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtNetwork/QLocalSocket>
#include <cstdio>
#include <limits>
#include <thread>

namespace {
using namespace Purple;
auto checks = 0;
auto failures = 0;
void Check(bool value, const char *name) {
	++checks;
	if (!value) { ++failures; std::printf("FAIL: %s\n", name); }
}
bool Wait(const std::function<bool()> &condition) {
	auto clock = QElapsedTimer();
	clock.start();
	while (!condition() && clock.elapsed() < 4000) {
		QCoreApplication::processEvents();
		QThread::msleep(1);
	}
	return condition();
}
QByteArray Request(qint64 id, const QString &command, QJsonObject args = {}) {
	return QJsonDocument(QJsonObject{
		{ u"v"_q, 1 }, { u"id"_q, id }, { u"cmd"_q, command }, { u"args"_q, args },
	}).toJson(QJsonDocument::Compact) + '\n';
}
void SnapshotCases() {
	auto snapshot = ControlMediaSnapshot{
		.type = ControlMediaType::Music, .state = ControlMediaState::Playing,
		.position = 1250, .duration = 9000, .name = u"song.ogg"_q,
		.path = u"/owned/song.ogg"_q, .mime = u"audio/ogg"_q,
		.title = u"Title"_q, .performer = u"Artist"_q,
	};
	auto value = ControlMediaInfo(snapshot, snapshot.path).value;
	Check(value.value(u"type"_q) == u"music"_q && value.value(u"state"_q) == u"playing"_q
		&& value.value(u"position_ms"_q).toInteger() == 1250
		&& value.value(u"duration_ms"_q).toInteger() == 9000
		&& value.value(u"music"_q).toObject().value(u"performer"_q) == u"Artist"_q,
		"music metadata, playback state and millisecond timeline");
	Check(value.value(u"file"_q).toObject().value(u"path"_q) == snapshot.path,
		"only verified current path is advertised");
	Check(ControlMediaInfo(snapshot, {}).value.value(u"file"_q).toObject().value(u"path"_q).isNull(),
		"not downloaded media has null path");
	Check(ControlMediaInfo(snapshot, u"/owned/other"_q).value.value(u"file"_q).toObject().value(u"path"_q).isNull(),
		"media switch after path check cannot advertise the previous file");
	snapshot.type = ControlMediaType::Voice;
	snapshot.state = ControlMediaState::Paused;
	snapshot.sender = u"Sender"_q;
	snapshot.chat = u"Chat"_q;
	snapshot.date = 123;
	value = ControlMediaInfo(snapshot, {}).value;
	Check(value.value(u"type"_q) == u"voice"_q && value.value(u"state"_q) == u"paused"_q
		&& value.value(u"voice"_q).toObject().value(u"sender"_q) == u"Sender"_q
		&& value.value(u"voice"_q).toObject().value(u"chat"_q) == u"Chat"_q
		&& value.value(u"voice"_q).toObject().value(u"date"_q).toInteger() == 123
		&& !value.contains(u"music"_q), "voice metadata never includes music fields");
	snapshot.hidden = true;
	value = ControlMediaInfo(snapshot, snapshot.path).value;
	Check(value.size() == 2 && value.value(u"hidden"_q).toBool()
		&& value.value(u"file"_q).toObject().size() == 2
		&& value.value(u"file"_q).toObject().value(u"name"_q) == snapshot.name,
		"Work Mode withholds all fields except hidden and filename/path");
	value = ControlMediaInfo({}, {}).value;
	Check(value.value(u"type"_q).isNull() && value.value(u"state"_q) == u"stopped"_q
		&& value.value(u"position_ms"_q).toInteger() == 0,
		"nothing playing is an explicit stopped null type");
	Check(ControlMediaMilliseconds(48001, 48000) == 1000
		&& ControlMediaMilliseconds(2, 3) == 666
		&& ControlMediaMilliseconds(-1, 48000) == 0
		&& ControlMediaMilliseconds(1, 0) == 0
		&& ControlMediaMilliseconds(std::numeric_limits<qint64>::max(), 1)
			== std::numeric_limits<qint64>::max(), "sample conversion clamps invalid and overflowing times");
	auto locked = 0;
	Check(ControlLock(false, [&] { ++locked; return true; }).code == u"no-passcode"_q && locked == 0,
		"no passcode never invokes lock");
	Check(ControlLock(true, [&] { ++locked; return true; }).value.value(u"locked"_q).toBool() && locked == 1,
		"configured passcode locks successfully");
	Check(ControlLock(true, [] { return false; }).code == u"busy"_q,
		"failed lock never claims success");
}
void PathCases() {
	auto temporary = QTemporaryDir(u"/private/tmp/pmedia-XXXXXX"_q);
	Check(temporary.isValid(), "owned short private path fixture");
	if (!temporary.isValid()) return;
	const auto path = temporary.path() + u"/audio"_q;
	auto file = QFile(path);
	Check(file.open(QIODevice::WriteOnly) && file.write("inert audio") == 11, "synthetic downloaded file");
	file.close();
	const auto link = temporary.path() + u"/link"_q;
	Check(QFile::link(path, link), "owned symlink fixture");
	Check(ExistingControlMediaPath(path).isEmpty(), "GUI thread performs no path IO");
	auto worker = std::thread([&] {
		Check(ExistingControlMediaPath(path) == path, "worker accepts downloaded regular file");
		Check(ExistingControlMediaPath(link).isEmpty(), "worker rejects symlink");
		Check(ExistingControlMediaPath(temporary.path()).isEmpty(), "worker rejects directory");
		Check(ExistingControlMediaPath(path + u"-missing"_q).isEmpty(), "worker rejects missing file");
		Check(ExistingControlMediaPath(u"relative"_q).isEmpty(), "worker rejects relative path");
	});
	worker.join();
}
void DispatchCases() {
	auto policy = ControlPolicy();
	policy.enabled = true;
	auto controls = 0;
	auto reads = 0;
	auto locks = 0;
	auto lastAction = ControlMediaAction::Toggle;
	auto lastType = std::optional<ControlMediaType>();
	const auto media = ControlMediaAdapter{
		.control = [&](auto action, auto type) {
			++controls; lastAction = action; lastType = type;
			return ControlResult{ QJsonObject{{u"accepted"_q, true}} };
		},
		.now = [&](auto type, auto active, auto reply) {
			++reads; lastType = type;
			if (!active || active()) reply(ControlMediaInfo({}, {}));
		},
		.lock = [&] { ++locks; return ControlLock(true, [] { return true; }); },
	};
	const auto call = [&](QString cmd, QJsonObject args = {}) {
		auto result = ControlResult();
		const auto parsed = ParseControlRequest(Request(1, cmd, args).chopped(1));
		DispatchControlRequest(*parsed.request, policy, u"synthetic"_q, {},
			[&](auto value) { result = value; }, {}, media);
		return result;
	};
	for (const auto &[cmd, action] : std::vector<std::pair<QString, ControlMediaAction>>{
		{u"media.toggle"_q, ControlMediaAction::Toggle}, {u"media.play"_q, ControlMediaAction::Play},
		{u"media.pause"_q, ControlMediaAction::Pause},
	}) {
		Check(call(cmd).code.isEmpty() && !lastType && lastAction == action, "default action uses active media");
		for (const auto &type : { u"music"_q, u"voice"_q }) {
			Check(call(cmd, {{u"type"_q, type}}).code.isEmpty()
				&& lastType == (type == u"music"_q ? ControlMediaType::Music : ControlMediaType::Voice)
				&& lastAction == action, "explicit music and voice controls route correctly");
		}
	}
	Check(controls == 9, "one control call per admitted request");
	for (const auto &args : { QJsonObject{{u"type"_q, u"video"_q}}, QJsonObject{{u"type"_q, 1}},
		QJsonObject{{u"extra"_q, true}}, QJsonObject{{u"type"_q, u"music"_q}, {u"extra"_q, true}} }) {
		for (const auto &cmd : { u"media.play"_q, u"media.now"_q }) {
			Check(call(cmd, args).code == u"bad-request"_q, "invalid media arguments refuse before adapter");
		}
	}
	Check(controls == 9 && reads == 0, "bad arguments have no media side effects");
	Check(call(u"media.now"_q, {{u"type"_q, u"voice"_q}}).code.isEmpty()
		&& reads == 1 && lastType == ControlMediaType::Voice, "voice now routes requested voice snapshot");
	policy.locked = true;
	Check(call(u"media.now"_q).code == u"locked"_q && reads == 1, "locked info never reads metadata");
	Check(call(u"media.play"_q).code.isEmpty() && controls == 10, "playback remains available while locked");
	Check(call(u"lock"_q).code.isEmpty() && locks == 1, "lock remains idempotently available while locked");
	Check(call(u"lock"_q, {{u"extra"_q, true}}).code == u"bad-request"_q && locks == 1,
		"lock rejects arguments without side effects");
}
void LockGenerationCases() {
	auto temporary = QTemporaryDir(u"/private/tmp/plock-XXXXXX"_q);
	Check(temporary.isValid(), "owned lock socket directory");
	if (!temporary.isValid()) return;
	auto policy = ControlPolicy();
	policy.enabled = true;
	auto deferred = ControlReply();
	auto calls = 0;
	auto published = QString();
	auto server = std::unique_ptr<ControlServer>();
	server = std::make_unique<ControlServer>([&](const auto &request, const auto &current, auto reply) {
		Check(QThread::currentThread() == QCoreApplication::instance()->thread(), "lock adapter runs on main");
		const auto media = ControlMediaAdapter{ .lock = [&] {
			++calls;
			policy.locked = true;
			server->setPolicy(policy);
			return ControlLock(true, [] { return true; });
		} };
		if (request.id > 1) deferred = reply;
		else DispatchControlRequest(request, current, u"synthetic"_q, {}, reply, {}, media);
	}, [&](auto path, auto) { published = path; });
	const auto path = temporary.path() + u"/s"_q;
	server->configure({ .configuredPath = path });
	server->setPolicy(policy);
	Check(Wait([&] { return published == path; }), "real worker endpoint ready");
	auto socket = QLocalSocket();
	socket.connectToServer(path);
	Check(Wait([&] { return socket.state() == QLocalSocket::ConnectedState; }), "lock socket connected");
	socket.write(Request(1, u"lock"_q));
	Check(Wait([&] { return socket.canReadLine(); }), "self policy change preserves lock reply");
	auto response = QJsonDocument::fromJson(socket.readLine()).object();
	Check(response.value(u"ok"_q).toBool() && response.value(u"result"_q).toObject().value(u"locked"_q).toBool()
		&& calls == 1, "successful lock survives its own passcode generation change");
	for (const auto master : { false, true }) {
		policy.enabled = true; policy.groups[3] = true;
		server->setPolicy(policy);
		deferred = {};
		socket.write(Request(master ? 3 : 2, u"lock"_q));
		Check(Wait([&] { return bool(deferred); }), "lock request deferred before revocation");
		if (master) policy.enabled = false;
		else policy.groups[3] = false;
		server->setPolicy(policy);
		if (deferred) deferred(ControlLock(true, [] { return true; }));
		Check(Wait([&] { return socket.canReadLine(); }), "revocation reply arrives");
		response = QJsonDocument::fromJson(socket.readLine()).object();
		Check(response.value(u"error"_q).toObject().value(u"code"_q)
			== (master ? u"automation-disabled"_q : u"capability-off"_q),
			"lock generation exception never bypasses master or group revocation");
	}
}
void LockReenableCases() {
	auto temporary = QTemporaryDir(u"/private/tmp/plockrace-XXXXXX"_q);
	Check(temporary.isValid(), "owned lock reenable socket directory");
	if (!temporary.isValid()) return;
	auto policy = ControlPolicy();
	policy.enabled = true;
	auto deferred = ControlReply();
	auto transition = false;
	auto published = QString();
	auto server = std::unique_ptr<ControlServer>();
	server = std::make_unique<ControlServer>([&](const auto &, const auto &, auto reply) {
		if (transition) {
			policy.locked = true;
			server->setPolicy(policy);
		}
		deferred = reply;
	}, [&](auto path, auto) { published = path; });
	const auto path = temporary.path() + u"/s"_q;
	server->configure({ .configuredPath = path });
	server->setPolicy(policy);
	Check(Wait([&] { return published == path; }), "lock reenable worker endpoint ready");
	auto socket = QLocalSocket();
	socket.connectToServer(path);
	Check(Wait([&] { return socket.state() == QLocalSocket::ConnectedState; }), "lock reenable socket connected");
	auto id = qint64(0);
	for (const auto ownTransition : { false, true }) {
		for (const auto master : { false, true }) {
			policy.enabled = true; policy.groups[3] = true; policy.locked = false;
			server->setPolicy(policy);
			transition = ownTransition;
			deferred = {};
			socket.write(Request(++id, u"lock"_q));
			Check(Wait([&] { return bool(deferred); }), "lock completion retained before disable and reenable");
			if (master) policy.enabled = false;
			else policy.groups[3] = false;
			server->setPolicy(policy);
			policy.enabled = true; policy.groups[3] = true;
			server->setPolicy(policy);
			if (deferred) deferred(ControlLock(true, [] { return true; }));
			Check(Wait([&] { return socket.canReadLine(); }), "reenabled policy produces a fenced reply");
			const auto response = QJsonDocument::fromJson(socket.readLine()).object();
			Check(!response.value(u"ok"_q).toBool()
				&& response.value(u"error"_q).toObject().value(u"code"_q) == u"busy"_q,
				"reenable cannot restore an old lock completion, including after its own transition");
		}
	}
	for (const auto unrelated : { false, true }) {
		policy.enabled = true; policy.groups.fill(true); policy.locked = false;
		server->setPolicy(policy); transition = true; deferred = {};
		socket.write(Request(++id, u"lock"_q));
		Check(Wait([&] { return bool(deferred); }), "own transition retained for exact generation test");
		if (unrelated) policy.groups[0] = false;
		server->setPolicy(policy);
		if (deferred) deferred(ControlLock(true, [] { return true; }));
		Check(Wait([&] { return socket.canReadLine(); }), "later generation produces a reply");
		const auto response = QJsonDocument::fromJson(socket.readLine()).object();
		Check(response.value(u"error"_q).toObject().value(u"code"_q) == u"busy"_q,
			"any later generation, including unchanged policy, invalidates own-lock exception");
	}
	policy.groups.fill(true); policy.locked = false; server->setPolicy(policy);
	transition = true; deferred = {};
	socket.write(Request(++id, u"lock"_q));
	Check(Wait([&] { return bool(deferred); }), "exact own transition completion retained");
	if (deferred) deferred(ControlLock(true, [] { return true; }));
	Check(Wait([&] { return socket.canReadLine(); }), "exact own transition reply arrives");
	Check(QJsonDocument::fromJson(socket.readLine()).object().value(u"ok"_q).toBool(),
		"only the exact own unlocked-to-locked generation is accepted");
}

}

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	SnapshotCases(); PathCases(); DispatchCases(); LockGenerationCases(); LockReenableCases();
	std::printf("purple media control: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
