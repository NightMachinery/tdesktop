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

**Strings (B2).** New Purple strings go in
`Telegram/Resources/langs/purple.strings`, in lang.strings' format, never in
lang.strings itself. A build step appends purple.strings to upstream's
lang.strings, and codegen_lang reads the result, so `tr::` keys, plurals and
tags work exactly as for upstream's strings. A Purple key may not repeat an
upstream one: codegen_lang stops with "duplicate found for key", which is
also what happens if upstream later adds a key of the same name. Editing
either file needs no CMake reconfigure.

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
- 45 account-less Work Mode calls in 10 upstream files after B11. The count must reach
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
- B2 Purple strings in a Purple-owned `purple.strings`, appended to
  lang.strings by a build step (D1 B; done 2026-10-03);
- B5 the settings-offer id out of SessionSettings, kept as a per-account pref
  (D4: keep the offer until the sync chat ships), plus one forced
  SessionSettings rewrite per session start (done in B5);
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

**`Telegram/Resources/langs/lang.strings`**: unchanged from the merge-base
after B2 (+42 -0, 3 hunks, near 1 before). Done in B2.
- Its 41 Purple lines (38 keys: 31 Last Seen Peek lines, 9 pinned-music
  lines and `lng_settings_replace_dashes`) moved, text unchanged, to
  `Telegram/Resources/langs/purple.strings`. See "Strings (B2)" above.

**`Telegram/cmake/td_lang.cmake`**: +1 -1, 1 hunk, near 1. Done in B2.
- The one line is `include(cmake/purple_lang.cmake)`, in place of upstream's
  `generate_lang(td_lang ${res_loc}/langs/lang.strings ${src_loc})`.
  `Telegram/cmake/purple_lang.cmake` adds a build step, which runs
  `purple_lang_merge.cmake` to write lang.strings followed by purple.strings
  to `out/Telegram/purple_lang/lang.strings`. It then makes upstream's call
  with that file.
- It replaces the call instead of adding a line because codegen_lang reads
  exactly one input file, and this call is what names it. The only way to
  redirect it from another line would be to redefine upstream's
  `generate_lang()` function, which hides what the build does.
- The near-hunk edit is upstream's b880396d60 (2026-07-13), which added the
  `${src_loc}` argument to this very line. A change like that conflicts
  here. To resolve it, keep the include and carry the new arguments into the
  `generate_lang()` call at the end of purple_lang.cmake. Both wrong
  resolutions fail loudly: taking upstream's line drops the Purple keys, so
  every Purple `tr::` use stops compiling; and keeping the include without
  carrying over a new required argument stops configure. The exposure is
  the same as lang.strings' was (near 1). The gain is that no new string
  touches an upstream file.
- The merged file is also named lang.strings, because codegen_lang writes
  its input's file name into every generated header. codegen_lang also keeps
  each key's index in `out/Telegram/gen/lang_auto.indices` and each tag's
  number in `lang_auto.tags`, so moving the Purple keys after upstream's
  renumbered nothing in out/. After B2, lang_auto.h, lang_auto_keys.h,
  lang_auto_counts.h, the indices, the tags and all 1,363 subset headers
  were byte-identical. lang_auto.cpp differed only in the order of the cases
  in `IsTagReplaced()`'s switch, which follows file order; each key still
  maps to the same tags.
- A new build directory has neither file, so there codegen_lang numbers keys
  and tags in file order: the Purple keys come after upstream's, the
  Purple-only tags `last_seen` and `age` come last, and `duration` is
  numbered where upstream first uses it. Only generated code sees these
  numbers: the cached language pack stores keys by name.
- codegen_lang's errors name the merged file. Line N after its
  "purple.strings starts here" comment is line N of purple.strings.
- Editing purple.strings or lang.strings reruns the merge and codegen_lang
  without a CMake reconfigure.

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

