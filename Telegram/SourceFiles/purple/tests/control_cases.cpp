#include "purple/purple_control_protocol.h"
#include "purple/purple_control_commands.h"
#include "purple/purple_control_server.h"
#include "base/basic_types.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtNetwork/QLocalSocket>
#include <cstdio>
#include <functional>
#include <cstring>
#include <memory>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
using namespace Purple;
auto checks = 0;
auto failures = 0;
void Check(bool value, const char *name) {
	++checks;
	if (!value) { ++failures; std::printf("FAIL: %s\n", name); }
}
bool Wait(const std::function<bool()> &condition, int milliseconds = 4000) {
	auto timer = QElapsedTimer();
	timer.start();
	while (!condition() && timer.elapsed() < milliseconds) {
		QCoreApplication::processEvents();
		QThread::msleep(1);
	}
	return condition();
}
QByteArray Request(qint64 id, QString command, QJsonObject args = {}) {
	return QJsonDocument(QJsonObject{
		{ u"v"_q, 1 }, { u"id"_q, id }, { u"cmd"_q, command }, { u"args"_q, args },
	}).toJson(QJsonDocument::Compact) + '\n';
}
QJsonObject Receive(QLocalSocket &socket) {
	if (!Wait([&] { return socket.canReadLine(); })) return {};
	return QJsonDocument::fromJson(socket.readLine()).object();
}
void PolicyCases() {
	const auto good = Request(17, u"ping"_q).chopped(1);
	const auto parsed = ParseControlRequest(good);
	Check(parsed.request && parsed.request->id == 17, "valid v1 frame");
	for (const auto &bytes : { QByteArray("[]"), QByteArray("null"), QByteArray("{}"),
		good + 'x', good + char(0xff), QByteArray(kControlLineBytes + 1, 'x'),
		QByteArray("{\"v\":1,\"id\":1.5,\"cmd\":\"ping\",\"args\":{}}"),
		QByteArray("{\"v\":1,\"id\":true,\"cmd\":\"ping\",\"args\":{}}"),
		QByteArray("{\"v\":1,\"id\":1,\"cmd\":\"ping\",\"args\":[]}") }) {
		Check(!ParseControlRequest(bytes).request, "malformed frame cannot dispatch");
	}
	Check(ParseControlRequest(good + QByteArray(kControlLineBytes - good.size(), ' ')).request.has_value(),
		"line exactly 64KiB is accepted");
	Check(ParseControlRequest("{\"v\":2,\"id\":1,\"cmd\":\"ping\",\"args\":{}}").error.code == u"unsupported"_q,
		"unsupported protocol version");
	for (const auto id : { std::numeric_limits<qint64>::min(), std::numeric_limits<qint64>::max(), qint64(9007199254740993LL) }) {
		const auto p = ParseControlRequest(Request(id, u"ping"_q).chopped(1));
		Check(p.request && p.request->id == id, "integer ids retain exact signed64 identity");
	}
	auto rate = ControlRateLimit();
	for (auto i = 0; i < 20; ++i) Check(rate.take(1000), "burst capacity");
	Check(!rate.take(1000) && !rate.take(999), "burst limit and rollback do not grant tokens");
	Check(rate.take(1100) && !rate.take(1100), "ten requests per monotonic second");
	auto policy = ControlPolicy();
	Check(AuthorizeControlCommand(u"hello"_q, policy).code.isEmpty(), "hello allowed with master off");
	for (const auto &name : ControlCommandNames()) {
		if (name.toString() != u"hello"_q) Check(AuthorizeControlCommand(name.toString(), policy).code == u"automation-disabled"_q,
			"all other registered commands refuse master off");
	}
	policy.enabled = true;
	for (const auto enabled : policy.groups) Check(enabled, "all five groups default on");
	policy.groups[0] = false;
	Check(AuthorizeControlCommand(u"window.list"_q, policy).code == u"capability-off"_q, "window group refusal");
	policy.groups[0] = true;
	policy.locked = true;
	Check(AuthorizeControlCommand(u"window.list"_q, policy).code == u"locked"_q, "window titles refuse locked");
	Check(AuthorizeControlCommand(u"ping"_q, policy).code.isEmpty(), "ping allowed while locked with master enabled");
	policy.locked = false;
	for (const auto &name : { u"media.next"_q, u"automation-off"_q }) {
		Check(AuthorizeControlCommand(name, policy).code == u"unsupported"_q, "future and settings commands unavailable");
	}
	auto activated = -1;
	const auto adapter = ControlWindowAdapter{
		.list = [] { return std::vector<ControlWindow>{ {u"one"_q, true, true}, {u"two"_q, false, false} }; },
		.activate = [&](int index) { activated = index; return index >= 0 && index < 2; },
	};
	const auto call = [&](QString command, QJsonObject args = {}) {
		auto result = ControlResult();
		DispatchControlRequest(*ParseControlRequest(Request(1, command, args)).request, policy, u"synthetic"_q, adapter, [&](auto value) { result = value; });
		return result;
	};
	Check(call(u"window.list"_q).value.value(u"windows"_q).toArray().size() == 2, "window list routes fake main adapter");
	Check(call(u"window.activate"_q, {{u"index"_q, 1}}).code.isEmpty() && activated == 1, "window activate index");
	Check(call(u"window.activate"_q, {{u"index"_q, 0.5}}).code == u"bad-request"_q && activated == 1, "fractional activate never defaults to zero");
	Check(call(u"window.activate"_q, {{u"index"_q, 5}}).code == u"not-found"_q, "missing index refusal");
	Check(call(u"window.cycle"_q).code.isEmpty() && activated == 1, "window cycle routes adapter");
	Check(call(u"hello"_q).value.value(u"commands"_q).toArray().size() == 13, "hello advertises only implemented commands");
	auto consent = ControlConsent();
	auto old = consent.begin();
	Check(!consent.confirm(old), "enable requires first step");
	Check(consent.advance(old), "first local confirmation advances");
	consent.invalidate();
	Check(!consent.confirm(old), "local disable invalidates old final dialog");
	old = consent.begin();
	const auto fresh = consent.begin();
	consent.cancel(old);
	Check(!consent.advance(old) && consent.advance(fresh) && consent.confirm(fresh), "old cancellation cannot retire fresh consent");
	Check(!consent.confirm(fresh), "consent cannot be replayed");
	Check(ValidControlSocketPath({}) && ValidControlSocketPath(u"/private/owned/s"_q)
		&& !ValidControlSocketPath(u"relative"_q) && !ValidControlSocketPath(u"/x/../s"_q)
		&& !ValidControlSocketPath(QString(104, 'x')), "configured path validation is lexical and bounded");
	Check(!ValidControlSocketPath(DecodeControlSocketPath(QByteArray(1, char(0xff))))
		&& DecodeControlSocketPath("/private/s") == u"/private/s"_q,
		"corrupt UTF-8 setting fails closed instead of opening a default socket");
	Check(ControlPeerAllowed(500, 500) && !ControlPeerAllowed(501, 500), "peer uid mismatch refused");
	auto options = ControlEndpointOptions{ .workingDirectory = u"/other"_q, .defaultDirectory = [] { return u"/default"_q; }, .fallbackPath = u"/fallback/control.sock"_q };
	Check(SelectControlSocketPath(options).isEmpty(), "alternate profile opens no automatic socket");
	options.workingDirectory = u"/default/"_q;
	Check(SelectControlSocketPath(options) == u"/default/purple-control.sock"_q, "default profile lexical path");
	options.configuredPath = u"/explicit/control.sock"_q;
	Check(SelectControlSocketPath(options) == options.configuredPath, "explicit path overrides profile default");
}


