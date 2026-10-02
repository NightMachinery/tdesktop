# Upstream hooks

Upstream tdesktop is merged into this fork again and again. Every line Purple
changes in an upstream file can conflict in a later merge, so AGENTS.md
("Keeping upstream merges easy") allows only hooks there. Much older Purple
code still sits inside upstream files, and a series of refactor batches (B1 to
B19) is moving it out.

This file is the hook registry. For every upstream file the fork changes it
records what Purple does there today, which batch moves it, what that batch
leaves behind, and how often upstream edited next to it. A batch that changes
an upstream file updates its entry here in the same commit, together with the
feature doc's "Upstream hooks" section. `purple/upstream_report.sh` produces
the numbers.

## Terms

- **Upstream file**: a path that exists at the merge-base of the fork and
  `upstream/dev`. Files the fork added (all of `Telegram/SourceFiles/purple/`,
  `purple/`, `docs/purple/`, the purple-core submodule) are not upstream
  files.
- **Purple hunk**: one hunk of `git diff -U0 <merge-base> HEAD` in an upstream
  file.
- **Hook**: an edit AGENTS.md allows in an upstream file: one include, one
  guarded call into `Purple::`, or one early return. A Purple menu row written
  as one call (`Purple::AddXRow(...)`) counts as a guarded call.
- **Near-hunk edit**: an upstream commit, in the year before the merge-base,
  that changed a base line within one line of a Purple hunk. For a hunk that
  replaces base lines a to b, the window is lines a-1 to b+1; for a pure
  insertion after line a, it is lines a and a+1. These are exactly the
  upstream edits that would have made git's three-way merge stop with a
  conflict, because git reports one whenever both sides change lines that
  overlap or touch. (Checked on scratch repositories: a change to line 5
  conflicts with a change to line 4 or 6 but not 3 or 7, and an insertion
  after line 5 conflicts with a change to line 5 or 6 but not 4 or 7.) They
  are counted per hunk with `git log -L`.
- **Owner**: the refactor batch that rewrites a Purple hunk. Batch letters and
  their plans are in the coordinator's desktop refactor plan; the short form of
  each batch is under "Batches" below.
- **Account-less Work Mode call**: a call, in an upstream file, of a gate
  function that answers for the running preset without being told which
  account asks: `ActiveResolved`, `ExtraViews`, `ExtraViewPins`, `PresetPins`,
  `PresetOwnsPins`, `Filtering`, `Peeking`, `HideEverywhere`,
  `RecentStaySeconds`, `RecentMarkStyle`, `ShownFolders`, `FoldersRestricted`,
  `ExemptFolders`, `SilencedFolders` or `QuietFolders`. Per-account Work Mode
  lists will resolve through the account, so none of these may stay in an
  upstream file.

## Measuring

```
purple/upstream_report.sh            # lines per upstream file, HEAD
purple/upstream_report.sh --near     # plus near-hunk edits per hunk, ~30 s here
purple/upstream_report.sh --worktree # the working tree instead of HEAD
purple/upstream_report.sh REV        # any other commit
```

The script uses git only. It takes the base from
`git merge-base REV upstream/dev` (`PURPLE_UPSTREAM_REF` picks another
upstream branch) and prints:

- added and removed lines per upstream file, and their totals;
- apart from the totals: AGENTS.md and README.md (B18 moves their Purple
  content), every changed submodule pointer (today only the lib_ui gitlink,
  with the number of times upstream moved it in the window), and the count of
  binary files (the recoloured icons);
- the account-less Work Mode calls in upstream files, per file. Comment lines
  are not counted;
- the callers of `TopPeers::list()`. B12 filters that list at its source, so a
  caller upstream adds later gets the filtered list too; after a merge, check
  any new caller this prints.

With `--near` it also prints, under each file, every Purple hunk as its base
line window, its added and removed lines and its near-hunk edits, and adds a
near-hunk total. Fetch `upstream` first if the base should be current; the
script never touches the network.

**Why some of the plan's figures are higher.** The refactor plan measured
near-hunk edits with an ad hoc script whose window reached one line further:
below a hunk that replaces lines, and above an insertion. That window does
not match git's conflict rule. It is why core/version.h scored 66 there: it
reached the `AppVersion` line two lines below `AppName`, which every release
changes, although a version bump merges cleanly. With the exact window
version.h scores 0 and info_profile_top_bar.cpp 40 instead of 46. A copy of
the script with the window widened the same way, run against the plan's
commit (91b54fe2b1), reproduces the plan's figures: version.h 66,
info_profile_top_bar.cpp 46, history.cpp 0, data_session.cpp 5 and AGENTS.md
8.

## Conventions

**Hook style (rule 4).** A hook that names `Purple::` gets no comment. An edit
that cannot name Purple (a changed default, a widened condition, a new
parameter) gets one `// Purple:` line. The explanations sitting at hooks today
move to the Purple definition or the feature doc when their batch runs.

**One include per upstream file (rule 5).** The single Purple include goes on
the line right after the file's own header include, before the blank line.
Upstream almost never edits those two lines: in the hot files only the commits
that created or moved a file touched them, while the alphabetical spots were
hit up to three times each in the year. So the Purple header must compile on
its own. In an upstream header, the include goes at the end of the include
block. Apply this whenever a batch rewrites an include.

**Build lists (B1).** New Purple sources, and every other Purple build
setting, go in `Telegram/cmake/purple.cmake`. Telegram/CMakeLists.txt keeps
only the one include that pulls that file in. A purple-core source goes in
the purple-core block there, and also in the toml++ property list if it
includes toml++.

**Facade headers (D5).** A Purple header that exists to be included by
upstream files goes in `Telegram/SourceFiles/purple/hooks/`, and may gather
the declarations of several Purple files. With rule 5, every Purple include in
an upstream file is then one line, right after the file's own header, naming
`purple/hooks/`, which a script can check. Batches create these headers as
they reshape their hooks; the other Purple headers stay flat in
`Telegram/SourceFiles/purple/`.

**Accounts.** Every Work Mode hook the refactor creates (list membership,
folder filtering, notification and unread filtering, chat-list hiding, mute,
temporary rows) carries the account it acts for. Each Work Mode entry below
has an "Account" line, with one of:

- *in hand*: the hook already passes something that names the account, and
  which (a peer, history, thread, entry, session, or `this` for a member of a
  per-session object);
- *must pass*: the planned shape does not show the account yet, and the
  owning batch has to add it;
- *needs account*: the call site has no account to give. It is recorded
  rather than given an invented one;
- *none needed*: a global signal, or a value that is app-wide by design.

Entries without an "Account" line are not Work Mode hooks (Last Seen,
premium, sync, settings rows, build, branding, device-wide screen time).

**Docs travel with the code (rule 6).** The commit that changes a hook updates
its entry here and the feature doc's "Upstream hooks" section. Only
version.md has such a section today ("Upstream files touched"); a batch adds
one to its feature doc the first time it reshapes that feature's hooks.

