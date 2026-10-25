#pragma once

#include "purple/purple_gate.h"
#include "base/weak_ptr.h"
#include "data/data_chat_filters.h"
#include "data/data_community.h"
#include "data/data_folder.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "history/history.h"
#include "main/main_session.h"
#include "ui/text/text_utilities.h"
#include <QtCore/QTimer>
#include <map>
#include <memory>
#include <set>

class ChatsListBoxController;
class ContactsBoxController;
class AddBotToGroupBoxController;
class PeerListController;
class ChannelData;
class ChooseRecipientBoxController;
class PeerListSearchDelegate;
class PeerListDelegate;
class PeerListContent;
class PeerListRow;
namespace Ui {
class BoxContent;
class GenericBox;
struct ConfirmBoxArgs;
}

namespace ChatHelpers { class Show; }
namespace Main { class SessionShow; }
namespace Payments { struct InvoiceId; }

namespace Purple {

[[nodiscard]] std::shared_ptr<ChatHelpers::Show> PickerGiftShow(
	std::shared_ptr<ChatHelpers::Show> show, not_null<PeerData*> peer);
[[nodiscard]] std::function<bool()> PickerGiftCurrent(
	std::shared_ptr<Main::SessionShow> show);
[[nodiscard]] bool PickerPaymentAllowed(const Payments::InvoiceId &id);
void WatchPickerPayment(const Payments::InvoiceId &id, Fn<void()> close, rpl::lifetime &lifetime);
[[nodiscard]] bool PickerVisible(not_null<History*> history);
[[nodiscard]] bool PickerVisible(not_null<PeerData*> peer);
[[nodiscard]] std::vector<not_null<History*>> PickerHistories(not_null<Main::Session*> session, FilterId filter = 0);
void WatchPickerChild(not_null<Ui::BoxContent*> box, not_null<PeerData*> peer);
[[nodiscard]] std::function<bool()> PickerAction(
	not_null<PeerData*> peer,
	rpl::lifetime &lifetime,
	Fn<void()> revoked = nullptr);
[[nodiscard]] object_ptr<Ui::GenericBox> PickerConfirmBox(
	not_null<PeerData*> peer,
	Ui::ConfirmBoxArgs &&args);
[[nodiscard]] bool PickerActionCurrent(
	const std::function<bool()> &current,
	not_null<PeerData*> peer);

template <typename Box>
[[nodiscard]] object_ptr<Box> WatchPickerBox(
		object_ptr<Box> box,
		not_null<PeerData*> peer,
		bool enabled = true) {
	if (enabled) {
		WatchPickerChild(box.data(), peer);
	}
	return box;
}

template <typename... Args>
[[nodiscard]] Fn<void(not_null<PeerData*>, Args...)> PickerCallback(Fn<void(not_null<PeerData*>, Args...)> callback) {
	return [callback = std::move(callback)](not_null<PeerData*> peer, Args... args) {
		if (PickerVisible(peer)) {
			callback(peer, std::forward<Args>(args)...);
		}
	};
}

template <typename Thread>
[[nodiscard]] Fn<bool(not_null<Thread*>)> PickerFilter(Fn<bool(not_null<Thread*>)> filter = nullptr) {
	return [filter = std::move(filter)](not_null<Thread*> thread) {
		return PickerVisible(thread->peer()) && (!filter || filter(thread));
	};
}

template <typename Thread>
[[nodiscard]] bool PickerChildAllowed(
		const Fn<bool(not_null<Thread*>)> &filter,
		not_null<Thread*> thread) {
	return !filter || filter(thread);
}

template <typename T, typename Visible>
[[nodiscard]] std::vector<T> PickerProjection(
		const std::vector<std::vector<T>> &buckets,
		Visible visible) {
	auto result = std::vector<T>();
	auto seen = std::set<const void*>();
	for (const auto &bucket : buckets) {
		for (const auto &entry : bucket) {
			if (visible(entry) && seen.emplace(std::to_address(entry)).second) {
				result.push_back(entry);
			}
		}
	}
	return result;
}

template <typename Selected, typename Count>
void PickerDeselected(Selected &selected, Count &count, not_null<PeerData*> peer) {
	selected.remove(peer);
	count = int(selected.size());
}

class RecipientPickerBridge final {
public:
	template <typename Controller>
	static void Watch(Controller *controller) {
		Watch(controller, [weak = base::make_weak(controller)] {
			return static_cast<ChatsListBoxController*>(weak.get());
		});
	}
	static void WatchRows(
		not_null<PeerListController*> controller,
		Fn<void()> refill = nullptr);
	static void WatchContacts(not_null<ContactsBoxController*> controller);
	[[nodiscard]] static bool RowAllows(
		not_null<PeerListContent*> content,
		not_null<PeerListRow*> row);
	[[nodiscard]] static std::function<bool()> BotRights(
		not_null<AddBotToGroupBoxController*> controller,
		not_null<ChannelData*> channel);
	[[nodiscard]] static bool Rebuild(not_null<ChatsListBoxController*> controller);
	[[nodiscard]] static bool Allows(not_null<ChatsListBoxController*> controller,
		not_null<History*> history);
	[[nodiscard]] static bool SearchAllows(not_null<PeerListSearchDelegate*> delegate,
		not_null<PeerData*> peer);
	static void Filter(not_null<PeerListDelegate*> delegate, FilterId id);
	static void BeforeRemove(not_null<PeerListContent*> content);
	[[nodiscard]] static rpl::producer<> SelectionChanges(
		not_null<const ChatsListBoxController*> controller, rpl::producer<> changes);

private:
	static void Watch(not_null<ChatsListBoxController*> controller,
		std::function<ChatsListBoxController*()> current);

};

template <typename Inner>
class SharePickerBridge final {
public:
	static void Watch(not_null<Inner*> inner) {
		const auto state = inner->lifetime().template make_state<SharePickerBridge>(inner);
		Instances.emplace(inner, state);
		inner->lifetime().add([inner] { Instances.erase(inner); });
		ActiveChanges() | rpl::on_next([=] {
			state->refresh();
			if (!state->_scheduled) {
				state->_scheduled = true;
				QTimer::singleShot(0, inner, [=] {
					state->_scheduled = false;
					state->refresh();
				});
			}
		}, inner->lifetime());
	}
	static void Filter(not_null<Inner*> inner, FilterId id) {
		if (const auto i = Instances.find(inner); i != Instances.end()) {
			i->second->_filterId = id;
		}
	}
	explicit SharePickerBridge(not_null<Inner*> inner) : _inner(inner) {
	}

private:
	void refresh() {
		if (_refreshing) {
			return;
		}
		_refreshing = true;
		const auto release = gsl::finally([&] { _refreshing = false; });
		const auto inner = _inner;
		for (const auto &[peer, chat] : inner->_dataMap) {
			if (chat->checkbox.checked() && !PickerVisible(chat->history)) {
				inner->changePeerCheckState(chat.get(), false);
			}
		}
		inner->setActive(-1);
		inner->_upon = -1;
		inner->_filtered.clear();
		inner->_byUsernameFiltered.clear();
		inner->d_byUsernameFiltered.clear();
		inner->_defaultChatsIndexed->clear();
		const auto self = inner->_descriptor.session->data().history(inner->_descriptor.session->user());
		if (PickerVisible(self) && inner->_descriptor.filterCallback(self)) {
			inner->_defaultChatsIndexed->addToEnd(self);
		}
		for (const auto history : PickerHistories(inner->_descriptor.session)) {
			if (!history->peer->isSelf() && (history->asForum()
				|| JoinedCommunityChats(history->peer) || inner->_descriptor.filterCallback(history))) {
				inner->_defaultChatsIndexed->addToEnd(history);
			}
		}
		inner->applyChatFilter(_filterId);
		if (!inner->_filter.isEmpty()) {
			inner->_filtered = inner->_chatsIndexed->filtered(TextUtilities::PrepareSearchWords(inner->_lastQuery));
			inner->_searching = true;
			inner->_searchRequests.fire({});
		}
		inner->refresh();
	}

	const not_null<Inner*> _inner;
	FilterId _filterId = 0;
	bool _scheduled = false;
	bool _refreshing = false;
	inline static std::map<Inner*, SharePickerBridge*> Instances;

};

}
