#include "base/basic_types.h"
#include "base/flat_set.h"
#include "purple/purple_engine.h"
#include "purple/purple_state.h"
#include "rpl/rpl.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>
#include <QtWidgets/QWidget>
#include <QtWidgets/QApplication>
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

using namespace Purple;
using FilterId = int;
using PeerListRowId = uint64_t;
auto Checks = 0;
auto Failures = 0;
#define CHECK(value) do { ++Checks; if (!(value)) { ++Failures; std::printf("FAIL picker:%d %s\n", __LINE__, #value); } } while (false)

struct PeerData {
	struct { uint64_t value = 0; } id;
	QString name;
	bool self = false;
	int rating = 99;
	int pin = 3;
	bool contact = true;
	bool isSelf() const { return self; }
};

struct History {
	PeerData *peer = nullptr;
	bool hidden = false;
	bool asForum() const { return false; }
	bool purpleHiddenFromChatList() const { return hidden; }
};

bool JoinedCommunityChats(PeerData*) { return false; }

namespace TextUtilities {
QStringList PrepareSearchWords(const QString &text) {
	return text.simplified().split(' ', Qt::SkipEmptyParts);
}
}

struct Index {
	struct Row {
		History *value = nullptr;
		History *history() const { return value; }
	};
	std::vector<std::unique_ptr<Row>> rows;
	void clear() { rows.clear(); }
	void addToEnd(History *history) {
		if (std::none_of(rows.begin(), rows.end(), [&](const auto &row) { return row->value == history; })) {
			rows.push_back(std::make_unique<Row>(Row{history}));
		}
	}
	std::vector<Row*> filtered(const QStringList &words) {
		auto result = std::vector<Row*>();
		for (const auto &row : rows) {
			if (std::all_of(words.begin(), words.end(), [&](const auto &word) {
				return row->value->peer->name.contains(word, Qt::CaseInsensitive);
			})) {
				result.push_back(row.get());
			}
		}
		return result;
	}
};

struct Session {
	std::vector<std::vector<History*>> buckets;
	std::vector<History*> filter;
	History *self = nullptr;
	Session &data() { return *this; }
	PeerData *user() { return self->peer; }
	History *history(PeerData *peer) { CHECK(peer == self->peer); return self; }
};

class PeerListRow {
public:
	explicit PeerListRow(History *history) : value(history) {}
	PeerData *peer() const { return value->peer; }
	PeerListRowId id() const { return peer()->id.value; }
	History *value = nullptr;
	bool checked = false;

};

class PeerListSearchDelegate { public: virtual ~PeerListSearchDelegate() = default; };
class PeerListDelegate {};

struct Delegate : PeerListDelegate {
	std::vector<std::unique_ptr<PeerListRow>> rows;
	std::vector<std::unique_ptr<PeerListRow>> search;
	int refreshed = 0;
	int peerListFullRowsCount() { return rows.size(); }
	int peerListSearchRowsCount() { return search.size(); }
	PeerListRow *peerListRowAt(int i) { return rows.at(i).get(); }
	PeerListRow *peerListSearchRowAt(int i) { return search.at(i).get(); }
	PeerListRow *peerListFindRow(PeerListRowId id) {
		for (const auto list : {&rows, &search}) {
			for (const auto &row : *list) { if (row->id() == id) return row.get(); }
		}
		return nullptr;
	}
	bool peerListIsRowChecked(PeerListRow *row) { return row->checked; }
	void peerListSetRowChecked(PeerListRow *row, bool checked) { row->checked = checked; }
	void peerListRemoveRow(PeerListRow *row) {
		CHECK(!row->checked);
		for (const auto list : {&rows, &search}) {
			std::erase_if(*list, [&](const auto &entry) { return entry.get() == row; });
		}
	}
	void peerListSortRows(std::function<bool(const PeerListRow&, const PeerListRow&)> compare) {
		std::stable_sort(rows.begin(), rows.end(), [&](const auto &a, const auto &b) { return compare(*a, *b); });
	}
	void peerListRefreshRows() { ++refreshed; }
};

class ChatsListBoxController : public PeerListSearchDelegate {
public:
	Session *owner = nullptr;
	Delegate *view = nullptr;
	Session &session() { return *owner; }
	Delegate *delegate() { return view; }
	QString savedMessagesChatStatus() { return u"Saved"_q; }
	bool appendRow(History *history) {
		if (view->peerListFindRow(history->peer->id.value)) return false;
		view->rows.push_back(std::make_unique<PeerListRow>(history));
		return true;
	}
	void checkForEmptyRows() {}

};

