/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/

// Standalone tests for the Purple config core. tdesktop has no unit test
// framework in the app target - cmake/tests.cmake builds visual GUI test apps
// behind DESKTOP_APP_TEST_APPS - and the parser and splicer are deliberately
// free of tdesktop dependencies, so they compile straight into this harness.
// Run with purple/test_config.sh.

#include "purple/purple_engine.h"
#include "purple/purple_settings.h"
#include "purple/purple_splice.h"
#include "purple/purple_state.h"

#include <QtCore/QDateTime>
#include <QtCore/QStringList>

#include <cstdio>
#include <type_traits>

namespace {

auto Checks = 0;
auto Failures = 0;
const char *Section = "";

void Report(bool ok, const QString &what, int line) {
	++Checks;
	if (!ok) {
		++Failures;
		std::printf("  FAIL  %s:%d  %s\n", Section, line, qPrintable(what));
	}
}

#define CHECK(cond) Report((cond), u"" #cond ""_q, __LINE__)
#define CHECK_EQ(a, b) Report( \
	(a) == (b), \
	u"%1 == %2, got '%3' vs '%4'"_q.arg(u"" #a ""_q, u"" #b ""_q) \
		.arg(Describe(a), Describe(b)), \
	__LINE__)

[[nodiscard]] QString Describe(const QString &value) {
	return value;
}

template <typename T>
	requires std::is_integral_v<T>
[[nodiscard]] QString Describe(T value) {
	if constexpr (std::is_same_v<T, bool>) {
		return value ? u"true"_q : u"false"_q;
	} else {
		return QString::number(qlonglong(value));
	}
}

[[nodiscard]] QString Describe(const std::vector<Purple::PeerIdValue> &value) {
	auto parts = QStringList();
	for (const auto id : value) {
		parts.push_back(QString::number(id));
	}
	return u"["_q + parts.join(u", "_q) + u"]"_q;
}

void Begin(const char *name) {
	Section = name;
	if (qEnvironmentVariableIsSet("PURPLE_TEST_TRACE")) {
		std::printf("== %s\n", name);
		std::fflush(stdout);
	}
}

[[nodiscard]] QString Path() {
	return u"settings.toml"_q;
}

[[nodiscard]] Purple::ParseResult Parse(const QString &text) {
	return Purple::ParseSettings(text, Path());
}

[[nodiscard]] bool WarnsAbout(
		const Purple::ParseResult &result,
		const QString &fragment) {
	for (const auto &warning : result.warnings) {
		if (warning.contains(fragment)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] QStringList ListNames(const Purple::Settings &settings) {
	auto result = QStringList();
	for (const auto &list : settings.lists) {
		result.push_back(list.name);
	}
	return result;
}

// -1 for "said nothing", which is a distinct and load-bearing answer: it is
// what makes an entry take the default for whatever kind the chat turns out
// to be.
[[nodiscard]] int Mode(const std::optional<Purple::ShowMode> &value) {
	return value ? int(*value) : -1;
}

[[nodiscard]] int Mode(Purple::ShowMode value) {
	return int(value);
}

[[nodiscard]] int Mode(Purple::ShowMode value, bool) = delete;

[[nodiscard]] Purple::MemberTitle Titles() {
	return [](Purple::PeerIdValue id) {
		return (id == 111222333)
			? u"Maman"_q
			: (id == 123456789)
			? u"Ali Rezaei"_q
			: (id == 987654321)
			? u"Backend Team"_q
			: QString();
	};
}

// The file the docs describe, used as the baseline for most checks.
[[nodiscard]] QString Example() {
	return uR"(# my settings

[lists.os]
title  = "OS"
members = [
  1234567890,   # My Todo Channel
]

[lists.emergency]
title  = "Emergency"
members = [
  111222333,    # Maman
]

[lists.colleagues]
title = "Colleagues"
# who I actually work with
members = [
  123456789,    # Ali Rezaei
  987654321,    # Backend Team
]

[lists.people]
title = "Everyone else"
kinds = ["private"]

[lists.noise]
kinds = ["channels", "bots"]

[list_sets.core]
list_order = [
  { list = "os",        show_mode = "always", notify_p = true },
  { list = "emergency", show_mode = "always", notify_p = true },
]

[folder_sets.work_folders]
folders = [
  { name = "Music", notify_p = false },
]

[presets.work]
list_order = [
  "*core",
  { list = "colleagues", show_mode = "mention",  notify_p = true },
  { list = "people",     show_mode = "always",  notify_p = false },
  { list = "noise",      show_mode = "never", notify_p = false },
]
folders = [ "*work_folders" ]

[presets.strict]
list_order = [ "*core" ]
folders = []
)"_q;
}

void TestLists() {
	Begin("lists");
	const auto result = Parse(Example());
	CHECK(result.ok());
	CHECK(result.warnings.empty());

	// Definitions only, in file order. There is no priority here any more -
	// that belongs to whichever preset names them.
	CHECK_EQ(ListNames(result.settings).join(u","_q),
		u"os,emergency,colleagues,people,noise"_q);

	const auto noise = result.settings.list(u"noise"_q);
	CHECK(noise != nullptr);
	CHECK_EQ(noise->title, u"noise"_q);
	CHECK(noise->members.empty());
	CHECK_EQ(int(noise->kinds.size()), 2);
	CHECK(noise->kinds[0] == Purple::ChatKind::Channel);
	CHECK(noise->kinds[1] == Purple::ChatKind::Bot);

	// A list carrying what a preset used to override is a file written against
	// the old model; saying so beats behaving differently in silence.
	const auto stale = Parse(uR"(
[lists.a]
show   = true
notify = false
locked = true
)"_q);
	CHECK(stale.ok());
	CHECK(WarnsAbout(stale, u"'show' is no longer a setting"_q));
	CHECK(WarnsAbout(stale, u"'notify' is no longer a setting"_q));
	CHECK(WarnsAbout(stale, u"'locked' is no longer a setting"_q));
	CHECK(WarnsAbout(stale, u"only reach a list it names"_q));
}

void TestKinds() {
	Begin("kinds");

	const auto result = Parse(uR"(
[lists.a]
kinds = ["private", "bots", "bots", "wombats"]

[lists.b]
kinds = "bots"
)"_q);
	CHECK(result.ok());
	CHECK(WarnsAbout(result, u"'wombats' is not one of"_q));
	CHECK(WarnsAbout(result, u"'kinds' should be an array"_q));

	// Deduplicated, order kept, the bad one dropped rather than the whole key.
	const auto a = result.settings.list(u"a"_q);
	CHECK_EQ(int(a->kinds.size()), 2);
	CHECK(a->kinds[0] == Purple::ChatKind::Private);
	CHECK(a->kinds[1] == Purple::ChatKind::Bot);
	CHECK(result.settings.list(u"b"_q)->kinds.empty());

	CHECK(Purple::ParseChatKind(u"Channels"_q) == Purple::ChatKind::Channel);
	CHECK(!Purple::ParseChatKind(u"channel"_q).has_value());
	CHECK_EQ(Purple::ChatKindName(Purple::ChatKind::Group), u"groups"_q);
}

void TestPresets() {
	Begin("presets");
	const auto result = Parse(Example());
	CHECK(result.ok());
	CHECK_EQ(result.settings.presets.size(), size_t(2));

	// File order, not alphabetical: the popover lists presets as written.
	CHECK_EQ(result.settings.presets[0].name, u"work"_q);
	CHECK_EQ(result.settings.presets[1].name, u"strict"_q);

	// "*core" spliced in at the front, then the three written inline.
	const auto work = result.settings.preset(u"work"_q);
	CHECK(work != nullptr);
	CHECK_EQ(int(work->listOrder.size()), 5);
	CHECK_EQ(work->listOrder[0].list, u"os"_q);
	CHECK_EQ(work->listOrder[1].list, u"emergency"_q);
	CHECK_EQ(work->listOrder[2].list, u"colleagues"_q);
	CHECK_EQ(Mode(work->listOrder[2].show), Mode(Purple::ShowMode::Mention));
	CHECK_EQ(work->listOrder[3].list, u"people"_q);
	CHECK(!work->listOrder[3].notify.value_or(true));
	CHECK_EQ(work->listOrder[4].list, u"noise"_q);
	CHECK_EQ(Mode(work->listOrder[4].show), Mode(Purple::ShowMode::Never));

	// Absent flags stay absent, so a warning can tell "false" from "unsaid".
	CHECK_EQ(Mode(work->listOrder[0].show), Mode(Purple::ShowMode::Always));

	CHECK_EQ(int(work->folders.size()), 1);
	CHECK_EQ(work->folders.front().name, u"Music"_q);
	CHECK(!work->folders.front().notify.value_or(true));
	CHECK(!work->folders.front().show.has_value());

	// strict shares the same set and asks for no folder tabs at all.
	const auto strict = result.settings.preset(u"strict"_q);
	CHECK(strict != nullptr);
	CHECK_EQ(int(strict->listOrder.size()), 2);
	CHECK_EQ(strict->listOrder[0].list, u"os"_q);
	CHECK(strict->folders.empty());
	CHECK(strict->views.empty());
}

void TestPresetPolicies() {
	Begin("preset policies");

	const auto reserved = Parse(uR"(
[presets.Normal]
list_order = [ { list = "a" } ]

[presets.keep]
list_order = [ { list = "a" } ]

[lists.a]
kinds = ["bots"]
)"_q);
	CHECK(reserved.ok());
	CHECK(WarnsAbout(reserved, u"reserved"_q));
	CHECK_EQ(reserved.settings.presets.size(), size_t(1));
	CHECK_EQ(reserved.settings.presets[0].name, u"keep"_q);

	// "default" was the implicit root of the old inheritance chain. There is no
	// chain now, so it is an ordinary name.
	const auto plain = Parse(uR"(
[presets.default]
list_order = [ { list = "a" } ]

[lists.a]
kinds = ["bots"]
)"_q);
	CHECK(plain.ok());
	CHECK_EQ(plain.settings.presets.size(), size_t(1));

	// Naming a list that is not defined claims nothing, which is worth saying
	// out loud - it looks exactly like a preset that is working.
	const auto ghost = Parse(uR"(
[presets.work]
list_order = [ { list = "ghosts" } ]
)"_q);
	CHECK(ghost.ok());
	CHECK(WarnsAbout(ghost, u"has no [lists.ghosts] table"_q));

	// An empty preset is legal and hides everything; silence about that would
	// be the single most confusing thing this parser could do.
	const auto empty = Parse(u"[presets.work]\n"_q);
	CHECK(empty.ok());
	CHECK(WarnsAbout(empty, u"hides and silences everything"_q));

	// Everything the old model spelled differently.
	const auto stale = Parse(uR"(
[presets.work]
inherit = "default"
groups_require_mention = true
hide_everywhere = true

[presets.work.overrides.a]
show = false
)"_q);
	CHECK(stale.ok());
	CHECK(WarnsAbout(stale, u"'inherit' is no longer a setting"_q));
	CHECK(WarnsAbout(stale, u"'overrides' is no longer a setting"_q));
	CHECK(WarnsAbout(stale, u"'groups_require_mention' is no longer"_q));
	CHECK(WarnsAbout(stale, u"spelled 'hide_everywhere_p' now"_q));
	CHECK(!stale.settings.preset(u"work"_q)->hideEverywhere.has_value());
}

void TestSpread() {
	Begin("spread");

	// Nested sets, a duplicate the second mention of which never decides
	// anything, and a name that is not a set at all.
	const auto result = Parse(uR"(
[lists.a]
kinds = ["bots"]
[lists.b]
kinds = ["channels"]
[lists.c]
kinds = ["groups"]

[list_sets.inner]
list_order = [ { list = "a", show_mode = "never" } ]

[list_sets.outer]
list_order = [ "*inner", { list = "b" } ]

[presets.work]
list_order = [ "*outer", { list = "a", show_mode = "always" }, "*ghosts", { list = "c" } ]
)"_q);
	CHECK(result.ok());
	CHECK(WarnsAbout(result, u"there is no list_set called 'ghosts'"_q));
	CHECK(WarnsAbout(result, u"already claimed further up"_q));

	const auto work = result.settings.preset(u"work"_q);
	CHECK_EQ(int(work->listOrder.size()), 3);
	CHECK_EQ(work->listOrder[0].list, u"a"_q);
	CHECK_EQ(work->listOrder[1].list, u"b"_q);
	CHECK_EQ(work->listOrder[2].list, u"c"_q);

	// First mention wins, so the "a" inside the set keeps its show_mode = "never"
	// and the later one is ignored rather than overriding it.
	CHECK_EQ(Mode(work->listOrder[0].show), Mode(Purple::ShowMode::Never));

	// The other way round is the idiom the spread exists for - override one
	// entry, then splice in the defaults - so it is silent.
	const auto overriding = Parse(uR"(
[lists.a]
kinds = ["bots"]
[lists.b]
kinds = ["channels"]

[list_sets.defaults]
list_order = [ { list = "a", show_mode = "always" }, { list = "b", show_mode = "always" } ]

[presets.work]
list_order = [ { list = "a", show_mode = "never" }, "*defaults" ]
)"_q);
	CHECK(overriding.ok());
	CHECK(!WarnsAbout(overriding, u"already claimed"_q));
	const auto tuned = overriding.settings.preset(u"work"_q);
	CHECK_EQ(int(tuned->listOrder.size()), 2);
	CHECK_EQ(tuned->listOrder[0].list, u"a"_q);
	CHECK_EQ(Mode(tuned->listOrder[0].show), Mode(Purple::ShowMode::Never));
	CHECK_EQ(tuned->listOrder[1].list, u"b"_q);

	// A set referring to itself is a typo, not a feature.
	const auto loop = Parse(uR"(
[lists.a]
kinds = ["bots"]

[list_sets.one]
list_order = [ "*two" ]

[list_sets.two]
list_order = [ "*one", { list = "a" } ]

[presets.work]
list_order = [ "*one" ]
)"_q);
	CHECK(loop.ok());
	CHECK(WarnsAbout(loop, u"refers back into itself"_q));
	CHECK_EQ(int(loop.settings.preset(u"work"_q)->listOrder.size()), 1);

	// A bare string that is not a reference is a mistake worth naming.
	const auto bare = Parse(uR"(
[presets.work]
list_order = [ "colleagues" ]
)"_q);
	CHECK(bare.ok());
	CHECK(WarnsAbout(bare, u"neither a table nor a \"*set\" reference"_q));
}

void TestFolderSelection() {
	Begin("folders");

	const auto result = Parse(uR"(
[folder_sets.mine]
folders = [ { name = "B", include_in_main_view = "all" } ]

[presets.work]
folders = [ "*mine", "*ALL", { name = "B", show_mode = "never" } ]

[presets.none]
list_order = []

[presets.every]
folders = [ "*ALL" ]
)"_q);
	CHECK(result.ok());
	CHECK(WarnsAbout(result, u"named more than once"_q));

	// The set first, then the marker; the third entry is B again and is
	// dropped, so B keeps the flags its first mention gave it.
	const auto work = result.settings.preset(u"work"_q);
	CHECK_EQ(int(work->folders.size()), 2);
	CHECK_EQ(work->folders[0].name, u"B"_q);
	CHECK_EQ(int(work->folders[0].include.value_or(
		Purple::FolderInclude::None)), int(Purple::FolderInclude::All));
	CHECK(Purple::IsAllFolders(work->folders[1]));

	// Saying nothing about folders is saying none, the same rule the lists
	// follow. "*ALL" is how you ask for the strip back.
	CHECK(result.settings.preset(u"none"_q)->folders.empty());
	const auto every = result.settings.preset(u"every"_q);
	CHECK_EQ(int(every->folders.size()), 1);
	CHECK(Purple::IsAllFolders(every->folders[0]));

	// The old spelling, and an attempt to redefine the built-in set.
	const auto stale = Parse(uR"(
[folder_sets.ALL]
folders = [ { name = "B" } ]

[presets.work]
folders = [ { name = "B", filtered = false } ]
)"_q);
	CHECK(stale.ok());
	CHECK(WarnsAbout(stale, u"cannot be redefined"_q));
	CHECK(WarnsAbout(stale, u"include_in_main_view = \"all\""_q));

	// The enum, and the two spellings it replaced. Both of those are retired
	// rather than quietly accepted: a folder still saying include_in_main_view_p
	// would otherwise include nothing and look exactly like one that meant to.
	const auto modes = Parse(uR"(
[presets.work]
folders = [
  { name = "Music", include_in_main_view = "pinned" },
  { name = "B",     include_in_main_view = "all" },
  { name = "Quiet", include_in_main_view = "none" },
  { name = "Plain" },
  { name = "Old",   include_in_main_view_p = true },
  { name = "Older", pinned_only_p = true },
  { name = "Typo",  include_in_main_view = "some" },
]
)"_q);
	CHECK(modes.ok());
	const auto shaped = modes.settings.preset(u"work"_q);
	const auto mode = [&](int index) {
		return int(shaped->folders[index].include.value_or(
			Purple::FolderInclude::None));
	};
	CHECK_EQ(mode(0), int(Purple::FolderInclude::Pinned));
	CHECK_EQ(mode(1), int(Purple::FolderInclude::All));
	CHECK_EQ(mode(2), int(Purple::FolderInclude::None));
	CHECK(!shaped->folders[3].include.has_value());
	CHECK(WarnsAbout(modes, u"no longer a yes-or-no"_q));
	CHECK(WarnsAbout(modes, u"write include_in_main_view = \"pinned\""_q));
	CHECK(WarnsAbout(modes, u"one of none, pinned, all"_q));

	// A misspelt value is ignored rather than guessed at, so the folder falls
	// back to contributing nothing.
	CHECK(!shaped->folders[6].include.has_value());

	CHECK_EQ(
		Purple::FolderIncludeName(Purple::FolderInclude::Pinned),
		u"pinned"_q);
	CHECK(!Purple::ParseFolderInclude(u"some"_q).has_value());
	CHECK_EQ(
		int(*Purple::ParseFolderInclude(u"  ALL "_q)),
		int(Purple::FolderInclude::All));
}

void TestShowModes() {
	Begin("show modes");

	const auto parsed = Parse(uR"(
[lists.everything]
kinds = ["private", "groups", "channels", "bots"]

[lists.loud]
kinds = ["channels"]

[presets.plain]
list_order = [ { list = "everything" } ]

[presets.spelled]
list_order = [
  { list = "loud",       show_mode = "never" },
  { list = "everything", show_mode = "message_or_reaction" },
]

[presets.stale]
list_order = [
  { list = "everything", show_p = true, groups_require_mention_p = true },
]

[presets.typo]
list_order = [ { list = "everything", show_mode = "sometimes" } ]
)"_q);
	CHECK(parsed.ok());

	// Saying nothing is the interesting case: the entry keeps no mode at all,
	// and the chat kind supplies one when the chat is finally in hand.
	const auto plain = parsed.settings.preset(u"plain"_q);
	CHECK(!plain->listOrder[0].show.has_value());

	const auto resolved = Purple::Resolve(parsed.settings, u"plain"_q);
	const auto modeOf = [&](Purple::ChatKind kind) {
		return Mode(Purple::Visible(parsed.settings, *resolved, 1, kind).show);
	};
	CHECK_EQ(modeOf(Purple::ChatKind::Channel),
		Mode(Purple::ShowMode::Always));
	CHECK_EQ(modeOf(Purple::ChatKind::Group),
		Mode(Purple::ShowMode::Mention));
	CHECK_EQ(modeOf(Purple::ChatKind::Private),
		Mode(Purple::ShowMode::Message));
	CHECK_EQ(modeOf(Purple::ChatKind::Bot),
		Mode(Purple::ShowMode::Always));

	// The same four, straight off DefaultShowMode, so the table above is not
	// only being read through one path.
	CHECK_EQ(Mode(Purple::DefaultShowMode(Purple::ChatKind::Channel)),
		Mode(Purple::ShowMode::Always));
	CHECK_EQ(Mode(Purple::DefaultShowMode(Purple::ChatKind::Group)),
		Mode(Purple::ShowMode::Mention));
	CHECK_EQ(Mode(Purple::DefaultShowMode(Purple::ChatKind::Bot)),
		Mode(Purple::ShowMode::Always));
	CHECK_EQ(Mode(Purple::DefaultShowMode(Purple::ChatKind::Private)),
		Mode(Purple::ShowMode::Message));

	// An explicit mode wins over the kind default, in both directions.
	const auto spelled = Purple::Resolve(parsed.settings, u"spelled"_q);
	CHECK_EQ(
		Mode(Purple::Visible(
			parsed.settings,
			*spelled,
			1,
			Purple::ChatKind::Channel).show),
		Mode(Purple::ShowMode::Never));
	CHECK_EQ(
		Mode(Purple::Visible(
			parsed.settings,
			*spelled,
			1,
			Purple::ChatKind::Group).show),
		Mode(Purple::ShowMode::MessageOrReaction));

	// Both retired spellings are reported rather than silently ignored, and
	// the entry falls back to the defaults.
	CHECK(WarnsAbout(parsed, u"no longer a yes-or-no"_q));
	CHECK(WarnsAbout(parsed, u"show_mode = \"mention\""_q));
	CHECK(!parsed.settings.preset(u"stale"_q)->listOrder[0].show.has_value());

	// A value nobody can spell is ignored, which leaves the default rather
	// than guessing at what was meant.
	CHECK(WarnsAbout(parsed, u"one of always, message, message_or_reaction"_q));
	CHECK(!parsed.settings.preset(u"typo"_q)->listOrder[0].show.has_value());

	// Fall-through is Never, which is the whole model in one line: a preset
	// names what gets through.
	const auto nothing = Purple::Resolve(parsed.settings, u"spelled"_q);
	CHECK_EQ(
		Mode(Purple::Visible(
			parsed.settings,
			*nothing,
			1,
			Purple::ChatKind::Bot).show),
		Mode(Purple::ShowMode::MessageOrReaction));

	// Rank is what makes "the most permissive of these two folders wins" a
	// comparison. It deliberately does not follow declaration order.
	CHECK(Purple::ShowModeRank(Purple::ShowMode::Always)
		> Purple::ShowModeRank(Purple::ShowMode::MessageOrReaction));
	CHECK(Purple::ShowModeRank(Purple::ShowMode::MessageOrReaction)
		> Purple::ShowModeRank(Purple::ShowMode::Message));
	CHECK(Purple::ShowModeRank(Purple::ShowMode::Message)
		> Purple::ShowModeRank(Purple::ShowMode::Mention));
	CHECK(Purple::ShowModeRank(Purple::ShowMode::Mention)
		> Purple::ShowModeRank(Purple::ShowMode::Never));

	// Only these two answer without the chat's unread state, which is what
	// keeps every other preset off the re-check path.
	CHECK(!Purple::ShowModeWatchesUnread(Purple::ShowMode::Always));
	CHECK(!Purple::ShowModeWatchesUnread(Purple::ShowMode::Never));
	CHECK(Purple::ShowModeWatchesUnread(Purple::ShowMode::Message));
	CHECK(Purple::ShowModeWatchesUnread(Purple::ShowMode::Mention));

	CHECK_EQ(Purple::ShowModeName(Purple::ShowMode::MessageOrReaction),
		u"message_or_reaction"_q);
	CHECK(!Purple::ParseShowMode(u"sometimes"_q).has_value());
	CHECK_EQ(Mode(*Purple::ParseShowMode(u"  MENTION "_q)),
		Mode(Purple::ShowMode::Mention));

	// A folder carries one too, for the chats it contributes - and badge_p
	// alongside it, which is a different question again.
	const auto folders = Parse(uR"(
[presets.work]
folders = [
  { name = "Music", include_in_main_view = "pinned", show_mode = "message", badge_p = false },
  { name = "B" },
]
)"_q);
	CHECK(folders.ok());
	const auto shaped = folders.settings.preset(u"work"_q);
	CHECK(shaped != nullptr);
	CHECK_EQ(int(shaped->folders.size()), 2);
	CHECK_EQ(Mode(shaped->folders[0].showMode), Mode(Purple::ShowMode::Message));
	CHECK(!shaped->folders[0].badge.value_or(true));

	// Saying nothing leaves a folder counted, which is what almost every
	// folder wants and nobody should have to write.
	CHECK(!shaped->folders[1].badge.has_value());
	CHECK(shaped->folders[1].badge.value_or(true));
}

void TestViews() {
	Begin("views");

	const auto result = Parse(uR"(
[lists.a]
kinds = ["private"]
[lists.b]
kinds = ["bots"]

[presets.work]
list_order = [ { list = "a" } ]

[[presets.work.views]]
name   = "Focus"
pinned = [ 5, 6, 5 ]
list_order = [ { list = "a", show_mode = "always", notify_p = false } ]

[[presets.work.views]]
name = "Focus"
list_order = [ { list = "b" } ]

[[presets.work.views]]
name = "Empty"

[[presets.work.views]]
list_order = [ { list = "b" } ]
)"_q);
	CHECK(result.ok());
	CHECK(WarnsAbout(result, u"already a view called 'Focus'"_q));
	CHECK(WarnsAbout(result, u"names no list"_q));
	CHECK(WarnsAbout(result, u"a view needs 'name'"_q));

	// notify_p inside a view cannot mean anything: a chat has one mute state
	// however many tabs are showing it.
	CHECK(WarnsAbout(result, u"'notify_p' means nothing inside a view"_q));

	// But only when the view wrote it. A list_set exists to be reused, and it
	// was written for a preset's own order where notify_p is exactly what it
	// should say - warning about it every time the set is spread onto a tab
	// would make the idiom unusable while saying nothing to act on.
	const auto spread = Parse(uR"(
[lists.a]
kinds = ["private"]

[list_sets.core]
list_order = [ { list = "a", show_mode = "always", notify_p = true } ]

[presets.work]
list_order = [ "*core" ]

[[presets.work.views]]
name = "Focus"
list_order = [ "*core" ]
)"_q);
	CHECK(spread.ok());
	CHECK(!WarnsAbout(spread, u"'notify_p' means nothing inside a view"_q));
	CHECK_EQ(int(spread.settings.preset(u"work"_q)->views.size()), 1);

	const auto work = result.settings.preset(u"work"_q);
	CHECK_EQ(int(work->views.size()), 1);
	CHECK_EQ(work->views[0].name, u"Focus"_q);
	CHECK_EQ(work->views[0].pinned,
		(std::vector<Purple::PeerIdValue>{ 5, 6 }));
	CHECK_EQ(int(work->views[0].listOrder.size()), 1);

	// A view showing a chat the preset has taken out of the app entirely is not
	// a preference, it is an assertion failure waiting to happen.
	const auto gone = Parse(uR"(
[lists.a]
kinds = ["private"]

[presets.work]
hide_everywhere_p = true
list_order = [ { list = "a" } ]

[[presets.work.views]]
name = "Focus"
list_order = [ { list = "a" } ]
)"_q);
	CHECK(gone.ok());
	CHECK(WarnsAbout(gone, u"leaves nothing for an extra view to show"_q));
	CHECK(gone.settings.preset(u"work"_q)->views.empty());
}

void TestMembers() {
	Begin("members");

	const auto result = Parse(uR"(
[lists.a]
members = [ 1, 2, 2, 3, 1 ]

[lists.b]
members = [ "nope", 4 ]

[lists."*sneaky"]
members = [ 9 ]
)"_q);
	CHECK(result.ok());
	CHECK_EQ(result.settings.list(u"a"_q)->members,
		(std::vector<Purple::PeerIdValue>{ 1, 2, 3 }));
	CHECK(WarnsAbout(result, u"should be peer ids"_q));
	CHECK_EQ(result.settings.list(u"b"_q)->members,
		(std::vector<Purple::PeerIdValue>{ 4 }));

	// A list whose name starts with '*' could never be told apart from a set
	// reference by anyone reading the file.
	CHECK(WarnsAbout(result, u"reserved for set references"_q));
	CHECK(result.settings.list(u"*sneaky"_q) == nullptr);
}

void TestScheduleAndFocus() {
	Begin("schedule and focus");

	const auto result = Parse(uR"(
[presets.work]
list_order = []

[schedule]
enabled_p = true

[[schedule.rules]]
days   = ["mon", "tue"]
from   = "09:00"
to     = "17:00"
preset = "work"

[[schedule.rules]]
from   = "09:00"
to     = "17:00"
preset = "ghost"

[[schedule.rules]]
from   = "09:00"
to     = "09:00"
preset = "work"

[focus_sync]
enabled_p    = true
enter_preset = "work"
exit_preset  = "previous"

[peek]
hotkey   = "Ctrl+Alt+K"
auto_off = "90s"
)"_q);
	CHECK(result.ok());
	CHECK(result.settings.schedule.enabled);

	// Only the first rule is usable: the second names a missing preset, the
	// third is a zero-length window.
	CHECK_EQ(result.settings.schedule.rules.size(), size_t(1));
	CHECK(WarnsAbout(result, u"'ghost' does not exist"_q));
	CHECK(WarnsAbout(result, u"the same time"_q));

	const auto &rule = result.settings.schedule.rules[0];
	CHECK(rule.enabled);
	CHECK_EQ(rule.from, 9 * 60);
	CHECK_EQ(rule.till, 17 * 60);
	CHECK_EQ(int(rule.days.size()), 2);

	CHECK(result.settings.focusSync.enabled);
	CHECK_EQ(result.settings.focusSync.enterPreset, u"work"_q);
	CHECK_EQ(result.settings.peek.hotkey, u"Ctrl+Alt+K"_q);
	CHECK_EQ(result.settings.peek.autoOffSeconds, 90);

	// A DISABLED rule aimed at a preset that does not exist is kept and quiet:
	// that is the normal state of the example in the starter file, and a fresh
	// install must not complain on every start.
	const auto sleeping = Parse(uR"(
[[schedule.rules]]
enabled_p = false
days      = ["mon"]
from      = "09:00"
to        = "17:00"
preset    = "ghost"
)"_q);
	CHECK(sleeping.ok());
	CHECK(sleeping.warnings.empty());
	CHECK_EQ(sleeping.settings.schedule.rules.size(), size_t(1));
	CHECK(!sleeping.settings.schedule.rules[0].enabled);

	// Focus sync pointing at nothing turns itself off rather than half-working.
	const auto broken = Parse(uR"(
[focus_sync]
enabled_p    = true
enter_preset = "ghost"
)"_q);
	CHECK(broken.ok());
	CHECK(!broken.settings.focusSync.enabled);
	CHECK(WarnsAbout(broken, u"turning focus sync off"_q));
}

void TestScalarParsers() {
	Begin("scalar parsers");

	CHECK_EQ(Purple::ParseDuration(u"2m"_q).value_or(-1), 120);
	CHECK_EQ(Purple::ParseDuration(u"90s"_q).value_or(-1), 90);
	CHECK_EQ(Purple::ParseDuration(u"1h"_q).value_or(-1), 3600);
	CHECK_EQ(Purple::ParseDuration(u"0"_q).value_or(-1), 0);
	CHECK_EQ(Purple::ParseDuration(u"off"_q).value_or(-1), 0);
	CHECK(!Purple::ParseDuration(u"soon"_q).has_value());
	CHECK(!Purple::ParseDuration(u"-5m"_q).has_value());

	CHECK_EQ(Purple::ParseTimeOfDay(u"09:00"_q).value_or(-1), 540);
	CHECK_EQ(Purple::ParseTimeOfDay(u"23:59"_q).value_or(-1), 1439);
	CHECK(!Purple::ParseTimeOfDay(u"24:00"_q).has_value());
	CHECK(!Purple::ParseTimeOfDay(u"9am"_q).has_value());

	CHECK_EQ(Purple::ParseWeekday(u"mon"_q).value_or(-1), 1);
	CHECK_EQ(Purple::ParseWeekday(u"Sunday"_q).value_or(-1), 7);
	CHECK(!Purple::ParseWeekday(u"caturday"_q).has_value());
}

void TestPremiumStillParses() {
	Begin("premium");

	const auto result = Parse(uR"([premium]
# Unlock the client-side features.
enabled_p = true
)"_q);
	CHECK(result.ok());
	CHECK(result.settings.premium.enabled);

	// A file with nothing but the Premium toggle is complete, not
	// half-configured: it must not log a warning on every single start.
	CHECK(result.warnings.empty());
	CHECK(result.settings.lists.empty());
	CHECK(result.settings.presets.empty());
	CHECK(!result.settings.focusSync.enabled);

	const auto off = Parse(u"[premium]\nenabled_p = false\n"_q);
	CHECK(off.ok());
	CHECK(!off.settings.premium.enabled);

	// The old spelling is gone, and gone loudly - silently defaulting to true
	// would turn the toggle off-looking and on-behaving.
	const auto stale = Parse(u"[premium]\nenabled = false\n"_q);
	CHECK(stale.ok());
	CHECK(stale.settings.premium.enabled);
	CHECK(WarnsAbout(stale, u"spelled 'enabled_p' now"_q));

	// No file content at all is still a usable configuration.
	const auto empty = Parse(QString());
	CHECK(empty.ok());
	CHECK(empty.settings.premium.enabled);
	CHECK(empty.settings.lists.empty());
	CHECK(empty.warnings.empty());

	// A top-level list_order is where priority used to live.
	const auto order = Parse(u"list_order = [\"a\"]\n"_q);
	CHECK(order.ok());
	CHECK(WarnsAbout(order, u"each preset writes its own"_q));
}

void TestBrokenFile() {
	Begin("broken file");

	const auto result = Parse(u"[lists.a\nmembers = ["_q);
	CHECK(!result.ok());
	CHECK(!result.error.isEmpty());

	// And the splicer refuses to touch it rather than appending blindly.
	const auto spliced = Purple::AddListMember(
		u"[lists.a\nmembers = ["_q,
		Path(),
		u"a"_q,
		42,
		Titles());
	CHECK(!spliced.ok());
	CHECK(!spliced.changed);
	CHECK_EQ(spliced.text, u"[lists.a\nmembers = ["_q);
}

void TestSpliceAdd() {
	Begin("splice add");

	const auto before = Example();
	const auto result = Purple::AddListMember(
		before,
		Path(),
		u"colleagues"_q,
		555000111,
		[](Purple::PeerIdValue) { return u"New Person"_q; });
	CHECK(result.ok());
	CHECK(result.changed);
	CHECK(result.text.contains(u"555000111,   # New Person"_q)
		|| result.text.contains(u"555000111, # New Person"_q));

	// Every comment in the file survives, including the one inside the array.
	CHECK(result.text.contains(u"# my settings"_q));
	CHECK(result.text.contains(u"# who I actually work with"_q));
	CHECK(result.text.contains(u"# Ali Rezaei"_q));
	CHECK(result.text.contains(u"# My Todo Channel"_q));

	// The new id joins the end of the right list and nothing else moves.
	CHECK_EQ(Purple::ListMembers(result.text, Path(), u"colleagues"_q),
		(std::vector<Purple::PeerIdValue>{ 123456789, 987654321, 555000111 }));
	CHECK_EQ(Purple::ListMembers(result.text, Path(), u"os"_q),
		(std::vector<Purple::PeerIdValue>{ 1234567890 }));

	// The indentation of existing members is matched, not invented.
	CHECK(result.text.contains(u"\n  555000111,"_q));

	// Adding what is already there changes nothing at all.
	const auto again = Purple::AddListMember(
		result.text,
		Path(),
		u"colleagues"_q,
		555000111,
		Titles());
	CHECK(again.ok());
	CHECK(!again.changed);
	CHECK_EQ(again.text, result.text);
}

void TestSpliceRemove() {
	Begin("splice remove");

	const auto before = Example();
	const auto result = Purple::RemoveListMember(
		before,
		Path(),
		u"colleagues"_q,
		123456789,
		Titles());
	CHECK(result.ok());
	CHECK(result.changed);

	// Exactly one line goes, and it is the right one.
	CHECK_EQ(result.text.count('\n'), before.count('\n') - 1);
	CHECK(!result.text.contains(u"# Ali Rezaei"_q));
	CHECK(!result.text.contains(u"  123456789,"_q));
	CHECK(result.text.contains(u"987654321,    # Backend Team"_q));

	// The similar-looking id in another list is untouched.
	CHECK(result.text.contains(u"1234567890,   # My Todo Channel"_q));
	CHECK(result.text.contains(u"# who I actually work with"_q));
	CHECK_EQ(Purple::ListMembers(result.text, Path(), u"colleagues"_q),
		(std::vector<Purple::PeerIdValue>{ 987654321 }));

	// Removing what is not there is a no-op, not an error.
	const auto absent = Purple::RemoveListMember(
		before,
		Path(),
		u"colleagues"_q,
		42,
		Titles());
	CHECK(absent.ok());
	CHECK(!absent.changed);
	CHECK_EQ(absent.text, before);

	// Emptying a list leaves a well-formed empty array.
	auto emptied = Purple::RemoveListMember(
		before,
		Path(),
		u"emergency"_q,
		111222333,
		Titles());
	CHECK(emptied.ok());
	CHECK(Purple::ListMembers(emptied.text, Path(), u"emergency"_q).empty());
	CHECK(Parse(emptied.text).ok());
}

void TestSpliceCanonicalises() {
	Begin("splice canonicalises");

	const auto squashed = uR"(# head comment
[lists.a]
members = [ 1, 2, 3 ]   # inline note
[lists.b]
members = []
)"_q;
	const auto result = Purple::AddListMember(
		squashed,
		Path(),
		u"a"_q,
		4,
		[](Purple::PeerIdValue id) {
			return u"Name %1"_q.arg(id);
		});
	CHECK(result.ok());
	CHECK(result.changed);

	// Ids are preserved in order and the array becomes one-per-line.
	CHECK_EQ(Purple::ListMembers(result.text, Path(), u"a"_q),
		(std::vector<Purple::PeerIdValue>{ 1, 2, 3, 4 }));
	CHECK(result.text.contains(u"\n  1, # Name 1"_q));
	CHECK(result.text.contains(u"\n  4, # Name 4"_q));

	// The blast radius stops at the array: text before and after is untouched,
	// including the trailing comment that followed the closing bracket.
	CHECK(result.text.startsWith(u"# head comment\n[lists.a]\n"_q));
	CHECK(result.text.contains(u"]   # inline note"_q));
	CHECK(result.text.contains(u"[lists.b]"_q));
	CHECK(Parse(result.text).ok());

	// An empty array is canonicalised the same way.
	const auto filled = Purple::AddListMember(
		result.text,
		Path(),
		u"b"_q,
		9,
		Titles());
	CHECK(filled.ok());
	CHECK_EQ(Purple::ListMembers(filled.text, Path(), u"b"_q),
		(std::vector<Purple::PeerIdValue>{ 9 }));
}

void TestSpliceMissingArray() {
	Begin("splice missing array");

	const auto text = uR"([lists.a]
title = "A"

[lists.b]
title = "B"
)"_q;
	const auto result = Purple::AddListMember(
		text,
		Path(),
		u"a"_q,
		77,
		Titles());
	CHECK(result.ok());
	CHECK(result.changed);
	CHECK(Parse(result.text).ok());
	CHECK_EQ(Purple::ListMembers(result.text, Path(), u"a"_q),
		(std::vector<Purple::PeerIdValue>{ 77 }));
	CHECK(result.text.contains(u"title = \"A\""_q));
	CHECK(result.text.contains(u"[lists.b]"_q));

