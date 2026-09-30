#!/usr/bin/env bash
set -e

RepoPath="$(cd "$(dirname "$0")/.." && pwd)"
CorePath="$RepoPath/Telegram/ThirdParty/purple_core"
QtPrefix="${QtPrefix:-$(dirname "$RepoPath")/tdesktop-libs/local/qt}"
BuildPath="$(command mktemp -d "${TMPDIR:-/tmp}/purple-sync-history-page-test.XXXXXX")"
trap 'command rm -rf "$BuildPath"' EXIT

clang++ -std=c++20 -Wall -Wextra \
    -I"$CorePath" \
    -I"$QtPrefix/frameworks/QtCore.framework/Headers" \
    -F"$QtPrefix/frameworks" \
    -framework QtCore \
    -Wl,-rpath,"$QtPrefix/frameworks" \
    "$RepoPath/purple/test_sync_history_page.cpp" \
    -o "$BuildPath/test_sync_history_page"

"$BuildPath/test_sync_history_page"
