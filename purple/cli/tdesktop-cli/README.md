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
./tdesktop-cli emoji render 5339220291157500400 --px 256 --json
```

Enable Local automation in Purple Settings first. `--socket PATH` overrides
`PURPLE_CONTROL_SOCKET` and the platform default. `--json` returns the v1 envelope.
Emoji artwork IDs are canonical nonzero uint64 decimals; `--px` accepts 1 through
512 and defaults to 512. Render calls wait up to ten seconds and report the
published path, actual pixel bound and source kind. Enable Local emoji export
and the emoji capability group as well as Local automation.
No command enables automation or changes settings.

See [the control socket guide](../../../docs/purple/control_socket.md) for paths,
capabilities, exit codes, limits and host tests. CMake does not build or install
this command.
