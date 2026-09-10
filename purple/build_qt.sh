#!/usr/bin/env bash
# Builds Qt 6.11.1 with upstream Telegram Desktop's patch set, into a prefix
# that build_app.sh and install.sh pick up in place of Homebrew's Qt.
#
# Why: Homebrew's qtbase is the right version and none of the patches. The set
# that matters on macOS is John Preston's rework of the Metal backing store
# (the app composites its whole window through QRhi) and the fix for a
# use-after-free of a CGColorSpace that has crashed this fork twice. See the
# "Qt with upstream's patches" section of docs/mac/build.md.
#
# The shape of the build is Homebrew's, not upstream's: shared frameworks,
# arm64 only, Release, linked against the same Homebrew libraries the Homebrew
# Qt links. Upstream builds a static universal Qt for a self-contained release,
# which the packaged route here cannot use - macdeployqt has to have frameworks
# to bundle. So: Homebrew's configure, upstream's patches.
#
#     purple/build_qt.sh              # a few hours, mostly qtbase
#     QtJobs=8 purple/build_qt.sh     # the default is 6
#
# Re-running is cheap and safe: clones, patches and installed modules are all
# skipped when they are already there. Build trees are deleted after each
# module installs, so a re-run after a failure re-does that module's build.
set -e

RepoPath="$(cd "$(dirname "$0")/.." && pwd)"
LibrariesPath="${LibrariesPath:-$(dirname "$RepoPath")/tdesktop-libs}"

QtVersion="${QtVersion:-6.11.1}"
QtTag="v$QtVersion"

# Both pinned to the commits Telegram/build/prepare/prepare.py pins, not to
# either repository's HEAD. HEAD of desktop-app/patches has moved on to
# qtbase_6.11.2 (renumbered, 46 patches) and qt6_highsierra_patches has been
# rebased onto 6.11.2 as well; neither set applies to 6.11.1.
PatchesCommit="${PatchesCommit:-a17d54b63128c83cb53bd71044119e77b3a2da02}"
HighSierraCommit="${HighSierraCommit:-4aae812a405f47553e001faf566de572d3eccd16}"

QtJobs="${QtJobs:-6}"

SourcePath="${SourcePath:-$LibrariesPath/qt-src}"
BuildRoot="${BuildRoot:-$LibrariesPath/qt-build}"
Prefix="${QtPatchedPrefix:-$LibrariesPath/qt-patched}"

Modules="qtbase qtshadertools qtsvg qtimageformats"
Stamp="$SourcePath/qtbase/.purple-patched"

# Homebrew builds Qt against the macOS 14 SDK's deployment target - later ones
# lose functions qtbase still calls (QTBUG-128900) - and the app links Qt, so
# the two have to agree with what the Homebrew build produced.
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-14.0}"

mkdir -p "$SourcePath" "$BuildRoot"

# ---------------------------------------------------------------- sources

for module in $Modules; do
    if [ -d "$SourcePath/$module" ]; then
        echo "=== $module: already cloned ==="
        continue
    fi
    echo "=== cloning $module $QtTag ==="
    git clone --depth 1 -b "$QtTag" \
        "https://github.com/qt/$module.git" "$SourcePath/$module"
done

clone_patches() {
    name="$1"; url="$2"; commit="$3"
    if [ ! -d "$SourcePath/$name" ]; then
        echo "=== cloning $name ==="
        git clone "$url" "$SourcePath/$name"
    fi
    git -C "$SourcePath/$name" checkout --quiet "$commit"
    echo "$name at $(git -C "$SourcePath/$name" rev-parse HEAD)"
}

clone_patches patches https://github.com/desktop-app/patches.git "$PatchesCommit"
clone_patches qt6_highsierra \
    https://github.com/desktop-app/qt6_highsierra_patches.git "$HighSierraCommit"

# ---------------------------------------------------------------- patches

# prepare.py's order exactly: the High Sierra set first, sorted, then the
# version's own set, sorted, all against qtbase. Applied one at a time rather
# than in one xargs run so that a failure names the patch that failed instead
# of taking the whole batch down with it.
if [ -f "$Stamp" ]; then
    echo "=== patches already applied ($(cat "$Stamp")) ==="
