#include "purple/purple_emoji_large.h"
#include "purple/purple_emoji_export_queue.h"
#include "purple/purple_emoji_export_store.h"
#include <QtCore/QString>
#include <cstdio>
#include <functional>
#include <variant>

namespace {

auto checks = 0;
auto failures = 0;
void Check(bool value, const char *name) {
	++checks;
	if (!value) { ++failures; std::printf("FAIL: %s\n", name); }
}

struct WireInteger { quint64 v = 0; };
WireInteger MTP_long(quint64 value) { return {value}; }
WireInteger MTP_int(int value) { return {quint64(value)}; }
struct StickerSetInput { quint64 id = 0; quint64 accessHash = 0; };
StickerSetInput MTP_inputStickerSetID(WireInteger id, WireInteger hash) {
	return {id.v, hash.v};
}
struct SetRequest { StickerSetInput set; WireInteger hash; };
SetRequest MTPmessages_GetStickerSet(StickerSetInput set, WireInteger hash) {
	return {set, hash};
}
QString qs(const QString &value) { return value; }
struct SetMetadata {
	WireInteger id;
	QString title;
	QString shortName;
	WireInteger vid() const { return id; }
	const QString &vtitle() const { return title; }
	const QString &vshort_name() const { return shortName; }
};
struct SetValue {
	SetMetadata value;
	const SetMetadata &data() const { return value; }
};
struct MTPDmessages_stickerSet {
	SetValue set;
	const SetValue &vset() const { return set; }
};
struct MTPDmessages_stickerSetNotModified {};
struct MTPmessages_StickerSet {
	std::variant<MTPDmessages_stickerSet, MTPDmessages_stickerSetNotModified> value;
	template <typename Full, typename Empty> void match(Full full, Empty empty) const {
		if (const auto data = std::get_if<MTPDmessages_stickerSet>(&value)) full(*data);
		else empty(std::get<MTPDmessages_stickerSetNotModified>(value));
	}
};
namespace MTP { struct Error {}; }
struct Wire {
	SetRequest value;
	std::function<void(const MTPmessages_StickerSet&)> done;
	std::function<void(const MTP::Error&)> fail;
};
class FakeApi {
public:
	struct Builder {
		FakeApi *owner = nullptr;
		Wire wire;
		template <typename F> Builder &done(F callback) {
			wire.done = callback; return *this;
		}
		template <typename F> Builder &fail(F callback) {
			wire.fail = callback; return *this;
		}
		Builder &handleAllErrors() { return *this; }
		int send() {
			owner->sent.push_back(std::move(wire)); return owner->sent.size();
		}
	};
	Builder request(SetRequest value) { return {this, {value}}; }
	std::vector<Wire> sent;

};
struct Sticker { StickerSetInput set; };
struct DocumentData {
	quint64 id = 0;
	Sticker value;
	Sticker *sticker() { return &value; }
};

struct QTimer {
	template <typename Context, typename F>
	static void singleShot(int, Context*, F) {}
};

struct EmojiExporter {
	Purple::EmojiSetTitleCache _setTitles;
	Purple::EmojiExportQueue _queue;
	std::vector<Purple::EmojiExportEntry> _entries;
	std::vector<std::optional<Purple::EmojiExportEntry>> _ready;
	Purple::EmojiExportEpoch _epoch;
	quint64 _token = _epoch.begin();
	size_t _index = 0;
	struct Clock { qint64 value = 0; qint64 elapsed() const { return value; } } _clock;
	struct Timer { int delay = 0; void start(int value) { delay = value; } } _continuation;
	FakeApi api;
	FakeApi *_api = &api;
	int _request = 0;
	Purple::EmojiLargeRequestQueue _largeRequests;
	std::set<quint64> _demandIds;
	std::set<quint64> _retiredDemandIds;
	size_t _baseCount = 3;
	size_t _demandReplace = 0;
	std::set<quint64> _acknowledge;
	std::set<quint64> _afterPublicationAck;
	std::optional<quint64> _demandId;
	std::optional<quint64> _deferredDemand;
	bool _demandRendered = false;
	bool _running = false;
	bool _publishPending = false;
	std::shared_ptr<int> _media;
	void rememberTransfer() {}
	void next();
	void chooseDemand();
	void holdDemandForMetadataWait();
	int resolutionAdmitted = 0;
	void resolveAdmission() {
		using Purple::EmojiResolveAdmission;
#include "emoji-large-resolve-admission.inc"
		++resolutionAdmitted;
	}
	void admitRequests(const std::vector<quint64> &ids);
	int pumps = 0;
	int reconciles = 0;
	bool current = true;
	EmojiExporter() {
		_entries = {{.id = 1, .setId = 10, .installed = false},
			{.id = 2, .setId = 10, .installed = false}, {.id = 3, .setId = 20}};
		_ready = {_entries[0], _entries[1], _entries[2]};
		_queue.begin({1, 2, 3}, {});
	}
	bool admitted() const { return current; }
	void reconcile() { ++reconciles; }
	void pump() { ++pumps; }
	bool resolveSetTitle(DocumentData *document);
	void applySetTitle(quint64 id);
};

using Purple::EmojiResolveAdmission;
using Purple::EmojiLargeAdmission;
using Purple::EmojiExportEntry;
#include "emoji-large-metadata-handler.inc"

MTPmessages_StickerSet Reply(quint64 id, const QString &title = QString("Resolved")) {
	return {MTPDmessages_stickerSet{{{{id}, title, QString("resolved")}}}};
}

}

