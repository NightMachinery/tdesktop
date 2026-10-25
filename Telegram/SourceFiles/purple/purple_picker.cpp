#include "purple/purple_picker.h"

#include "apiwrap.h"
#include "chat_helpers/compose/compose_show.h"
#include "chat_helpers/tabbed_selector.h"
#include "menu/menu_send_details.h"
#include "payments/payments_form.h"
#include "base/weak_ptr.h"
#include "boxes/peer_list_controllers.h"
#include "boxes/peers/add_bot_to_chat_box.h"
#include "crl/crl_on_main.h"
#include "data/data_channel.h"
#include "data/data_file_origin.h"
#include "data/data_user.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/popup_menu.h"
#include <QtCore/QCoreApplication>

namespace Purple {
namespace {

class RecipientGiftShow final : public ChatHelpers::Show {
public:
	RecipientGiftShow(
		std::shared_ptr<ChatHelpers::Show> show,
		not_null<PeerData*> peer)
	: _show(std::move(show))
	, _peer(peer)
	, _current(PickerAction(peer, _lifetime)) {
	}

	void activate() override {
		if (valid()) {
			_show->activate();
		}
	}
	Main::Session &session() const override {
		return _show->session();
	}
	bool paused(ChatHelpers::PauseReason reason) const override {
		return _show->paused(reason);
	}
	rpl::producer<> pauseChanged() const override {
		return _show->pauseChanged();
	}
	rpl::producer<bool> adjustShadowLeft() const override {
		return _show->adjustShadowLeft();
	}
	SendMenu::Details sendMenuDetails() const override {
		return _show->sendMenuDetails();
	}
	bool showMediaPreview(
			Data::FileOrigin origin,
			not_null<DocumentData*> document) const override {
		return valid() && _show->showMediaPreview(std::move(origin), document);
	}
	bool showMediaPreview(
			Data::FileOrigin origin,
			not_null<PhotoData*> photo) const override {
		return valid() && _show->showMediaPreview(std::move(origin), photo);
	}
	void processChosenSticker(ChatHelpers::FileChosen &&chosen) const override {
		if (valid()) {
			_show->processChosenSticker(std::move(chosen));
		}
	}
	Window::SessionController *resolveWindow() const override {
		return _show->resolveWindow();
	}
	not_null<QWidget*> toastParent() const override {
		return _show->toastParent();
	}
	bool valid() const override {
		return _show->valid() && _current();
	}
	operator bool() const override {
		return valid();
	}
	void showOrHideBoxOrLayer(
			std::variant<
				v::null_t,
				object_ptr<Ui::BoxContent>,
				std::unique_ptr<Ui::LayerWidget>> &&layer,
			Ui::LayerOptions options,
			anim::type animated) const override {
		if (!valid()) {
			return;
		}
		if (const auto box = std::get_if<object_ptr<Ui::BoxContent>>(&layer)
			; box && *box) {
			WatchPickerChild(box->data(), _peer);
		}
		_show->showOrHideBoxOrLayer(std::move(layer), options, animated);
	}

private:
	std::shared_ptr<ChatHelpers::Show> _show;
	not_null<PeerData*> _peer;
	rpl::lifetime _lifetime;
	std::function<bool()> _current;

};

struct RecipientState {
	std::function<ChatsListBoxController*()> current;
	bool scheduled = false;
	bool rebuilding = false;
	FilterId filter = 0;
	rpl::event_stream<> selection;
};

std::map<ChatsListBoxController*, std::shared_ptr<RecipientState>> Recipients;

struct RowsState {
	PeerListController *controller = nullptr;
	Fn<void()> refill;
	Fn<void()> refreshSearch;
	std::vector<not_null<PeerData*>> candidates;
	bool alive = true;
	bool refreshing = false;
	bool scheduled = false;
};
std::map<PeerListController*, std::shared_ptr<RowsState>> RowPickers;
struct RightsState {
	bool revoked = false;
	rpl::lifetime lifetime;
};
std::map<AddBotToGroupBoxController*, std::shared_ptr<RightsState>> BotRequests;

void RefreshRows(const std::shared_ptr<RowsState> &state) {
	if (!state->alive || state->refreshing) {
		return;
	}
	state->refreshing = true;
	const auto release = gsl::finally([state] { state->refreshing = false; });
	const auto delegate = state->controller->delegate();
	auto ids = std::set<PeerListRowId>();
	for (const auto search : {false, true}) {
		const auto count = search ? delegate->peerListSearchRowsCount() : delegate->peerListFullRowsCount();
		for (auto index = 0; index < count; ++index) {
			const auto row = search ? delegate->peerListSearchRowAt(index) : delegate->peerListRowAt(index);
			if (state->refill || !PickerVisible(row->peer())) {
				ids.insert(row->id());
			}
		}
	}
	for (const auto id : ids) {
		if (const auto row = delegate->peerListFindRow(id)) {
			if (delegate->peerListIsRowChecked(row)) {
				delegate->peerListSetRowChecked(row, false);
			}
			if (!state->alive) {
				return;
			}
			if (const auto live = delegate->peerListFindRow(id)) {
				delegate->peerListRemoveRow(live);
			}
			if (!state->alive) {
				return;
			}
		}
	}
	if (state->refill) {
		state->refill();
	} else {
		const auto candidates = state->candidates;
		for (const auto peer : candidates) {
			if (PickerVisible(peer) && !delegate->peerListFindRow(peer->id.value)) {
				delegate->peerListAppendRow(std::make_unique<PeerListRow>(peer));
				if (!state->alive) {
					return;
				}
			}
		}
		const auto order = state->candidates;
		delegate->peerListSortRows([&](const PeerListRow &a, const PeerListRow &b) {
			return ranges::find(order, a.peer()) < ranges::find(order, b.peer());
		});
	}
	if (state->alive) {
		delegate->peerListRefreshRows();
	}
}

}

bool PickerVisible(not_null<History*> history) {
	return !history->purpleHiddenFromChatList();
}

bool PickerVisible(not_null<PeerData*> peer) {
	return PickerVisible(peer->owner().history(peer));
}

std::function<bool()> PickerAction(
		not_null<PeerData*> peer,
		rpl::lifetime &lifetime,
		Fn<void()> revoked) {
	const auto active = std::make_shared<bool>(true);
	const auto session = base::make_weak(&peer->session());
	ActiveChanges() | rpl::on_next([=] {
		if (*active && (!session || !PickerVisible(peer))) {
			*active = false;
			if (revoked) {
				revoked();
			}
		}
	}, lifetime);
	lifetime.add([active] { *active = false; });
	return [=] { return *active && session && PickerVisible(peer); };
}

bool PickerActionCurrent(
		const std::function<bool()> &current,
		not_null<PeerData*> peer) {
	return !current || (current() && PickerVisible(peer));
}

object_ptr<Ui::GenericBox> PickerConfirmBox(
		not_null<PeerData*> peer,
		Ui::ConfirmBoxArgs &&args) {
	const auto current = std::make_shared<std::function<bool()>>();
	args.confirmed = [current, callback = std::move(args.confirmed)](Fn<void()> close) {
		if (!*current || !(*current)()) {
			close();
			return;
		}
		if (const auto plain = std::get_if<1>(&callback)) {
			if (const auto invoke = *plain) {
				invoke();
			}
		} else if (const auto withClose = std::get_if<2>(&callback)) {
			if (const auto invoke = *withClose) {
				invoke(std::move(close));
			}
		} else {
			close();
		}
	};
	auto result = Ui::MakeConfirmBox(std::move(args));
	*current = PickerAction(peer, result->lifetime());
	WatchPickerChild(result.data(), peer);
	return result;
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

void RecipientPickerBridge::WatchRows(
		not_null<PeerListController*> controller,
		Fn<void()> refill) {
	if (RowPickers.contains(controller)) {
		return;
	}
	const auto state = std::make_shared<RowsState>();
	state->controller = controller;
	state->refill = std::move(refill);
	RowPickers.emplace(controller, state);
	controller->lifetime().add([=] {
		state->alive = false;
		RowPickers.erase(controller);
	});
	ActiveChanges() | rpl::on_next([=] {
		RefreshRows(state);
		if (state->alive && !state->scheduled) {
			state->scheduled = true;
			QTimer::singleShot(0, QCoreApplication::instance(), [=] {
				state->scheduled = false;
				RefreshRows(state);
				if (state->alive && state->refreshSearch) {
					state->refreshSearch();
				}
			});
		}
	}, controller->lifetime());
}

void RecipientPickerBridge::WatchContacts(
		not_null<ContactsBoxController*> controller) {
	WatchRows(controller, [=] { controller->rebuildRows(); });
	RowPickers.at(controller)->refreshSearch = [=] {
		if (controller->searchController()) {
			static_cast<PeerListGlobalSearchController*>(controller->searchController())->searchInCache();
		}
	};
}

bool RecipientPickerBridge::RowAllows(
		not_null<PeerListContent*> content,
		not_null<PeerListRow*> row) {
	const auto i = RowPickers.find(content->_controller);
	if (i == RowPickers.end()) {
		return true;
	}
	auto &candidates = i->second->candidates;
	const auto peer = row->peer();
	if (!ranges::contains(candidates, peer)) {
		candidates.push_back(peer);
	}
	return PickerVisible(peer);
}

std::function<bool()> RecipientPickerBridge::BotRights(
		not_null<AddBotToGroupBoxController*> controller,
		not_null<ChannelData*> channel) {
	if (!BotRequests.contains(controller)) {
		controller->lifetime().add([=] { BotRequests.erase(controller); });
	}
	const auto state = std::make_shared<RightsState>();
	BotRequests[controller] = state;
	const auto weak = base::make_weak(controller.get());
	ActiveChanges() | rpl::on_next([weak, state = std::weak_ptr(state), channel] {
		const auto active = state.lock();
		if (!weak || !active || active->revoked
			|| BotRequests[weak.get()] != active
			|| PickerVisible(channel)) {
			return;
		}
		active->revoked = true;
		weak->session().api().request(base::take(weak->_existingRightsRequestId)).cancel();
		weak->_existingRightsChannel = nullptr;
		weak->_existingRights.reset();
	}, state->lifetime);
	return [weak, state, channel] {
		return weak && !state->revoked && BotRequests[weak.get()] == state
			&& weak->_existingRightsChannel == channel && PickerVisible(channel);
	};
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
	for (const auto &[controller, state] : RowPickers) {
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
	if (RowPickers.contains(content->_controller) && content->_contextMenu) {
		content->_contextMenu->setDestroyedCallback(nullptr);
		content->_contextMenu = nullptr;
		return;
	}
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

namespace Purple {
std::shared_ptr<ChatHelpers::Show> PickerGiftShow(std::shared_ptr<ChatHelpers::Show> show, not_null<PeerData*> peer) {
	return std::make_shared<RecipientGiftShow>(std::move(show), peer);
}
std::function<bool()> PickerGiftCurrent(std::shared_ptr<Main::SessionShow> show) {
	if (!dynamic_cast<RecipientGiftShow*>(show.get())) {
		return {};
	}
	return [show = std::move(show)] { return show->valid(); };
}
bool PickerPaymentAllowed(const Payments::InvoiceId &id) {
	return std::visit([](const auto &invoice) {
		if constexpr (requires { invoice.pickerCurrent; }) {
			return !invoice.pickerCurrent || invoice.pickerCurrent();
		} else {
			return true;
		}
	}, id.value);
}
void WatchPickerPayment(const Payments::InvoiceId &id, Fn<void()> close, rpl::lifetime &lifetime) {
	ActiveChanges() | rpl::on_next([=] {
		if (!PickerPaymentAllowed(id)) close();
	}, lifetime);
}
}
