#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:?Pass a scratch render directory}"
qt="${QtPrefix:-/opt/homebrew/opt/qt}"
frameworks="${qt}/frameworks"
if [[ ! -d "${frameworks}/QtWidgets.framework" ]]; then
  frameworks="${qt}/lib"
fi
work="$(mktemp -d "${TMPDIR:-/tmp}/purple-offscreen.XXXXXX")"
trap 'rm -rf "${work}"' EXIT
mkdir -p "${work}/config" "${work}/cache"

clang++ -std=c++20 -Wall -Wextra -Werror -I"${root}/Telegram/SourceFiles" \
  -I"${root}/Telegram/ThirdParty/purple_core" -F"${frameworks}" \
  -framework QtCore -framework QtGui -framework QtWidgets "-Wl,-rpath,${frameworks}" \
  "${root}/purple/test_customize_render.cpp" -o "${work}/render"
env QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORM_PLUGIN_PATH="${qt}/share/qt/plugins/platforms" \
  XDG_CONFIG_HOME="${work}/config" XDG_CACHE_HOME="${work}/cache" "${work}/render" "${out}"
