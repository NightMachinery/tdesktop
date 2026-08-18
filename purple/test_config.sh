#!/usr/bin/env bash
# Compiles and runs the standalone tests for the Purple config core.
#
# The parser and the splice engine are deliberately free of tdesktop
# dependencies, so they build straight into a small harness here rather than
# needing the app. That matters: settings.toml is hand-owned, the splicer's job
# is to leave the user's comments alone, and proving that takes a few hundred
# fixture edits - far too slow to iterate on through a full app build.
#
# See docs/purple/config.md.
set -e

RepoPath="$(cd "$(dirname "$0")/.." && pwd)"
LibrariesPath="${LibrariesPath:-$(dirname "$RepoPath")/tdesktop-libs}"
QtPrefix="${QtPrefix:-$LibrariesPath/local/qt}"
Output="${Output:-${TMPDIR:-/tmp}}/purple_test_config"

if [ ! -d "$QtPrefix/frameworks/QtCore.framework" ]; then
    echo "No Qt at $QtPrefix - run purple/build_app.sh once first." >&2
    exit 1
fi

clang++ -std=c++20 -g -O0 -o "$Output" \
    "$RepoPath/purple/test_config.cpp" \
    "$RepoPath/Telegram/SourceFiles/purple/purple_settings.cpp" \
    "$RepoPath/Telegram/SourceFiles/purple/purple_splice.cpp" \
    -I"$RepoPath/Telegram/SourceFiles" \
    -I"$RepoPath/Telegram/lib_base" \
    -I"$RepoPath/Telegram/ThirdParty/GSL/include" \
    -I"$RepoPath/Telegram/ThirdParty/tomlplusplus" \
    -I"$QtPrefix/frameworks/QtCore.framework/Headers" \
    -F"$QtPrefix/frameworks" \
    -framework QtCore \
    -Wl,-rpath,"$QtPrefix/frameworks"

"$Output"
