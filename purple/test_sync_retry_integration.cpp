#include <atomic>
#include "purple_sync_retry.h"

#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace Purple;
using mtpRequestId = int64_t;
using QString = std::string;
template <typename T> using Fn = std::function<T>;
static auto Mono = int64_t(0);
static auto Unix = int64_t(1000000);
namespace crl { int64_t now() { return Mono; } }
namespace base {
template <typename T> T take(T &v) { return std::exchange(v, {}); }
namespace unixtime { int64_t now() { return Unix; } }
}
std::string operator""_q(const char16_t *text, size_t size) {
	return std::string(text, text + size);
}
namespace MTP {
struct Text : std::string {
	using std::string::string;
	Text toUtf8() const { return *this; }
	const char *constData() const { return data(); }
};
struct Error {
	int value = 0;
	Text name;
	int code() const { return value; }
	Text type() const { return name; }
	static Error Local(std::string name, std::string) {
		return { -1, Text(name.c_str()) };
	}
};
}
struct Timer {
	int64_t due = 0;
	void cancel() { due = 0; }
 std::function<void()> callback;
 void setCallback(std::function<void()> f){callback=std::move(f);}
 void callOnce(int64_t ms){due=Mono+ms;}
};
namespace MTP { using ShiftedDcId = int; }
template <typename T> using FnMut = std::function<T>;
struct FakeRequest { using ResponseType=int; std::string payload; uint64_t random=7; };
struct Api {
 struct Event { FakeRequest request; FnMut<void(const int&)> done; Fn<void(const MTP::Error&)> fail; };
 std::map<mtpRequestId,Event> events;
 int cancelled=0;mtpRequestId next=100;
 struct Cancellation {Api *api;void cancel(){++api->cancelled;}};
 Cancellation request(mtpRequestId){return {this};}
 mtpRequestId allocateRequestId(){return ++next;}
 template<typename Request> struct Builder {
  Api *api;Request request;FnMut<void(const int&)> completed;Fn<void(const MTP::Error&)> failed;
  Builder &toDC(int){return *this;}
  Builder &done(FnMut<void(const int&)> value){completed=std::move(value);return *this;}
  Builder &fail(Fn<void(const MTP::Error&)> value){failed=std::move(value);return *this;}
  Builder &handleAllErrors(){return *this;}
  mtpRequestId send(){const auto id=++api->next;api->events.emplace(id,Event{request,completed,failed});return id;}
 };
 template<typename Request>Builder<Request> request(Request request){return {this,request,{},{}};}
};
namespace base {
template <typename T> struct Weak {
	T *pointer;
	std::weak_ptr<int> lifetime;
	explicit operator bool() const { return !lifetime.expired(); }
 T *get() const { return lifetime.expired()?nullptr:pointer; }
};
template<typename T>Weak<T> make_weak(T*);
}

class SyncRequestSender {
public:
	struct Pending {
		explicit Pending(int64_t now) : policy(now) {
		}
		SyncRetryState policy;
		Timer timer;
		Fn<void()> dispatch;
		Fn<void()> expired;
		Fn<void(Fn<void(bool)>)> resume;
		Fn<void(const MTP::Error &)> fail;
		std::optional<SyncRequestTransient> transient;
		mtpRequestId id = 1;
		mtpRequestId wire = 0;
		int64_t retryAtMs = 0;
		uint64_t generation = 0;
		bool active = true;
	};
	Fn<bool()> _current;
	Api _api;
	std::shared_ptr<int> lifetime = std::make_shared<int>(0);
	std::map<mtpRequestId, std::shared_ptr<Pending>> _pending;
	int publishes = 0;
	void publish() { ++publishes; }
	#include "retry-wire.inc"

	std::shared_ptr<Pending> create(Fn<void(const MTP::Error &)> fail);
	void cancelAll();
	struct Cancellation { SyncRequestSender *owner; mtpRequestId id; void cancel(){owner->cancel(id);} };
	Cancellation request(mtpRequestId id){return {this,id};}
	mtpRequestId watch(Fn<void()> expired, Fn<void()> retry, Fn<void()> cancelled);
	void resume(const std::shared_ptr<Pending> &pending);
	void arm(const std::shared_ptr<Pending> &p,int64_t ms);
	void dispatch(const std::shared_ptr<Pending> &pending);
	void failed(const std::shared_ptr<Pending> &pending,
		const MTP::Error &error, bool timeout = false);
	void tick(const std::shared_ptr<Pending> &pending);
	void cancel(mtpRequestId id);

};