## Baseline, 2026-10-02

Measured at 1fdc4988e7 plus the commit that added this file, against the
merge-base 8e18cb7110 (2026-08-07). That commit changed only AGENTS.md among
upstream files, by pointing at this registry:

- 90 upstream files changed, +4,100 / -283 lines, in 421 Purple hunks;
- 189 near-hunk edits over those hunks in the year before the base;
- apart from those totals: AGENTS.md +140 / -3 (6 near-hunk edits), README.md
  +32 / -1 (1), the lib_ui gitlink (upstream moved it 214 times in the year,
  so each lib_ui bump conflicts with the fork pointer), and 37 binary files;
- 48 account-less Work Mode calls in 11 upstream files. The count must reach
  0 once B3, B4, B8, B10u, B10d, B11, B12 and B13 have run;
- 3 callers of `TopPeers::list()`: star_gift_box.cpp, dialogs_suggestions.cpp
  and history_view_top_peers_selector.cpp.

The plan quoted 89 files, +4,087 / -284 at 91b54fe2b1. That count included the
lib_ui gitlink's one-line numstat entry; without it the script gives 88 files,
+4,086 / -283 there. The two files added since are settings_main.cpp (the
version footer) and data_document_media.cpp (the sync record download guard).

The per-file figures below are from the same run. "near" is the file's
near-hunk total; re-run the script for current values.

## Batches

The order Evar chose (D9 A) runs every batch before any playlist code. In
short:

- B1 build lists into `Telegram/cmake/purple.cmake`, purple_core first in
  .gitmodules (done 2026-10-03);
- B2 Purple strings in a Purple-owned `purple.strings` merged at configure
  time (D1 B);
- B5 the settings-offer id out of SessionSettings, kept as a per-account pref
  (D4: keep the offer until the sync chat ships), plus one forced
  SessionSettings rewrite per session start;
- B6 Last Seen surfaces; B11 mute; B7 screen time and the composer, keeping
  the call sites (D13); B8 chat menus;
- B9 settings UI and instant replaces (D14: the dash toggle moves into the
  lib_ui fork); B10u the folder strip's UI half and the Normal-mode fix;
- B3 and B4 stage 1 of data_session.cpp and history.cpp (member definitions
  into Purple TUs), with B3c's pin fixes; B13 temporary-row marks;
- B12 suggestions, stories and top peers; B15 app-level and small hooks
  (D15: the Persian-layout legacy passcode retry stays); B17 api_user_privacy;
  B16 the sync receipt chain, replaced by a Purple-owned upload (D3 a);
  B10d the ChatFilters data half;
- B14 header stage 2 (with B10d stage 2), floating; B18 the Purple content of
  AGENTS.md and README.md; B19 upstreamable fixes, held while D7 says no
  upstream PRs; the clang-15 patches stay (D16).

## Build, resources and branding

**`.gitmodules`**: +4 -1, 2 hunks, near 0 after B1 (near 1 before). Done in
B1.
- The lib_ui URL points at the fork.
- The purple_core block is first in the file. It used to be appended at the
  end, where all four upstream .gitmodules commits of the year also appended.
  Git ignores entry order, so `git submodule status` is unchanged.

**`Telegram/CMakeLists.txt`**: +1 -0, 1 hunk, near 0 after B1 (+123 -4, 8
hunks, near 2 before). Done in B1.
- The one line is `include(cmake/purple.cmake)`, right after the
  `build_macstore` endif. Everything Purple used to add here is in
  `Telegram/cmake/purple.cmake`: `purple_core_loc`, the 62 `purple/` source
  lines, the purple-core source block, the include directory, the toml++
  source properties, the bundle id and output name, and the
  `purple_version.cmake` include.
- The line has to stay between that endif and the `bundle_identifier_plist`
  lines below it: the branding overrides upstream's `bundle_identifier` and
  `output_name` after the endif sets them and before the target properties
  and `Telegram.plist` read them. It has to stay an `include()`, not an
  `add_subdirectory()`, because the toml++ source properties are scoped to a
  directory and only `include()` keeps the caller's.
- What B1 changed in the build: every compile command is identical, `-I`
  order included (SourceFiles, then purple_core). The only difference is the
  link line, where the 49 Purple objects now come after all of upstream's
  instead of among them. No upstream initializer calls into Purple, so
  static initialization order does not matter here.

**`Telegram/Resources/langs/lang.strings`**: +42 -0, 3 hunks, near 1.
Owner: B2.
- Today: 32 Last Seen Peek strings near the top, 9 pinned-music strings, and
  `lng_settings_replace_dashes`.
- Leaves: nothing in lang.strings; one line in td_lang.cmake merges
  `purple.strings`.

**`Telegram/cmake/lib_fido2.cmake`**: +8 -1, 1 hunk, near 2. Stays.
- The `TDESKTOP_VENDORED_FIDO2` option and its condition: a system libfido2
  older than 1.14 lacks functions webauthn needs. It is a candidate upstream
  PR, held by D7.

**`Telegram/SourceFiles/core/version.h`**: +1 -1, 1 hunk, near 0. Stays.
- `AppName = "Purple Telegram"`. A version bump does not conflict with it:
  `AppFile` sits unchanged between it and `AppVersion`.

**`Telegram/SourceFiles/ffmpeg/ffmpeg_utility.h`**: +12 -0, 1 hunk, near 1.
Owner: B15.
- Today: a `static_assert` that the FFmpeg headers are major version 60, with
  its explanation.
- Leaves: nothing. The check moves to `purple/purple_build_checks.cpp`,
  which purple.cmake compiles into both `Telegram` and `lib_ffmpeg`.

**The lib_ui gitlink** (`Telegram/lib_ui`): stays.
- It points at the fork's lib_ui commit, which adds
  `Integration::systemTextReplacesEnabled`. Every upstream lib_ui bump means
  taking upstream's commit, rebasing the fork commit onto it and pointing at
  the result: 214 times in the year. D14 adds the instant-replace filter hook
  to the same fork commit.

**The 37 recoloured PNGs** (`Telegram/Resources/art/` and
`Telegram/Telegram/Images.xcassets/`): stay. `purple/recolour_icons.py`
regenerates them.

**`AGENTS.md`** (+140 -3, 6 hunks, near 6) and **`README.md`** (+32 -1, 4
hunks, near 1): owner B18. The Purple sections move to
`docs/purple/agents.md`, leaving a pointer and the hook rule (D6).

## Work Mode core

**`Telegram/SourceFiles/data/data_session.cpp`**: +607 -11, 23 hunks, near 5.
Owners: B3 (stage 1), B11 (two lines), B14 (stage 2).
- Today: the Work Mode walk (`setupPurpleWorkMode`, `refreshPurpleWorkMode`),
  the grace and tick timers, the view pinned lists, the view and quiet lists
  in `refreshChatListEntry` and `removeChatListEntry`, pin hooks in
  `setChatPinned`, `pinnedCanPin` and `reorderTwoPinnedChats`, and nine badge
  token swaps (`purpleBadgeUnread()`), plus a stray `QtCore/QDateTime`
  include.
