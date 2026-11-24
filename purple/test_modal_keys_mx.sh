#!/usr/bin/env bash
set -e

RepoPath="$(cd "$(dirname "$0")/.." && pwd)"
LibrariesPath="${LibrariesPath:-$(dirname "$RepoPath")/tdesktop-libs}"
if [ -z "$QtPrefix" ]; then
    if [ -d "$LibrariesPath/qt-patched" ]; then
        QtPrefix="$LibrariesPath/qt-patched"
    else
        QtPrefix="$LibrariesPath/local/qt"
    fi
fi

Work="$(command mktemp -d "${TMPDIR:-/tmp}/purple-mx-test.XXXXXX")"
trap 'command rm -rf "$Work"' EXIT

QtHeaders="$QtPrefix/lib/QtCore.framework/Headers"
if [ ! -d "$QtHeaders" ]; then
    QtHeaders="$QtPrefix/frameworks/QtCore.framework/Headers"
fi
QtFw="$QtPrefix/lib"
if [ ! -d "$QtFw/QtCore.framework" ]; then
    QtFw="$QtPrefix/frameworks"
fi

clang++ -std=c++20 -g -O0 -o "$Work/test_modal_keys_mx" \
    "$RepoPath/purple/test_modal_keys_mx.cpp" \
    -I"$RepoPath/Telegram/SourceFiles" \
    -I"$QtHeaders" \
    -I"$QtPrefix/include" \
    -F"$QtFw" \
    -framework QtCore \
    -Wl,-rpath,"$QtFw"
"$Work/test_modal_keys_mx"
