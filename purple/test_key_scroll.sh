#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/purple-key-scroll.XXXXXX")"
trap 'rm -rf "${work}"' EXIT
clang++ -std=c++20 -Wall -Wextra -Werror -I"${root}/Telegram/SourceFiles" \
  "${root}/purple/test_key_scroll.cpp" -o "${work}/scroll"
"${work}/scroll"
