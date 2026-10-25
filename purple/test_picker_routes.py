#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--output-dir', type=Path, required=True)
parser.add_argument('--build-only', action='store_true')
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
src = repo / 'Telegram/SourceFiles'
work = args.output_dir.resolve()
work.mkdir(parents=True, exist_ok=True)
source = (src / 'purple/purple_picker.cpp').read_text()
header = (src / 'purple/purple_picker.h').read_text()


def extract(text, signature):
    begin = text.index(signature)
    position = text.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (text[position] == '{') - (text[position] == '}')
        position += 1
    return text[begin:position] + '\n'


def method(file, signature):
    return extract((src / file).read_text(), signature)


(work / 'routes-state.inc').write_text(source[source.index('struct RowsState'):source.index('void RefreshRows(')])
(work / 'routes-owned.inc').write_text(''.join(extract(source, signature) for signature in (
    'void RefreshRows(', 'std::function<bool()> PickerAction(', 'bool PickerActionCurrent(',
    'object_ptr<Ui::GenericBox> PickerConfirmBox(', 'void WatchPickerChild(',
    'void RecipientPickerBridge::WatchRows(', 'void RecipientPickerBridge::WatchContacts(',
    'bool RecipientPickerBridge::RowAllows(', 'std::function<bool()> RecipientPickerBridge::BotRights(',
    'bool RecipientPickerBridge::SearchAllows(', 'void RecipientPickerBridge::BeforeRemove(')))
(work / 'routes-templates.inc').write_text(header[header.index('template <typename Box>'):header.index('template <typename T, typename Visible>')])
parts = [method('boxes/peers/choose_peer_box.cpp', 'void ChoosePeerBoxController::rowClicked('),
    method('boxes/peer_list_controllers.cpp', 'void ChooseTopicBoxController::rowClicked('),
    method('boxes/peer_list_controllers.cpp', 'void ChooseSublistBoxController::rowClicked('),
    method('boxes/peers/add_bot_to_chat_box.cpp', 'void Controller::rowClicked(').replace('void Controller::', 'void AuxController::', 1),
    method('boxes/star_gift_box.cpp', 'void Controller::rowClicked(').replace('void Controller::', 'void StarController::', 1)]
credit = (src / 'boxes/gift_credits_box.cpp').read_text()
parts.append(extract(credit, 'void rowClicked(not_null<PeerListRow*> row) override').replace('void rowClicked(', 'void FrequentController::rowClicked(', 1).replace(' override', '', 1))
credit = credit[credit.index('void ShowGiftCreditsBox('):]
parts.append(extract(credit, 'void rowClicked(not_null<PeerListRow*> row) override').replace('void rowClicked(', 'void CreditController::rowClicked(', 1).replace(' override', '', 1))
for signature in ('void Controller::prepare(', 'void Controller::addRow('):
    parts.append(method('boxes/peers/add_bot_to_chat_box.cpp', signature).replace('void Controller::', 'void AuxController::', 1))
custom = (src / 'boxes/star_gift_box.cpp').read_text()
custom = custom[custom.index('class CustomController final'):]
parts.append(extract(custom, 'void prepare() override').replace('void prepare()', 'void CustomController::prepare()', 1).replace(' override', '', 1))
frequent = (src / 'boxes/gift_credits_box.cpp').read_text()
parts.append(extract(frequent, 'void prepare() override').replace('void prepare()', 'void FrequentController::prepare()', 1).replace(' override', '', 1))
(work / 'routes-native.inc').write_text('\n'.join(parts))
bot = (src / 'boxes/peers/add_bot_to_chat_box.cpp').read_text()
(work / 'routes-bot-callback.inc').write_text(extract(bot, 'auto callback = crl::guard(this, [=] {') + ');\n')

community = (src / 'boxes/peer_list_controllers.cpp').read_text()
community = community[community.index('void ChooseCommunityChatBoxController::rowClicked('):]
(work / 'routes-community.inc').write_text(extract(community, 'auto callback = [=](not_null<Data::Thread*> thread)') + ';\n')
drops = (src / 'window/window_peer_menu.cpp').read_text()
drops = drops[drops.index('base::weak_qptr<Ui::BoxContent> ShowDropMediaBox('):]
first = drops.index('auto chosen = [')
second = drops.index('auto chosen = [', first + 1)
(work / 'routes-drop-topic.inc').write_text(extract(drops[first:], 'auto chosen = [') + ';\n')
(work / 'routes-drop-sublist.inc').write_text(extract(drops[second:], 'auto chosen = [') + ';\n')

admin = (src / 'boxes/peers/edit_participants_box.cpp').read_text()
(work / 'routes-admin.inc').write_text(admin[admin.index('void SaveChatAdmin('):admin.index('void SaveChannelAdmin(')])