using PeerListController = ChatsListBoxController;
struct TestMenu {
	bool *closed = nullptr;
	bool detached = false;
	void setDestroyedCallback(std::nullptr_t) { detached = true; }
	~TestMenu() { CHECK(detached); *closed = true; }
};
class PeerListContent {
public:
	PeerListController *_controller = nullptr;
	std::unique_ptr<TestMenu> _contextMenu;

};
namespace Ui {
class BoxContent {
public:
	int closed = 0;
	rpl::lifetime life;
	rpl::lifetime &lifetime() { return life; }
	void closeBox() { ++closed; }

};
}

namespace Purple {
rpl::event_stream<> Active;
rpl::producer<> ActiveChanges() { return Active.events(); }
class RecipientPickerBridge {
public:
	static bool Rebuild(not_null<ChatsListBoxController*> controller);
	static bool Allows(not_null<ChatsListBoxController*> controller, not_null<History*> history);
	static bool SearchAllows(not_null<PeerListSearchDelegate*> delegate, not_null<PeerData*> peer);
	static void Filter(not_null<PeerListDelegate*> delegate, FilterId id);
	static void BeforeRemove(not_null<PeerListContent*> content);
	static rpl::producer<> SelectionChanges(not_null<const ChatsListBoxController*> controller, rpl::producer<> changes);

};
struct RecipientState {
	std::function<ChatsListBoxController*()> current;
	bool rebuilding = false;
	FilterId filter = 0;
	rpl::event_stream<> selection;
};
std::map<ChatsListBoxController*, std::shared_ptr<RecipientState>> Recipients;

#include "picker-projection.inc"
#include "picker-visible.inc"

std::map<PeerData*, History*> Histories;
bool PickerVisible(not_null<PeerData*> peer) { return PickerVisible(Histories.at(peer)); }

std::vector<History*> PickerHistories(Session *session, FilterId filter = 0) {
	return PickerProjection(filter ? std::vector<std::vector<History*>>{session->filter} : session->buckets,
		[](History *history) { return PickerVisible(history); });
}

#include "picker-recipient.inc"
#include "picker-share.inc"
}

struct Inner final : QWidget {
	struct Checkbox { bool value = false; bool checked() const { return value; } };
	struct Chat { History *history = nullptr; Checkbox checkbox; };
	struct Descriptor { Session *session = nullptr; std::function<bool(History*)> filterCallback; } _descriptor;
	std::unique_ptr<Index> _defaultChatsIndexed = std::make_unique<Index>();
	std::unique_ptr<Index> custom;
	Index *_chatsIndexed = _defaultChatsIndexed.get();
	std::vector<Index::Row*> _filtered;
	std::vector<PeerData*> _byUsernameFiltered;
	std::vector<std::unique_ptr<Chat>> d_byUsernameFiltered;
	std::map<PeerData*, std::unique_ptr<Chat>> _dataMap;
	rpl::event_stream<> _searchRequests;
	QString _lastQuery;
	QString _filter;
	bool _searching = false;
	int _upon = -1;
	int active = -1;
	int refreshed = 0;
	int deselections = 0;
	rpl::lifetime life;
	rpl::lifetime &lifetime() { return life; }
	void changePeerCheckState(Chat *chat, bool value) { chat->checkbox.value = value; ++deselections; }
	void setActive(int value) { active = value; }
	void applyChatFilter(FilterId id) {
		SharePickerBridge<Inner>::Filter(this, id);
		if (id) {
			custom = std::make_unique<Index>();
			for (const auto history : _descriptor.session->filter) {
				if (PickerVisible(history)) custom->addToEnd(history);
			}
			_chatsIndexed = custom.get();
		} else {
			_chatsIndexed = _defaultChatsIndexed.get();
		}
	}
	void refresh() { ++refreshed; }
};

std::vector<uint64_t> Ids(const Index &index) {
	auto result = std::vector<uint64_t>();
	for (const auto &row : index.rows) result.push_back(row->history()->peer->id.value);
	return result;
}

