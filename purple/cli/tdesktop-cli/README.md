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
```

Enable Local automation in Purple Settings first. `--socket PATH` overrides
`PURPLE_CONTROL_SOCKET` and the macOS default. `--json` returns the v1 envelope.
No command enables automation or changes settings.

See [the control socket guide](../../../docs/purple/control_socket.md) for paths,
capabilities, exit codes, limits and host tests. CMake does not build or install
this command.
