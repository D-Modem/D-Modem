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
		"Features now: HST, Fax, Terbo, V.FC/V.34, x2\r\n",
		"the host enabled the features",
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

// TestFeatureNames decodes record 2 values.
func TestFeatureNames(t *testing.T) {
	for v, want := range map[string]string{"031": "HST, Fax, Terbo, V.FC/V.34, x2",
		"019": "HST, Fax, x2", "000": "none", "x": "?"} {
		if got := featureNames(v); got != want {
			t.Errorf("featureNames(%q) = %q, want %q", v, got, want)
		}
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

// fakeControl keeps the requests, and when a request comes, sends the
// modem's next report on the input.
type fakeControl struct {
	w      io.Writer
	answer string
	report string
	lines  []string
}

func (c *fakeControl) request(line string) string {
	c.lines = append(c.lines, line)
	if c.report != "" {
		go io.WriteString(c.w, c.report)
	}
	return c.answer
}

// runSession runs a session on data, with a control channel that gives
// answer and then the report next (no control channel when answer is "").
func runSession(t *testing.T, data, answer, next string, idle time.Duration) (report, bool, string, *fakeControl) {
	t.Helper()
	pr, pw := io.Pipe()
	t.Cleanup(func() { pw.Close() })
	go io.WriteString(pw, data)
	var out bytes.Buffer
	var ctl control
	fc := &fakeControl{w: pw, answer: answer, report: next}
	if answer != "" {
		ctl = fc
	}
	rep, found := session(newInput(pr), &out, ctl, time.Second, idle)
	return rep, found, out.String(), fc
}

// TestSessionSwitchAndApply: the caller switches x2 on and V.FC off.
func TestSessionSwitchAndApply(t *testing.T) {
	rep, found, out, fc := runSession(t,
		"\r\n+X2UP: QUERIED,\"0009540034268322\",015,015\r\n3\r2\rA\r", "OK",
		"\r\n+X2UP: CHANGED,\"0009540034268322\",015,023\r\n", time.Second)
	if !found || rep != (report{"CHANGED", "0009540034268322", "015", "023"}) {
		t.Fatalf("session = %+v, %v", rep, found)
	}
	if len(fc.lines) != 1 || fc.lines[0] != "X2UP=2,16,8" {
		t.Errorf("control lines = %q", fc.lines)
	}
	for _, s := range []string{
		"Features now: HST, Fax, Terbo, V.FC/V.34\r\n",
		"  3. x2         ON   (changed)\r\n",
		"  2. V.FC/V.34  OFF  (changed)\r\n",
		"Features now: HST, Fax, Terbo, x2\r\n",
		"enabled and disabled",
	} {
		if !strings.Contains(out, s) {
			t.Errorf("output has no %q:\n%s", s, out)
		}
	}
}

// TestSessionNoChange: Q, A with no change, and no key, change nothing.
func TestSessionNoChange(t *testing.T) {
	for _, keys := range []string{"1q", "11a", ""} {
		rep, _, out, fc := runSession(t, "+X2UP: QUERIED,\"1\",019,019\r\n"+keys,
			"OK", "", 100*time.Millisecond)
		if rep.result != "UNCHANGED" || len(fc.lines) != 0 {
			t.Errorf("keys %q: session = %+v, control %q", keys, rep, fc.lines)
		}
		if !strings.Contains(out, "you made no change") {
			t.Errorf("keys %q: output:\n%s", keys, out)
		}
	}
}

// TestSessionControlError: the modem cannot start the change.
func TestSessionControlError(t *testing.T) {
	rep, _, out, fc := runSession(t, "+X2UP: QUERIED,\"1\",003,003\r\n3a",
		"ERROR", "", time.Second)
	if rep.result != "FAILED" || len(fc.lines) != 1 || fc.lines[0] != "X2UP=2,16,0" {
		t.Errorf("session = %+v, control %q", rep, fc.lines)
	}
	if !strings.Contains(out, "could not start the change") {
		t.Errorf("output:\n%s", out)
	}
}

// TestSessionNoControl: with no control channel, the result only.
func TestSessionNoControl(t *testing.T) {
	rep, _, out, _ := runSession(t, "+X2UP: ENABLED,\"7\",003,019\r\n", "", "", time.Second)
	if rep.result != "ENABLED" || strings.Contains(out, "Type 1, 2 or 3") ||
		!strings.Contains(out, "Features now: HST, Fax, x2") {
		t.Errorf("session = %+v, output:\n%s", rep, out)
	}
}

// TestChanges finds the bits to enable and to disable.
func TestChanges(t *testing.T) {
	for _, c := range [][4]int{{15, 23, 16, 8}, {19, 19, 0, 0}, {3, 0, 0, 0}} {
		if e, d := changes(c[0], c[1]); e != c[2] || d != c[3] {
			t.Errorf("changes(%d, %d) = %d, %d", c[0], c[1], e, d)
		}
	}
}