	// A list that is not in the file is refused, not created.
	const auto missing = Purple::AddListMember(
		text,
		Path(),
		u"ghosts"_q,
		1,
		Titles());
	CHECK(!missing.ok());
	CHECK_EQ(missing.text, text);
	CHECK(missing.error.contains(u"[lists.ghosts]"_q));
}

void TestSpliceStability() {
	Begin("splice stability");

	// 100 add/remove cycles, as the spec's acceptance test asks for. The file
	// has to come back byte-identical and every comment has to still be there.
	auto text = Example();
	const auto original = text;
	for (auto i = 0; i != 100; ++i) {
		const auto id = Purple::PeerIdValue(500000 + i);
		auto added = Purple::AddListMember(
			text,
			Path(),
			u"colleagues"_q,
			id,
			Titles());
		if (!added.ok()) {
			CHECK(added.ok());
			return;
		}
		auto removed = Purple::RemoveListMember(
			added.text,
			Path(),
			u"colleagues"_q,
			id,
			Titles());
		if (!removed.ok()) {
			CHECK(removed.ok());
			return;
		}
		text = removed.text;
	}
	CHECK_EQ(text, original);

	// And a long run of adds keeps the file parseable and ordered throughout.
	auto grown = Example();
	auto expected = std::vector<Purple::PeerIdValue>{ 123456789, 987654321 };
	for (auto i = 0; i != 50; ++i) {
		const auto id = Purple::PeerIdValue(600000 + i);
		auto added = Purple::AddListMember(
			grown,
			Path(),
			u"colleagues"_q,
			id,
			Titles());
		if (!added.ok()) {
			CHECK(added.ok());
			return;
		}
		grown = added.text;
		expected.push_back(id);
	}
	CHECK_EQ(Purple::ListMembers(grown, Path(), u"colleagues"_q), expected);
	CHECK(grown.contains(u"# who I actually work with"_q));
	CHECK(Parse(grown).ok());
}

