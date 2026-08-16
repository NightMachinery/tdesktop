#!/usr/bin/env python3
"""Merges Homebrew's separate Qt module kegs into one prefix of symlinks.

Upstream CI installs qtbase, qtsvg, qtimageformats and qtshadertools with
nothing else claiming /opt/homebrew, so Homebrew links all four into that one
prefix and CMake finds every Qt6 component under it. On a machine that also
has the full "qt" formula, that formula owns the links and the four module
kegs stay unlinked in prefixes of their own — at which point Qt6Config looks
for its components next to qtbase and fails on Qt6Svg.

This reproduces the merged layout without touching Homebrew's links, so the
system "qt" keeps working. Files are symlinked, directories are recreated, and
the first keg to provide a path wins.

    python3 purple/merge_qt_prefix.py <destination>
"""

import os
import shutil
import sys

KEGS = ("qtbase", "qtsvg", "qtimageformats", "qtshadertools")
SKIP = {".brew", "INSTALL_RECEIPT.json", "sbom.spdx.json"}


def merge(source, destination):
    linked = 0
    for entry in sorted(os.listdir(source)):
        if entry in SKIP:
            continue
        source_path = os.path.join(source, entry)
        target_path = os.path.join(destination, entry)
        if os.path.isdir(source_path) and not os.path.islink(source_path):
            os.makedirs(target_path, exist_ok=True)
            linked += merge(source_path, target_path)
        elif not os.path.lexists(target_path):
            os.symlink(source_path, target_path)
            linked += 1
    return linked


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    destination = os.path.abspath(sys.argv[1])
    if os.path.exists(destination):
        shutil.rmtree(destination)
    os.makedirs(destination)

    for keg in KEGS:
        source = f"/opt/homebrew/opt/{keg}"
        if not os.path.isdir(source):
            print(f"missing keg: {source}", file=sys.stderr)
            return 1
        print(f"{keg}: {merge(source, destination)} links")

    config = os.path.join(destination, "lib/cmake/Qt6/Qt6Config.cmake")
    if not os.path.exists(config):
        print(f"merged prefix has no {config}", file=sys.stderr)
        return 1

    drop_vulkan_headers(destination)
    make_framework_search_dir(destination)
    print("merged Qt prefix at", destination)
    return 0


def make_framework_search_dir(destination):
    """Creates a second, distinct directory holding links to Qt's frameworks.

    Qt's own framework directory reaches the compiler as "-iframework", which
    clang searches only after every "-isystem" directory — including
    /opt/homebrew/include, which on a machine with the full "qt" formula
    shadows Qt's headers with that formula's older ones.

    "-F" is searched in the earlier angled list and would win, but pointing it
    at the same directory is a no-op: clang resolves search paths to real
    directories and discards the duplicate. A separate real directory holding
    symlinks to each framework is distinct enough to survive that, while
    resolving to exactly the same headers.
    """
    library_dir = os.path.join(destination, "lib")
    search_dir = os.path.join(destination, "frameworks")
    os.makedirs(search_dir, exist_ok=True)
    count = 0
    for entry in sorted(os.listdir(library_dir)):
        if not entry.endswith(".framework"):
            continue
        link = os.path.join(search_dir, entry)
        if not os.path.lexists(link):
            os.symlink(os.path.join(library_dir, entry), link)
        count += 1
    print(f"framework search dir: {count} frameworks in {search_dir}")


def drop_vulkan_headers(destination):
    """Removes Vulkan headers from Qt6::Gui's interface.

    Homebrew's qtbase records the Vulkan headers as living in /opt/homebrew/
    include, so that directory becomes part of Qt6::Gui's include interface.
    On a machine that also has the full "qt" formula, /opt/homebrew/include
    holds QtGui, QtCore ... symlinks into that formula's older headers, and
    clang searches every -isystem directory before any framework directory —
    so those headers shadow the Qt we configured against and the build fails
    on whatever the older Qt lacks.

    Qt6GuiTargets.cmake refers to the target through TARGET_NAME_IF_EXISTS, so
    simply never creating it drops the directory cleanly. Telegram Desktop
    renders through Metal and OpenGL on macOS and does not use Vulkan.
    """
    path = os.path.join(destination, "lib/cmake/Qt6Gui/Qt6GuiDependencies.cmake")
    original = os.path.realpath(path)
    with open(original) as handle:
        text = handle.read()
    patched = text.replace(r";WrapVulkanHeaders\;TRUE\;\;\;", "")
    if patched == text:
        print("warning: WrapVulkanHeaders not found in Qt6GuiDependencies.cmake",
              file=sys.stderr)
        return
    os.remove(path)
    with open(path, "w") as handle:
        handle.write(patched)
    print("dropped WrapVulkanHeaders from Qt6::Gui")


if __name__ == "__main__":
    sys.exit(main())