- Leaves after B3: one include (rule 5), the constructor timers and setup
  call, the pin hooks reshaped to claim every view id
  (`purpleSetChatPinned`, `purpleCanPin`, the `reorderTwoPinnedChats` hook
  after the two `Expects`), one call each in `refreshChatListEntry` and
  `removeChatListEntry`, and the nine badge swaps, about 24 lines. The badge
  swaps cannot move: there is no single lower point, because
  `Dialogs::MainList::unreadState()` also feeds the folder badges. All 5
  near-hunk edits of the file are in those badge lines.
- B11 gives the two mute calls inside the Purple block (lines 1881 and
  5969) their final names, and B3 then moves them verbatim.
- Account: the pin hooks and the two list calls are *in hand* (`this`, a
  `Data::Session`). The 13 account-less calls (lines 1966, 2049, 2663, 2666,
  2707, 2708, 2752, 2753, 2992, 3164, 5849, 5853, 5930) move into
  `purple/purple_work_view.cpp` with B3.

**`Telegram/SourceFiles/data/data_session.h`**: +70 -0, 4 hunks, near 0.
Owners: B3, B14.
- Today: the public view-list accessors, the private Purple members and the
  timers.
- Leaves: about 75 declaration lines until B14 (B3 adds five private
  declarations); B14 moves the state into a per-session `Purple::WorkView`.

**`Telegram/SourceFiles/history/history.cpp`**: +515 -8, 10 hunks, near 0.
Owners: B4 (stage 1), B14.
- Today: the 365-line block of `History::purple*` definitions, the
  constructor's uncounted test, `setFakeUnreadWhileOpened` calls,
  `adjustedChatListTimeId`'s kept-for-view time, `chatListUnreadState`
  wrapped around upstream's body, `shouldBeInChatList`'s early return and
  extra term, and a stray `QtCore/QDateTime` include.
- Leaves after B4: `_purpleUncounted = Purple::StartsUncounted(peer)`;
  upstream's `chatListUnreadState` body with one bypass-guarded first line;
  the `purpleHiddenFromChatList` early return and
  `if (purpleKeptForView()) { return true; }`;
  `if (!result && purpleKeptForView()) { return TimeId(1); }`; the
  `purpleSetOpened` and `purpleRefreshShowMode` calls. About 18 lines.
- Account: the member hooks are *in hand* (`this`, a History) and so is
  `StartsUncounted(peer)`. The 13 account-less calls (lines 3431, 3514, 3535,
  3567, 3583, 3649, 3683, 3704, 3713, 3766, 3776, 3791, 3848) move into
  `purple/purple_history.cpp` with B4.

**`Telegram/SourceFiles/history/history.h`**: +125 -0, 4 hunks, near 0.
Owners: B4, B14.
- Today: the `purple*` member declarations and the opened and grace state.
- Leaves: the declarations until B14; then `_purpleUncounted` and its setter,
  with the opened and grace state in a WorkView map.

**`Telegram/SourceFiles/history/history_unread_things.cpp`**: +25 -1, 3
hunks, near 0. Owner: B4.
- Today: `setCount` split so a mention or reaction edge refreshes the show
  mode outside the `inChatList()` branch, gated by `Purple::Filtering()`.
- Leaves: upstream's line back, followed by `Purple::UnreadThingEdge(...)`.
  About 3 lines.
- Account: *must pass*. `UnreadThingEdge` takes `_thread` or its owning
  history, replacing the account-less `Purple::Filtering()` at line 126.

**`Telegram/SourceFiles/dialogs/dialogs_entry.cpp`**: +32 -1, 2 hunks, near
1. Owner: not named in the plan; proposed B4, beside the History hooks.
- Today: `computeSortPosition` puts fixed-on-top entries (the Archive row) on
  top of a preset view; `removeFromChatList` keeps the pin of a chat a preset
  hides, and leaves a view's own pinned list alone.
- Leaves: the two conditions, which the plan lists among the file-local
  pieces that cannot move. Their comments (19 lines) move to work_mode.md.
- Account: *in hand* (`this`, an Entry).

**`Telegram/SourceFiles/dialogs/dialogs_entry.h`**: +26 -0, 1 hunk, near 0.
Owner: B14.
- Today: three virtuals (`purpleHiddenFromChatList`, `purpleHiddenFromView`,
  `purpleShownFromArchive`) that History overrides, with long comments.
- Leaves: the three one-line virtuals; the comments move with B4 or B14.

## Folder strip and views

**`Telegram/SourceFiles/data/data_chat_filters.cpp`**: +190 -4, 10 hunks,
near 0. Owners: B11 (one line), B10d stage 1, B14 with B10d stage 2.
- Today: `ChatFilter::contains`' `ignorePresetMute` parameter, the
  shown-list builder (`purpleRefreshShown`, `purpleViewFilter`), the
  `chatsList` comment, `chatsListLoaded`, the `saveOrder` refusal and the
  `lookupId` rewrite.
- Leaves after B10d stage 1: `if (Purple::RefuseFolderOrderSave(...))` in
  `saveOrder`, the early id return in `lookupId` before upstream's restored
  `Expects`, the constructor call, the views' All-chats icon, the
  `ignorePresetMute` term and `chatsListLoaded`. The 8-line `chatsList`
  comment goes to work_mode.md.
- Why the two extension points stay: deciding which chats a preset silences
  by folder must ask what "Exclude muted" says without the preset's own mute,
  or the answer feeds itself (silencing a chat takes it out of an
  exclude-muted folder, which un-silences it), and `contains` is upstream's
  member, so a parameter is the smallest way in. `chatsListLoaded` looks a
  list up without creating it; `chatsList()` would build the very lists a
  sweep of the view ids checks are empty.
- B11 switches the mute call at line 378 to its final name.
- Account: `RefuseFolderOrderSave` *must pass* `&_owner->session()`,
  replacing `FoldersRestricted()` at line 1034. The constructor call and the
  `lookupId` id test are *in hand* or need none. The other 8 account-less
  calls (427, 445, 462, 468, 476, 537, 1080, 1093) move into
  `purple/purple_folder_strip.cpp`; stage 2 makes `Purple::FolderStrip`
  belong to one ChatFilters, so to one session.

**`Telegram/SourceFiles/data/data_chat_filters.h`**: +88 -1, 7 hunks, near 0.
Owner: B10d stage 2 (with B14).
- Today: the view id constants and `IsPurpleView`, the `ignorePresetMute`
  declaration, `purpleShownList`, `purpleViewCount`, `chatsListLoaded` and the
  strip state.