void TestSpliceOddFormatting() {
	Begin("splice odd formatting");

	// Comments and blank lines between members must survive a removal of a
	// neighbour, which is the whole reason removal is line-based.
	const auto text = uR"([lists.a]
members = [
  1,  # first

  # a note about the next one
  2,
  3,
]
)"_q;
	const auto result = Purple::RemoveListMember(
		text,
		Path(),
		u"a"_q,
		2,
		Titles());
	CHECK(result.ok());
	CHECK(result.text.contains(u"# a note about the next one"_q));
	CHECK(result.text.contains(u"1,  # first"_q));
	CHECK_EQ(Purple::ListMembers(result.text, Path(), u"a"_q),
		(std::vector<Purple::PeerIdValue>{ 1, 3 }));

	// Tabs are indentation too.
	const auto tabbed = u"[lists.a]\nmembers = [\n\t1,\n]\n"_q;
	const auto added = Purple::AddListMember(
		tabbed,
		Path(),
		u"a"_q,
		2,
		Titles());
	CHECK(added.ok());
	CHECK(added.text.contains(u"\n\t2,"_q));
}

void TestSpliceCrlf() {
	Begin("splice CRLF");

	const auto text = u"[lists.a]\r\nmembers = [\r\n  1,\r\n]\r\n"_q;
	const auto added = Purple::AddListMember(
		text,
		Path(),
		u"a"_q,
		2,
		Titles());
	CHECK(added.ok());
	CHECK(Parse(added.text).ok());
	CHECK_EQ(Purple::ListMembers(added.text, Path(), u"a"_q),
		(std::vector<Purple::PeerIdValue>{ 1, 2 }));

	// No stray lone-LF line is introduced into a CRLF file.
	CHECK(!added.text.contains(u"\n"_q)
		|| added.text.count(u"\r\n"_q) == added.text.count('\n'));
}

