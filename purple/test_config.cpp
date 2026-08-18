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

#include "purple/purple_settings.h"
#include "purple/purple_splice.h"
#include "purple/purple_state.h"

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

// The file the spec documents, used as the baseline for most checks.
[[nodiscard]] QString Example() {
	return uR"(# my settings

list_order = ["os", "emergency", "colleagues"]

[lists.os]
title  = "OS"
show   = true
notify = true
locked = true
members = [
  1234567890,   # My Todo Channel
]

[lists.emergency]
title  = "Emergency"
locked = true
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

[lists."@private"]
show   = true
notify = true

[presets.work]
inherit = "default"
groups_require_mention = true
folders = [ { name = "Music", notify = false } ]

[presets.work.overrides."@private"]
notify = false

[presets.strict]
inherit = "work"

[presets.strict.overrides."@private"]
show = false
)"_q;
}

void TestCatchAlls() {
	Begin("catch-alls");
	const auto result = Parse(Example());
	CHECK(result.ok());

	// Only @private is declared; the other three have to exist anyway, or the
	// engine would need an "unlisted chat" case everywhere.
	const auto names = ListNames(result.settings);
	CHECK_EQ(names.size(), qsizetype(7));
	CHECK_EQ(names.join(u","_q),
		u"os,emergency,colleagues,@private,@groups,@channels,@bots"_q);

	const auto groups = result.settings.list(u"@groups"_q);
	CHECK(groups != nullptr);
	CHECK(groups->show);
	CHECK(groups->notify);
	CHECK(groups->kind == Purple::ListKind::Groups);
	CHECK(groups->members.empty());
}

void TestListOrder() {
	Begin("list_order");

	// Names a list that does not exist, and forgets one that does.
	const auto result = Parse(uR"(
list_order = ["ghosts", "b"]

[lists.a]
title = "A"

[lists.b]
title = "B"
)"_q);
	CHECK(result.ok());
	CHECK(WarnsAbout(result, u"'ghosts'"_q));
	CHECK(WarnsAbout(result, u"missing from 'list_order'"_q));
	CHECK_EQ(ListNames(result.settings).join(u","_q),
		u"b,a,@private,@groups,@channels,@bots"_q);

	// Catch-alls always sink below custom lists whatever the file says.
	const auto sunk = Parse(uR"(
list_order = ["@bots", "@private", "work"]

[lists.work]
title = "Work"
)"_q);
	CHECK(sunk.ok());
	CHECK(WarnsAbout(sunk, u"always sort below"_q));

	// @bots before @private because the file said so - reordering the
	// catch-alls among themselves is allowed, hoisting them above a user list
	// is not - and the two the file never mentioned follow in canonical order.
	CHECK_EQ(ListNames(sunk.settings).join(u","_q),
		u"work,@bots,@private,@groups,@channels"_q);
}

void TestPresets() {
	Begin("presets");
	const auto result = Parse(Example());
	CHECK(result.ok());
	CHECK_EQ(result.settings.presets.size(), size_t(2));

	// File order, not alphabetical: the popover lists presets as written.
	CHECK_EQ(result.settings.presets[0].name, u"work"_q);
	CHECK_EQ(result.settings.presets[1].name, u"strict"_q);

	const auto work = result.settings.preset(u"work"_q);
	CHECK(work != nullptr);
	CHECK_EQ(work->inherit, u"default"_q);
	CHECK(work->groupsRequireMention.value_or(false));
	CHECK(work->folders.has_value());
	CHECK_EQ(work->folders->size(), size_t(1));
	CHECK_EQ(work->folders->front().name, u"Music"_q);
	CHECK(work->folders->front().notify.has_value());
	CHECK(!work->folders->front().notify.value_or(true));

	CHECK_EQ(work->overrides.size(), size_t(1));
	CHECK_EQ(work->overrides[0].list, u"@private"_q);
	CHECK(!work->overrides[0].show.has_value());
	CHECK(work->overrides[0].notify.has_value());
	CHECK(!work->overrides[0].notify.value_or(true));

	// strict says nothing about folders, so the engine will walk up to work.
	const auto strict = result.settings.preset(u"strict"_q);
	CHECK(strict != nullptr);
	CHECK_EQ(strict->inherit, u"work"_q);
	CHECK(!strict->folders.has_value());
	CHECK(!strict->groupsRequireMention.has_value());
}

void TestPresetPolicies() {
	Begin("preset policies");

	const auto reserved = Parse(uR"(
[presets.Normal]
inherit = "default"

[presets.keep]
inherit = "default"
)"_q);
	CHECK(reserved.ok());
	CHECK(WarnsAbout(reserved, u"reserved"_q));
	CHECK_EQ(reserved.settings.presets.size(), size_t(1));
	CHECK_EQ(reserved.settings.presets[0].name, u"keep"_q);

	const auto unknown = Parse(uR"(
[presets.work]
inherit = "nowhere"
)"_q);
	CHECK(unknown.ok());
	CHECK(WarnsAbout(unknown, u"does not exist"_q));
	CHECK_EQ(unknown.settings.presets[0].inherit, u"default"_q);

	// A cycle has no root to resolve against, so it is fatal by design.
	const auto cycle = Parse(uR"(
[presets.a]
inherit = "b"

[presets.b]
inherit = "a"
)"_q);
	CHECK(!cycle.ok());
	CHECK(cycle.error.contains(u"loops"_q));

	const auto selfCycle = Parse(uR"(
[presets.a]
inherit = "a"
)"_q);
	CHECK(!selfCycle.ok());
}

