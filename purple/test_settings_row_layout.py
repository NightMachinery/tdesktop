#!/usr/bin/env python3
"""No Purple settings row may carry both a right label and a toggle.

SettingsButton draws its toggle at the right edge, and CreateRightLabel puts the
label at the same edge without leaving room for it, so a row with both draws the
text under the switch. That shipped once ("Not Available" under three per-account
switches). This check reads every Purple source and fails on any such row.
"""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ROOT / 'Telegram/SourceFiles/purple'


def strip(text):
    """Blank out comments and string literals, keeping offsets and newlines."""
    pattern = re.compile(r'//[^\n]*|/\*.*?\*/|R"([^(\s]*)\(.*?\)\1"|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)
    return pattern.sub(lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text)


def block(text, start):
    depth = 0
    for index in range(start, len(text)):
        if text[index] in '({[':
            depth += 1
        elif text[index] in ')}]':
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    return text[start:]


def problems(text):
    text = strip(text)
    found = []
    for match in re.finditer(r'\baddButton\s*\(', text):
        body = block(text, match.end() - 1)
        if re.search(r'\.\s*label\s*=', body) and re.search(r'\.\s*toggled\s*=', body):
            found.append((text.count('\n', 0, match.start()) + 1, 'addButton with .label and .toggled'))
    for match in re.finditer(r'\b(?:const\s+)?auto\s+(\w+)\s*=\s*(?:[\w:]+::)?(?:AddButtonWithLabel|CreateButtonWithLabel)\s*\(', text):
        if re.search(r'\b' + re.escape(match.group(1)) + r'\s*->\s*toggleOn\s*\(', text[match.end():]):
            found.append((text.count('\n', 0, match.start()) + 1, match.group(1) + ' has a right label and toggleOn'))
    for match in re.finditer(r'\bCreateRightLabel\s*\(\s*(\w+)', text):
        name = match.group(1)
        if re.search(r'\b' + re.escape(name) + r'\s*->\s*toggleOn\s*\(', text):
            found.append((text.count('\n', 0, match.start()) + 1, name + ' has CreateRightLabel and toggleOn'))
    return found


class Detector(unittest.TestCase):
    def test_builder_row_with_both(self):
        self.assertTrue(problems('builder.addButton({ .id = x, .label = l(), .st = &s, .toggled = t() });'))

    def test_builder_rows_with_one(self):
        self.assertFalse(problems('builder.addButton({ .label = l() }); builder.addButton({ .toggled = t() });'))

    def test_helper_label_then_toggle(self):
        self.assertTrue(problems('const auto row = ::Settings::AddButtonWithLabel(c, t, l, st);\nrow->toggleOn(v());'))
        self.assertTrue(problems('auto button = Foo(); CreateRightLabel(button.data(), l, st, t); button->toggleOn(v());'))

    def test_comments_and_strings_ignored(self):
        self.assertFalse(problems('// addButton({ .label = a, .toggled = b })\nauto s = ".label = .toggled = addButton(";'))


def main():
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Detector)
    if not unittest.TextTestRunner(stream=sys.stdout, verbosity=0).run(suite).wasSuccessful():
        return 1
    failures = 0
    files = sorted(p for p in SOURCES.rglob('*') if p.suffix in ('.cpp', '.mm', '.h'))
    for path in files:
        for line, what in problems(path.read_text(encoding='utf-8', errors='replace')):
            print(f'{path.relative_to(ROOT)}:{line}: {what}')
            failures += 1
    print(f'settings row layout: {len(files)} files, {failures} rows with a label under a toggle')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
