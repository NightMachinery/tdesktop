package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

func fakeServer(t *testing.T, send func(net.Conn, map[string]any)) string {
	t.Helper()
	dir, err := os.MkdirTemp("", "pc-")
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "s")
	listener, err := net.Listen("unix", path)
	if err != nil {
		os.RemoveAll(dir)
		t.Fatal(err)
	}
	done := make(chan struct{})
	go func() {
		defer close(done)
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		conn.SetDeadline(time.Now().Add(3 * time.Second))
		var request map[string]any
		decoder := json.NewDecoder(bufio.NewReader(conn))
		decoder.UseNumber()
		if err := decoder.Decode(&request); err != nil {
			t.Errorf("request: %v", err)
			return
		}
		if request["v"] != json.Number("1") || request["id"] != json.Number("1") || request["args"] == nil {
			t.Errorf("invalid request: %v", request)
			return
		}
		send(conn, request)
	}()
	t.Cleanup(func() {
		listener.Close()
		select {
		case <-done:
		case <-time.After(4 * time.Second):
			t.Error("fake server did not stop")
		}
		os.RemoveAll(dir)
	})
	return path
}

func TestOptionsAndDiscovery(t *testing.T) {
	t.Setenv("XDG_DATA_HOME", "")
	wantDefault := "/home/example/.local/share/TelegramDesktop/purple-control.sock"
	if runtime.GOOS == "darwin" {
		wantDefault = "/home/example/Library/Application Support/Purple Telegram/purple-control.sock"
	}
	for _, args := range [][]string{{"ping"}, {"hello", "--json"}, {"--socket", "/private/control.sock", "windows"}, {"activate", "3", "--json"}, {"cycle"}} {
		if _, err := parseOptions(args); err != nil {
			t.Errorf("%v: %v", args, err)
		}
	}
	for _, args := range [][]string{{}, {"--socket"}, {"--socket="}, {"--bad", "ping"}, {"activate"}, {"activate", "-1"}, {"activate", "1.5"}, {"ping", "extra"}, {"media", "seek"}} {
		if _, err := parseOptions(args); err == nil {
			t.Errorf("accepted %v", args)
		}
	}
	for _, tc := range []struct{ explicit, environment, want string }{
		{"/explicit/s", "/env/s", "/explicit/s"}, {"", "/env/s", "/env/s"}, {"", "", wantDefault},
	} {
		got, err := resolveSocket(tc.explicit, tc.environment, "/home/example")
		if err != nil || got != tc.want {
			t.Errorf("resolve=%q %v want%q", got, err, tc.want)
		}
	}
	if got := platformDefaultSocket("darwin", "/"+strings.Repeat("x", 50), "", func(string) bool { return false }); !strings.HasSuffix(got, "Purple/control.sock") {
		t.Fatalf("long primary path did not choose fallback: %s", got)
	}
	for _, path := range []string{"relative", "/x/../s", "/x\x00s", "/" + strings.Repeat("x", 103)} {
		if _, err := resolveSocket(path, "", "/home/example"); err == nil {
			t.Errorf("accepted invalid path %q", path)
		}
	}
}

