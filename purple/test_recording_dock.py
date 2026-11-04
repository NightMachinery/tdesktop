#!/usr/bin/env python3
"""Build the recording ownership and round geometry host cases."""
import argparse, pathlib, shutil, subprocess
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output-dir', type=pathlib.Path, required=True)
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
args.output_dir.mkdir(parents=True, exist_ok=False)
program = args.output_dir / 'recording-dock'
subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'Telegram/SourceFiles'), str(root / 'Telegram/SourceFiles/purple/tests/recording_dock_cases.cpp'), '-o', str(program)], check=True)
subprocess.run([str(program)], check=True)

from recording_integration import run
run(root, args.output_dir)
