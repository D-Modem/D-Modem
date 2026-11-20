// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors

// Command x2upgrade-host is the caller's screen of the x2 upgrade host
// (docs/x2-upgrade.md). It does the same work as tools/x2upgrade-host.py.
//
// Run this program on the modem's DTE, for example as an exec profile of
// app_dmodem or dmodem-ari. Its standard input and output are the call.
//
// With a control channel (DMODEM_CONTROL, from the exec profile) and the
// modem set to AT+X2UP=1, the program shows the features of the caller's
// Courier. The caller switches Terbo, V.FC and x2 on or off, and applies
// the change. The program then tells the modem to do the change, and shows
// the result.
//
// With no control channel, the modem must be set to AT+X2UP=2 (and the
// features in its profile). The program then only shows the result.
//
// When the program stops, the call ends.
//
// Options:
//
//	-timeout S   seconds to wait for +X2UP: (default 60)
//	-idle S      seconds to wait for a key in the menu (default 300)
//	-log FILE    add one line for each call to FILE
package main

import (
	"bufio"
	"bytes"
	"flag"
	"fmt"
	"io"
	"os"
	"regexp"
	"strconv"
	"strings"
	"time"
)

const banner = "\r\n" +
	"D-Modem x2 upgrade host\r\n" +
	"=======================\r\n" +
	"\r\n" +
	"This host changes the features of an upgradable USRobotics product that\r\n" +
	"has the x2 code. It reads the serial number and the feature record of\r\n" +
	"your modem through the error-corrected link. Then it enables or disables\r\n" +
	"features (Terbo, V.FC, x2) in that record.\r\n" +
	"\r\n" +
	"Please wait. Do not type.\r\n" +
	"\r\n"

const after = "To activate the change, do these steps:\r\n" +
	"  1. Hang up.\r\n" +
	"  2. Type AT&F1S0=0&W and push Enter.\r\n" +
	"  3. Set the power switch of the modem to OFF, then to ON.\r\n" +
	"  4. Type ATI7. Make sure that the Options line shows the change.\r\n"

var messages = map[string]string{
	"ENABLED": "Result: the host enabled the features in your modem.\r\n\r\n" +
		after,
	"DISABLED": "Result: the host disabled the features in your modem." +
		"\r\n\r\n" + after,
	"CHANGED": "Result: the host enabled and disabled features in your " +
		"modem.\r\n\r\n" + after,
	"ALREADY": "Result: your modem already had the features that this host " +
		"sets.\r\nNo change was necessary.\r\n",
	"REFUSED": "Result: your modem did not keep the change.\r\n\r\n" +
		"Your modem must permit feature upgrades. Type ATS30=1 and " +
		"push Enter.\r\nThen call this host again.\r\n",
	"QUERIED": "Result: the host read your modem. It made no change.\r\n",
	"NO REPLY": "Result: your modem did not reply.\r\n\r\n" +
		"This host operates only with a USRobotics Courier that " +
		"has x2 code,\r\nand only with error control (V.42 LAPM). " +
		"Type AT&F1 and push Enter.\r\nThen call this host again.\r\n",
	"UNCHANGED": "Result: you made no change.\r\n",
	"FAILED": "Result: the host could not start the change. Please call " +
		"again later.\r\n",
}

// features are record 2's feature bits, as ATC8 has them.
var features = []struct {
	bit  int
	name string
}{{1, "HST"}, {2, "Fax"}, {4, "Terbo"}, {8, "V.FC/V.34"}, {16, "x2"}}

// featureNames returns the names of the features in a record 2 value,
// for example "HST, x2".
func featureNames(value string) string {
	v, err := strconv.Atoi(value)
	if err != nil {
		return "?"
	}
	var names []string
	for _, f := range features {
		if v&f.bit != 0 {
			names = append(names, f.name)
		}
	}
	if len(names) == 0 {
		return "none"
	}
	return strings.Join(names, ", ")
}

var reportRE = regexp.MustCompile(`^\+X2UP: ([A-Z ]+?)(?:,"([^"]*)")?(?:,(\d+),(\d+))?\s*$`)

// report is one +X2UP: line from the modem.
type report struct {
	result, serial, before, after string
}

// parseReport returns the report in line, if line is a +X2UP: line.
func parseReport(line string) (report, bool) {
	m := reportRE.FindStringSubmatch(strings.TrimSpace(line))
	if m == nil {
		return report{}, false
	}
	return report{m[1], m[2], m[3], m[4]}, true
}

// chunk is one read of the input.
type chunk struct {
	data []byte
	err  error
}

