#!/usr/bin/env bash
# Bundles the Qt frameworks into the built app, signs it ad-hoc and installs
# it to /Applications. See docs/mac/build.md.
set -e

RepoPath="$(cd "$(dirname "$0")/.." && pwd)"
BuildPath="${BuildPath:-$RepoPath/out}"
AppName="${AppName:-Purple Telegram}"
BundleId="com.tdesktop.PurpleTelegram"
Source="$BuildPath/$AppName.app"
Target="/Applications/$AppName.app"
MacDeployQt="${MacDeployQt:-/opt/homebrew/opt/qtbase/bin/macdeployqt}"

if [ ! -d "$Source" ]; then
    echo "No bundle at $Source - build it first." >&2
    exit 1
fi

# Never clobber a different app that happens to sit at the target path.
if [ -d "$Target" ]; then
    existing="$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$Target/Contents/Info.plist" 2>/dev/null || echo '')"
    if [ "$existing" != "$BundleId" ]; then
        echo "$Target already exists and is '$existing', not '$BundleId'." >&2
        echo "Refusing to overwrite it. Remove it by hand if that is what you want." >&2
        exit 1
    fi
fi

echo "=== macdeployqt ==="
"$MacDeployQt" "$Source"

echo "=== signing ad-hoc ==="
codesign --force --deep --sign - "$Source"

echo "=== installing to $Target ==="
rm -rf "$Target"
ditto "$Source" "$Target"

echo "=== installed ==="
/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$Target/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Print CFBundleName' "$Target/Contents/Info.plist"
du -sh "$Target"
