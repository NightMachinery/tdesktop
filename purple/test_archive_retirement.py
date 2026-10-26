#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--output-dir', required=True, type=Path)
parser.add_argument('--build-only', action='store_true')
parser.add_argument('--negative-control', action='store_true')
parser.add_argument('--compiler', default='/usr/bin/c++')
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
work = args.output_dir.resolve()
work.mkdir(parents=True, exist_ok=True)
source = repo / 'Telegram/SourceFiles/purple/purple_archive_service.cpp'
service = source.read_text()
start = service.index('void PruneStreamsSoon()')
position = service.index('{', start) + 1
depth = 1
while depth:
    depth += (service[position] == '{') - (service[position] == '}')
    position += 1
method = service[start:position]
if args.negative_control:
    lock = '\n\t\tconst auto lock = std::lock_guard(_state->mutex);'
    assert method.count(lock) == 1
    method = method.replace(lock, '')
predicate = re.search(r'return state->cancelled.load\(\) \|\| !state->tasks.empty\(\).*?;',
                      service, re.S).group()
(work / 'retirement-method.inc').write_text(method + '\n')
(work / 'retirement-predicate.inc').write_text(predicate + '\n')
compiler = args.compiler
fixture = repo / 'purple/test_archive_retirement.cpp'
command = [compiler, '-std=c++20', '-pthread', str(fixture), '-I', str(work),
           '-o', str(work / 'test_archive_retirement')]
with (work / 'build.log').open('w') as log:
    subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
(work / 'inputs.json').write_text(json.dumps({
    'command': command, 'negative_control': args.negative_control,
    'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'fixture_sha256': hashlib.sha256(fixture.read_bytes()).hexdigest(),
    'method_sha256': hashlib.sha256(method.encode()).hexdigest(),
    'predicate_sha256': hashlib.sha256(predicate.encode()).hexdigest(),
}, indent=2) + '\n')
if not args.build_only:
    raise SystemExit(subprocess.run([str(work / 'test_archive_retirement')], timeout=10).returncode)