gift = (src / 'boxes/star_gift_box.cpp').read_text()
gift = gift[gift.index('void SendGiftBoxWithShow('):]
a = gift.rindex('if (!show->valid()) return;', 0, gift.index('const auto copy = state->media;'))
b = gift.index('SendGift(window, peer, api, details, done, show);', a) + len('SendGift(window, peer, api, details, done, show);')
(work / 'gift-submit.inc').write_text(gift[a:b] + '\n')
(work / 'gift-final-stars.inc').write_text(method('boxes/send_credits_box.cpp', 'void SendStarsForm('))
(work / 'gift-scope.inc').write_text(source[source.index('class RecipientGiftShow final'):source.index('struct RecipientState')])
(work / 'gift-owned.inc').write_text(''.join(extract(source, signature) for signature in (
    'std::function<bool()> PickerAction(', 'void WatchPickerChild(', 'bool PickerPaymentAllowed(', 'void WatchPickerPayment(')))
(work / 'gift-current.inc').write_text(extract(source, 'std::function<bool()> PickerGiftCurrent('))

inventory = {'boxes/peers/choose_peer_box.cpp': {'Purple::PickerConfirmBox(peer, {': 1,
                                     'if (!Purple::PickerVisible(peer))': 2},
 'boxes/peers/add_bot_to_chat_box.cpp': {'RecipientPickerBridge::WatchRows(this)': 1,
                                         'RecipientPickerBridge::BotRights(this, channel)': 1,
                                         'Purple::PickerConfirmBox(chat, {': 1,
                                         'Purple::WatchPickerChild(box.data(), chat)': 1,
                                         'const auto current = Purple::PickerAction(chat, lifetime())': 1,
                                         'if (!current())': 5},
 'boxes/peers/edit_participants_box.cpp': {'Purple::PickerActionCurrent(current,': 6,
                                           'Purple::WatchPickerBox(': 1},
 'boxes/peer_list_controllers.cpp': {'RecipientPickerBridge::WatchRows(this, [=] { refreshRows(true); })': 1,
                                     'Purple::PickerChildAllowed(_filter,': 3,
                                     'Purple::PickerVisible(thread->peer())': 4,
                                     'Purple::WatchPickerChild(owned.data(), peer)': 4},
 'window/window_peer_menu.cpp': {'Purple::WatchPickerChild(box, forum->history()->peer)': 1,
                                 'Purple::WatchPickerChild(box, monoforum->owningHistory()->peer)': 1,
                                 'Purple::PickerFilter<Data::Thread>()': 1,
                                 'Purple::PickerFilter<Data::SavedSublist>()': 1,
                                 'if (!Purple::PickerVisible(thread->peer()))': 1,
                                 'if (!Purple::PickerVisible(sublist->peer()))': 1},
 'boxes/star_gift_box.cpp': {'RecipientPickerBridge::WatchContacts(this)': 1,
                             'RecipientPickerBridge::WatchRows(this,': 1,
                             'Purple::PickerCallback(std::move(pick))': 1,
                             'Purple::PickerAction(peer, i->second.lifetime,': 1,
                             'Purple::PickerActionCurrent(current, peer)': 1,
                             'Purple::WatchPickerBox(': 1,
                             'ShowStarGiftBox(window, peer, true)': 1},
 'boxes/gift_credits_box.cpp': {'RecipientPickerBridge::WatchContacts(this)': 1,
                                'RecipientPickerBridge::WatchRows(this,': 1,
                                'Purple::PickerCallback(std::move(choose))': 1,
                                'Purple::WatchPickerBox(': 1}}
inventory.update({
 'boxes/star_gift_auction_box.cpp': {'SendGiftBoxWithShow,': 2, '.show = show,': 1, 'boxClosed, show);': 1, 'ChooseAndShowAuctionBox(window, peer, state, close, show)': 1},
 'boxes/star_gift_resale_box.cpp': {'.recipientShow = recipientShow,': 1, 'std::move(info), show)': 1},
 'boxes/transfer_gift_box.cpp': {'if (!show->valid()) return;': 1, 'done, true, show)': 1, 'done, false, show)': 1},
 'payments/payments_form.cpp': {'Purple::PickerPaymentAllowed(_id)': 3},
 'payments/payments_form.h': {'std::function<bool()> pickerCurrent;': 3},
 'payments/payments_checkout_process.cpp': {'Purple::WatchPickerPayment(id,': 1},
 'boxes/send_credits_box.cpp': {'Purple::PickerPaymentAllowed(data->id)': 1},
 'payments/payments_non_panel_process.cpp': {'recipientShow ? recipientShow : controller->uiShow()': 1},
 'settings/settings_credits_graphics.cpp': {'.pickerCurrent = Purple::PickerGiftCurrent(show)': 1}})
