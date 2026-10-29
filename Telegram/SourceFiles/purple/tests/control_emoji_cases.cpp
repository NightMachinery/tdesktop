#include "purple/purple_control_commands.h"
#include "purple/purple_control_server.h"
#include "purple/purple_emoji_export_worker.h"
#include "purple/purple_emoji_export_queue.h"
#include "purple/purple_emoji_large.h"
#include "purple/purple_emoji_large_store.h"
#include "purple/purple_emoji_render.h"
#include "base/basic_types.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtNetwork/QLocalSocket>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <limits>

namespace {
using namespace Purple;
auto checks = 0;
auto failures = 0;
void Check(bool value, const char *name) {
	++checks;
	if (!value) { ++failures; std::printf("FAIL: %s\n", name); }
}
bool Wait(const std::function<bool()> &condition, int limit = 4000) {
	auto clock = QElapsedTimer();
	clock.start();
	while (!condition() && clock.elapsed() < limit) {
		QCoreApplication::processEvents();
		QThread::msleep(1);
	}
	return condition();
}
QByteArray Request(qint64 id, QByteArray args = "{\"id\":1}") {
	return "{\"v\":1,\"id\":" + QByteArray::number(id)
		+ ",\"cmd\":\"emoji.render\",\"args\":" + args + "}\n";
}
QJsonObject Receive(QLocalSocket &socket) {
	if (!Wait([&] { return socket.canReadLine(); })) return {};
	return QJsonDocument::fromJson(socket.readLine()).object();
}
QString Error(const QJsonObject &reply) {
	return reply.value(u"error"_q).toObject().value(u"code"_q).toString();
}
QString CurrentControlSocketPath() { return u"synthetic-control.sock"_q; }
qint64 EmojiLargeCacheCap() { return kEmojiLargeDefaultBytes; }

class EmojiExporter final : public QObject {
public:
	explicit EmojiExporter(QString directory);
	void render(quint64 id, int pixels, std::function<bool()> active, ControlReply reply);
	bool serveRender();
	void passcodeChanged(bool locked);
	void completeOrdinary(quint64 id, bool wantLarge, bool ok);
	void next() {}
	void outputFailed() { enabled = false; }
	EmojiExportQueue _queue{1};
	std::vector<std::optional<EmojiExportEntry>> _ready{1};
	std::optional<quint64> _demandId;
	bool _demandRendered = false;
	bool _running = false;
	QTimer _debounce;
	static constexpr auto kDebounceMs = 5000;
	bool admitted() const { return enabled; }
	void reconcile() {}
	void start() { pump(); }
	void pump();

