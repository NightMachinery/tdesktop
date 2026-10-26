#include "base/unixtime.h"
#include "base/flat_map.h"
#include "base/weak_ptr.h"
#include "purple/purple_archive_acquired_source.h"
#include "purple/purple_archive_capture.h"
#include "purple/purple_archive_journal.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_markdown_source.h"
#include "purple/purple_archive_sql_maintenance.h"
#include "media/streaming/media_streaming_loader.h"
#include "tests/archive_test_support.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <deque>
#include <atomic>
#include <set>
#include <map>
using namespace Purple;
using namespace ArchiveTest;
struct ProbeSession : base::has_weak_ptr {
	rpl::lifetime life;
	rpl::lifetime &lifetime() { return life; }
};
struct ProbeLoader {
	rpl::event_stream<Media::Streaming::LoadedPart> event;
	int64 size() const { return 3; }
	auto parts() const { return event.events(); }
};
struct ProbeReader : base::has_weak_ptr {
	std::unique_ptr<ProbeLoader> _loader;
	ProbeReader(std::unique_ptr<ProbeLoader> loader, void*) : _loader(std::move(loader)) {}
	bool isRemoteLoader() const { return true; }
};
struct ProbeData {
	uint64 id = 555;
	int created = 0;
	std::unique_ptr<ProbeLoader> createStreamingLoader(Data::FileOrigin, bool) { ++created; return std::make_unique<ProbeLoader>(); }
};
struct ProbeDocument {
	std::shared_ptr<ProbeReader> reader;
	ProbeDocument(not_null<ProbeData*>, std::shared_ptr<ProbeReader> value, std::vector<int>) : reader(std::move(value)) {}
	void setOtherQualities(std::vector<int>) {}
};
struct ProbeWorker {
	std::deque<std::function<void(DesktopArchiveStore&)>> tasks;
	bool Post(std::function<void(DesktopArchiveStore&)> task) { tasks.push_back(std::move(task)); return true; }
	void PruneStreamsSoon() { Post([](DesktopArchiveStore &store) { store.PruneStreams(); }); }
	void Run(DesktopArchiveStore &store) { while (!tasks.empty()) { auto task = std::move(tasks.front()); tasks.pop_front(); task(store); store.PruneStreams(); } }
};
struct ProbeValue { std::shared_ptr<ProbeWorker> worker; std::shared_ptr<DesktopArchiveJournal> journal; } Value;
std::set<ProbeSession*> Retired;
int Gaps = 0;
int Taps = 0;
AccountKey SessionAccountKey(not_null<ProbeSession*>) { return {0,123}; }
ProbeValue &For(not_null<ProbeSession*>) { return Value; }
void QueueArchiveRetentionGap(not_null<ProbeSession*>, QString) { ++Gaps; }
#include "archive-passive-registry.inc"
Registry StreamRegistry;
Registry &StreamRegistryFor(not_null<ProbeSession*>) { return StreamRegistry; }
std::optional<DesktopArchiveObservation> ArchiveAcquisitionObservation(not_null<ProbeSession*>, const ArchiveMessageKey &key, const QString &identity) {
	for (const auto &entry : StreamRegistry.observations) if (entry.key == key && entry.identity == identity) return entry;
	return {};
}
#include "archive-passive-sink.inc"
#include "archive-passive-bridge.inc"
#include "archive-passive-source.inc"
#include "archive-passive-document.inc"
namespace Purple {
void ObserveArchiveStream(ProbeSession *session, not_null<ProbeData*>, const Data::FileOrigin &origin, ProbeReader *reader) {
	++Taps; Stream(session, u"document:555"_q, origin, reader);
}
void ObserveArchiveStream(ProbeSession *session, not_null<ProbeData*>, const Data::FileOrigin &origin, ProbeDocument *document) {
	++Taps; ReusedDocument(session, u"document:555"_q, origin, document);
}
void BindArchiveStreamDocument(ProbeSession *session, const std::shared_ptr<ProbeDocument> &document, const std::shared_ptr<ProbeReader> &reader) {
	BindDocument(session, document, reader);
}
}
struct ProbeOwner { ProbeSession value; int cache = 0; ProbeSession &session() { return value; } int &cacheBigFile() { return cache; } };
class StreamingProbe {
public:
	using Reader = ProbeReader;
	using Document = ProbeDocument;
	ProbeOwner owner;
	ProbeOwner *_owner = &owner;
	template <typename Data> std::shared_ptr<Reader> sharedReader(base::flat_map<not_null<Data*>, std::weak_ptr<Reader>>&, not_null<Data*>, ::Data::FileOrigin, bool = false);
	template <typename Data> std::shared_ptr<Document> sharedDocument(base::flat_map<not_null<Data*>, std::weak_ptr<Document>>&, base::flat_map<not_null<Data*>, std::weak_ptr<Reader>>&, not_null<Data*>, DocumentData*, HistoryItem*, ::Data::FileOrigin);
};
template <typename Map, typename Data, typename Reader> bool PruneDestroyedAndSet(Map&, Data, Reader) { return false; }
template <typename T> std::vector<int> LookupOtherQualities(DocumentData*, T, HistoryItem*) { return {}; }
#include "archive-passive-reader.inc"
#include "archive-passive-shared-document.inc"
Data::FileOrigin Origin(int message) { return FullMsgId(peerFromChannel(ChannelId(45)), MsgId(message)); }
void Observe(DesktopArchiveStore &store, int message) {
	auto event = Event(Empty(), u"v"_q + QString::number(message), message, u"observed"_q, ArchiveOrigin::Baseline, Key({0,123},45,message));
	event.content->media.push_back({u"document:555"_q,u"document"_q});
	CHECK(store.Capture(event).retained);
	StreamRegistry.observations.push_back({event.key,event.id,ArchiveContentHash(*event.content),u"document:555"_q,u"document"_q});
}
bool Owned(DesktopArchiveStore &store, int message) {
	auto bytes = QByteArray();
	return store.ReadMedia(Key({0,123},45,message),u"v"_q+QString::number(message),u"document:555"_q,[&](const auto &part) { bytes += part; return true; }) && bytes == "abc";
}
void HeaderIoAdmissionControls() {
	Section = "header IO commit disclosure and bounded exact-scope admission renewal";
	for (const auto mode : {u"healthy"_q, u"transient"_q, u"purge-io"_q,
		u"chat-changed"_q, u"exhausted"_q, u"close"_q, u"discard"_q}) {
		auto temporary = QTemporaryDir();
		CHECK(DesktopArchiveDirectory(temporary.path() + u"/profile"_q));
		auto options = DesktopArchiveOptions{temporary.path() + u"/archives"_q,
			temporary.path() + u"/profile"_q, {0,123}, u"0"_q, QByteArray(256,'k'), true};
		options.loginAuthority = QByteArray(32,'a');
		options.loginAuthKeyId = options.loginAuthKeyIds = QByteArray(16,'1');
		auto store = DesktopArchiveStore();
		CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
		const auto opening = store.JournalAccess(); CHECK(opening && opening->active());
		if (!opening) continue;
		auto journal = DesktopArchiveJournal(*opening); CHECK(journal.Open());
		auto program = ArchiveSqlProgram{ArchiveSqlStatus::Ready, {
			{u"BEGIN IMMEDIATE"_q},
			{u"UPDATE archive_meta SET revision=revision WHERE singleton=1"_q, {}, ArchiveSqlExpectation::OneChange},
			{u"COMMIT"_q}}};
		if (mode == u"purge-io"_q) {
			const auto row = DesktopArchiveReadHeader(store.database(), {0,123}, store.security());
			CHECK(row);
			if (!row) continue;
			const auto state = std::get<ArchiveState>(row->row.value);
			program = BeginArchiveSqlPurge(state,
				{ArchivePurgeKind::EditHistory, {{0,123}, std::nullopt, {}}, state.revision},
				{}, u"header-io-purge"_q, store.security()).program;
		}
		if (mode != u"healthy"_q) store.database().FailForTest(u"prepare"_q, int(program.statements.size()));
		const auto executed = store.Execute(program);
		std::printf("HEADER_IO mode=%s executed=%d current=%d admission=%d captureAllowed=%d failure='%s'\n",
			mode.toUtf8().constData(), int(executed), int(store.Current()), int(opening->active()),
			int(journal.CaptureAllowed()), store.failure().toUtf8().constData());
		CHECK(executed && store.Current());
		if (mode == u"healthy"_q) {
			CHECK(opening->active() && store.failure().isEmpty() && !store.JournalRenewalPending());
		} else {
			CHECK(!opening->active() && store.JournalRenewalPending());
			CHECK(store.failure() == u"capture-admission-header-unreadable"_q);
			CHECK(!store.JournalAccess());
			if (mode == u"close"_q || mode == u"discard"_q) {
				CHECK(mode == u"close"_q ? store.Close() : store.Remove());
				CHECK(!store.RenewJournalAccess() && !store.JournalRenewalPending());
				CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
				const auto fresh = store.JournalAccess(); CHECK(fresh && fresh->active());
				CHECK(!opening->active());
			} else if (mode == u"exhausted"_q) {
				for (auto attempt = 0; attempt != 3; ++attempt) {
					store.database().FailForTest(u"prepare"_q, 2);
					CHECK(!store.RenewJournalAccess());
					CHECK(!store.CompleteJournalRenewal(false));
				}
				CHECK(store.JournalRenewalHeld() && !store.JournalRenewalPending());
				CHECK(store.failure() == u"capture-admission-renewal-exhausted"_q);
				CHECK(!store.RenewJournalAccess() && !opening->active());
			} else {
				if (mode == u"chat-changed"_q) {
					const auto preview = store.PreparePurge(ArchivePurgeKind::EditHistory,
						{{0,123}, ArchiveChatKey{ArchiveChatKind::Channel,45}, {}});
					CHECK(preview.confirmation && store.BeginPurge(*preview.confirmation));
					for (auto page = 0; page != 32 && store.MaintenancePending(); ++page) CHECK(store.ContinuePurge().valid);
					CHECK(!store.MaintenancePending());
				}
				CHECK(journal.RetireWriter());
				const auto renewed = store.RenewJournalAccess();
				if (mode == u"transient"_q) {
					CHECK(renewed && renewed->active());
					auto fresh = renewed ? std::make_unique<DesktopArchiveJournal>(*renewed) : nullptr;
					const auto opened = fresh && fresh->Open() && fresh->Drain(store);
					CHECK(opened && store.CompleteJournalRenewal(opened));
					CHECK(!store.JournalRenewalPending() && !opening->active());
					CHECK(store.failure() == u"capture-admission-renewed"_q);
				} else {
					CHECK(!renewed && !store.CompleteJournalRenewal(false));
					CHECK(store.JournalRenewalHeld() && !opening->active());
					CHECK(store.failure() == u"capture-admission-scope-changed"_q);
				}
			}
		}
		CHECK(store.Remove());
	}
}
int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	HeaderIoAdmissionControls();
	Section = "literal passive reader and document reuse with authenticated historical bindings";
	auto temp = QTemporaryDir(); CHECK(DesktopArchiveDirectory(temp.path()+u"/profile"_q));
	auto options = DesktopArchiveOptions{temp.path()+u"/archives"_q,temp.path()+u"/profile"_q,{0,123},u"0"_q,QByteArray(256,'k'),true};
	options.loginAuthority=QByteArray(32,'a'); options.loginAuthKeyId=options.loginAuthKeyIds=QByteArray(16,'1');
	auto store=DesktopArchiveStore(); CHECK(store.Open(options)==DesktopArchiveStatus::Ready);
	for (const auto message : {101,202,303}) Observe(store,message);
	auto access=store.JournalAccess(); CHECK(access); if (!access) return Finish();
	Value={std::make_shared<ProbeWorker>(),std::make_shared<DesktopArchiveJournal>(*access)}; CHECK(Value.journal->Open());
	auto streaming=StreamingProbe(); auto data=ProbeData();
	auto readers=base::flat_map<not_null<ProbeData*>,std::weak_ptr<ProbeReader>>();
	auto documents=base::flat_map<not_null<ProbeData*>,std::weak_ptr<ProbeDocument>>();
	auto one=streaming.sharedReader(readers,not_null{&data},Origin(101),false);
	auto two=streaming.sharedReader(readers,not_null{&data},Origin(202),false);
	CHECK(one==two && data.created==1 && Taps==2);
	auto docOne=streaming.sharedDocument(documents,readers,not_null{&data},nullptr,nullptr,Origin(101));
	auto docTwo=streaming.sharedDocument(documents,readers,not_null{&data},nullptr,nullptr,Origin(303));
	CHECK(docOne==docTwo && docTwo->reader==one && data.created==1);
	CHECK(StreamRegistry.streams.at(one.get()).observations.size()==3);
	auto edited=Event(Empty(),u"new303"_q,1000,u"later edit"_q,ArchiveOrigin::AuthoritativeEdit,Key({0,123},45,303));
	edited.content->media.push_back({u"document:555"_q,u"document"_q}); CHECK(store.Capture(edited).retained);
	CHECK(Value.journal->access().active());
	const auto guiBefore = store.StagingCountersForTest().fenceReads;
	one->_loader->event.fire({0,QByteArray("abc")});
	CHECK(store.StagingCountersForTest().fenceReads == guiBefore);
	CHECK(!Value.worker->tasks.empty());
	Value.worker->Run(store);
	CHECK(Owned(store,101)); CHECK(Owned(store,202)); CHECK(Owned(store,303)); CHECK(Gaps==0);
	CHECK(!store.ReadMedia(Key({0,123},45,303),u"new303"_q,u"document:555"_q,[](const auto&) { return true; }));
	std::printf("shared reader/document: origins101/202/303 owned, loaders=%d gaps=%d\n",data.created,Gaps);
	Observe(store,404);
	auto late=streaming.sharedDocument(documents,readers,not_null{&data},nullptr,nullptr,Origin(404));
	CHECK(late==docOne && data.created==1 && Gaps==1); CHECK(!Owned(store,404));
	auto duplicate=streaming.sharedReader(readers,not_null{&data},Origin(404),false); CHECK(Gaps==1);
	auto unbound=streaming.sharedReader(readers,not_null{&data},Origin(505),false); CHECK(Gaps==2 && data.created==1);
	one.reset(); two.reset(); duplicate.reset(); unbound.reset(); docOne.reset(); docTwo.reset(); late.reset();
	auto fresh=streaming.sharedReader(readers,not_null{&data},Origin(404),false); CHECK(data.created==2);
	fresh->_loader->event.fire({0,QByteArray("abc")}); Value.worker->Run(store); CHECK(Owned(store,404));
	const auto before = Gaps;
	fresh->_loader->event.fire({Media::Streaming::LoadedPart::kFailedOffset,{}}); CHECK(Gaps==before+1);
	std::printf("late binding explicit gap, unobserved refused; fresh-reader control owned with loaders=%d\n",data.created);
	Observe(store,606);
	const auto observation = StreamRegistry.observations.back();
	{
		const auto sink = ArchiveStreamSink(&streaming.owner.value, observation, 262145);
		const auto before = store.StagingCountersForTest().fenceReads;
		sink(0, QByteArray(128 * 1024, 'z'));
		CHECK(store.StagingCountersForTest().fenceReads == before);
		Value.worker->Run(store);
		CHECK(store.StagingCountersForTest().cachedStreams == 1);
	}
	Value.worker->Run(store);
	CHECK(store.StagingCountersForTest().cachedStreams == 0);
	CHECK(store.Close());
	const auto pending = Value.worker->tasks.size();
	const auto beforeClose = store.StagingCountersForTest().fenceReads;
	fresh->_loader->event.fire({0,QByteArray("abc")});
	CHECK(store.StagingCountersForTest().fenceReads == beforeClose);
	CHECK(Value.worker->tasks.size() == pending);
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(store.Remove());

	Section = "purge commit revokes GUI admission without fence reads";
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	Observe(store,707);
	const auto purgeObservation = StreamRegistry.observations.back();
	const auto purgeAccess = store.JournalAccess(); CHECK(purgeAccess);
	if (!purgeAccess) return Finish();
	Value.journal = std::make_shared<DesktopArchiveJournal>(*purgeAccess);
	CHECK(Value.journal->Open());
	const auto oldJournal = Value.journal;
	const auto purgeSink = ArchiveStreamSink(&streaming.owner.value, purgeObservation, 262145);
	purgeSink(0, QByteArray(128 * 1024, 'p'));
	Value.worker->Run(store);
	CHECK(store.StagingCountersForTest().cachedStreams == 1);
	const auto purgeSlice = store.ReadSlice(purgeObservation.key, {}, purgeObservation.observation);
	CHECK(purgeSlice);
	if (purgeSlice) {
		auto header = purgeSlice->state;
		header.messages.clear(); header.blobs.clear(); header.jobs.clear();
		header.leases.clear(); header.chatGenerations.clear();
		const auto purge = BeginArchiveSqlPurge(header,
			{ArchivePurgeKind::EditHistory, {header.account, std::nullopt, {}}, header.revision},
			{}, u"review-gui-purge"_q, store.security());
		CHECK(purge.program && store.Execute(purge.program));
		CHECK(store.StagingCountersForTest().cachedStreams == 0);
		const auto queued = Value.worker->tasks.size();
		const auto beforeGui = store.StagingCountersForTest().fenceReads;
		purgeSink(128 * 1024, QByteArray(1, 'p'));
		std::printf("REVIEW purge: memoryAdmission=%d queuedBefore=%zu queuedAfter=%zu guiFences=%llu\n",
			int(oldJournal->access().active()), queued, Value.worker->tasks.size(),
			static_cast<unsigned long long>(store.StagingCountersForTest().fenceReads - beforeGui));
		CHECK(!oldJournal->access().active());
		CHECK(Value.worker->tasks.size() == queued);
		CHECK(store.StagingCountersForTest().fenceReads == beforeGui);
		CHECK(!store.JournalAccess());
		CHECK(store.RetryMaintenance());
		auto finished = false;
		for (auto step = 0; step != 128 && !finished; ++step) {
			const auto result = store.ContinuePurge();
			CHECK(result.valid);
			if (!result.valid) break;
			finished = result.finished;
		}
		CHECK(finished);
		const auto renewed = store.JournalAccess(); CHECK(renewed);
		if (renewed) {
			Value.journal = std::make_shared<DesktopArchiveJournal>(*renewed);
			CHECK(Value.journal->Open());
			CHECK(Value.journal->access().active());
			const auto beforeRenewed = store.StagingCountersForTest().fenceReads;
			purgeSink(128 * 1024, QByteArray(1, 'p'));
			CHECK(!oldJournal->access().active());
			CHECK(Value.worker->tasks.size() == queued);
			CHECK(store.StagingCountersForTest().fenceReads == beforeRenewed);
		}
	}

	Section = "logout and discard revoke retained GUI producers";
	const auto closedJournal = Value.journal;
	const auto closeSink = ArchiveStreamSink(&streaming.owner.value, purgeObservation, 262145);
	CHECK(store.Close());
	CHECK(!closedJournal->access().active());
	const auto closeQueue = Value.worker->tasks.size();
	const auto closeFences = store.StagingCountersForTest().fenceReads;
	closeSink(0, QByteArray(1, 'p'));
	CHECK(Value.worker->tasks.size() == closeQueue);
	CHECK(store.StagingCountersForTest().fenceReads == closeFences);
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	const auto discardAccess = store.JournalAccess(); CHECK(discardAccess);
	if (discardAccess) {
		Value.journal = std::make_shared<DesktopArchiveJournal>(*discardAccess);
		CHECK(Value.journal->Open());
		const auto discardJournal = Value.journal;
		const auto discardSink = ArchiveStreamSink(&streaming.owner.value, purgeObservation, 262145);
		CHECK(store.Remove());
		CHECK(!discardJournal->access().active());
		const auto discardQueue = Value.worker->tasks.size();
		const auto discardFences = store.StagingCountersForTest().fenceReads;
		discardSink(0, QByteArray(1, 'p'));
		CHECK(Value.worker->tasks.size() == discardQueue);
		CHECK(store.StagingCountersForTest().fenceReads == discardFences);
		CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
		const auto freshAccess = store.JournalAccess(); CHECK(freshAccess && freshAccess->active());
		discardSink(0, QByteArray(1, 'p'));
		CHECK(!discardJournal->access().active());
		CHECK(Value.worker->tasks.size() == discardQueue);
		CHECK(store.StagingCountersForTest().fenceReads >= discardFences);
		CHECK(store.Remove());
	}
	return Finish();
}