void Cases() {
	auto peers = std::array<PeerData, 6>{{{{1}, u"Saved"_q, true}, {{2}, u"Person"_q},
		{{3}, u"Alice"_q}, {{4}, u"Bob"_q}, {{5}, u"Archived"_q}, {{6}, u"Contact"_q}}};
	auto histories = std::array<History, 6>();
	for (auto i = 0; i != 6; ++i) { histories[i].peer = &peers[i]; Histories.emplace(&peers[i], &histories[i]); }
	auto &person = histories[1];
	auto selected = base::flat_set<not_null<PeerData*>>{ person.peer, histories[2].peer };
	auto selectedCount = rpl::variable<int>(2);
	PickerDeselected(selected, selectedCount, person.peer);
	CHECK(!selected.contains(person.peer));
	CHECK(selected.contains(histories[2].peer));
	CHECK(selectedCount.current() == 1);
	auto session = Session{{{&histories[2], &histories[3]}, {&histories[4]}, {&person, &histories[5]}},
		{&person, &histories[2], &histories[5]}, &histories[0]};
	const auto raw = session.buckets;
	auto inner = Inner();
	inner._descriptor = {&session, [](History*) { return true; }};
	for (const auto history : {&person, &histories[2]}) {
		auto chat = std::make_unique<Inner::Chat>(); chat->history = history; chat->checkbox.value = true;
		inner._dataMap.emplace(history->peer, std::move(chat));
	}
	SharePickerBridge<Inner>::Watch(&inner);
	const auto settings = ParseSettings(u"version=1\n[lists.hidden]\nmembers=[2]\n[lists.all]\nkinds=['private']\n[presets.away]\nhide_everywhere_p=true\nlist_order=[{list='hidden',show_mode='never'},{list='all',show_mode='always'}]\n"_q, QString());
	CHECK(settings.ok());
	const auto away = Resolve(settings.settings, u"away"_q);
	CHECK(away && away->hideEverywhere);
	if (!away || !settings.ok()) return;
	const auto visibility = Visible(settings.settings, *away, 2, ChatKind::Private);
	CHECK(visibility.show == ShowMode::Never);
	person.hidden = away->hideEverywhere && visibility.show == ShowMode::Never;
	Active.fire({});
	CHECK(Ids(*inner._defaultChatsIndexed) == (std::vector<uint64_t>{1, 3, 4, 5, 6}));
	CHECK(!inner._dataMap.at(person.peer)->checkbox.checked());
	CHECK(inner._dataMap.at(histories[2].peer)->checkbox.checked());
	CHECK(inner.deselections == 1);
	inner.applyChatFilter(1);
	inner._filter = inner._lastQuery = u"Person"_q;
	Active.fire({});
	CHECK(Ids(*inner._chatsIndexed) == (std::vector<uint64_t>{3, 6}));
	CHECK(inner._filtered.empty());
	CHECK(!PickerVisible(&person));
	auto delegate = Delegate();
	auto controller = ChatsListBoxController(); controller.owner = &session; controller.view = &delegate;
	delegate.rows.push_back(std::make_unique<PeerListRow>(&person)); delegate.rows.back()->checked = true;
	delegate.rows.push_back(std::make_unique<PeerListRow>(&histories[2])); delegate.rows.back()->checked = true;
	auto state = std::make_shared<RecipientState>(); state->current = [&] { return &controller; };
	Recipients.emplace(&controller, state);
	auto notifications = 0;
	auto notified = RecipientPickerBridge::SelectionChanges(&controller, rpl::never<>())
		| rpl::on_next([&] { ++notifications; });
	auto menuClosed = false;
	auto content = PeerListContent(); content._controller = &controller;
	content._contextMenu = std::make_unique<TestMenu>(); content._contextMenu->closed = &menuClosed;
	RecipientPickerBridge::BeforeRemove(&content);
	CHECK(menuClosed && !content._contextMenu);
	auto child = Ui::BoxContent();
	WatchPickerChild(&child, person.peer);
	Active.fire({});
	CHECK(child.closed == 1);
	CHECK(RecipientPickerBridge::Rebuild(&controller));
	CHECK(notifications == 1);
	CHECK(!delegate.peerListFindRow(2));
	CHECK(delegate.peerListFindRow(3)->checked);
	CHECK(!RecipientPickerBridge::SearchAllows(&controller, person.peer));
	CHECK(!RecipientPickerBridge::Allows(&controller, &person));
	auto peek = State();
	CHECK(StartPeek(peek, 100, 60));
	person.hidden = away->hideEverywhere && visibility.show == ShowMode::Never && !PeekLive(peek, 100);
	CHECK(!person.hidden);
	Active.fire({});
	session.buckets = {{&person, &histories[2], &histories[3]}, {&histories[4]}, {&histories[5]}};
	QCoreApplication::processEvents();
	inner.applyChatFilter(0);
	Active.fire({});
	QCoreApplication::processEvents();
	CHECK(Ids(*inner._defaultChatsIndexed) == (std::vector<uint64_t>{1, 2, 3, 4, 5, 6}));
	CHECK(RecipientPickerBridge::Rebuild(&controller));
	CHECK(delegate.rows[1]->id() == 2);
	CHECK(delegate.peerListFindRow(3)->checked);
	CHECK(RecipientPickerBridge::SearchAllows(&controller, person.peer));
	CHECK(RecipientPickerBridge::Allows(&controller, &person));
	RecipientPickerBridge::Filter(&delegate, 1);
	CHECK(state->filter == 1);
	CHECK(RecipientPickerBridge::Rebuild(&controller));
	CHECK(delegate.rows.front()->id() == 2);
	CHECK(!delegate.peerListFindRow(4));
	CHECK(!delegate.peerListFindRow(5));
	CHECK(delegate.peerListFindRow(3)->checked);
	RecipientPickerBridge::Filter(&delegate, 0);
	CHECK(RecipientPickerBridge::Rebuild(&controller));
	CHECK(delegate.rows[1]->id() == 2);
	auto viewOnly = *away;
	viewOnly.hideEverywhere = false;
	CHECK(Visible(settings.settings, viewOnly, 2, ChatKind::Private).show == ShowMode::Never);
	person.hidden = viewOnly.hideEverywhere && visibility.show == ShowMode::Never;
	CHECK(PickerVisible(&person));
	Active.fire({});
	CHECK(Ids(*inner._defaultChatsIndexed) == (std::vector<uint64_t>{1, 2, 3, 4, 5, 6}));
	CHECK(!inner._dataMap.at(person.peer)->checkbox.checked());
	auto remote = PeerData{{7}, u"Remote search result"_q};
	auto remoteHistory = History{&remote, true};
	Histories.emplace(&remote, &remoteHistory);
	delegate.search.push_back(std::make_unique<PeerListRow>(&remoteHistory));
	CHECK(RecipientPickerBridge::Rebuild(&controller));
	CHECK(delegate.search.empty());
	CHECK(!RecipientPickerBridge::SearchAllows(&controller, &remote));
	CHECK(delegate.peerListFindRow(3)->checked);
	StopPeek(peek);
	CHECK(!peek.peekActive);
	person.hidden = true;
	session.buckets = raw;
	Active.fire({});
	CHECK(RecipientPickerBridge::Rebuild(&controller));
	CHECK(!delegate.peerListFindRow(2));
	const auto normal = Resolve(settings.settings, NormalPreset());
	CHECK(normal && normal->normal);
	person.hidden = normal && !normal->normal && visibility.show == ShowMode::Never;
	Active.fire({});
	session.buckets = {{&person, &histories[2], &histories[3]}, {&histories[4]}, {&histories[5]}};
	QCoreApplication::processEvents();
	CHECK(RecipientPickerBridge::Rebuild(&controller));
	CHECK(delegate.rows[1]->id() == 2);
	CHECK(Ids(*inner._defaultChatsIndexed) == (std::vector<uint64_t>{1, 2, 3, 4, 5, 6}));
	CHECK(delegate.peerListFindRow(3)->checked);
	for (const auto &peer : peers) CHECK(peer.rating == 99 && peer.pin == 3 && peer.contact);
	CHECK(raw[2][0] == &person);
	Recipients.erase(&controller);
	person.hidden = true;
	CHECK(RecipientPickerBridge::Allows(&controller, &person));
	CHECK(RecipientPickerBridge::SearchAllows(&controller, person.peer));
	{
		auto gone = std::make_unique<Inner>(); gone->_descriptor = inner._descriptor;
		SharePickerBridge<Inner>::Watch(gone.get());
		Active.fire({});
		gone.reset();
		const auto before = inner.refreshed;
		QCoreApplication::processEvents();
		CHECK(inner.refreshed > before);
	}
	Histories.clear();
}

int main(int argc, char **argv) {
	const auto app = QApplication(argc, argv);
	Cases();
	std::printf("picker: %d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
