#!/usr/bin/env python3
import unittest
from check_wait_routes import inventory, WRITERS


class WaitRoutes(unittest.TestCase):
    def test_called_writer_with_any_receiver(self):
        for name in WRITERS:
            for prefix in ('', 'Purple::', 'Renamed::', 'any.', 'something->', 'factory().', 'obj.template '):
                with self.subTest(name=name, prefix=prefix):
                    self.assertEqual(inventory(f'{prefix}{name} \n (input);'), {name: 1})

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


if __name__ == '__main__':
    unittest.main()