void TestNameSanitising() {
	Begin("name sanitising");

	// A display name is user-controlled text landing in a TOML file; a newline
	// in it would otherwise write a second line into the array.
	const auto result = Purple::AddListMember(
		u"[lists.a]\nmembers = [\n]\n"_q,
		Path(),
		u"a"_q,
		5,
		[](Purple::PeerIdValue) {
			return u"evil\n999999, # injected"_q;
		});
	CHECK(result.ok());
	CHECK_EQ(Purple::ListMembers(result.text, Path(), u"a"_q),
		(std::vector<Purple::PeerIdValue>{ 5 }));
	CHECK(Parse(result.text).ok());
}

// A preset with two views, the first of them already pinning somebody. The
// pinned array is written the way the splice writes one, so a round trip can be
// checked byte for byte rather than "near enough".
[[nodiscard]] QString Viewed() {
	return uR"(# my settings

[lists.a]
kinds = ["private"]

[presets.work]
list_order = [ { list = "a" } ]

# the tab I actually watch
[[presets.work.views]]
name = "Focus"
pinned = [
  123456789, # Ali Rezaei
]
list_order = [ { list = "a" } ]

[[presets.work.views]]
name = "Rest"
list_order = [ { list = "a" } ]
)"_q;
}

void TestSpliceViewPinned() {
	Begin("splice view pins");

	using Ids = std::vector<Purple::PeerIdValue>;
	const auto before = Viewed();
	CHECK_EQ(Purple::ViewPinned(before, Path(), u"work"_q, u"Focus"_q),
		(Ids{ 123456789 }));
	CHECK(Purple::ViewPinned(before, Path(), u"work"_q, u"Rest"_q).empty());

	// Reordering rewrites the array and nothing else in the file.
	const auto moved = Purple::SetViewPinned(
		before,
		Path(),
		u"work"_q,
		u"Focus"_q,
		Ids{ 987654321, 123456789 },
		Titles());
	CHECK(moved.ok());
	CHECK(moved.changed);
	CHECK_EQ(Purple::ViewPinned(moved.text, Path(), u"work"_q, u"Focus"_q),
		(Ids{ 987654321, 123456789 }));
	CHECK(moved.text.contains(u"# my settings"_q));
	CHECK(moved.text.contains(u"# the tab I actually watch"_q));
	CHECK(moved.text.contains(u"\n  987654321, # Backend Team\n"_q));

	// The other view is not the one that moved, and stays without a key.
	CHECK(Purple::ViewPinned(moved.text, Path(), u"work"_q, u"Rest"_q).empty());
	CHECK_EQ(moved.text.count(u"pinned = ["_q), 1);

	// And back again, byte for byte. This is the property that matters: the
	// file is the user's, and the app has to be able to touch one array in it
	// without leaving a trace anywhere else.
	const auto back = Purple::SetViewPinned(
		moved.text,
		Path(),
		u"work"_q,
		u"Focus"_q,
		Ids{ 123456789 },
		Titles());
	CHECK(back.ok());
	CHECK(back.changed);
	CHECK_EQ(back.text, before);

	// Writing what is already there writes nothing.
	const auto same = Purple::SetViewPinned(
		before,
		Path(),
		u"work"_q,
		u"Focus"_q,
		Ids{ 123456789 },
		Titles());
	CHECK(same.ok());
	CHECK(!same.changed);
	CHECK_EQ(same.text, before);

	// A view with no array yet gets one, under its own name rather than at the
	// top of the block, and matched case-insensitively the way the parser
	// decides two views are the same one.
	const auto added = Purple::SetViewPinned(
		before,
		Path(),
		u"work"_q,
		u"rest"_q,
		Ids{ 111222333 },
		Titles());
	CHECK(added.ok());
	CHECK(added.changed);
	CHECK_EQ(Purple::ViewPinned(added.text, Path(), u"work"_q, u"Rest"_q),
		(Ids{ 111222333 }));
	CHECK(added.text.contains(
		u"name = \"Rest\"\npinned = [\n  111222333, # Maman\n]\n"_q));

	// Emptying leaves the array rather than the key, which is the shape the
	// next pin will write into.
	const auto emptied = Purple::SetViewPinned(
		before,
		Path(),
		u"work"_q,
		u"Focus"_q,
		Ids(),
		Titles());
	CHECK(emptied.ok());
	CHECK(emptied.changed);
	CHECK(Purple::ViewPinned(emptied.text, Path(), u"work"_q, u"Focus"_q)
		.empty());
	CHECK(emptied.text.contains(u"pinned = [\n]\n"_q));

	// Nothing is written for a preset or a view that is not there.
	for (const auto &[preset, view] : { std::pair{ u"work"_q, u"Gone"_q },
			std::pair{ u"missing"_q, u"Focus"_q } }) {
		const auto refused = Purple::SetViewPinned(
			before,
			Path(),
			preset,
			view,
			Ids{ 123456789 },
			Titles());
		CHECK(!refused.ok());
		CHECK(!refused.changed);
		CHECK_EQ(refused.text, before);
	}

	// An inline view is refused rather than mangled: there is no line to edit.
	const auto inlined = uR"([presets.work]
list_order = [ { list = "a" } ]
views = [ { name = "Focus", list_order = [ { list = "a" } ] } ]
)"_q;
	const auto squashed = Purple::SetViewPinned(
		inlined,
		Path(),
		u"work"_q,
		u"Focus"_q,
		Ids{ 123456789 },
		Titles());
	CHECK(!squashed.ok());
	CHECK_EQ(squashed.text, inlined);

	// A file that does not parse is never written to, same as every other
	// splice: the user may be halfway through an edit of their own.
	const auto broken = Purple::SetViewPinned(
		u"[presets.work"_q,
		Path(),
		u"work"_q,
		u"Focus"_q,
		Ids{ 123456789 },
		Titles());
	CHECK(!broken.ok());
	CHECK(!broken.changed);
}

