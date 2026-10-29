/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    interop_call.h - calls between two modem processes: the interop
 *                     tests' driver (interop_test.cpp) and interop_call's
 *                     (by hand)
 *
 *    Each modem is a program that reads its line's audio from fd 3,
 *    writes it to fd 4 and prints "created <name>: <pty>" (half_open,
 *    half_agere, half_sm56, ...); the driver is their DTE on the ptys.
 *    The environment sets what a call does, as the variables below say
 *    (and passes the rest on to the modems and netsim: DSP_*, NET_*, ...).
 *
 *    MODEM_REALTIME=1 runs the modems in real time.  Without it they run
 *    as fast as they can, a little nicer than the rest, keeping time by
 *    the line's samples; a call's seconds (STREAM, MIDCALL, WAIT,
 *    ANSWER_AFTER, HANGUP_WAIT, the fax's pauses, how long it waits for
 *    the modems) are then the line's, by netsim's clock (NET_CLOCK_FD)
 *    or for fax by the driver's relay of the line.
 *
 */

#ifndef INTEROP_CALL_H
#define INTEROP_CALL_H

#include <string>

namespace interop {

/* A data call: both modems get the ';' separated AT commands proto
 * (PROTO_B=... gives modemB its own), modemB answers, modemA dials, data
 * goes both ways and is checked.  Prints its progress; true if it went
 * through.
 *
 *   NET=<rate A>:<rate B>:<ulaw|alaw>  netsim (NETSIM=<path>) in between
 *   STREAM=<s>        keep the data going for that long, then check it
 *   BULK=<octets>     that much each way: text, binary, repeats, noise
 *   BULK_FILE=<path>  the file's octets each way instead (XON and XOFF
 *                     left out); with BULK or BULK_FILE, "bench:" says
 *                     how long each way took, on the call's clock
 *   CALLS=<n>         that many calls, the data on the last; DIAL=<number>
 *   ANSWER_AFTER=<s>  modemB's ATA that long after modemA's ATD (as it
 *                     rings once A has dialed); 0: both at once
 *   HANGUP_WAIT=<s>   on hook between calls (2)
 *   COUNTRY_A=<name>  modemA's --country (the core's names); COUNTRY_B
 *   NET_RING=<country> netsim puts the call through (with NET: dial
 *                     tone, modemA's digits, ringing; NET_BUSY=1 and
 *                     NET_CONGESTION=1 its busy or congestion tone in
 *                     place of the ringing): the modems'
 *                     hooks to it (MODEM_HOOK_FD=5), modemB in ATS0 for
 *                     RING_ANSWER=<n> rings (1), modemA's ATD alone, and
 *                     a RING from modemB before its CONNECT
 *   RESTART_B=1       modemB a new process for each call after the first
 *   MIDCALL_A=<s>:<cmd>;...  (or _B) escape that modem s seconds into the
 *                     data, give it the commands (WAIT <s> waits), ATO
 *                     (MIDCALL_ATO_A=<cmd>, _B: that instead, as ATO1)
 *   EXPECT_B=<regex>  modemB's log must have it ((?s) allowed, '.' takes
 *                     newlines anyway); EXPECT_A modemA's
 *   REJECT_B=<regex>  and must not have it; REJECT_A modemA's
 *   EXPECT_DROP=1     the call is to end on the way: the data unchecked
 *   EXPECT_FAIL=1     the call is to end before CONNECT, modemA with NO
 *                     CARRIER (EXPECT_A, EXPECT_B say how); or =<result>
 *                     with that (BUSY)
 *   DATA_WAIT=<s>     the moment after CONNECT before the data (0.1)
 *   DATA_RATE=<bit/s> the slowest direction's rate, when the CONNECTs do
 *                     not say it
 *   NET_SYNC=1        netsim's line events count from CONNECT
 *   WAV=<file>        the line through netsim as a stereo WAV
 *   LOG_DIR=<dir>     modemA.log, modemB.log (default: the current dir)
 *   DBG=<level>       the modems' debug level (3)
 *   V80=1             V.80: frames both ways in the framed sub-mode
 *                     instead of the data (the AT commands ask for
 *                     Synchronous Access Mode); V80_FRAMES=<n> (20),
 *                     V80_EXTRA_A=<n> (_B): A's frames come with n
 *                     octets more (a DCE that passes the FCS on)
 *                     V80_STRAY_A=1 (_B): an EM before an EM allowed
 *                     V80_ONLY=A (B): only A's (B's) frames checked;
 *                     V80_PAD_A=<octets> (_B): after each of A's frames
 *   V80=2             V.80: octets both ways in the transparent sub-mode,
 *                     the receivers hunting for the SYN (+ESA=1 or 2 in
 *                     the AT commands; V80_SYN=<hex>, 16 or 1696, its
 *                     octets); V80_HUNT=1: A's DTE's <EM><hunt>, then
 *                     more of B's octets; V80_PAIR_LOST=A (B): no two
 *                     octets that A's (B's) DCE shields in a row;
 *                     V80_IDLE_FIRST=A (B): that DTE's octets may begin
 *                     in the middle of the SYN idle
 *   PIPE_SYNC=<n>     the line samples the modems take commands at (4800;
 *                     AGERE_SYNC, SM56_SYNC, ... default to it)
 */
bool data_call(const std::string &exe_a, const std::string &exe_b,
	       const std::string &proto);

/* A class 1 fax call: modemA sends one page at modulation mod (+FTM's
 * code; V.17 codes take the short train for the page) to modemB, the
 * driver playing T.30 for both; setup are the AT commands after
 * AT+FCLASS=1.  The page is checked (a few bit errors allowed).
 *
 *   LOG_DIR=<dir>     modemA.log, modemB.log (default: the current dir)
 *   RECORD=<dir>      the line both ways (a2b.raw, b2a.raw, 16 bit), and
 *                     the page sent and received (page.tx, page.rx)
 *   SHOW_TCF=1        the TCF's non-zero octets
 *   DIAL=<number>     modemA's ATD's number (none)
 *   SETUP_B=<cmds>    modemB's own AT commands after setup's (AT+FAA=1:
 *                     it answers adaptively, FAX before its CONNECT)
 *   ANSWER_B=<text>   modemB's answer has it before its CONNECT (FAX)
 *   ANSWER_AFTER=<s>  modemB's ATA that long after modemA's ATD (both at
 *                     once by default)
 *   LINE_RATE=<Hz>    unpaced, modemA's sample rate, for the line's clock
 *                     (8000; the original library's: 9600)
 *   MODEM_SPEED=<x>   unpaced, no faster than x times real time (10: the
 *                     driver's answers in time for T.30's turnarounds)
 */
bool fax_call(const std::string &exe_a, const std::string &exe_b, int mod,
	      const std::string &setup = "ATX3");

} // namespace interop

#endif
