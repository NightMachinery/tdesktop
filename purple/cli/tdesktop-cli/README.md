# tdesktop-cli

A small local command for Purple hotkeys and scripts. Requires a local Go 1.26 or
later toolchain and uses only the standard library.

```sh
go test ./...
go build -o tdesktop-cli .
./tdesktop-cli hello --json
./tdesktop-cli windows
./tdesktop-cli activate 0
./tdesktop-cli cycle
./tdesktop-cli media toggle
./tdesktop-cli media play --music
./tdesktop-cli media pause --voice
./tdesktop-cli media now --json
./tdesktop-cli voice now
./tdesktop-cli lock
./tdesktop-cli emoji render 5339220291157500400 --px 256 --json
```

Enable Local automation in Purple Settings first. `--socket PATH` overrides
`PURPLE_CONTROL_SOCKET` and the platform default. `--json` returns the v1 envelope.
Emoji artwork IDs are canonical nonzero uint64 decimals; `--px` accepts 1 through
512 and defaults to 512. Render calls wait up to ten seconds and report the
published path, actual pixel bound and source kind. Enable Local emoji export
and the emoji capability group as well as Local automation.
Playback commands use the active media type unless `--voice` or `--music` selects
one. `media now` reports the active item; `voice now` requests voice explicitly.
A not-downloaded file has no path. Work Mode hidden items return only their
filename and available path. Media information refuses while passcode locked;
playback and lock still work. `lock` requires a configured local passcode.
No command enables automation or changes settings.

See [the control socket guide](../../../docs/purple/control_socket.md) for paths,
capabilities, exit codes, limits and host tests. CMake does not build or install
this command.