QByteArray RawRequest(QByteArray id, QByteArray index = "0") {
	return "{\"v\":1,\"id\":" + id
		+ ",\"cmd\":\"window.activate\",\"args\":{\"index\":" + index + "}}";
}

void RawIntegerCases() {
	for (const auto token : {
		"9223372036854775808", "-9223372036854775809",
		"9223372036854776000", "-9223372036854776000",
		"9007199254740992.5", "9007199254740993.5",
		"-9007199254740992.5", "-9007199254740993.5",
		"1.00000000000000001", "-1.00000000000000001",
		"9.223372036854775808e18", "-9.223372036854775809e18",
		"1001e-3", "1e-100000", "1e100000", "true", "\"1\"",
	}) {
		const auto parsed = ParseControlRequest(RawRequest(token));
		Check(!parsed.request && parsed.id.isNull(),
			"raw fractional or overflowing id is never rounded into a request");
	}
	for (const auto &[token, expected] : std::vector<std::pair<QByteArray, qint64>>{
		{ "9223372036854775807", std::numeric_limits<qint64>::max() },
		{ "-9223372036854775808", std::numeric_limits<qint64>::min() },
		{ "9.223372036854775807e18", std::numeric_limits<qint64>::max() },
		{ "-9.223372036854775808e18", std::numeric_limits<qint64>::min() },
		{ "92233720368547758070e-1", std::numeric_limits<qint64>::max() },
		{ "-92233720368547758080e-1", std::numeric_limits<qint64>::min() },
		{ "9007199254740993.0", 9007199254740993LL },
		{ "90071992547409930e-1", 9007199254740993LL },
		{ "1.00000000000000000", 1 }, { "1000e-3", 1 },
		{ "0.01e2", 1 }, { "1E+0", 1 }, { "-0e-100000", 0 },
	}) {
		const auto parsed = ParseControlRequest(RawRequest(token));
		Check(parsed.request && parsed.request->id == expected,
			"exact decimal and exponent ids retain full signed64 precision");
		const auto reply = QJsonDocument::fromJson(EncodeControlReply(parsed.id, {}));
		Check(reply.object().value(u"id"_q).toInteger() == expected,
			"accepted raw id is echoed exactly");
	}
	auto policy = ControlPolicy();
	policy.enabled = true;
	auto activated = -1;
	const auto adapter = ControlWindowAdapter{
		.list = [] { return std::vector<ControlWindow>{
			{u"one"_q, true, true}, {u"two"_q, false, false},
		}; },
		.activate = [&](int index) { activated = index; return true; },
	};
	for (const auto token : {
		"1.00000000000000001", "0.99999999999999999",
		"9007199254740992.5", "9007199254740993.5",
		"9223372036854775808", "-9223372036854775809",
		"1001e-3", "1e-100000", "true", "\"1\"",
	}) {
		const auto parsed = ParseControlRequest(RawRequest("7", token));
		auto result = ControlResult();
		activated = -1;
		if (parsed.request) {
			DispatchControlRequest(*parsed.request, policy, u"synthetic"_q, adapter,
				[&](auto value) { result = std::move(value); });
		} else {
			result = parsed.error;
		}
		Check(result.code == u"bad-request"_q && activated == -1,
			"raw fractional or overflowing window index never activates");
	}
	for (const auto &token : {
		QByteArray("1.00000000000000000"), QByteArray("0.1e1"),
		QByteArray("1000E-3"), "1." + QByteArray(8000, '0'),
	}) {
		const auto parsed = ParseControlRequest(RawRequest("7", token));
		activated = -1;
		if (parsed.request) {
			DispatchControlRequest(*parsed.request, policy, u"synthetic"_q, adapter,
				[](auto) {});
		}
		Check(activated == 1, "exact integral raw window index is accepted");
	}
	Check(!ParseControlRequest(
		"{\"v\":1.00000000000000001,\"id\":7,\"cmd\":\"ping\",\"args\":{}}"
	).request, "protocol version uses its original numeric token");
	const auto escaped = ParseControlRequest(
		"{\"v\":1e0,\"i\\u0064\":7.0,\"cmd\":\"ping\","
		"\"args\":{\"text\":\"numbers 9.5 and \\\"1e2\\\" stay strings\"}}");
	Check(escaped.request && escaped.request->id == 7
		&& escaped.request->arguments.value(u"text"_q).isString(),
		"numeric token scanning preserves escaped names and string contents");
	for (auto mask = 0; mask != (1 << kControlGroupCount); ++mask) {
		for (const auto enabled : { false, true }) {
			for (const auto locked : { false, true }) {
				policy.enabled = enabled;
				policy.locked = locked;
				for (auto group = 0; group != kControlGroupCount; ++group) {
					policy.groups[group] = (mask & (1 << group)) != 0;
				}
				for (const auto &entry : ControlCommandNames()) {
					const auto command = entry.toString();
					const auto group = command.startsWith(u"window."_q) ? 0
						: command == u"emoji.render"_q ? 4
						: command == u"media.now"_q ? 2
						: command == u"lock"_q ? 3 : 1;
					const auto refusesLocked = group == 0 || group == 2 || group == 4;
					const auto expected = (command == u"hello"_q) ? QString()
						: !enabled ? u"automation-disabled"_q
						: (command == u"ping"_q || command == u"subscribe"_q || command == u"unsubscribe"_q) ? QString()
						: !policy.groups[group] ? u"capability-off"_q
						: (locked && refusesLocked) ? u"locked"_q : QString();
					Check(AuthorizeControlCommand(command, policy).code == expected,
						"every registered command follows the complete authorization matrix");
				}
			}
		}
	}
}

