#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
qt="${QtPrefix:-/opt/homebrew/opt/qt}"
frameworks="${qt}/frameworks"
if [[ ! -d "${frameworks}/QtGui.framework" ]]; then
  frameworks="${qt}/lib"
fi
work="$(mktemp -d "${TMPDIR:-/tmp}/purple-window-hints.XXXXXX")"
trap 'rm -rf "${work}"' EXIT
flags=(-std=c++20 -Wall -Wextra -Werror -I"${root}/Telegram/SourceFiles")
for test in test_hint_labels test_hint_mnemonics; do
  "${CXX:-c++}" "${flags[@]}" "${root}/purple/${test}.cpp" -o "${work}/${test}"
  "${work}/${test}"
done
"${CXX:-c++}" "${flags[@]}" -F"${frameworks}" -framework QtCore "-Wl,-rpath,${frameworks}" \
  "${root}/purple/test_message_hint_placement.cpp" -o "${work}/placement"
"${work}/placement"
"${CXX:-c++}" "${flags[@]}" -F"${frameworks}" -framework QtWidgets -framework QtGui -framework QtCore \
  "-Wl,-rpath,${frameworks}" "${root}/purple/test_hint_item_identity.cpp" -o "${work}/item-identity"
env QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORM_PLUGIN_PATH="${qt}/share/qt/plugins/platforms" \
  "${work}/item-identity"

if [[ $# -gt 0 ]]; then
  "${CXX:-c++}" "${flags[@]}" -F"${frameworks}" -framework QtGui -framework QtCore "-Wl,-rpath,${frameworks}" \
    "${root}/purple/test_window_hint_renders.cpp" -o "${work}/renders"
  env QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORM_PLUGIN_PATH="${qt}/share/qt/plugins/platforms" \
    "${work}/renders" "$1"
fi
