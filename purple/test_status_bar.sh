#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:?Pass a scratch render directory}"
qt="${QtPrefix:-/opt/homebrew/opt/qt}"
frameworks="${qt}/frameworks"
if [[ ! -d "${frameworks}/QtGui.framework" ]]; then
  frameworks="${qt}/lib"
fi
work="$(mktemp -d "${TMPDIR:-/tmp}/purple-status-bar.XXXXXX")"
trap 'rm -rf "${work}"' EXIT

clang++ -std=c++20 -Wall -Wextra -Werror -I"${root}/Telegram/SourceFiles" \
  -F"${frameworks}" -framework QtCore -framework QtGui "-Wl,-rpath,${frameworks}" \
  "${root}/purple/test_status_bar.cpp" -o "${work}/render"
env QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORM_PLUGIN_PATH="${qt}/share/qt/plugins/platforms" \
  "${work}/render" "${out}" "${root}/Telegram/lib_ui/fonts"
