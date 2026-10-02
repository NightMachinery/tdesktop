# Purple version

Purple Telegram has a version of its own, separate from the Telegram Desktop
version it is built on. Desktop shows it as one line:

    Purple 1.0.0 (864b1934d3+dirty, core 337829e)

Clicking the line copies it to the clipboard and shows "Text copied to
clipboard." It appears in two places:

- at the bottom of the main Settings page, below the help rows;
- as the first row of Settings > Advanced > Purple.

Settings search finds it under "purple", "version", "commit" and "build".
Telegram's own version is not in Settings on desktop: it is in the main menu
footer and in the About box, and the Purple line is not shown there.

## The three parts

**The version number** is `kVersion` in
`Telegram/SourceFiles/purple/purple_version.cpp`. It started at 1.0.0 on
2026-10-02. The coordinator bumps it when a build is delivered: the minor
number (1.1.0) when the delivered build adds features since the last
delivered build, the patch number (1.0.1) when it only fixes things. Android
is to have a Purple version of its own, bumped by the same rule when an APK is
delivered; the two clients ship at different times, so their numbers are not
meant to match.

**The desktop commit** is `git rev-parse --short HEAD` of the top-level
worktree, the same abbreviation `git log --oneline` shows. `+dirty` follows it
when `git status --porcelain --untracked-files=no --ignore-submodules=all`
lists anything at build time: staged or unstaged changes to tracked files.
Untracked files and changes inside submodules do not count, and neither does
a submodule checked out at another commit than the one recorded, because the
core commit already shows that.

**The core commit** is the commit checked out in
`Telegram/ThirdParty/purple_core`, the purple-core submodule that was
compiled. It can differ from the commit the desktop commit records for the
submodule; the line shows what was built.

Either commit is `unknown` when git is not found or the directory is not a git
checkout (it has no `.git` of its own), so a copy of the sources outside git
never reports the commit of some enclosing repository.

## How the commits stay right in incremental builds

The commits are taken at build time, not at configure time. The
`purple_build_info` custom target runs `Telegram/cmake/purple_build_info.cmake`
on every build. The script computes the header
`out/Telegram/gen/purple/purple_build_info.h` and writes it only when its
content differs from the file already there. `Telegram` depends on the target,
so the header is current before anything compiles, and only
`purple_version.cpp` includes it. A build after a commit, a checkout or an
edit to a tracked file therefore recompiles that one file and relinks; a build
where none of that changed recompiles nothing for it.

`purple/test_version.sh` checks the line format and runs the script against a
throwaway repository with a submodule: a clean tree, a second run that must
not rewrite the header, an untracked file, a dirty and then a moved
submodule, a tracked change, a new commit, a core directory that is not a
checkout, and no git at all.

## Upstream files touched

- `Telegram/CMakeLists.txt`: one `include(cmake/purple_version.cmake)` line.
  The module, `Telegram/cmake/purple_version.cmake`, adds the custom target
  and the two Purple sources.
- `Telegram/SourceFiles/settings/sections/settings_main.cpp`: an include and
  `Purple::AddVersionFooter(builder)` at the end of the page.
- `Telegram/SourceFiles/settings/sections/settings_advanced.cpp`: an include
  and `Purple::AddVersionRow(builder)` in the existing Purple section.