void IntegerWireCases() {
	auto temporary = QTemporaryDir(u"/private/tmp/pipc-XXXXXX"_q);
	Check(temporary.isValid(), "raw integer wire private directory");
	if (!temporary.isValid()) return;
	const auto path = temporary.path() + u"/s"_q;
	auto ready = false;
	auto replies = std::vector<ControlReply>();
	auto ids = std::vector<qint64>();
	auto server = ControlServer([&](const auto &request, const auto &, auto reply) {
		ids.push_back(request.id);
		replies.push_back(std::move(reply));
	}, [&](QString value, QString) { ready = (value == path); });
	auto policy = ControlPolicy();
	policy.enabled = true;
	server.setPolicy(policy);
	server.configure({ .configuredPath = path });
	Check(Wait([&] { return ready; }), "raw integer listener ready");
	auto socket = QLocalSocket();
	socket.connectToServer(path);
	Check(Wait([&] { return socket.state() == QLocalSocket::ConnectedState; }),
		"raw integer client admitted");
	socket.write(RawRequest("9007199254740992") + '\n');
	Check(Wait([&] { return replies.size() == 1; }), "first exact id remains pending");
	socket.write(RawRequest("9007199254740992.5") + '\n');
	const auto fraction = Receive(socket);
	Check(fraction.value(u"id"_q).isNull()
		&& fraction.value(u"error"_q).toObject().value(u"code"_q) == u"bad-request"_q,
		"rounded fractional id cannot collide with a pending integer id");
	socket.write(RawRequest("9007199254740993") + '\n');
	Check(Wait([&] { return replies.size() == 2; }),
		"adjacent signed64 ids above2to53 stay distinct while pending");
	socket.write(RawRequest("90071992547409920e-1") + '\n');
	const auto duplicate = Receive(socket);
	Check(duplicate.value(u"id"_q).toInteger() == 9007199254740992LL
		&& duplicate.value(u"error"_q).toObject().value(u"code"_q) == u"busy"_q,
		"mathematically equal exact id spellings retain duplicate detection");
	socket.write(RawRequest("-9223372036854775808") + '\n');
	Check(Wait([&] { return replies.size() == 3; }), "minimum signed64 id remains pending");
	socket.write(RawRequest("-9223372036854775809") + '\n');
	const auto overflow = Receive(socket);
	Check(overflow.value(u"id"_q).isNull()
		&& overflow.value(u"error"_q).toObject().value(u"code"_q) == u"bad-request"_q,
		"negative overflow cannot alias the pending minimum signed64 id");
	Check(ids == std::vector<qint64>{9007199254740992LL, 9007199254740993LL,
		std::numeric_limits<qint64>::min()}, "only exact ids reach the main handler");
	for (auto i = 0; i < replies.size(); ++i) {
		replies[i]({});
		const auto response = Receive(socket);
		Check(response.value(u"ok"_q).toBool()
			&& response.value(u"id"_q).toInteger() == ids[i],
			"pending replies preserve every accepted raw integer identity");
	}
}

