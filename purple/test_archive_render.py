#!/usr/bin/env python3
"""Render the actual archive v2 boxes with Telegram styles on an offscreen Qt surface."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import zipfile

repo = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build-dir", type=Path, required=True)
parser.add_argument("--output-dir", type=Path, required=True)
parser.add_argument("--qt", type=Path, required=True)
parser.add_argument("--build-only", action="store_true")
args = parser.parse_args()
build = args.build_dir.resolve()
out = args.output_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
products = build / "Telegram"
frameworks = args.qt / "frameworks"
if not (frameworks / "QtCore.framework").exists():
    frameworks = args.qt / "lib"
target = "Telegram/CMakeFiles/Telegram.dir/SourceFiles/purple/purple_archive_v2_ui.cpp.o"
exe = out / "test_archive_render"
if not exe.exists() or args.build_only:
    output = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", target], text=True)
    recipe = shlex.split(output.splitlines()[-1])
    flags = [part for part in recipe[:recipe.index("-MD")] if not part.startswith("-DLANG_KEYS_SUBSET=")]
    obj = out / "render.o"
    subprocess.run([*flags, "-c", str(repo / "purple/test_archive_render.cpp"), "-o", str(obj)], cwd=build, check=True)
    ui = products / "CMakeFiles/td_ui.dir"
    styles = sorted((ui / "gen/styles").glob("*.cpp.o"))
    source = products / "CMakeFiles/Telegram.dir/SourceFiles"
    iv = products / "CMakeFiles/td_iv.dir/SourceFiles/iv"
    capture = [
        source / "purple/purple_archive_capture.cpp.o",
        source / "api/api_text_entities.cpp.o",
        source / "data/data_peer_id.cpp.o",
        source / "data/stickers/data_custom_emoji.cpp.o",
        source / "history/history_item_reply_markup.cpp.o",
        source / "iv/iv_rich_page.cpp.o",
        source / "iv/iv_rich_page_quote.cpp.o",
        iv / "markdown/iv_markdown_prepare_native_blocks.cpp.o",
        iv / "markdown/iv_markdown_prepare_serialize.cpp.o",
        iv / "markdown/iv_markdown_prepare_links.cpp.o",
    ]
    crl = products / "lib_crl/CMakeFiles/lib_crl.dir/crl"
    subprocess.run([
        recipe[0], str(obj), str(build / target), *map(str, styles), *map(str, capture),
        str(crl / "dispatch/crl_dispatch_async.cpp.o"),
        str(products / "CMakeFiles/td_scheme.dir/gen/scheme.cpp.o"),
        str(products / "lib_tl/CMakeFiles/lib_tl.dir/tl/tl_basic_types.cpp.o"),
        str(products / "CMakeFiles/td_lang.dir/gen/lang_auto.cpp.o"),
        str(products / "CMakeFiles/td_lang.dir/SourceFiles/lang/lang_tag.cpp.o"),
        str(products / "CMakeFiles/td_lang.dir/SourceFiles/lang/lang_keys.cpp.o"),
        str(ui / "SourceFiles/ui/text/format_values.cpp.o"),
        str(products / "lib_ui/liblib_ui.a"), str(products / "lib_base/liblib_base.a"),
        "-L/opt/homebrew/opt/openssl@3/lib", "-lcrypto", "-L/opt/homebrew/opt/xxhash/lib", "-lxxhash",
        "-F", str(frameworks), f"-Wl,-rpath,{frameworks}", "-Wl,-dead_strip",
        *(arg for name in ("QtCore", "QtGui", "QtWidgets", "QtSVG", "QtDBus", "Cocoa", "CoreGraphics",
                          "CoreText", "CoreServices", "IOKit", "QuartzCore", "Metal") for arg in ("-framework", name)),
        "-o", str(exe),
    ], check=True)
if args.build_only:
    raise SystemExit(0)
theme = out / "night.colors"
with zipfile.ZipFile(repo / "Telegram/Resources/night.tdesktop-theme") as z:
    theme.write_bytes(z.read("colors.tdesktop-theme"))
env = dict(os.environ, QT_QPA_PLATFORM="offscreen",
           PURPLE_ARCHIVE_RENDER_RESOURCES=str(build / "Telegram.rcc"),
           QT_PLUGIN_PATH=str(args.qt / "share/qt/plugins"),
           QT_QPA_PLATFORM_PLUGIN_PATH=str(args.qt / "share/qt/plugins/platforms"),
           XDG_CONFIG_HOME=str(out / "config"), XDG_CACHE_HOME=str(out / "cache"),
           XDG_DATA_HOME=str(out / "data"))
for scheme, colors in [("light", ""), ("dark", str(theme))]:
    for width in (500, 800, 1300):
        for direction in ("ltr", "rtl"):
            directory = out / f"{scheme}-{width}-{direction}"
            directory.mkdir(exist_ok=True)
            subprocess.run([str(exe), str(directory), colors, str(width), direction], cwd=repo,
                           env=env, check=True)
print("Rendered seven native archive boxes at 500, 800 and 1300 px in light/dark and LTR/RTL.")