func TestPlatformDefaultSocket(t *testing.T) {
	for _, platform := range []string{"linux", "freebsd", "openbsd", "netbsd", "dragonfly"} {
		for _, tc := range []struct {
			name, dataHome, legacyFile, want string
		}{
			{"default", "", "", "/h/.local/share/TelegramDesktop/purple-control.sock"},
			{"xdg", "/data", "", "/data/TelegramDesktop/purple-control.sock"},
			{"relative-xdg", "relative/data", "", "/h/.local/share/TelegramDesktop/purple-control.sock"},
			{"legacy0", "/data", "/h/.TelegramDesktop/tdata/settings0", "/h/.TelegramDesktop/purple-control.sock"},
			{"legacy1", "/data", "/h/.TelegramDesktop/tdata/settings1", "/h/.TelegramDesktop/purple-control.sock"},
			{"legacys", "/data", "/h/.TelegramDesktop/tdata/settingss", "/h/.TelegramDesktop/purple-control.sock"},
			{"unrelated-legacy", "/data", "/h/.TelegramDesktop/tdata/settings2", "/data/TelegramDesktop/purple-control.sock"},
			{"long-xdg", "/" + strings.Repeat("x", 100), "", "/h/Library/Application Support/Purple/control.sock"},
			{"utf8-xdg", "/" + strings.Repeat("界", 30), "", "/h/Library/Application Support/Purple/control.sock"},
		} {
			t.Run(platform+"/"+tc.name, func(t *testing.T) {
				got := platformDefaultSocket(platform, "/h", tc.dataHome, func(path string) bool {
					return path == tc.legacyFile
				})
				if got != tc.want {
					t.Fatalf("got %q want %q", got, tc.want)
				}
			})
		}
	}
	for _, tc := range []struct{ home, dataHome, want string }{
		{"/h", "", "/h/Library/Application Support/Purple Telegram/purple-control.sock"},
		{"/h", "/ignored", "/h/Library/Application Support/Purple Telegram/purple-control.sock"},
		{"/" + strings.Repeat("x", 50), "", "/" + strings.Repeat("x", 50) + "/Library/Application Support/Purple/control.sock"},
	} {
		got := platformDefaultSocket("darwin", tc.home, tc.dataHome, func(string) bool { return true })
		if got != tc.want {
			t.Errorf("macOS got %q want %q", got, tc.want)
		}
	}
	for _, length := range []int{103, 104} {
		dataHome := "/" + strings.Repeat("x", length-len("/TelegramDesktop/purple-control.sock")-1)
		want := dataHome + "/TelegramDesktop/purple-control.sock"
		if len(want) != length {
			t.Fatal("invalid independent boundary setup")
		}
		if length == 104 {
			want = "/h/Library/Application Support/Purple/control.sock"
		}
		got := platformDefaultSocket("linux", "/h", dataHome, func(string) bool { return false })
		if got != want {
			t.Errorf("%d byte default: got %q want %q", length, got, want)
		}
	}
}