void SocketCases() {
	auto temporary = QTemporaryDir(u"/private/tmp/pipc-XXXXXX"_q);
	Check(temporary.isValid(), "private socket test directory");
	if (!temporary.isValid()) return;
	const auto root = temporary.path();
	const auto path = root + u"/control.sock"_q;
	auto published = QString();
	auto error = QString();
	auto status = 0;
	auto actions = 0;
	auto deferred = std::vector<ControlReply>();
	auto server = std::make_unique<ControlServer>([&](const auto &request, const auto &policy, auto reply) {
		Check(QThread::currentThread() == QCoreApplication::instance()->thread(), "handler runs on main owner thread");
		if (request.command == u"window.activate"_q) { deferred.push_back(reply); ++actions; return; }
		DispatchControlRequest(request, policy, u"synthetic"_q, {}, reply);
	}, [&](QString value, QString message) { published = value; error = message; ++status; });
	const auto configure = [&](QString value) {
		const auto before = status;
		server->configure({ .configuredPath = value });
		return Wait([&] { return status >= before + 2; });
	};
	Check(configure(path) && published == path && error.isEmpty(), "real worker binds private endpoint");
	struct stat info = {};
	Check(lstat(path.toUtf8().constData(), &info) == 0 && S_ISSOCK(info.st_mode) && (info.st_mode & 0777) == 0600,
		"socket is0600 and parent remains0700");
	auto socket = QLocalSocket();
	socket.connectToServer(path);
	Check(Wait([&] { return socket.state() == QLocalSocket::ConnectedState; }), "same uid client admitted");
	socket.write(Request(1, u"hello"_q));
	Check(Receive(socket).value(u"ok"_q).toBool(), "hello works on actual socket while disabled");
	socket.write(Request(2, u"ping"_q));
	Check(Receive(socket).value(u"error"_q).toObject().value(u"code"_q).toString() == u"automation-disabled"_q,
		"socket cannot enable automation");
	auto policy = ControlPolicy();
	policy.enabled = true;
	server->setPolicy(policy);
	const auto split = Request(3, u"ping"_q);
	socket.write(split.first(4));
	Check(!socket.canReadLine(), "fragment does not dispatch prematurely");
	socket.write(split.sliced(4) + Request(4, u"ping"_q));
	Check(Receive(socket).value(u"id"_q).toInteger() == 3 && Receive(socket).value(u"id"_q).toInteger() == 4,
		"split and multiple frames preserve request identity");
	socket.write(Request(5, u"window.activate"_q, {{u"index"_q, 0}}));
	Check(Wait([&] { return deferred.size() == 1; }), "asynchronous handler retains bounded pending request");
	policy.enabled = false;
	server->setPolicy(policy);
	if (!deferred.empty()) deferred.back()({{{u"activated"_q, 0}}});
	Check(Receive(socket).value(u"error"_q).toObject().value(u"code"_q).toString() == u"automation-disabled"_q,
		"disable fences successful stale asynchronous reply");
	policy.enabled = true;
	server->setPolicy(policy);
	const auto before = actions;
	socket.write(Request(6, u"window.activate"_q, {{u"index"_q, 0}}));
	socket.flush();
	QThread::msleep(60);
	socket.abort();
	QThread::msleep(60);
	QCoreApplication::processEvents();
	Check(actions == before, "disconnect cancels queued main action before dispatch");
	const auto oldReply = deferred.empty() ? ControlReply([](auto) {}) : deferred.front();
	Check(configure(path), "same service reconfigures and retires connections");
	oldReply({{{u"activated"_q, 0}}});
	Check(published == path, "retired reply cannot clear current endpoint");
	const auto displaced = root + u"/displaced"_q;
	Check(QFile::rename(path, displaced), "test displaces listener pathname");
	auto sentinel = QFile(path);
	Check(sentinel.open(QIODevice::WriteOnly) && sentinel.write("sentinel") == 8, "replacement sentinel created");
	sentinel.close();
	server->stop();
	Check(QFileInfo(path).isFile(), "shutdown preserves replaced pathname");
	server.reset();
	oldReply({{{u"activated"_q, 0}}});
	Check(true, "late reply after destroyed owner is harmless");
}

