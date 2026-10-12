#pragma once

#include "base/basic_types.h"
#include "base/flat_map.h"
#include "base/timer.h"
#include "rpl/producer.h"

#include <optional>
#include <vector>

namespace Data {
class ChatFilters;
class Session;
class Thread;
} // namespace Data

class History;

namespace Main {
class Session;
} // namespace Main

namespace Dialogs {
class Key;
class MainList;
struct UnreadState;
} // namespace Dialogs

namespace Purple {

struct OpenedGraceState {
	bool opened = false;
	bool eligible = false;
	crl::time until = 0;
};

class WorkView final {
public:
	static WorkView &ForSession(not_null<Data::Session*> owner);
	explicit WorkView(not_null<Data::Session*> owner);
	~WorkView();

	void setup();
	void refreshWorkMode();
	void watchGrace(not_null<History*> history);
	void rearmGraceTimer();
	void startTicking();
	void tickTemporary();
	void graceExpired();
	void refreshViews();
	void refreshViewPinned();
	void refreshViewPins(int index);
	void saveViewPins(int index);
	[[nodiscard]] not_null<Dialogs::MainList*> viewList(int index = 0);
	[[nodiscard]] Dialogs::UnreadState badgeUnread() const;
	[[nodiscard]] FilterId badgeFilterId() const;
	[[nodiscard]] not_null<const Dialogs::MainList*> badgeList() const;
	[[nodiscard]] bool setChatPinned(Dialogs::Key key, FilterId id, bool pinned);
	[[nodiscard]] std::optional<bool> canPin(
		FilterId id,
		not_null<History*> history) const;
	void reorderPinned(FilterId id, Dialogs::Key key1, Dialogs::Key key2);
	void refreshChatListEntry(Dialogs::Key key);
	void removeChatListEntry(Dialogs::Key key);
	[[nodiscard]] OpenedGraceState openedGrace(const History *history) const;
	void setOpenedGrace(History *history, OpenedGraceState state);
	void forgetHistory(const History *history);

private:
	[[nodiscard]] Main::Session &session() const;
	[[nodiscard]] Data::ChatFilters &chatsFilters() const;
	[[nodiscard]] History *historyLoaded(PeerId peerId) const;

	not_null<Data::Session*> _owner;
	rpl::lifetime _lifetime;
	bool _purpleViewPinning = false;
	base::Timer _purpleGraceTimer;
	base::Timer _purpleTickTimer;
	base::flat_map<PeerId, OpenedGraceState> _openedGrace;
};

void SetupWorkView(not_null<Data::Session*> owner);
[[nodiscard]] not_null<Dialogs::MainList*> ViewList(
	not_null<Data::Session*> owner,
	int index = 0);
void SaveViewPins(not_null<Data::Session*> owner, int index);
[[nodiscard]] not_null<const Dialogs::MainList*> BadgeList(
	not_null<Data::Session*> owner);
[[nodiscard]] Dialogs::UnreadState BadgeUnread(
	not_null<Data::Session*> owner);
[[nodiscard]] FilterId BadgeFilterId(not_null<Data::Session*> owner);
void WatchGrace(not_null<Data::Session*> owner, not_null<History*> history);
void RefreshViewPinned(not_null<Data::Session*> owner);
[[nodiscard]] bool SetChatPinned(
	not_null<Data::Session*> owner,
	Dialogs::Key key,
	FilterId id,
	bool pinned);
[[nodiscard]] std::optional<bool> CanPin(
	not_null<const Data::Session*> owner,
	FilterId id,
	not_null<History*> history);
void ReorderPinned(
	not_null<Data::Session*> owner,
	FilterId id,
	Dialogs::Key key1,
	Dialogs::Key key2);
void RefreshChatListEntry(not_null<Data::Session*> owner, Dialogs::Key key);
void RemoveChatListEntry(not_null<Data::Session*> owner, Dialogs::Key key);
[[nodiscard]] OpenedGraceState OpenedGrace(const History *history);
void SetOpenedGrace(History *history, OpenedGraceState state);
void ForgetHistory(const History *history);

} // namespace Purple