**`AGENTS.md`** (+144 -3, 6 hunks, near 6, after B2's strings bullet) and
**`README.md`** (+32 -1, 4 hunks, near 1): owner B18. The Purple sections
move to `docs/purple/agents.md`, leaving a pointer and the hook rule (D6).
The B1 and B2 bullets (new sources in purple.cmake, new strings in
purple.strings) belong with the hook rule.

## Work Mode core

**`Telegram/SourceFiles/data/data_session.cpp`**: +607 -11, 23 hunks, near 5.
Owners: B3 (stage 1), B11 (two lines), B14 (stage 2).
- Today: the 15 `Session::purple*` definitions have moved to
  `purple/purple_work_view.cpp`; the member definitions retain their account.
- Leaves after B3c: the `purple/hooks/mute.h` include, constructor timers and
  setup call, five `Session` helpers that claim every Purple view id, one
  Purple refresh call in each chat-list entry method, and nine badge swaps.
  A preset-owned main view saves pins locally; a mirroring main view re-enters
  upstream with `FilterId(0)` and saves the ordinary server order. The badge
  swaps cannot move because folder badges also read the main-list totals; all
  five near-hunk edits stay on those lines.
- B11 gives the two mute calls inside the Purple block (lines 1881 and 5969)
  their final names, and B3 then moves them verbatim.
- Account: pin hooks and two list calls are in hand (`this`, a `Data::Session`);
  the moved definitions retain their owning `Data::Session`.

**`Telegram/SourceFiles/data/data_session.h`**: +70 -0, 4 hunks, near 0.
Owners: B3, B14.
- Today: the public view-list accessors, the private Purple members and the
  timers.
- Leaves: about 75 declaration lines until B14 (B3 adds five private
  declarations); B14 moves the state into a per-session `Purple::WorkView`.

**`Telegram/SourceFiles/history/history.cpp`**: +515 -8, 10 hunks, near 0.
Owners: B4 (stage 1), B14.
- Today: the 365-line block of `History::purple*` definitions has moved to
  `purple/purple_history.cpp`; constructor, unread, membership, time and
  opened-state hooks remain in the upstream files.
- Leaves after B4a: the inline constructor check, `setFakeUnreadWhileOpened`
  calls, kept-for-view time, unread-state override and `shouldBeInChatList`
  hooks remain, together with the explicit Purple include.
- Account: the member definitions retain their owning History. The 13
  account-less calls move into `purple/purple_history.cpp` with B4.


**`Telegram/SourceFiles/history/history.h`**: +125 -0, 4 hunks, near 0.
Owners: B4, B14.
- Today: the `purple*` member declarations and the opened and grace state.
- Leaves: the declarations until B14; then `_purpleUncounted` and its setter,
  with the opened and grace state in a WorkView map.

**`Telegram/SourceFiles/history/history_unread_things.cpp`**: +25 -1, 3
hunks, near 0. Owner: B4.
- Today: `setCount` has a Purple mention/reaction edge block outside its
  `inChatList()` branch.
- Leaves after B4a: unchanged; the Purple edge block and gate include remain.
- Account: the later `UnreadThingEdge` hook takes `_thread` or its owning
  history, replacing the account-less `Purple::Filtering()` check.


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
- B10u leaves `ShowViewTabMenu`, `ShownList(session)` token swaps, upstream
  pinned intervals plus the `PinWholeStripIfRestricted(session)` hook, and the
  upstream icon condition with `UseAllFilterIcon` preserving the views' All icon.
  B10d later decides whether the shown-list wrapper
  becomes a free function.
- Account: the strip's session is passed at both pin sites, lines 288 and 456.
  Current preset settings remain shared app-wide.

**`Telegram/SourceFiles/window/window_filters_menu.cpp`**: +54 -13, 13
hunks, near 5. Owner: B10u.
- B10u leaves the `FilterStripChanges` and `ShownList(session)` facade calls,
  upstream pinned intervals plus `PinWholeStripIfRestricted(session)`, and the
  `FillViewMenu` hook. `UseAllFilterIcon` preserves the views' All icon; nonview
  filter icons keep upstream computation.
- Account: `PinWholeStripIfRestricted(session)` carries the owning session at
  lines 364 and 843. Current preset settings remain shared app-wide; the
  session is passed at the seam for its folder data and future account policy.
  The `ActiveChanges()` signal itself is global.

**`Telegram/SourceFiles/dialogs/dialogs_inner_widget.cpp`**: +47 -7, 12
hunks, near 3. Owners: B10u, B3c, B13.
- Today: `refreshWithCollapsedRows`' view term, the temporary-row paint
  block, the `cacheAllowed` term, `savePinnedOrder`'s view branch,
  `refreshShownList`, `refreshEmpty`, `switchToFilter` and `setupShortcuts`.
- B10u leaves: one condition in `refreshShownList`, the original nonzero-id
  predicate in `refreshEmpty`, one guard in `switchToFilter`, one
  `|| Data::IsPurpleView(_filterId)` term in `refreshWithCollapsedRows`, and
  the `ShownList(session)` token swap in `setupShortcuts`. `savePinnedOrder`
  stays with B3c.
- B3c leaves upstream's `else if (_filterId)` in `savePinnedOrder`, preceded
  by `} else if (Purple::SavePinnedViewOrder(&session(), _filterId,
  _openedFolder)) {` with an empty body. The owner-session hook saves an extra
  view or preset-owned view 0 to settings, and re-enters the ordinary API save
  for a mirroring main view. This is the only `savePinnedOrder` edit.
- B13 leaves: `context.purpleMark = Purple::RowMarkFor(row->entry());` in
  place of the 13-line paint block, and the `cacheAllowed` term testing that
  field.
- Account: `SavePinnedViewOrder(&session(), ...)` and
  `RowMarkFor(row->entry())` are *in hand*; the view-id tests need none.

**`Telegram/SourceFiles/dialogs/dialogs_widget.cpp`**: +5 -1, 2 hunks, near
0. Owners: B10u, B12.
- B10u leaves one `Purple::EscapeToHome(controller())` call in `escape()`.
  B12 adds the top-peers restart at line 2151 (see "Suggestions, stories and
  top peers").

**`Telegram/SourceFiles/window/window_session_controller.cpp`**: +44 -14, 8
hunks, near 3. Owners: B10u, B7, B5.
- Today: three includes, the settings offer at session start, the
  `ActiveChanges()` producer in the filters-menu merge, `WatchScreenTime`,
  `checkOpenedFilter` rewritten, home-filter literals in `openFolder` and
  `openCommunity`, and the `CheckAndJumpToNearChatsFilter` swap.
- B10u leaves `Purple::CheckOpenedView(this)` before the restored upstream
  `checkOpenedFilter` body, `Purple::HomeFilterId(&session())` at four
  home-selection sites, one `Purple::ActiveChanges()` merge term, and the
  `CheckAndJumpToNearChatsFilter` forwarding hook. B10a first makes Normal's
  home id zero; callers use `defaultId()` for home only while a preset is
  active.
- B7 leaves: `Purple::WatchScreenTime(this)` without its 3-line comment.
- B5 and D4: `Purple::OfferNewerSettingsFromSavedMessages(session, uiShow())`
  stays (a 3-line hook) until the sync chat ships and the offer retires.
- Includes: one facade include (rule 5).
- Account: `CheckOpenedView(this)` and `HomeFilterId(&session())` are *in
  hand*; `ActiveChanges()` needs none.

## Mute

**`Telegram/SourceFiles/data/notify/data_notify_settings.cpp`** and
**`Telegram/SourceFiles/data/notify/data_notify_settings.h`**: B11 leaves
`purpleRefreshMute()` and its declaration as the thin entry into private
`updateLocal()`. The original upstream peer `isMuted(peer, changesIn)` body
follows `Purple::PresetMutes(peer, changesIn, kMaxNotifyCheckDelay)`.
`MutedWithoutPreset` calls public `isMuted()` under an owner-scoped bypass;
it does not duplicate topic, community or default mute logic. Folder policy
and session-lifetime transition state live in `purple/purple_mute.cpp`.

**`Telegram/SourceFiles/menu/menu_mute.cpp`**: B11 calls `Purple::Silenced`
and `Purple::MutedWithoutPreset`. B8's menu composition and preset name
presentation remain unchanged. `menu/menu_mute.h` and `Descriptor::purplePreset`
are unchanged.

**`Telegram/SourceFiles/info/profile/info_profile_values.cpp`**: both topic
and peer notification controls call `Purple::MutedWithoutPreset` for the
actual owner.

**`Telegram/SourceFiles/settings/sections/settings_notifications_type.cpp`**:
the exception row calls `Purple::MutedWithoutPreset(peer)`.

**`Telegram/SourceFiles/history/history_widget.cpp`**: the notification button
label and toggle call `Purple::MutedWithoutPreset(_history)`. B7's composer
and screen-time code remain unchanged.

**`Telegram/SourceFiles/data/data_chat_filters.cpp`**: the existing
`ignorePresetMute` extension calls `Purple::MutedWithoutPreset(history->peer)`.

**`Telegram/SourceFiles/data/data_session.cpp`**: the preset refresh walk calls
`Purple::RefreshMute(peer)` and the existing chat-entry transition site calls
`Purple::RefreshFolderMute(history->peer)`. Both retain their ordering.

**`Telegram/SourceFiles/window/window_peer_menu.cpp`**: the existing preset
row and ordinary mute choice use `Purple::Silenced` and
`Purple::MutedWithoutPreset`. B8 composition is unchanged.

Each B11 upstream implementation adds the `purple/hooks/mute.h` facade
include. The peer, topic, history or thread carries its actual session;
notification defaults, folder membership and deferred refresh use that owner.
The settings, active preset, peek and overrides retain the existing shared
app-wide policy. This batch adds no independent per-account preset selection
or sync-token opt-in.
`Telegram/cmake/purple.cmake` registers only the new Purple mute implementation,
header and facade. See [work_mode.md](work_mode.md#mute-upstream-hooks).

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

**`Telegram/SourceFiles/window/window_peer_menu.cpp`**: B8 extraction, after
B11.
- One `purple_chat_menus.h` include replaces the direct Purple helper includes.
  The mute row moves behind `AddPresetMuteRow`; the thread's notification
  settings, `MutedWithoutPreset`, submenu ordering and actual peer/session
  remain tied to the B11 caller. The three Purple-only Filler members become
  free functions called at their existing menu positions. The local-premium
  translation term moves behind the facade.
- `TogglePinnedInView` owns only Purple views. It receives the actual
  controller, entry, filter id and callback. Extra views and an owned main
  view keep the existing settings-list behavior; the mirroring main view
  re-enters the public four-argument `TogglePinnedThread` with `FilterId(0)`.
  Ordinary server-filter behavior remains upstream, including `if (!filterId)`.
- The pin-ownership query receives `&controller->session()`. The active preset
  and its pin order remain shared app-wide in this version of Work Mode; this
  call does not add an account selector or synchronization binding.

**`Telegram/SourceFiles/window/window_main_menu.cpp`**: B8 extraction.
- One `purple_chat_menus.h` include replaces the direct preset-box include.
  The preset label and row position stay the same; opening it goes through
  `Purple::ShowPresetBox(controller)`. The existing generic-box include stays
  for unrelated menu boxes.

## Last Seen

B6 extracts the desktop surfaces into `purple_last_seen_ui`. The existing
`purple_last_seen` engine, privacy requests, journal and cooldown are unchanged.

**`Telegram/SourceFiles/history/view/history_view_top_bar_widget.cpp`** and
**`Telegram/SourceFiles/history/view/history_view_top_bar_widget.h`**: B6.
- One UI facade include, `LastSeenTail` state and its resets, and
  `OnLastSeenInputsChanged`. `TopBarOnlineText` chooses the same long or narrow
  note using the available status width. The online-color decision, support
  warning, group status, general layout and mouse-button gate stay upstream.
- `LastSeenTailRect` repeats the existing scaled top-bar geometry and RTL
  transform; elided, absent and fully narrow tails remain non-clickable.
  `HandleLastSeenTailClick` consumes only a user tail hit. Resizes refresh only
  while the tail is shown, as before.

**`Telegram/SourceFiles/info/profile/info_profile_status_label.cpp`** and
**`Telegram/SourceFiles/info/profile/info_profile_status_label.h`**: B6.
- `AttachProfileStatus` subscribes the user label to StateChanges and
  SettingsChanges under the StatusLabel lifetime and a label guard.
  `ProfileStatusBody` preserves full/plain text, online color and link index 3.
  `ApplyLastSeenLink` follows the upstream member/hidden link block, which keeps
  indices 1 and 2. The Last Seen setter, getter and callback member are removed.
- The label owns its guarded click handler. It resolves its actual window at
  click time, requires the original live session and loads that session's user
  by typed ID. It never borrows another account's active controller.

**`Telegram/SourceFiles/info/profile/info_profile_top_bar.cpp`**: B6 and D16.
- The `show_or_premium_box.h` include and upstream button text are restored,
  with one Purple UI include. `SetupShowLastSeen` handles the existing fork
  branch after the original eligibility and premium early returns. The latter
  retains `|| Purple::LocalPremium()`. Purple sets the live Peek text and owns
  the button subscriptions and guarded click; there is no fallback to the
  permanent Everyone privacy action. The clang-15 aggregate fix stays unchanged.
- The planned deferred first label refresh is unsafe for custom statuses:
  construction and `adjustColors` bind the custom producer after setup. A queued
  refresh would then overwrite its text. The allowed fallback keeps two
  synchronous `RefreshProfileStatus` owner hooks before those custom bindings.
  They retain no callback. Link construction and callback-copy plumbing are gone.
- Drift obligation: the Purple helper repeats the LastSeen privacy value/filter/
  refetch block (9 nonblank lines excluding the separating blank),
  `setOpacity(0.)` and `setFullRadius(true)`. Compare all 11 against upstream on
  every merge. The current moved block is retained without changing its
  Everyone condition, hidden-by-me refetch or button lifetime. The surrounding
  premium-transition/online/CanPeek predicates also remain the established fork
  behavior; this batch does not change the contradictory older premium comment.

**`Telegram/SourceFiles/boxes/peer_list_box.cpp`**: B6.
- One facade include, `RowOnlineText` and `RowOnlineChangeTimeout`. Row
  activation, online color, Saved Messages and non-user paths stay unchanged.
  The 60-second cap applies only when the note has a tail, preserving the
  existing row scheduling policy.

**`Telegram/SourceFiles/boxes/peers/prepare_short_info_box.cpp`**: B6b.
- The existing `LastSeenNoteFor(user, now, false, true).text` stays. Its timer
  uses `RowOnlineChangeTimeout`, and each emission reads the current clock so
  remembered ages and expiration advance, rather than capturing the opening
  time for the producer lifetime.

**`Telegram/SourceFiles/boxes/peers/edit_participant_box.cpp`** and
**`Telegram/SourceFiles/history/view/history_view_chat_preview.cpp`**: unchanged
by B6. Their existing per-site `LastSeenNoteFor(..., false, true).text` swaps
remain. There is no global interception in `Data::OnlineText`, which would
change unrelated callers and discard the distinct full/narrow arguments.

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
- Today: two includes, direct cover construction, peer and geometry calls,
  `NoteScreenTimeAction` for voice, typing, send, attach, reply and edit, and
  a send activity hook before upstream's `if` / `else if` chain.
- Leaves: one `purple_screentime_history_widget.h` facade include and one
  facade call each for cover construction, peer and geometry, one null-safe
  `NoteScreenTimeAction` before the null-history / `else if (_editMsgId)`
  draft branch chain in `send()`, and one call per other activity.

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
  and one call each for typing, edit and reply. The helper reads `_history`
  for each event and binds subscriptions to the passed lifetime, because the
  controls outlive the chat.
- B7b: the recorder observes the unfiltered internal stream before
  `sendRequests()` applies the stars check, avoiding a second `editStarsFrom`
  dialog while counting an insufficient-stars send request once.

`Telegram/SourceFiles/core/application.cpp` starts the recorder; see
"App-level hooks".

## Settings UI and instant replaces

**`Telegram/SourceFiles/settings/sections/settings_advanced.cpp`**, owner B9:
- The three `BuildPurple*` bodies move to `purple/purple_settings_section.cpp`.
  The `kMeta` callback keeps one `Purple::BuildAdvancedSection(builder)` call
  in its existing position, before the screen-reader section. The order on the
  Advanced page, search metadata, version row and actual `builder.session()`
  stay the same.

**`Telegram/SourceFiles/settings/sections/settings_chat.cpp`**, owner B9:
- The dash search entry and checkbox move behind two Purple facade calls.
  Their label, id, keywords, checked state, checkbox highlight and disabled
  state tied to Replace emoji stay the same.
- The read and write live in `purple_instant_replaces.cpp`, using the existing
  generic preference key `purple-replace-dashes`.

**`Telegram/SourceFiles/settings/sections/settings_main.cpp`**: +3 -0, 3
hunks, near 4. Final.
- One include, placed per rule 5 (near 1; it scored 2 in the alphabetical
  block), and `Purple::AddVersionFooter(builder)` after
  `BuildHelpSection(builder)` twice: in `Main::setupContent`, which draws the
  page (near 2), and in `kMeta`, which builds the search index (near 1). The
  page call was missing until 2026-10-03, so the row was searchable but never
  drawn (bug V1). See version.md.

**`Telegram/SourceFiles/core/core_settings.cpp` and `core_settings.h`**, owner
B9:
- The dash preference key, accessor, setter and member leave `Core::Settings`.
  The changed `_replaceEmoji` and `_systemTextReplace` defaults remain false.
- `resetOnLastLogout` still clears `_prefs`; after that, the Purple getter sees
  the missing key and returns false. No live input field has to outlast the
  logout that clears the preference.

**`Telegram/SourceFiles/core/ui_integration.cpp` and `ui_integration.h`**, owner
B9:
- `UiIntegration::instantReplaceAllowed` delegates to Purple's dash policy.
  The generic `Ui::Integration` hook in the lib_ui fork defaults to allowing a
  replacement. `InputField` asks it only after a trie match, at event time, so
  every field honors the current dash setting without replacing its map or
  suppressing other replacements.

**The 12 instant-replace sites**, owner B9 under D14: the default-map calls in
11 sites return to upstream form and keep their existing per-field
`replaceEmoji` and system-text-replace toggles. `edit_tag_control.cpp` uses
`Purple::TextOnlyFollowToggles` to keep its text-only map and both toggles.
The lib_ui filter handles the dash choice for all of them and for fields added
later:
- `chat_helpers/message_field.cpp`: three sites;
- `boxes/peers/edit_peer_info_box.cpp`: two sites;
- `boxes/add_contact_box.cpp`: two sites;
- `boxes/peers/add_to_community_box.cpp`, `settings/sections/settings_information.cpp`,
  `support/support_helper.cpp`, `boxes/edit_todo_list_box.cpp` and
  `boxes/peers/edit_tag_control.cpp`: one site each.

**Verify**: as in the first plan. Under D14, use an existing replacement-enabled
field, such as an originally empty Saved Messages composer, for the live dash
OFF/ON comparison without restarting. Restore the draft and both preferences
afterward, without sending. Main Search has no replacement map and is a negative
control, so it cannot establish the positive toggle behavior.

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
- The optional inert paint witness adds one include and a default-disabled startup call. Its descriptor validation and work remain in Purple.
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

**`Telegram/SourceFiles/main/main_session_settings.cpp`** and
**`Telegram/SourceFiles/main/main_session_settings.h`**: owner B5.
- B5 restores upstream serialization/deserialization, including the reserved
  size, and removes the offer-id and offer-start members. Only the existing
  `_archiveInMainMenu` default remains a Purple difference in SessionSettings.
- `purple_session_settings` stores the offer id in the account's QByteArray
  pref `purple.sync.settings_offer_message_id`. It does not migrate the old
  serialized id. Its offer-start latch belongs to the original session lifetime;
  inactive/no-show answers release it, while completed searches keep it set.
- Old trailing bytes are ignored without changing preceding settings. One
  guarded queued rewrite per session start strips the old tail. Every account
  profile must run a B5 build once before a build with a new upstream field.
  Skipping that build can make an old Purple id look like an upstream count.

**`Telegram/SourceFiles/main/main_account.cpp`**: B5 leaves one include and
`Purple::StartSessionSettings(_session.get())` after `_session` is constructed,
self data restored and `_sessionValue` published. The Purple function queues the
write behind the original session guard and rechecks actual account ownership
and logout state before calling the existing save path. It does not
send, import, change focus or modify sync state.

**`Telegram/SourceFiles/storage/storage_account.cpp`**: existing prefs reset
and QByteArray extension stay. B5 adds one include and
`Purple::SessionSettingsWritten(_owner, userDataInstance, userData.size())`
after the existing encrypted-payload preparation. Purple records that actual
payload count during the forced write, then logs after the save path returns
and the descriptor has submitted the existing storage write:
`Purple: rewrote SessionSettings, N bytes` only for the forced write of that
session's actual settings object; N is the serialized payload used by storage,
not a second serialization or encrypted-file size. Other writes do not log this
line. Storage's existing write/error handling remains responsible for disk I/O.
The rewrite and offer state disappear when the session lifetime ends.

**`Telegram/SourceFiles/data/data_document_media.cpp`**: +3 -0, 2 hunks,
near 0. Final.
- One include and an `else if (Purple::IsSyncRecordFileName(...)) { return; }`
  in `automaticLoad`, so sync records are never auto-downloaded. The include
  sits in the alphabetical block; move it per rule 5 when the file is next
  touched.

**`Telegram/SourceFiles/history/view/history_view_context_menu.cpp`**: the
former import include and raw `AddDocumentActions` import call are removed.
Import now consumes the current invocation's qualified record in the list
builder after `FillContextMenu`; document saving and ordinary grouped menu
behavior remain upstream. See "Import content provenance and menu routes"
below for the replacement extension sites.

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
- B7: `WatchScreenTime(this)` without its explanatory comment;
- B5 and D4: the settings-offer hook in window_session_controller.cpp;
- B9: the twelve instant-replace sites, including the text-only tag field;
- B16: file_upload.{h,cpp}.

Added since the plan, with no batch needed: settings_main.cpp (version
footer) and data_document_media.cpp (download guard) are already hooks. The
former history_view_context_menu.cpp import hook is removed in favor of the
qualified builder hooks described below. The
`purple_version.cmake` include moved from CMakeLists.txt into purple.cmake in
B1.

Not named by any batch, with a proposed owner: dialogs_entry.{h,cpp} (B4 for
the comments, B14 for the virtuals) and main_window.cpp (B15).

## Account-less Work Mode calls

45 lines after B11, by remaining owner:

- B3: 13 in data/data_session.cpp (they move into a Purple TU);
- B4: 13 in history/history.cpp (they move), and 1 in
  history/history_unread_things.cpp (`UnreadThingEdge` takes the thread);
- B10d: 9 in data/data_chat_filters.cpp (8 move,
  `RefuseFolderOrderSave(session)` replaces 1), and 1 in
  data/data_unread_value.cpp (moves);
- B11: none in upstream notification settings after the mute extraction;
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

## Import content provenance and menu routes

Qualification, canonical session/message/document checks and action lifetime
are owned by `purple_sync`. Native files expose the existing final hit and
read-only state; no native hit-test, grouping or accessibility implementation
is copied or moved. The following entries describe the added extension
surface, not a new measurement of the older merge-base figures above.

**`Telegram/SourceFiles/history/history_inner_widget.cpp`**:
- One guarded Purple call in the key handler supplies the current focused
  item/view only for macOS Control+Return. Purple owns exact physical modifier
  matching, current receiver checks, keyboard context-event dispatch and the
  short-lived filter for repeats of that initiating shortcut.
- A guarded Purple provenance check suppresses the hovered click handler for
  every keyboard context event, independently of Import eligibility. Mouse
  dispatch and Import qualification remain unchanged.
- Purple computes keyboard selection from focused-item membership, restricts
  sponsored/reaction identities to that focused item and makes album-group
  behavior independent of pointer hit testing. Non-keyboard fallback callbacks
  keep the existing mouse decisions; Import qualification is unchanged.
- The inert witness adds a guarded paint scope, final ordinary-menu provenance call, and dirty notifications at existing focused-item mutations. The captured lookup is weak and resolves the current view without accessibility-map mutation.
- One Purple include, an optional output propagated/reset through both mouse
  update overloads, one capture before `setActive`, one keyboard preparation
  and one consumer after specialized/userpic exits. Capture rejects nonempty
  reaction/reply snapshot owner ids even when their links or current views
  are absent. A read-only observer
  exports visible item geometry and existing selection, touch, report,
  overlay, resize, reveal and removal readiness. Purple exclusions invalidate
  only the optional result; stock refresh and menus continue.

**`Telegram/SourceFiles/history/history_inner_widget.h`**:
- Forward declarations and private optional-output/read-only signatures.

**`Telegram/SourceFiles/history/view/history_view_list_widget.cpp`**:
- One guarded Purple call supplies the secondary receiver's current focused
  item/view for the same macOS shortcut. Existing keyboard menu construction
  and import qualification remain the consumer; Files is unchanged.
- The same Purple provenance check keeps secondary keyboard context events
  independent of hovered click handlers and Import eligibility.
- A Purple keyboard request hook binds the current focused item/view, uses
  deliberate whole-album point state, limits text/selected-set operations to
  focused membership and skips pointer selection hit testing. Mouse/touch use
  their original request branch. Reaction attachment reads the resulting
  request state instead of a stale mouse point.
- The inert witness adds the same weak paint scope, exact secondary asking-menu provenance and focused-item dirty hooks. It does not change menu construction or focus.
- One Purple include and the same optional capture/keyboard protocol. Its
  single consumer follows `FillContextMenu` and precedes the empty-menu check.
  Specialized dispatch and `ContextMenuRequest` share the local effective
  link. Capture rejects each nonempty invocation-local reaction/reply owner
  id independently of resolved views and links. The observer adds existing
  refresh/reveal/resize readiness to native
  selection, overlay, touch and removal state.

**`Telegram/SourceFiles/history/view/history_view_list_widget.h`**:
- Forward declarations and private optional-output/read-only signatures.

**`Telegram/SourceFiles/history/view/history_view_context_menu.cpp`**:
- The old include and direct item/document import call are removed; the
  qualified builder consumer above is the sole secondary import insertion.

**`Telegram/SourceFiles/info/media/info_media_list_widget.cpp`**:
- One Purple include, optional output/reset on both existing mouse overloads,
  one final row capture before `setActive` and one document-menu consumer.
  A read-only observer exposes the viewport below the top overlay, row
  geometry and existing selection/reorder/return/shift state. Purple checks
  the exact row/global id, self Media/File section and parent session; Files
  keyboard/other sources have no record. Stock save and Finder actions stay.

**`Telegram/SourceFiles/info/media/info_media_list_widget.h`**:
- Forward declarations and private optional-output/read-only signatures.

**`Telegram/SourceFiles/history/view/history_view_cursor_state.h`**:
- Generic default-None content origin plus an independent producer message id,
  with paired set/clear accessors. Selection offsets and final grouped member
  ids preserve producer ownership. This is evidence about native content,
  rather than a settings-import policy.

**`Telegram/SourceFiles/history/view/history_view_message.cpp`**:
- The normal body draw borrows its existing empty HighlightInfoRequest slot through one Purple call. A neutral const bodyText accessor exposes the exact leaf for current-state comparison. Selection/ripple/reveal/highlight paths retain precedence.
- Plain/rich winning body glyphs mark the actual `textItem()` owner. Rich
  results also require the final native text cursor. Right-action, summarize
  and all three `onlyMessageText` media-suppression replacements clear origin
  and owner together. Native cursor/handler precedence stays unchanged.

**`Telegram/SourceFiles/iv/markdown/iv_markdown_article.h`**:
- One default-false `bodyGlyph` observation on the hit result.

**`Telegram/SourceFiles/iv/markdown/iv_markdown_article.cpp`**:
- Only `HitTextSegment` sets that observation, from `insideText` and the
  underlying `uponSymbol`; wrappers propagate it without synthesizing it.

**`Telegram/SourceFiles/history/view/media/history_view_document.cpp`**:
- The normal caption draw has the same empty-highlight hook and a neutral nullable const captionText accessor. Grouped owners remain outside the witness catalog. Nongroup caption reachability is not established.
- Winning open/save/open-with document-card handlers and actual caption
  glyphs mark `realParent` ownership. Caption links and offsets stay intact;
  cancellation, seeking and transcription controls remain unmarked.

**`Telegram/SourceFiles/history/view/media/history_view_gif.cpp`**:
- Winning open/save card handlers, including grouped results, mark
  `realParent`. Spoiler/seek controls stay unmarked; later right-action and
  transcription replacements clear origin and owner. Purple still requires
  positive File classification, so ordinary animation/video hits do not
  become settings-file imports.

**`Telegram/SourceFiles/history/view/media/history_view_theme_document.cpp`**:
- The actual constructed parent-data owner marks the open-document winner.
  No invented `realParent` parameter or cancellation producer is added.

**`Telegram/SourceFiles/history/view/media/history_view_media_grouped.cpp`**:
- Whole-media spoiler/right-action replacements clear provenance. Existing
  member-id rewriting and selection offsets remain unchanged, allowing
  Purple to reject an owner/member mismatch without a group fallback.

**`Telegram/SourceFiles/history/view/media/history_view_web_page.cpp`**:
- Nested attachment provenance is cleared immediately after its returned hit,
  before sponsored, hint or handler replacements. The same native WebPage
  type is used by log-original and fact-check previews.

**`Telegram/SourceFiles/overview/overview_layout.cpp`**:
- Document-handler filename/icon/thumbnail/corner regions mark the actual
  row owner. Date jump handlers and whitespace stay unmarked. These native
  observations do not turn a non-File attachment into an import target.

**`Telegram/SourceFiles/ui/effects/thanos_effect.h`** and
**`Telegram/SourceFiles/ui/effects/thanos_effect.cpp`**:
- Declaration and implementation of a read-only accessor for existing shown
  state, covering the interval between animation completion and queued hide.

**`Telegram/SourceFiles/ui/effects/thanos_effect_controller.h`** and
**`Telegram/SourceFiles/ui/effects/thanos_effect_controller.cpp`**:
- Declaration and implementation of the read-only `geometryBusy()` observer.
  It reads existing effect animation/shown state, collapse animation,
  internal/published collapse gaps and unconsumed removal height. No
  capability probe, forced layout, pointer-existence gate or independent
  precapture/prepend/restore/saved-scroll gate is introduced. Native hide,
  collapse completion and geometry/paint consumption release those fields.

## Inert attachment witness extension

The extension above leaves lib_ui, core and Qt unchanged. The Message and
Document headers add only their respective neutral const leaf accessor
declarations. All transport, admission and native association decisions live
in Purple-owned files registered by purple.cmake. See [ui_witness.md](ui_witness.md)
for the closed grammar, strict wire and explicit remaining qualification gates.
