#!/usr/bin/env python3
# T5J wiring routes: with all five carry switches OFF, startup runs ordinary reconciliation alone,
# the setup screen never constructs the carry service, and every carry control keeps its OFF result.
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
root = repo / 'Telegram/SourceFiles/purple'
paths = {
    'owner': root / 'purple_sync_chat_carry_owner.cpp',
    'host': root / 'purple_sync_chat_carry_service.cpp',
    'service': root / 'purple_sync_chat_service.cpp',
    'ui': root / 'purple_sync_chat_setup_box.cpp',
    'setup': root / 'purple_sync_chat_setup.cpp',
    'consent': root / 'purple_sync_chat_consent.cpp',
    'recovery': root / 'purple_sync_chat_recovery.cpp',
}
sources = {name: path.read_text() for name, path in paths.items()}


def body(text, marker):
    start = text.index('{', text.index(marker))
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


guards = [
    # Each switch is its own constant; the owner admits nothing its switch holds.
    ('owner', 'case DesktopSyncChatCarrySwitch::Move: return DesktopSyncChatMoveActivated();'),
    ('owner', 'case DesktopSyncChatCarrySwitch::FollowFill: return DesktopSyncChatFollowFillActivated();'),
    ('owner', 'case DesktopSyncChatCarrySwitch::MarkRemoval: return DesktopSyncChatMarkRemovalActivated();'),
    ('owner', 'case DesktopSyncChatCarrySwitch::Cleanup: return DesktopSyncChatCleanupActivated();'),
    ('owner', 'case DesktopSyncChatCarrySwitch::Recovery: return DesktopSyncChatRecoveryActivated();'),
    ('owner', 'if (kind != DesktopSyncChatCarryKind::Resume && !_host.activated(gate)) { return false; }'),
    ('owner', '!(_host.activated(DesktopSyncChatCarrySwitch::Move) || _host.activated(DesktopSyncChatCarrySwitch::Recovery)'),
    ('owner', '_active->run->cleanup = _host.activated(DesktopSyncChatCarrySwitch::Cleanup);'),
    # Follow/Fill and recovery renew each effect against their controller's own engine.
    ('owner', '_host.transport(opening, controllerRenewal(_generation, live)'),
    ('owner', 'ready(valid() ? std::move(result) : std::nullopt);'),
    ('consent', 'work(self && SyncStoreWorkerThread() && self->live() ? self->engine.get() : nullptr);'),
    ('recovery', 'work(self && SyncStoreWorkerThread() && self->live() ? self->engine.get() : nullptr);'),
    # Startup: OFF is ordinary reconciliation alone.
    ('host', 'if (!ResumeActivated()) { if (then) { then(); } return; }'),
    ('host', 'if (!ResumeActivated()) {\n\t\tif (then) { then(); }\n\t\treturn;\n\t}\n\tSyncChatCarry(session).startup(std::move(then));'),
    ('host', 'if (unfinished && resumeLater && ResumeActivated())'),
    ('service', 'ResumeSyncChatCarry(session, [=] {\n\t\t\t\t\t\t\tEnsureSyncChatResolved(session);'),
    # The setup screen constructs the carry service only behind CarryOn().
    ('ui', 'return (session && CarryOn()) ? &SyncChatCarry(session).owner() : nullptr;'),
    ('ui', 'if (!strong || !CarryOn()) { return nullptr; }'),
    ('ui', 'if (CarryOn()) {\n\t\t// The box owns what it starts'),
    ('ui', 'if (live && CarryOn()) { SyncChatCarry(live).owner().closeWindow('),
    ('ui', 'if (ResumeOn()) {\n\t\tstate->resume = box->addRow('),
    # Held only where its switch is OFF.
    ('setup', 'if (!DesktopSyncChatMoveActivated()) {\n\t\t_reason = u"move-qualification-held"_q;\n\t\treturn true;\n\t}'),
    ('ui', 'if (accepted && !draft && state->recovery) { draft = state->move->reviewed(); }'),
    ('ui', ': !draft ? MoveNotice(DesktopSyncChatMoveNotice::Held)'),
    ('ui', 'if (DesktopSyncChatMoveActivated() || state->recovery) { refresh(); } else {'),
    ('ui', 'DesktopSyncChatCleanupActivated() ? tr::lng_purple_chat_stale(tr::now)'),
]


def validate(values):
    for name, token in guards:
        assert token in values[name], (name, token)
    ui = values['ui']
    assert 'Unrenewable' not in values['owner']
    # Every construction site of the carry service is behind CarryOn().
    index = ui.find('SyncChatCarry(')
    while index >= 0:
        assert 'CarryOn()' in ui[max(0, index - 240):index], ('unguarded carry service', ui.count('\n', 0, index) + 1)
        index = ui.find('SyncChatCarry(', index + 1)
    # The old Held labels for mark removal and repair are gone; an ON control reports progress.
    assert 'lng_purple_chat_remove_mark_held' not in ui
    assert 'QString::number(quintptr(' not in ui
    held = [line for line in ui.splitlines() if 'lng_purple_chat_move_held' in line]
    assert len(held) == 5, held
    # The setup screen talks to the owner only; it never runs a controller itself.
    for forbidden in ['DesktopSyncChatConsentFlow', 'DesktopSyncChatRecovery(', 'DesktopSyncChatMarkActions', 'DesktopSyncChatCarry(']:
        assert forbidden not in ui, forbidden
    resume = body(values['host'], 'void ResumeSyncChatCarry(')
    assert resume.index('ResumeActivated()') < resume.index('SyncChatCarry(session)')


validate(sources)
for name, token in guards:
    changed = dict(sources)
    changed[name] = changed[name].replace(token, '')
    try:
        validate(changed)
    except (AssertionError, ValueError):
        continue
    raise AssertionError(('negative mutation escaped', name, token))
for line in ['\tSyncChatCarry(session).changes();\n', '\tconst auto held = tr::lng_purple_chat_move_held(tr::now);\n']:
    changed = dict(sources)
    changed['ui'] = '\n' * 8 + line + changed['ui']
    try:
        validate(changed)
    except AssertionError:
        continue
    raise AssertionError(('negative insertion escaped', line))
print(f'T5J wiring routes: {len(guards)} guards and {len(guards)} negative mutations, 2 insertion negatives PASS; carry service OFF-unconstructed')