namespace base {

template <typename T> Weak<T> make_weak(T *pointer) {
	return { pointer, pointer->lifetime };
}
}

#include "retry-controller.inc"

static auto Checks = 0;
static auto Failures = 0;
void Check(bool value, const char *name) {
	++Checks;
	if (!value) {
		++Failures;
		std::cerr << "FAIL " << name << '\n';
	}
}
struct Run {
	SyncRequestSender sender;
	std::shared_ptr<SyncRequestSender::Pending> pending;
	bool current = true;
	int sends = 0;
	int finals = 0;
	int permanentCode = 0;
	std::string permanentType;
	std::vector<std::string> payloads;
	Run() {
		Mono = 0;
		Unix = 1000000;
		pending = std::make_shared<SyncRequestSender::Pending>(Mono);
		sender._pending.emplace(1, pending);
		sender._current = [this] { return current; };
		pending->dispatch = [this, payload = std::string("same-peer/body/random-id/part") ] {
			payloads.push_back(payload);
			pending->wire = ++sends;
		};
		pending->fail = [this](const MTP::Error &error) {
			++finals;
			permanentCode = error.code();
			permanentType = error.type();
		};
		sender.dispatch(pending);
	}
	void Advance(int64_t ms) {
		Mono += ms;
		Unix = 1000000 + Mono / 1000;
		if (pending->timer.due && Mono >= pending->timer.due) {
			pending->timer.due = 0;
			sender.tick(pending);
		}
	}
};


namespace Main { struct Session; }
struct AccountMock {Main::Session *session=nullptr;bool loggingOut()const{return false;}Main::Session*maybeSession()const{return session;}};
namespace Main {struct Session {AccountMock a;AccountMock&account(){a.session=this;return a;}};}
int SessionAccountKey(Main::Session*){return 1;}
struct PeerToken {int id=0;bool operator==(const PeerToken&)const=default;};
struct Io {struct Peer {struct Origin {int account=1;} origin;PeerToken chat;} peer;};
bool ValidDesktopSyncChatIo(const Io&){return true;}
bool CompatibleDesktopSyncChatIo(const Io&,const Io&){return true;}
struct Lease {Io route;Fn<bool()>current;};
enum class DesktopSyncChatPhase {Use,Paused,Searching};
struct DesktopSyncChatStatus {bool complete=true,postingHeld=false;DesktopSyncChatPhase phase=DesktopSyncChatPhase::Use;struct Resolution {PeerToken readChat;} resolution;int64_t retryAt=0;std::string reason;};
bool MarkValid=true,HoldGate=false;int Gates=0;Fn<void(DesktopSyncChatStatus)> DeferredGate;
void CheckSyncChatBeforePost(Main::Session*,Fn<void(DesktopSyncChatStatus)>done,int){++Gates;if(HoldGate){DeferredGate=std::move(done);return;}auto s=DesktopSyncChatStatus();s.complete=MarkValid;s.postingHeld=!MarkValid;done(s);}
std::optional<Lease> SyncChatIoForPost(Main::Session*,PeerToken){return Lease{Io{},[]{return true;}};}
class SyncConfigUpload:public std::enable_shared_from_this<SyncConfigUpload> {
 public:
  struct WeakSession {Main::Session *value;Main::Session*get()const{return value;}}_session;
  bool _done=false,_checking=false;int _editId=0;Fn<void(Fn<void(bool)>)> _authorizeEdit;int _account=1,_operation=1,finals=0;std::optional<Io>_route;std::optional<Lease>_lease;
  Fn<bool()>_stillCurrent=[]{return true;};std::unique_ptr<SyncRequestSender>_api;
  SyncConfigUpload(Main::Session *s):_session{s},_api(std::make_unique<SyncRequestSender>()){_api->_current=[this]{return Current();};}
  bool Current()const;void SendMedia();void RenewBeforePost(Fn<void(bool)> ready);void ResumeEffect(Fn<void(bool)> ready);
  void Finish(std::optional<int>){_done=true;++finals;_api->cancelAll();}
  void DispatchMedia(){_api->send(FakeRequest{"send-or-edit",7},[this](const int&){Finish(1);},[this](const MTP::Error&){Finish({});},0,[self=shared_from_this()](Fn<void(bool)> ready){self->ResumeEffect(std::move(ready));});}
};