- Leaves: `ignorePresetMute`, `chatsListLoaded` and one
  `std::unique_ptr<Purple::FolderStrip>`. View ids move to
  `purple/purple_view_ids.h`; the forwarders become `Purple::ShownList` and
  `Purple::ViewCount`.

**`Telegram/SourceFiles/data/data_unread_value.cpp`**: +48 -4, 5 hunks, near
0. Owner: B10d stage 1.
- Today: the quiet-folder test and the view unread state inside
  `UnreadStateValue`.
- Leaves: `Purple::ViewUnreadStateValue(session, filterId)` and
  `Purple::QuietFolderUnread(session, filterId)`.
- Account: *in hand*: the session is already a parameter. The account-less
  `QuietFolders()` call at line 37 moves into the Purple side.

**`Telegram/SourceFiles/ui/widgets/chat_filters_tabs_strip.cpp`**: +62 -19,
16 hunks, near 7. Owner: B10u.
- Today: `purpleShownList()` swaps, the view tab menu, a whole-strip pin
  under a restricted preset in place of upstream's pinned intervals, and the
  icon condition.
- Leaves: `ShowViewTabMenu`, upstream's pinned intervals plus one whole-strip
  line, and the `purpleShownList()` swaps (B10d decides whether they become
  free functions). The icon condition goes back to upstream.
- Account: `PinWholeStripIfRestricted(session)` *must pass* the session,
  replacing `FoldersRestricted()` at lines 288 and 456.

**`Telegram/SourceFiles/window/window_filters_menu.cpp`**: +54 -13, 13
hunks, near 5. Owner: B10u.
- Today: the strip's change merge, `purpleShownList()` swaps, the whole-strip
  pin, the icon ternary, the view menu branch and the reorder refusal.
- Leaves: `FilterStripChanges`, upstream's pinned-interval block followed by
  `PinWholeStripIfRestricted`, and the `IsPurpleView` / `FillViewMenu`
  branch. The icon ternary goes.
- Account: `PinWholeStripIfRestricted(session)` *must pass* the session,
  replacing `FoldersRestricted()` at lines 364 and 843. The `ActiveChanges()`
  producer inside `FilterStripChanges` needs none.

**`Telegram/SourceFiles/dialogs/dialogs_inner_widget.cpp`**: +47 -7, 12
hunks, near 3. Owners: B10u, B3c, B13.
- Today: `refreshWithCollapsedRows`' view term, the temporary-row paint
  block, the `cacheAllowed` term, `savePinnedOrder`'s view branch,
  `refreshShownList`, `refreshEmpty`, `switchToFilter` and `setupShortcuts`.
- B10u leaves: one condition in `refreshShownList` and `(_filterId > 0)` back
  in `refreshEmpty`; one guard in `switchToFilter`; one
  `|| Data::IsPurpleView(_filterId)` term in `refreshWithCollapsedRows`; one
  token swap in `setupShortcuts`.
- B3c leaves: upstream's `else if (_filterId)` in `savePinnedOrder`, preceded
  by `} else if (Purple::SavePinnedViewOrder(&session(), _filterId,
  _openedFolder)) {` with an empty body. This is the only `savePinnedOrder`
  edit.
- B13 leaves: `context.purpleMark = Purple::RowMarkFor(row->entry());` in
  place of the 13-line paint block, and the `cacheAllowed` term testing that
  field.
- Account: `SavePinnedViewOrder(&session(), ...)` and
  `RowMarkFor(row->entry())` are *in hand*; the view-id tests need none.

**`Telegram/SourceFiles/dialogs/dialogs_widget.cpp`**: +5 -1, 2 hunks, near
0. Owners: B10u, B12.
- Today: `escape()` swapped to the shown list, with a comment.
- Leaves: one line in `escape()` (B10u). B12 adds the top-peers restart at
  line 2151 (see "Suggestions, stories and top peers").

**`Telegram/SourceFiles/window/window_session_controller.cpp`**: +44 -14, 8
hunks, near 3. Owners: B10u, B7, B5.
- Today: three includes, the settings offer at session start, the
  `ActiveChanges()` producer in the filters-menu merge, `WatchScreenTime`,
  `checkOpenedFilter` rewritten, home-filter literals in `openFolder` and
  `openCommunity`, and the `CheckAndJumpToNearChatsFilter` swap.
