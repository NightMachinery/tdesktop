#include "base/basic_types.h"
#include "base/algorithm.h"
#include <range/v3/all.hpp>
#include "base/weak_ptr.h"
#include "base/weak_qptr.h"
#include "rpl/rpl.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>
#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <variant>

int Checks = 0;
int Failures = 0;
#define CHECK(value) do { ++Checks; if (!(value)) { ++Failures; std::printf("FAIL routes:%d %s\n", __LINE__, #value); } } while (false)
template <typename T> class object_ptr : public std::unique_ptr<T> {
public:
	using std::unique_ptr<T>::unique_ptr;
	T *data() const { return this->get(); }

};
struct AdminMethod;
struct AdminRequest;
struct Session : base::has_weak_ptr {
	AdminRequest request(AdminMethod);
	std::vector<int> cancelled;
	struct Request {
		Session *session = nullptr;
		int id = 0;
		void cancel() { session->cancelled.push_back(id); }
	};
	Session &api() { return *this; }
	Request request(int id) { return {this, id}; }
};
struct PeerData {
	struct { uint64_t value = 0; } id;
	Session *owner = nullptr;
	bool hidden = false;
	bool user = false;
	int rating = 99;
	int pin = 3;
	bool contact = true;
	Session &session() { return *owner; }
	bool isUser() const { return user; }
};
struct ChannelData : PeerData {};
struct History { PeerData *peer = nullptr; bool purpleHiddenFromChatList() const { return peer->hidden; } };
namespace Data {
struct Thread {
	PeerData *value = nullptr;
	not_null<PeerData*> peer() const { return value; }
};
struct SavedSublist : Thread {};
}
using PeerListRowId = uint64_t;
struct PeerListRow {
	explicit PeerListRow(PeerData *value) : value(value) {}
	PeerData *value = nullptr;
	bool selected = false;
	PeerListRowId rowId = 0;
	not_null<PeerData*> peer() const { return value; }
	PeerListRowId id() const { return rowId ? rowId : value->id.value; }
	bool checked() const { return selected; }
};
namespace Ui {
struct BoxContent : QObject {
	rpl::lifetime life;
	int closed = 0;
	rpl::lifetime &lifetime() { return life; }
	void closeBox() { ++closed; }
};
struct ConfirmBoxArgs {
	std::variant<std::monostate, Fn<void()>, Fn<void(Fn<void()>)>> confirmed;
};
struct GenericBox : BoxContent {
	ConfirmBoxArgs args;
	void confirm() {
		if (const auto fn = std::get_if<1>(&args.confirmed)) { if (*fn) (*fn)(); }
		else if (const auto fn = std::get_if<2>(&args.confirmed)) { if (*fn) (*fn)([=] { closeBox(); }); }
	}
};
object_ptr<GenericBox> MakeConfirmBox(ConfirmBoxArgs &&args) {
	auto result = object_ptr<GenericBox>(new GenericBox());
	result->args = std::move(args);
	return result;
}
}
struct Show {
	object_ptr<Ui::GenericBox> box;
	void showBox(object_ptr<Ui::GenericBox> value) { box = std::move(value); }
};
struct PeerListController;
struct Menu {
	bool detached = false;
	void setDestroyedCallback(std::nullptr_t) { detached = true; }
};
struct PeerListContent {
	PeerListController *_controller = nullptr;
	std::unique_ptr<Menu> _contextMenu;
};
struct Delegate {
	PeerListContent *content = nullptr;
	std::vector<std::unique_ptr<PeerListRow>> rows;
	std::vector<std::unique_ptr<PeerListRow>> search;
	Show show;
	int refreshed = 0;
	int peerListFullRowsCount() { return rows.size(); }
	int peerListSearchRowsCount() { return search.size(); }
	PeerListRow *peerListRowAt(int i) { return rows.at(i).get(); }
	PeerListRow *peerListSearchRowAt(int i) { return search.at(i).get(); }
	PeerListRow *peerListFindRow(PeerListRowId id) {
		for (const auto list : {&rows, &search}) for (const auto &row : *list) if (row->id() == id) return row.get();
		return nullptr;
	}
	bool peerListIsRowChecked(PeerListRow *row) { return row->checked(); }
	void peerListSetRowChecked(PeerListRow *row, bool value) { row->selected = value; }
	void peerListRemoveRow(PeerListRow *row) {
		CHECK(!row->checked());
		for (const auto list : {&rows, &search}) std::erase_if(*list, [&](const auto &entry) { return entry.get() == row; });
	}
	void peerListAppendRow(std::unique_ptr<PeerListRow> row);
	void peerListSortRows(Fn<bool(const PeerListRow&, const PeerListRow&)> compare) {
		std::stable_sort(rows.begin(), rows.end(), [&](const auto &a, const auto &b) { return compare(*a, *b); });
	}
	void peerListRefreshRows() { ++refreshed; }
	Show *peerListUiShow() { return &show; }
};
struct PeerListGlobalSearchController { int calls = 0; void searchInCache() { ++calls; } };
struct PeerListSearchDelegate { virtual ~PeerListSearchDelegate() = default; };
struct PeerListController : PeerListSearchDelegate, base::has_weak_ptr {
	PeerListContent content{this};
	Delegate view{&content};
	rpl::lifetime life;
	PeerListGlobalSearchController search;
	Delegate *delegate() { return &view; }
	rpl::lifetime &lifetime() { return life; }
	PeerListGlobalSearchController *searchController() { return &search; }
};
struct ContactsBoxController : PeerListController {
	std::vector<PeerData*> contacts;
	void rebuildRows() { for (const auto peer : contacts) view.peerListAppendRow(std::make_unique<PeerListRow>(peer)); }
};
struct ChatsListBoxController : PeerListController {};
struct AddBotToGroupBoxController : PeerListController {
	Session *owner = nullptr;
	int _existingRightsRequestId = 0;
	ChannelData *_existingRightsChannel = nullptr;
	std::optional<int> _existingRights;
	Session &session() { return *owner; }
};
namespace Purple {
rpl::event_stream<> Active;
rpl::producer<> ActiveChanges() { return Active.events(); }
bool PickerVisible(not_null<PeerData*> peer) { return !peer->hidden; }
void WatchPickerChild(not_null<Ui::BoxContent*> box, not_null<PeerData*> peer);
std::function<bool()> PickerAction(not_null<PeerData*> peer, rpl::lifetime &lifetime, Fn<void()> revoked = nullptr);
class RecipientPickerBridge {
public:
	static void WatchRows(not_null<PeerListController*> controller, Fn<void()> refill = nullptr);
	static void WatchContacts(not_null<ContactsBoxController*> controller);
	static bool RowAllows(not_null<PeerListContent*> content, not_null<PeerListRow*> row);
	static bool SearchAllows(not_null<PeerListSearchDelegate*> delegate, not_null<PeerData*> peer);
	static void BeforeRemove(not_null<PeerListContent*> content);
	static std::function<bool()> BotRights(not_null<AddBotToGroupBoxController*> controller, not_null<ChannelData*> channel);

};
std::map<ChatsListBoxController*, int> Recipients;
#include "routes-state.inc"
#include "routes-owned.inc"
#include "routes-templates.inc"
}
void Delegate::peerListAppendRow(std::unique_ptr<PeerListRow> row) {
	if (!Purple::RecipientPickerBridge::RowAllows(content, row.get())) return;
	if (!peerListFindRow(row->id())) rows.push_back(std::move(row));
}
struct PeerSet : std::set<not_null<PeerData*>> { void remove(not_null<PeerData*> peer) { erase(peer); } };
struct ChoosePeerBoxController : PeerListController {
	struct { int maxQuantity = 1; } _query;
	PeerData *_bot = nullptr;
	PeerSet _selected;
	int _selectedCount = 0;
	Fn<void(std::vector<not_null<PeerData*>>)> _callback;
	void rowClicked(not_null<PeerListRow*> row);
};
template <typename Query>
object_ptr<Ui::GenericBox> MakeConfirmBox(PeerData*, PeerData *peer, Query, Fn<void()> callback) {
	return Purple::PickerConfirmBox(peer, {.confirmed = std::move(callback)});
}
namespace Ui { template <typename T, typename F> void PostponeCall(T*, F callback) { callback(); } }
struct AuxController : PeerListController {
	bool _prepared = false;
	bool _refreshing = false;
	std::vector<PeerData*> _list;
	void prepare();
	void addRow(not_null<PeerData*> peer);
	Fn<void(not_null<PeerData*>)> _callback;
	void rowClicked(not_null<PeerListRow*> row);
};
struct Forum { Data::Thread *value = nullptr; Data::Thread *history() { return value; } };
struct ChooseTopicBoxController : base::has_weak_ptr {
	struct Row : PeerListRow { using PeerListRow::PeerListRow; Data::Thread *value = nullptr; Data::Thread *topic() { return value; } };
	Forum *_forum = nullptr;
	Fn<bool(not_null<Data::Thread*>)> _filter;
	Fn<void(not_null<Data::Thread*>)> _callback;
	void rowClicked(not_null<PeerListRow*> row);
};
struct Monoforum {
	Data::SavedSublist *value = nullptr;
	not_null<Data::SavedSublist*> sublist(PeerData*) { return value; }
};
struct ChooseSublistBoxController : base::has_weak_ptr {
	Monoforum *_monoforum = nullptr;
	Fn<bool(not_null<Data::SavedSublist*>)> _filter;
	Fn<void(not_null<Data::SavedSublist*>)> _callback;
	void rowClicked(not_null<PeerListRow*> row);
};
namespace crl { template <typename T, typename F> F guard(T*, F callback) { return callback; } }
int BotAdds = 0;
void AddBotToGroup(int, PeerData*, PeerData*, const QString&) { ++BotAdds; }
struct Window { int closes = 0; void hideLayer() { ++closes; } };
struct BotCallback {
	QString _token;
	Fn<void()> confirm;
	Window window;
	PeerData botPeer;
	void prepare(PeerData *chat, rpl::lifetime &lifetime) {
		const auto current = Purple::PickerAction(chat, lifetime, nullptr);
		const auto show = 0;
		const auto bot = &botPeer;
		const auto controller = &window;
#include "routes-bot-callback.inc"
		confirm = callback;
	}
};
struct CustomController : PeerListController {
	Fn<void(not_null<PeerListController*>)> _fill;
	void prepare();
};
struct StarController {
	enum class PickType { Activate, SendMessage, OpenProfile };
	Fn<void(not_null<PeerData*>, PickType)> _pick;
	void rowClicked(not_null<PeerListRow*> row);
};
struct FrequentController : PeerListController {
	std::vector<PeerData*> _users;
	void prepare();
	Fn<void(not_null<PeerData*>)> _chosen;
	void rowClicked(not_null<PeerListRow*> row);
};
struct CreditController {
	Fn<void(not_null<PeerData*>)> _choose;
	void rowClicked(not_null<PeerListRow*> row);
};
namespace MTP {
struct Error { QString value; const QString &type() const { return value; } };
}
namespace Ui { struct Show { void showToast(const QString&) {} }; }
struct ChatData : PeerData {
	int inputChat() { return 1; }
	void applyEditAdmin(PeerData*, bool) {}
};
struct UserData : PeerData { int inputUser() { return 2; } };
struct AdminMethod {};
AdminMethod MTPmessages_EditChatAdmin(int, int, bool) { return {}; }
bool MTP_bool(bool value) { return value; }
struct AdminRequest {
	Fn<void()> doneCallback;
	Fn<void(const MTP::Error&)> failCallback;
	template <typename F> AdminRequest &done(F callback) { doneCallback = std::move(callback); return *this; }
	template <typename F> AdminRequest &fail(F callback) { failCallback = std::move(callback); return *this; }
	void send();
};
std::vector<AdminRequest> AdminRequests;
std::vector<Fn<void()>> Joins;
void AdminRequest::send() { AdminRequests.push_back(std::move(*this)); }
AdminRequest Session::request(AdminMethod) { return {}; }
void AddChatParticipant(std::shared_ptr<Ui::Show>, not_null<ChatData*>, not_null<UserData*>, Fn<void()> done, Fn<void()>) {
	Joins.push_back(std::move(done));
}
#include "routes-admin.inc"
struct CommunityCallback : base::has_weak_ptr {
	Fn<bool(not_null<Data::Thread*>)> _filter;
	Fn<void(not_null<Data::Thread*>)> _callback;
	Fn<void(not_null<Data::Thread*>)> prepare(Ui::BoxContent *box) {
		auto guard = base::make_weak(this);
		const auto weak = std::make_shared<base::weak_qptr<Ui::BoxContent>>(box);
#include "routes-community.inc"
		return callback;
	}
};
struct DropContent {
	int dispatches = 0;
	template <typename Thread> bool filesOrForwardDrop(not_null<Thread*>, int*, bool) { ++dispatches; return true; }
};
struct DropNavigation {
	DropContent value;
	DropNavigation *parentController() { return this; }
	DropContent *content() { return &value; }
};
auto DropTopic(DropNavigation *navigation, Ui::BoxContent *box, Fn<void()> successCallback) {
	auto data = std::make_shared<int>(0);
	const auto weak = std::make_shared<base::weak_qptr<Ui::BoxContent>>(box);
#include "routes-drop-topic.inc"
	return chosen;
}
auto DropSublist(DropNavigation *navigation, Ui::BoxContent *box, Fn<void()> successCallback) {
	auto data = std::make_shared<int>(0);
	const auto weak = std::make_shared<base::weak_qptr<Ui::BoxContent>>(box);
#include "routes-drop-sublist.inc"
	return chosen;
}
#include "routes-native.inc"
std::vector<uint64_t> Ids(const Delegate &view) {
	auto result = std::vector<uint64_t>();
	for (const auto &row : view.rows) result.push_back(row->id());
	return result;
}
void Changed() { Purple::Active.fire({}); QCoreApplication::processEvents(); }
void Cases() {
	using namespace Purple;
	auto session = Session();
	auto a = PeerData{{1}, &session};
	auto b = PeerData{{2}, &session};
	auto c = PeerData{{3}, &session};
	const auto raw = std::vector<PeerData*>{&a, &b, &c};
	for (const auto hiddenAtOpen : {false, true}) {
		a.hidden = hiddenAtOpen;
		auto management = AuxController();
		management._list = raw;
		management.prepare();
		CHECK(Ids(management.view) == (hiddenAtOpen ? std::vector<uint64_t>{2, 3} : std::vector<uint64_t>{1, 2, 3}));
		a.hidden = false; Changed();
		CHECK(Ids(management.view) == (std::vector<uint64_t>{1, 2, 3}));
		management.view.peerListFindRow(1)->selected = true;
		a.hidden = true; Changed();
		CHECK(!management.view.peerListFindRow(1));
		CHECK(!RecipientPickerBridge::SearchAllows(&management, &a));
		a.hidden = false; Changed();
		CHECK(Ids(management.view) == (std::vector<uint64_t>{1, 2, 3}));
		CHECK(management.search.calls == 0);
	}
	for (const auto source : {"ordinary contacts", "cached contact search"}) {
		std::printf("source %s\n", source);
		auto contacts = ContactsBoxController();
		contacts.contacts = raw;
		RecipientPickerBridge::WatchContacts(&contacts);
		contacts.rebuildRows();
		a.hidden = true; Changed();
		CHECK(Ids(contacts.view) == (std::vector<uint64_t>{2, 3}));
		CHECK(!RecipientPickerBridge::SearchAllows(&contacts, &a));
		contacts.contacts = {&c, &a, &b};
		a.hidden = false; Changed();
		CHECK(Ids(contacts.view) == (std::vector<uint64_t>{3, 1, 2}));
		CHECK(contacts.contacts == (std::vector<PeerData*>{&c, &a, &b}));
		CHECK(contacts.search.calls >= 2);
	}
	for (const auto source : {"birthday", "frequent stars", "self"}) {
		std::printf("custom source %s\n", source);
		a.hidden = true;
		auto custom = CustomController();
		auto candidates = raw;
		custom._fill = [&](not_null<PeerListController*> controller) {
			for (const auto peer : candidates) controller->view.peerListAppendRow(std::make_unique<PeerListRow>(peer));
		};
		custom.prepare(); CHECK(Ids(custom.view) == (std::vector<uint64_t>{2, 3}));
		a.hidden = false; Changed(); CHECK(Ids(custom.view) == (std::vector<uint64_t>{1, 2, 3}));
		candidates = {&c, &a, &b}; Changed(); CHECK(Ids(custom.view) == (std::vector<uint64_t>{3, 1, 2}));
		a.hidden = true; Changed(); CHECK(!custom.view.peerListFindRow(1));
	}
	{
		auto frequent = FrequentController(); frequent._users = raw; frequent.prepare();
		CHECK(!frequent.view.peerListFindRow(1));
		a.hidden = false; Changed(); CHECK(Ids(frequent.view) == (std::vector<uint64_t>{1, 2, 3}));
		a.hidden = true; Changed(); CHECK(!frequent.view.peerListFindRow(1));
	}
	a.hidden = true;
	{
		auto privacy = ContactsBoxController();
		privacy.contacts = raw;
		privacy.rebuildRows(); Changed();
		CHECK(Ids(privacy.view) == (std::vector<uint64_t>{1, 2, 3}));
		CHECK(RecipientPickerBridge::SearchAllows(&privacy, &a));
	}
	{
		auto controller = std::make_unique<PeerListController>();
		RecipientPickerBridge::WatchRows(controller.get());
		Active.fire({}); controller.reset(); QCoreApplication::processEvents();
		CHECK(RowPickers.empty());
	}
	for (const auto revive : {false, true}) {
		a.hidden = false;
		auto choose = ChoosePeerBoxController();
		auto row = PeerListRow(&a);
		auto calls = 0;
		choose._callback = [&](auto peers) { CHECK(!peers.front()->hidden); ++calls; };
		choose.rowClicked(&row); CHECK(bool(choose.view.show.box));
		a.hidden = true; Changed(); CHECK(choose.view.show.box->closed > 0);
		if (revive) { a.hidden = false; Changed(); }
		choose.view.show.box->confirm(); CHECK(calls == 0);
	}
	a.hidden = false;
	{
		auto choose = ChoosePeerBoxController(); auto row = PeerListRow(&a); auto calls = 0;
		choose._callback = [&](auto) { ++calls; }; choose.rowClicked(&row); choose.view.show.box->confirm(); CHECK(calls == 1);
	}
	{
		auto calls = 0; auto aux = AuxController(); aux._callback = [&](auto) { ++calls; }; auto row = PeerListRow(&a);
		a.hidden = true; aux.rowClicked(&row); CHECK(calls == 0);
		a.hidden = false; aux.rowClicked(&row); CHECK(calls == 1);
	}
	{
		auto lifetime = rpl::lifetime(); auto bot = BotCallback(); bot.prepare(&a, lifetime);
		a.hidden = true; Changed(); a.hidden = false; Changed(); bot.confirm(); CHECK(BotAdds == 0);
		bot.prepare(&a, lifetime); bot.confirm(); CHECK(BotAdds == 1);
	}
	{
		auto channel = ChannelData(); channel.id.value = 4; channel.owner = &session;
		auto next = ChannelData(); next.id.value = 5; next.owner = &session;
		auto bot = AddBotToGroupBoxController(); bot.owner = &session;
		bot._existingRightsChannel = &channel; bot._existingRightsRequestId = 42; bot._existingRights = 7;
		auto old = RecipientPickerBridge::BotRights(&bot, &channel); CHECK(old());
		channel.hidden = true; Changed(); CHECK(!old()); CHECK(bot._existingRightsRequestId == 0);
		CHECK(!bot._existingRightsChannel); CHECK(!bot._existingRights); CHECK(session.cancelled.back() == 42);
		channel.hidden = false; Changed(); CHECK(!old());
		bot._existingRightsChannel = &channel; auto first = RecipientPickerBridge::BotRights(&bot, &channel);
		bot._existingRightsChannel = &next; bot._existingRightsRequestId = 43;
		auto newer = RecipientPickerBridge::BotRights(&bot, &next);
		channel.hidden = true; Changed(); CHECK(!first()); CHECK(newer()); CHECK(bot._existingRightsRequestId == 43);
		CHECK(bot._existingRightsChannel == &next);
	}
	CHECK(BotRequests.empty());
	{
		auto chat = ChatData(); chat.id.value = 7; chat.owner = &session; auto bot = UserData();
		auto lifetime = rpl::lifetime(); auto current = PickerAction(&chat, lifetime);
		SaveChatAdmin({}, &chat, &bot, true, nullptr, nullptr, true, current);
		CHECK(AdminRequests.size() == 1);
		chat.hidden = true; Changed();
		AdminRequests.back().failCallback({u"USER_NOT_PARTICIPANT"_q});
		CHECK(Joins.empty()); CHECK(AdminRequests.size() == 1);
		chat.hidden = false; current = PickerAction(&chat, lifetime);
		SaveChatAdmin({}, &chat, &bot, true, nullptr, nullptr, true, current);
		AdminRequests.back().failCallback({u"USER_NOT_PARTICIPANT"_q}); CHECK(Joins.size() == 1);
		chat.hidden = true; Changed(); Joins.back()(); CHECK(AdminRequests.size() == 2);
		chat.hidden = false; Joins.back()(); CHECK(AdminRequests.size() == 2);
	}

	{
		auto thread = Data::Thread{&a}; auto forum = Forum{&thread}; auto topic = ChooseTopicBoxController(); topic._forum = &forum;
		topic._filter = PickerFilter<Data::Thread>();
		auto calls = 0; topic._callback = [&](auto) { ++calls; };
		auto row = ChooseTopicBoxController::Row(&a); row.value = &thread;
		a.hidden = true; topic.rowClicked(&row); CHECK(calls == 0);
		a.hidden = false; topic.rowClicked(&row); CHECK(calls == 1);
		row.rowId = 0; a.id.value = 0; a.hidden = true; topic.rowClicked(&row); CHECK(calls == 1); a.id.value = 1;
		topic._filter = nullptr; topic.rowClicked(&row); CHECK(calls == 2);
	}
	{
		auto sublist = Data::SavedSublist{{&a}}; auto mono = Monoforum{&sublist}; auto choose = ChooseSublistBoxController(); choose._monoforum = &mono;
		choose._filter = PickerFilter<Data::SavedSublist>(); auto calls = 0; choose._callback = [&](auto) { ++calls; }; auto row = PeerListRow(&a);
		a.hidden = true; choose.rowClicked(&row); CHECK(calls == 0);
		a.hidden = false; choose.rowClicked(&row); CHECK(calls == 1);
	}
	{
		auto calls = 0; auto stars = StarController(); auto credits = CreditController(); auto frequent = FrequentController();
		stars._pick = PickerCallback(Fn<void(not_null<PeerData*>, StarController::PickType)>([&](auto, auto) { ++calls; }));
		credits._choose = PickerCallback(Fn<void(not_null<PeerData*>)>([&](auto) { ++calls; }));
		frequent._chosen = credits._choose;
		auto row = PeerListRow(&a);
		a.hidden = true; frequent.rowClicked(&row); stars.rowClicked(&row); credits.rowClicked(&row); CHECK(calls == 0);
		for (const auto action : {StarController::PickType::Activate, StarController::PickType::SendMessage, StarController::PickType::OpenProfile}) stars._pick(&a, action);
		CHECK(calls == 0);
		a.hidden = false; stars.rowClicked(&row); credits.rowClicked(&row); CHECK(calls == 2);
		auto lifetime = rpl::lifetime(); auto revoked = 0;
		auto ready = PickerAction(&a, lifetime, [&] { ++revoked; }); CHECK(PickerActionCurrent(ready, &a));
		a.hidden = true; Changed(); CHECK(!PickerActionCurrent(ready, &a)); CHECK(revoked == 1);
		a.hidden = false; Changed(); CHECK(!PickerActionCurrent(ready, &a));
		auto box = WatchPickerBox(object_ptr<Ui::GenericBox>(new Ui::GenericBox()), &a);
		a.hidden = true; Changed(); CHECK(box->closed > 0);
		CHECK(revoked == 1);
		CHECK(PickerActionCurrent({}, &a));
	}
	{
		auto thread = Data::Thread{&a}; auto box = Ui::BoxContent();
		auto community = std::make_unique<CommunityCallback>(); auto calls = 0;
		community->_callback = [&](auto) { ++calls; };
		community->_filter = PickerFilter<Data::Thread>();
		auto callback = community->prepare(&box);
		a.hidden = true; callback(&thread); CHECK(calls == 0); CHECK(box.closed == 1);
		a.hidden = false; callback(&thread); CHECK(calls == 1);
		community->_filter = [](auto) { return false; }; callback(&thread); CHECK(calls == 1);
		community.reset(); callback(&thread); CHECK(calls == 1);
	}
	{
		auto navigation = DropNavigation(); auto box = Ui::BoxContent(); auto calls = 0;
		auto topic = DropTopic(&navigation, &box, [&] { ++calls; });
		auto sublist = DropSublist(&navigation, &box, [&] { ++calls; });
		auto thread = Data::Thread{&a}; auto saved = Data::SavedSublist{{&a}};
		a.hidden = true; topic(&thread); sublist(&saved); CHECK(navigation.value.dispatches == 0); CHECK(calls == 0);
		a.hidden = false; topic(&thread); sublist(&saved); CHECK(navigation.value.dispatches == 2); CHECK(calls == 2); CHECK(box.closed == 2);
	}
	for (const auto peer : raw) { CHECK(peer->rating == 99); CHECK(peer->pin == 3); CHECK(peer->contact); }
	CHECK(RowPickers.empty());
}
int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	Cases();
	std::printf("picker routes %d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