// input is the octets from the caller and the modem, on one reader.
type input struct {
	ch  chan chunk
	buf []byte
	eof bool
}

// newInput starts to read r.
func newInput(r io.Reader) *input {
	in := &input{ch: make(chan chunk)}
	go func() {
		for {
			b := make([]byte, 256)
			n, err := r.Read(b)
			in.ch <- chunk{b[:n], err}
			if err != nil {
				return
			}
		}
	}()
	return in
}

// fill adds more octets to buf. It returns false at the deadline or at
// the end of the input.
func (in *input) fill(deadline <-chan time.Time) bool {
	if in.eof {
		return false
	}
	select {
	case <-deadline:
		return false
	case c := <-in.ch:
		in.buf = append(in.buf, c.data...)
		if c.err != nil {
			in.eof = true
		}
		return true
	}
}

// report reads until a +X2UP: line comes, the input ends, or the timeout
// occurs.
func (in *input) report(timeout time.Duration) (report, bool) {
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	for {
		// Only complete lines; the rest waits for more octets, or for
		// the end of the input.
		end := bytes.LastIndexAny(in.buf, "\r\n")
		if in.eof {
			end = len(in.buf)
		}
		if end >= 0 {
			for _, line := range strings.FieldsFunc(string(in.buf[:end]), isEOL) {
				if rep, ok := parseReport(line); ok {
					at := bytes.Index(in.buf, []byte(line)) + len(line)
					in.buf = append([]byte(nil), in.buf[at:]...)
					return rep, true
				}
			}
			in.buf = append([]byte(nil), in.buf[min(end+1, len(in.buf)):]...)
		}
		if len(in.buf) > 200 {
			in.buf = in.buf[len(in.buf)-200:]
		}
		if !in.fill(timer.C) {
			return report{}, false
		}
	}
}

// key returns the next key that the caller typed (not a space or a line
// end). It returns false at the timeout or at the end of the input.
func (in *input) key(timeout time.Duration) (byte, bool) {
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	for {
		for len(in.buf) > 0 {
			c := in.buf[0]
			in.buf = in.buf[1:]
			if c != ' ' && c != '\r' && c != '\n' && c != 0 {
				return c, true
			}
		}
		if !in.fill(timer.C) {
			return 0, false
		}
	}
}

// waitReport reads r until a +X2UP: line comes, r ends, or the timeout
// occurs.
func waitReport(r io.Reader, timeout time.Duration) (report, bool) {
	return newInput(r).report(timeout)
}

// isEOL tells if r ends a line.
func isEOL(r rune) bool { return r == '\r' || r == '\n' }

// switches are the features that the caller can switch.
var switches = []struct {
	key  byte
	bit  int
	name string
}{{'1', 4, "Terbo"}, {'2', 8, "V.FC/V.34"}, {'3', 16, "x2"}}

// switched is the sum of the bits in switches.
const switched = 4 | 8 | 16

// changes returns the bits to enable and to disable, to go from have to
// want.
func changes(have, want int) (enable, disable int) {
	return want &^ have & switched, have &^ want & switched
}

// menu writes the menu for the features have, with want chosen.
func menu(w io.Writer, have, want int) {
	io.WriteString(w, "\r\nFeatures of your modem:\r\n")
	for _, s := range switches {
		state := "OFF"
		if want&s.bit != 0 {
			state = "ON "
		}
		note := ""
		if (want^have)&s.bit != 0 {
			note = "  (changed)"
		}
		fmt.Fprintf(w, "  %c. %-10s %s%s\r\n", s.key, s.name, state, note)
	}
	io.WriteString(w, "\r\nType 1, 2 or 3 to switch a feature on or off.\r\n"+
		"Type A to apply the changes. Type Q to stop with no change.\r\n"+
		"> ")
}

// control is the control channel of the exec profile (DMODEM_CONTROL).
type control interface {
	// request sends a command line, and returns the answer (OK or
	// ERROR), or "" when no answer came.
	request(line string) string
}

// fileControl is a control channel on a descriptor.
type fileControl struct {
	f     *os.File
	lines chan string
}

// controlFromEnv returns the control channel that DMODEM_CONTROL names,
// or nil.
func controlFromEnv() control {
	fd, err := strconv.Atoi(os.Getenv("DMODEM_CONTROL"))
	if err != nil || fd < 0 {
		return nil
	}
	f := os.NewFile(uintptr(fd), "control")
	if f == nil {
		return nil
	}
	if _, err := f.Stat(); err != nil {
		return nil
	}
	c := &fileControl{f: f, lines: make(chan string)}
	go func() {
		sc := bufio.NewScanner(f)
		for sc.Scan() {
			c.lines <- strings.TrimSpace(sc.Text())
		}
		close(c.lines)
	}()
	return c
}