- B10u leaves: `if (Purple::CheckOpenedView(this)) { return; }` before
  upstream's restored `checkOpenedFilter` body; `Purple::HomeFilterId(
  &session())` at the four literals; `Purple::ActiveChanges()` as one
  producer in the merge, comment gone; the `CheckAndJumpToNearChatsFilter`
  swap.
- B7 leaves: `Purple::WatchScreenTime(this)` without its 3-line comment.
- B5 and D4: `Purple::OfferNewerSettingsFromSavedMessages(session, uiShow())`
  stays (a 3-line hook) until the sync chat ships and the offer retires.
- Includes: one facade include (rule 5).
- Account: `CheckOpenedView(this)` and `HomeFilterId(&session())` are *in
  hand*; `ActiveChanges()` needs none.

## Mute

**`Telegram/SourceFiles/data/notify/data_notify_settings.cpp`**: +127 -0, 5
hunks, near 0. Owner: B11.
- Today: `purpleRefreshMute`, `purpleSilenced`, `purpleMutedWithoutPreset`
  and the preset branch inside `isMuted`.
- Leaves: upstream's `isMuted(peer, changesIn)` body with the first line
  `if (Purple::PresetMutes(peer, changesIn, kMaxNotifyCheckDelay)) { return
  true; }`, and `purpleRefreshMute`. The members become free functions in
  `purple/purple_mute.cpp`; `MutedWithoutPreset` runs the public `isMuted`
  under the bypass guard; the folder set becomes per session.
- Account: `PresetMutes(peer, ...)` is *in hand*. The 3 account-less calls
  (377, 642, 661) move into the Purple side. This batch is what makes
  background accounts' notifications follow their own account.

**`Telegram/SourceFiles/data/notify/data_notify_settings.h`**: +47 -0, 3
hunks, near 0. Owner: B11.
- Leaves: `purpleRefreshMute`'s declaration. The rest goes with the free
  functions.

**`Telegram/SourceFiles/menu/menu_mute.cpp`**: +26 -1, 4 hunks, near 2.
Owners: B11 (callers), B8.
- Today: the preset name in the thread descriptor, `ToggleMuteForever`
  reading the mute without the preset, and the "Silenced by" row.
- Leaves: `SilencingPresetName` and `AddSilencedByRow`, and the B11 caller
  names at lines 258 and 311.
- Account: *none needed*: they label a preset mute, which a stock account
  never has.

**`Telegram/SourceFiles/menu/menu_mute.h`**: +6 -0, 1 hunk, near 1. Stays.
- `Descriptor::purplePreset`, an extension point.

**`Telegram/SourceFiles/info/profile/info_profile_values.cpp`**: +10 -2, 2
hunks, near 0. Owner: B11.
- Today and after: the profile's notifications switch reads the mute without
  the preset, at two sites; B11 gives them their final names.
- Account: *in hand* (topic, peer).

**`Telegram/SourceFiles/settings/sections/settings_notifications_type.cpp`**:
+7 -1, 1 hunk, near 0. Owner: B11.
- The exceptions list status reads the mute without the preset; one call
  after B11.
- Account: *in hand* (peer).

**`Telegram/SourceFiles/history/history_widget.cpp`** (mute part): the mute
button text and `toggleMuteUnmute` read the mute without the preset (lines
3662 and 5884). B11 gives them their final names; B7 then edits the rest of
the file. See "Screen time and the composer".

## Temporary-row marks

**`Telegram/SourceFiles/dialogs/ui/dialogs_layout.cpp`**: +83 -0, 4 hunks,
near 1. Owner: B13.
- Today: the stripe and ring painters and two style reads.
- Leaves: two calls; the painters move to `purple_row_mark.cpp`.
- Account: *must pass*. The two `RecentMarkStyle()` reads at lines 318 and
  568 become reads of the style carried in `context.purpleMark`, which
  `RowMarkFor` leaves empty for a stock account.

**`Telegram/SourceFiles/dialogs/ui/dialogs_layout.h`**: +11 -0, 1 hunk, near
1. Owner: B13.
- Leaves: one include and one `Purple::RowMark purpleMark` member.

## Suggestions, stories and top peers

**`Telegram/SourceFiles/dialogs/ui/dialogs_suggestions.cpp`**: +205 -78, 21
hunks, near 3. Owner: B12.
- Today: the chat menu's lists submenu, `ShownInSuggestions`, rebuildable
  recents, my-channels refresh, the recommendations predicate and refill,
  `TopPeersContent` rewritten with a preset subscription.
- Leaves: `AddListsSubmenu` in `FillEntryMenu`; `ShownInSuggestions` in
  `RecentPeersContent`; `RecentsController::prepare` back to upstream with
  the `RebuildRows` block appended; `prepare()` and `fill()` back to upstream,
  with one guard in `fill()` and a block appended to `prepare()` that hides
  the section by count; `TopPeersContent` back to upstream byte for byte. My
  channels follow D8, which is still open: a live refresh copies about 15
  upstream lines, a refresh on reopen copies none. About 45 lines with the
  other B12 files.
- Account: the recommendations predicate *must pass* `&session()`. Today it
  is the file-local `ShowRecommendedChannels()` (line 462, called from three
  places); the design proposes `ShowRecommendedChannels(session)`.
  `ShownInSuggestions` (peers) and `AddListsSubmenu(menu, peer)` are *in
  hand*.

**`Telegram/SourceFiles/dialogs/ui/dialogs_stories_content.cpp`**: +23 -4, 5
hunks, near 2. Owner: B12.
- Today: the `StoryShown` guard, the total recomputed after the loop, and
  `ActiveChanges()` merged into the refresh chain.
- Leaves: upstream's `.total` initializer back; the `continue` guard and the
  post-loop `result.total = int(result.elements.size());` as hooks, comments
  gone; the chain through `Purple::WithPresetChanges(...)`.
- Why the guard sits here: `State::next()` is the one place that only feeds
  the strip. `Data::Stories` also feeds the counters, the archive strip and
  upstream's hidden-stories handling, which a preset must not change. The
  total is recounted after the loop, or the strip's count would include the
  sources it hides.
- Account: `StoryShown(peer, ...)` is *in hand*; `WithPresetChanges` needs
  none.

**`Telegram/SourceFiles/history/view/history_view_top_peers_selector.cpp`**:
+8 -0, 2 hunks, near 3. Owner: B12.
- Leaves: nothing. `TopPeers::list()` filters at its source.

**`Telegram/SourceFiles/boxes/star_gift_box.cpp`**: +8 -0, 2 hunks, near 2.
Owner: B12.
- Leaves: nothing, for the same reason. This hook was missed by the first
  plan.

**`Telegram/SourceFiles/data/components/top_peers.cpp`**: not changed today.
B12 wraps `TopPeers::list()`'s return expression in
`Purple::WithoutHiddenSuggestions(...)`, a two-line wrap, and
dialogs_widget.cpp:2151 wraps the producer in
`Purple::RestartOnPresetChange(...)`. Account: *in hand* (the peers name their
session).

## Chat menus

**`Telegram/SourceFiles/window/window_peer_menu.cpp`**: +114 -3, 17 hunks,
near 4. Owners: B11 (callers), B8.
- Today: five includes, the mute submenu's preset row, three Filler members
  (lists, Last Seen Peek, pinned music) and their calls, the `addTranslate`
  local-premium term, and the view branch in `TogglePinnedThread`.
- Leaves: one facade include; `MuteMenuState` with B11's names (lines 265 and
  275); the three Filler members as free functions, one
  call each; the `addTranslate` term (moved here from B15 so the file is
  edited once); `if (Purple::TogglePinnedInView(controller, entry, filterId,
  onToggled)) { return; }` with upstream's `if (!filterId)` back. The
  mirroring main view re-enters the public 4-argument `TogglePinnedThread`
  with `FilterId(0)`. About 26 lines.
- Account: `TogglePinnedInView` is *in hand* through the controller; its
  Purple body must read `PresetOwnsPins` for `&controller->session()`,
  replacing the account-less call at line 4464. `MuteMenuState` (peer) is *in
  hand*.

**`Telegram/SourceFiles/window/window_main_menu.cpp`**: +12 -0, 3 hunks,
near 1. Owner: B8.
- Today: the preset menu entry with a comment, and an extra
  `ui/layers/generic_box.h` include.
- Leaves: `Purple::ShowPresetBox(controller)` as one guarded row, one include.
- Account: *in hand* (the controller).

## Last Seen

**`Telegram/SourceFiles/history/view/history_view_top_bar_widget.cpp`**: +76
-1, 8 hunks, near 0. Owner: B6.
- Leaves: `OnLastSeenInputsChanged`, one `Purple::LastSeenTail` member,
  `TopBarOnlineText` replacing only the `Data::OnlineText` line, clicks
  through `HandleLastSeenTailClick` and `LastSeenTailRect`.

**`Telegram/SourceFiles/history/view/history_view_top_bar_widget.h`**: +3 -0,
2 hunks, near 0. Owner: B6.
- Leaves: the member.

**`Telegram/SourceFiles/info/profile/info_profile_status_label.cpp`**: +58
-6, 7 hunks, near 11. Owner: B6.
- Today: the constructor's subscriptions, the status body with the
  last-seen note and its link, and the link callback setter.
- Leaves: `Purple::AttachProfileStatus(_label, _peer, [=] { refresh(); },
  _lifetime)` in the constructor (it schedules the first refresh with
  `crl::on_main`, after TopBar has set up the label); `Purple::
  ProfileStatusBody(...)` in `refresh()`, keeping link index 3; and
  `Purple::ApplyLastSeenLink(_label, user, hasLink)` after upstream's link
  block. The label installs its own link, so the setter goes.
- Fallback: if the deferred first refresh misbehaves, B6 keeps the owner
  callback (about 15 lines more).

**`Telegram/SourceFiles/info/profile/info_profile_status_label.h`**: +8 -0,
2 hunks, near 4. Owner: B6.
- Leaves: nothing (setter, getter and member go).

**`Telegram/SourceFiles/info/profile/info_profile_top_bar.cpp`**: +53 -23, 13
hunks, near 40, the fork's hottest file. Owners: B6, and D16 for one line.
- Today: two includes replacing `show_or_premium_box.h`, the peek-now
  string, the status link callback at construction and in `adjustColors`
  (three lines), and `setupShowLastSeen` rewritten.
- Leaves: the `show_or_premium_box.h` include restored and one Purple include
  after the file's own header; the premium early return keeping its
  `|| Purple::LocalPremium()` term, followed by `if
  (Purple::SetupShowLastSeen(controller, user, _showLastSeen.data())) {
  return; }`. The construction call and the three `adjustColors` lines go.
- Drift risk: the Purple side repeats 11 upstream lines (the 9-line LastSeen
  privacy refetch, `setOpacity(0.)` and `setFullRadius(true)`). Check them
  against upstream after every merge.
- The clang-15 aggregate-init fix at line 349 (6 near-hunk edits) stays
  under D16.

**`Telegram/SourceFiles/boxes/peer_list_box.cpp`**: +23 -2, 5 hunks, near 1.
Owner: B6.
- Leaves: `Purple::RowOnlineText` and `Purple::RowOnlineChangeTimeout`, one
  include.

**`Telegram/SourceFiles/boxes/peers/prepare_short_info_box.cpp`** (+3 -1, 2
hunks, near 0), **`Telegram/SourceFiles/boxes/peers/edit_participant_box.cpp`**
(+5 -2, 3 hunks, near 1) and
**`Telegram/SourceFiles/history/view/history_view_chat_preview.cpp`** (+2 -1,
2 hunks, near 0): owner B6.
- Each swaps `Data::OnlineText` for `Purple::LastSeenNoteFor(...).text`; one
  call each after B6. B6b makes the short info box's refresh timer follow
  `RowOnlineChangeTimeout`.
- Rejected: one early return in `Data::OnlineText` instead of these swaps.
  The sites pass different arguments, it would change all 9 upstream callers,
  and it would need a bypass guard.

**`Telegram/SourceFiles/api/api_user_privacy.cpp`**: +131 -10, 10 hunks, near
0. Owner: B17.
- Today: `Representable`, the extended `save` (completion callback and
  `afterRequest`), and `reloadFresh`.
- Leaves: the extended `save` and `reloadFresh` (about 21 lines) calling
  `Purple::Representable`. `reloadFresh` is the only route to the private
  `pushPrivacy` that skips `apply()`'s side effects: for LastSeen, `apply()`
  runs `updatePrivacyLastSeens()`, which cancels and resends
  `contacts.getStatuses`. About 70 lines stay.

**`Telegram/SourceFiles/api/api_user_privacy.h`**: +5 -2, 3 hunks, near 0.
Stays: the extended `save` signature and `reloadFresh`.

**`Telegram/SourceFiles/main/main_account.cpp`**: +2 -0, 2 hunks, near 0.
Owner: B15 (optional).
- `Purple::RecoverLastSeenPeek(_session.get())` in `createSession`, already
  one call. `sessionChanges()` could host it.

## Screen time and the composer

D13 keeps the call sites: screen time's composer actions are activity signals
for time, so they are recorded when the user acts, not when a message is sent.

**`Telegram/SourceFiles/history/history_widget.cpp`**: +62 -3, 12 hunks,
near 2. Owners: B11 (two mute lines), then B7.
- Today: two includes, the screen time cover (construction, peer, geometry),
  `NoteScreenTimeAction` for voice, typing, send, attach, reply and edit, and
  `send()` split around the send note.
- Leaves: one facade include, the cover's three one-line hooks, one call per
  action, and upstream's `else if (_editMsgId)` back in `send()`.

**`Telegram/SourceFiles/history/history_widget.h`**: +11 -0, 2 hunks, near
1. Stays: the cover member and its forward declaration.

**`Telegram/SourceFiles/history/view/controls/history_view_compose_controls.cpp`**:
+43 -0, 5 hunks, near 3. Owner: B7, with B7b.
- Today: three subscriptions in `init()` (send, attach, voice) and calls for
  typing, edit and reply.
- Leaves: `Purple::RecordComposeActions([=] { return _history ?
  _history->peer.get() : nullptr; }, sendContentRequests(SendRequestType::Text)
  | rpl::to_empty, _attachRequests.events(),
  _voiceRecordBar->recordingStateChanges(), _wrap->lifetime())` from `init()`,
  and one call each for typing, edit and reply. The getter reads `_history`
  when the event fires, because the controls outlive the chat.
- B7b: send comes from the unfiltered internal stream, which removes the
  doubled `editStarsFrom` stars box. A send refused for too few stars still
  counts once.

`Telegram/SourceFiles/core/application.cpp` starts the recorder; see
"App-level hooks".

## Settings UI and instant replaces

**`Telegram/SourceFiles/settings/sections/settings_advanced.cpp`**: +353 -0,
3 hunks, near 11. Owner: B9.
- Today: ten includes, the three `BuildPurple*` sections (342 lines, now
  including the version row), and `BuildPurpleSection(builder)` in `kMeta`.
- Leaves: `Purple::BuildAdvancedSection(builder)` and one include. That
  `kMeta` line had 7 near-hunk edits: upstream added the screen-reader section
  right after it in March 2026. Moving the Purple block elsewhere on the page
  changes the page, so that is a question for Evar, not part of B9.
- Account: the Work Mode choice row later reads `builder.session()`: *in
  hand*.

**`Telegram/SourceFiles/settings/sections/settings_chat.cpp`**: +28 -0, 2
hunks, near 2. Owner: B9.
- Today: the dash toggle's search entry and its checkbox.
- Leaves: two calls.

**`Telegram/SourceFiles/settings/sections/settings_main.cpp`**: +3 -0, 3
hunks, near 4. Final.
- One include, placed per rule 5 (near 1; it scored 2 in the alphabetical
  block), and `Purple::AddVersionFooter(builder)` after
  `BuildHelpSection(builder)` twice: in `Main::setupContent`, which draws the
  page (near 2), and in `kMeta`, which builds the search index (near 1). The
  page call was missing until 2026-10-03, so the row was searchable but never
  drawn (bug V1). See version.md.

**`Telegram/SourceFiles/core/core_settings.cpp`**: +10 -2, 4 hunks, near 4.
Owner: B9 (dashes); the defaults stay.
- Today: the `purple-replace-dashes` pref (key, read, setter) and the changed
  `resetOnLastLogout` defaults.
- Leaves: the changed defaults (`_replaceEmoji` and `_systemTextReplace`
  false). The dash setting moves to `purple_instant_replaces.cpp` with the
  same pref key; `resetOnLastLogout` still clears it, because `_prefs.clear()`
  removes the key.

**`Telegram/SourceFiles/core/core_settings.h`**: +10 -2, 2 hunks, near 2.
Owner: B9.
- Leaves: the two changed defaults; the dash accessors and member go.

**`Telegram/SourceFiles/core/ui_integration.cpp`** (+4 -0, 1 hunk, near 0)
and **`Telegram/SourceFiles/core/ui_integration.h`** (+1 -0, 1 hunk, near 0):
stay.
- `UiIntegration::systemTextReplacesEnabled`, the override of the lib_ui fork
  hook. D14 adds the instant-replace filter override here.

**The instant-replace sites**, owner B9 under D14: 12 call sites in 8 files
swap `setInstantReplaces(Ui::InstantReplaces::Default())` for
`Purple::InstallInstantReplaces(...)`, each with one include. Under D14 the
toggle is applied in the lib_ui fork, and all 12 swaps and 8 includes go back
to upstream:
- `Telegram/SourceFiles/chat_helpers/message_field.cpp`: +4 -3, 4 hunks,
  near 0 (lines 173, 543, 644);
- `Telegram/SourceFiles/boxes/peers/edit_peer_info_box.cpp`: +3 -2, 3 hunks,
  near 0 (730, 839);
- `Telegram/SourceFiles/boxes/add_contact_box.cpp`: +3 -2, 3 hunks, near 1
  (576, 593);
- `Telegram/SourceFiles/boxes/peers/add_to_community_box.cpp`: +2 -1, 2
  hunks, near 7 (174; 6 of the 7 are at that line);
- `Telegram/SourceFiles/settings/sections/settings_information.cpp`: +2 -1,
  2 hunks, near 1 (795);
- `Telegram/SourceFiles/support/support_helper.cpp`: +2 -1, 2 hunks, near 0
  (96);
- `Telegram/SourceFiles/boxes/edit_todo_list_box.cpp`: +2 -1, 2 hunks, near 0
  (186);
- `Telegram/SourceFiles/boxes/peers/edit_tag_control.cpp`: +9 -1, 3 hunks,
  near 3 (379). This site also turns the replaces on with the app settings,
  and adds two includes for it; B9 folds that into
  `Purple::TextOnlyFollowToggles`.

## Premium and translation

**`Telegram/SourceFiles/data/components/sponsored_messages.cpp`**: +21 -0, 5
hunks, near 1. Owner: B15.
- Leaves: the local-premium subscription and the three early returns in
  `canHaveFor` (twice) and `isTopBarFor`, as uncommented one-liners.

**`Telegram/SourceFiles/main/main_domain.cpp`**: +6 -0, 2 hunks, near 0.
Owner: B15.
- Leaves: the `LocalPremium` early return in `maxAccounts`, comment gone.

**`Telegram/SourceFiles/api/api_peer_search.cpp`**: +7 -2, 3 hunks, near 0.
Stays, with B15 trimming its comment.
- The sponsored request condition gains `|| Purple::LocalPremium()`, and the
  `_type` test moves into it so that every path that skips
  `requestSponsored()` marks the sponsored half ready. That part is an
  upstream bug fix, a D7 candidate.

**`Telegram/SourceFiles/boxes/language_box.cpp`** (+7 -2, 3 hunks, near 2)
and **`Telegram/SourceFiles/history/view/history_view_translate_tracker.cpp`**
(+50 -1, 6 hunks, near 1): owner B15.
- The translate switch and the translate bar honour local premium; the
  tracker's offer logic moves behind one call.

**`Telegram/SourceFiles/apiwrap.cpp`** (premium part): the include and the
`updatePrivacyLastSeens` term `&& !Purple::LocalPremium()` (line 2130), whose
comment B15 removes. The rest of apiwrap.cpp belongs to B16.

## App-level hooks, passcode and notifications

**`Telegram/SourceFiles/core/application.cpp`**: +37 -0, 5 hunks, near 0.
Owner: B15.
- Today: five includes; `StartSchedule`, `StartFocusSync` and
  `StartScreenTime` in `run()`; lock reports and peek notices in
  `setScreenIsLocked`, `lockByPasscode` and `unlockPasscode`.
- Leaves: one include, `Purple::StartAppServices()` and
  `Purple::OnScreenLockChanged(locked)`, plus the passcode lock and unlock
  calls. Optional: `passcodeLockChanges()` (application.h:307) could replace
  those two.
- Account: *none needed*: schedules, focus sync, screen time and peeks are
  device-wide.

**`Telegram/SourceFiles/window/main_window.cpp`**: +6 -0, 2 hunks, near 1.
Owner: not named in the plan; proposed B15.
- `Purple::ListenHotkeys(this)` for the peek key, already one call. Its
  3-line comment goes.

**`Telegram/SourceFiles/storage/storage_domain.cpp`**: +39 -10, 8 hunks,
near 2. Owner: B15, with D15.
- Today: includes reordered, the Persian-layout passcode mapping, and the
  retries in `start`, `startModern` and `checkPasscode`.
- Leaves: upstream's include order with one include; `DecryptWithMappedPasscode`
  as one clause in `startModern`; `checkPasscode` as one line; and, because
  D15 keeps the legacy retry until Evar says otherwise, the 3-line legacy
  hook (the `legacyStart` line and the `startWithSingleAccount` argument).

**`Telegram/SourceFiles/window/notifications_manager.cpp`** (+13 -2, 7
hunks, near 3), **`Telegram/SourceFiles/window/notifications_manager.h`** (+3
-0, 3 hunks, near 0) and
**`Telegram/SourceFiles/window/notifications_manager_default.cpp`** (+1 -0, 1
hunk, near 0): owner B15.
- Today: `getNotificationOptions` takes the peer, sets `previewAlways` from
  `Purple::PreviewAlways(Purple::ActiveSettings(), ...)` and unhides name and
  text; the native manager skips its own hiding for it.
- Leaves: `WithPreviewAlways` at both call sites; the header goes back to
  upstream. Amendment: `WithPreviewAlways` takes the peer (or account and
  peer), not a settings object, so per-account settings resolve
  `preview_always` through it with no new upstream hook.

**`Telegram/SourceFiles/core/file_utilities.cpp`**: +4 -1, 1 hunk, near 0.
Stays, with B15 shrinking the comment to one line after the Downloads guard.
- The downloads folder keeps upstream's "Telegram Desktop" name, not
  `AppName`.

## Sync

**`Telegram/SourceFiles/main/main_session_settings.cpp`** (+7 -0, 5 hunks,
near 6) and **`Telegram/SourceFiles/main/main_session_settings.h`** (+23 -1, 3
hunks, near 3): owner B5.
- Today: `_purpleSettingsOfferMessageId` serialized at the end of the
  SessionSettings stream, plus the changed `_archiveInMainMenu` default.
- Hazard: if upstream's next field is a count followed by items, the Purple
  integer is read as that count, the stream fails, and that account loses all
  its session settings.
- Leaves: only the `_archiveInMainMenu` default. The offer id moves to a
  per-account pref, and Purple forces one SessionSettings rewrite per session
  start, logging `Purple: rewrote SessionSettings, N bytes`. Every profile
  must run a B5-or-later build once before a build with a merged upstream
  field.

**`Telegram/SourceFiles/storage/storage_account.cpp`**: +16 -0, 3 hunks,
near 1. Stays.
- `reset()` clears the prefs and cancels their write timer: an upstream bug
  fix (prefs leaked across a logout), a D7 candidate. The `QByteArray`
  pref specializations are an extension point: the sync account binding
  stores its token through them.

**`Telegram/SourceFiles/data/data_document_media.cpp`**: +3 -0, 2 hunks,
near 0. Final.
- One include and an `else if (Purple::IsSyncRecordFileName(...)) { return; }`
  in `automaticLoad`, so sync records are never auto-downloaded. The include
  sits in the alphabetical block; move it per rule 5 when the file is next
  touched.

**`Telegram/SourceFiles/history/view/history_view_context_menu.cpp`**: +6 -0,
2 hunks, near 1. Final.
- One include and `Purple::AddImportSettingsAction(menu, item, document,
  controller->uiShow())` in `AddDocumentActions`.

**`Telegram/SourceFiles/storage/file_upload.cpp`**: +9 -1, 6 hunks, near 2.
Owner: B16.
- Today: the receipt passed through `finishFront` and `notifyFailed`, and
  `!Purple::IsPurplePost(*file)` keeping a sync post out of the downloads
  folder copy.
- Leaves under D3 a (Purple-owned upload with `messages.sendMedia`): no
  receipt hunks. A record the Purple upload sends never enters
  `Uploader::upload`, so the `IsPurplePost` condition should have nothing
  left to guard; B16 confirms that before removing it.

**`Telegram/SourceFiles/storage/file_upload.h`** (+2 -0, 2 hunks, near 0),
**`Telegram/SourceFiles/storage/localimageloader.cpp`** (+19 -1, 6 hunks,
near 1), **`Telegram/SourceFiles/storage/localimageloader.h`** (+17 -0, 2
hunks, near 0), **`Telegram/SourceFiles/apiwrap.cpp`** (receipt part: 13 of
its 15 hunks) and **`Telegram/SourceFiles/apiwrap.h`** (+9 -4, 5 hunks, near
1): owner B16.
- Today: `SendFileReceipt`, `FileLoadTo::receipt`, and the receipt threaded
  through `sendFiles`, `sendUploadedDocument`, `sendMedia` and
  `sendMediaWithRandomId`, which completes with the server message id.
- Leaves under D3 a: nothing. Records are capped at 10 MB compressed, so the
  Purple upload needs no big-file part method. Manual "Send to Saved
  Messages" keeps upstream's path. apiwrap.cpp as a whole: +56 -8, 15 hunks,
  near 5; B15 and B16 both edit it, so they run one after the other.

## Compiler fixes (D16)

The clang-15 aggregate-init fixes stay as local patches until an OS update
brings Xcode 16. They are a real merge cost: 18 near-hunk edits in the year,
and new call sites on most pulls. Fix them first on every merge.

- `Telegram/SourceFiles/api/api_transcribes.cpp`: +3 -2, 2 hunks, near 7;
- `Telegram/SourceFiles/boxes/url_auth_box.cpp`: +1 -1, 1 hunk, near 4;
- `Telegram/SourceFiles/info/profile/info_profile_top_bar.cpp` line 349
  (counted with that file above).

## Hunks the first plan missed

All of them exist at HEAD, and each now has an owner:

- B4: `adjustedChatListTimeId`, `setFakeUnreadWhileOpened` and the
  `QtCore/QDateTime` include in history.cpp;
- B10u: `refreshWithCollapsedRows`, `setupShortcuts`, dialogs_widget's
  `escape()`, the `ActiveChanges()` merge in window_session_controller.cpp
  and `CheckAndJumpToNearChatsFilter`;
- B13: the `cacheAllowed` term in dialogs_inner_widget.cpp;
- B12: star_gift_box.cpp, `RecentPeersContent`'s `ShownInSuggestions` and
  `FillEntryMenu`'s `AddListsSubmenu`;
- B15: main_domain.cpp, apiwrap.cpp's `LocalPremium` term and the three
  sponsored early returns;
- B8: `addTranslate`'s local-premium term;
- B7: `WatchScreenTime`'s comment;
- B5 and D4: the settings-offer hook in window_session_controller.cpp;
- B9: the twelve instant-replace sites;
- B16: file_upload.{h,cpp}.

Added since the plan, with no batch needed: settings_main.cpp (version
footer), data_document_media.cpp (download guard) and
history_view_context_menu.cpp (import action) are already hooks. The
`purple_version.cmake` include moved from CMakeLists.txt into purple.cmake in
B1.

Not named by any batch, with a proposed owner: dialogs_entry.{h,cpp} (B4 for
the comments, B14 for the virtuals) and main_window.cpp (B15).

## Account-less Work Mode calls

48 lines today, by owner:

- B3: 13 in data/data_session.cpp (they move into a Purple TU);
- B4: 13 in history/history.cpp (they move), and 1 in
  history/history_unread_things.cpp (`UnreadThingEdge` takes the thread);
- B10d: 9 in data/data_chat_filters.cpp (8 move,
  `RefuseFolderOrderSave(session)` replaces 1), and 1 in
  data/data_unread_value.cpp (moves);
- B11: 3 in data/notify/data_notify_settings.cpp (they move);
- B10u: 2 in ui/widgets/chat_filters_tabs_strip.cpp and 2 in
  window/window_filters_menu.cpp (`PinWholeStripIfRestricted(session)`);
- B13: 2 in dialogs/ui/dialogs_layout.cpp (the style comes from
  `context.purpleMark`);
- B8: 1 in window/window_peer_menu.cpp (`TogglePinnedInView` reads the
  controller's session);
- B12: 1 in dialogs/ui/dialogs_suggestions.cpp (the recommendations
  predicate takes the session).

The 11 upstream lines that must carry the account after the refactor are B4
1, B8 1, B10u 4, B10d 1, B12 2 and B13 2. All are lines their batches rewrite
anyway, so they add no hunks.
