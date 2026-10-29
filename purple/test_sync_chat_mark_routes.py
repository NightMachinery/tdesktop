#!/usr/bin/env python3
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
sources = {name: (repo / path).read_text() for name, path in {
    'actions': 'Telegram/SourceFiles/purple/purple_sync_chat_mark_actions.cpp',
    'transport': 'Telegram/SourceFiles/purple/purple_sync_chat_mark_transport.cpp',
    'service': 'Telegram/SourceFiles/purple/purple_sync_chat_service.cpp',
    'ui': 'Telegram/SourceFiles/purple/purple_sync_chat_setup_box.cpp',
}.items()}

def body(text, marker):
    start = text.index('{', text.index(marker))
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

guards = [
    ('actions', 'bool DesktopSyncChatMarkRemovalActivated() { return false; }'),
    ('actions', 'CheckStaleMarkRemoval(row.origin.account,'),
    ('actions', 'GetSyncChatMessageOwnership(MapDesktopSyncChatMessage'),
    ('actions', 'ParseSyncChatMark(text, hasMedia, own).status != SyncChatMarkStatus::Supported'),
    ('actions', '!exact.readable || !ValidDesktopSyncChatReadIo(exact.route)'),
    ('actions', 'messages->size() != 1'),
    ('actions', 'message->type() != mtpc_messageEmpty'),
    ('actions', 'store.RetainedEffects()'),
    ('actions', 'journal.phase == SyncCarryPhase::Done'),
    ('actions', 'journal.markAttempted && journal.target == row.location.chat'),
    ('actions', 'self.lease->quiescent()'),
    ('actions', 'state.ticket != run'),
    ('actions', 'state.sendTicket != run || state.acknowledged'),
    ('actions', 'state.authorize(std::move(ready))'),
    ('actions', 'state.apply(route, *affected)'),
    ('actions', 'state.verify()'),
    ('actions', 'closed.exchange(true)'),
    ('actions', 'if (released) { return; }'),
    ('actions', 'store.reset(); lease.reset();'),
    ('actions', '_claimed->exchange(true)'),
    ('transport', '.resumeEffect('),
    ('transport', 'nextWire != wire'),
    ('transport', 'renew([active,'),
    ('transport', 'onSender([active,'),
    ('service', 'const auto rows = SyncChatStaleMarksOf(session)'),
]

def validate(values):
    for name, token in guards:
        assert token in values[name], (name, token)
    ui = values['ui']
    rows = body(ui, 'void AddStaleMarkRows(')
    confirm = body(ui, 'void ConfirmStaleMark(')
    entry = body(values['service'], 'bool ReviewSyncChatStaleMark(')
    gate = '!DesktopSyncChatMarkRemovalActivated()'
    assert rows.count(gate) == 2, 'row and click guards'
    assert confirm.count(gate) == 3, 'confirmation entry, construction and positive guards'
    assert gate in entry and entry.index(gate) < entry.index('start(row)'), 'public entry guard'
    assert rows.index(gate) < rows.index('addRow'), 'no actionable OFF rows'
    assert 'CheckSyncCarryOldMarkDeletion' not in values['actions']
    assert 'Begin(' not in values['actions'] and 'StartSyncCarryJournal' not in values['actions']
    assert 'Core::App' not in values['actions']
    assert '.handleAllErrors()' not in values['transport']
    assert 'QFile(' not in values['ui'] and 'QFile(' not in entry

validate(sources)
for name, token in guards:
    mutated = dict(sources)
    mutated[name] = mutated[name].replace(token, '')
    try:
        validate(mutated)
    except (AssertionError, ValueError):
        continue
    raise AssertionError(('negative escaped', name, token))
gate = '!DesktopSyncChatMarkRemovalActivated()'
for marker, count, name in [('void AddStaleMarkRows(', 2, 'ui'), ('void ConfirmStaleMark(', 3, 'ui'), ('bool ReviewSyncChatStaleMark(', 1, 'service')]:
    original = body(sources[name], marker)
    for index in range(count):
        parts = original.split(gate)
        edited = gate.join(parts[:index + 1]) + 'false' + gate.join(parts[index + 1:])
        mutated = dict(sources); mutated[name] = mutated[name].replace(original, edited, 1)
        try:
            validate(mutated)
        except (AssertionError, ValueError):
            continue
        raise AssertionError(('UI gate negative escaped', marker, index))
print(f'T5f mark routes: {len(guards)} guards + {len(guards)} negative mutations, six OFF entry mutations PASS')