func (c *fileControl) request(line string) string {
	if _, err := io.WriteString(c.f, line+"\n"); err != nil {
		return ""
	}
	select {
	case a := <-c.lines:
		return a
	case <-time.After(5 * time.Second):
		return ""
	}
}

// result writes the result screen for rep (found is false at the timeout).
func result(w io.Writer, rep report, found bool) {
	if !found {
		io.WriteString(w, messages["NO REPLY"])
		return
	}
	if rep.serial != "" {
		fmt.Fprintf(w, "Serial number: %s\r\n", rep.serial)
	}
	if rep.before != "" {
		fmt.Fprintf(w, "Feature record: %s before, %s after\r\n", rep.before, rep.after)
		fmt.Fprintf(w, "Features now: %s\r\n", featureNames(rep.after))
	}
	msg, ok := messages[rep.result]
	if !ok {
		msg = "Result: " + rep.result + "\r\n"
	}
	io.WriteString(w, "\r\n"+msg)
}

// screen writes the result screen for rep and the last line.
func screen(w io.Writer, rep report, found bool) {
	result(w, rep, found)
	io.WriteString(w, "\r\nGoodbye.\r\n")
}

// session does one call: the features read, the caller's menu, and the
// change. It returns the report for the log (found is false at the
// timeout).
func session(in *input, w io.Writer, ctl control, timeout, idle time.Duration) (report, bool) {
	io.WriteString(w, banner)
	rep, found := in.report(timeout)
	if !found || rep.result != "QUERIED" || rep.after == "" || ctl == nil {
		result(w, rep, found)
		return rep, found
	}
	have, _ := strconv.Atoi(rep.after)
	fmt.Fprintf(w, "Serial number: %s\r\n", rep.serial)
	fmt.Fprintf(w, "Features now: %s\r\n", featureNames(rep.after))
	want := have
	unchanged := report{"UNCHANGED", rep.serial, rep.after, rep.after}
	for {
		menu(w, have, want)
		k, ok := in.key(idle)
		if !ok {
			io.WriteString(w, "\r\n\r\nNo key for too long.\r\n")
			k = 'Q'
		} else {
			fmt.Fprintf(w, "%c\r\n", k)
		}
		if k >= 'a' && k <= 'z' {
			k -= 'a' - 'A'
		}
		switch {
		case k == 'Q' || (k == 'A' && want == have):
			io.WriteString(w, "\r\n"+messages["UNCHANGED"])
			return unchanged, true
		case k == 'A':
			enable, disable := changes(have, want)
			io.WriteString(w, "\r\nPlease wait. Do not type.\r\n\r\n")
			if ctl.request(fmt.Sprintf("X2UP=2,%d,%d", enable, disable)) != "OK" {
				io.WriteString(w, messages["FAILED"])
				return report{"FAILED", rep.serial, rep.after, ""}, true
			}
			res, found := in.report(timeout)
			result(w, res, found)
			if !found {
				res = report{"", rep.serial, rep.after, ""}
			}
			return res, found
		}
		for _, s := range switches {
			if s.key == k {
				want ^= s.bit
			}
		}
	}
}

// logLine is the line that -log adds for one call.
func logLine(now time.Time, rep report, found bool, caller string) string {
	result := rep.result
	if !found {
		result = "TIMEOUT"
	}
	return fmt.Sprintf("%s %s caller=%s serial=%s features=%s->%s\n",
		now.Format("2006-01-02T15:04:05"), result, caller,
		rep.serial, rep.before, rep.after)
}

func main() {
	timeout := flag.Float64("timeout", 60, "seconds to wait for +X2UP:")
	idle := flag.Float64("idle", 300, "seconds to wait for a key in the menu")
	logFile := flag.String("log", "", "add one line for each call to `FILE`")
	flag.Parse()

	rep, found := session(newInput(os.Stdin), os.Stdout, controlFromEnv(),
		time.Duration(*timeout*float64(time.Second)),
		time.Duration(*idle*float64(time.Second)))
	io.WriteString(os.Stdout, "\r\nGoodbye.\r\n")
	if *logFile != "" {
		f, err := os.OpenFile(*logFile, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
		if err != nil {
			fmt.Fprintln(os.Stderr, "x2upgrade-host:", err)
		} else {
			io.WriteString(f, logLine(time.Now(), rep, found, os.Getenv("DMODEM_CALLERID")))
			f.Close()
		}
	}
	time.Sleep(3 * time.Second)
}
