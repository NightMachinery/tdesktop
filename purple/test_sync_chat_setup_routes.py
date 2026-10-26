#!/usr/bin/env python3
from pathlib import Path
repo = Path(__file__).resolve().parent.parent
rules = [
 ('Telegram/SourceFiles/purple/purple_sync_chat_setup_box.cpp', 'DesktopSyncChatSetupEligibility(', 'chooser uses shared eligibility'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_setup.cpp', 'DesktopSyncChatEligibility(peer)', 'shared core acceptance predicate'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_setup_box.cpp', 'AcceptDesktopSyncChatSetup(*state->opening,', 'final confirmation validates opening snapshot'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_setup_box.cpp', 'SameDesktopSyncChatDisclosure(*peer, *acceptedPeer)', 'fresh consent snapshot comparison'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_setup_box.cpp', 'ValidateAccountSettingsOpenTarget(session, *state.target)', 'account-settings r3 authority'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_setup_box.cpp', 'state->status->setText(tr::lng_purple_chat_qualification_held(tr::now))', 'confirmation is qualification-held'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_peer_request.cpp', 'self->Current()', 'full-info callbacks revalidate authority'),
 ('Telegram/SourceFiles/purple/purple_sync_chat_peer_request.cpp', '_timeout.callOnce(30000)', 'full-info request timeout'),
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
    assert forbidden not in ui, 'outward route in held UI: ' + forbidden
request = (repo / 'Telegram/SourceFiles/purple/purple_sync_chat_peer_request.cpp').read_text()
assert request.count('_api.request(') == 3
assert all(name in request for name in ['MTPusers_GetFullUser', 'MTPmessages_GetFullChat', 'MTPchannels_GetFullChannel'])
assert 'status.reason;' not in ui
print(f'T3A routes {len(rules)} guards + {len(rules)} negative, held/read-only boundaries PASS')

assert 'scope.owner ==' not in ui
assert 'Readers(DesktopSyncChatEligibility(*peer)->readers, chosen.title)' in ui
assert '+ u"\\n"_q + SettingsDisclosure(chosen.title)' in ui
strings = (repo / 'Telegram/Resources/langs/purple.strings').read_text()
for text in ['your settings file, which lists chats and people by id and name, including chats from your other accounts that you added to Work Mode lists', 'every signed-in session of this account can read it. state.toml stays on this device.']:
    assert text in strings
assert 'For the settings home account' not in strings
