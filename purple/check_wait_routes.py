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
    'UpdateState', 'SetPeekStartDelay', 'SetPeekReadyTimeout',
)
TOKENS = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|R"(?P<tag>[^ ()\\\t\r\n]{0,16})\([\s\S]*?\)(?P=tag)"|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
CALL = re.compile(r'\b(' + '|'.join(WRITERS) + r'|SetTable\w*)\s*\(')
PRESET = re.compile(r'\bactivePreset\s*=(?!=)')
FOCUS_KEY = '__PEEK_FOCUS_PREFERENCE_KEY__'
PREFERENCE_WRITE = re.compile(r'\bwritePref\s*(?:<[^;{}]+?>)?\s*\(([^,;]+),')
PREFERENCE_ALIAS = re.compile(r'\b(\w+)\s*=\s*(?:(?:u8?|U|L)\s*)?(\w+)\b')


def focus_write_inventory(text):
    def token(match):
        if match.group() == '"purple-peek-end-on-focus-loss"':
            return ' ' + FOCUS_KEY + ' '
        return '\n' * match.group().count('\n') + ' '

    code = TOKENS.sub(token, text)
    aliases = {FOCUS_KEY}
    declarations = PREFERENCE_ALIAS.findall(code)
    while True:
        expanded = aliases | {name for name, value in declarations if value in aliases}
        if expanded == aliases:
            break
        aliases = expanded
    guarded = re.search(r'\bvoid\s+RequestPeekOptionChange\s*\([^;{]*\)\s*\{', code)
    begin = guarded.end() if guarded else -1
    end = begin
    depth = 1
    while begin >= 0 and end < len(code) and depth:
        depth += (code[end] == '{') - (code[end] == '}')
        end += 1
    found = Counter()
    for call in PREFERENCE_WRITE.finditer(code):
        if aliases.intersection(re.findall(r'\b\w+\b', call.group(1))):
            found['writePref<peek-focus-loss>'] += 1
            if not (begin <= call.start() < end):
                found['ungatedPeekFocusWrite'] += 1
    return found


def inventory(text):
    code = TOKENS.sub(lambda match: '\n' * match.group().count('\n') + ' ', text)
    found = Counter(match.group(1) for match in CALL.finditer(code))
    found['activePreset='] += len(PRESET.findall(code))
    found.update(focus_write_inventory(text))
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
