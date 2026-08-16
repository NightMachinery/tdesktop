#!/usr/bin/env bash
# Builds the three dependencies that Homebrew does not package, for the
# packaged macOS build described in docs/mac/build.md.
#
# Mirrors the steps in .github/workflows/mac_packaged.yml. The install prefix
# must NOT be a directory named "Libraries" next to the repository, or
# cmake/validate_special_target.cmake switches DESKTOP_APP_USE_PACKAGED off.
set -e

RepoPath="$(cd "$(dirname "$0")/.." && pwd)"
LibrariesPath="${LibrariesPath:-$(dirname "$RepoPath")/tdesktop-libs}"
TDE2E="${TDE2E:-51743dfd01dff6179e2d8f7095729caa4e2222e9}"
GIT="${GIT:-https://github.com}"

export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-13}"
export CMAKE_PREFIX_PATH="/opt/homebrew/opt/ffmpeg@6:/opt/homebrew/opt/openal-soft:/opt/homebrew/opt/openssl@3${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"

mkdir -p "$LibrariesPath"
cd "$LibrariesPath"
echo "Building into $LibrariesPath/local (deployment target $MACOSX_DEPLOYMENT_TARGET)"

if [ ! -d "$LibrariesPath/local/rnnoise" ]; then
    echo "=== rnnoise ==="
    rm -rf rnnoise
    git clone --depth=1 $GIT/xiph/rnnoise.git
    cd rnnoise
    ./autogen.sh
    ./configure --prefix=$LibrariesPath/local/rnnoise --disable-examples --disable-doc
    make -j$(sysctl -n hw.logicalcpu)
    make install
    cd "$LibrariesPath"
fi

if [ ! -d "$LibrariesPath/local/tg_owt" ]; then
    echo "=== tg_owt ==="
    rm -rf tg_owt
    git clone --depth=1 --recursive --shallow-submodules $GIT/desktop-app/tg_owt.git
    cd tg_owt
    cmake -Bbuild . -G Ninja -DCMAKE_INSTALL_PREFIX=$LibrariesPath/local/tg_owt
    cmake --build build
    cmake --install build
    cd "$LibrariesPath"
fi

if [ ! -d "$LibrariesPath/local/tde2e" ]; then
    echo "=== tde2e ==="
    rm -rf tde2e
    git init tde2e
    cd tde2e
    git remote add origin $GIT/tdlib/td.git
    git fetch --depth=1 origin $TDE2E
    git reset --hard FETCH_HEAD
    cmake -Bbuild . -G Ninja -DCMAKE_INSTALL_PREFIX=$LibrariesPath/local/tde2e -DTD_E2E_ONLY=ON
    cmake --build build
    cmake --install build
    cd "$LibrariesPath"
fi

echo "=== done ==="
du -sh "$LibrariesPath"/local/* 2>/dev/null || true
df -h / | tail -1