else
    Failed=""
    for patch in $(ls "$SourcePath/qt6_highsierra"/*.patch | sort) \
                 $(ls "$SourcePath/patches/qtbase_$QtVersion"/*.patch | sort); do
        Output="$(git -C "$SourcePath/qtbase" apply -v "$patch" 2>&1)" \
            && Status=0 || Status=$?
        echo "$Output" | sed "s|^|    |"
        if [ "$Status" -eq 0 ]; then
            echo "PATCH ok    $(basename "$patch")"
        else
            echo "PATCH FAIL  $(basename "$patch")"
            Failed="$Failed $(basename "$patch")"
        fi
    done
    if [ -n "$Failed" ]; then
        echo "=== patches that did not apply:$Failed ==="
        echo "Fix or record them, then create $Stamp by hand to continue." >&2
        exit 1
    fi

    # Same list Homebrew's qtbase formula removes, for the same reason: with
    # FEATURE_system_* on, the bundled copies are dead weight, and removing
    # them makes it impossible for the build to quietly fall back to one.
    # After patching, so that a patch touching src/3rdparty still applies.
    (cd "$SourcePath/qtbase" && rm -rf \
        src/3rdparty/double-conversion \
        src/3rdparty/freetype \
        src/3rdparty/harfbuzz-ng \
        src/3rdparty/libjpeg \
        src/3rdparty/libpng \
        src/3rdparty/md4c \
        src/3rdparty/pcre2 \
        src/3rdparty/sqlite \
        src/3rdparty/xcb \
        src/3rdparty/zlib)
    (cd "$SourcePath/qtimageformats" && rm -rf src/3rdparty)

    echo "patches $PatchesCommit / highsierra $HighSierraCommit" > "$Stamp"
    echo "=== all patches applied ==="
fi

# ------------------------------------------------------------- toolchain

# Every Homebrew dependency of qtbase, including the keg-only ones, so the
# patched Qt links the same libraries the Homebrew Qt does. If it does not,
# macdeployqt and relink_bundle.py end up bundling a different set and the
# whole point of matching Homebrew's shape is lost.
Deps="brotli dbus double-conversion freetype glib harfbuzz icu4c@78 jpeg-turbo
      libb2 libpng md4c openssl@3 pcre2 zstd
      jasper libtiff webp libmng"

DepPrefix=""
DepPkgConfig=""
for dep in $Deps; do
    dir="/opt/homebrew/opt/$dep"
    [ -d "$dir" ] || continue
    DepPrefix="$DepPrefix;$dir"
    [ -d "$dir/lib/pkgconfig" ] && DepPkgConfig="$DepPkgConfig:$dir/lib/pkgconfig"
done
export PKG_CONFIG_PATH="/opt/homebrew/lib/pkgconfig${DepPkgConfig}${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# Homebrew's own arguments, minus the parts that only make sense inside a keg
# (CMAKE_STAGING_PREFIX and the QtQmakeHelpers relocatability hack, both there
# because Homebrew installs to a keg and symlinks it into /opt/homebrew; this
# installs into one real prefix, where the stock relocatable logic is right).
#
# The one deliberate addition is FEATURE_vulkan=OFF. Homebrew's Qt records the
# Vulkan headers as living in /opt/homebrew/include, which drags that directory
# into the include path of every Qt-using target and shadows Qt's own headers
# with the full "qt" formula's older ones - merge_qt_prefix.py has to edit it
# back out afterwards. Telegram Desktop renders through Metal and OpenGL here
# and never asks for Vulkan, so not building it is simpler than removing it.
CommonArgs="
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=$Prefix
    -DCMAKE_PREFIX_PATH=$Prefix;/opt/homebrew$DepPrefix
    -DCMAKE_FIND_FRAMEWORK=FIRST
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET
    -DCMAKE_OSX_ARCHITECTURES=arm64
    -DQT_NO_APPLE_SDK_AND_XCODE_CHECK=ON
    -DQT_BUILD_TESTS=OFF
    -DQT_BUILD_EXAMPLES=OFF
    -DBUILD_TESTING=OFF
    -DINSTALL_ARCHDATADIR=share/qt
    -DINSTALL_DATADIR=share/qt
    -DINSTALL_EXAMPLESDIR=share/qt/examples
    -DINSTALL_MKSPECSDIR=share/qt/mkspecs
    -DINSTALL_TESTSDIR=share/qt/tests
    -Wno-dev"

build_module() {
    module="$1"; shift
    echo "=== configuring $module ==="
    date
    rm -rf "$BuildRoot/$module"
    nice -n 15 cmake -S "$SourcePath/$module" -B "$BuildRoot/$module" -G Ninja \
        $CommonArgs "$@"
    echo "=== building $module ==="
    date
    nice -n 15 cmake --build "$BuildRoot/$module" --parallel "$QtJobs"
    echo "=== installing $module ==="
    nice -n 15 cmake --install "$BuildRoot/$module"
    date
    rm -rf "$BuildRoot/$module"
    echo "=== $module done, build tree removed ==="
    df -h /
}

# ---------------------------------------------------------------- qtbase

if [ -f "$Prefix/lib/cmake/Qt6/Qt6Config.cmake" ]; then
    echo "=== qtbase already installed in $Prefix ==="
else
    build_module qtbase \
        -DFEATURE_sql_mysql=OFF \
        -DFEATURE_sql_odbc=OFF \
        -DFEATURE_sql_psql=OFF \
        -DFEATURE_openssl_linked=ON \
        -DFEATURE_pkg_config=ON \
        -DFEATURE_system_doubleconversion=ON \
        -DFEATURE_system_freetype=ON \
        -DFEATURE_system_harfbuzz=ON \
        -DFEATURE_system_jpeg=ON \
        -DFEATURE_system_libb2=ON \
        -DFEATURE_system_pcre2=ON \
        -DFEATURE_system_png=ON \
        -DFEATURE_system_sqlite=ON \
        -DFEATURE_system_zlib=ON \
        -DFEATURE_vulkan=OFF \
        -DFEATURE_relocatable=ON \
        -DQT_ALLOW_SYMLINK_IN_PATHS=ON \
        -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
fi

# ------------------------------------------------------- the other modules
#
# Plain cmake against the prefix, which is what Homebrew's module formulae do;
# qt-configure-module is a wrapper around the same thing that insists on being
# run out of an installed Qt's bin directory.

if [ -f "$Prefix/lib/cmake/Qt6ShaderTools/Qt6ShaderToolsConfig.cmake" ]; then
    echo "=== qtshadertools already installed ==="
else
    build_module qtshadertools
fi

if [ -f "$Prefix/lib/cmake/Qt6Svg/Qt6SvgConfig.cmake" ]; then
    echo "=== qtsvg already installed ==="
else
    build_module qtsvg
fi

if [ -d "$Prefix/share/qt/plugins/imageformats" ] \
        && ls "$Prefix/share/qt/plugins/imageformats"/libqwebp* >/dev/null 2>&1; then
    echo "=== qtimageformats already installed ==="
else
    build_module qtimageformats \
        -DFEATURE_system_tiff=ON \
        -DFEATURE_system_webp=ON
fi

# ------------------------------------------------------- framework search dir

# A second, distinct real directory of symlinks to the same frameworks. Qt's
# own lib/ reaches the compiler as "-iframework", which clang searches after
# every "-isystem" directory - including /opt/homebrew/include, which on a
# machine with the full "qt" formula shadows Qt's headers with 6.9.2's. "-F"
# is searched earlier and wins, but only if it names a directory clang has not
# already seen: it resolves search paths to real directories and drops
# duplicates. Same reasoning as make_framework_search_dir() in
# merge_qt_prefix.py, which does this for the Homebrew route.
mkdir -p "$Prefix/frameworks"
count=0
for framework in "$Prefix/lib"/*.framework; do
    [ -d "$framework" ] || continue
    ln -sfn "$framework" "$Prefix/frameworks/$(basename "$framework")"
    count=$((count + 1))
done
echo "=== framework search dir: $count frameworks in $Prefix/frameworks ==="

echo "=== Qt $QtVersion with upstream's patches is in $Prefix ==="
du -sh "$Prefix" "$SourcePath"
df -h /
