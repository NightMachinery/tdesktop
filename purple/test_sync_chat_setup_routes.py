#!/usr/bin/env python3
from pathlib import Path
import re
repo = Path(__file__).resolve().parent.parent
rules = [
 ('Telegram/SourceFiles/purple/purple_sync_chat_setup_box.cpp', 'DesktopSyncChatSetupEligibility(', 'chooser uses shared eligibility'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_setup.cpp', 'DesktopSyncChatEligibility(peer)', 'shared core acceptance predicate'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_setup_box.cpp', 'AcceptDesktopSyncChatSetup(*state->opening,', 'final confirmation validates opening snapshot'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_setup_box.cpp', 'SameDesktopSyncChatDisclosure(*peer, *acceptedPeer)', 'fresh consent snapshot comparison'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_setup_box.cpp', 'ValidateAccountSettingsOpenTarget(session, *state.target)', 'account-settings r3 authority'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_setup_box.cpp', 'std::make_unique<DesktopSyncChatSetupService>', 'confirmation enters worker-owned setup'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_peer_request.cpp', 'self->Current()', 'full-info callbacks revalidate authority'),
 ('Telegram/SourceFiles/purple/purple_sync_retry.h', '_deadline(nowMs + 30000)', 'transient-aware full-info request deadline'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_peer_request.cpp', 'SyncRequestSender _api;', 'full-info shares transient controller'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_service.cpp', '_peerRequest.reset();', 'discovery cancels full-info requests'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_setup_box.cpp', 'lng_purple_chat_refused_mark', 'refused-mark line'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_setup_box.cpp', 'lng_purple_chat_new_readers', 'persistent C6 presentation'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_setup_box.cpp', 'lng_purple_chat_settings_disclosure', 'per-account settings disclosure'),
]
for path, sentinel, label in rules:
    source = (repo / path).read_text()
    assert sentinel in source, label
    assert sentinel not in source.replace(sentinel, ''), 'negative guard did not fail: ' + label
ui = (repo / rules[0][0]).read_text()
for forbidden in ['MTPchannels_CreateChannel', 'MTPmessages_SendMessage', 'DesktopSyncChatSetupExecutor', 'MTPmessages_UpdatePinnedMessage']:
    assert forbidden not in ui, 'direct outward route in setup UI: ' + forbidden
request = (repo / 'Telegram/SourceFiles/purple/purple_sync_chat_peer_request.cpp').read_text()
assert '_timeout' not in request, 'outer timeout must not finalize a transient full-info request'
assert request.count('_api.request(') == 3
assert all(name in request for name in ['MTPusers_GetFullUser', 'MTPmessages_GetFullChat', 'MTPchannels_GetFullChannel'])
assert 'status.reason;' not in ui
print(f'T3A routes {len(rules)} guards + {len(rules)} negative, service-only UI boundaries PASS')

assert 'scope.owner ==' not in ui
assert 'Readers(DesktopSyncChatEligibility(*peer)->readers, chosen.title)' in ui
assert '+ u"\\n"_q + SettingsDisclosure(chosen.title)' in ui
strings = (repo / 'Telegram/Resources/langs/purple.strings').read_text()
for text in ['your settings file, which lists chats and people by id and name, including chats from your other accounts that you added to Work Mode lists', 'every signed-in session of this account can read it. state.toml stays on this device.']:
    assert text in strings
assert 'For the settings home account' not in strings


def temporary_input_accessors(source):
    factory = r"\b(?:input|inputChannel|inputUser|inputChat|inputPeer|inputDocument|mtpInput)"
    return list(re.finditer(factory + r"\s*\([^;{}]*?\)\s*\.\s*(?:c_[A-Za-z_]\w*|data)\s*\(", source))


sources = sorted((repo / 'Telegram/SourceFiles/purple').rglob('*'))
checked = 0
for path in sources:
    if path.suffix not in {'.cpp', '.h', '.mm'}:
        continue
    checked += 1
    source = path.read_text()
    for match in temporary_input_accessors(source):
        line = source.count('\n', 0, match.start()) + 1
        raise AssertionError(f'{path.relative_to(repo)}:{line}: retain the input value before accessing its data')

negative = [
    'const auto &data = peer->input().c_inputPeerChannel();',
    'const auto &data = peer->inputChannel().c_inputChannel();',
    'const auto &data = peer->mtpInput().data();',
    'const auto &data = peer->inputUser().c_inputUser();',
    'const auto &data = peer->inputDocument().c_inputDocument();',
    'const auto &data = peer->input(\n).\n c_inputPeerChannel();',
    'const auto &data = (peer->input()).c_inputPeerChannel();',
]
positive = [
    'const auto input = peer->input(); const auto &data = input.c_inputPeerChannel();',
    'const auto &input = peer->input(); const auto &data = input.c_inputPeerChannel();',
    'const auto &data = full.vparticipants().c_chatParticipants();',
    'const auto &data = data.vchats().v.front().c_channel();',
]
for source in negative:
    assert temporary_input_accessors(source), 'temporary-input negative guard missed a regression'
for source in positive:
    assert not temporary_input_accessors(source), 'named input or borrowed TL accessor rejected'
print(f'Temporary input lifetimes: {checked} Purple sources, {len(negative)} negative and {len(positive)} safe controls PASS')

move = (repo / 'Telegram/SourceFiles/purple/purple_sync_chat_setup.cpp').read_text()
service = (repo / 'Telegram/SourceFiles/purple/purple_sync_chat_service.cpp').read_text()
for sentinel in ['DesktopSyncChatMoveEligibility(*state->opening, *peer)',
                 'ReviewSyncChatMove(', 'offer->cleanup', 'confirmation->boxClosing()',
                 'state->move->dismiss(offer->ticket)', 'lng_purple_chat_move_disclosure']:
    assert sentinel in ui, sentinel
    assert sentinel not in ui.replace(sentinel, ''), 'move negative route failed'
assert 'bool DesktopSyncChatMoveActivated() { return false; }' in move
review = service[service.index('bool ReviewSyncChatMove('):service.index('void StartSyncChatDiscovery()')]
for forbidden in ['DesktopSyncChatCarryService', 'DesktopSyncChatSetupService', 'crl::', '.send(', 'Write', 'ReadSyncPrivateFile']:
    assert forbidden not in review, 'qualification review dispatches or performs IO'
assert 'action.confirm(offer, *fresh, std::move(peer), cleanup, consent, std::move(start), removeOldMark)' in review
assert 'status.reason;' not in ui
print('T5d move UI: shared eligibility, exact cleanup/confirmation, close revocation and qualification service no-dispatch PASS')

# With Move or Follow/Fill ON, the setup box says what the user can do instead of the OFF wording.
for sentinel in ['DesktopSyncChatFollowFillActivated() ? tr::lng_purple_chat_follow_required(tr::now)',
                 'DesktopSyncChatMoveActivated()\n\t\t\t? tr::lng_purple_chat_change_unavailable(tr::now)']:
    assert sentinel in ui, sentinel
    assert sentinel not in ui.replace(sentinel, ''), 'switch-aware text negative failed'
strings = (repo / 'Telegram/Resources/langs/purple.strings').read_text()
held_words = r'disabled|not available|qualification|this build'
for key in ['lng_purple_chat_follow_required', 'lng_purple_chat_change_unavailable', 'lng_purple_chat_carry_unavailable']:
    value = re.search('"' + key + '" = "([^"]*)";', strings).group(1)
    assert not re.search(held_words, value, re.I), key
assert re.search(held_words, re.search('"lng_purple_chat_change_disabled" = "([^"]*)";', strings).group(1)), 'held-word negative failed'
print('T5d switch-aware texts: Follow required, Change unavailable and held carry say nothing about a held build PASS')

# A mark check that can end in finish-for-Follow never claims the move or the Follow happened: the finish
# texts say the settings were carried and ask for the ordinary Follow.
claims = r'Following that chat|Sync follows|Sync continues|Finishing the move'
ftf = {key: re.search('"' + key + '" = "([^"]*)";', strings).group(1) for key in [
    'lng_purple_chat_mark_recovery_running', 'lng_purple_chat_mark_recovery_follow',
    'lng_purple_chat_mark_recovery_unconfirmed', 'lng_purple_chat_mark_recovery_old']}
assert ftf['lng_purple_chat_mark_recovery_running'] == 'Checking whether the move to {chat} finished.'
for key, value in ftf.items():
    assert not re.search(claims, value), key
    if key != 'lng_purple_chat_mark_recovery_running':
        assert 'settings were carried' in value and 'newest settings' in value, key
        assert value.endswith('Open Sync chat and press Follow to use it.') or value.endswith(
            'Open Sync chat and press Follow to use it. You can move again after that.'), key
for old in ['Another device moved sync to {chat}. Following that chat.', 'The move could not be confirmed. Sync follows {chat}.',
            'The move did not finish. Sync continues in {chat}. You can move again.', 'Finishing the move to {chat}.']:
    assert re.search(claims, old), 'FtF claim negative failed'
assert 'tr::lng_purple_chat_mark_recovery_running(tr::now, lt_chat, target)' in ui
print('FtF texts: the check and the finish never claim a move or Follow that did not happen, 4 negatives PASS')


def move_cleanup_warning(source, target):
    begin = source.index('void AcceptMove(')
    end = source.index('\nvoid Accept(', begin)
    body = source[begin:end]
    checkbox = body.index('const auto cleanup = confirmation->addRow(')
    submit = body.index('confirmation->addButton(tr::lng_continue()')
    adjacent = body[checkbox:submit]
    expected = '''if (OffersCleanup(*state) && state->opening->scope.settingsChat == PeerToken{}) {
            confirmation->addRow(object_ptr<Ui::FlatLabel>(confirmation,
                tr::lng_purple_chat_move_cleanup_self_warning(tr::now), st::boxLabel));
        }'''
    compact = lambda text: re.sub(r'\s+', '', text)
    assert compact(expected) in compact(adjacent), target + ': missing old-Self warning beside cleanup'
    assert adjacent.count('if (') == 1, target + ': warning must not depend on destination choice'
    assert 'selection.choice' not in adjacent and 'chosen.' not in adjacent, target
    return True


for target in ('Create', 'Existing'):
    assert move_cleanup_warning(ui, target)
    warning = 'tr::lng_purple_chat_move_cleanup_self_warning(tr::now)'
    for broken in (
            ui.replace(warning, 'QString()'),
            ui.replace('state->opening->scope.settingsChat == PeerToken{}', 'offer->selection.peer->chat == PeerToken{}'),
            ui.replace('state->opening->scope.settingsChat == PeerToken{}',
                       'state->opening->scope.settingsChat == PeerToken{} && offer->selection.choice == DesktopSyncChatSetupChoice::Create'),
            ui.replace('state->opening->scope.settingsChat == PeerToken{}',
                       'state->opening->scope.settingsChat == PeerToken{} && offer->selection.choice == DesktopSyncChatSetupChoice::Existing')):
        try:
            move_cleanup_warning(broken, target)
        except AssertionError:
            pass
        else:
            raise AssertionError(target + ': cleanup-warning negative guard passed')
assert 'Older Purple builds still read settings from Saved Messages.' in strings
assert 'Removing these messages may make them pause or offer to start over.' in strings
print('T5d FIX1 old-Self cleanup warning: Create and Existing, 8 negative controls PASS')
