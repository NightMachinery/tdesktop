#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
qt="${QtPrefix:-/opt/homebrew/opt/qt}/frameworks"
if [[ ! -d "${qt}/QtGui.framework" ]]; then
  qt="${QtPrefix:-/opt/homebrew/opt/qt}/lib"
fi
bin="$(mktemp "${TMPDIR:-/tmp}/purple-modal-colors.XXXXXX")"
trap 'rm -f "${bin}"' EXIT

clang++ -std=c++20 -Wall -Wextra -Werror -I"${root}/Telegram/SourceFiles" \
  -F"${qt}" -framework QtCore -framework QtGui -Wl,-rpath,"${qt}" \
  "${root}/purple/test_modal_keys_colors.cpp" -o "${bin}"
"${bin}"