inventory['boxes/star_gift_box.cpp'].update({'SendGiftBoxWithShow,': 1, '.pickerCurrent = Purple::PickerGiftCurrent(show)': 3, 'invoice.pickerCurrent = Purple::PickerGiftCurrent(show)': 1, 'if (!show->valid()) return;': 6, 'Purple::PickerGiftShow(window->uiShow(), peer)': 1, '.show = show,': 2})
negative = 0
for file, expected in inventory.items():
    text = (src / file).read_text()
    def verify(candidate):
        for guard, count in expected.items():
            if not count or candidate.count(guard) != count:
                raise RuntimeError(f'Changed route guard: {file}: {guard}')
    verify(text)
    for guard in expected:
        try:
            verify(text.replace(guard, '', 1))
        except RuntimeError:
            negative += 1
        else:
            raise RuntimeError(f'Negative inventory probe escaped: {file}: {guard}')

factories = {
    'boxes/share_box.cpp': 3,
    'boxes/peer_list_controllers.cpp': 4,
    'window/window_peer_menu.cpp': 2,
}
for file, count in factories.items():
    text = (src / file).read_text()
    actual = sum(text.count('std::make_unique<' + name + '>') for name in (
        'ChooseTopicBoxController', 'ChooseSublistBoxController', 'ChooseCommunityChatBoxController'))
    if actual != count:
        raise RuntimeError(f'Recipient child inventory changed: {file}: {actual}')
content = (src / 'boxes/peer_list_box.cpp').read_text()
if content.count('RecipientPickerBridge::RowAllows(this, row.get())') != 3:
    raise RuntimeError('All three recipient row admission hooks are required')

build = args.build_dir.resolve()
target = 'Telegram/CMakeFiles/Telegram.dir/SourceFiles/purple/purple_config_file.cpp.o'
recipes = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', target], text=True)
recipe = next(shlex.split(line) for line in recipes.splitlines() if ' -c ' in line and 'purple_config_file.cpp' in line)
flags = ['-std=c++20', '-I', str(work)]
for flag in ('-arch', '-isysroot', '-isystem'):
    for index, value in enumerate(recipe):
        if value == flag:
            flags += [flag, recipe[index + 1]]
for value in recipe:
    if value.startswith(('-I', '-D', '-F', '-mmacosx-version-min=')):
        flags.append(value)
frameworks = next(Path(value[2:]) for value in flags if value.startswith('-F') and (Path(value[2:]) / 'QtCore.framework').exists())
with (work / 'build.log').open('w') as log:
    subprocess.run([recipe[0], *flags, str(repo / 'purple/test_picker_routes.cpp'), str(build / 'Telegram/lib_base/liblib_base.a'),
        '-Wl,-dead_strip', '-framework', 'QtCore', '-framework', 'Foundation', '-lobjc',
        '-Wl,-rpath,' + str(frameworks), '-o', str(work / 'test_picker_routes')], check=True, stdout=log, stderr=subprocess.STDOUT)
(work / 'inputs.json').write_text(json.dumps({
    'source_sha256': {str(path.relative_to(repo)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in [src / 'purple/purple_picker.cpp', src / 'purple/purple_picker.h', repo / 'purple/test_picker_routes.cpp', repo / 'purple/test_picker_gifts.cpp', *[src / file for file in inventory]]},
    'negative_inventory_cases': negative,
    'boundary': 'Exact production row reconciliation, action, confirmation, rights ownership, templates and native ChoosePeer, management, topic/sublist and gift row methods. Inert list/UI/transport ports; real weak ownership, RPL and Qt queued events. No GUI or network qualification.'}, indent=2) + '\n')
with (work / 'gift-build.log').open('w') as log:
    subprocess.run([recipe[0], *flags, str(repo / 'purple/test_picker_gifts.cpp'), str(build / 'Telegram/lib_base/liblib_base.a'),
        '-Wl,-dead_strip', '-framework', 'QtCore', '-framework', 'Foundation', '-lobjc',
        '-Wl,-rpath,' + str(frameworks), '-o', str(work / 'test_picker_gifts')], check=True, stdout=log, stderr=subprocess.STDOUT)
print(f'route inventory {negative} negative cases PASS')
if not args.build_only:
    gift_result = subprocess.run([str(work / 'test_picker_gifts')], capture_output=True, text=True)
    (work / 'gift-run.log').write_text(gift_result.stdout + gift_result.stderr)
    print(gift_result.stdout)
    if gift_result.returncode: raise SystemExit(gift_result.returncode)
    result = subprocess.run([str(work / 'test_picker_routes')], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (work / 'run.log').write_text(result.stdout)
    (work / 'run.exit').write_text(str(result.returncode) + '\n')
    print(result.stdout)
    raise SystemExit(result.returncode)
