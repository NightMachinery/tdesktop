package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unicode/utf8"
)

const lineLimit = 64 * 1024
const callTimeout = 2 * time.Second

var usage = "usage: tdesktop-cli [--socket PATH] [--json] ping|hello|windows|activate N|cycle|emoji render ID [--px N]|media toggle|play|pause [--voice|--music]|media now|voice now|lock|watch [media|lock|all]"

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
	var pixels string
	var mediaType string
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
		case a == "--px" || strings.HasPrefix(a, "--px="):
			if pixels != "" {
				return o, errors.New("--px may be specified once")
			}
			if a == "--px" {
				i++
				if i == len(args) {
					return o, errors.New("--px needs an integer from 1 to 512")
				}
				pixels = args[i]
			} else {
				pixels = strings.TrimPrefix(a, "--px=")
			}
			n, err := strconv.ParseUint(pixels, 10, 16)
			if err != nil || n < 1 || n > 512 || strconv.FormatUint(n, 10) != pixels {
				return o, errors.New("--px needs an integer from 1 to 512")
			}
			o.args["px"] = n
		case a == "--voice" || a == "--music":
			if mediaType != "" {
				return o, errors.New("choose --voice or --music once")
			}
			mediaType = strings.TrimPrefix(a, "--")
		case strings.HasPrefix(a, "-"):
			return o, fmt.Errorf("unknown option: %s", a)
		default:
			positional = append(positional, a)
		}
	}
	if len(positional) == 0 {
		return o, errors.New(usage)
	}
	if pixels != "" && positional[0] != "emoji" {
		return o, errors.New("--px is only valid for emoji render")
	}
	if mediaType != "" && positional[0] != "media" {
		return o, errors.New("--voice and --music are only valid for media controls")
	}
	switch positional[0] {
	case "ping", "hello", "lock":
		o.command = positional[0]
	case "windows":
		o.command = "window.list"
	case "cycle":
		o.command = "window.cycle"
	case "media", "voice":
		if len(positional) != 2 {
			return o, errors.New("media needs toggle, play, pause or now; voice needs now")
		}
		action := positional[1]
		if positional[0] == "voice" {
			if action != "now" {
				return o, errors.New("voice only supports now")
			}
			o.args["type"] = "voice"
		} else {
			if action != "toggle" && action != "play" && action != "pause" && action != "now" {
				return o, errors.New("unknown media action")
			}
			if action == "now" && mediaType != "" {
				return o, errors.New("use voice now for voice information")
			}
			if mediaType != "" {
				o.args["type"] = mediaType
			}
		}
		o.command = "media." + action
		return o, nil
	case "watch":
		if len(positional) > 2 {
			return o, errors.New("watch accepts media, lock or all")
		}
		kind := "all"
		if len(positional) == 2 {
			kind = positional[1]
		}
		var topics []string
		switch kind {
		case "media":
			topics = []string{"media.state"}
		case "lock":
			topics = []string{"lock.state"}
		case "all":
			topics = []string{"media.state", "lock.state"}
		default:
			return o, errors.New("watch accepts media, lock or all")
		}
		o.command = "subscribe"
		o.args["topics"] = topics
		return o, nil
	case "emoji":
		if len(positional) != 3 || positional[1] != "render" {
			return o, errors.New("emoji render needs an artwork id")
		}
		id, err := strconv.ParseUint(positional[2], 10, 64)
		if err != nil || id == 0 || strconv.FormatUint(id, 10) != positional[2] {
			return o, errors.New("artwork id must be a canonical nonzero uint64")
		}
		o.command = "emoji.render"
		o.args["id"] = id
		return o, nil
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

func exchangeTimeout(command string) time.Duration {
	if command == "emoji.render" {
		return 10 * time.Second
	}
	return callTimeout
}

func exchange(path string, o options) ([]byte, response, int, error) {
	var reply response
	conn, err := net.DialTimeout("unix", path, callTimeout)
	if err != nil {
		return nil, reply, 3, err
	}
	defer conn.Close()
	if err = conn.SetDeadline(time.Now().Add(exchangeTimeout(o.command))); err != nil {
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
	return decodeControlReply(line[:len(line)-1])
}

func decodeControlReply(line []byte) ([]byte, response, int, error) {
	var reply response
	var err error
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
	case "lock":
		var value struct {
			Locked bool `json:"locked"`
		}
		if json.Unmarshal(result, &value) != nil || !value.Locked {
			return "", errors.New("invalid lock result")
		}
		return "locked\n", nil
	case "media.toggle", "media.play", "media.pause":
		var value struct {
			Type  string `json:"type"`
			State string `json:"state"`
		}
		if json.Unmarshal(result, &value) != nil || (value.Type != "music" && value.Type != "voice") || !mediaState(value.State) {
			return "", errors.New("invalid media control result")
		}
		return value.Type + " " + value.State + "\n", nil
	case "media.now":
		return humanMedia(result)
	case "emoji.render":
		var value struct {
			Path       string `json:"path"`
			Pixels     int    `json:"px"`
			SourceKind string `json:"sourceKind"`
		}
		if json.Unmarshal(result, &value) != nil || !filepath.IsAbs(value.Path) || value.Pixels < 1 || value.Pixels > 512 || (value.SourceKind != "tgs" && value.SourceKind != "webp" && value.SourceKind != "webm") {
			return "", errors.New("invalid emoji render result")
		}
		return fmt.Sprintf("%s (%d px, %s)\n", strconv.QuoteToGraphic(value.Path), value.Pixels, value.SourceKind), nil
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

func mediaState(state string) bool {
	return state == "playing" || state == "paused" || state == "stopped"
}

func humanMedia(result json.RawMessage) (string, error) {
	var value struct {
		Type     *string `json:"type"`
		State    string  `json:"state"`
		Position int64   `json:"position_ms"`
		Duration int64   `json:"duration_ms"`
		Hidden   bool    `json:"hidden"`
		File     *struct {
			Name string  `json:"name"`
			Path *string `json:"path"`
			Mime string  `json:"mime"`
		} `json:"file"`
		Music *struct {
			Title     string `json:"title"`
			Performer string `json:"performer"`
		} `json:"music"`
		Voice *struct {
			Sender string `json:"sender"`
			Chat   string `json:"chat"`
			Date   int64  `json:"date"`
		} `json:"voice"`
	}
	if json.Unmarshal(result, &value) != nil || value.File == nil {
		return "", errors.New("invalid media information result")
	}
	path := "not downloaded"
	if value.File.Path != nil {
		if !filepath.IsAbs(*value.File.Path) {
			return "", errors.New("invalid media file path")
		}
		path = strconv.QuoteToGraphic(*value.File.Path)
	}
	file := strconv.QuoteToGraphic(value.File.Name) + "\t" + path + "\n"
	if value.Hidden {
		return "hidden\t" + file, nil
	}
	if !mediaState(value.State) || value.Position < 0 || value.Duration < 0 {
		return "", errors.New("invalid media state")
	}
	if value.Type == nil {
		if value.State != "stopped" {
			return "", errors.New("missing media type")
		}
		return "nothing playing\n", nil
	}
	var detail string
	switch *value.Type {
	case "music":
		if value.Music == nil {
			return "", errors.New("missing music information")
		}
		detail = strconv.QuoteToGraphic(value.Music.Title) + " by " + strconv.QuoteToGraphic(value.Music.Performer)
	case "voice":
		if value.Voice == nil {
			return "", errors.New("missing voice information")
		}
		detail = strconv.QuoteToGraphic(value.Voice.Sender) + " in " + strconv.QuoteToGraphic(value.Voice.Chat) + fmt.Sprintf(" (%d)", value.Voice.Date)
	default:
		return "", errors.New("invalid media type")
	}
	return fmt.Sprintf("%s %s %d/%d ms\n%s\n%s", *value.Type, value.State, value.Position, value.Duration, detail, file), nil
}

func run(args []string, getenv func(string) string, home string, out, diagnostic io.Writer) int {
	return runContext(context.Background(), args, getenv, home, out, diagnostic)
}

func runContext(ctx context.Context, args []string, getenv func(string) string, home string, out, diagnostic io.Writer) int {
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
	if o.command == "subscribe" {
		return watch(ctx, path, o, out, diagnostic)
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
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	code := runContext(ctx, os.Args[1:], os.Getenv, home, os.Stdout, os.Stderr)
	stop()
	os.Exit(code)
}
