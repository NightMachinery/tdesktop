#!/usr/bin/env bash
set -euo pipefail

Root="$(cd "$(dirname "$0")/.." && pwd)"
QtPrefix="${QtPrefix:-/opt/homebrew/opt/qt}"
QtInclude="${QtPrefix}/include"
QtLib="${QtPrefix}/lib"
if [[ -d "${QtPrefix}/frameworks/QtCore.framework" ]]; then QtLib="${QtPrefix}/frameworks"; fi

if [[ ! -d "${QtInclude}/QtCore" && -d "${QtLib}/QtCore.framework/Headers" ]]; then
  QtFlags=(
    -F "${QtLib}"
    -I "${QtLib}/QtCore.framework/Headers"
    -framework QtCore
    -Wl,-rpath,"${QtLib}"
  )
  QtWidgetsFlags=(-framework QtWidgets -framework QtGui)
else
  QtFlags=(
    -I "${QtInclude}"
    -I "${QtInclude}/QtCore"
    -L "${QtLib}"
    -lQt6Core
    -Wl,-rpath,"${QtLib}"
  )
  QtWidgetsFlags=(-I "${QtInclude}/QtWidgets" -I "${QtInclude}/QtGui" -lQt6Widgets -lQt6Gui)
fi

Bin="$(mktemp "${TMPDIR:-/tmp}/test_privacy_screen.XXXXXX")"
trap 'rm -f "${Bin}"' EXIT

if [[ "${1:-}" == "--render" ]]; then
  clang++ -std=c++20 -Wall -Wextra -Werror \
    -I "${Root}/Telegram/SourceFiles" \
    -I "${Root}/Telegram/ThirdParty/purple_core" \
    "${Root}/purple/test_privacy_canvas.cpp" \
    "${Root}/Telegram/SourceFiles/purple/purple_privacy_canvas.cpp" \
    "${QtFlags[@]}" "${QtWidgetsFlags[@]}" -o "${Bin}"
  QT_QPA_PLATFORM=offscreen "${Bin}" "${2:?Pass an offscreen render directory}"
  exit
fi

clang++ -std=c++20 -Wall -Wextra -Werror \
  -I "${Root}/Telegram/SourceFiles" \
  -I "${Root}/Telegram/ThirdParty/purple_core" \
  "${Root}/purple/test_privacy_screen.cpp" \
  "${QtFlags[@]}" \
  -o "${Bin}"

"${Bin}"
