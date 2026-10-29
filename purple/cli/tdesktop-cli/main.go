package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
)

const lineLimit = 64 * 1024
const callTimeout = 2 * time.Second

var usage = "usage: tdesktop-cli [--socket PATH] [--json] ping|hello|windows|activate N|cycle"

type options struct {
	socket  string
	asJSON  bool
	command string
	args    map[string]any
}

type response struct {
	Version json.Number     `json:"v"`
	ID      json.Number     `json:"id"`
	OK      *bool           `json:"ok"`
	Result  json.RawMessage `json:"result"`
	Error   *struct {
		Code    string `json:"code"`
		Message string `json:"message"`
	} `json:"error"`
}

func parseOptions(args []string) (options, error) {
	o := options{args: map[string]any{}}
	var positional []string
	for i := 0; i < len(args); i++ {
		a := args[i]
		switch {
		case a == "--json":
			o.asJSON = true
		case a == "--socket":
			i++
			if i == len(args) || args[i] == "" {
				return o, errors.New("--socket needs a path")
			}
			o.socket = args[i]
		case strings.HasPrefix(a, "--socket="):
			o.socket = strings.TrimPrefix(a, "--socket=")
			if o.socket == "" {
				return o, errors.New("--socket needs a path")
			}
		case strings.HasPrefix(a, "-"):
			return o, fmt.Errorf("unknown option: %s", a)
		default:
			positional = append(positional, a)
		}
	}
	if len(positional) == 0 {
		return o, errors.New(usage)
	}
	switch positional[0] {
	case "ping", "hello":
		o.command = positional[0]
	case "windows":
		o.command = "window.list"
	case "cycle":
		o.command = "window.cycle"
	case "activate":
		o.command = "window.activate"
		if len(positional) != 2 {
			return o, errors.New("activate needs a window index")
		}
		n, err := strconv.ParseInt(positional[1], 10, 32)
		if err != nil || n < 0 {
			return o, errors.New("window index must be a nonnegative integer")
		}
		o.args["index"] = n
		return o, nil
	default:
		return o, fmt.Errorf("unsupported command: %s", positional[0])
	}
	if len(positional) != 1 {
		return o, errors.New("unexpected arguments")
	}
	return o, nil
}

func platformDefaultSocket(platform, home, dataHome string, exists func(string) bool) string {
	var directory string
	if platform == "darwin" {
		directory = filepath.Join(home, "Library", "Application Support", "Purple Telegram")
	} else {
		legacy := filepath.Join(home, ".TelegramDesktop")
		for _, suffix := range []string{"0", "1", "s"} {
			if exists(filepath.Join(legacy, "tdata", "settings"+suffix)) {
				directory = legacy
				break
			}
		}
		if directory == "" {
			if !filepath.IsAbs(dataHome) {
				dataHome = filepath.Join(home, ".local", "share")
			}
			directory = filepath.Join(dataHome, "TelegramDesktop")
		}
	}
	path := filepath.Join(directory, "purple-control.sock")
	if len(path) < 104 {
		return path
	}
	return filepath.Join(home, "Library", "Application Support", "Purple", "control.sock")
}

func defaultSocket(home string) string {
	return platformDefaultSocket(runtime.GOOS, home, os.Getenv("XDG_DATA_HOME"), func(path string) bool {
		_, err := os.Stat(path)
		return err == nil
	})
}

func resolveSocket(explicit, environment, home string) (string, error) {
	path := explicit
	if path == "" {
		path = environment
	}
	if path == "" {
		path = defaultSocket(home)
	}
	if !filepath.IsAbs(path) || filepath.Clean(path) != path || strings.ContainsRune(path, 0) || len([]byte(path)) >= 104 {
		return "", errors.New("socket path must be absolute, clean and shorter than 104 bytes")
	}
	return path, nil
}

