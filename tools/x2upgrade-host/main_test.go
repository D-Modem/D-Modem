// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors

package main

import (
	"bytes"
	"io"
	"strings"
	"testing"
	"time"
)

// TestParseReport examines the forms of the +X2UP: line.
func TestParseReport(t *testing.T) {
	cases := []struct {
		line string
		want report
		ok   bool
	}{
		{`+X2UP: ENABLED,"0009540034268322",015,031`,
			report{"ENABLED", "0009540034268322", "015", "031"}, true},
		{`+X2UP: ALREADY,"0009540034268322",031,031`,
			report{"ALREADY", "0009540034268322", "031", "031"}, true},
		{`+X2UP: NO REPLY`, report{result: "NO REPLY"}, true},
		{`+X2UP: QUERIED,"",015,015`, report{"QUERIED", "", "015", "015"}, true},
		{"  +X2UP: REFUSED,\"x\",015,015 \r", report{"REFUSED", "x", "015", "015"}, true},
		{`CONNECT 52000/ARQ`, report{}, false},
		{`+X2UP: enabled`, report{}, false},
	}
	for _, c := range cases {
		got, ok := parseReport(c.line)
		if ok != c.ok || got != c.want {
			t.Errorf("parseReport(%q) = %+v, %v; want %+v, %v", c.line, got, ok, c.want, c.ok)
		}
	}
}

// TestWaitReport finds the line among other output, in small reads.
func TestWaitReport(t *testing.T) {
	in := "\r\nCONNECT 52000/ARQ/X2\r\n\r\n+X2UP: ENABLED,\"0009540034268322\",015,031\r\n"
	r := &slowReader{data: []byte(in), step: 5}
	rep, ok := waitReport(r, time.Second)
	if !ok || rep.result != "ENABLED" || rep.after != "031" {
		t.Fatalf("waitReport = %+v, %v", rep, ok)
	}
}

// TestWaitReportEnd stops at the end of the input and at the timeout.
func TestWaitReportEnd(t *testing.T) {
	if _, ok := waitReport(strings.NewReader("NO CARRIER\r\n"), time.Second); ok {
		t.Fatal("found a report in input that has none")
	}
	pr, pw := io.Pipe()
	defer pw.Close()
	start := time.Now()
	if _, ok := waitReport(pr, 50*time.Millisecond); ok {
		t.Fatal("found a report in empty input")
	}
	if time.Since(start) > time.Second {
		t.Fatal("the timeout did not stop the wait")
	}
}

// TestScreen examines the text that the caller sees.
func TestScreen(t *testing.T) {
	var b bytes.Buffer
	screen(&b, report{"ENABLED", "0009540034268322", "015", "031"}, true)
	out := b.String()
	for _, s := range []string{
		"Serial number: 0009540034268322\r\n",
		"Feature record: 015 before, 031 after\r\n",
		"x2 is now enabled",
		"AT&F1S0=0&W",
		"Goodbye.",
	} {
		if !strings.Contains(out, s) {
			t.Errorf("screen output has no %q:\n%s", s, out)
		}
	}
	b.Reset()
	screen(&b, report{}, false)
	if !strings.Contains(b.String(), "did not reply") {
		t.Errorf("timeout screen: %q", b.String())
	}
	b.Reset()
	screen(&b, report{result: "ODD"}, true)
	if !strings.Contains(b.String(), "Result: ODD\r\n") {
		t.Errorf("unknown result screen: %q", b.String())
	}
}

// TestLogLine examines the -log line.
func TestLogLine(t *testing.T) {
	now := time.Date(2026, 11, 14, 12, 0, 0, 0, time.UTC)
	got := logLine(now, report{"ENABLED", "123", "015", "031"}, true, "5551234")
	want := "2026-11-14T12:00:00 ENABLED caller=5551234 serial=123 features=015->031\n"
	if got != want {
		t.Errorf("logLine = %q, want %q", got, want)
	}
	if got := logLine(now, report{}, false, ""); !strings.Contains(got, " TIMEOUT ") {
		t.Errorf("timeout logLine = %q", got)
	}
}

// slowReader gives data step octets at a time.
type slowReader struct {
	data []byte
	step int
}

func (r *slowReader) Read(p []byte) (int, error) {
	if len(r.data) == 0 {
		return 0, io.EOF
	}
	n := r.step
	if n > len(r.data) {
		n = len(r.data)
	}
	n = copy(p, r.data[:n])
	r.data = r.data[n:]
	return n, nil
}

// TestWaitReportLastLine finds a line that has no end of line before
// the end of the input.
func TestWaitReportLastLine(t *testing.T) {
	rep, ok := waitReport(&slowReader{data: []byte("+X2UP: NO REPLY"), step: 4}, time.Second)
	if !ok || rep.result != "NO REPLY" {
		t.Fatalf("waitReport = %+v, %v", rep, ok)
	}
}
