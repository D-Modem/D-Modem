# Code review (October 2026): technical details

This is the technical companion to [Code review (October 2026)](review.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

The review covered the whole tree:

- every function of our own code got a comment saying what it does;
- small and clearly correct fixes were made, with a test where one could
  catch the bug;
- everything else was listed here, under [Open](#open).

`dsp/test/legacy` (the old slmodemd core, then kept for `core_equiv` and
for the original library) was left as it was. It has since left the
tree.

This page is a record of what was fixed at the time. File names are
given as they are now:

- the daemon `slmodemd/` is now `frontend/dmodemd/` (its ctest `slmodemd_root` is
  now `dmodemd_root`);
- the test harnesses `dsp/test/half_*.c` are now `half_*.cpp`, and the
  helpers they shared (`half_dte.h`, `half_stack.h`) are now part of
  `dsp/test/modem_half.hpp`;
- the emulated hardware is in `dsp/test/hw/`.

Where the code has changed again since, a note says so. "slmodemd" on
its own still means Smart Link's original daemon, which D-Modem began
from.

Each entry below gives the file, what was wrong, how it was fixed, and
the test that now checks it, where there is one.

## Fixed

### Data link and core

- **`link/lapm.cpp`**: a received I frame longer than N401 overflowed
  `rx_buf_`, and a frame shorter than its header gave a negative
  `memcpy` size. Test: `LapmBadFrames`.
- **`link/v42bis.cpp`**: STEPUP was allowed past N2, and a codeword at
  or above the dictionary size was not treated as a C-ERROR. Test:
  `V42bisStepup`.
- **`link/v44.cpp`**: STEPUP was accepted once past N2.
- **`link/link.cpp`**: V.42bis kept our N2 and N7 when the remote asked
  for one smaller and one larger. Each is now the smaller of the two
  (`LapmV42bisParameters`). Compression in one direction only is now
  tested too (`LapmOneDirection`).
- **`link/lapm.cpp`**: the P0 direction bits in an XID response were
  read the wrong way round.
- **`link/link.cpp`**: the detection phase's timer kept running after a
  hangup, which could give a CONNECT while hanging up. Test:
  `DetectHangup`.
- **`link/hdlc.cpp`**: a frame of fewer than 4 octets was not counted as
  a size error.
- **`link/lapm.cpp`**: `t401_timeout()` gave up only after N400 + 1
  retransmissions (`> MAX_RTX`; slmodemd had the same). An unanswered
  XID, SABME or DISC went out 7 times, and the data phase polled 6
  times. V.42 (8.3.2.2, 8.4.8) allows N400, now 5. Test: `LapmN400`.
- **`link/lapm.cpp`**: an unsolicited DM with F = 0 while LAPM was idle
  reported the link up, with neither SABME nor UA. V.42 (8.8, Table 9)
  has it ask for the link. It now sends an SABME, until the link has
  been up in the call; after a release the DM is ignored. Test:
  `LapmUnsolicitedDm`.
- **`link/hdlc.cpp`, `link/link.cpp`**: the async and detection phase
  transmitters, and the idle link's marks, gave items of `nbits` bits
  with the bits above them set (stale bits of the queue, or 0xff for
  marks). Only the pumps' own masking kept them off the line. They are
  masked now, as LAPM's were. Test: `BitsMasked`.
- **`link/hdlc.cpp`**: the originator's detection phase had no T400.
  Against a remote without V.42 (no ADP, no flags) it sent ODPs for ever
  and the call never came up. The core runs the phase at the caller by
  default (S-register EC), so this hit every such call. T400 now starts
  with the ODPs, and the originator goes to async when it runs out
  (7.2/V.42). Test: `OdpUnanswered`.
- **`core/modem.cpp`**: `stop_call()` left a caller ID stop armed, which
  then killed the next call.
- **`core/at.cpp`**: `AT+MS=56` (K56flex) answered ERROR. `+MS` takes
  only registered pumps, and no pump registers K56flex, so the call's
  mapping of it to V.90 (`change_dp()`) was never reached. `+MS` now
  takes 56 while V.90 is registered and lists it in `+MS=?`; the call
  runs V.8 offering V.90 (`at.expect`). `dmodem_test`'s `ms56` case had
  sent it with Q1, which hid the ERROR: it ran the default `+MS=92`.
- **`core/at.cpp`**: `AT*B`, `AT\A` and `AT+M<x>` (other than `+MS`)
  answered with the result of the command before them on the line, or
  ERROR at the line's start. Now `*B` and `\A0` to `\A3` answer OK, and
  `\A` with another digit, or an unknown `+M<x>`, answers ERROR (the old
  core did the same; `at.expect`).
- **`core/modem.cpp`**: a call that ended online kept the DTE's data
  that had not gone out, and the AT interpreter later took it for
  commands: a line of it that began with AT ran. It is dropped now. (In
  online command state the buffer holds commands, and they stay.)
  Script: `data_drop`.
- **`core/modem.cpp`**: what the DTE sent after a fax or voice command's
  data ran only when more characters came. It now runs as the command
  ends. Script: `fax_dte`.
- **`core/test/script.cpp`**: `\x` at the end of a script line read past
  the string.
- **`core/test/scripts/at.txt`**: `AT\N3\A2` and `AT\Q` were sent
  without their backslashes (`ATN3A2`, `ATQ`), and the backspace line
  with `b`s (the script language has no `\b`). None of the three was
  being tested.

### Fax

- **`dsp/fax_class1.cpp`**: no zero was stuffed after an FCS that ended
  in five ones, so the frame was lost at the receiver.
- **`dsp/fax_modems.cpp`**: the V.21 receiver did not count
  `carrier_run`, so `+FRH` never answered NO CARRIER.
- **`dsp/fax_class1.cpp`**: `read_dte()` read past `<DLE><ETX>`. In
  `+FTH` the next frame's octets went into the frame waiting to go out;
  in `+FTM` the DTE's next command was thrown away. A frame under 2
  octets, or an empty one, got no result code at all (T.31 ends `+FTH`
  on an empty frame with OK). It now reads a character at a time and
  stops at `<DLE><ETX>`. Test: core_equiv's `FaxDteUntilsCome`.
- **`dsp/t30.cpp`**: a 4-octet frame with a good FCS gave a `memcpy` of
  `(size_t)-1`, and a short FIF a negative length.
- **`dsp/t30.cpp`**: in ECM, the block counter was not reset with a new
  page, so the MCF to a repeated PPS could not be repeated.
- **`dsp/t30.cpp`**: without ECM, a repeated MPS, EOM or EOP (when the
  sender missed our MCF) got RTN, and the page came twice. When no
  page's training has been found since, the MCF now goes again. Test:
  fax_test `T30Lost`, with `DSP_T30_LOSE`.
- **`dsp/t4.cpp`**: in MH, a row of zero-length runs wrote past the
  changes array. Test: `T4.ZeroRunsAreNoRow`.
- **`dsp/t4.cpp`**: TIFF T.4 pages stored in several strips kept only
  the first strip. Test: `T4Libtiff` `*_r_16`.
- **`dsp/t4.cpp`**: `T4Rx::row` used the reference row of a failed
  `malloc`.
- **`dsp/tiff.cpp`**: an IFD chain that links back on itself gave the
  same pages for ever. At most `TIFF_MAX_PAGES` (1024) pages are now
  read. Test: `T4.TiffIfdCycle`.

### Data pumps

- **`dsp/dp_v34.cpp`**: INFO1a symbol rate codes past the table (6 or 7,
  where they are not allowed) indexed `v34_bauds[]`. They now cause a
  retrain.
- **`dsp/dp_v34.cpp`**: `primary_start()` ignored a failing
  `prx.init()` (out of memory). It now returns `DPSTAT_ERROR`.
- **`dsp/dp_v34.cpp`**: the answerer's choice of symbol rate ignored the
  caller's INFO0c (3429 supported, and the carriers it can send on at
  3000 and 3200). It now keeps to them, asking for the high carrier
  where the caller has only that, and the caller retrains on an INFO1a
  outside them. Test: pump_test `V34Info0`, with `DSP_V34_INFO0`.
- **`dsp/dp_v34.cpp`**: rates went down to N = 1 at symbol rates
  without 2400 bit/s, and to N = 1 for V.90's upstream (which starts at
  4800). On a noisy line V.34 chose 3429 baud at 2400 bit/s and failed
  (`v34_snr10`), and a renegotiation could start V.32bis in the middle of
  a call. Now:
  - rates stay at or above the symbol rate's least;
  - a symbol rate without even that is not offered;
  - errors at the least rate cause a retrain;
  - a call that is up retrains instead of falling back.