func TestDefaultSocketLegacyFiles(t *testing.T) {
	if runtime.GOOS == "darwin" {
		t.Skip("macOS does not use legacy Unix profile selection")
	}
	home := t.TempDir()
	legacy := filepath.Join(home, ".TelegramDesktop", "tdata")
	if err := os.MkdirAll(legacy, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(legacy, "settingss"), []byte("synthetic"), 0600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("XDG_DATA_HOME", "/data")
	primary := filepath.Join(home, ".TelegramDesktop", "purple-control.sock")
	want := primary
	if len(primary) >= 104 {
		want = filepath.Join(home, "Library", "Application Support", "Purple", "control.sock")
	}
	if got := defaultSocket(home); got != want {
		t.Errorf("production legacy selection got %q want %q", got, want)
	}
}

func TestCommandsAndHumanOutput(t *testing.T) {
	for _, tc := range []struct {
		args                  []string
		command, result, want string
	}{
		{[]string{"ping"}, "ping", `{"pong":true}`, "pong\n"},
		{[]string{"hello"}, "hello", `{"version":1,"commands":["hello"]}`, "\"version\": 1"},
		{[]string{"windows"}, "window.list", `{"windows":[{"id":0,"title":"window\nname","primary":true,"active":true}]}`, `* 0`},
		{[]string{"activate", "2"}, "window.activate", `{"activated":2}`, "activated 2\n"},
		{[]string{"cycle"}, "window.cycle", `{"activated":0}`, "activated 0\n"},
	} {
		t.Run(tc.command, func(t *testing.T) {
			path := fakeServer(t, func(c net.Conn, req map[string]any) {
				if req["cmd"] != tc.command {
					t.Errorf("command=%v", req["cmd"])
				}
				if tc.command == "window.activate" && req["args"].(map[string]any)["index"] != json.Number("2") {
					t.Errorf("index not preserved")
				}
				fmt.Fprintf(c, "{\"v\":1,\"id\":1,\"ok\":true,\"result\":%s}\n", tc.result)
			})
			var out, diagnostic bytes.Buffer
			code := run(append(tc.args, "--socket", path), func(string) string { return "" }, "/unused", &out, &diagnostic)
			if code != 0 || !strings.Contains(out.String(), tc.want) || diagnostic.Len() != 0 {
				t.Errorf("code=%d output=%q error=%q", code, out.String(), diagnostic.String())
			}
		})
	}
}

func TestReplyValidationAndExitCodes(t *testing.T) {
	for _, tc := range []struct {
		name, reply string
		code        int
	}{
		{"disabled", `{"v":1,"id":1,"ok":false,"error":{"code":"automation-disabled","message":"off"}}`, 2},
		{"capability", `{"v":1,"id":1,"ok":false,"error":{"code":"capability-off","message":"off"}}`, 2},
		{"locked", `{"v":1,"id":1,"ok":false,"error":{"code":"locked","message":"locked"}}`, 2},
		{"unsupported", `{"v":1,"id":1,"ok":false,"error":{"code":"unsupported","message":"old"}}`, 4},
		{"busy", `{"v":1,"id":1,"ok":false,"error":{"code":"busy","message":"later"}}`, 1},
		{"version", `{"v":2,"id":1,"ok":true,"result":{}}`, 4},
		{"id", `{"v":1,"id":2,"ok":true,"result":{}}`, 1},
		{"fractional", `{"v":1,"id":1.5,"ok":true,"result":{}}`, 1},
		{"missing-id", `{"v":1,"ok":true,"result":{}}`, 1},
		{"missing-ok", `{"v":1,"id":1,"result":{}}`, 1},
		{"missing-result", `{"v":1,"id":1,"ok":true}`, 1},
		{"nonobject-result", `{"v":1,"id":1,"ok":true,"result":[]}`, 1},
		{"missing-error", `{"v":1,"id":1,"ok":false}`, 1},
		{"mixed", `{"v":1,"id":1,"ok":true,"result":{},"error":{"code":"busy","message":"later"}}`, 1},
		{"trailing", `{"v":1,"id":1,"ok":true,"result":{}} {}`, 1},
		{"invalid-utf8", string([]byte{0xff}), 1},
		{"oversized", strings.Repeat("x", lineLimit+1), 1},
		{"malformed", `{`, 1},
	} {
		t.Run(tc.name, func(t *testing.T) {
			path := fakeServer(t, func(c net.Conn, _ map[string]any) { fmt.Fprintln(c, tc.reply) })
			var out, diagnostic bytes.Buffer
			code := run([]string{"--socket", path, "--json", "ping"}, func(string) string { return "" }, "/unused", &out, &diagnostic)
			if code != tc.code {
				t.Errorf("code%d want%d: %s", code, tc.code, diagnostic.String())
			}
			if tc.name == "disabled" && !strings.Contains(diagnostic.String(), "confirm both dialogs") {
				t.Error("missing local enable guidance")
			}
		})
	}
}

func TestFramingAndNoSocket(t *testing.T) {
	for _, tc := range []struct {
		name string
		body []byte
		code int
	}{
		{"exact-limit", append([]byte(`{"v":1,"id":1,"ok":true,"result":{"pong":true}}`), bytes.Repeat([]byte(" "), lineLimit-len(`{"v":1,"id":1,"ok":true,"result":{"pong":true}}`))...), 0},
		{"truncated", []byte(`{"v":1,"id":1,"ok":true,"result":{"pong":true}}`), 1},
	} {
		t.Run(tc.name, func(t *testing.T) {
			path := fakeServer(t, func(c net.Conn, _ map[string]any) {
				c.Write(tc.body)
				if tc.name != "truncated" {
					c.Write([]byte("\n"))
				}
			})
			var out, diagnostic bytes.Buffer
			if code := run([]string{"ping", "--json", "--socket", path}, func(string) string { return "" }, "/unused", &out, &diagnostic); code != tc.code {
				t.Errorf("code%d want%d %s", code, tc.code, diagnostic.String())
			}
		})
	}
	var out, diagnostic bytes.Buffer
	dir, err := os.MkdirTemp("", "pc-")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(dir)
	if code := run([]string{"ping", "--socket", filepath.Join(dir, "absent")}, func(string) string { return "" }, "/unused", &out, &diagnostic); code != 3 {
		t.Errorf("absent socket code%d", code)
	}
}

func TestDeadline(t *testing.T) {
	path := fakeServer(t, func(c net.Conn, _ map[string]any) { time.Sleep(callTimeout + 100*time.Millisecond) })
	var out, diagnostic bytes.Buffer
	started := time.Now()
	code := run([]string{"ping", "--socket", path}, func(string) string { return "" }, "/unused", &out, &diagnostic)
	if code != 1 || time.Since(started) > callTimeout+time.Second {
		t.Errorf("deadline code%d elapsed%v: %s", code, time.Since(started), diagnostic.String())
	}
}

func TestEmojiRenderOptions(t *testing.T) {
	for _, args := range [][]string{
		{"emoji", "render", "18446744073709551615"},
		{"--json", "emoji", "render", "9007199254740993", "--px", "128"},
		{"--px=512", "emoji", "render", "1"},
	} {
		o, err := parseOptions(args)
		if err != nil || o.command != "emoji.render" {
			t.Fatalf("%v: %v", args, err)
		}
	}
	for _, args := range [][]string{
		{"emoji"}, {"emoji", "render"}, {"emoji", "other", "1"},
		{"emoji", "render", "0"}, {"emoji", "render", "01"}, {"emoji", "render", "+1"},
		{"emoji", "render", "1.0"}, {"emoji", "render", "1e2"}, {"emoji", "render", "18446744073709551616"},
		{"emoji", "render", "1", "--px"}, {"emoji", "render", "1", "--px=0"},
		{"emoji", "render", "1", "--px=513"}, {"emoji", "render", "1", "--px=1.5"},
		{"emoji", "render", "1", "--px=01"}, {"emoji", "render", "1", "--px="},
		{"emoji", "render", "1", "--px=2", "--px=3"}, {"ping", "--px=128"},
	} {
		if _, err := parseOptions(args); err == nil {
			t.Errorf("accepted %v", args)
		}
	}
	if exchangeTimeout("emoji.render") != 10*time.Second || exchangeTimeout("ping") != 2*time.Second {
		t.Fatal("render wait must outlast the bounded app render timeout")
	}
}

func TestEmojiRenderFakeServer(t *testing.T) {
	for _, asJSON := range []bool{false, true} {
		path := fakeServer(t, func(conn net.Conn, request map[string]any) {
			args := request["args"].(map[string]any)
			if request["cmd"] != "emoji.render" || args["id"] != json.Number("18446744073709551615") || args["px"] != json.Number("128") {
				t.Errorf("wrong exact emoji command: %v", request)
			}
			fmt.Fprintln(conn, `{"v":1,"id":1,"ok":true,"result":{"path":"/owned/emoji-export/large/18446744073709551615-128.png","px":128,"sourceKind":"tgs"}}`)
		})
		args := []string{"--socket", path, "emoji", "render", "18446744073709551615", "--px", "128"}
		if asJSON {
			args = append(args, "--json")
		}
		var out, diagnostic bytes.Buffer
		if code := run(args, func(string) string { return "" }, "/unused", &out, &diagnostic); code != 0 || !strings.Contains(out.String(), "18446744073709551615-128.png") {
			t.Fatalf("render: code=%d out=%s err=%s", code, out.String(), diagnostic.String())
		}
		if asJSON && !strings.Contains(out.String(), `"ok":true`) {
			t.Fatal("missing JSON envelope")
		}
	}
	for _, tc := range []struct {
		code string
		exit int
	}{
		{"automation-disabled", 2}, {"capability-off", 2}, {"locked", 2}, {"unsupported", 4},
		{"export-disabled", 1}, {"busy", 1}, {"timeout", 1}, {"not-found", 1},
	} {
		path := fakeServer(t, func(conn net.Conn, _ map[string]any) {
			fmt.Fprintf(conn, `{"v":1,"id":1,"ok":false,"error":{"code":%q,"message":"refused"}}`+"\n", tc.code)
		})
		var out, diagnostic bytes.Buffer
		if code := run([]string{"--socket", path, "emoji", "render", "1"}, func(string) string { return "" }, "/unused", &out, &diagnostic); code != tc.exit {
			t.Errorf("%s exit=%d want%d", tc.code, code, tc.exit)
		}
	}
	if _, err := humanResult("emoji.render", json.RawMessage(`{"path":"relative","px":1024,"sourceKind":"bad"}`)); err == nil {
		t.Fatal("invalid render result accepted")
	}
}

func TestEmojiRenderWaitsBeyondOrdinaryDeadline(t *testing.T) {
	path := fakeServer(t, func(conn net.Conn, _ map[string]any) {
		time.Sleep(2100 * time.Millisecond)
		fmt.Fprintln(conn, `{"v":1,"id":1,"ok":true,"result":{"path":"/owned/emoji-export/large/1-128.png","px":128,"sourceKind":"webp"}}`)
	})
	var out, diagnostic bytes.Buffer
	if code := run([]string{"--socket", path, "emoji", "render", "1"}, func(string) string { return "" }, "/unused", &out, &diagnostic); code != 0 {
		t.Fatalf("render inherited ordinary deadline: code=%d %s", code, diagnostic.String())
	}
}

func TestMediaOptions(t *testing.T) {
	for _, tc := range []struct {
		args          []string
		command, kind string
	}{
		{[]string{"media", "toggle"}, "media.toggle", ""},
		{[]string{"media", "play", "--voice"}, "media.play", "voice"},
		{[]string{"--music", "media", "pause"}, "media.pause", "music"},
		{[]string{"media", "now", "--json"}, "media.now", ""},
		{[]string{"voice", "now"}, "media.now", "voice"},
		{[]string{"lock"}, "lock", ""},
	} {
		o, e := parseOptions(tc.args)
		if e != nil || o.command != tc.command || (tc.kind != "" && o.args["type"] != tc.kind) || (tc.kind == "" && o.args["type"] != nil) {
			t.Fatalf("%v: %+v %v", tc.args, o, e)
		}
	}
	for _, args := range [][]string{
		{"media"}, {"media", "seek"}, {"media", "play", "extra"}, {"voice", "pause"}, {"lock", "extra"},
		{"media", "play", "--voice", "--music"}, {"media", "play", "--voice", "--voice"},
		{"ping", "--music"}, {"voice", "now", "--voice"}, {"media", "now", "--voice"}, {"lock", "--px=1"},
	} {
		if _, e := parseOptions(args); e == nil {
			t.Errorf("accepted %v", args)
		}
	}
}

func TestMediaAndLockFakeServer(t *testing.T) {
	for _, tc := range []struct {
		args                         []string
		command, kind, result, human string
	}{
		{[]string{"media", "toggle"}, "media.toggle", "", `{"type":"music","state":"paused"}`, "music paused"},
		{[]string{"media", "play", "--voice"}, "media.play", "voice", `{"type":"voice","state":"playing"}`, "voice playing"},
		{[]string{"media", "pause", "--music"}, "media.pause", "music", `{"type":"music","state":"paused"}`, "music paused"},
		{[]string{"lock"}, "lock", "", `{"locked":true}`, "locked"},
		{[]string{"media", "now"}, "media.now", "", `{"type":"music","state":"playing","position_ms":123,"duration_ms":456,"file":{"name":"song.ogg","path":"/owned/song.ogg","mime":"audio/ogg"},"music":{"title":"Title","performer":"Artist"}}`, "Title"},
		{[]string{"voice", "now"}, "media.now", "voice", `{"type":"voice","state":"paused","position_ms":0,"duration_ms":456,"file":{"name":"voice.ogg","path":null,"mime":"audio/ogg"},"voice":{"sender":"Sender","chat":"Chat","date":123}}`, "not downloaded"},
		{[]string{"media", "now"}, "media.now", "", `{"type":null,"state":"stopped","position_ms":0,"duration_ms":0,"file":{"name":"","path":null,"mime":""}}`, "nothing playing"},
		{[]string{"voice", "now"}, "media.now", "voice", `{"hidden":true,"file":{"name":"voice.ogg","path":"/owned/voice.ogg"}}`, "hidden"},
	} {
		for _, asJSON := range []bool{false, true} {
			path := fakeServer(t, func(c net.Conn, q map[string]any) {
				a := q["args"].(map[string]any)
				if q["cmd"] != tc.command || (tc.kind != "" && a["type"] != tc.kind) || (tc.kind == "" && a["type"] != nil) {
					t.Errorf("wrong command: %v", q)
				}
				fmt.Fprintf(c, `{"v":1,"id":1,"ok":true,"result":%s}`+"\n", tc.result)
			})
			args := append([]string{"--socket", path}, tc.args...)
			if asJSON {
				args = append(args, "--json")
			}
			var out, diag bytes.Buffer
			if code := run(args, func(string) string { return "" }, "/unused", &out, &diag); code != 0 {
				t.Fatalf("%v code%d %s", args, code, diag.String())
			}
			if asJSON {
				if !strings.Contains(out.String(), `"ok":true`) {
					t.Fatal(out.String())
				}
			} else if !strings.Contains(out.String(), tc.human) {
				t.Fatal(out.String())
			}
		}
	}
	for _, tc := range []struct {
		reason string
		code   int
	}{{"no-passcode", 1}, {"automation-disabled", 2}, {"capability-off", 2}, {"locked", 2}, {"not-found", 1}, {"unsupported", 4}} {
		path := fakeServer(t, func(c net.Conn, _ map[string]any) {
			fmt.Fprintf(c, `{"v":1,"id":1,"ok":false,"error":{"code":%q,"message":"refused"}}`+"\n", tc.reason)
		})
		var out, diag bytes.Buffer
		if code := run([]string{"lock", "--socket", path}, func(string) string { return "" }, "/unused", &out, &diag); code != tc.code {
			t.Errorf("%s:%d", tc.reason, code)
		}
	}
	for command, result := range map[string]string{"lock": `{"locked":false}`, "media.play": `{"type":"bad","state":"playing"}`, "media.now": `{"type":"music","state":"invalid","file":{"path":null}}`} {
		if _, e := humanResult(command, json.RawMessage(result)); e == nil {
			t.Error("invalid result accepted", command)
		}
	}
}
