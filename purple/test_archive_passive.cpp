#include "base/unixtime.h"
#include "base/flat_map.h"
#include "base/weak_ptr.h"
#include "purple/purple_archive_acquired_source.h"
#include "purple/purple_archive_capture.h"
#include "purple/purple_archive_journal.h"
#include "purple/purple_archive_files.h"
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
	void Run(DesktopArchiveStore &store) { while (!tasks.empty()) { auto task = std::move(tasks.front()); tasks.pop_front(); task(store); } }
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
int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
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
	one->_loader->event.fire({0,QByteArray("abc")}); Value.worker->Run(store);
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
	CHECK(store.Remove());
	return Finish();
}