	QString _directory;
	EmojiExportWorker _worker;
	quint64 _workerGeneration = _worker.generation();
	EmojiExportEpoch _epoch;
	quint64 _token = _epoch.begin();
	EmojiLargeRequestQueue _largeRequests;
	EmojiRenderRequests _renderRequests;
	QElapsedTimer _clock;
	QTimer timer;
	std::shared_ptr<const std::vector<EmojiExportEntry>> _published
		= std::make_shared<const std::vector<EmojiExportEntry>>();
	bool _rendering = false;
	bool _pending = false;
	bool enabled = true;
	bool paused = false;
	int rendered = 0;

};

EmojiExporter::EmojiExporter(QString directory)
: _directory(std::move(directory))
, _worker(_directory, this) {
	_clock.start();
	connect(&timer, &QTimer::timeout, this, [=] {
		_renderRequests.poll(_clock.elapsed());
		pump();
	});
	timer.start(20);
}

#include "control-emoji-frontend.inc"

void EmojiExporter::pump() {
	if (_rendering || paused || !enabled || serveRender()) return;
	const auto id = _largeRequests.take(_clock.elapsed());
	if (!id) return;
	const auto entry = std::make_shared<EmojiExportEntry>(EmojiExportEntry{
		.id = *id, .large = EmojiLargePngName(*id, 300),
		.sourceKind = u"webp"_q, .sourcePx = 300,
	});
	const auto published = std::make_shared<std::vector<EmojiExportEntry>>(*_published);
	const auto old = std::find_if(published->begin(), published->end(), [&](const auto &e) { return e.id == *id; });
	if (old != published->end()) *old = *entry;
	else published->push_back(*entry);
	_rendering = true;
	const auto accepted = _worker.submit(_workerGeneration, [=](const EmojiExportAuthority &current) {
		auto image = QImage(300, 150, QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::red);
		return WriteEmojiExportPng(_directory, *id, EmojiExportFrame(image, {}), current)
			&& WriteEmojiLargePng(_directory, *id, image, {}, current)
			&& PublishEmojiLargeRequests(_directory, *published, QDateTime::currentDateTimeUtc(), {}, current, CurrentControlSocketPath());
	}, [=](quint64 generation, bool ok) {
		if (generation != _workerGeneration) return;
		_rendering = false;
		++rendered;
		_largeRequests.complete(*id, ok, _clock.elapsed());
		if (ok) {
			_published = published;
			_renderRequests.published(*published);
		} else {
			_renderRequests.fail(*id, ControlError(u"unsupported"_q, u"Synthetic source failed."_q));
		}
		pump();
	});
	Check(accepted, "synthetic source uses the production worker queue");
}

void EmojiExporter::completeOrdinary(quint64 id, bool wantLarge, bool ok) {
	const auto token = _token;
	const auto index = size_t(0);
	const auto entry = EmojiExportEntry{ .id = id };
	const auto renderedEntry = std::make_shared<EmojiExportEntry>(EmojiExportEntry{
		.id = id, .sourceKind = u"webp"_q, .sourcePx = 300,
	});
	const auto storageFailed = std::make_shared<bool>(false);
	const auto done =
#include "control-emoji-completion.inc"
	;
	done(_workerGeneration, ok);
}

void OrdinaryPreviewRace(QCoreApplication &app) {
	auto temporary = QTemporaryDir(u"/private/tmp/pemoji-XXXXXX"_q);
	Check(temporary.isValid(), "preview race uses an owned socket directory");
	if (!temporary.isValid()) return;
	const auto directory = QFileInfo(temporary.path()).canonicalFilePath() + u"/emoji-export"_q;
	auto exporter = EmojiExporter(directory);
	exporter.paused = true;
	exporter._rendering = true;
	exporter._queue.begin({77}, {});
	const auto path = temporary.path() + u"/s"_q;
	auto ready = false;
	auto policy = ControlPolicy();
	policy.enabled = true;
	auto server = ControlServer([&](const auto &request, const auto &p, auto reply) {
		Check(QThread::currentThread() == app.thread(), "preview race admission runs on main");
		DispatchControlRequest(request, p, {}, {}, std::move(reply),
			[&](auto id, auto pixels, auto active, auto done) {
				exporter.render(id, pixels, std::move(active), std::move(done));
			});
	}, [&](QString value, QString) { ready = (value == path); });
	server.setPolicy(policy);
	server.configure({ .configuredPath = path });
	Check(Wait([&] { return ready; }), "preview race listener ready");
	auto socket = QLocalSocket();
	socket.connectToServer(path);
	Check(Wait([&] { return socket.state() == QLocalSocket::ConnectedState; }), "preview race connected");
	socket.write(Request(200, "{\"id\":77,\"px\":128}"));
	Check(Wait([&] { return exporter._renderRequests.size() == 1; })
		&& exporter._largeRequests.size() == 1 && exporter._rendering,
		"socket demand arrives after preview-only job starts and before completion");
	exporter.completeOrdinary(77, false, true);
	Check(exporter._renderRequests.size() == 1 && !socket.canReadLine(),
		"successful preview-only completion retains the admitted socket waiter");
	exporter.paused = false;
	exporter.pump();
	const auto reply = Receive(socket);
	const auto result = reply.value(u"result"_q).toObject();
	Check(reply.value(u"id"_q).toInteger() == 200 && reply.value(u"ok"_q).toBool()
		&& result.value(u"px"_q).toInt() == 128
		&& QFileInfo(result.value(u"path"_q).toString()).isFile(),
		"late demand receives eventual successful reply for an existing image");
	Check(Wait([&] { return exporter.rendered == 1 && !exporter._rendering; })
		&& exporter._renderRequests.size() == 0 && !socket.canReadLine(),
		"preview overlap yields exactly one reply and one shared demand render");
	for (const auto wantLarge : { false, true }) {
		auto replies = 0;
		exporter.paused = true;
		exporter.render(88, 128, {}, [&](auto result) {
			++replies;
			Check(result.code == u"unsupported"_q, "actual render failure retains typed unsupported");
		});
		exporter.completeOrdinary(88, wantLarge, false);
		Check(replies == 1 && exporter._renderRequests.size() == 0,
			"failed preview or large render retires its waiter once");
	}
	auto missingLargeReplies = 0;
	exporter.render(89, 128, {}, [&](auto result) {
		++missingLargeReplies;
		Check(result.code == u"unsupported"_q, "attempted large render without large output is unsupported");
	});
	exporter.completeOrdinary(89, true, true);
	Check(missingLargeReplies == 1 && exporter._renderRequests.size() == 0,
		"missing output from an actual large attempt retires the waiter once");
	server.stop();
}

void NumericCases() {
	const auto dispatch = [&](QByteArray args) {
		const auto parsed = ParseControlRequest(Request(9, args));
		auto result = ControlResult();
		auto policy = ControlPolicy();
		policy.enabled = true;
		if (parsed.request) DispatchControlRequest(*parsed.request, policy, {}, {},
			[&](auto value) { result = std::move(value); },
			[&](quint64 id, int pixels, auto, auto reply) {
				reply({ QJsonObject{ { u"id"_q, QString::number(id) }, { u"px"_q, pixels } } });
			});
		else result = parsed.error;
		return result;
	};
	for (const auto &id : { QByteArray("1"), QByteArray("18446744073709551615"),
		QByteArray("9007199254740993"), QByteArray("184467440737095516150e-1"),
		QByteArray("\"18446744073709551615\"") }) {
		const auto result = dispatch("{\"id\":" + id + "}");
		Check(result.code.isEmpty() && result.value.value(u"px"_q).toInt() == 512,
			"exact nonzero uint64 artwork id and default pixels");
	}
	Check(dispatch("{\"id\":18446744073709551615}").value.value(u"id"_q) == u"18446744073709551615"_q,
		"maximum unsigned id reaches adapter with every digit");
	for (const auto &id : { QByteArray("0"), QByteArray("-1"), QByteArray("18446744073709551616"),
		QByteArray("-18446744073709551616"), QByteArray("9007199254740992.1"),
		QByteArray("1.00000000000000001"), QByteArray("1e-1"), QByteArray("true"),
		QByteArray("\"01\""), QByteArray("\"+1\""), QByteArray("\"1e2\""), QByteArray("\" 1\""),
		QByteArray("\"0\""), QByteArray("null") }) {
		Check(dispatch("{\"id\":" + id + "}").code == u"bad-request"_q,
			"unsigned overflow fractional or noncanonical id cannot dispatch");
	}
	for (const auto &px : { QByteArray("0"), QByteArray("513"), QByteArray("-1"),
		QByteArray("1.00000000000000001"), QByteArray("18446744073709551616"), QByteArray("\"128\"") }) {
		Check(dispatch("{\"id\":1,\"px\":" + px + "}").code == u"bad-request"_q,
			"pixel argument uses exact integer token and Large bounds");
	}
	for (const auto &px : { QByteArray("1"), QByteArray("512"), QByteArray("128.0"), QByteArray("1280e-1") }) {
		Check(dispatch("{\"id\":1,\"px\":" + px + "}").code.isEmpty(), "inclusive pixel bounds and exact integral spellings");
	}
}

void BrokerCases() {
	auto broker = EmojiRenderRequests();
	auto replies = 0;
	for (auto i = 0; i != 64; ++i) broker.add(i + 1, 512, 100, {}, [&](auto) { ++replies; });
	Check(broker.size() == 64 && broker.full(), "waiter index is capped at64");
	broker.add(65, 512, 100, {}, [&](auto r) { Check(r.code == u"busy"_q, "65th waiter gets busy"); });
	broker.poll(8099);
	Check(!replies, "timeout never fires early");
	broker.poll(8100);
	Check(replies == 64 && broker.size() == 0, "every waiter gets typed bounded timeout and releases its slot");
	for (const auto code : { u"not-found"_q, u"unsupported"_q, u"export-disabled"_q }) {
		broker.add(1, 128, 0, {}, [&](auto r) { Check(r.code == code, "typed artwork or export failure"); });
		broker.fail(1, ControlError(code, code));
	}
	auto active = true;
	broker.add(1, 128, 0, [&] { return active; }, [&](auto) { Check(false, "cancelled waiter never replies"); });
	active = false;
	broker.poll(0);
	Check(broker.size() == 0, "disconnect releases waiter without a reply");
	auto entry = EmojiExportEntry{ .id = 1, .large = EmojiLargePngName(1, 300), .sourceKind = u"webp"_q, .sourcePx = 300 };
	broker.add(1, 128, 0, {}, [&](auto) { ++replies; });
	broker.published({entry});
	const auto old = broker.take();
	Check(old.has_value() && !broker.take(), "only one worker completion owns each serial");
	broker.clear(ControlError(u"export-disabled"_q, u"revoked"_q));
	broker.add(1, 512, 0, {}, [&](auto r) { Check(r.code == u"timeout"_q, "new waiter retains its own deadline"); });
	broker.complete(old ? old->serial : 0, 1, {});
	Check(broker.size() == 1, "stale completion cannot retire a fresh request for the same artwork");
	broker.poll(8000);
	broker.add(1, 128, 0, {}, [&](auto) {
		broker.add(2, 128, 1, {}, [&](auto) {});
	});
	broker.clear(ControlError(u"export-disabled"_q, u"revoked"_q));
	Check(broker.size() == 1, "retirement callback can rearm without old clear erasing it");
	broker.clear({});
}

void ExporterBounds() {
	auto temporary = QTemporaryDir(u"/private/tmp/pemoji-XXXXXX"_q);
	auto e = EmojiExporter(QFileInfo(temporary.path()).canonicalFilePath() + u"/emoji-export"_q);
	e.paused = true;
	for (auto id = quint64(1); id != 65; ++id) {
		Check(e._largeRequests.offer(id, 0) == EmojiLargeAdmission::Queued, "fill the actual shared demand queue");
	}
	auto result = ControlResult();
	e.render(65, 512, {}, [&](auto value) { result = value; });
	Check(result.code == u"busy"_q && e._renderRequests.size() == 0,
		"full file-demand queue refuses a socket waiter without retaining it");
	e._largeRequests.clear();
	for (auto id = quint64(1); id != 65; ++id) e.render(id, 512, {}, [](auto) {});
	e.render(1, 128, {}, [&](auto value) { result = value; });
	Check(result.code == u"busy"_q && e._renderRequests.size() == 64,
		"exporter independently enforces64 socket waiters even for a duplicate artwork");
	e._renderRequests.clear({});
}

void StoreCases(QCoreApplication &app) {
	auto temporary = QTemporaryDir(u"/private/tmp/pemoji-XXXXXX"_q);
	Check(temporary.isValid(), "store fixture private directory");
	if (!temporary.isValid()) return;
	const auto directory = QFileInfo(temporary.path()).canonicalFilePath() + u"/emoji-export"_q;
	auto worker = EmojiExportWorker(directory, &app);
	auto done = false;
	auto result = false;
	Check(worker.submit(worker.generation(), [&](const EmojiExportAuthority &current) {
		Check(QThread::currentThread() != app.thread(), "render publication and decoding run off main");
		auto image = QImage(500, 100, QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::red);
		const auto source = EmojiExportEntry{ .id = 42, .large = EmojiLargePngName(42, 500),
			.sourceKind = u"webm"_q, .sourcePx = 500 };
		const auto input = directory + '/' + source.large;
		if (!WriteEmojiExportPng(directory, 42, EmojiExportFrame(image, {}), current)
			|| !WriteEmojiLargePng(directory, 42, image, {}, current)) return false;
		for (const auto px : { 1, 100, 512 }) {
			const auto rendered = PublishEmojiRender(directory, {source}, { 1, 42, px, source },
				kEmojiLargeDefaultBytes, current, CurrentControlSocketPath());
			const auto path = rendered.value.value(u"path"_q).toString();
			auto catalog = QFile(directory + u"/catalog.json"_q);
			if (!catalog.open(QIODevice::ReadOnly)) return false;
			const auto entries = QJsonDocument::fromJson(catalog.readAll()).object().value(u"entries"_q).toArray();
			const auto output = QImage(path);
			Check(rendered.code.isEmpty() && !entries.isEmpty()
				&& path == directory + '/' + entries[0].toObject().value(u"large"_q).toString()
				&& QFileInfo(path).isFile() && QFileInfo(path).size() > 0,
				"each size reply accompanies its advertised existing nonempty image");
			Check(output.width() == std::min(px, 500) && output.height() >= 1
				&& rendered.value.value(u"px"_q).toInt() == std::min(px, 500),
				"inclusive minimum, requested downscale and no native upscale");
		}
		Check(PublishEmojiRender(directory, {source}, { 1, 42, 128, source },
			kEmojiLargeDefaultBytes, [] { return false; }, {}).code == u"not-found"_q,
			"retired worker authority never publishes a successful reply");
		if (!QFile::remove(input)) return false;
		Check(PublishEmojiRender(directory, {source}, { 1, 42, 128, source },
			kEmojiLargeDefaultBytes, current, {}).code == u"not-found"_q,
			"evicted full-size source produces typed not-found rather than upscaling a variant");
		auto corrupt = QFile(input);
		if (!corrupt.open(QIODevice::WriteOnly) || corrupt.write("invalid image") != 13) return false;
		corrupt.close();
		Check(PublishEmojiRender(directory, {source}, { 1, 42, 128, source },
			kEmojiLargeDefaultBytes, current, {}).code == u"not-found"_q,
			"corrupt owned image cannot become a successful reply");
		if (!corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate)
			|| !corrupt.resize(4 * 1024 * 1024 + 1)) return false;
		corrupt.close();
		Check(PublishEmojiRender(directory, {source}, { 1, 42, 128, source },
			kEmojiLargeDefaultBytes, current, {}).code == u"not-found"_q,
			"owned input byte cap is enforced before decode");
		return true;
	}, [&](quint64, bool ok) { result = ok; done = true; }), "store qualification submitted");
	Check(Wait([&] { return done; }) && result, "store qualification completes on production worker");
}