#include "retry-upload.inc"

struct DesktopSyncChatOperation { Fn<bool()> current; int marker=0; };
struct ChangesMock {int count=0;void fire_copy(Main::Session*){++count;}} Changes;
void RetireDisk(int){}
struct LocalOpenProbe {
 bool _busy=true,_retryLocalOpen=false,_openContinuation=false,context=true,borrowed=true,operationCurrent=true;
 std::shared_ptr<std::atomic<bool>> _current=std::make_shared<std::atomic<bool>>(true);
 uint64_t _run=1;int _disk=1;
 DesktopSyncChatStatus _status;Timer _timer,_requestTimeout;
 struct {Main::Session *get()const{return nullptr;}} _session;
 std::vector<Fn<void(DesktopSyncChatStatus)>> _callbacks;
 std::shared_ptr<int> lifetime=std::make_shared<int>(0);
 bool Current()const{return context&&(!borrowed||operationCurrent)&&_current&&_current->load();}
 bool _beforePost=false;int64_t _lastStarted=0;DesktopSyncChatOperation _operation;
 int begins=0;bool openedBorrowed=false;
 DesktopSyncChatStatus status()const{return _status;}
 void Begin(bool){++begins;openedBorrowed=bool(_operation.current);_busy=true;}
 void Ensure(bool force,Fn<void(DesktopSyncChatStatus)> done,DesktopSyncChatOperation operation,bool beforePost);
 void Publish(bool final=true);void RetryLocalOpen();
};
#include "retry-local-open.inc"
#include "retry-ensure.inc"

struct DownloadProbe {
 SyncRequestSender _api;std::shared_ptr<int> lifetime=std::make_shared<int>(0);
 bool _done=false,valid=true,fail=false;int requests=0,finals=0;
 std::unique_ptr<int> _loader;std::optional<int> _downloaded;
 mtpRequestId _downloadWatch=0;uint64_t _downloadGeneration=0;
 DownloadProbe(){_api._current=[this]{return Current();};}
 bool Current()const{return valid;}
 void Cancel(){_done=true;++finals;_api.cancelAll();RetireDownload();}
 void RequestNext(){++requests;}
 void RetireDownload();void RetryDownload();
 void Start(){
  _loader=std::make_unique<int>(1);
#include "retry-download-watch.inc"
  late=[weak,generation]{const auto self=weak.get();if(self&&self->_downloadGeneration==generation){++self->finals;}};
 }
 Fn<void()> late;
};
#include "retry-download.inc"

struct SyncAutoPlan { int ticket=1; };
struct SyncAutoServerRead { bool fresh=true,owned=true; };
struct EffectStore {
 bool target=true;int authorizations=0;
 bool AuthorizeEffect(const SyncAutoPlan &,const SyncAutoServerRead *read){
  ++authorizations;return target&&read&&read->fresh&&read->owned;
 }
};
class AutoSession {
public:
 struct {Main::Session *value;Main::Session *get()const{return value;}}_session;
 std::shared_ptr<int> lifetime=std::make_shared<int>(0);
 std::unique_ptr<EffectStore> _store=std::make_unique<EffectStore>();
 bool current=true,fresh=true,owned=true;int reads=0;
 bool EffectCurrent(const SyncAutoPlan &)const{return current;}
 int OperationFor(const SyncAutoPlan &){return 1;}
 void ReadForEffect(const SyncAutoPlan &,Fn<void(SyncAutoServerRead)> done){++reads;done({fresh,owned});}
 void RenewEffect(const SyncAutoPlan &plan, Fn<void(bool)> ready);
};
#include "retry-delete-gate.inc"

