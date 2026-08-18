#!/usr/bin/env python3
"""Rewrites the app binary's load commands the way macdeployqt would.

macdeployqt does far more than this - it copies the Qt frameworks and plugins
into the bundle, relinks each of them, and writes qt.conf. But it only needs to
do any of that once. On every rebuild after the first, the frameworks in the
bundle are already there and already correct, and the single thing that has
changed is the main binary, freshly linked against absolute paths in Homebrew
and in the merged Qt prefix.

macdeployqt fixes those by running install_name_tool once per dependency, and
each run rewrites the whole binary. There are 81 of them, so a 232MB binary gets
written 81 times: about 19GB of I/O, and seven minutes of it. This does the same
81 changes in one pass.

Exit codes:
    0  relinked, the caller can skip macdeployqt
    2  the bundle is not ready for the fast path; run macdeployqt
    1  something went wrong

See docs/mac/build.md.
"""

import os
import re
import subprocess
import sys

# Anything under these prefixes belongs to the OS and is linked by absolute
# path on purpose.
SYSTEM_PREFIXES = ("/usr/lib/", "/System/")

FRAMEWORK_TAIL = re.compile(
    r"/([A-Za-z0-9_]+\.framework/(?:Versions/[^/]+/)?[A-Za-z0-9_]+)$")


BUNDLE_RPATH = "@executable_path/../Frameworks"

# Per-formula Homebrew paths. Left in place they sit ahead of the bundle in the
# search order and can shadow a library we bundled with whatever the machine
# happens to have - the same trap that made us compile against FFmpeg 8 headers
# while linking FFmpeg 6. macdeployqt drops them; so do we. The generic
# /opt/homebrew/lib and the Qt prefix are left alone, which is also what
# macdeployqt does.
KEG_RPATH = re.compile(r"^/opt/homebrew/opt/[^/]+/")


def dependencies(binary):
    out = subprocess.run(
        ["otool", "-L", binary],
        capture_output=True, text=True, check=True).stdout
    return [line.split()[0] for line in out.splitlines()[1:]
            if line.startswith("\t")]


def rpaths(binary):
    out = subprocess.run(
        ["otool", "-l", binary],
        capture_output=True, text=True, check=True).stdout
    result = []
    lines = out.splitlines()
    for i, line in enumerate(lines):
        if line.strip() == "cmd LC_RPATH":
            for follow in lines[i:i + 4]:
                stripped = follow.strip()
                if stripped.startswith("path "):
                    result.append(stripped.split(None, 2)[1])
                    break
    return result


def bundled_path(dependency):
    """Where this dependency lives inside the bundle, relative to Frameworks."""
    match = FRAMEWORK_TAIL.search(dependency)
    return match.group(1) if match else os.path.basename(dependency)


def main():
    if len(sys.argv) != 2:
        print("usage: relink_bundle.py <path to .app>", file=sys.stderr)
        return 1

    app = sys.argv[1].rstrip("/")
    name = os.path.basename(app)[:-len(".app")]
    binary = os.path.join(app, "Contents", "MacOS", name)
    frameworks = os.path.join(app, "Contents", "Frameworks")
    plugins = os.path.join(app, "Contents", "PlugIns")
    qtconf = os.path.join(app, "Contents", "Resources", "qt.conf")

    if not os.path.isfile(binary):
        print(f"no binary at {binary}", file=sys.stderr)
        return 1

    # A bundle that has never been deployed needs the real thing. So does one
    # missing its plugins or qt.conf, which is how Qt finds them at runtime.
    for required in (frameworks, plugins):
        if not os.path.isdir(required) or not os.listdir(required):
            print(f"not deployed yet: {required} is missing or empty")
            return 2
    if not os.path.isfile(qtconf):
        print(f"not deployed yet: {qtconf} is missing")
        return 2

    changes = []
    for dependency in dependencies(binary):
        if dependency.startswith("@") or dependency.startswith(SYSTEM_PREFIXES):
            continue
        if not dependency.startswith("/"):
            continue
        inside = bundled_path(dependency)

        # If the build has started depending on something the bundle does not
        # carry, rewriting it would point the binary at a file that is not
        # there and the app would not launch. Hand over to macdeployqt, which
        # knows how to copy it in.
        if not os.path.exists(os.path.join(frameworks, inside)):
            print(f"not in the bundle: {inside}")
            return 2
        changes += ["-change", dependency,
                    f"@executable_path/../Frameworks/{inside}"]

    # The bundled Qt frameworks resolve some of their own dependencies through
    # @rpath, against the rpath list of the executable that loaded them, so
    # this one is load-bearing rather than cosmetic.
    existing = rpaths(binary)
    rpath_changes = []
    for path in existing:
        if KEG_RPATH.match(path):
            rpath_changes += ["-delete_rpath", path]
    if BUNDLE_RPATH not in existing:
        rpath_changes += ["-add_rpath", BUNDLE_RPATH]

    if not changes and not rpath_changes:
        print("nothing to relink")
        return 0

    # One pass. install_name_tool rewrites the whole file per invocation, so
    # the number of invocations is the entire cost.
    subprocess.run(
        ["install_name_tool"] + changes + rpath_changes + [binary],
        check=True,
        stderr=subprocess.DEVNULL)

    print(f"relinked {len(changes) // 2} dependencies"
          f" and {len(rpath_changes) // 2} rpaths in one pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