- **`dsp/v90.cpp`, `dsp/v91.cpp`**: a CP whose constellations cannot
  carry its rate (an empty mask, or a drn too large for the 48-bit
  frames) gave a division by zero or a buffer overflow in the encoders.
  `V90Cp::usable()` now checks a CP. Such CPs (CP/CPt in V.90, CPu/CPus
  in V.92, CP in V.91) are ignored where they are received, and the
  encoders' `init()` leaves a harmless encoder when it fails.
- **`dsp/v92.cpp`**: the least squares scratch matrices and the
  prefilter simulation's buffers were function `static`s, shared by
  calls in different threads (Asterisk, FreeSWITCH, libdmodem). They
  are now per call, on the heap. The TRN1u reference is made once,
  through `pthread_once`.
- **`dsp/v92.cpp`**: a leftover `TMP_LC_MIN` test switch changed the
  constellation.
- **`dsp/v92.cpp`**: a fast parameter exchange in which the prefilter
  stayed at its rate (after a slip's exchange had changed the precoder)
  built its CPd from the first design's gap with the new precoder's
  number of points. `pf_cpd()` also ignored a failing `pf_points()` and
  an `lc` past `V92_LC_MAX`. At the rate it has, the CPd now stays as it
  is, and `pf_cpd()` fails (keeping the old CPd) when the points do not
  fit. Test: `smartlink_v92_slip_fpe`.
- **`dsp/v34_modem.cpp`**: the Ja debug dump read past the DIL's arrays.
- **`dsp/v34_modem.cpp`**: with precoding, a renegotiation on a line
  where the training kept no good equalizer (its mse never under 0.02)
  trained from the equalizer with 1 + H(z) in it, and E put 1 + H(z) in
  again (60 taps). The equalizer that 1 + H(z) goes into is now the one
  to go back to, and it never goes in twice. Test: pump_test
  `V34Precoding`, with `DSP_V34_PRECODE_MIN`.
- **`dsp/v92.cpp`**: a fast parameter exchange without the prefilter
  took its rate from the data mode mse with only the training's margin.
  From 36000 bit/s it went to 38666 or 40000, where the original
  library's B1u came through or not depending on the data before it,
  and `smartlink_v92_fpe_remote_noprefilter` failed now and then. It now
  goes up one step at most, with 2.5 times the margin (as a
  renegotiation up does); otherwise the CPd stays as it is.
- **`dsp/dp_v34.cpp`**: phase 2 had no recovery for a lost INFO0
  (11.2.2/V.34, 9.2.1.2/V.90), except after V.92's quick connect. When
  the remote missed ours, it repeated its own INFO0 and we stayed in our
  tone until the call gave up. The PCTel modem misses our INFO0d at some
  starts of a call, so `pctel_v90_calls` failed about once in six full
  runs. Ours now goes again, back to back, until the remote acknowledges
  it, in both roles. Tests: pump_test `V34LoseInfo0`,
  `agere_34_lose_info0`, `pctel_v90_lose_info0`, `hsf_v90_lose_info0`,
  with `DSP_V34_LOSE_INFO0`.
- **`dsp/dp_v34.cpp`**: repeated INFO0s, and what follows one, could
  pass for an INFO1 frame (sync and CRC right). With our INFO0d lost, an
  HSF call took one for its INFO1a and retrained on its symbol rate. A
  candidate that holds an INFO0 frame is no longer taken for INFO1.
- **`dsp/v90.h`**: `v90_tx_sample()` was declared but never defined.
- **`dsp/dp_fsk.cpp`, `dp_v22.cpp`, `dp_v32.cpp`**: an early return
  (another pump, an error) left the rest of the output block unwritten,
  so whatever the host's buffer held went to the line. The block is now
  cleared first.
- **`dsp/dp_v34.cpp`**: the same: after an early return (V.8 done,
  another pump, an error) the rest of the output block was left
  unwritten. After CJ that was a few ms of whatever an earlier block had
  left there.
- **`dsp/dp_v34.cpp`**: the caller went on to the low speed modulation
  V.8 had agreed on right after CJ, without V.8's 75 ms of silence
  (8.1.2/V.8). The PCTel modem takes CJ only with the gap:
  `pctel_21_open_calls` failed about half the time unpaced (1 in 10 to
  20 in real time), passing only when the stale block above happened to
  hold a V.21 space.
- **`dsp/dp_fsk.cpp`, `dp_v22.cpp`, `dp_v32.cpp`, `dp_v34.cpp`**: the
  answer tone came 3 s after going off hook: the core's answer delay (S92,
  2 s, V.25's silent interval) and then 1 s of the pump's own. The pumps
  now wait V.8's 0.2 s, so the tone comes at 2.2 s (V.25: 1.8 to 2.5 s).
- **`dsp/dsp_glue.cpp`**: caller ID reported a private ('P') number or
  name as out of area ('O').
- **`dsp/dp_dummy.cpp`**: a `malloc` was not checked.
- **`dsp/modem_defs.h`**: `MFMT_BYTES()` used a misspelt macro.

### dmodemd and the PBX modules

The daemon was still called slmodemd at the time.

- **`frontend/dmodemd/dmodem.cpp`**: `dmodem_dial()` with a number of 124
  characters or more read past its buffer. It now returns -1.
- **`frontend/dmodemd/sip.cpp`**: the dial string went into the SIP URI as it
  was: `ATDT5551000` called `sip:T5551000@...` (NO CARRIER from
  Asterisk), and pauses and a formatted number's dashes went along. A
  telephone number is now its digits and DTMF characters only; anything
  else is a user name, taken as it is. Code: `sip::dial_user()`, now in
  `frontend/dmodemd/sip_dial.cpp`. Tests: `sip_dial_test`;
  `DIAL=ATDT frontend/asterisk/test/sip.sh`.
- **`frontend/dmodemd/sip.cpp`**: a `SIP_LOGIN` with an '@' in the password was
  split at the wrong '@'.
- **`frontend/dmodemd/sip.cpp`**: the outgoing call's id was stored only once
  `pjsua_call_make_call()` returned. An INVITE arriving in between (with
  `-L`) took the line and was left ringing, and the call's own
  DISCONNECTED arriving in between was not recognised as ours, so
  nothing cleaned up. Fixed:
  - the line is busy (DIALING) while the call is made, and the call is
    known by its user data until its id is;
  - `audio_attach()` no longer clears a hangup that came after the
    `ringing` check;
  - the port goes into the bridge in the service thread, which alone
    keeps `conf_id`.
- **`frontend/dmodemd/sip.cpp`**: one port served every call, without a group
  lock of its own. From pjproject 2.15 on, the bridge gives such a port
  a group lock in its port pool, whose last reference the port keeps, so
  each call's pool was never freed. And `call_cleanup()` closed the
  socket while the media clock could still use the port, whose
  descriptor the next call could get. Now each call has a port with its
  own pool and group lock. At the call's end its socket is shut down
  (the modem sees the end, and a read the media clock waits in returns)
  and it leaves the bridge; once the bridge is done with it, its
  `on_destroy` closes the socket and frees the pool. Test:
  `CALLS=3 frontend/asterisk/test/sip.sh`.
- *Since then*, `frontend/dmodemd/sip.cpp` has moved from pjproject to libre, so
  the code of the last two entries (`pjsua_call_make_call()`,
  `audio_attach()`, `call_cleanup()`, `conf_id`, `on_destroy`) is gone.
- **`frontend/dmodemd/pipe_modem.cpp`**: `MODEM_REC` called `fclose(NULL)` at
  exit when one file could not be made, and leaked a `FILE` per block.
  The run loop spun at 100% CPU once the line's other end closed.
- **`frontend/dmodemd/pty.cpp`**: the pty master leaked when `grantpt()` or
  `unlockpt()` failed.
- **`frontend/dmodemd/dmodem.cpp`, `core/modem.cpp`**: the network's law went to
  the pumps once, and again only after `dmodem_dial()` or
  `dmodem_answer()`. A later call that the DTE made (ATD, ATA) or the
  core answered (S0) ran on mu-law, whatever the configuration said. The
  core keeps the law now (`set_line_law()`) and gives it to each call's
  pumps. Test: `DmodemCxx.LawForEachCall`.
- **`frontend/dmodemd/pipe_modem.cpp`**: `MODEM_REC` recorded nothing with
  `PIPE_SYNC`, the way the interop tests run `half_open`.
- **`frontend/dmodemd/main.cpp`**: the pty's link was always `/dev/ttySL0`,
  whatever the device, so two daemons took the same link. It became
  `/dev/ttySL<n>`, n being the digits the device's name ends with
  (`/dev/slamr1` gave `/dev/ttySL1`). The link is now named
  `/dev/ttyDM<n>`.
- **`frontend/dmodemd/main.cpp`, `pty.cpp`, `datafile.cpp`**: started as root, the
  daemon went on as nobody before everything that needed root was done.
  - The data file was opened for saving only at the end, which failed
    silently with the usual layout (`/var/lib/slmodem` belonging to
    root).
  - After the DTE hung up (HUPCL), the new pty could not be given its
    group (`dialout`), nor could the link in `/dev` be pointed at it, so
    the link went stale (pointing at another program's pty once the
    number was reused). The same happened at the end, when the link
    could not be removed.

  Now the data file is opened (and created) for reading and writing
  while still root, and saved through that descriptor. The pty's group,
  mode and link are set by a process forked before the privileges are
  dropped, which keeps root's rights for that alone. (The master is
  passed to it on a socket, so it can only touch the pty the daemon
  holds.) That process also removes the link when the daemon ends, even
  if it is killed. Whatever cannot be done is logged. Test:
  `frontend/dmodemd/test/root_test.py` (ctest `dmodemd_root`, as root).
