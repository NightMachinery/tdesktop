package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"strings"
	"time"
	"unicode/utf8"
)

type controlEvent struct {
	Version json.Number     `json:"v"`
	Event   string          `json:"event"`
	Result  json.RawMessage `json:"result"`
	Error   *struct {
		Code    string `json:"code"`
		Message string `json:"message"`
	} `json:"error"`
	Ended bool            `json:"ended"`
	ID    json.RawMessage `json:"id"`
	OK    json.RawMessage `json:"ok"`
}

func readControlLine(reader *bufio.Reader) ([]byte, error) {
	line, err := reader.ReadSlice('\n')
	if err != nil {
		return nil, fmt.Errorf("incomplete or oversized line: %w", err)
	}
	line = line[:len(line)-1]
	if len(line) > lineLimit || !utf8.Valid(line) {
		return nil, errors.New("invalid or oversized UTF-8 line")
	}
	return line, nil
}

func eventExit(code string) int {
	switch code {
	case "automation-disabled", "capability-off", "locked":
		return 2
	case "unsupported":
		return 4
	default:
		return 1
	}
}

func decodeEvent(line []byte, topics map[string]bool) (controlEvent, error) {
	var event controlEvent
	if err := json.Unmarshal(line, &event); err != nil {
		return event, err
	}
	version, err := event.Version.Int64()
	if err != nil || version != 1 || !topics[event.Event] || len(event.ID) != 0 || len(event.OK) != 0 {
		return event, errors.New("invalid event envelope or unexpected topic")
	}
	if event.Error != nil {
		if event.Error.Code == "" || event.Error.Message == "" || len(event.Result) != 0 {
			return event, errors.New("invalid event error")
		}
		if !event.Ended && (event.Event != "media.state" || event.Error.Code != "locked") {
			return event, errors.New("unexpected nonterminal event error")
		}
	} else {
		var object map[string]json.RawMessage
		if event.Ended || json.Unmarshal(event.Result, &object) != nil || object == nil {
			return event, errors.New("invalid event result")
		}
		if _, err := humanEvent(event); err != nil {
			return event, err
		}
	}
	return event, nil
}

func humanEvent(event controlEvent) (string, error) {
	if event.Error != nil {
		return event.Event + "\t" + event.Error.Code + "\n", nil
	}
	if event.Event == "lock.state" {
		var state struct {
			Locked *bool `json:"locked"`
		}
		if json.Unmarshal(event.Result, &state) != nil || state.Locked == nil {
			return "", errors.New("invalid lock state")
		}
		if *state.Locked {
			return "lock.state\tlocked\n", nil
		}
		return "lock.state\tunlocked\n", nil
	}
	text, err := humanMedia(event.Result)
	if err != nil {
		return "", err
	}
	return "media.state\t" + strings.ReplaceAll(strings.TrimSuffix(text, "\n"), "\n", "\t") + "\n", nil
}

func watch(ctx context.Context, path string, o options, out, diagnostic io.Writer) int {
	conn, err := net.DialTimeout("unix", path, callTimeout)
	if err != nil {
		fmt.Fprintln(diagnostic, err)
		return 3
	}
	defer conn.Close()
	done := make(chan struct{})
	defer close(done)
	go func() {
		select {
		case <-ctx.Done():
			conn.Close()
		case <-done:
		}
	}()
	fail := func(code int, err error) int {
		if ctx.Err() != nil {
			return 0
		}
		fmt.Fprintln(diagnostic, err)
		return code
	}
	if err := conn.SetDeadline(time.Now().Add(callTimeout)); err != nil {
		return fail(1, err)
	}
	request, err := json.Marshal(map[string]any{"v": 1, "id": 1, "cmd": "subscribe", "args": o.args})
	if err != nil {
		return fail(1, err)
	}
	if _, err = conn.Write(append(request, '\n')); err != nil {
		return fail(1, err)
	}
	reader := bufio.NewReaderSize(conn, lineLimit+2)
	line, err := readControlLine(reader)
	if err != nil {
		return fail(1, err)
	}
	_, reply, code, err := decodeControlReply(line)
	if err != nil {
		if o.asJSON {
			if _, e := fmt.Fprintln(out, string(line)); e != nil {
				return fail(1, e)
			}
		}
		return fail(code, err)
	}
	var accepted struct {
		Topics []string `json:"topics"`
	}
	if json.Unmarshal(reply.Result, &accepted) != nil {
		return fail(1, errors.New("invalid subscription acknowledgement"))
	}
	wanted := o.args["topics"].([]string)
	topics := map[string]bool{}
	for _, topic := range wanted {
		topics[topic] = true
	}
	if len(accepted.Topics) != len(topics) {
		return fail(1, errors.New("subscription topics mismatch"))
	}
	seen := map[string]bool{}
	for _, topic := range accepted.Topics {
		if !topics[topic] || seen[topic] {
			return fail(1, errors.New("subscription topics mismatch"))
		}
		seen[topic] = true
	}
	if err := conn.SetDeadline(time.Time{}); err != nil {
		return fail(1, err)
	}
	for {
		line, err := readControlLine(reader)
		if err != nil {
			return fail(1, err)
		}
		event, err := decodeEvent(line, topics)
		if err != nil {
			return fail(1, err)
		}
		text := string(line) + "\n"
		if !o.asJSON {
			text, err = humanEvent(event)
			if err != nil {
				return fail(1, err)
			}
		}
		if _, err := io.WriteString(out, text); err != nil {
			return fail(1, err)
		}
		if event.Ended {
			return fail(eventExit(event.Error.Code), errors.New(event.Error.Code+": "+event.Error.Message))
		}
	}
}
