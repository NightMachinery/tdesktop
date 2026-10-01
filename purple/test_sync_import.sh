#!/usr/bin/env bash
set -e

RepoPath="$(cd "$(dirname "$0")/.." && pwd)"
CorePath="$RepoPath/Telegram/ThirdParty/purple_core"
QtPrefix="${QtPrefix:-$(dirname "$RepoPath")/tdesktop-libs/local/qt}"
BuildPath="$(command mktemp -d "${TMPDIR:-/tmp}/purple-sync-import-test.XXXXXX")"
trap 'command rm -rf "$BuildPath"' EXIT

clang++ -std=c++20 -g -O0 -o "$BuildPath/test_sync_import" \
    "$RepoPath/purple/test_sync_import.cpp" \
    -I"$RepoPath/Telegram/SourceFiles" \
    -I"$CorePath" \
    -I"$CorePath/tomlplusplus" \
    -I"$QtPrefix/frameworks/QtCore.framework/Headers" \
    -F"$QtPrefix/frameworks" \
    -framework QtCore \
    -Wl,-rpath,"$QtPrefix/frameworks"

"$BuildPath/test_sync_import"
