// Driven by purple/test_history_menu_lists.py, which writes the .inc files from
// the production sources.
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

template <typename T>
using not_null = T;
using QString = std::string;

inline QString operator""_q(const char16_t *text, std::size_t size) {
	auto result = QString();
	for (auto i = std::size_t(); i != size; ++i) {
		result.push_back(char(text[i]));
	}
	return result;
}

namespace style {
struct icon {};
struct PopupMenu {};
} // namespace style

namespace st {
inline style::icon menuIconAddToFolder;
inline style::PopupMenu popupMenuWithIcons;
} // namespace st

struct PeerData {
	std::uint64_t id = 0;
	bool self = false;
	bool isSelf() const { return self; }
};

namespace Ui {
struct Show {};
struct PopupMenu {};
namespace Menu {
struct MenuCallback {
	struct Args {
		QString text;
		std::function<void()> handler;
		const style::icon *icon = nullptr;
		std::function<void(not_null<PopupMenu*>)> fillSubmenu;
		const style::PopupMenu *submenuSt = nullptr;
		bool isSeparator = false;
	};
	std::function<void(Args)> callback;
	void operator()(Args args) const { callback(std::move(args)); }
};
} // namespace Menu
} // namespace Ui

namespace Window {
struct SessionController {
	std::shared_ptr<Ui::Show> show;
	std::shared_ptr<Ui::Show> uiShow() const { return show; }
};
} // namespace Window

namespace {

struct Item {
	QString name;
	Ui::Menu::MenuCallback::Args args;
};
std::vector<Item> Items;

void Record(const char *name) {
	Items.push_back({ name, {} });
}

Ui::Menu::MenuCallback Recorder() {
	return { [](Ui::Menu::MenuCallback::Args args) {
		Items.push_back({ args.isSeparator ? QString("-") : args.text, args });
	} };
}

int Failures = 0;

void Check(bool condition, const char *what) {
	if (!condition) {
		++Failures;
		std::fprintf(stderr, "FAIL: %s\n", what);
	}
}

} // namespace

namespace Purple {

bool CustomLists = true;
bool Filters = false;
std::vector<std::pair<std::shared_ptr<Ui::Show>, PeerData*>> Filled;

bool HasCustomLists() { return CustomLists; }
bool Filtering() { return Filters; }
std::uint64_t IdOf(not_null<PeerData*> peer) { return peer->id; }
void FillListsMenu(
		not_null<Ui::PopupMenu*>,
		std::shared_ptr<Ui::Show> show,
		not_null<PeerData*> peer) {
	Filled.emplace_back(show, peer);
}

#include "history-purple-stubs.inc"
#include "history-lists.inc"

} // namespace Purple

namespace Window {

using PeerMenuCallback = Ui::Menu::MenuCallback;

struct History {
	bool forum = false;
	bool isForum() const { return forum; }
};

struct Thread {
	PeerData *owner = nullptr;
	History history;
	void *sublist = nullptr;
	PeerData *peer() const { return owner; }
	void *asSublist() const { return sublist; }
	History *asHistory() { return &history; }
};

template <typename ...Args>
void PeerMenuAddMuteSubmenuAction(Args&&...) {
	Record("mute");
}

struct Filler {
	SessionController *_controller = nullptr;
	PeerData *_peer = nullptr;
	Thread *_thread = nullptr;
	bool _topic = false;
	bool _sublist = false;
	PeerMenuCallback _addAction = Recorder();

	void fillHistoryActions();
	void addToggleMuteSubmenu(bool addSeparator);
#include "history-members.inc"
};

#include "history-fillers.inc"

} // namespace Window

namespace {

struct Setup {
	std::uint64_t id = 7;
	bool self = false;
	bool forum = false;
	bool topic = false;
	bool sublist = false;
};

Window::SessionController Controller{ std::make_shared<Ui::Show>() };

std::vector<QString> HistoryMenu(Setup setup) {
	Items.clear();
	auto peer = PeerData{ setup.id, setup.self };
	auto thread = Window::Thread{ &peer, { setup.forum } };
	auto filler = Window::Filler();
	filler._controller = &Controller;
	filler._peer = &peer;
	filler._thread = &thread;
	filler._topic = setup.topic;
	filler._sublist = setup.sublist;
	filler.fillHistoryActions();
	auto names = std::vector<QString>();
	for (const auto &item : Items) {
		names.push_back(item.name);
	}
	return names;
}

int IndexOf(const std::vector<QString> &names, const QString &name) {
	for (auto i = 0; i != int(names.size()); ++i) {
		if (names[i] == name) {
			return i;
		}
	}
	return -1;
}

bool NoDoubleOrEdgeSeparators(const std::vector<QString> &names) {
	if (names.empty()) {
		return true;
	}
	if (names.front() == "-" || names.back() == "-") {
		return false;
	}
	for (auto i = 1; i != int(names.size()); ++i) {
		if (names[i] == "-" && names[i - 1] == "-") {
			return false;
		}
	}
	return true;
}

} // namespace

