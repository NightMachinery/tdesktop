#!/usr/bin/env python3
from pathlib import Path
import re
repo = Path(__file__).resolve().parent.parent
root = repo / 'Telegram/SourceFiles/purple'
names = ['native', 'native_io', 'native_resolver', 'carry_executor', 'config_upload']
paths = {name: root / ('purple_sync_' + ('config_upload' if name == 'config_upload' else 'chat_' + name) + '.cpp') for name in names}
paths['config_upload'] = root / 'purple_sync_config_upload.cpp'
sources = {name: path.read_text() for name, path in paths.items()}
guards = [
 ('native', 'permit.sourceMessageId()'),
 ('native', 'permit.compatible(*fresh)'),
 ('native', 'Wire(*request) == wire'),
 ('native', 'DesktopSyncChatCarryInventory(*inventory)'),
 ('native', 'SealDesktopSyncChatRecord(route,'),
 ('native', 'work->reply.sendIntent = command.pendingIntent;'),
 ('native', 'completedOnce->exchange(true)'),
 ('native', 'facts.current = [admitted = self->_admitted]'),
 ('native', 'epoch == self->_epoch'),
 ('native', 'if (!self->live(epoch)) { return; }'),
 ('native', 'self->_renewal.beginCleanup(work->command'),
 ('native', 'self->_renewal.cleanup(work->command'),
 ('native', 'self->_io.removeMark(permit, renew,'),
 ('native', 'row.login != self->_opening.scope.loginBinding'),
 ('native_resolver', 'cache->carry.reset();'),
 ('native_resolver', 'result.error == u"CHANNEL_PRIVATE"_q'),
 ('native_resolver', 'history.error == u"CHANNEL_PRIVATE"_q'),
 ('native_resolver', '_model.Complete(reply, base::unixtime::now())'),
 ('native_resolver', 'ResolveDesktopSyncNative('),
 ('native_io', 'MakeDesktopSyncCarryTransport('),
 ('native_io', 'MakeDesktopSyncMarkNativePort('),
 ('native_io', 'SendDesktopSyncChatMarkDelete(self->_api'),
 ('native_io', 'SendDesktopSyncChatCleanup(self->_api'),
 ('native_io', 'session->api().applyAffectedMessages'),
 ('native_io', 'crl::async('),
 ('native_io', 'CaptureSyncOwnedFile(path, kSyncRecordMaximumBytes)'),
 ('native_io', '.resumeEffect('),
 ('native_io', 'generation != self->_generation'),
 ('config_upload', '_carryResult = result;'),
 ('config_upload', '[this] { return RequestCurrent(); }'),
 ('config_upload', 'if (!_carry || _offset < _content.size()) { return Current(); }'),
 ('carry_executor', 'command.pendingIntent = !issued ? SyncCarrySendIntent::Unknown'),
]

def valid(text):
    return all(re.sub(r'\s+', '', token) in re.sub(r'\s+', '', text[name]) for name, token in guards) and all(
        token not in text['native_resolver'] for token in ['SaveDesktopSyncChatDisk', 'WriteSync', 'MTPmessages_SendMessage('])

assert valid(sources)
for name, token in guards:
    changed = dict(sources)
    changed[name] = re.sub(r'\s+', '', changed[name]).replace(re.sub(r'\s+', '', token), 'REMOVED_GUARD')
    assert not valid(changed), (name, token)
# The owned-file rule reads settings.toml only: no header declares it and no other source uses it, so every
# sync/ file keeps the private rule.
owned = sorted(path.name for path in root.glob('purple_*.cpp') if 'CaptureSyncOwnedFile(' in path.read_text())
assert owned == ['purple_sync_chat_native_io.cpp', 'purple_sync_local_store.cpp'], owned
assert not any('CaptureSyncOwnedFile' in path.read_text() for path in root.glob('*.h'))
# Each production switch is one constant, ON or OFF; the native guards above hold for either value.
switches = []
for name, function in [('setup', 'Move'), ('consent', 'FollowFill'), ('mark_actions', 'MarkRemoval'), ('cleanup', 'Cleanup'), ('recovery', 'Recovery')]:
    value = re.findall(r'^bool DesktopSyncChat' + function + r'Activated\(\) \{ return (true|false); \}$', (root / ('purple_sync_chat_' + name + '.cpp')).read_text(), re.M)
    assert len(value) == 1, function
    switches.append(function + ('=ON' if value[0] == 'true' else '=OFF'))
print(f'T5K native routes: {len(guards)} guards and {len(guards)} negative mutations PASS; production switches ' + ' '.join(switches))
