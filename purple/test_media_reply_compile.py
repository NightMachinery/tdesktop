#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys

parser = argparse.ArgumentParser(description="Compile native media-reply production units without a product build")
parser.add_argument("--build-dir", type=Path, required=True)
parser.add_argument("--output-dir", type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
build = args.build_dir.resolve()
out = args.output_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
gen = out / "gen"
gen.mkdir(exist_ok=True)
strings = out / "lang.strings"
strings.write_bytes((repo / "Telegram/Resources/langs/lang.strings").read_bytes()
    + b"\n" + (repo / "Telegram/Resources/langs/purple.strings").read_bytes())
codegen = build / "Telegram/codegen/codegen/lang/codegen_lang"
subprocess.run([str(codegen), "-o" + str(gen), str(strings)], check=True)
target = "Telegram/CMakeFiles/Telegram.dir/SourceFiles/purple/purple_config_file.cpp.o"
lines = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", target], text=True).splitlines()
recipe = next(shlex.split(line) for line in lines if " -c " in line
    and shlex.split(line)[shlex.split(line).index("-c") + 1].endswith("/purple_config_file.cpp"))
old = Path(recipe[recipe.index("-c") + 1]).parents[3]
flags = []
skip = False
for value in recipe[1:]:
    if skip:
        skip = False
        continue
    if value in ["-o", "-MF", "-MT", "-c", "-include", "-include-pch", "-Xclang"]:
        skip = True
        continue
    if value in ["-MD", "-MMD"] or value.startswith(("-g", "-DLANG_KEYS_SUBSET=")) or "cmake_pch" in value:
        continue
    if value == "-I" + str(old):
        value = "-I" + str(repo)
    else:
        value = value.replace(str(old) + "/", str(repo) + "/")
    flags.append(value)
flags = ["-I" + str(gen), "-I" + str(repo / "Telegram/SourceFiles"),
    "-I" + str(repo / "Telegram/ThirdParty/purple_core"), "-include",
    str(repo / "Telegram/SourceFiles/stdafx.h"), *flags]
units = ["purple/purple_media_reply_adapter.cpp", "purple/purple_media_reply_settings.cpp",
    "purple/purple_config.cpp", "purple/purple_settings_section.cpp",
    "purple/purple_app_services.cpp", "api/api_updates.cpp"]
results = []
for unit in units:
    path = repo / "Telegram/SourceFiles" / unit
    command = [recipe[0], *flags, "-fsyntax-only", "-MD", "-MF",
        str(out / (path.stem + ".d")), str(path)]
    with (out / (path.stem + ".log")).open("w") as log:
        result = subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT)
    results.append({"source": unit, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "command": command, "exit": result.returncode})
    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    if result.returncode:
        print((out / (path.stem + ".log")).read_text(), flush=True)
        raise SystemExit(result.returncode)
print("MEDIA_REPLY_NATIVE_UNITS=" + str(len(results)))
