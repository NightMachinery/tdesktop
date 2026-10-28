#!/usr/bin/env python3
# The chat's top-bar menu offers the same Work Mode row as the chat list's
# context menu. Compiles the production fillHistoryActions, addToggleMuteSubmenu,
# AddHistoryListsSubmenu, AddPeerListsSubmenu and AddListsSubmenu against stubs
# and checks where the row lands and that both menus build the same row.
import argparse
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--output-dir', required=True, type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
src = repo / 'Telegram/SourceFiles'
work = args.output_dir.resolve()
work.mkdir(parents=True, exist_ok=True)


def extract(text, signature):
    begin = text.index(signature)
    position = text.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (text[position] == '{') - (text[position] == '}')
        position += 1
    return text[begin:position] + '\n'


def declaration(text, signature):
    begin = text.index(signature)
    return text[begin:text.index(';', begin) + 1] + '\n'


peer_menu = (src / 'window/window_peer_menu.cpp').read_text()
chat_menus = (src / 'purple/purple_chat_menus.cpp').read_text()
list_menu = (src / 'purple/purple_list_menu.cpp').read_text()
list_header = (src / 'purple/purple_list_menu.h').read_text()

history = extract(peer_menu, 'void Filler::fillHistoryActions()')
mute = extract(peer_menu, 'void Filler::addToggleMuteSubmenu(')
context = extract(peer_menu, 'void Filler::fillContextMenuActions()')
assert 'Purple::AddPeerListsSubmenu(' in context, 'the chat list context menu no longer offers the Work Mode row'

# Every other row these two fillers add is recorded by name, so the test sees
# the whole order without depending on what each upstream row does.
extracted = {'AddHistoryListsSubmenu', 'AddPeerListsSubmenu'}
members = sorted(set(re.findall(r'^\s*(add\w+)\(\);', history, re.M)))
purple_calls = sorted(set(re.findall(r'Purple::(\w+)\(', history)) - extracted)
(work / 'history-members.inc').write_text(''.join(
    'void ' + name + '() { Record("' + name + '"); }\n' for name in members))
(work / 'history-purple-stubs.inc').write_text(''.join(
    'template <typename ...Args> void ' + name + '(Args&&...) { Record("' + name + '"); }\n'
    for name in purple_calls))
(work / 'history-fillers.inc').write_text(history + mute)
(work / 'history-lists.inc').write_text(
    declaration(list_header, 'void AddListsSubmenu(')
    + extract(list_menu, 'void AddListsSubmenu(')
    + extract(chat_menus, 'void AddPeerListsSubmenu(')
    + extract(chat_menus, 'void AddHistoryListsSubmenu('))

# The row's tick goes through the same write and refusal as the context
# menu's, so a missing settings owner leads to the owner chooser there too.
fill = extract(list_menu, 'void FillListsMenu(')
assert 'SetAccountListMember(' in fill, 'list ticks no longer write through SetAccountListMember'
assert 'ShowAccountSettingsRefusal(show, result.error' in fill, 'a refused list tick no longer offers the owner chooser'

binary = work / 'test-history-menu-lists'
subprocess.run(['xcrun', 'clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
    '-I' + str(work), str(repo / 'purple/test_history_menu_lists.cpp'), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
