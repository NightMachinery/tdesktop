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

# Qt must come first, and must be a prefix of our own: a machine with the full
# "qt" formula installed has it linked into /opt/homebrew, where CMake would
# otherwise find it and compile against the wrong Qt version.
#
# There are two such prefixes. purple/build_qt.sh builds Qt 6.11.1 with
# upstream's patch set into "qt-patched"; when that exists it wins, because it
# is the same version with the macOS rendering fixes the stock build lacks.
# Otherwise merge_qt_prefix.py assembles Homebrew's four unlinked Qt kegs into
# one prefix, which is the original route and still works.
#
# Set QtPrefix in the environment to force either one. install.sh takes
# macdeployqt from the same prefix by the same rule - change one, change both.
QtMergedPrefix="$LibrariesPath/local/qt"
QtPatchedPrefix="$LibrariesPath/qt-patched"
if [ -z "$QtPrefix" ]; then
    if [ -d "$QtPatchedPrefix" ]; then
        QtPrefix="$QtPatchedPrefix"
    else
        QtPrefix="$QtMergedPrefix"
    fi
fi
if [ "$QtPrefix" = "$QtMergedPrefix" ] && [ ! -d "$QtPrefix" ]; then
    python3 "$(dirname "$0")/merge_qt_prefix.py" "$QtPrefix"
fi
if [ ! -d "$QtPrefix" ]; then
    echo "No Qt prefix at $QtPrefix - run purple/build_qt.sh, or unset QtPrefix." >&2
    exit 1
fi
echo "=== Qt prefix: $QtPrefix ==="
export CMAKE_PREFIX_PATH="$QtPrefix:/opt/homebrew/opt/ffmpeg@6:/opt/homebrew/opt/openal-soft:/opt/homebrew/opt/openssl@3$(find "$LibrariesPath/local" -mindepth 1 -maxdepth 1 -type d -exec printf ':%s' {} +)"

# Pins every translation unit to the Qt we configured against; see
# make_framework_search_dir() in merge_qt_prefix.py for why this is needed and
# why it has to be the "frameworks" directory rather than "lib".
QtFrameworkFlag="-F$QtPrefix/frameworks"

# Same problem as Qt, and it corrupts memory rather than failing to build. We
# link ffmpeg@6, but a machine with the full "ffmpeg" formula has its headers
# linked into /opt/homebrew/include, which reaches the compiler as "-isystem"
# ahead of ffmpeg@6's own "-isystem". Every FFmpeg struct then gets the wrong
# layout: offsetof(AVCodecParameters, coded_side_data) is 32 in ffmpeg 8 and
# 176 in ffmpeg 6, so the app reads pointers out of the wrong fields and
# crashes inside libavcodec.
#
# "-I" wins because clang searches the whole angled list before any "-isystem"
# directory, whatever the order on the command line. It has to be a directory of
# its own, though: clang resolves search paths to real directories and drops
# duplicates, so pointing "-I" at ffmpeg@6's own include dir - already on the
# command line as "-isystem" - is silently a no-op. Same trap as Qt, see
# make_framework_search_dir() in merge_qt_prefix.py.
FFmpegInclude="$LibrariesPath/local/ffmpeg6-include"
if [ ! -d "$FFmpegInclude" ]; then
    mkdir -p "$FFmpegInclude"
    for dir in /opt/homebrew/opt/ffmpeg@6/include/*/; do
        ln -sfn "${dir%/}" "$FFmpegInclude/$(basename "$dir")"
    done
fi
CompilerFlags="$QtFrameworkFlag -I$FFmpegInclude"

# Debug info: line tables, not full DWARF. "-O2 -g" put 122MB of DWARF into a
# single object file - 90% of history_widget.cpp.o, and 17GB across out/ - and
# none of it is reachable by anything this fork does. Symbolication goes
# through atos and the debug map, which resolves function names and file:line
# from the line tables alone; nothing here runs a debugger, so the variable
# and type information is the whole cost and none of the benefit. See
# docs/mac/build.md.
#
# This has to override the per-config flags rather than join CompilerFlags:
# CMake emits CMAKE_<LANG>_FLAGS first and CMAKE_<LANG>_FLAGS_<CONFIG> after
# it, so RelWithDebInfo's own "-g" would win over anything appended above.
RelWithDebInfoFlags="${RelWithDebInfoFlags:--O2 -gline-tables-only -DNDEBUG}"

cd "$RepoPath"
cmake -B "$BuildPath" -G Ninja . \
    -D CMAKE_BUILD_TYPE="$BuildType" \
    -D DESKTOP_APP_DISABLE_SWIFT6=ON \
    -D TDESKTOP_VENDORED_FIDO2=ON \
    -D CMAKE_C_FLAGS="$CompilerFlags" \
    -D CMAKE_CXX_FLAGS="$CompilerFlags" \
    -D CMAKE_OBJC_FLAGS="$CompilerFlags" \
    -D CMAKE_OBJCXX_FLAGS="$CompilerFlags" \
    -D CMAKE_C_FLAGS_RELWITHDEBINFO="$RelWithDebInfoFlags" \
    -D CMAKE_CXX_FLAGS_RELWITHDEBINFO="$RelWithDebInfoFlags" \
    -D CMAKE_OBJC_FLAGS_RELWITHDEBINFO="$RelWithDebInfoFlags" \
    -D CMAKE_OBJCXX_FLAGS_RELWITHDEBINFO="$RelWithDebInfoFlags" \
    -D TDESKTOP_API_ID="$TDESKTOP_API_ID" \
    -D TDESKTOP_API_HASH="$TDESKTOP_API_HASH"

cmake --build "$BuildPath" --parallel ${BuildJobs:+"$BuildJobs"}

echo "=== built ==="
ls -d "$BuildPath"/*.app
