#!/usr/bin/env bash
set -euo pipefail

Root="$(cd "$(dirname "$0")/.." && pwd)"
QtPrefix="${QtPrefix:-/opt/homebrew/opt/qt}"
QtInclude="${QtPrefix}/include"
QtLib="${QtPrefix}/lib"

if [[ ! -d "${QtInclude}/QtCore" && -d "${QtLib}/QtCore.framework/Headers" ]]; then
  QtFlags=(
    -F "${QtLib}"
    -I "${QtLib}/QtCore.framework/Headers"
    -framework QtCore
    -Wl,-rpath,"${QtLib}"
  )
  KeyQtFlags=(-framework QtGui -framework QtWidgets)
else
  QtFlags=(
    -I "${QtInclude}"
    -I "${QtInclude}/QtCore"
    -L "${QtLib}"
    -lQt6Core
    -Wl,-rpath,"${QtLib}"
  )
  KeyQtFlags=(-I "${QtInclude}/QtGui" -I "${QtInclude}/QtWidgets" -lQt6Gui -lQt6Widgets)
fi

Bin="$(mktemp "${TMPDIR:-/tmp}/test_modal_keys_unread.XXXXXX")"
trap 'rm -f "${Bin}"' EXIT

clang++ -std=c++20 -Wall -Wextra -Werror \
  -I "${Root}/Telegram/SourceFiles" \
  "${Root}/purple/test_modal_keys_unread.cpp" \
  "${QtFlags[@]}" \
  -o "${Bin}"

"${Bin}"

clang++ -std=c++20 -Wall -Wextra -Werror \
  -I "${Root}/Telegram/SourceFiles" \
  "${Root}/purple/test_list_filter_keys.cpp" \
  "${QtFlags[@]}" "${KeyQtFlags[@]}" \
  -o "${Bin}"

"${Bin}"
