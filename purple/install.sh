#!/usr/bin/env bash
# Bundles the Qt frameworks into the built app, signs it and installs it to
# /Applications. See docs/mac/build.md.
set -e

RepoPath="$(cd "$(dirname "$0")/.." && pwd)"
BuildPath="${BuildPath:-$RepoPath/out}"
AppName="${AppName:-Purple Telegram}"
BundleId="com.tdesktop.PurpleTelegram"
Source="$BuildPath/$AppName.app"
# Overridable so a test deploy can go somewhere that is not the installed app.
Target="${Target:-/Applications/$AppName.app}"
LibrariesPath="${LibrariesPath:-$(dirname "$RepoPath")/tdesktop-libs}"

# macdeployqt has to come from the same prefix the app was linked against, or
# the bundled plugins disagree on Qt version with the Qt they are loaded into
# and the app dies with "no Qt platform plugin could be initialized". Same rule
# as build_app.sh, and it has the long version of the comment: the patched
# prefix when purple/build_qt.sh has produced one, otherwise the merged
# Homebrew prefix - whose copy of macdeployqt is additionally pinned by
# pin_deploy_tool() in merge_qt_prefix.py. Setting QtPrefix overrides both.
if [ -z "$QtPrefix" ]; then
    if [ -d "$LibrariesPath/qt-patched" ]; then
        QtPrefix="$LibrariesPath/qt-patched"
    else
        QtPrefix="$LibrariesPath/local/qt"
    fi
fi
MacDeployQt="${MacDeployQt:-$QtPrefix/bin/macdeployqt}"
echo "=== Qt prefix: $QtPrefix ==="

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

# macdeployqt only needs to do its real work once. After the first deploy the
# frameworks and plugins in the bundle are already there and already relinked,
# and the only thing that changed is the main binary. Fixing that up takes one
# install_name_tool pass; macdeployqt takes one per dependency, and there are
# 121 of them, each rewriting the whole binary.
#
# relink_bundle.py hands back to macdeployqt whenever the bundle is not in a
# state it can finish - never deployed, missing plugins or qt.conf, or linking
# something the bundle does not carry. Set ForceDeploy=1 to skip it entirely.
if [ -n "$ForceDeploy" ]; then
    RelinkStatus=2
else
    set +e
    python3 "$RepoPath/purple/relink_bundle.py" "$Source"
    RelinkStatus=$?
    set -e
fi

if [ "$RelinkStatus" -eq 2 ]; then
    echo "=== macdeployqt ==="
    "$MacDeployQt" "$Source"
elif [ "$RelinkStatus" -ne 0 ]; then
    echo "Relinking failed." >&2
    exit "$RelinkStatus"
fi

#: Signed with a certificate when there is one, because TCC keys its grants -
#: Full Disk Access for the focus detector, in particular - to the app's
#: designated requirement. Ad-hoc has no certificate, so that requirement is
#: the code hash of one exact binary and every build invalidates it. With a
#: certificate the requirement names the identifier and the certificate, and
#: neither moves when the code does. purple/make_signing_cert.sh creates it.
SigningIdentity="${SigningIdentity:-Purple Telegram Local Signing}"
if security find-identity -v -p codesigning \
        | grep -qF "$SigningIdentity"; then
    echo "=== signing as '$SigningIdentity' ==="
    codesign --force --deep --sign "$SigningIdentity" "$Source"
else
    echo "=== signing ad-hoc ==="
    echo "    (run purple/make_signing_cert.sh to keep TCC grants across builds)"
    codesign --force --deep --sign - "$Source"
fi

echo "=== installing to $Target ==="
rm -rf "$Target"
ditto "$Source" "$Target"

echo "=== installed ==="
/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$Target/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Print CFBundleName' "$Target/Contents/Info.plist"
du -sh "$Target"