QJsonObject BlockingReceive(QLocalSocket &socket) {
	auto timer = QElapsedTimer();
	timer.start();
	while (!socket.canReadLine() && timer.elapsed() < 4000) socket.waitForReadyRead(100);
	return QJsonDocument::fromJson(socket.readLine()).object();
}

void QueueBoundCases() {
	auto temporary = QTemporaryDir(u"/private/tmp/pipc-XXXXXX"_q);
	Check(temporary.isValid(), "queue test private directory");
	if (!temporary.isValid()) return;
	const auto path = temporary.path() + u"/s"_q;
	auto ready = false;
	auto actions = 0;
	auto server = ControlServer([&](const auto &request, const auto &policy, auto reply) {
		if (request.command == u"window.activate"_q) ++actions;
		DispatchControlRequest(request, policy, u"synthetic"_q, {}, reply);
	}, [&](QString value, QString) { ready = value == path; });
	auto policy = ControlPolicy();
	policy.enabled = true;
	server.setPolicy(policy);
	server.configure({ .configuredPath = path });
	Check(Wait([&] { return ready; }), "bounded queue listener ready");
	auto clients = std::vector<std::unique_ptr<QLocalSocket>>();
	for (auto client = 0; client < 8; ++client) {
		auto socket = std::make_unique<QLocalSocket>();
		socket->connectToServer(path);
		Check(socket->waitForConnected(1000), "queue client connects without dispatching main");
		auto frames = QByteArray();
		for (auto id = 0; id < 8; ++id) frames += Request(id, u"window.activate"_q);
		frames += Request(99, u"not-implemented"_q);
		socket->write(frames);
		socket->flush();
		Check(BlockingReceive(*socket).value(u"error"_q).toObject().value(u"code"_q).toString() == u"unsupported"_q,
			"worker barrier confirms eight pending per client while main is paused");
		clients.push_back(std::move(socket));
	}
	Check(actions == 0, "64 queued main actions have not executed");
	auto extra = QLocalSocket();
	extra.connectToServer(path);
	Check(extra.waitForConnected(1000), "extra bounded-queue client connects");
	extra.write(Request(100, u"window.activate"_q));
	extra.flush();
	Check(BlockingReceive(extra).value(u"error"_q).toObject().value(u"code"_q).toString() == u"busy"_q,
		"64 total pending requests refuse additional work");
	for (const auto &socket : clients) socket->abort();
	QThread::msleep(100);
	extra.write(Request(101, u"window.activate"_q));
	extra.flush();
	Check(BlockingReceive(extra).value(u"error"_q).toObject().value(u"code"_q).toString() == u"busy"_q,
		"cancelled queued main events retain bounded admission until consumed");
	QCoreApplication::processEvents();
	Check(actions == 0, "all disconnected queued actions are skipped");
	extra.write(Request(102, u"ping"_q));
	Check(Receive(extra).value(u"result"_q).toObject().value(u"pong"_q).toBool(),
		"fresh command succeeds after cancelled queue drains");
}

