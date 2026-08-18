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
LibrariesPath="${LibrariesPath:-$(dirname "$RepoPath")/tdesktop-libs}"
# Must be the merged prefix's copy, so the bundled plugins match the Qt the
# app was linked against - see pin_deploy_tool() in merge_qt_prefix.py.
MacDeployQt="${MacDeployQt:-$LibrariesPath/local/qt/bin/macdeployqt}"

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

# macdeployqt strips the binary, but only at the very end - after it has run
# install_name_tool once per Qt framework reference, each of which rewrites the
# whole binary. On the 788MB unstripped binary that measured at 625 seconds.
# Stripping it ourselves first leaves 232MB for that loop to chew through, and
# the strip itself takes two seconds.
#
# Keep the symbols before we do. This copy has the same LC_UUID as the shipped
# binary, so crash reports symbolicate against it, and it still carries the
# debug map pointing at out/'s object files:
#     atos -o "out/Purple Telegram.unstripped" -l <load address> <address>
#     dsymutil "out/Purple Telegram.unstripped"     # for a standalone .dSYM
Binary="$Source/Contents/MacOS/$AppName"
Unstripped="$BuildPath/$AppName.unstripped"
echo "=== keeping symbols in $Unstripped ==="
cp "$Binary" "$Unstripped"
strip -S -x "$Binary"

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