void SocketCases(QCoreApplication &app) {
	auto temporary = QTemporaryDir(u"/private/tmp/pemoji-XXXXXX"_q);
	Check(temporary.isValid(), "owned short socket directory");
	if (!temporary.isValid()) return;
	const auto directory = QFileInfo(temporary.path()).canonicalFilePath() + u"/emoji-export"_q;
	auto exporter = EmojiExporter(directory);
	const auto path = temporary.path() + u"/s"_q;
	auto ready = false;
	auto policy = ControlPolicy();
	policy.enabled = true;
	auto server = ControlServer([&](const auto &request, const auto &p, auto reply) {
		Check(QThread::currentThread() == app.thread(), "socket emoji handler runs on main owner thread");
		DispatchControlRequest(request, p, {}, {}, std::move(reply),
			[&](auto id, auto pixels, auto active, auto done) { exporter.render(id, pixels, std::move(active), std::move(done)); });
	}, [&](QString value, QString) { ready = (value == path); });
	server.setPolicy(policy);
	server.configure({ .configuredPath = path });
	Check(Wait([&] { return ready; }), "emoji listener ready");
	auto socket = QLocalSocket();
	socket.connectToServer(path);
	Check(Wait([&] { return socket.state() == QLocalSocket::ConnectedState; }), "emoji socket connected");
	exporter.paused = true;
	socket.write(Request(100, "{\"id\":18446744073709551615,\"px\":128}")
		+ Request(101, "{\"id\":18446744073709551615,\"px\":512}"));
	Check(Wait([&] { return exporter._renderRequests.size() == 2; }), "two async renders coexist on one connection");
	socket.write(Request(100));
	Check(Error(Receive(socket)) == u"busy"_q && exporter._renderRequests.size() == 2,
		"duplicate correlation id refuses without replacing pending render");
	exporter.paused = false;
	for (auto i = 0; i != 2; ++i) {
		const auto reply = Receive(socket);
		const auto result = reply.value(u"result"_q).toObject();
		const auto pixels = i ? 300 : 128;
		Check(reply.value(u"id"_q).toInteger() == 100 + i && reply.value(u"ok"_q).toBool(),
			"async replies keep their original correlation ids");
		Check(result.value(u"px"_q).toInt() == pixels && result.value(u"sourceKind"_q) == u"webp"_q
			&& QFileInfo(result.value(u"path"_q).toString()).isFile(), "requested bound and native no-upscale result refer to existing file");
		const auto image = QImage(result.value(u"path"_q).toString());
		Check(image.width() == pixels && image.height() == pixels / 2, "aspect ratio survives downscale and native-size reply");
	}
	Check(exporter.rendered == 1 && exporter._renderRequests.size() == 0,
		"same-artwork concurrent requests coalesce into the shared demand renderer");
	auto inspected = false;
	auto advertised = false;
	Check(Wait([&] { return !exporter._rendering; }), "publication worker is idle before catalog probe");
	Check(exporter._worker.submit(exporter._workerGeneration, [&](const EmojiExportAuthority &) {
		auto file = QFile(directory + u"/catalog.json"_q);
		if (!file.open(QIODevice::ReadOnly)) return false;
		const auto catalog = QJsonDocument::fromJson(file.readAll()).object();
		const auto entries = catalog.value(u"entries"_q).toArray();
		if (entries.isEmpty()) return false;
		const auto entry = entries[0].toObject();
		advertised = catalog.value(u"controlSocket"_q).toString() == CurrentControlSocketPath()
			&& entry.value(u"large"_q).toString() == EmojiLargePngName(std::numeric_limits<quint64>::max(), 300)
			&& QFileInfo(directory + '/' + entry.value(u"large"_q).toString()).isFile();
		return advertised;
	}, [&](quint64, bool ok) { advertised = advertised && ok; inspected = true; }), "catalog probe uses production disk worker");
	Check(Wait([&] { return inspected; }) && advertised, "render reply accompanies advertised existing image and cached control socket");
	exporter.enabled = false;
	socket.write(Request(102));
	Check(Error(Receive(socket)) == u"export-disabled"_q, "export off is a typed refusal");
	exporter.enabled = true;
	exporter.paused = true;
	for (const auto code : { u"not-found"_q, u"unsupported"_q, u"timeout"_q }) {
		socket.write(Request(107, "{\"id\":2}"));
		Check(Wait([&] { return exporter._renderRequests.size() == 1; }), "typed asynchronous failure is pending");
		if (code == u"timeout"_q) exporter._renderRequests.poll(exporter._clock.elapsed() + kEmojiRenderTimeoutMs);
		else exporter._renderRequests.fail(2, ControlError(code, code));
		const auto reply = Receive(socket);
		Check(Error(reply) == code && reply.value(u"id"_q).toInteger() == 107,
			"unknown unsupported and bounded-timeout replies retain correlation id");
	}
	socket.write(Request(103));
	Check(Wait([&] { return exporter._renderRequests.size() == 1; }), "pending render before group revocation");
	policy.groups[4] = false;
	server.setPolicy(policy);
	exporter._renderRequests.fail(1, {});
	Check(Error(Receive(socket)) == u"capability-off"_q, "late successful render is reauthorized after group off");
	policy.groups[4] = true;
	server.setPolicy(policy);
	socket.write(Request(108));
	Check(Wait([&] { return exporter._renderRequests.size() == 1; }), "pending render before lock notification precedes policy refresh");
	exporter.passcodeChanged(true);
	const auto lockedReply = Receive(socket);
	Check(Error(lockedReply) == u"locked"_q && lockedReply.value(u"id"_q).toInteger() == 108,
		"exporter lock notification sends locked before cached control policy changes");
	policy.locked = true;
	server.setPolicy(policy);
	socket.write(Request(104));
	Check(Error(Receive(socket)) == u"locked"_q, "locked emoji admission is refused");
	policy.locked = false;
	server.setPolicy(policy);
	socket.write(Request(105));
	Check(Wait([&] { return exporter._renderRequests.size() == 1; }), "pending render before disconnect");
	socket.abort();
	Check(Wait([&] { return exporter._renderRequests.size() == 0; }), "disconnect retires pending callback by active ticket");
	auto second = QLocalSocket();
	second.connectToServer(path);
	Check(Wait([&] { return second.state() == QLocalSocket::ConnectedState; }), "fresh connection after cancellation");
	second.write(Request(106));
	Check(Wait([&] { return exporter._renderRequests.size() == 1; }), "pending render before server stop");
	server.stop();
	Check(Wait([&] { return exporter._renderRequests.size() == 0; }), "server stop releases pending callback without dead reply");
	exporter._renderRequests.complete(1, 0, {});
	Check(exporter._renderRequests.size() == 0, "old worker completion after stop is inert");
}

}

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	OrdinaryPreviewRace(app);
	NumericCases();
	BrokerCases();
	ExporterBounds();
	StoreCases(app);
	SocketCases(app);
	std::printf("purple emoji control: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
