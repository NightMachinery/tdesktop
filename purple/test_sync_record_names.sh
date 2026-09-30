#!/usr/bin/env bash
set -e

RepoPath="$(cd "$(dirname "$0")/.." && pwd)"
CorePath="$RepoPath/Telegram/ThirdParty/purple_core"
QtPrefix="${QtPrefix:-$(dirname "$RepoPath")/tdesktop-libs/local/qt}"
BuildPath="$(command mktemp -d "${TMPDIR:-/tmp}/purple-sync-record-names-test.XXXXXX")"
trap 'command rm -rf "$BuildPath"' EXIT

clang++ -std=c++20 -g -O0 -o "$BuildPath/test_sync_record_names" \
    "$RepoPath/purple/test_sync_record_names.cpp" \
    "$CorePath/purple/purple_settings.cpp" \
    "$CorePath/purple/purple_splice.cpp" \
    "$CorePath/purple/purple_state.cpp" \
    "$CorePath/purple/purple_config_sync.cpp" \
    "$CorePath/purple/purple_config_payload.cpp" \
    "$CorePath/purple/purple_sync_json.cpp" \
    "$CorePath/purple/purple_sync_envelope.cpp" \
    "$CorePath/purple/purple_sync_directory.cpp" \
    "$CorePath/purple/purple_sync_inventory.cpp" \
    "$CorePath/purple/purple_sync_config_flow.cpp" \
    "$CorePath/purple/purple_sync_local_state.cpp" \
    -I"$RepoPath/Telegram/SourceFiles" \
    -I"$CorePath" \
    -I"$CorePath/tomlplusplus" \
    -I"$QtPrefix/frameworks/QtCore.framework/Headers" \
    -F"$QtPrefix/frameworks" \
    -framework QtCore \
    -Wl,-rpath,"$QtPrefix/frameworks"

"$BuildPath/test_sync_record_names"