void TestSetTableBool() {
	Begin("set table bool");

	// The Premium toggle writes into a file the user owns, so it may change
	// nothing but the one token it is responsible for.
	const auto text = uR"(# my settings
[premium]
# keep the ads away
enabled   =    true   # this comment matters

[other]
untouched = 1
)"_q;
	const auto off = Purple::SetTableBool(
		text,
		Path(),
		u"premium"_q,
		u"enabled"_q,
		false);
	CHECK(off.ok());
	CHECK(off.changed);
	CHECK(off.text.contains(u"enabled   =    false   # this comment matters"_q));
	CHECK(off.text.contains(u"# keep the ads away"_q));
	CHECK(off.text.contains(u"# my settings"_q));
	CHECK(off.text.contains(u"untouched = 1"_q));
	CHECK_EQ(off.text.count('\n'), text.count('\n'));

	// Setting what is already set writes nothing at all.
	const auto same = Purple::SetTableBool(
		text,
		Path(),
		u"premium"_q,
		u"enabled"_q,
		true);
	CHECK(same.ok());
	CHECK(!same.changed);
	CHECK_EQ(same.text, text);

	// A missing key joins the existing table rather than starting a new one.
	const auto added = Purple::SetTableBool(
		u"[premium]\n# a note\n\n[other]\nx = 1\n"_q,
		Path(),
		u"premium"_q,
		u"enabled"_q,
		true);
	CHECK(added.ok());
	CHECK(added.text.contains(u"enabled = true"_q));
	CHECK(added.text.contains(u"# a note"_q));
	CHECK_EQ(Parse(added.text).settings.premium.enabled, true);
	CHECK(Parse(added.text).ok());

	// A missing table is appended, and an empty file gains no leading blank.
	const auto fresh = Purple::SetTableBool(
		QString(),
		Path(),
		u"premium"_q,
		u"enabled"_q,
		false);
	CHECK(fresh.ok());
	CHECK_EQ(fresh.text, u"[premium]\nenabled = false\n"_q);

	const auto appended = Purple::SetTableBool(
		u"[other]\nx = 1\n"_q,
		Path(),
		u"premium"_q,
		u"enabled"_q,
		false);
	CHECK(appended.ok());
	CHECK(appended.text.startsWith(u"[other]\nx = 1\n"_q));
	CHECK(appended.text.contains(u"[premium]\nenabled = false"_q));
	CHECK(Parse(appended.text).ok());

	// A file mid-edit is left exactly as it is.
	const auto broken = Purple::SetTableBool(
		u"[premium\nenabled = true"_q,
		Path(),
		u"premium"_q,
		u"enabled"_q,
		false);
	CHECK(!broken.ok());
	CHECK(!broken.changed);
	CHECK_EQ(broken.text, u"[premium\nenabled = true"_q);
}

void TestStateRoundTrip() {
	Begin("state round trip");

	auto state = Purple::State();
	state.activePreset = u"work"_q;
	state.activeSource = Purple::PresetSource::Schedule;
	state.previousPreset = u"normal"_q;
	state.previousSource = Purple::PresetSource::Manual;
	state.focusActive = true;
	state.focusSeen = true;
	state.schedulePaused = true;
	state.scheduleTarget = u"work"_q;
	state.peekActive = true;
	state.peekDeadlineUnix = 1755400000;
	state.resolvedCache.preset = u"work"_q;
	state.resolvedCache.viewName = u"Deep Work"_q;
	state.resolvedCache.hideEverywhere = true;
	state.resolvedCache.lists = {
		{ u"os"_q, Purple::ShowMode::Always, true },
		{ u"people"_q, Purple::ShowMode::Mention, false },
	};
	state.resolvedCache.folders = {
		{ .name = u"Music"_q, .notify = false },
		{ .name = u"Family"_q, .include = Purple::FolderInclude::All },
		{ .name = Purple::AllFoldersName(), .show = false, },
	};
	state.resolvedCache.views = {
		{
			u"Focus"_q,
			{ 5, 6 },
			{ { u"os"_q, Purple::ShowMode::Always, true } },
		},
	};

	const auto text = Purple::SerializeState(state);
	const auto back = Purple::ParseState(text, u"state.toml"_q);
	CHECK_EQ(back.activePreset, u"work"_q);
	CHECK(back.activeSource == Purple::PresetSource::Schedule);
	CHECK_EQ(back.previousPreset, u"normal"_q);
	CHECK(back.previousSource == Purple::PresetSource::Manual);
	CHECK(back.focusActive);
	CHECK(back.focusSeen);
	CHECK(back.schedulePaused);
	CHECK_EQ(back.scheduleTarget, u"work"_q);
	CHECK(back.peekActive);
	CHECK_EQ(back.peekDeadlineUnix, int64(1755400000));

	CHECK(back.resolvedCache.valid());
	CHECK_EQ(back.resolvedCache.preset, u"work"_q);
	CHECK_EQ(back.resolvedCache.viewName, u"Deep Work"_q);
	CHECK(back.resolvedCache.hideEverywhere);
	CHECK_EQ(back.resolvedCache.lists.size(), size_t(2));
	CHECK_EQ(back.resolvedCache.lists[1].list, u"people"_q);
	CHECK_EQ(Mode(back.resolvedCache.lists[1].show),
		Mode(Purple::ShowMode::Mention));
	CHECK(!back.resolvedCache.lists[1].notify);
	
	
	CHECK_EQ(int(back.resolvedCache.folders.size()), 3);
	CHECK_EQ(back.resolvedCache.folders[0].name, u"Music"_q);
	CHECK(back.resolvedCache.folders[0].notify.has_value());
	CHECK(!back.resolvedCache.folders[0].notify.value_or(true));
	CHECK(!back.resolvedCache.folders[0].include.has_value());
	CHECK_EQ(back.resolvedCache.folders[1].name, u"Family"_q);
	CHECK_EQ(
		int(back.resolvedCache.folders[1].include.value_or(
			Purple::FolderInclude::None)),
		int(Purple::FolderInclude::All));
	CHECK(Purple::IsAllFolders(back.resolvedCache.folders[2]));
	CHECK(!back.resolvedCache.folders[2].show.value_or(true));

	CHECK_EQ(int(back.resolvedCache.views.size()), 1);
	CHECK_EQ(back.resolvedCache.views[0].name, u"Focus"_q);
	CHECK_EQ(back.resolvedCache.views[0].pinned,
		(std::vector<Purple::PeerIdValue>{ 5, 6 }));
	CHECK_EQ(int(back.resolvedCache.views[0].lists.size()), 1);
	CHECK_EQ(back.resolvedCache.views[0].lists[0].list, u"os"_q);

	// Serialising the round-tripped state reproduces the file exactly, so
	// nothing drifts as the app rewrites it over and over.
	CHECK_EQ(Purple::SerializeState(back), text);
}

void TestStateDefaults() {
	Begin("state defaults");

	// No file, and a file we cannot read, both start from stock behaviour
	// rather than from a preset the user never chose.
	for (const auto &text : { QString(), u"active_preset = ["_q }) {
		const auto state = Purple::ParseState(text, u"state.toml"_q);
		CHECK_EQ(state.activePreset, u"normal"_q);
		CHECK(state.activeSource == Purple::PresetSource::Manual);
		CHECK(!state.schedulePaused);
		CHECK(!state.focusActive);
		CHECK(!state.focusSeen);
		CHECK(!state.peekActive);
		CHECK(!state.resolvedCache.valid());
	}

	// A cache that names a preset but describes no lists is not usable: the
	// engine would resolve nothing and quietly default every chat.
	const auto partial = Purple::ParseState(
		u"active_preset = \"work\"\n[resolved_cache]\npreset = \"work\"\n"_q,
		u"state.toml"_q);
	CHECK_EQ(partial.activePreset, u"work"_q);
	CHECK(!partial.resolvedCache.valid());

	// An unknown source name is not a reason to refuse the rest of the file.
	const auto odd = Purple::ParseState(
		u"active_preset = \"work\"\nactive_preset_source = \"telepathy\"\n"_q,
		u"state.toml"_q);
	CHECK_EQ(odd.activePreset, u"work"_q);
	CHECK(odd.activeSource == Purple::PresetSource::Manual);
}

void TestStateQuoting() {
	Begin("state quoting");

	// Preset names come out of the hand-written settings.toml, so they can
	// hold anything a TOML key can hold.
	auto state = Purple::State();
	state.activePreset = u"quote\" and \\ backslash"_q;
	state.resolvedCache.preset = state.activePreset;
	state.resolvedCache.lists = { { u"tab\there"_q, Purple::ShowMode::Always, true } };

	const auto text = Purple::SerializeState(state);
	const auto back = Purple::ParseState(text, u"state.toml"_q);
	CHECK_EQ(back.activePreset, state.activePreset);
	CHECK(back.resolvedCache.valid());
	CHECK_EQ(back.resolvedCache.lists[0].list, u"tab\there"_q);
}

