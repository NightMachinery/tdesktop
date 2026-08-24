/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>

#include <optional>
#include <vector>

// The parsed form of settings.toml. Deliberately free of every tdesktop
// dependency beyond base/ and Qt Core, so purple/test_config.sh can compile it
// on its own and drive it against fixture files - the file is hand-owned, so
// the parser has to survive anything a text editor can produce, and that is far
// easier to prove outside a running app.
namespace Purple {

// Peer ids exactly as written in the file. Turning these into tdesktop PeerIds
// is the engine's job; the parser stays ignorant of what a peer is.
using PeerIdValue = int64;

// What a chat is, for lists that match by type rather than by member. Lives
// here rather than in the engine because a list definition names these, and
// the engine sits above the parser.
enum class ChatKind : uchar {
	Private,
	Group,
	Channel,
	Bot,
};

// "private", "groups", "channels", "bots" - the spellings `kinds' accepts.
[[nodiscard]] std::optional<ChatKind> ParseChatKind(const QString &value);
[[nodiscard]] QString ChatKindName(ChatKind kind);

// How much of a folder a preset pulls into its own view, whatever the lists
// decided - the escape hatch for "hide everything except what is in here".
//
// `Pinned' is for a folder you keep a handful of current things at the top of:
// the difference between the two albums you are listening to and every channel
// ever filed there.
enum class FolderInclude : uchar {
	None,
	Pinned,
	All,
};

// "none", "pinned", "all" - the spellings `include_in_main_view' accepts.
[[nodiscard]] std::optional<FolderInclude> ParseFolderInclude(
	const QString &value);
[[nodiscard]] QString FolderIncludeName(FolderInclude value);

// When a chat is in the view at all. Everything except Always and Never
// depends on the chat's unread state, so a chat can enter and leave the view
// on its own as messages arrive and are read.
//
// Mention is the narrow one and is what `groups_require_mention_p' used to
// spell; it is now simply what a group defaults to.
enum class ShowMode : uchar {
	Always,
	Message,          // An unread message, or a manual unread mark.
	MessageOrReaction,
	Mention,
	Never,
};

// "always", "message", "message_or_reaction", "mention", "never".
[[nodiscard]] std::optional<ShowMode> ParseShowMode(const QString &value);
[[nodiscard]] QString ShowModeName(ShowMode value);

// What a chat of this kind does when nothing says otherwise. Channels and bots
// are things you subscribed to or started, so they stay; groups are the noisy
// ones and only speak up when they name you; a private chat appears when the
// person has said something.
//
// The default depending on the chat rather than being flat is why a resolved
// entry cannot collapse its mode: one list_order entry can match several kinds.
[[nodiscard]] ShowMode DefaultShowMode(ChatKind kind);

// Whether the mode needs the chat's unread state to answer. False for exactly
// Always and Never, which is what keeps a preset made of those two out of the
// re-check path entirely.
[[nodiscard]] bool ShowModeWatchesUnread(ShowMode value);

// How often the mode shows a chat, as a number, so "the most permissive of
// these two wins" is a comparison rather than a table of special cases. Spelt
// out rather than taken from the declaration order, which does not match:
// MessageOrReaction shows strictly more often than Message and is declared
// after it.
[[nodiscard]] int ShowModeRank(ShowMode value);

// A list says who is in it and nothing else. What happens to those chats is
// decided entirely by the preset that names the list, which is what lets one
// list mean "let through" in one preset and "swallow silently" in another
// without a second table saying so. See docs/purple/work_mode.md.
struct List {
	QString name;
	QString title;

	// A chat matches when its id is here, or when its kind is in `kinds'. Both
	// may be empty, which is a list that matches nothing - useful as a
	// placeholder you fill in from the chat menu later.
	std::vector<PeerIdValue> members;
	std::vector<ChatKind> kinds;
};

// One step of a preset's ordered list_order: the list it names, and what the
// preset does with the chats that list claims. Order is priority AND capture -
// the first entry whose list holds a chat decides it, and later entries never
// see that chat again.
//
// Tri-state so that "said nothing" stays distinguishable from "said false",
// which matters for warnings rather than for behaviour: an unset flag takes
// the documented default below.
struct ListEntry {
	QString list;

	// Unset means DefaultShowMode() for whatever kind the chat turns out to be,
	// which is not knowable here: one entry can claim private chats, groups and
	// channels at once. Collapsed by Visible(), which has the chat in hand.
	std::optional<ShowMode> show;

	std::optional<bool> notify; // Default true.

