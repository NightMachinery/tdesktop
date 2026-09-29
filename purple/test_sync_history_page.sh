#!/usr/bin/env bash
set -e

RepoPath="$(cd "$(dirname "$0")/.." && pwd)"
BuildPath="$(command mktemp -d "${TMPDIR:-/tmp}/purple-sync-history-page-test.XXXXXX")"
trap 'command rm -rf "$BuildPath"' EXIT

clang++ -std=c++20 -Wall -Wextra \
    -I"$RepoPath/Telegram/SourceFiles" \
    "$RepoPath/purple/test_sync_history_page.cpp" \
    -o "$BuildPath/test_sync_history_page"

"$BuildPath/test_sync_history_page"
