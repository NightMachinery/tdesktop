#!/usr/bin/env bash
# Configures and builds Purple Telegram against Homebrew-packaged libraries.
# See docs/mac/build.md.
#
# Credentials are read from the environment so they never enter the repository:
#
#     export TDESKTOP_API_ID=... TDESKTOP_API_HASH=...
#     purple/build_app.sh
#
# or put those two exports in a file outside the checkout and source it first.
set -e

RepoPath="$(cd "$(dirname "$0")/.." && pwd)"
LibrariesPath="${LibrariesPath:-$(dirname "$RepoPath")/tdesktop-libs}"
BuildType="${BuildType:-RelWithDebInfo}"
BuildPath="${BuildPath:-$RepoPath/out}"

if [ -z "$TDESKTOP_API_ID" ] || [ -z "$TDESKTOP_API_HASH" ]; then
    echo "Set TDESKTOP_API_ID and TDESKTOP_API_HASH first (see docs/mac/build.md)." >&2
    exit 1
fi

export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-13}"

# Qt must come first, and must be the merged prefix: a machine with the full
# "qt" formula installed has it linked into /opt/homebrew, where CMake would
# otherwise find it and compile against the wrong Qt version.
QtPrefix="$LibrariesPath/local/qt"
if [ ! -d "$QtPrefix" ]; then
    python3 "$(dirname "$0")/merge_qt_prefix.py" "$QtPrefix"
fi
export CMAKE_PREFIX_PATH="$QtPrefix:/opt/homebrew/opt/ffmpeg@6:/opt/homebrew/opt/openal-soft:/opt/homebrew/opt/openssl@3$(find "$LibrariesPath/local" -mindepth 1 -maxdepth 1 -type d -exec printf ':%s' {} +)"

# Pins every translation unit to the Qt we configured against; see
# make_framework_search_dir() in merge_qt_prefix.py for why this is needed and
# why it has to be the "frameworks" directory rather than "lib".
QtFrameworkFlag="-F$QtPrefix/frameworks"

cd "$RepoPath"
cmake -B "$BuildPath" -G Ninja . \
    -D CMAKE_BUILD_TYPE="$BuildType" \
    -D DESKTOP_APP_DISABLE_SWIFT6=ON \
    -D CMAKE_C_FLAGS="$QtFrameworkFlag" \
    -D CMAKE_CXX_FLAGS="$QtFrameworkFlag" \
    -D CMAKE_OBJC_FLAGS="$QtFrameworkFlag" \
    -D CMAKE_OBJCXX_FLAGS="$QtFrameworkFlag" \
    -D TDESKTOP_API_ID="$TDESKTOP_API_ID" \
    -D TDESKTOP_API_HASH="$TDESKTOP_API_HASH"

cmake --build "$BuildPath" --parallel ${BuildJobs:+"$BuildJobs"}

echo "=== built ==="
ls -d "$BuildPath"/*.app
