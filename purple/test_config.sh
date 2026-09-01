#!/usr/bin/env bash
# Compiles and runs the standalone tests for the Purple config core.
#
# The core itself now lives in the purple-core submodule under
# Telegram/ThirdParty/purple_core, together with its own test suite, so that the
# Android app can compile the same sources. This stays as the desktop-side entry
# point: it works out where the fork's Qt is and hands over to the suite there.
#
# See docs/purple/config.md.
set -e

RepoPath="$(cd "$(dirname "$0")/.." && pwd)"
LibrariesPath="${LibrariesPath:-$(dirname "$RepoPath")/tdesktop-libs}"
QtPrefix="${QtPrefix:-$LibrariesPath/local/qt}"

if [ ! -d "$QtPrefix/frameworks/QtCore.framework" ]; then
    echo "No Qt at $QtPrefix - run purple/build_app.sh once first." >&2
    exit 1
fi

exec env QT_PREFIX="$QtPrefix" \
    "$RepoPath/Telegram/ThirdParty/purple_core/tests/run.sh" "$@"