// A file exercising every resolution rule at once: a locked list, a chat in two
// lists, a three-deep inheritance chain, and a child that empties its folders.
[[nodiscard]] QString Presets() {
	return uR"(
[lists.os]
members = [ 100 ]

[lists.emergency]
members = [ 200, 300 ]

[lists.colleagues]
members = [ 300, 400 ]

[lists.private]
kinds = ["private"]

[lists.groups]
kinds = ["groups"]

[lists.channels]
kinds = ["channels"]

[lists.bots]
kinds = ["bots"]

[lists.everything]
kinds = ["private", "groups", "channels", "bots"]

[list_sets.protected]
list_order = [
  { list = "os",        show_mode = "always", notify_p = true },
  { list = "emergency", show_mode = "always", notify_p = true },
]

[presets.work]
list_order = [
  "*protected",
  { list = "colleagues", show_mode = "mention",  notify_p = true },
  { list = "private",    show_mode = "always",  notify_p = false },
  { list = "groups",     show_mode = "mention",  notify_p = true },
  { list = "channels",   show_mode = "never", notify_p = false },
  { list = "bots",       show_mode = "always",  notify_p = true },
]
folders = [ { name = "Music", notify_p = false } ]

[presets.strict]
list_order = [
  "*protected",
  { list = "colleagues", show_mode = "always",  notify_p = true },
  { list = "private",    show_mode = "never", notify_p = false },
  { list = "groups",     show_mode = "mention",  notify_p = true },
  { list = "channels",   show_mode = "never", notify_p = false },
]
folders = [ "*ALL" ]

[presets.lockdown]
list_order = [ "*protected" ]
folders = []
)"_q;
}

void TestResolveBasics() {
	Begin("resolve basics");

	const auto parsed = Parse(Presets());
	CHECK(parsed.ok());

	const auto work = Purple::Resolve(parsed.settings, u"work"_q);
	CHECK(work.has_value());
	CHECK(!work->normal);

	// The preset's own entries, in its own order, with "*protected" spliced in
	// at the front - not every list in the file.
	CHECK_EQ(work->lists.size(), size_t(7));
	CHECK_EQ(work->lists[0].list, u"os"_q);
	CHECK_EQ(Mode(work->list(u"private"_q)->show),
		Mode(Purple::ShowMode::Always));
	CHECK(!work->list(u"private"_q)->notify);
	CHECK_EQ(Mode(work->list(u"channels"_q)->show),
		Mode(Purple::ShowMode::Never));
	CHECK(work->list(u"everything"_q) == nullptr);

	// notify collapses here; show deliberately does not, because its default
	// depends on the chat and one entry can claim several kinds.
	CHECK_EQ(Mode(work->list(u"os"_q)->show), Mode(Purple::ShowMode::Always));
	CHECK_EQ(Mode(work->list(u"colleagues"_q)->show),
		Mode(Purple::ShowMode::Mention));

	CHECK_EQ(int(work->folders.size()), 1);
	CHECK_EQ(work->folders[0].name, u"Music"_q);
	CHECK(work->views.empty());

	// Off unless a preset asks: hiding a chat from the work view is a small
	// thing to get wrong, and taking it out of the forward picker is not.
	CHECK(!work->hideEverywhere);

	// Normal is a bypass, not a preset with everything switched on.
	const auto normal = Purple::Resolve(parsed.settings, u"normal"_q);
	CHECK(normal.has_value());
	CHECK(normal->normal);
	CHECK(normal->lists.empty());
	CHECK(Purple::MatchList(
		parsed.settings,
		*normal,
		400,
		Purple::ChatKind::Private) == nullptr);
	CHECK(Purple::Visible(
		parsed.settings,
		*normal,
		400,
		Purple::ChatKind::Private).show
		!= Purple::ShowMode::Never);

	// A preset the settings do not describe cannot be resolved, and the caller
	// is meant to fall back rather than to defaults. With no inheritance there
	// is no implicit root to land on either.
	CHECK(!Purple::Resolve(parsed.settings, u"ghost"_q).has_value());
	CHECK(!Purple::Resolve(parsed.settings, u"default"_q).has_value());
	CHECK(!Purple::Resolve(parsed.settings, QString()).has_value());
}

void TestResolveViewName() {
	Begin("resolve view name");

	const auto named = Parse(uR"(
[presets.work]
default_view_name = "Deep Work"

[presets.plain]
list_order = []

[presets."deep focus"]
list_order = []
)"_q);
	CHECK(named.ok());
	const auto work = Purple::Resolve(named.settings, u"work"_q);
	const auto plain = Purple::Resolve(named.settings, u"plain"_q);
	const auto spaced = Purple::Resolve(named.settings, u"deep focus"_q);
	CHECK(work.has_value() && work->viewName == u"Deep Work"_q);
	CHECK(plain.has_value() && plain->viewName == u"Plain"_q);

	// Only the first letter, so a name with a space in it is not title-cased
	// into something the user did not write.
	CHECK(spaced.has_value() && spaced->viewName == u"Deep focus"_q);
	CHECK_EQ(Purple::DefaultViewName(u"work"_q), u"Work"_q);
	CHECK_EQ(Purple::DefaultViewName(u"WORK"_q), u"WORK"_q);
	CHECK(Purple::DefaultViewName(QString()).isEmpty());

	// A capital anywhere means the casing was chosen, so it is left alone
	// rather than tidied into something the user did not write.
	CHECK_EQ(Purple::DefaultViewName(u"iH"_q), u"iH"_q);
	CHECK_EQ(Purple::DefaultViewName(u"deep Focus"_q), u"deep Focus"_q);
	CHECK_EQ(Purple::DefaultViewName(u"eBay"_q), u"eBay"_q);

	// Digits and punctuation are not capitals and decide nothing.
	CHECK_EQ(Purple::DefaultViewName(u"p0"_q), u"P0"_q);
	CHECK_EQ(Purple::DefaultViewName(u"9to5"_q), u"9to5"_q);

	// The cache carries it, so a broken reload does not also rename the tab.
	const auto cached = Purple::FromCache(Purple::ToCache(*work));
	CHECK(cached.has_value() && cached->viewName == u"Deep Work"_q);

	// A cache written by a build that did not know the key still names the
	// tab something, rather than leaving it blank.
	auto older = Purple::ToCache(*work);
	older.viewName = QString();
	const auto restored = Purple::FromCache(older);
	CHECK(restored.has_value() && restored->viewName == u"Work"_q);

	// The same answer from the parsed preset, without resolving it. This is
	// what the preset picker reads: it lists presets from the file, and a row
	// naming one has to agree with the tab that preset produces.
	const auto presets = &named.settings;
	CHECK(presets->preset(u"work"_q) != nullptr);
	CHECK_EQ(
		Purple::PresetTitle(*presets->preset(u"work"_q)),
		u"Deep Work"_q);
	CHECK_EQ(Purple::PresetTitle(*presets->preset(u"plain"_q)), u"Plain"_q);
	CHECK_EQ(
		Purple::PresetTitle(*presets->preset(u"deep focus"_q)),
		u"Deep focus"_q);

	// The two-argument form is what the gate has in hand, since a resolution
	// carries the name and the view name rather than the preset.
	CHECK_EQ(Purple::PresetTitle(u"work"_q, QString()), u"Work"_q);
	CHECK_EQ(Purple::PresetTitle(u"work"_q, u"Deep Work"_q), u"Deep Work"_q);

	// A name whose casing was chosen comes back as it was written, whichever
	// letter carries the capital.
	CHECK_EQ(Purple::PresetTitle(u"WORK"_q, QString()), u"WORK"_q);
	CHECK_EQ(Purple::PresetTitle(u"iH"_q, QString()), u"iH"_q);
	CHECK(Purple::PresetTitle(QString(), QString()).isEmpty());
}

void TestFallThrough() {
	Begin("fall-through");

	const auto parsed = Parse(Presets());

	// lockdown names two lists and nothing else. Everything they do not hold is
	// hidden AND silenced: a preset names what gets through, and saying nothing
	// about a chat is saying no.
	const auto lockdown = Purple::Resolve(parsed.settings, u"lockdown"_q);
	CHECK(lockdown.has_value());
	CHECK(Purple::MatchList(
		parsed.settings,
		*lockdown,
		999,
		Purple::ChatKind::Private) == nullptr);
	const auto missed = Purple::Visible(
		parsed.settings,
		*lockdown,
		999,
		Purple::ChatKind::Private);
	CHECK_EQ(Mode(missed.show), Mode(Purple::ShowMode::Never));
	CHECK(!missed.notify);
	

	// What it does name still comes through, by id and by kind alike.
	const auto kept = Purple::Visible(
		parsed.settings,
		*lockdown,
		100,
		Purple::ChatKind::Channel);
	CHECK(kept.show != Purple::ShowMode::Never);
	CHECK(kept.notify);

	// An entry naming a list that does not exist claims nothing, so the chat
	// falls through rather than being swallowed by a name that means nothing.
	const auto ghost = Parse(uR"(
[lists.a]
kinds = ["private"]

[presets.work]
list_order = [ { list = "ghosts", show_mode = "always" }, { list = "a", show_mode = "always" } ]
)"_q);
	const auto resolved = Purple::Resolve(ghost.settings, u"work"_q);
	CHECK_EQ(Purple::MatchList(
		ghost.settings,
		*resolved,
		1,
		Purple::ChatKind::Private)->list, u"a"_q);
}

void TestMatchPriority() {
	Begin("match priority");

	const auto parsed = Parse(Presets());
	const auto work = Purple::Resolve(parsed.settings, u"work"_q);

	// 300 is in both emergency and colleagues; the earlier entry wins.
	const auto matched = Purple::MatchList(
		parsed.settings,
		*work,
		300,
		Purple::ChatKind::Private);
	CHECK(matched != nullptr);
	CHECK_EQ(matched->list, u"emergency"_q);

	// 400 is only in colleagues.
	CHECK_EQ(Purple::MatchList(
		parsed.settings,
		*work,
		400,
		Purple::ChatKind::Private)->list, u"colleagues"_q);

	// A member id beats a kind rule further down, which is the whole point of
	// being able to name one chat out of a sweeping rule.
	CHECK_EQ(Purple::MatchList(
		parsed.settings,
		*work,
		100,
		Purple::ChatKind::Channel)->list, u"os"_q);
	CHECK_EQ(Purple::MatchList(
		parsed.settings,
		*work,
		999,
		Purple::ChatKind::Channel)->list, u"channels"_q);
	CHECK_EQ(Purple::MatchList(
		parsed.settings,
		*work,
		999,
		Purple::ChatKind::Bot)->list, u"bots"_q);

	// A list matches when either half does; both empty matches nothing.
	const auto os = parsed.settings.list(u"os"_q);
	CHECK(Purple::ListHolds(*os, 100, Purple::ChatKind::Bot));
	CHECK(!Purple::ListHolds(*os, 101, Purple::ChatKind::Bot));
	const auto bots = parsed.settings.list(u"bots"_q);
	CHECK(Purple::ListHolds(*bots, 12345, Purple::ChatKind::Bot));
	CHECK(!Purple::ListHolds(*bots, 12345, Purple::ChatKind::Group));

	// Moving an entry flips which of the two wins - the order in the preset IS
	// the priority, and nothing else decides it. The spread stays in the file,
	// just below colleagues, so this is a reorder rather than a deletion.
	const auto before =
		u"  \"*protected\",\n"
		"  { list = \"colleagues\", show_mode = \"mention\",  "
		"notify_p = true },\n"_q;
	auto swapped = Presets();
	CHECK(swapped.contains(before));
	swapped.replace(
		before,
		u"  { list = \"colleagues\", show_mode = \"mention\",  "
		"notify_p = true },\n  \"*protected\",\n"_q);
	const auto reparsed = Parse(swapped);
	CHECK(reparsed.ok());
	const auto after = Purple::Resolve(reparsed.settings, u"work"_q);

	// Still seven entries: emergency moved, it did not disappear.
	CHECK_EQ(after->lists.size(), size_t(7));
	CHECK(after->list(u"emergency"_q) != nullptr);
	CHECK_EQ(Purple::MatchList(
		reparsed.settings,
		*after,
		300,
		Purple::ChatKind::Private)->list, u"colleagues"_q);

	// And 200, which only emergency holds, still lands there.
	CHECK_EQ(Purple::MatchList(
		reparsed.settings,
		*after,
		200,
		Purple::ChatKind::Private)->list, u"emergency"_q);
}

