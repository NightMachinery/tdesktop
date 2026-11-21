# Local passcode keyboard layout

Purple Telegram accepts an English local passcode typed while the Persian
keyboard layout is active. When checking an entered passcode, desktop first
tries the exact UTF-8 bytes entered. Only if that fails does it try the
Persian-to-English candidate from the shared `purple-core` mapper. Characters
outside that mapper stay unchanged. A failed pair of checks counts as one
passcode attempt for the lock screen and existing passcode controls.

This applies to an already running account and to startup decryption, including
the legacy single-account format. Upstream derives each key in
`Storage::PasscodeDerivation`; Purple's `PreferPasscodeForWrap` runs at its start
and keeps the counterpart only when it alone opens the stored wrap, so a KDF
passcode costs extra derivations only when its text has a counterpart. The host
gate `cases-storage-passcode` checks that decision. Legacy migration encrypts the new local key
with whichever candidate actually unlocked the old key. Setting or changing a
passcode still stores the exact new passcode entered; no stored passcode is
rewritten just because its keyboard-layout counterpart was accepted.

The mapper is `purple/purple_passcode.cpp` in the `purple-core` submodule. Its
test covers the complete 85-character map, mixed text, and characters that
must remain unchanged. The desktop application compiles that source directly.

On Android, the signed APK from source `5e428532` with core `e7bf8544` was
tested on a disposable emulator account. A text passcode set as `qwer` unlocked
with Persian keyboard input `ضصثق`, and the exact ASCII `qwer` unlocked too.
Passcode Lock was then turned off and the account remained logged in.

## Touch ID on macOS

Purple checks that the lock screen is visible and has keyboard focus before
requesting system authentication. Touch ID and Apple Watch use a macOS sheet
attached to that lock window. Only one request can be pending across Purple's
windows. Switching away cancels the sheet. You can retry from the lock screen
or enter your Purple passcode.

The sheet offers the system password through the standard macOS authentication
dialog. See [upstream hooks](upstream_hooks.md#macos-passcode-unlock).
