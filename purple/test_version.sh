#!/usr/bin/env bash
set -e

RepoPath="$(cd "$(dirname "$0")/.." && pwd)"
CorePath="$RepoPath/Telegram/ThirdParty/purple_core"
QtPrefix="${QtPrefix:-$(dirname "$RepoPath")/tdesktop-libs/local/qt}"
Script="$RepoPath/Telegram/cmake/purple_build_info.cmake"
Work="$(command mktemp -d "${TMPDIR:-/tmp}/purple-version-test.XXXXXX")"
trap 'command rm -rf "$Work"' EXIT

clang++ -std=c++20 -g -O0 -o "$Work/test_version" \
    "$RepoPath/purple/test_version.cpp" \
    -I"$RepoPath/Telegram/SourceFiles" \
    -I"$CorePath" \
    -I"$QtPrefix/frameworks/QtCore.framework/Headers" \
    -F"$QtPrefix/frameworks" \
    -framework QtCore \
    -Wl,-rpath,"$QtPrefix/frameworks"
"$Work/test_version"

Git="$(type -P git)"
git_() {
    command git \
        -c user.name=Purple \
        -c user.email=purple@example.invalid \
        -c commit.gpgsign=false \
        -c core.hooksPath=/dev/null \
        -c init.defaultBranch=main \
        -c protocol.file.allow=always \
        "$@"
}

Core="$Work/core-origin"
Repo="$Work/desktop"
Output="$Work/gen/purple/purple_build_info.h"
command mkdir -p "$Core" "$Repo"
git_ -C "$Core" init -q
echo a > "$Core/a.txt"
git_ -C "$Core" add a.txt
git_ -C "$Core" commit -qm core
git_ -C "$Repo" init -q
echo x > "$Repo/tracked.txt"
git_ -C "$Repo" add tracked.txt
git_ -C "$Repo" commit -qm one
git_ -C "$Repo" submodule add -q "$Core" core > /dev/null 2>&1
git_ -C "$Repo" commit -qm submodule

Checks=0
Failures=0

fail() {
    Failures=$((Failures + 1))
    echo "FAIL: $1"
}

run() {
    cmake \
        -DPURPLE_GIT="${1-$Git}" \
        -DPURPLE_REPO="$Repo" \
        -DPURPLE_CORE="${2-$Repo/core}" \
        -DPURPLE_OUTPUT="$Output" \
        -P "$Script"
}

expect() {
    local what="$1" desktop="$2" dirty="$3" core="$4" coreDirty="$5"
    local line
    for line in \
        "inline constexpr auto kDesktopCommit = \"$desktop\";" \
        "inline constexpr auto kDesktopDirty = $dirty;" \
        "inline constexpr auto kCoreCommit = \"$core\";" \
        "inline constexpr auto kCoreDirty = $coreDirty;"; do
        Checks=$((Checks + 1))
        if ! command grep -qxF -- "$line" "$Output"; then
            fail "$what: no line '$line'"
        fi
    done
}

mtime() {
    command stat -f %m "$Output"
}

age() {
    command touch -t 200001010000 "$Output"
}

expect_untouched() {
    Checks=$((Checks + 1))
    if [ "$(mtime)" != "$1" ]; then
        fail "$2: rewritten although nothing changed"
    fi
}

expect_rewritten() {
    Checks=$((Checks + 1))
    if [ "$(mtime)" = "$1" ]; then
        fail "$2: not rewritten although it changed"
    fi
}

head_of() {
    git_ -C "$1" rev-parse --short HEAD
}

run
expect "clean checkout" \
    "$(head_of "$Repo")" false "$(head_of "$Repo/core")" false

age
old="$(mtime)"
run
expect_untouched "$old" "second run"

echo u > "$Repo/untracked.txt"
run
expect "untracked file" \
    "$(head_of "$Repo")" false "$(head_of "$Repo/core")" false
expect_untouched "$old" "untracked file"

echo u > "$Repo/core/untracked.txt"
run
expect "untracked file in the submodule" \
    "$(head_of "$Repo")" false "$(head_of "$Repo/core")" false
expect_untouched "$old" "untracked file in the submodule"

echo b >> "$Repo/core/a.txt"
run
expect "dirty submodule" \
    "$(head_of "$Repo")" false "$(head_of "$Repo/core")" true
expect_rewritten "$old" "dirty submodule"

age
old="$(mtime)"
git_ -C "$Repo/core" commit -qam more
run
expect "moved submodule" \
    "$(head_of "$Repo")" false "$(head_of "$Repo/core")" false
expect_rewritten "$old" "moved submodule"

age
old="$(mtime)"
echo y >> "$Repo/tracked.txt"
run
expect "tracked change" \
    "$(head_of "$Repo")" true "$(head_of "$Repo/core")" false
expect_rewritten "$old" "tracked change"

git_ -C "$Repo" commit -qam two
run
expect "new commit" \
    "$(head_of "$Repo")" false "$(head_of "$Repo/core")" false

command mkdir "$Repo/plain"
run "$Git" "$Repo/plain"
expect "core without a checkout" "$(head_of "$Repo")" false unknown false

run ""
expect "no git" unknown false unknown false

echo "$Checks checks, $Failures failures"
[ "$Failures" = 0 ]