void TestViewMembership() {
	Begin("view membership");

	const auto parsed = Parse(uR"(
[lists.team]
members = [ 10, 20 ]

[lists.people]
kinds = ["private"]

[presets.work]
list_order = [ { list = "people", show_mode = "always" } ]

[[presets.work.views]]
name   = "Focus"
pinned = [ 20, 10 ]
list_order = [
  { list = "team",   show_mode = "never" },
  { list = "people", show_mode = "always" },
]
)"_q);
	CHECK(parsed.ok());
	const auto work = Purple::Resolve(parsed.settings, u"work"_q);
	CHECK(work.has_value());
	CHECK_EQ(int(work->views.size()), 1);

	const auto &view = work->views[0];
	CHECK_EQ(view.name, u"Focus"_q);
	CHECK_EQ(view.pinned, (std::vector<Purple::PeerIdValue>{ 20, 10 }));

	// The first entry claims the team ids and drops them; everyone else who is
	// a DM is on the tab. A kind nothing names is off it.
	CHECK(!Purple::ViewHolds(
		parsed.settings,
		view,
		10,
		Purple::ChatKind::Private));
	CHECK(Purple::ViewHolds(
		parsed.settings,
		view,
		30,
		Purple::ChatKind::Private));
	CHECK(!Purple::ViewHolds(
		parsed.settings,
		view,
		30,
		Purple::ChatKind::Bot));

	// A view selects membership; it never changes what a chat may do. Both
	// those team ids are still shown and still notifying on the main view.
	const auto visible = Purple::Visible(
		parsed.settings,
		*work,
		10,
		Purple::ChatKind::Private);
	CHECK(visible.show != Purple::ShowMode::Never);
	CHECK(visible.notify);

	// And the cache carries the whole thing, pins included.
	const auto cached = Purple::FromCache(Purple::ToCache(*work));
	CHECK(cached.has_value());
	CHECK_EQ(int(cached->views.size()), 1);
	CHECK_EQ(cached->views[0].name, u"Focus"_q);
	CHECK_EQ(cached->views[0].pinned,
		(std::vector<Purple::PeerIdValue>{ 20, 10 }));
	CHECK(!Purple::ViewHolds(
		parsed.settings,
		cached->views[0],
		10,
		Purple::ChatKind::Private));
}

void TestMentionGate() {
	Begin("mention gate");

	const auto parsed = Parse(Presets());
	const auto work = Purple::Resolve(parsed.settings, u"work"_q);
	const auto strict = Purple::Resolve(parsed.settings, u"strict"_q);

	// A visible group is gated when its entry asks for it.
	const auto group = Purple::Visible(
		parsed.settings,
		*work,
		999,
		Purple::ChatKind::Group);
	CHECK(group.show != Purple::ShowMode::Never);
	CHECK_EQ(Mode(group.show), Mode(Purple::ShowMode::Mention));

	// Channels and DMs never are, whatever the entry says.
	CHECK(Purple::Visible(
		parsed.settings,
		*work,
		999,
		Purple::ChatKind::Private).show
		!= Purple::ShowMode::Mention);
	CHECK(Purple::Visible(
		parsed.settings,
		*work,
		100,
		Purple::ChatKind::Channel).show
		!= Purple::ShowMode::Mention);

	// The gate is per entry, so strict can leave colleagues ungated while
	// gating everything the "groups" entry sweeps up.
	CHECK(Purple::Visible(
		parsed.settings,
		*strict,
		400,
		Purple::ChatKind::Group).show
		!= Purple::ShowMode::Mention);
	CHECK_EQ(Mode(Purple::Visible(
		parsed.settings,
		*strict,
		999,
		Purple::ChatKind::Group).show),
		Mode(Purple::ShowMode::Mention));

	// A chat nothing claims is hidden whether or not anyone mentioned us in it.
	const auto lockdown = Purple::Resolve(parsed.settings, u"lockdown"_q);
	const auto hidden = Purple::Visible(
		parsed.settings,
		*lockdown,
		999,
		Purple::ChatKind::Group);
	CHECK_EQ(Mode(hidden.show), Mode(Purple::ShowMode::Never));
	CHECK(hidden.show != Purple::ShowMode::Mention);
}

void TestPeek() {
	Begin("peek");

	const auto parsed = Parse(Presets());
	auto work = *Purple::Resolve(parsed.settings, u"work"_q);

	// What work does without a peek: channels are gone, DMs are silent, groups
	// wait for a mention.
	const auto channel = [&] {
		return Purple::Visible(
			parsed.settings,
			work,
			999,
			Purple::ChatKind::Channel);
	};
	const auto priv = [&] {
		return Purple::Visible(
			parsed.settings,
			work,
			999,
			Purple::ChatKind::Private);
	};
	const auto group = [&] {
		return Purple::Visible(
			parsed.settings,
			work,
			999,
			Purple::ChatKind::Group);
	};
	CHECK_EQ(Mode(channel().show), Mode(Purple::ShowMode::Never));
	CHECK(!priv().notify);
	CHECK_EQ(Mode(group().show), Mode(Purple::ShowMode::Mention));

	// A peek reveals everything the preset hides and lifts the mention gate,
	// and deliberately leaves the silencing alone: it is a look at the chat
	// list, not two minutes of notifications for chats already on the screen.
	work.peeking = true;
	CHECK(channel().show != Purple::ShowMode::Never);

	// Revealed and still silent, which is the rule stated as plainly as it can
	// be: a peek changes what you can see and nothing about what may interrupt.
	CHECK(!channel().notify);
	CHECK(group().show != Purple::ShowMode::Never);
	CHECK(group().show != Purple::ShowMode::Mention);
	CHECK(priv().show != Purple::ShowMode::Never);
	CHECK(!priv().notify);

	// It never reaches the cache, so a resolution restored from state.toml
	// cannot come back still revealed with nothing left to end it.
	const auto restored = Purple::FromCache(Purple::ToCache(work));
	CHECK(restored.has_value());
	CHECK(!restored->peeking);
	CHECK_EQ(Mode(restored->list(u"channels"_q)->show),
		Mode(Purple::ShowMode::Never));

	// The deadline is what expires a peek that outlived the app.
	auto state = Purple::State();
	state.peekActive = true;
	state.peekDeadlineUnix = 2000;
	CHECK(Purple::PeekLive(state, 1999));
	CHECK(!Purple::PeekLive(state, 2000));
	CHECK(!Purple::PeekLive(state, 5000));

	// No deadline is auto_off = "off": it runs until it is turned off by hand.
	state.peekDeadlineUnix = 0;
	CHECK(Purple::PeekLive(state, 5000));

	state.peekActive = false;
	CHECK(!Purple::PeekLive(state, 0));
}

void TestNamedExplicitly() {
	Begin("named explicitly");

	const auto parsed = Parse(uR"(
[lists.close_people]
members = [ 111, 222 ]

[lists.everyone]
kinds = ["private"]

[lists.banned]
members = [ 333 ]

[presets.work]
list_order = [
  { list = "close_people" },
  { list = "banned", show_mode = "never" },
  { list = "everyone" },
]

[[presets.work.views]]
name       = "P0"
list_order = [ { list = "close_people" } ]

[presets.plain]
list_order = [ { list = "everyone" } ]
)"_q);
	CHECK(parsed.ok());
	const auto work = Purple::Resolve(parsed.settings, u"work"_q);
	const auto plain = Purple::Resolve(parsed.settings, u"plain"_q);
	CHECK(work.has_value() && plain.has_value());

	const auto named = [&](const std::optional<Purple::Resolved> &resolved,
			Purple::PeerIdValue id) {
		return Purple::NamedExplicitly(parsed.settings, *resolved, id);
	};

	// Written into a list's members, and that list is ordered: yes.
	CHECK(named(work, 111));
	CHECK(named(work, 222));

	// The whole point of the predicate. 999 is a private chat, so
	// { list = "everyone" } matches it and it is very much in the view - but
	// nobody wrote it down, so it is not grounds for keeping an empty chat in
	// the chat list. Getting this wrong drags in every contact with no
	// conversation.
	CHECK(!named(work, 999));

	// Named in order to be hidden is not a request for it to be anywhere.
	CHECK(!named(work, 333));

	// A view counts, which is the case that motivated all of this: the main
	// order may gate the chat while a tab holds it unconditionally.
	const auto viewOnly = Parse(uR"(
[lists.close_people]
members = [ 111 ]

[presets.work]
list_order = []

[[presets.work.views]]
name       = "P0"
list_order = [ { list = "close_people" } ]
)"_q);
	CHECK(viewOnly.ok());
	const auto held = Purple::Resolve(viewOnly.settings, u"work"_q);
	CHECK(held.has_value());
	CHECK(Purple::NamedExplicitly(viewOnly.settings, *held, 111));

	// A preset that does not order the list gets nothing from it, even though
	// the list is right there in the file.
	CHECK(!named(plain, 111));

	// Normal is a bypass, and must not be keeping anything anywhere.
	const auto normal = Purple::Resolve(parsed.settings, Purple::NormalPreset());
	CHECK(normal.has_value());
	CHECK(!Purple::NamedExplicitly(parsed.settings, *normal, 111));

	// An id of zero is "we could not work out what this peer is", never a match.
	CHECK(!named(work, 0));
}

void TestRecent() {
	Begin("recent");

	// Off unless the file asks, and the narrow scope unless it says otherwise:
	// nothing a preset hides should start appearing because a key was added
	// with no `applies_to' beside it.
	const auto silent = Parse(u"[presets.work]\nlist_order = []\n"_q);
	CHECK(silent.ok());
	CHECK_EQ(silent.settings.recent.staySecondsAfterClose, 0);
	CHECK(silent.settings.recent.scope
		== Purple::RecentScope::AlreadyInView);

	const auto parsed = Parse(uR"(
[recent]
stay_visible_after_close = "2m"
applies_to = "any_open_chat"
)"_q);
	CHECK(parsed.ok());
	CHECK(parsed.warnings.empty());
	CHECK_EQ(parsed.settings.recent.staySecondsAfterClose, 120);
	CHECK(parsed.settings.recent.scope == Purple::RecentScope::AnyOpenChat);

	// The same duration spellings as [peek] auto_off, which is the point of
	// reusing ParseDuration rather than inventing a seconds-only key.
	const auto units = [](const QString &text) {
		const auto one = Parse(
			u"[recent]\nstay_visible_after_close = \"%1\"\n"_q.arg(text));
		return one.settings.recent.staySecondsAfterClose;
	};
	CHECK_EQ(units(u"90s"_q), 90);
	CHECK_EQ(units(u"1h"_q), 3600);
	CHECK_EQ(units(u"45"_q), 45);
	CHECK_EQ(units(u"off"_q), 0);

	// Unparseable leaves it off and says so, rather than guessing at a number.
	const auto broken = Parse(
		u"[recent]\nstay_visible_after_close = \"soon\"\n"_q);
	CHECK(broken.ok());
	CHECK_EQ(broken.settings.recent.staySecondsAfterClose, 0);
	CHECK_EQ(int(broken.warnings.size()), 1);

	// A misspelt scope keeps the narrow default rather than the widest one.
	const auto scope = Parse(uR"(
[recent]
stay_visible_after_close = "2m"
applies_to = "any_chat"
)"_q);
	CHECK(scope.ok());
	CHECK_EQ(int(scope.warnings.size()), 1);
	CHECK(scope.settings.recent.scope == Purple::RecentScope::AlreadyInView);

	CHECK(Purple::ParseRecentScope(u"  ANY_OPEN_CHAT  "_q)
		== Purple::RecentScope::AnyOpenChat);
	CHECK(Purple::ParseRecentScope(
		u"any_open_chat_except_in_folder"_q)
			== Purple::RecentScope::AnyOpenChatExceptInFolder);
	CHECK(!Purple::ParseRecentScope(QString()).has_value());

	// Round trip, so a warning can name back what it kept.
	for (const auto value : {
			Purple::RecentScope::AlreadyInView,
			Purple::RecentScope::AnyOpenChat,
			Purple::RecentScope::AnyOpenChatExceptInFolder }) {
		CHECK(Purple::ParseRecentScope(Purple::RecentScopeName(value))
			== value);
	}
}

