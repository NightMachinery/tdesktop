#!/usr/bin/env python3
import json
from pathlib import Path
r=Path(__file__).resolve().parent.parent/'Telegram/SourceFiles/purple'
files={name:(r/name).read_text() for name in ['purple_sync_chat_service.cpp','purple_sync_chat_discovery.cpp','purple_sync_chat_cache.cpp','purple_sync_chat_requests.cpp','purple_sync_chat_write.cpp']}
def check(v):
 s=v['purple_sync_chat_service.cpp'];d=v['purple_sync_chat_discovery.cpp'];c=v['purple_sync_chat_cache.cpp'];q=v['purple_sync_chat_requests.cpp'];w=v['purple_sync_chat_write.cpp']
 result=[]
 def require(name,value):
  if not value:result.append(name)
 require('before-post-path','For(session).Ensure(true, std::move(done), std::move(operation), true);' in s)
 require('busy-post-held','if (operation.current || beforePost)' in s)
 require('scope-from-disk','disk.settingsSpace, disk.settingsBound, disk.settingsChat' in s)
 require('complete-save-stamp','if (saved && cache && self->_status.complete' in s and '&& !self->_status.postingHeld)' in s)
 require('direct-no-extension','if (!self->_directBeforePost) {\n\t\t\t\t\tself->_recent =' in s)
 require('expiry-recheck',s.count('ReuseDesktopSyncChatDiscovery(*self->_recent, scope, now)')==2 and 'self->Begin(false);\n\t\t\t\t\treturn;' in s)
 require('cancel-invalidates','_recent.reset();' in s.split('void SyncChatSession::Cancel(QString reason) {',1)[1].split('bool SyncChatSession::Accept(',1)[0])
 require('full-info-before-r3','Peer(command,' in s.split('void SyncChatSession::Exact(',1)[1].split('void SyncChatSession::Run(',1)[0])
 require('age-and-scope','now - receipt.completedAt < 15 * 60' in c and 'receipt.scope == current' in c and 'now >= receipt.completedAt' in c)
 require('direct-r3','void DesktopSyncChatDiscovery::StartBeforePost(' in d and 'Issue(DesktopSyncChatStep::ExactMark, effective->chat, effective);' in d)
 require('unknown-held','resolved.action == SyncChatAction::Use && resolved.readers.populationUnknown' in d and 'resolved.postingHeld = true;' in d)
 require('prior-consent-known','_cache->readerConsent && !resolved.readers.populationUnknown' in d)
 require('sticky-warning','next.newReaders = previous->newReaders' in d and 'NewDesktopSyncChatReaders(*previous, *facts)' in d)
 require('inventory-1000','if (_pages >= kDesktopSyncChatInventoryPages)' in q and q.index('if (_pages >= kDesktopSyncChatInventoryPages)')<q.index('if (messages->empty() || (_total && after == *_total))'))
 require('r1-20','if (_pages >= 20)' in q.split('DesktopSyncChatSearchPages::DesktopSyncChatSearchPages(',1)[1])
 require('fresh-compat-known','!DesktopSyncChatEligibility(fresh.peer)->readers.populationUnknown' in w and '== DesktopSyncChatEligibility(fresh.peer)->readers' not in w)
 return result
assert not check(files),check(files)
controls=[
 ('purple_sync_chat_service.cpp','For(session).Ensure(true, std::move(done), std::move(operation), true);','Other();','before-post-path'),
 ('purple_sync_chat_service.cpp','if (operation.current || beforePost)','if (operation.current)','busy-post-held'),
 ('purple_sync_chat_service.cpp','disk.settingsSpace, disk.settingsBound, disk.settingsChat','QString(), disk.settingsBound, disk.settingsChat','scope-from-disk'),
 ('purple_sync_chat_service.cpp','if (saved && cache && self->_status.complete','if (saved && cache','complete-save-stamp'),
 ('purple_sync_chat_service.cpp','if (!self->_directBeforePost) {\n\t\t\t\t\tself->_recent =','if (true) {\n\t\t\t\t\tself->_recent =','direct-no-extension'),
 ('purple_sync_chat_service.cpp','self->Begin(false);\n\t\t\t\t\treturn;','return;','expiry-recheck'),
 ('purple_sync_chat_service.cpp','_recent.reset();','RetainRecent();','cancel-invalidates'),
 ('purple_sync_chat_service.cpp','Peer(command,','Other(command,','full-info-before-r3'),
 ('purple_sync_chat_cache.cpp','now - receipt.completedAt < 15 * 60','true','age-and-scope'),
 ('purple_sync_chat_discovery.cpp','Issue(DesktopSyncChatStep::ExactMark, effective->chat, effective);','Search(DesktopSyncChatSearch::Global);','direct-r3'),
 ('purple_sync_chat_discovery.cpp','resolved.postingHeld = true;','resolved.postingHeld = false;','unknown-held'),
 ('purple_sync_chat_discovery.cpp','_cache->readerConsent && !resolved.readers.populationUnknown','_cache->readerConsent','prior-consent-known'),
 ('purple_sync_chat_discovery.cpp','next.newReaders = previous->newReaders','next.newReaders = false','sticky-warning'),
 ('purple_sync_chat_requests.cpp','if (_pages >= kDesktopSyncChatInventoryPages)','if (_pages >= 20)','inventory-1000'),
 ('purple_sync_chat_requests.cpp','if (_pages >= 20)','if (_pages >= 1000)','r1-20'),
 ('purple_sync_chat_write.cpp','!DesktopSyncChatEligibility(fresh.peer)->readers.populationUnknown','true','fresh-compat-known')]
for name,before,after,expected in controls:
 v=dict(files);assert before in v[name];v[name]=v[name].replace(before,after)
 try: failures=check(v)
 except ValueError: failures=['inventory-1000']
 assert expected in failures,(expected,failures)
print(json.dumps({'guards':16,'negative_controls':len(controls),'failures':0}))