int main() {
	{
		auto e = EmojiExporter();
		auto ids = std::vector<quint64>();
		for (auto id = quint64(1); id <= 100; ++id) ids.push_back(id);
		e.admitRequests(ids);
		Check(e._largeRequests.size() == 64, "literal file admission caps pending requests");
		e.chooseDemand();
		Check(e._demandId == 1 && e._running && e._queue.current() == 0,
			"demand selects exact existing entry without rebuilding pass");
		e._index = *e._queue.current(); e.next();
		Check(e._acknowledge.contains(1) && !e._afterPublicationAck.contains(1),
			"unresolvable demand drops with acknowledgement and cooldown");
		e.admitRequests({1});
		Check(e._largeRequests.size() == 63, "acknowledgement-pending duplicate does not requeue");
		e._clock.value = 250; e.chooseDemand();
		Check(e._demandId == 2 && e._queue.current() == 1, "later demand progresses past failed prefix");
		e._index = *e._queue.current(); e._demandRendered = true; e.next();
		Check(e._afterPublicationAck.contains(2) && !e._acknowledge.contains(2)
			&& e._publishPending, "rendered demand awaits catalog publication before acknowledgement");
		e.admitRequests({2});
		Check(e._largeRequests.size() == 62, "publication-pending duplicate does not requeue");
	}
	{
		auto e = EmojiExporter(); e.admitRequests({1000}); e.chooseDemand();
		Check(e._demandId == 1000 && e._entries.back().id == 1000 && e._ready.size() == 4
			&& e._demandIds.contains(1000), "unknown chooser id adds one bounded normal entry");
		e._deferredDemand = e._demandId; e._demandId.reset();
		e._queue.transient(110, 100, 0);
		e.chooseDemand();
		Check(!e._demandId && e._deferredDemand == 1000,
			"deferred chooser demand cannot rearm before global deadline");
		e._clock.value = 10000; e.chooseDemand();
		Check(e._demandId == 1000 && !e._deferredDemand,
			"retained chooser demand resumes without external trigger");
	}
	{
		auto e = EmojiExporter();
		for (auto id = quint64(100); id < 1124; ++id) e._demandIds.insert(id);
		e.admitRequests({5000}); e.chooseDemand();
		Check(e._entries.size() == 4 && e._demandId == 5000 && e._demandIds.size() == 1024,
			"bounded memory still admits later unknown ids");
	}

	{
		auto e = EmojiExporter();
		for (auto id = quint64(100); id < 1124; ++id) {
			e._entries.push_back({.id = id}); e._ready.emplace_back(); e._demandIds.insert(id);
		}
		e.admitRequests({5000}); e.chooseDemand();
		Check(e._entries.size() == 1027 && e._entries[3].id == 5000
			&& e._retiredDemandIds.contains(100) && e._publishPending,
			"rolling demand slot retires old metadata without enlarging the candidate bound");
	}
	{
		auto e = EmojiExporter(); e._queue.transient(110, 100, 0);
		e.admitRequests({5000}); e.chooseDemand(); e._index = *e._queue.current();
		e.resolveAdmission();
		Check(e._deferredDemand == 5000 && !e._demandId && e._largeRequests.size() == 1
			&& e._acknowledge.empty() && !e.resolutionAdmitted,
			"new chooser demand during an existing global wait is retained instead of dropped");
		e._clock.value = 10000; e.chooseDemand(); e._index = *e._queue.current(); e.resolveAdmission();
		Check(e._demandId == 5000 && !e._deferredDemand && e.resolutionAdmitted == 1,
			"newly deferred demand resumes at the shared deadline without outside input");
	}
	auto document = DocumentData{1, {{10, 100}}};
	{
		auto e = EmojiExporter();
		Check(e.resolveSetTitle(&document) && e.api.sent.size() == 1, "unknown set read once");
		Check(e.api.sent[0].value.set.id == 10 && e.api.sent[0].value.set.accessHash == 100
			&& e.api.sent[0].value.hash.v == 0, "exact read-only input identity");
		Check(!e.resolveSetTitle(&document) && e.api.sent.size() == 1, "duplicate shares pending request");
		auto other = DocumentData{3, {{20, 200}}};
		Check(!e.resolveSetTitle(&other) && e.api.sent.size() == 1, "single set request in flight");
		e.api.sent[0].done(Reply(10));
		Check(!e._request && e.pumps == 1, "completion resumes current item");
		Check(e._entries[0].setTitle == QString("Resolved")
			&& e._entries[1].setShortName == QString("resolved")
			&& e._ready[1]->setTitle == QString("Resolved")
			&& e._entries[2].setTitle.isEmpty(), "all matching entries and ready records enriched");
		e._index = 1;
		auto same = DocumentData{2, {{10, 100}}};
		Check(!e.resolveSetTitle(&same) && e.api.sent.size() == 1, "another entry in cached set makes no request");
	}
	{
		auto e = EmojiExporter(); e._entries[0].installed = true;
		Check(!e.resolveSetTitle(&document) && e.api.sent.empty(), "installed set makes no request");
	}
	{
		auto e = EmojiExporter();
		Check(e.resolveSetTitle(&document), "permanent-error request starts");
		e.api.sent[0].fail({});
		Check(e._entries[0].setTitle.isEmpty() && !e._request && e.pumps == 1,
			"permanent error leaves unknown title and resumes artwork");
		Check(!e.resolveSetTitle(&document) && e.api.sent.size() == 1,
			"permanent error has cooldown instead of immediate retry");
		e._clock.value = 60000;
		Check(e.resolveSetTitle(&document) && e.api.sent.size() == 2,
			"bounded cooldown later permits one retry");
	}
	for (const auto &response : {Reply(99), Reply(10, QString()),
		MTPmessages_StickerSet{MTPDmessages_stickerSetNotModified{}}}) {
		auto e = EmojiExporter(); e.resolveSetTitle(&document);
		e.api.sent[0].done(response);
		Check(e._entries[0].setTitle.isEmpty() && !e._setTitles.active(),
			"wrong, empty or not-modified response is not invented metadata");
	}
	{
		auto e = EmojiExporter(); e.resolveSetTitle(&document);
		e._setTitles.known(10, {QString("New local"), QString("new")});
		e.api.sent[0].done(Reply(10));
		Check(e._entries[0].setTitle == QString("New local"), "new local title wins stale server metadata");
	}
	{
		auto e = EmojiExporter(); e.resolveSetTitle(&document);
		e._epoch.revoke(); e._request = 999;
		e.api.sent[0].done(Reply(10)); e.api.sent[0].fail({});
		Check(e._request == 999 && e._entries[0].setTitle.isEmpty() && !e.pumps,
			"retired epoch cannot clear or mutate a newer request");
	}
	{
		auto e = EmojiExporter(); e.resolveSetTitle(&document);
		e._index = 1; e._request = 999;
		e.api.sent[0].done(Reply(10));
		Check(e._request == 999 && e._entries[0].setTitle.isEmpty(),
			"superseded exact message identity rejects old completion");
	}
	{
		auto e = EmojiExporter(); e._queue.transient(110, 100, 0);
		Check(!e.resolveSetTitle(&document) && e.api.sent.empty(),
			"shared transient deadline blocks set request while cached artwork may continue");
		e._clock.value = 10000;
		Check(e.resolveSetTitle(&document), "set request resumes at shared deadline");
	}
	std::printf("emoji literal set metadata: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