void TestScheduleTarget() {
	Begin("schedule target");

	// 2026-08-17 is a Monday, so dayOfWeek() runs 1..7 across that week.
	const auto at = [](int weekday, int hour, int minute) {
		return QDateTime(
			QDate(2026, 8, 16 + weekday),
			QTime(hour, minute));
	};
	const auto rule = [](
			std::vector<int> days,
			int from,
			int till,
			const QString &preset) {
		auto result = Purple::ScheduleRule();
		result.days = std::move(days);
		result.from = from;
		result.till = till;
		result.preset = preset;
		return result;
	};
	const auto target = [](
			const Purple::Schedule &schedule,
			const QDateTime &when) {
		const auto result = Purple::ScheduleTarget(schedule, when);
		return result ? *result : u"<nothing>"_q;
	};

	// A schedule with no rules drives nothing. It must not read as "wants
	// Normal", or an empty section would override every other way of choosing.
	auto schedule = Purple::Schedule();
	CHECK_EQ(target(schedule, at(1, 10, 0)), u"<nothing>"_q);

	schedule.rules.push_back(rule({ 1, 2, 3, 4, 5 }, 9 * 60, 17 * 60, u"work"_q));
	CHECK_EQ(target(schedule, at(1, 10, 0)), u"work"_q);

	// Half-open: the start belongs to the window, the end does not, so
	// neighbouring windows cannot both claim the moment they meet.
	CHECK_EQ(target(schedule, at(1, 9, 0)), u"work"_q);
	CHECK_EQ(target(schedule, at(1, 8, 59)), u"normal"_q);
	CHECK_EQ(target(schedule, at(1, 16, 59)), u"work"_q);
	CHECK_EQ(target(schedule, at(1, 17, 0)), u"normal"_q);

	// Saturday is not listed.
	CHECK_EQ(target(schedule, at(6, 10, 0)), u"normal"_q);

	// Switching the whole schedule off is not the same as it wanting Normal:
	// it stops driving, and whatever is active stays.
	schedule.enabled = false;
	CHECK_EQ(target(schedule, at(1, 10, 0)), u"<nothing>"_q);
	schedule.enabled = true;

	// A window crossing midnight belongs to the day it starts on.
	auto night = Purple::Schedule();
	night.rules.push_back(rule({ 1 }, 22 * 60, 6 * 60, u"sleep"_q));
	CHECK_EQ(target(night, at(1, 22, 0)), u"sleep"_q);
	CHECK_EQ(target(night, at(1, 23, 59)), u"sleep"_q);
	CHECK_EQ(target(night, at(2, 5, 59)), u"sleep"_q);
	CHECK_EQ(target(night, at(2, 6, 0)), u"normal"_q);

	// ... and not to the morning of the day it is listed for, which is the
	// trap: Monday 02:00 is the tail of a Sunday window, not of Monday's.
	CHECK_EQ(target(night, at(1, 2, 0)), u"normal"_q);
	night.rules[0].days = { 7 };
	CHECK_EQ(target(night, at(1, 2, 0)), u"sleep"_q);

	// First match wins in file order, and a disabled rule is not a match.
	auto several = Purple::Schedule();
	several.rules.push_back(rule({ 1 }, 9 * 60, 17 * 60, u"first"_q));
	several.rules.push_back(rule({ 1 }, 9 * 60, 17 * 60, u"second"_q));
	CHECK_EQ(target(several, at(1, 10, 0)), u"first"_q);
	several.rules[0].enabled = false;
	CHECK_EQ(target(several, at(1, 10, 0)), u"second"_q);
}

void TestResolvedCache() {
	Begin("resolved cache");

	const auto parsed = Parse(Presets());
	const auto work = Purple::Resolve(parsed.settings, u"work"_q);
	const auto cache = Purple::ToCache(*work);
	CHECK(cache.valid());
	CHECK_EQ(cache.preset, u"work"_q);
	CHECK_EQ(cache.lists.size(), work->lists.size());

	// It survives the trip through state.toml, which is the point: a settings
	// file that stops parsing between runs must not reshuffle the chat list.
	const auto text = Purple::SerializeState([&] {
		auto state = Purple::State();
		state.activePreset = u"work"_q;
		state.resolvedCache = cache;
		return state;
	}());
	const auto reloaded = Purple::ParseState(text, u"state.toml"_q);
	const auto restored = Purple::FromCache(reloaded.resolvedCache);
	CHECK(restored.has_value());
	CHECK_EQ(restored->preset, u"work"_q);
	CHECK_EQ(Mode(restored->list(u"private"_q)->show),
		Mode(Purple::ShowMode::Always));
	CHECK(!restored->list(u"private"_q)->notify);
	CHECK_EQ(Mode(restored->list(u"channels"_q)->show),
		Mode(Purple::ShowMode::Never));
	CHECK_EQ(Mode(restored->list(u"colleagues"_q)->show),
		Mode(Purple::ShowMode::Mention));
	CHECK_EQ(int(restored->folders.size()), 1);

	// include_in_main_view_p survives the cache, since a broken settings.toml
	// must not quietly start hiding the chats a folder was pulling in.
	auto folders = std::vector<Purple::PresetFolder>{
		{ .name = u"Work"_q },
		{ .name = u"Family"_q, .include = Purple::FolderInclude::All },
		{ .name = u"Loud"_q, .include = Purple::FolderInclude::None },
	};
	CHECK_EQ(Purple::ExemptFolderList(folders).size(), size_t(1));
	CHECK_EQ(Purple::ExemptFolderList(folders).front().name, u"Family"_q);
	CHECK(Purple::ExemptFolderList({}).empty());

	// notify and include_in_main_view are independent: a folder can be silenced
	// without being pulled in, and pulled in without being silenced.
	folders.push_back({ .name = u"Noise"_q, .notify = false });
	folders.push_back({ .name = u"Loud2"_q, .notify = true });
	CHECK_EQ(Purple::SilencedFolderNames(folders).size(), size_t(1));
	CHECK_EQ(Purple::SilencedFolderNames(folders).front(), u"Noise"_q);
	CHECK_EQ(Purple::ExemptFolderList(folders).size(), size_t(1));
	CHECK(Purple::SilencedFolderNames({}).empty());

	// badge_p is its own axis: a folder can be pulled in, silenced and quiet
	// independently, and only an explicit false makes it quiet.
	CHECK(Purple::QuietFolderNames(folders).empty());
	folders.push_back({ .name = u"Hush"_q, .badge = false });
	folders.push_back({ .name = u"Counted"_q, .badge = true });
	CHECK_EQ(Purple::QuietFolderNames(folders).size(), size_t(1));
	CHECK_EQ(Purple::QuietFolderNames(folders).front(), u"Hush"_q);
	CHECK(Purple::QuietFolderNames({}).empty());

	// "pinned" and a mode ride along on the exempt entry, so the one walk in
	// History::purpleExemptFolderMode() has everything it needs.
	folders.push_back({
		.name = u"Music"_q,
		.showMode = Purple::ShowMode::Message,
		.include = Purple::FolderInclude::Pinned,
	});
	folders.push_back({ .name = u"Alone"_q });
	const auto exempt = Purple::ExemptFolderList(folders);
	CHECK_EQ(exempt.size(), size_t(2));
	CHECK_EQ(exempt.back().name, u"Music"_q);
	CHECK_EQ(int(exempt.back().include), int(Purple::FolderInclude::Pinned));
	CHECK_EQ(Mode(exempt.back().showMode), Mode(Purple::ShowMode::Message));
	CHECK_EQ(int(exempt.front().include), int(Purple::FolderInclude::All));
	CHECK(!exempt.front().showMode.has_value());

	auto withFolders = *work;
	withFolders.folders = folders;
	const auto cachedFolders = Purple::FromCache(
		Purple::ToCache(withFolders));
	CHECK(cachedFolders.has_value());
	CHECK_EQ(cachedFolders->exemptFolders.size(), size_t(2));
	CHECK_EQ(cachedFolders->exemptFolders.front().name, u"Family"_q);
	CHECK_EQ(cachedFolders->exemptFolders.back().name, u"Music"_q);
	CHECK_EQ(
		int(cachedFolders->exemptFolders.back().include),
		int(Purple::FolderInclude::Pinned));
	CHECK_EQ(
		Mode(cachedFolders->exemptFolders.back().showMode),
		Mode(Purple::ShowMode::Message));
	CHECK_EQ(cachedFolders->silencedFolders.size(), size_t(1));
	CHECK_EQ(cachedFolders->silencedFolders.front(), u"Noise"_q);
	CHECK_EQ(cachedFolders->quietFolders.size(), size_t(1));
	CHECK_EQ(cachedFolders->quietFolders.front(), u"Hush"_q);
	CHECK_EQ(int(cachedFolders->folders.size()), 9);

	// The marker survives too, so a broken reload does not take the whole
	// folder strip away along with everything else it cannot read.
	auto everyFolder = *work;
	everyFolder.folders = { { .name = Purple::AllFoldersName() } };
	const auto cachedAll = Purple::FromCache(Purple::ToCache(everyFolder));
	CHECK(cachedAll.has_value());
	CHECK_EQ(int(cachedAll->folders.size()), 1);
	CHECK(Purple::IsAllFolders(cachedAll->folders[0]));

	// Normal caches nothing - there is no resolution to remember.
	const auto normal = Purple::Resolve(parsed.settings, u"normal"_q);
	CHECK(!Purple::ToCache(*normal).valid());
	CHECK(!Purple::FromCache(Purple::ResolvedCache()).has_value());
}

} // namespace

int main() {
	TestLists();
	TestKinds();
	TestPresets();
	TestPresetPolicies();
	TestSpread();
	TestFolderSelection();
	TestShowModes();
	TestViews();
	TestMembers();
	TestScheduleAndFocus();
	TestScalarParsers();
	TestPremiumStillParses();
	TestBrokenFile();
	TestSpliceAdd();
	TestSpliceRemove();
	TestSpliceCanonicalises();
	TestSpliceMissingArray();
	TestSpliceStability();
	TestSpliceOddFormatting();
	TestSpliceCrlf();
	TestNameSanitising();
	TestSpliceViewPinned();
	TestSetTableBool();
	TestStateRoundTrip();
	TestStateDefaults();
	TestStateQuoting();
	TestResolveBasics();
	TestResolveViewName();
	TestFallThrough();
	TestMatchPriority();
	TestViewMembership();
	TestMentionGate();
	TestPeek();
	TestNamedExplicitly();
	TestRecent();
	TestScheduleTarget();
	TestResolvedCache();

	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
