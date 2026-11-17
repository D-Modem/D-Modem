// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors

// Command x2upgrade-host is the caller's screen of the x2 upgrade host
// (docs/x2-upgrade.md). It does the same work as tools/x2upgrade-host.py.
//
// Run this program on the modem's DTE, for example as an exec profile of
// app_dmodem or dmodem-ari, with the modem set to AT+X2UP=2. Its standard
// input and output are the call. The program shows a banner, waits for the
// modem's +X2UP: line, tells the caller the result and what to do next, and
// then stops. When the program stops, the call ends.
//
// Options:
//
//	-timeout S   seconds to wait for +X2UP: (default 60)
//	-log FILE    add one line for each call to FILE
package main

import (
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

const after = "To use x2, do these steps:\r\n" +
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
		"sets.\r\nNo change was necessary.\r\n\r\n" + after,
	"REFUSED": "Result: your modem did not keep the change.\r\n\r\n" +
		"Your modem must permit feature upgrades. Type ATS30=1 and " +
		"push Enter.\r\nThen call this host again.\r\n",
	"QUERIED": "Result: the host read your modem. It made no change.\r\n",
	"NO REPLY": "Result: your modem did not reply.\r\n\r\n" +
		"This host operates only with a USRobotics Courier that " +
		"has x2 code,\r\nand only with error control (V.42 LAPM). " +
		"Type AT&F1 and push Enter.\r\nThen call this host again.\r\n",
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

// waitReport reads r until a +X2UP: line comes, r ends, or the timeout
// occurs.
func waitReport(r io.Reader, timeout time.Duration) (report, bool) {
	type chunk struct {
		data []byte
		err  error
	}
	ch := make(chan chunk)
	go func() {
		for {
			b := make([]byte, 256)
			n, err := r.Read(b)
			ch <- chunk{b[:n], err}
			if err != nil {
				return
			}
		}
	}()
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	var buf []byte
	for {
		select {
		case <-timer.C:
			return report{}, false
		case c := <-ch:
			buf = append(buf, c.data...)
			// Only complete lines; the rest waits for more octets,
			// or for the end of the input.
			end := bytes.LastIndexAny(buf, "\r\n")
			if c.err != nil {
				end = len(buf)
			}
			if end >= 0 {
				for _, line := range strings.FieldsFunc(string(buf[:end]), isEOL) {
					if rep, ok := parseReport(line); ok {
						return rep, true
					}
				}
				buf = append([]byte(nil), buf[min(end+1, len(buf)):]...)
			}
			if len(buf) > 200 {
				buf = buf[len(buf)-200:]
			}
			if c.err != nil {
				return report{}, false
			}
		}
	}
}

// isEOL tells if r ends a line.
func isEOL(r rune) bool { return r == '\r' || r == '\n' }

// screen writes the result screen for rep (found is false at the timeout).
func screen(w io.Writer, rep report, found bool) {
	if !found {
		io.WriteString(w, messages["NO REPLY"])
	} else {
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
	io.WriteString(w, "\r\nGoodbye.\r\n")
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
	logFile := flag.String("log", "", "add one line for each call to `FILE`")
	flag.Parse()

	io.WriteString(os.Stdout, banner)
	rep, found := waitReport(os.Stdin, time.Duration(*timeout*float64(time.Second)))
	screen(os.Stdout, rep, found)
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
