#!/usr/bin/env python3
import unittest
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from check_wait_routes import inventory, WRITERS


class WaitRoutes(unittest.TestCase):
    def test_cover_teardown_restores_before_hiding(self):
        root = Path(__file__).resolve().parent.parent
        source = (root / 'Telegram/SourceFiles/purple/purple_peek_controls.cpp').read_text()
        close = source.split('void PendingWait::closeCover() {', 1)[1].split('\n}', 1)[0]
        self.assertIn('CloseWaitCover(cover, focus);', close)
        self.assertNotIn('->hide()', close)
        self.assertNotIn('setInnerFocus', close)
        self.assertLess(close.index('std::exchange(_previousFocus, nullptr)'), close.index('CloseWaitCover('))
        self.assertLess(close.index('_window = nullptr'), close.index('CloseWaitCover('))
        request = source.split('if (result.outcome == WaitOutcome::Pending) {', 1)[1]
        self.assertLess(request.index('_previousFocus = QApplication::focusWidget()'), request.index('new WaitCover('))
        for signature in ('void PendingWait::cancel()', 'void PendingWait::confirm()', 'void PendingWait::render('):
            body = source.split(signature, 1)[1].split('\n}', 1)[0]
            self.assertIn('closeCover();', body)
        key = (root / 'Telegram/SourceFiles/purple/purple_wait_key_filter.cpp').read_text()
        close = key.split('void CloseWaitCover(', 1)[1].split('\n}', 1)[0]
        self.assertLess(close.index('previousFocus->setFocus'), close.index('cover->hide()'))
        self.assertLess(close.index('focused->clearFocus()'), close.index('cover->hide()'))
        self.assertNotIn('setInnerFocus', close)

    def test_called_writer_with_any_receiver(self):
        for name in WRITERS:
            for prefix in ('', 'Purple::', 'Renamed::', 'any.', 'something->', 'factory().', 'obj.template '):
                with self.subTest(name=name, prefix=prefix):
                    self.assertEqual(inventory(f'{prefix}{name} \n (input);'), {name: 1})

    def test_generic_table_splices_with_any_receiver(self):
        for name in ('SetTable', 'SetTable2', 'SetTableBool', 'SetTableString', 'SetTableInt', 'SetTableFutureKey'):
            for prefix in ('', 'Purple::', 'Renamed::', 'any.', 'something->', 'factory().'):
                with self.subTest(name=name, prefix=prefix):
                    self.assertEqual(inventory(f'{prefix}{name}(text, path, table, key, value);'), {name: 1})

    def test_option_writes_cannot_hide_behind_receivers_or_table_aliases(self):
        baseline = inventory('SetPeekStartDelay(text, path, 10); SetTableBool(text, path, "other", "key", true);')
        for extra in (
                'Alias::SetPeekStartDelay(text, path, 0);',
                'renamed.SetPeekStartDelay(text, path, 0);',
                'SetTableBool(text, path, "peek", "exit_hidden_chats_p", false);',
                'renamed.SetTableString(text, path, tableAlias, keyAlias, value);'):
            self.assertNotEqual(inventory('SetPeekStartDelay(text, path, 10); SetTableBool(text, path, "other", "key", true);' + extra), baseline)

    def test_preset_assignment(self):
        for prefix in ('', 'fresh.', 'pointer->', 'someNamespace::'):
            self.assertEqual(inventory(prefix + 'activePreset = value;'), {'activePreset=': 1})

    def test_non_calls(self):
        self.assertEqual(inventory('''
            // a.writeWorkMode(x); activePreset = x;
            /* Other::UpdateState(x); */
            auto s = "ExtendPeek(x)";
            auto raw = R"custom(StopPeek(x); activePreset = x;)custom";
            auto c = 'x';
            if (state.activePreset == value) {}
            RestartPeek();
        '''), {})

    def test_new_bypass_changes_inventory(self):
        baseline = inventory('writer.writeWorkMode(target);')
        for extra in ('writeWorkMode(other);', 'Alias::writeWorkMode(other);', 'renamed.writeWorkMode(other);'):
            self.assertNotEqual(inventory('writer.writeWorkMode(target);' + extra), baseline)

    def test_focus_preference_alias_and_literal(self):
        for key in ('"purple-peek-end-on-focus-loss"', 'kKey', '(kAlias)'):
            code = '''
                constexpr auto kKey = "purple-peek-end-on-focus-loss";
                const auto kAlias = kKey;
                void RequestPeekOptionChange(bool enabled) {
                    RequestWait({.apply = [=] { settings().writePref<bool>(KEY, enabled); }});
                }
            '''.replace('KEY', key)
            self.assertEqual(inventory(code), {'writePref<peek-focus-loss>': 1})
        self.assertEqual(inventory('''
            // settings().writePref<bool>("purple-peek-end-on-focus-loss", false);
            auto other = "writePref<bool>(key, false)";
            settings().readPref<bool>("purple-peek-end-on-focus-loss", false);
            settings().writePref<bool>("another-preference", false);
        '''), {})

    def test_focus_preference_moved_outside_gate(self):
        code = '''
            constexpr auto kKey = "purple-peek-end-on-focus-loss";
            void DirectDisable() { settings().writePref<bool>(kKey, false); }
        '''
        self.assertEqual(inventory(code), {
            'ungatedPeekFocusWrite': 1, 'writePref<peek-focus-loss>': 1})

    def test_direct_focus_disable_rejected_by_guard(self):
        with tempfile.TemporaryDirectory(prefix='purple-focus-route.') as temporary:
            repo = Path(temporary)
            source = repo / 'Telegram/SourceFiles/purple/prefs.cpp'
            source.parent.mkdir(parents=True)
            purple = repo / 'purple'
            purple.mkdir()
            baseline = '''
                constexpr auto kKey = "purple-peek-end-on-focus-loss";
                void RequestPeekOptionChange(bool enabled) {
                    RequestWait({.apply = [=] { settings().writePref<bool>(kKey, enabled); }});
                }
            '''
            source.write_text(baseline)
            (purple / 'wait_routes.json').write_text(json.dumps({
                str(source.relative_to(repo)): inventory(baseline)}))
            guard = purple / 'check_wait_routes.py'
            shutil.copyfile(Path(__file__).with_name('check_wait_routes.py'), guard)
            def run():
                return subprocess.run([sys.executable, str(guard)], capture_output=True, text=True)
            self.assertEqual(run().returncode, 0)
            source.write_text(baseline + '''
                void DirectDisable() {
                    Core::App().settings().writePref<bool>("purple-peek-end-on-focus-loss", false);
                }
            ''')
            rejected = run()
            self.assertEqual(rejected.returncode, 1, rejected.stdout)
            self.assertIn('ungatedPeekFocusWrite', rejected.stdout)
            for call in (
                    'SetPeekStartDelay(text, path, 0);',
                    'Renamed::SetPeekStartDelay(text, path, 0);',
                    'writer.SetTableBool(text, path, "peek", "exit_hidden_chats_p", false);',
                    'pointer->SetTableString(text, path, alias, key, value);'):
                source.write_text(baseline + 'void DirectOption() {' + call + '}')
                rejected = run()
                self.assertEqual(rejected.returncode, 1, rejected.stdout)


if __name__ == '__main__':
    unittest.main()
