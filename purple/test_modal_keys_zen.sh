#!/usr/bin/env bash
set -euo pipefail

RepoPath="$(cd "$(dirname "$0")/.." && pwd)"
LibrariesPath="${LibrariesPath:-$(dirname "$RepoPath")/tdesktop-libs}"
QtPrefix="${QtPrefix:-$LibrariesPath/qt-patched}"
if [ ! -d "$QtPrefix/frameworks/QtCore.framework" ]; then
    QtPrefix="$LibrariesPath/local/qt"
fi

TmpBin="$(mktemp -t purple_keys_zen_test)"
trap 'rm -f "$TmpBin"' EXIT

clang++ -std=c++20 -O2 \
    -F"$QtPrefix/frameworks" \
    -I"$RepoPath/Telegram/SourceFiles" \
    -I"$QtPrefix/frameworks/QtCore.framework/Headers" \
    "$RepoPath/purple/test_modal_keys_zen.cpp" \
    -framework QtCore \
    -Wl,-rpath,"$QtPrefix/frameworks" \
    -o "$TmpBin"

"$TmpBin"