func exchange(path string, o options) ([]byte, response, int, error) {
	var reply response
	conn, err := net.DialTimeout("unix", path, callTimeout)
	if err != nil {
		return nil, reply, 3, err
	}
	defer conn.Close()
	if err = conn.SetDeadline(time.Now().Add(callTimeout)); err != nil {
		return nil, reply, 1, err
	}
	request, err := json.Marshal(map[string]any{"v": 1, "id": 1, "cmd": o.command, "args": o.args})
	if err != nil {
		return nil, reply, 1, err
	}
	if _, err = io.Copy(conn, bytes.NewReader(append(request, '\n'))); err != nil {
		return nil, reply, 1, err
	}
	line, err := bufio.NewReaderSize(conn, lineLimit+2).ReadSlice('\n')
	if err != nil {
		return nil, reply, 1, fmt.Errorf("incomplete or oversized reply: %w", err)
	}
	line = line[:len(line)-1]
	if len(line) > lineLimit || !utf8.Valid(line) {
		return nil, reply, 1, errors.New("invalid or oversized UTF-8 reply")
	}
	decoder := json.NewDecoder(bytes.NewReader(line))
	decoder.UseNumber()
	if err = decoder.Decode(&reply); err != nil {
		return nil, reply, 1, fmt.Errorf("invalid reply: %w", err)
	}
	var trailing any
	if decoder.Decode(&trailing) != io.EOF {
		return nil, reply, 1, errors.New("trailing reply data")
	}
	version, err := reply.Version.Int64()
	if err != nil {
		return nil, reply, 1, errors.New("missing or invalid protocol version")
	}
	if version != 1 {
		return nil, reply, 4, errors.New("unsupported protocol version")
	}
	id, err := reply.ID.Int64()
	if err != nil || id != 1 || reply.OK == nil {
		return nil, reply, 1, errors.New("mismatched or incomplete reply envelope")
	}
	if *reply.OK {
		var object map[string]json.RawMessage
		if reply.Error != nil || json.Unmarshal(reply.Result, &object) != nil || object == nil {
			return nil, reply, 1, errors.New("invalid success result")
		}
		return line, reply, 0, nil
	}
	if reply.Error == nil || reply.Error.Code == "" || reply.Error.Message == "" || len(reply.Result) != 0 {
		return nil, reply, 1, errors.New("invalid error result")
	}
	code := 1
	switch reply.Error.Code {
	case "automation-disabled", "capability-off", "locked":
		code = 2
	case "unsupported":
		code = 4
	}
	return line, reply, code, errors.New(reply.Error.Code + ": " + reply.Error.Message)
}

func humanResult(command string, result json.RawMessage) (string, error) {
	switch command {
	case "ping":
		var value struct {
			Pong bool `json:"pong"`
		}
		if json.Unmarshal(result, &value) != nil || !value.Pong {
			return "", errors.New("invalid pong result")
		}
		return "pong\n", nil
	case "window.list":
		var value struct {
			Windows []struct {
				ID      int    `json:"id"`
				Title   string `json:"title"`
				Primary bool   `json:"primary"`
				Active  bool   `json:"active"`
			} `json:"windows"`
		}
		if json.Unmarshal(result, &value) != nil || value.Windows == nil {
			return "", errors.New("invalid window list")
		}
		var output strings.Builder
		for _, w := range value.Windows {
			marker := " "
			if w.Active {
				marker = "*"
			}
			fmt.Fprintf(&output, "%s %d\t%s\n", marker, w.ID, strconv.QuoteToGraphic(w.Title))
		}
		return output.String(), nil
	case "window.activate", "window.cycle":
		var value struct {
			Activated *int `json:"activated"`
		}
		if json.Unmarshal(result, &value) != nil || value.Activated == nil || *value.Activated < 0 {
			return "", errors.New("invalid activation result")
		}
		return fmt.Sprintf("activated %d\n", *value.Activated), nil
	default:
		var formatted bytes.Buffer
		if err := json.Indent(&formatted, result, "", "  "); err != nil {
			return "", err
		}
		return formatted.String() + "\n", nil
	}
}

func run(args []string, getenv func(string) string, home string, out, diagnostic io.Writer) int {
	if len(args) == 1 && (args[0] == "--help" || args[0] == "-h") {
		fmt.Fprintln(out, usage)
		return 0
	}
	o, err := parseOptions(args)
	if err != nil {
		fmt.Fprintln(diagnostic, err)
		return 1
	}
	path, err := resolveSocket(o.socket, getenv("PURPLE_CONTROL_SOCKET"), home)
	if err != nil {
		fmt.Fprintln(diagnostic, err)
		return 1
	}
	line, reply, code, err := exchange(path, o)
	if o.asJSON && line != nil {
		fmt.Fprintln(out, string(line))
	}
	if err != nil {
		fmt.Fprintln(diagnostic, err)
		if reply.Error != nil && reply.Error.Code == "automation-disabled" {
			fmt.Fprintln(diagnostic, "Enable Local automation in Purple Settings and confirm both dialogs.")
		}
		return code
	}
	if !o.asJSON {
		text, err := humanResult(o.command, reply.Result)
		if err != nil {
			fmt.Fprintln(diagnostic, err)
			return 1
		}
		fmt.Fprint(out, text)
	}
	return 0
}

func main() {
	home, err := os.UserHomeDir()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	os.Exit(run(os.Args[1:], os.Getenv, home, os.Stdout, os.Stderr))
}