	friend bool operator==(const ListEntry &, const ListEntry &) = default;
};

// One of the account's real Telegram folders, as a preset sees it.
struct PresetFolder {
	QString name;

	// False makes the preset ignore this entry entirely: no tab, nothing
	// silenced, nothing fed into the main view, nothing counted. Default true.
	//
	// Distinct from show_p below, which only takes the tab off the strip - a
	// folder with show_p = false still silences, still feeds the main view and
	// still counts. This one is the comment character you do not have to add
	// and remove: an entry you are keeping around for a preset you are still
	// tuning, with the settings you chose for it intact and doing nothing.
	//
	// It also beats "*ALL". A folder you disabled by hand stays disabled in a
	// preset that also asks for every folder, because otherwise disabling one
	// would silently stop working the day you added the spread.
	std::optional<bool> enabled;

	// Whether the folder's tab appears in the strip. Default true: naming a
	// folder at all is normally how you ask for it.
	std::optional<bool> show;

	// False silences the folder's chats, on top of whatever their list said.
	std::optional<bool> notify;

	// False takes this folder out of every count: no number on its own tab, and
	// its chats left out of the app badge. Default true. For a folder that is
	// background noise on purpose, a count is just a number you have decided in
	// advance not to act on.
	std::optional<bool> badge;

	// The mode for the chats this folder contributes, on the same terms as
	// `notify' above - it is about the folder's chats, not about its tab, which
	// is what `show' means. Unset leaves them to the default for what they are,
	// because naming a folder chose which chats come in rather than when they
	// show.
	std::optional<ShowMode> showMode;

	// How much of this folder joins the preset's main view. Default None.
	//
	// Whatever it lets in comes in even when the chat is archived. Archiving is
	// how visibility is controlled in stock Telegram; under a preset the preset
	// controls it, so a folder that asked for its chats gets them wherever they
	// happen to be filed. The chats stay archived - they are simply also in the
	// view, the way a real Telegram folder holds archived chats too.
	std::optional<FolderInclude> include;

	friend bool operator==(
		const PresetFolder &,
		const PresetFolder &) = default;
};

// An extra tab a preset invents, alongside its main view. Its list_order picks
// membership only: a chat an entry claims with show = false is dropped from
// this tab, and `notify' means nothing here because a chat has one mute state
// however many tabs it appears in.
struct PresetView {
	QString name;

	// The tab's pinned order, in the order it should appear. Owned by the file
	// rather than by the server, which knows nothing about a tab you invented.
	std::vector<PeerIdValue> pinned;

	std::vector<ListEntry> listOrder;

	friend bool operator==(const PresetView &, const PresetView &) = default;
};

struct Preset {
	QString name;

	// What the preset's main tab is called where All chats used to be. Empty
	// means the preset's own name with its first letter capitalised, which is
	// what almost everyone wants and nobody should have to type.
	QString viewName;

	// A key that turns this preset on, and off again if it is already on. Qt
	// portable text, the same spelling `[peek] hotkey' uses - so on macOS
	// "Ctrl" is Command and "Meta" is the physical Control key. Empty for a
	// preset you only ever pick from the box.
	QString hotkey;

	// Whether a chat this preset hides is gone from the whole app rather than
	// only from the preset's own view of the chat list - so out of the forward
	// picker, out of search, out of recent chats. Nothing means no, which is
	// the default because a work mode is about what you are looking at, not
	// about what you are allowed to reach. See docs/purple/work_mode.md.
	std::optional<bool> hideEverywhere;

	// Priority order, first match wins. A chat no entry claims is hidden and
	// silenced: a preset names what gets through.
	std::vector<ListEntry> listOrder;

	// The real folders this preset shows. Empty means no folder tabs at all -
	// write "*ALL" to get them back.
	std::vector<PresetFolder> folders;

	std::vector<PresetView> views;
};

struct ScheduleRule {
	bool enabled = true;
	std::vector<int> days; // Qt::Monday .. Qt::Sunday, 1 .. 7.
	int from = -1; // Minutes since local midnight.
	int till = -1;
	QString preset;
};

struct Schedule {
	bool enabled = true;
	std::vector<ScheduleRule> rules;
};

struct FocusSync {
	bool enabled = false;
	QString enterPreset;
	QString exitPreset;
};

struct Peek {
	QString hotkey;
	int autoOffSeconds = 0; // Zero disables the timer.
};

// Which chats a recently-closed grace period covers.
enum class RecentScope : uchar {
	// Only a chat that was in the preset's view when it was opened. Reading it
	// is what would have taken it out, so this is the narrow repair: nothing
	// you open can pull in a chat the preset was hiding.
	AlreadyInView,

