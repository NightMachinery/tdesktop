#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import runpy
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--output-dir', type=Path, required=True)
parser.add_argument('--seed', type=int, action='append')
parser.add_argument('--scenario', choices=['matrix', 'races', 'all'], default='all')
parser.add_argument('--build-only', action='store_true')
args = parser.parse_args()
if args.seed and any(seed < 0 or seed > 0xffffffff for seed in args.seed):
    parser.error('--seed must fit an unsigned 32-bit integer')
repo = Path(__file__).resolve().parent.parent
work = args.output_dir.resolve()
work.mkdir(parents=True, exist_ok=True)
opening_argv = sys.argv
try:
    sys.argv = [str(repo / 'purple/test_sync_chat_write.py'),
        '--build-dir', str(args.build_dir.resolve()), '--output-dir', str(work),
        '--test-source', 'test_sync_chat_move.cpp', '--build-only']
    runpy.run_path(sys.argv[0], run_name='__main__')
finally:
    sys.argv = opening_argv
inputs = work / 'inputs.json'
receipt = json.loads(inputs.read_text())
receipt['move_driver_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
receipt['move_fixtures_sha256'] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
    for path in [repo / 'purple/test_sync_chat_move.cpp',
        repo / 'Telegram/SourceFiles/purple/tests/sync_chat_move_file_cases.cpp']}
inputs.write_text(json.dumps(receipt, indent=2) + '\n')
if not args.build_only:
    results = []
    for seed in args.seed or [271828, 314159, 424242]:
        fixtures = work / ('fixtures-' + str(seed))
        fixtures.mkdir(exist_ok=True)
        command = [str(work / 'test_sync_chat_move'), str(seed), str(fixtures), args.scenario]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env=os.environ | {'TMPDIR': str(fixtures)})
        (work / ('seed-' + str(seed) + '.log')).write_bytes(result.stdout)
        print(result.stdout.decode(), end='', flush=True)
        results.append({'seed': seed, 'scenario': args.scenario, 'argv': command, 'exit': result.returncode})
    (work / 'seeds.json').write_text(json.dumps(results, indent=2) + '\n')
    raise SystemExit(any(result['exit'] != 0 for result in results))
