#!/usr/bin/env python3
"""Check Purple settings routes and optionally render native widgets from a macOS build."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "Telegram/SourceFiles"


def ordered(text, *parts):
    cursor = 0
    for part in parts:
        cursor = text.index(part, cursor) + len(part)


def routes():
    purple = (SOURCE / "purple/purple_settings_section.cpp").read_text()
    search = (SOURCE / "settings/settings_search.cpp").read_text()
    builder = (SOURCE / "settings/settings_builder.cpp").read_text()
    playback = (SOURCE / "purple/purple_media_playback.cpp").read_text()
    recording = (SOURCE / "purple/purple_recording_settings.cpp").read_text()
    archive = (SOURCE / "purple/purple_archive_service.cpp").read_text()
    ordered(purple, "const auto kPages = std::array", "ArchiveSettings::Id()",
            "WorkActivitySettings::Id()", "MediaSettings::Id()", "PrivacySettings::Id()",
            "AccountsSettings::Id()", "DiagnosticsSettings::Id()", ".parentId = PurpleSettings::Id()")
    ordered(purple, "void BuildPurpleSection", "advanced/purple_customize_settings",
            "for (const auto &page : kPages)", "builder.addSectionButton", "AddVersionRow")
    ordered(purple, "builder.addSectionButton", "ArchiveSummaryValue(builder.session(), true)")
    ordered(archive, "QString ArchiveStorageSizeLabel", "FormatArchiveSize(usage->bytes)",
            "if (!usage->complete)", "label += u'+'", "return st::wrap_rtl(label)")
    ordered(purple, "void BuildArchiveSettings", "ArchiveSummaryValue(session)",
            "purple/archive_cleanup", "advanced/purple_archive_trash",
            "advanced/purple_archive_edits", 'u"trash"_q', "LocalStorageId()",
            "advanced/purple_archive_discard", "ArchiveDiscardAction", ".confirmed",
            "if (current()) { discard(); }", "sessionControllerValue()", "!current()",
            "confirm->closeBox()")
    ordered(purple, "auto CurrentSettingsWindow", "base::make_weak(builder.controller())",
            "own->window().sessionController() == own", "live->account().maybeSession() == live")
    ordered(builder, "SearchRegistry::collectAll", "contains(root, sectionId)",
            "contains(root, entry.sectionId)", "entry.indexer(session)")
    ordered(builder, "BuildHelper::index", "SectionBuilder(SearchContext", "_method(builder)")
    ordered(search, "void Search::buildIndex", "registry.collectAll(&controller()->session(), _scope.root)")
    ordered(search, "const auto controlId = entry.id", "controller()->setHighlightControlId(controlId)",
            "showOtherMethod()(targetSection)")
    ordered(search, "_showLanding(queryWords.isEmpty())", "void Search::sectionSaveState",
            "SearchSectionState{ query }", "void Search::sectionRestoreState", "_searchField->setText")
    assert "builder.addDividerText(tr::lng_purple_video_message_size_about());\n}" in playback
    ordered(recording, "void BuildMediaRecordingSettings", "builder.addSkip();",
            "advanced/purple_media_recording")
    prefix = recording.split("void BuildMediaRecordingSettings", 1)[1].split("builder.addSkip();", 1)[0]
    assert "builder.addDivider();" not in prefix
    print("Purple settings navigation, scoped search, and cleanup route checks passed")


def render(build, qt, out):
    qt = qt.resolve()
    build = build.resolve()
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    products = build / "Telegram"
    frameworks = qt / "frameworks"
    if not (frameworks / "QtCore.framework").exists():
        frameworks = qt / "lib"
    target = "Telegram/CMakeFiles/Telegram.dir/SourceFiles/settings/settings_builder.cpp.o"
    commands = subprocess.check_output(
        ["ninja", "-C", str(build), "-t", "commands", target], text=True)
    flags = shlex.split(commands.splitlines()[-1])
    flags = flags[:flags.index("-MD")]
    with tempfile.TemporaryDirectory(prefix="settings-nav-", dir=out) as temporary:
        fixture = Path(temporary)
        obj, exe = fixture / "render.o", fixture / "render"
        subprocess.run([*flags, "-c", str(ROOT / "purple/test_settings_navigation_render.cpp"),
                        "-o", str(obj)], cwd=build, check=True)
        ui_objects = products / "CMakeFiles/td_ui.dir"
        styles = sorted((ui_objects / "gen/styles").glob("*.cpp.o"))
        subprocess.run([
            flags[0], str(obj), str(ui_objects / "SourceFiles/ui/vertical_list.cpp.o"),
            *map(str, styles), str(products / "lib_ui/liblib_ui.a"),
            str(products / "lib_base/liblib_base.a"),
            "-L", "/opt/homebrew/opt/openssl@3/lib", "-lcrypto",
            "-L", "/opt/homebrew/opt/xxhash/lib", "-lxxhash",
            "-F", str(frameworks), f"-Wl,-rpath,{frameworks}", "-Wl,-dead_strip",
            *(arg for name in ("QtCore", "QtGui", "QtWidgets", "QtSVG", "QtDBus", "Cocoa",
                                "CoreGraphics", "CoreText", "CoreServices", "IOKit",
                                "QuartzCore", "Metal") for arg in ("-framework", name)),
            "-o", str(exe),
        ], check=True)
        strings = (ROOT / "Telegram/Resources/langs/purple.strings").read_text()
        strings = dict(re.findall(
            r'^"(lng_purple_(?:settings_|video_message_|media_recording|recording_keep_chats|archive_usage)\w*)"\s*=\s*("(?:[^"\\]|\\.)*");',
            strings, re.M))
        text = fixture / "strings.json"
        text.write_text(json.dumps({key: json.loads(value) for key, value in strings.items()}))
        theme = fixture / "night.colors"
        with zipfile.ZipFile(ROOT / "Telegram/Resources/night.tdesktop-theme") as archive:
            theme.write_bytes(archive.read("colors.tdesktop-theme"))
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen",
                   QT_QPA_PLATFORM_PLUGIN_PATH=str(qt / "share/qt/plugins/platforms"),
                   XDG_CONFIG_HOME=str(fixture / "config"),
                   XDG_CACHE_HOME=str(fixture / "cache"))
        for name, colors in (("light", ""), ("dark", str(theme))):
            for scale in (100, 150):
                subprocess.run([str(exe), str(scale), colors, str(text), str(out / f"{name}-{scale}"), "wide"],
                               cwd=ROOT, env=env, check=True)
        for direction in ("ltr", "rtl"):
            subprocess.run([
                str(exe), "150", str(theme), str(text), str(out / f"narrow-{direction}-150"), direction,
            ], cwd=ROOT, env=env, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--qt", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    routes()
    if args.build or args.qt or args.out:
        if not (args.build and args.qt and args.out):
            parser.error("--build, --qt, and --out are required together for rendering")
        render(args.build, args.qt, args.out)


if __name__ == "__main__":
    main()