	// Any chat you open, hidden or not. One rule - a chat you have looked at
	// recently is in the view - and the only one of the three that helps when
	// you reach a hidden chat through search or through an extra view.
	AnyOpenChat,

	// The above, minus the chats that are already one click away somewhere else
	// in this preset: on an extra view, or in a folder whose tab is showing.
	AnyOpenChatExceptInFolder,
};

// "already_in_view", "any_open_chat", "any_open_chat_except_in_folder".
[[nodiscard]] std::optional<RecentScope> ParseRecentScope(const QString &value);
[[nodiscard]] QString RecentScopeName(RecentScope value);

// Keeping a chat in the view for a while after you stop looking at it. Without
// it a gated chat vanishes on the exact frame you click away from it, which is
// both startling and wrong: having just read something is the best evidence
// there is that you are still working on it.
struct Recent {
	int staySecondsAfterClose = 0; // Zero disables it entirely.
	RecentScope scope = RecentScope::AlreadyInView;
};

struct Premium {
	bool enabled = true;
};

struct Settings {
	Premium premium;

	// Definitions only, in file order. Priority is a preset's business.
	std::vector<List> lists;

	std::vector<Preset> presets;
	Schedule schedule;
	FocusSync focusSync;
	Peek peek;
	Recent recent;

	[[nodiscard]] const List *list(const QString &name) const;
	[[nodiscard]] const Preset *preset(const QString &name) const;
};

// Everything recoverable is a warning and leaves usable settings behind; only
// a TOML syntax error sets `error', because nothing else leaves the file
// without something meaningful to run on. See docs/purple/config.md.
struct ParseResult {
	Settings settings;
	std::vector<QString> warnings;
	QString error;

	[[nodiscard]] bool ok() const {
		return error.isEmpty();
	}
};

[[nodiscard]] ParseResult ParseSettings(
	const QString &text,
	const QString &path);

// The name the parser will not accept for a preset, because the engine uses it
// for the stock-behaviour bypass.
[[nodiscard]] bool IsReservedPresetName(const QString &name);

// Whether exit_preset says "previous" - put back whatever was active when the
// focus mode came on, rather than a preset named outright.
[[nodiscard]] bool IsPreviousPresetName(const QString &name);

// The preset name with its first letter capitalised, which is what a preset
// that did not write `default_view_name' calls its tab. Only the first letter:
// a preset called "deep focus" becomes "Deep focus", not "Deep Focus", because
// guessing at word boundaries in a name the user chose is how you end up
// mangling one.
//
// And only when there is no capital in it anywhere. A name that already has one
// has had its casing decided - "iH" stays "iH" - and the whole point of the
// rule is to tidy a name nobody thought about, not to overrule one somebody
// did.
[[nodiscard]] QString DefaultViewName(const QString &preset);

// The label a preset carries wherever one is shown to the user: its own
// `default_view_name', or the above. The tab standing in for All chats, the
// preset picker and every line naming the preset that silenced a chat all read
// the same word this way - the picker used to offer "work" above a tab that
// said "Work", and a preset that renamed its tab was two names for one thing.
[[nodiscard]] QString PresetTitle(
	const QString &name,
	const QString &viewName);
[[nodiscard]] QString PresetTitle(const Preset &preset);

// The spread marker: "*core" in a list_order or folders array splices in the
// entries of [list_sets.core] or [folder_sets.core]. Nothing if the string is
// not a reference at all.
[[nodiscard]] std::optional<QString> SpreadReference(const QString &value);

// The one built-in folder set, written "*ALL": every real folder the account
// has, with whatever flags the entry carries. It survives parsing as a
// PresetFolder holding this exact name, because the parser has never heard of
// a Telegram folder and cannot expand it - the display side does, in place, so
// the entry keeps its position in the strip and its flags. The asterisk stays
// in the name so it can never collide with a folder actually called "ALL".
[[nodiscard]] const QString &AllFoldersName();

// Whether this entry is that marker rather than a folder of the user's.
[[nodiscard]] bool IsAllFolders(const PresetFolder &folder);

// "90s", "2m", "1h", "0" / "off" for no timer. Nothing for unparseable input.
[[nodiscard]] std::optional<int> ParseDuration(const QString &value);

// "HH:MM" to minutes since midnight.
[[nodiscard]] std::optional<int> ParseTimeOfDay(const QString &value);

// "mon" .. "sun" to Qt::Monday .. Qt::Sunday.
[[nodiscard]] std::optional<int> ParseWeekday(const QString &value);

} // namespace Purple