void TestOverridePolicies() {
	Begin("override policies");

	const auto result = Parse(uR"(
[lists.os]
locked = true

[lists.free]
show = true

[presets.work.overrides.os]
show = false

[presets.work.overrides.ghosts]
show = false

[presets.work.overrides.free]
show = false
)"_q);
	CHECK(result.ok());
	CHECK(WarnsAbout(result, u"is locked"_q));
	CHECK(WarnsAbout(result, u"not a list"_q));

	// Only the override of the unlocked, existing list survives.
	const auto work = result.settings.preset(u"work"_q);
	CHECK(work != nullptr);
	CHECK_EQ(work->overrides.size(), size_t(1));
	CHECK_EQ(work->overrides[0].list, u"free"_q);
}

void TestMembers() {
	Begin("members");

	const auto result = Parse(uR"(
[lists.a]
members = [ 1, 2, 2, 3, 1 ]

[lists."@groups"]
members = [ 7 ]
locked = true
)"_q);
	CHECK(result.ok());
	CHECK_EQ(result.settings.list(u"a"_q)->members,
		(std::vector<Purple::PeerIdValue>{ 1, 2, 3 }));
	CHECK(WarnsAbout(result, u"cannot have members"_q));
	CHECK(WarnsAbout(result, u"cannot be locked"_q));
	CHECK(result.settings.list(u"@groups"_q)->members.empty());
	CHECK(!result.settings.list(u"@groups"_q)->locked);
}

void TestScheduleAndFocus() {
	Begin("schedule and focus");

	const auto result = Parse(uR"(
[presets.work]
inherit = "default"

[schedule]
enabled = true

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
enabled      = true
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

	// Focus sync pointing at nothing turns itself off rather than half-working.
	const auto broken = Parse(uR"(
[focus_sync]
enabled      = true
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

	// The file already on disk from the premium work has to keep working.
	const auto result = Parse(uR"([premium]
# Unlock the client-side features.
enabled = true
)"_q);
	CHECK(result.ok());
	CHECK(result.settings.premium.enabled);

	// A file that predates Work Mode is complete, not half-configured: it must
	// not log a warning on every single start.
	CHECK(result.warnings.empty());
	CHECK_EQ(result.settings.lists.size(), size_t(4));
	CHECK(result.settings.presets.empty());
	CHECK(!result.settings.focusSync.enabled);

	const auto off = Parse(u"[premium]\nenabled = false\n"_q);
	CHECK(off.ok());
	CHECK(!off.settings.premium.enabled);

	// No file content at all is still a usable configuration.
	const auto empty = Parse(QString());
	CHECK(empty.ok());
	CHECK(empty.settings.premium.enabled);
	CHECK_EQ(empty.settings.lists.size(), size_t(4));
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
	state.schedulePaused = true;
	state.peekActive = true;
	state.peekDeadlineUnix = 1755400000;
	state.resolvedCache.preset = u"work"_q;
	state.resolvedCache.groupsRequireMention = false;
	state.resolvedCache.lists = {
		{ u"os"_q, true, true },
		{ u"@private"_q, true, false },
	};
	state.resolvedCache.folders = { { u"Music"_q, false } };

	const auto text = Purple::SerializeState(state);
	const auto back = Purple::ParseState(text, u"state.toml"_q);
	CHECK_EQ(back.activePreset, u"work"_q);
	CHECK(back.activeSource == Purple::PresetSource::Schedule);
	CHECK_EQ(back.previousPreset, u"normal"_q);
	CHECK(back.previousSource == Purple::PresetSource::Manual);
	CHECK(back.schedulePaused);
	CHECK(back.peekActive);
	CHECK_EQ(back.peekDeadlineUnix, int64(1755400000));

	CHECK(back.resolvedCache.valid());
	CHECK_EQ(back.resolvedCache.preset, u"work"_q);
	CHECK(!back.resolvedCache.groupsRequireMention);
	CHECK_EQ(back.resolvedCache.lists.size(), size_t(2));
	CHECK_EQ(back.resolvedCache.lists[1].list, u"@private"_q);
	CHECK(back.resolvedCache.lists[1].show);
	CHECK(!back.resolvedCache.lists[1].notify);
	CHECK_EQ(back.resolvedCache.folders.size(), size_t(1));
	CHECK_EQ(back.resolvedCache.folders[0].name, u"Music"_q);
	CHECK(back.resolvedCache.folders[0].notify.has_value());
	CHECK(!back.resolvedCache.folders[0].notify.value_or(true));

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
	state.resolvedCache.lists = { { u"tab\there"_q, true, true } };

	const auto text = Purple::SerializeState(state);
	const auto back = Purple::ParseState(text, u"state.toml"_q);
	CHECK_EQ(back.activePreset, state.activePreset);
	CHECK(back.resolvedCache.valid());
	CHECK_EQ(back.resolvedCache.lists[0].list, u"tab\there"_q);
}

} // namespace

int main() {
	TestCatchAlls();
	TestListOrder();
	TestPresets();
	TestPresetPolicies();
	TestOverridePolicies();
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
	TestSetTableBool();
	TestStateRoundTrip();
	TestStateDefaults();
	TestStateQuoting();

	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
