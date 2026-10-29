package main

import (
	"bytes"
	"context"
	"fmt"
	"net"
	"strings"
	"testing"
	"time"
)

func TestWatchOptions(t *testing.T) {
	for _, args := range [][]string{{"watch"}, {"watch", "media"}, {"watch", "lock", "--json"}, {"watch", "all"}} {
		o, e := parseOptions(args)
		if e != nil || o.command != "subscribe" {
			t.Fatalf("%v: %v", args, e)
		}
	}
	for _, args := range [][]string{{"watch", "bad"}, {"watch", "media", "all"}, {"watch", "--voice"}, {"watch", "--px=1"}} {
		if _, e := parseOptions(args); e == nil {
			t.Fatal("accepted", args)
		}
	}
}

func TestWatchEvents(t *testing.T) {
	for _, asJSON := range []bool{false, true} {
		path := fakeServer(t, func(c net.Conn, q map[string]any) {
			if q["cmd"] != "subscribe" {
				t.Error(q)
			}
			topics := q["args"].(map[string]any)["topics"].([]any)
			if len(topics) != 2 || topics[0] != "media.state" || topics[1] != "lock.state" {
				t.Error(q)
			}
			fmt.Fprintln(c, `{"v":1,"id":1,"ok":true,"result":{"topics":["media.state","lock.state"]}}`)
			fmt.Fprintln(c, `{"v":1,"event":"media.state","result":{"hidden":true,"file":{"name":"voice","path":null}}}`)
			fmt.Fprintln(c, `{"v":1,"event":"lock.state","result":{"locked":false}}`)
			fmt.Fprintln(c, `{"v":1,"event":"media.state","error":{"code":"locked","message":"locked"}}`)
			fmt.Fprintln(c, `{"v":1,"event":"lock.state","error":{"code":"capability-off","message":"off"},"ended":true}`)
		})
		args := []string{"watch", "--socket", path}
		if asJSON {
			args = append(args, "--json")
		}
		var out, diag bytes.Buffer
		code := run(args, func(string) string { return "" }, "/unused", &out, &diag)
		if code != 2 || strings.Count(out.String(), "\n") != 4 {
			t.Fatalf("code%d out%q err%s", code, out.String(), diag.String())
		}
		if asJSON {
			if !strings.Contains(out.String(), `"event":"media.state"`) {
				t.Fatal(out.String())
			}
		} else {
			if !strings.Contains(out.String(), "lock.state\tunlocked") || !strings.Contains(out.String(), "media.state\tlocked") {
				t.Fatal(out.String())
			}
		}
	}
}

func TestWatchRefusalsAndFraming(t *testing.T) {
	for _, tc := range []struct {
		body string
		code int
	}{
		{`{"v":1,"id":1,"ok":false,"error":{"code":"unsupported","message":"old"}}`, 4},
		{`{"v":1,"id":1,"ok":false,"error":{"code":"automation-disabled","message":"off"}}`, 2},
		{`{"v":1,"id":1,"ok":true,"result":{"topics":["media.state"]}}`, 1},
		{`{"v":1,"id":1,"ok":true,"result":{"topics":["lock.state","lock.state"]}}`, 1},
		{strings.Repeat("x", lineLimit+1), 1},
	} {
		path := fakeServer(t, func(c net.Conn, _ map[string]any) { fmt.Fprintln(c, tc.body) })
		var out, diag bytes.Buffer
		if code := run([]string{"watch", "--socket", path}, func(string) string { return "" }, "/unused", &out, &diag); code != tc.code {
			t.Errorf("code%d want%d %s", code, tc.code, diag.String())
		}
	}
	topics := map[string]bool{"lock.state": true, "media.state": true}
	for _, body := range []string{
		`{}`, `{"v":2,"event":"lock.state","result":{"locked":true}}`,
		`{"v":1,"event":"unknown","result":{}}`,
		`{"v":1,"event":"lock.state","id":1,"result":{"locked":true}}`,
		`{"v":1,"event":"lock.state","result":{}}`,
		`{"v":1,"event":"media.state","result":{}}`,
		`{"v":1,"event":"media.state","error":{"code":"locked","message":""}}`,
		`{"v":1,"event":"lock.state","result":{"locked":true},"ended":true}`,
		`{"v":1,"event":"lock.state","error":{"code":"capability-off","message":"off"}}`,
		`{"v":1,"event":"media.state","result":{},"error":{"code":"locked","message":"locked"}}`,
	} {
		if _, e := decodeEvent([]byte(body), topics); e == nil {
			t.Error("accepted", body)
		}
	}
}

func TestWatchInterruptAndIdle(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	path := fakeServer(t, func(c net.Conn, _ map[string]any) {
		fmt.Fprintln(c, `{"v":1,"id":1,"ok":true,"result":{"topics":["lock.state"]}}`)
		time.Sleep(callTimeout + 100*time.Millisecond)
		fmt.Fprintln(c, `{"v":1,"event":"lock.state","result":{"locked":true}}`)
		time.Sleep(20 * time.Millisecond)
		cancel()
	})
	var out, diag bytes.Buffer
	if code := runContext(ctx, []string{"watch", "lock", "--socket", path}, func(string) string { return "" }, "/unused", &out, &diag); code != 0 || !strings.Contains(out.String(), "locked") || diag.Len() != 0 {
		t.Fatalf("interrupt code%d output%s err%s", code, out.String(), diag.String())
	}
}
