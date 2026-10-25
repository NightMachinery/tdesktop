#include "purple/purple_picker.h"

#include "boxes/peer_list_controllers.h"
#include "base/weak_ptr.h"
#include "crl/crl_on_main.h"
#include "data/data_user.h"
#include "ui/widgets/popup_menu.h"

namespace Purple {
namespace {

struct RecipientState {
	std::function<ChatsListBoxController*()> current;
	bool scheduled = false;
	bool rebuilding = false;
	FilterId filter = 0;
	rpl::event_stream<> selection;
};

std::map<ChatsListBoxController*, std::shared_ptr<RecipientState>> Recipients;

}

bool PickerVisible(not_null<History*> history) {
	return !history->purpleHiddenFromChatList();
}

bool PickerVisible(not_null<PeerData*> peer) {
	return PickerVisible(peer->owner().history(peer));
}

void WatchPickerChild(not_null<Ui::BoxContent*> box, not_null<PeerData*> peer) {
	ActiveChanges() | rpl::on_next([=] {
		if (!PickerVisible(peer)) {
			box->closeBox();
		}
	}, box->lifetime());
}

std::vector<not_null<History*>> PickerHistories(not_null<Main::Session*> session, FilterId filter) {
	auto buckets = std::vector<std::vector<not_null<History*>>>(3);
	const auto append = [&](not_null<Dialogs::IndexedList*> list, int bucket) {
		for (const auto &row : list->all()) {
			if (const auto history = row->history()) {
				buckets[bucket].push_back(history);
			}
		}
	};
	if (filter) {
		append(session->data().chatsFilters().chatsList(filter)->indexed(), 0);
	} else {
		append(session->data().chatsList()->indexed(), 0);
		if (const auto archive = session->data().folderLoaded(Data::Folder::kId)) {
			append(archive->chatsList()->indexed(), 1);
		}
		append(session->data().contactsNoChatsList(), 2);
	}
	return PickerProjection(buckets, [](auto history) { return PickerVisible(history); });
}

void RecipientPickerBridge::Watch(not_null<ChatsListBoxController*> controller,
	std::function<ChatsListBoxController*()> current) {
	if (Recipients.contains(controller.get())) {
		return;
	}
	const auto state = std::make_shared<RecipientState>();
	state->current = std::move(current);
	Recipients.emplace(controller.get(), state);
	controller->lifetime().add([controller] { Recipients.erase(controller.get()); });
	ActiveChanges() | rpl::on_next([state] {
		if (const auto live = state->current()) {
			static_cast<void>(Rebuild(live));
		}
		if (!state->scheduled) {
			state->scheduled = true;
			crl::on_main([state] {
				state->scheduled = false;
				if (const auto live = state->current()) {
					static_cast<void>(Rebuild(live));
					if (state->current() && live->searchController()) {
						const auto search = static_cast<PeerListGlobalSearchController*>(live->searchController());
						static_cast<void>(search->searchInCache());
					}
				}
			});
		}
	}, controller->lifetime());
}

bool RecipientPickerBridge::Allows(not_null<ChatsListBoxController*> controller,
	not_null<History*> history) {
	return !Recipients.contains(controller.get()) || PickerVisible(history);
}

bool RecipientPickerBridge::SearchAllows(not_null<PeerListSearchDelegate*> delegate,
	not_null<PeerData*> peer) {
	for (const auto &[controller, state] : Recipients) {
		if (static_cast<PeerListSearchDelegate*>(controller) == delegate.get()) {
			return PickerVisible(peer);
		}
	}
	return true;
}

void RecipientPickerBridge::Filter(not_null<PeerListDelegate*> delegate, FilterId id) {
	for (const auto &[controller, state] : Recipients) {
		if (controller->delegate() == delegate) {
			state->filter = id;
			return;
		}
	}
}

void RecipientPickerBridge::BeforeRemove(not_null<PeerListContent*> content) {
	for (const auto &[controller, state] : Recipients) {
		if (static_cast<PeerListController*>(controller) == content->_controller && content->_contextMenu) {
			content->_contextMenu->setDestroyedCallback(nullptr);
			content->_contextMenu = nullptr;
			return;
		}
	}
}

rpl::producer<> RecipientPickerBridge::SelectionChanges(
	not_null<const ChatsListBoxController*> controller, rpl::producer<> changes) {
	const auto found = Recipients.find(const_cast<ChatsListBoxController*>(controller.get()));
	return found == Recipients.end() ? std::move(changes)
		: rpl::merge(std::move(changes), found->second->selection.events());
}

bool RecipientPickerBridge::Rebuild(not_null<ChatsListBoxController*> controller) {
	const auto found = Recipients.find(controller.get());
	if (found == Recipients.end()) {
		return false;
	}
	const auto state = found->second;
	if (state->rebuilding || !state->current()) {
		return true;
	}
	state->rebuilding = true;
	const auto release = gsl::finally([state] { state->rebuilding = false; });
	const auto delegate = controller->delegate();
	auto histories = PickerHistories(&controller->session(), state->filter);
	if (!state->filter && !controller->savedMessagesChatStatus().isEmpty()) {
		histories.insert(histories.begin(), controller->session().data().history(controller->session().user()));
	}
	auto candidates = std::set<PeerListRowId>();
	for (const auto history : histories) {
		candidates.emplace(history->peer->id.value);
	}
	auto remove = std::set<PeerListRowId>();
	for (const auto search : {false, true}) {
		const auto count = search ? delegate->peerListSearchRowsCount() : delegate->peerListFullRowsCount();
		for (auto i = count; i > 0; --i) {
			const auto row = search ? delegate->peerListSearchRowAt(i - 1) : delegate->peerListRowAt(i - 1);
			if (!PickerVisible(row->peer())
				|| (!search && !candidates.contains(row->id()) && !delegate->peerListIsRowChecked(row))) {
				remove.emplace(row->id());
			}
		}
	}
	for (const auto id : remove) {
		if (const auto row = delegate->peerListFindRow(id)) {
			if (delegate->peerListIsRowChecked(row)) {
				delegate->peerListSetRowChecked(row, false);
			}
			if (!state->current()) {
				return true;
			}
			if (const auto live = delegate->peerListFindRow(id)) {
				delegate->peerListRemoveRow(live);
			}
		}
	}
	auto order = std::map<PeerListRowId, size_t>();
	const auto add = [&](not_null<History*> history) {
		if (PickerVisible(history)) {
			order.emplace(history->peer->id.value, order.size());
			static_cast<void>(controller->appendRow(history));
		}
	};
	for (const auto history : histories) {
		add(history);
	}
	for (auto i = 0, count = delegate->peerListFullRowsCount(); i != count; ++i) {
		order.emplace(delegate->peerListRowAt(i)->id(), order.size());
	}
	delegate->peerListSortRows([order = std::move(order)](const PeerListRow &a, const PeerListRow &b) {
		return order.at(a.id()) < order.at(b.id());
	});
	controller->checkForEmptyRows();
	delegate->peerListRefreshRows();
	state->selection.fire({});
	return true;
}

}