int main() {
	Purple::CustomLists = true;
	Purple::Filters = false;

	// An ordinary chat: mute rows, their rule, the Work Mode row, a rule,
	// then everything that was there before.
	const auto plain = HistoryMenu({});
	const auto mute = IndexOf(plain, "mute");
	const auto row = IndexOf(plain, "Work Mode");
	Check(mute == 0, "the mute rows still open the menu");
	Check(row == mute + 2 && plain[mute + 1] == "-", "Work Mode follows the mute rows' rule");
	Check(row >= 0 && row + 1 < int(plain.size()) && plain[row + 1] == "-", "a rule sets Work Mode apart from the rows below");
	Check(NoDoubleOrEdgeSeparators(plain), "no doubled or edge rules with the Work Mode row");
	Check(row >= 0 && Items[row].args.fillSubmenu != nullptr && !Items[row].args.handler, "Work Mode opens a submenu");

	// The rest of the menu is the base order with the row and its rule
	// taken out.
	Purple::CustomLists = false;
	const auto stock = HistoryMenu({});
	Check(IndexOf(stock, "Work Mode") < 0, "no lists and no running preset: no Work Mode row");
	auto without = plain;
	if (row >= 0 && row + 2 <= int(without.size())) {
		without.erase(without.begin() + row, without.begin() + row + 2);
	}
	Check(row >= 0 && without == stock, "the row adds only itself and one rule");
	Check(NoDoubleOrEdgeSeparators(stock), "the base menu keeps its rules");

	// A running preset with no custom lists still offers the row, as the
	// chat list does.
	Purple::Filters = true;
	Check(IndexOf(HistoryMenu({}), "Work Mode") == 2, "a running preset offers Work Mode");
	Purple::Filters = false;
	Purple::CustomLists = true;

	// No mute rows (Saved Messages, a forum): the row opens the menu with no
	// rule above it.
	for (const auto setup : { Setup{ .self = true }, Setup{ .forum = true } }) {
		const auto names = HistoryMenu(setup);
		Check(IndexOf(names, "mute") < 0, "no mute rows for self or forum");
		Check(IndexOf(names, "Work Mode") == 0, "Work Mode opens a menu without mute rows");
		Check(NoDoubleOrEdgeSeparators(names), "no leading rule without mute rows");
	}

	// Whatever a list cannot name gets no row: no id, a topic, a sublist.
	Check(IndexOf(HistoryMenu({ .id = 0 }), "Work Mode") < 0, "a peer without an id gets no row");
	Check(IndexOf(HistoryMenu({ .topic = true }), "Work Mode") < 0, "a topic gets no row");
	Check(IndexOf(HistoryMenu({ .sublist = true }), "Work Mode") < 0, "a sublist gets no row");

	// The same row as the chat list's context menu: wording, icon, submenu
	// style, and a submenu filled for the same show and peer.
	auto peer = PeerData{ 7, false };
	auto popup = Ui::PopupMenu();
	Items.clear();
	Purple::AddPeerListsSubmenu(&Controller, &peer, false, false, Recorder());
	const auto contextItems = Items;
	Items.clear();
	Purple::AddHistoryListsSubmenu(&Controller, &peer, false, false, Recorder());
	const auto historyItems = Items;
	Check(contextItems.size() == 3 && historyItems.size() == 2, "context menu has rules on both sides, history menu below only");
	if (contextItems.size() == 3 && historyItems.size() == 2) {
		const auto &a = contextItems[1].args;
		const auto &b = historyItems[0].args;
		Check(a.text == b.text && a.text == "Work Mode", "same wording");
		Check(a.icon == b.icon && a.icon == &st::menuIconAddToFolder, "same icon");
		Check(a.submenuSt == b.submenuSt && a.submenuSt == &st::popupMenuWithIcons, "same submenu style");
		Purple::Filled.clear();
		a.fillSubmenu(&popup);
		b.fillSubmenu(&popup);
		Check(Purple::Filled.size() == 2
			&& Purple::Filled[0] == Purple::Filled[1]
			&& Purple::Filled[0].first == Controller.show
			&& Purple::Filled[0].second == &peer, "both submenus are FillListsMenu for the same show and peer");
	}

	if (Failures) {
		std::fprintf(stderr, "%d failures\n", Failures);
		return 1;
	}
	std::printf("history menu lists: ok\n");
	return 0;
}