- **`frontend/asterisk/app_dmodem.c`, `frontend/freeswitch/mod_dmodem.c`**: the data the
  link delivered in the block where it came up was thrown away with the
  modem's responses.
- **`frontend/asterisk/app_dmodem.c`**: in exec mode the program was forked with
  `ast_safe_fork(0)`, so Asterisk's reaper could collect it during the
  call, and at the hangup its pid (perhaps another process's by then)
  got SIGHUP. Nothing came after that if the program ignored it. It is
  now forked with `ast_safe_fork(1)`, ours to reap: SIGHUP at the
  hangup, SIGKILL after 5 s, collected, then `ast_safe_fork_cleanup()`,
  as FreeSWITCH's module does. Test:
  `BBS_LINGER=1 frontend/asterisk/test/run.sh AT+MS=34 bbsexec` (a program that
  closes its pty and ignores SIGHUP).
- **`frontend/asterisk/app_dmodem.c`, `frontend/freeswitch/mod_dmodem.c`**: the TCP
  connect to the profile's host (DNS and `connect()`) ran in the
  channel's thread after CONNECT, so the line's audio stopped until it
  was done. With a server slow to take the connection (9 s), D-Modem
  calling over SIP got NO CARRIER. The connect now runs in a thread of
  its own and hands its socket back on a socketpair, while the line goes
  on (what the remote sends waits in the DTE's socketpair). Tests:
  `BBS_CONNECT_DELAY=15 frontend/asterisk/test/sip.sh` and
  `frontend/freeswitch/test/sip.sh`.
- **`frontend/asterisk/app_dmodem.c`**: `DMODEMSTATUS=FAX` was not documented.

### Tests

- **`dsp/test/t30_spandsp.cpp`**: `sp_done` was never reset, so only the
  first case checked spandsp's result.
- **`dsp/test/spandsp_test.cpp`**: `SpandspV17` passed when our receiver
  stopped short of the end.
- **`dsp/test/dmodem_test.cpp`**: `DmodemPair` checked only the first of
  five lines each way.
- **`dsp/test/fax_docs.cpp`**: `same_pages` passed for two files without
  a readable page.
- **`dsp/test/interop_call.cpp`**: the BULK write loop had no time
  limit; the fax relays could hang at the join after a modem died;
  children outlived a dead driver (now `PR_SET_PDEATHSIG`); and a reaped
  pid could get a SIGKILL.
- **`dsp/test/half_hsf.cpp`**: the last character of a ';' comment in an
  .inf value was taken as a byte.
- **`dsp/test/interop_call.cpp`**: a fax page was found by its first 128
  bits only when all of them were right. The original library's V.17
  receiver at 12000 bit/s decides one point wrong about once a page,
  depending on how its 20 ms frames fall on the trainings, and in one of
  those alignments the wrong point is in those bits:
  `smartlink_fax_121_open_sends` failed about half the time. Now 8 of
  the bits may be wrong, and the page's error count includes them.
- **`dsp/test/interop_call.cpp`**: MIDCALL dropped the remote's data
  along with the commands' answers: what came ahead of the escape's OK,
  and what followed `ATO`'s CONNECT in the same read. This made
  `agere_v92_moh_grant` fail once in a full run (data right behind the
  CONNECT).
- **`dsp/test/half_agere.cpp`, `half_sm56.cpp`, `half_pctel.cpp`,
  `half_hsf.cpp`, `half_spandsp.cpp`**: the modem's output went to the
  non-blocking pty with a single `write()`, and what the pty refused (it
  takes some 20 KB ahead of the driver's reading) was lost:
  `agere_v44_agere_calls` lost a block of 4646 octets in the middle of
  Agere's 60000 once in a few full runs, when the driver was late to
  read on a loaded machine. The other way, the SM56 and HSF harnesses
  dropped what they had read from the pty and the modem did not take,
  and the spandsp harness dropped what did not fit its line's queue.
  `half_dte.h` (now the `Pty` buffering in `modem_half.hpp`) keeps both
  until they are taken.
- **`frontend/dmodemd/pipe_modem.cpp`, `dsp/test/legacy/modem_half.cpp`** (the
  latter since removed with `dsp/test/legacy`): with `PIPE_SYNC` the line
  of `half_open` and `half_smartlink` stood still until the first ATD or
  ATA, when its lead silence was written. While it stood, the commands
  were taken as they came, and the call began wherever the scheduler
  had the two modems at that moment. The lead silence is now written
  when they start, as the other harnesses do.
- **`dsp/test/interop_call.cpp`**: the fax relays ran the line on while
  the driver had yet to answer what a modem said. On a loaded machine
  the Agere modem's `+FTH` ended in ERROR now and then, its frame
  seconds of line time after its CONNECT. The relays now hold the line
  from a modem's output until the driver's next poll or write.
- **`dsp/test/half_pctel.cpp`, `interop_test.cpp`**: PCTel's controller
  compares an unterminated copy of `+MS`'s carrier name on its stack, so
  `+MS=V90` took V.90 only when the byte after the copy happened to be
  0. That byte was whatever the harness's own calls had left there:
  adding a print to the harness, or filling the stack with 0, turned
  `pctel_v90_calls` from 50666 to 45333 every time. The harness now
  clears the stack before each call into PCTel's objects. With the name
  matched, `+MS=V90` without a top rate keeps the downstream to 45333,
  so the V.90 tests give one (`+MS=V90,1,300,56000`).
- **`dsp/test/half_agere.cpp`, `half_sm56.cpp`, `half_hsf.cpp`,
  `half_stack.h`** (now `stack_clear()` in `modem_half.hpp`): Agere's,
  SM56's and HSF's modules also read some of their stack before writing
  it (valgrind: the status Agere's V.32 and V.22 pumps report, SM56's
  dial response, two of HSF's engine routines). They found there what
  the harness's earlier calls had left, including its reads and writes
  of the pty. The PCTel harness's stack clear is now shared, and the
  other three harnesses clear the stack before each call into their
  modules as well. `HALF_STACK_FILL` fills it with another byte; their
  tests connect at the same rates with 0, 0xa5 and 0xff. `half_hsf.cpp`
  also gave `prctl(PR_SET_TSC)` only its two arguments, and valgrind
  reported the three after them unset (the kernel does not read them):
  they are now 0.
- **`frontend/asterisk/test/sip_call.py`**: it printed a traceback instead of
  FAIL when the daemon gave no pty or went away.
- **`dsp/test/half_hsf.cpp`**: the INF's values were built in fixed
  buffers that a long value overran. Since the harness became a class
  they are strings, but its subkeys and keys were still read into
  128-byte buffers, so a longer one was cut. They are taken whole now
  (the shipped files read the same).
- **`dsp/test/spandsp_test.cpp`, `spandsp_tx.cpp`, `t30_spandsp.cpp`**:
  - spandsp's T.4 encoders were never freed (one for each run of the
    V.17 page cases, two of them per test, and one per T.4 case), nor
    was one whose first page failed;
  - a V.17 transmitter was left behind when a receiver failed to
    initialise, and a missing spandsp receiver was not checked for;
  - the T.4 cases left their pages and receiver behind;
  - gtest printed the T.30 calls' parameters byte by byte, padding and
    all (valgrind: uninitialised reads).

  Under valgrind, `spandsp_test` now frees every block, with no errors.
- **`dsp/test/loopback.cpp`**:
  - `LoopbackSide::ok()` accepted a call that delivered a few hundred
    right bits and then stalled. It now also wants 90% of what the
    call's rate carries from its CONNECT to the end (the pump tests
    carry 97% at least). Test: `Loopback.StalledCallFails`.
  - The clock offset's resampler started 256 samples behind its input
    and kept 1024. Over a long run at a large offset the reading
    overtook the input and read the ring's stale samples, or fell behind
    it and read nothing (V.21 at 3000 ppm lost its carrier at 27 s, and
    had passed on the bits before). The lag and the ring are now sized
    for the run's drift, unchanged wherever the old size was enough.
    Test: `v21_3000ppm_long`.
- **`dsp/test/netsim.cpp`**: with `NET_PLAIN`, the path down to a 9600
  Hz modem applied the law to the resampler's 9600 Hz output (so every
  sample the modem got was a G.711 level), took `NET_GAIN_UP` for
  `NET_GAIN`, and added the line's noise to the codes. The law is now
  applied to the 8000 Hz samples, as on the raised cosine path, then
  `NET_GAIN`, the resampler and the noise.
- **`dsp/test/half_agere.cpp`, `half_sm56.cpp`, `dsp/test/hw/ich.hpp`**:
  the ICH's interrupt was taken on its rising edge only. An interrupt
  that came up while the Agere library polled the clock (the bus
  masters creep on then, leaving the interrupt for the next sample), or
  before the SM56 module had its ISR, or that the ISR left up after four
  rounds, was never taken, and the bus masters halted at LVI for good.
  It is now level-triggered, as on the PCI bus: taken at each sample
  while it is up, but not from within the ISR or its bottom halves.
  Also, only the HSF harness's bus masters went on once LVI moved past
  the one they had halted at, or kept their place when paused and run
  again (RPBM). Those are the ICH's behaviours, so every harness has
  them now (`hw_test`). None of the calls met the edge case (a count
  over the Agere label found none): the bug was latent.

## Checked and left as they are

- **`dp_v34.cpp` `v90_start()`**: in V.92's short phase 2, INFO1a's bit
  33 names the analogue modem's carrier (Table 19/V.92), and we always
  receive on the low one. The modems that set the bit send on the low
  carrier all the same: the SM56 modem at 3200 baud (measured from the
  line), and the Agere modem at 3429 (which has only one carrier).
  Following the bit makes the SM56 modem retrain. The bit is logged
  ("says: high carrier"), and `sm56_v92_qc` now fails on a retrain after
  short phase 2.

## Open

These were not fixed at the time because they needed a design decision,
more than a local change, or a peer to test against. The review gave
each a severity: *bug*, *likely bug*, *risk* (a failure needs unusual
input or timing) or *cleanup*.

### V.34, V.90, V.92

None left.

### Fax

None left.

### Data link and core

None left.

### dmodemd and the PBX modules

None left.

### Tests

None left.