void SocketSafetyCases() {
	auto temporary = QTemporaryDir(u"/private/tmp/pipc-XXXXXX"_q);
	if (!temporary.isValid()) { Check(false, "safety temporary directory"); return; }
	const auto root = temporary.path();
	auto status = 0;
	auto published = QString();
	auto server = ControlServer([](const auto &, const auto &, auto reply) { reply({}); },
		[&](QString path, QString) { published = path; ++status; });
	const auto configure = [&](const QString &path) {
		const auto before = status;
		server.configure({ .configuredPath = path });
		return Wait([&] { return status >= before + 2; });
	};
	const auto filePath = root + u"/file"_q;
	auto file = QFile(filePath);
	Check(file.open(QIODevice::WriteOnly) && file.write("keep") == 4, "prepare unrelated file sentinel");
	file.close();
	Check(configure(filePath) && published.isEmpty() && QFileInfo(filePath).isFile(), "non-socket file preserved");
	const auto link = root + u"/link"_q;
	Check(QFile::link(filePath, link), "prepare socket-path symlink");
	Check(configure(link) && published.isEmpty() && QFileInfo(link).isSymLink(), "symlink and target preserved");
	const auto publicParent = root + u"/public"_q;
	Check(QDir().mkdir(publicParent) && chmod(publicParent.toUtf8().constData(), 0755) == 0, "prepare non-private parent");
	Check(configure(publicParent + u"/s"_q) && published.isEmpty(), "existing non-private parent refused");
	struct stat info = {};
	Check(lstat(publicParent.toUtf8().constData(), &info) == 0 && (info.st_mode & 0777) == 0755,
		"arbitrary parent permissions never changed");
	const auto stale = root + u"/stale"_q;
	auto address = sockaddr_un();
	address.sun_family = AF_UNIX;
	const auto bytes = stale.toUtf8();
	std::memcpy(address.sun_path, bytes.constData(), bytes.size() + 1);
	const auto fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	Check(fd >= 0 && ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "prepare owned stale socket");
	if (fd >= 0) ::close(fd);
	Check(configure(stale) && published == stale, "owned non-answering stale socket replaced");
	auto otherReady = false;
	auto other = ControlServer([](const auto &, const auto &, auto reply) { reply({}); },
		[&](QString, QString error) { if (!error.isEmpty()) otherReady = true; });
	other.configure({ .configuredPath = stale });
	Check(Wait([&] { return otherReady; }) && QFileInfo(stale).exists(), "live listening socket never replaced");
	Check(configure(root + u"/fresh"_q) && !QFileInfo::exists(stale), "normal reconfigure removes only owned old socket");
}