int main(){
 for(const auto type:{"FLOOD_WAIT_8","FLOOD_WAIT_90","FLOOD_PREMIUM_WAIT_90"}){
  for(bool valid:{false,true}){
   Mono=0;Unix=1000000;MarkValid=true;HoldGate=false;Gates=0;Main::Session session;
   auto upload=std::make_shared<SyncConfigUpload>(&session);upload->SendMedia();
   auto &sender=*upload->_api;auto pending=sender._pending.begin()->second;
   auto first=sender._api.events[pending->wire];
   Check(Gates==1&&sender._api.events.size()==1,"initial before-post gate");
   first.fail({420,MTP::Text(type)});MarkValid=valid;Mono=pending->retryAtMs;sender.tick(pending);
   Check(Gates==2,"flood resume renews full before-post evidence");
   Check(sender._api.events.size()==(valid?2:1),"changed mark never replayed");
   if(valid){auto last=sender._api.events.rbegin()->second;Check(last.request.payload==first.request.payload&&last.request.random==first.request.random,"same wire identity after renewed authority");}
   Check(upload->finals==(valid?0:1),"refused renewal reaches outcome-unknown");
   upload->Finish({});
  }
 }
 for(const auto &error:{MTP::Error{500,MTP::Text("INTERNAL")},MTP::Error{-1,MTP::Text("TRANSPORT")},MTP::Error{-1,MTP::Text("TIMEOUT")}}){
  Mono=0;Unix=1000000;MarkValid=true;Gates=0;Main::Session session;
  auto upload=std::make_shared<SyncConfigUpload>(&session);upload->SendMedia();auto &sender=*upload->_api;auto p=sender._pending.begin()->second;
  if(error.type()=="TIMEOUT"){Mono=30000;sender.tick(p);}else sender._api.events[p->wire].fail(error);
  Check(upload->finals==1&&sender._pending.empty()&&sender._api.events.size()==1,"ambiguous effect reconciles without replay");
 }
 for(bool authorized:{false,true}){
  Mono=0;Unix=1000000;MarkValid=true;Gates=0;Main::Session session;
  auto upload=std::make_shared<SyncConfigUpload>(&session);upload->_editId=7;int targetReads=0;
  upload->_authorizeEdit=[&](Fn<void(bool)> ready){++targetReads;ready(targetReads==1||authorized);};upload->SendMedia();
  auto &sender=*upload->_api;auto p=sender._pending.begin()->second;sender.failed(p,{420,MTP::Text("FLOOD_WAIT_90")});Mono=p->retryAtMs;sender.tick(p);
  Check(targetReads==2&&Gates==2,"edit repeats before-post and exact target authorization");
  Check(sender._api.events.size()==(authorized?2:1),"changed edit target cannot replay");upload->Finish({});
 }
 {
  Mono=0;Unix=1000000;SyncRequestSender sender;sender._current=[]{return true;};int done=0,final=0;
  auto id=sender.send(FakeRequest{"read",4},[&](const int&){++done;},[&](const MTP::Error&){++final;},0,{});
  auto p=sender._pending.at(id);auto first=sender._api.events[p->wire];Mono=30000;sender.tick(p);first.done(1);
  Check(done==0&&final==0,"late timed out read rejected");Mono=p->retryAtMs;sender.tick(p);auto last=sender._api.events[p->wire];first.fail({400,MTP::Text("CHANNEL_PRIVATE")});last.done(1);last.done(1);
  Check(done==1&&final==0&&sender._pending.empty(),"safe read resumes once with literal templated callbacks");
 }
 for(bool borrowed:{false,true}){
  Mono=0;Unix=1000000;LocalOpenProbe service;service.borrowed=borrowed;
  HoldGate=true;MarkValid=true;Main::Session session;auto upload=std::make_shared<SyncConfigUpload>(&session);upload->SendMedia();service._callbacks.push_back(std::move(DeferredGate));
  const auto old=service._current;const auto oldRun=service._run;service.RetryLocalOpen();
  Check(upload->finals==0&&upload->_checking&&service._callbacks.size()==1,"local-open timeout retains logical completion");
  Check(!old->load()&&!service._current&&service._run!=oldRun&&service._timer.due==Mono+5000,"late worker capability revoked with timed retry");
  Check(service._retryLocalOpen&&service._openContinuation,"owned and borrowed continuation retained");
  service._current=std::make_shared<std::atomic<bool>>(true);service._status=DesktopSyncChatStatus();service._busy=false;service.Publish();HoldGate=false;
  Check(upload->finals==0&&upload->_api->_api.events.size()==1&&service._callbacks.empty(),"recovered open delivers once and posts once");
  upload->Finish({});
 }
 for(bool current:{false,true}){
  Mono=0;Unix=1000000;DownloadProbe reader;reader.Start();auto late=reader.late;auto p=reader._api._pending.at(reader._downloadWatch);Mono=30000;reader._api.tick(p);
  Check(!reader._loader&&reader.finals==0&&p->transient&&p->transient->kind==SyncTransientKind::Timeout,"stalled loader retired with visible typed retry");
  late();Check(reader.finals==0,"retired loader callback cannot complete candidate");reader.valid=current;Mono=p->retryAtMs;reader._api.tick(p);
  Check(reader.requests==(current?1:0)&&reader.finals==(current?0:1),"download retry preserves or cancels current candidate without final Unknown");
  Check(reader._api._pending.empty(),"old download timer removed before fresh metadata");
 }
 {
  Mono=0;Unix=1000000;SyncRequestSender sender;sender._current=[]{return true;};Fn<void(bool)> gate;
  auto id=sender.send(FakeRequest{"effect",9},[](const int&){},[](const MTP::Error&){},0,[&](Fn<void(bool)> f){gate=f;});auto p=sender._pending.at(id);
  sender.failed(p,{420,MTP::Text("FLOOD_WAIT_8")});Mono=p->retryAtMs;sender.tick(p);sender.cancelAll();gate(true);
  Check(sender._api.events.size()==1,"cancel during asynchronous resume gate prevents replay");
 }
 for(int mode=0;mode!=5;++mode){
  Main::Session session;AutoSession owner;owner._session.value=&session;MarkValid=mode!=0;HoldGate=false;
  if(mode==1)owner._store->target=false;
  if(mode==2)owner.fresh=false;
  if(mode==3)owner.owned=false;
  int replies=0;bool allowed=false;
  owner.RenewEffect({},[&](bool value){++replies;allowed=value;});
  Check(replies==1&&allowed==(mode==4),"delete resume renews mark and exact owned target bytes");
  Check(owner.reads==(mode==0?0:1),"lost mark refuses before delete target read");
 }
 {
  Mono=0;Unix=1000000;DownloadProbe reader;reader.Start();auto late=reader.late;
  reader.Cancel();late();Mono=60000;
  Check(reader._api._pending.empty()&&reader.finals==1&&reader.requests==0,"logout cancels stalled downloader and its timer");
 }
 {
  Mono=0;Unix=1000000;DownloadProbe reader;reader.Start();auto old=reader.late;auto p=reader._api._pending.at(reader._downloadWatch);
  Mono=30000;reader._api.tick(p);Mono=p->retryAtMs;reader._api.tick(p);reader.Start();old();
  Check(reader.finals==0&&reader.requests==1&&reader._loader,"fresh download is not completed by old generation");reader.Cancel();
 }
 {
  Mono=0;Unix=1000000;SyncRequestSender sender;sender._current=[]{return true;};Fn<void(bool)> gate;
  auto id=sender.send(FakeRequest{"effect",9},[](const int&){},[](const MTP::Error&){},0,[&](Fn<void(bool)> f){gate=f;});auto p=sender._pending.at(id);
  sender.failed(p,{420,MTP::Text("FLOOD_WAIT_8")});Mono=p->retryAtMs;sender.tick(p);const auto original=p->policy.deadline();Mono+=90000;gate(true);
  Check(p->policy.deadline()==original+90000&&sender._api.events.size()==2,"async authority read pauses only effect response clock");
  Check(!p->transient,"renewed effect clears wait notice");sender.cancelAll();
 }
 for(const auto overdue:{0,5,60}){
  for(const auto force:{false,true}){
   for(const auto replacement:{false,true}){
    Mono=0;Unix=1000000;LocalOpenProbe service;service._operation={[]{return true;},19};service._beforePost=true;
    int original=0,joined=0;
    service._callbacks.push_back([&](DesktopSyncChatStatus){++original;});service.RetryLocalOpen();
    const auto timer=service._timer.due;Mono+=overdue*1000;Unix+=overdue;
    auto other=DesktopSyncChatOperation();if(replacement){other={[]{return true;},77};}
    service.Ensure(force,[&](DesktopSyncChatStatus){++joined;},other,false);
    Check(service._operation.current&&service._operation.marker==19&&service._beforePost,
      "due or overdue force call preserves original borrowed capability and before-post intent");
    Check(service._callbacks.size()==2&&original==0&&joined==0&&service.begins==0&&service._timer.due==timer,
      "new ensure joins pending local open without early completion or another timer");
    service.Begin(false);Check(service.openedBorrowed,"timer reopening still borrows original writer lock");
    service._current=std::make_shared<std::atomic<bool>>(true);service._status=DesktopSyncChatStatus();service.Publish();service.Publish();
    Check(original==1&&joined==1,"retained and joined local-open callbacks each complete once");
   }
  }
 }
 std::cout<<"INTEGRATION "<<Checks<<" checks, "<<Failures<<" failures\n";return Failures?1:0;
}
