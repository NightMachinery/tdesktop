# Local passcode keyboard layout

Purple Telegram accepts an English local passcode typed while the Persian
keyboard layout is active. When checking an entered passcode, desktop first
tries the exact UTF-8 bytes entered. Only if that fails does it try the
Persian-to-English candidate from the shared `purple-core` mapper. Characters
outside that mapper stay unchanged. A failed pair of checks counts as one
passcode attempt for the lock screen and existing passcode controls.

This applies to an already running account and to startup decryption, including
the legacy single-account format. Legacy migration encrypts the new local key
with whichever candidate actually unlocked the old key. Setting or changing a
passcode still stores the exact new passcode entered; no stored passcode is
rewritten just because its keyboard-layout counterpart was accepted.

The mapper is `purple/purple_passcode.cpp` in the `purple-core` submodule. Its
test covers the complete 85-character map, mixed text, and characters that
must remain unchanged. The desktop application compiles that source directly.