void ProtocolBoundCases() {
	auto temporary = QTemporaryDir(u"/private/tmp/pipc-XXXXXX"_q);
	if (!temporary.isValid()) { Check(false, "protocol bound temporary directory"); return; }
	const auto path = temporary.path() + u"/s"_q;
	auto ready = false;
	auto pending = std::vector<ControlReply>();
	auto server = ControlServer([&](const auto &request, const auto &policy, auto reply) {
		if (request.command == u"window.activate"_q) pending.push_back(reply);
		else DispatchControlRequest(request, policy, u"synthetic"_q, {}, reply);
	}, [&](QString value, QString) { ready = value == path; });
	auto policy = ControlPolicy();
	policy.enabled = true;
	server.setPolicy(policy);
	server.configure({ .configuredPath = path });
	Check(Wait([&] { return ready; }), "protocol bound listener ready");
	const auto connect = [&] {
		auto socket = std::make_unique<QLocalSocket>();
		socket->connectToServer(path);
		Check(Wait([&] { return socket->state() == QLocalSocket::ConnectedState; }), "bounded client connects");
		return socket;
	};
	auto socket = connect();
	auto exact = Request(1, u"ping"_q).chopped(1);
	exact += QByteArray(kControlLineBytes - exact.size(), ' ');
	socket->write(exact + '\n');
	Check(Receive(*socket).value(u"ok"_q).toBool(), "actual socket accepts exact64KiB frame");
	socket->write(QByteArray(1, char(0xff)) + '\n');
	Check(Receive(*socket).value(u"error"_q).toObject().value(u"code"_q).toString() == u"bad-request"_q,
		"actual malformed UTF8 never dispatches");
	auto frames = QByteArray();
	for (auto id = 10; id < 19; ++id) frames += Request(id, u"window.activate"_q);
	socket->write(frames);
	Check(Receive(*socket).value(u"error"_q).toObject().value(u"code"_q).toString() == u"busy"_q
		&& Wait([&] { return pending.size() == 8; }), "ninth per-client pending command is refused");
	Check(pending.size() == 8, "pending handler callbacks remain capped at eight");
	socket->abort();
	socket = connect();
	socket->write(QByteArray(kControlLineBytes + 1, 'x'));
	Check(Wait([&] { return socket->state() == QLocalSocket::UnconnectedState; }), "oversized unterminated frame disconnects");
	socket = connect();
	frames.clear();
	for (auto id = 0; id < 21; ++id) frames += Request(id, u"ping"_q);
	socket->write(frames);
	Check(Wait([&] { return socket->state() == QLocalSocket::UnconnectedState; }), "twenty-first burst request disconnects");
	auto clients = std::vector<std::unique_ptr<QLocalSocket>>();
	for (auto i = 0; i < 16; ++i) clients.push_back(connect());
	auto overflow = std::make_unique<QLocalSocket>();
	overflow->connectToServer(path);
	(void)overflow->waitForConnected(1000);
	overflow->write(Request(1, u"hello"_q));
	Check(Wait([&] { return overflow->state() == QLocalSocket::UnconnectedState; }), "seventeenth active client refused");
	clients.clear();
	QThread::msleep(100);
	socket = connect();
	socket->write(Request(1, u"ping"_q));
	Check(Receive(*socket).value(u"ok"_q).toBool(), "connection and rate limits release for fresh clients");
	const auto huge = EncodeControlReply(1, { {{u"large"_q, QString(kControlLineBytes, 'x')}} });
	Check(huge.size() <= kControlLineBytes + 1
		&& QJsonDocument::fromJson(huge).object().value(u"error"_q).toObject().value(u"code"_q).toString() == u"busy"_q,
		"oversized result becomes bounded busy response");
}
}

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	PolicyCases();
	RawIntegerCases();
	IntegerWireCases();
	SocketCases();
	QueueBoundCases();
	SocketSafetyCases();
	ProtocolBoundCases();
	std::printf("purple control: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
