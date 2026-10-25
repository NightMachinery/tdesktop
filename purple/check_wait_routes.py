#!/usr/bin/env python3
import argparse
from collections import Counter
import json
from pathlib import Path
import re

WRITERS = (
    'StartPeek', 'ExtendPeek', 'StopPeek', 'ActivatePeekFor',
    'ActivatePeekExtension', 'DeactivatePeek', 'writeWorkMode', 'SetAccountWorkMode',
    'switchGlobalAccount', 'commitGlobalChoice', 'activateGlobalChoice',
    'UpdateState',
)
TOKENS = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|R"(?P<tag>[^ ()\\\t\r\n]{0,16})\([\s\S]*?\)(?P=tag)"|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
CALL = re.compile(r'\b(' + '|'.join(WRITERS) + r')\s*\(')
PRESET = re.compile(r'\bactivePreset\s*=(?!=)')


def inventory(text):
    code = TOKENS.sub(lambda match: '\n' * match.group().count('\n') + ' ', text)
    found = Counter(match.group(1) for match in CALL.finditer(code))
    found['activePreset='] += len(PRESET.findall(code))
    return dict(sorted((key, value) for key, value in found.items() if value))


def scan(repo):
    result = {}
    for root in (repo / 'Telegram/SourceFiles', repo / 'Telegram/ThirdParty/purple_core/purple'):
        for source in sorted(root.rglob('*')):
            if source.suffix not in {'.cpp', '.h', '.mm', '.cc'} or 'tests' in source.parts or 'test' in source.parts:
                continue
            calls = inventory(source.read_text())
            if calls:
                result[str(source.relative_to(repo))] = calls
    return result


def main():
    parser = argparse.ArgumentParser(description='Review direct Work Mode/Peek writer call spellings, regardless of receiver.')
    parser.add_argument('--inventory', action='store_true')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    actual = scan(repo)
    if args.inventory:
        print(json.dumps(actual, indent=2, sort_keys=True))
        return 0
    expected = json.loads((repo / 'purple/wait_routes.json').read_text())
    if actual != expected:
        for name in sorted(set(actual) | set(expected)):
            if actual.get(name) != expected.get(name):
                print(f'{name}: expected {expected.get(name, {})}, found {actual.get(name, {})}')
        print('Direct writer inventory changed. Route UI changes through RequestWait and review documented exemptions.')
        return 1
    print(f'Wait writer inventory: {len(actual)} files match.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
