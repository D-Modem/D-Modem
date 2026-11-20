# Code review (October 2026): technical details

This page gives the technical details for [Code review (October 2026)](review.md), the plain-English overview. Read the overview first. This page keeps the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

The review covered all of the tree:

- each function of our own code got a comment that tells what it does;
- we made small fixes that were clearly correct, with a test where a test could
  find the bug;
- we listed all other items here, under [Open](#open).

We did not change `dsp/test/legacy` (the old slmodemd core, which we kept at that time for `core_equiv` and
for the original library). It is now removed from the
tree.

This page is a record of the fixes at that time. The file names are
the current names:

- the daemon `slmodemd/` is now `frontend/dmodemd/` (its ctest `slmodemd_root` is
  now `dmodemd_root`);
- the test harnesses `dsp/test/half_*.c` are now `half_*.cpp`, and the
  helpers that they shared (`half_dte.h`, `half_stack.h`) are now part of
  `dsp/test/modem_half.hpp`;
- the emulated hardware is in `dsp/test/hw/`.

Where the code has changed again after the review, a note tells this. "slmodemd"
alone still means the original daemon of Smart Link, from which D-Modem started.

Each entry below gives the file, the problem, the fix, and
the test that now checks it, where there is one.

## Fixed

### Data link and core

- **`link/lapm.cpp`**: a received I frame longer than N401 caused an overflow of
  `rx_buf_`, and a frame shorter than its header gave a negative
  `memcpy` size. Test: `LapmBadFrames`.
- **`link/v42bis.cpp`**: STEPUP was permitted after N2, and a codeword at
  or above the dictionary size was not treated as a C-ERROR. Test:
  `V42bisStepup`.
- **`link/v44.cpp`**: STEPUP was accepted one time after N2.
- **`link/link.cpp`**: V.42bis kept our N2 and N7 when the remote asked
  for one smaller and one larger. Now, each is the smaller of the two
  (`LapmV42bisParameters`). Compression in only one direction is now
  also tested (`LapmOneDirection`).
- **`link/lapm.cpp`**: the P0 direction bits in an XID response were
  read in the opposite sequence.
- **`link/link.cpp`**: the timer of the detection phase continued after a
  hangup. This could give a CONNECT during the hangup. Test:
  `DetectHangup`.
- **`link/hdlc.cpp`**: a frame of fewer than 4 octets was not counted as
  a size error.
- **`link/lapm.cpp`**: `t401_timeout()` stopped only after N400 + 1
  retransmissions (`> MAX_RTX`; slmodemd had the same problem). An unanswered
  XID, SABME or DISC went out 7 times, and the data phase polled 6
  times. V.42 (8.3.2.2, 8.4.8) permits N400, now 5. Test: `LapmN400`.
- **`link/lapm.cpp`**: an unsolicited DM with F = 0 while LAPM was idle
  reported the link up, with neither SABME nor UA. In V.42 (8.8, Table 9),
  the DM asks for the link. Now, LAPM sends an SABME, until the link has
  been up in the call. After a release, LAPM ignores the DM. Test:
  `LapmUnsolicitedDm`.
- **`link/hdlc.cpp`, `link/link.cpp`**: the async and detection phase
  transmitters, and the marks of the idle link, gave items of `nbits` bits
  with the bits above them set (stale bits of the queue, or 0xff for
  marks). Only the masking of the pumps kept them off the line. Now they are
  masked, as the items of LAPM were. Test: `BitsMasked`.
- **`link/hdlc.cpp`**: the detection phase of the originator had no T400.
  Against a remote without V.42 (no ADP, no flags), it sent ODPs for ever,
  and the call never came up. By default, the core runs the phase at the caller
  (S-register EC). Thus, this problem occurred on all such calls. Now, T400 starts
  with the ODPs, and the originator goes to async when T400 expires
  (7.2/V.42). Test: `OdpUnanswered`.
- **`core/modem.cpp`**: `stop_call()` left a caller ID stop armed. This stop
  then stopped the next call.
- **`core/at.cpp`**: `AT+MS=56` (K56flex) answered ERROR. `+MS` accepts
  only registered pumps, and no pump registers K56flex. Thus, the call never got to
  the mapping of K56flex to V.90 (`change_dp()`). Now, `+MS` accepts
  56 while V.90 is registered, and gives it in `+MS=?`. The call
  runs V.8 and offers V.90 (`at.expect`). The `ms56` case of `dmodem_test` had
  sent it with Q1, which hid the ERROR: it ran the default `+MS=92`.
- **`core/at.cpp`**: `AT*B`, `AT\A` and `AT+M<x>` (other than `+MS`)
  answered with the result of the command before them on the line, or
  ERROR at the start of the line. Now, `*B` and `\A0` to `\A3` answer OK, and
  `\A` with a different digit, or an unknown `+M<x>`, answers ERROR (the old
  core did the same; `at.expect`).
- **`core/modem.cpp`**: when a call stopped online, it kept the data of the DTE
  that had not gone out. Later, the AT interpreter accepted this data as
  commands: a line of it that started with AT ran. Now, the modem removes this data. (In
  online command state, the buffer holds commands, and they stay.)
  Script: `data_drop`.
- **`core/modem.cpp`**: the data that the DTE sent after the data of a fax or voice command
  ran only when more characters came. Now, it runs when the command
  stops. Script: `fax_dte`.
- **`core/test/script.cpp`**: `\x` at the end of a script line read past
  the end of the string.
- **`core/test/scripts/at.txt`**: `AT\N3\A2` and `AT\Q` were sent
  without their backslashes (`ATN3A2`, `ATQ`), and the backspace line
  with `b`s (the script language has no `\b`). The tests did not test
  these three items.

### Fax

- **`dsp/fax_class1.cpp`**: no zero was stuffed after an FCS that ended
  in five ones. Thus, the receiver lost the frame.
- **`dsp/fax_modems.cpp`**: the V.21 receiver did not count
  `carrier_run`. Thus, `+FRH` never answered NO CARRIER.
- **`dsp/fax_class1.cpp`**: `read_dte()` read past `<DLE><ETX>`. In
  `+FTH`, the octets of the next frame went into the frame that waited to go out.
  In `+FTM`, the next command of the DTE was lost. A frame of less than 2
  octets, or an empty frame, got no result code (T.31 stops `+FTH`
  on an empty frame with OK). Now, it reads one character at a time and
  stops at `<DLE><ETX>`. Test: the `FaxDteUntilsCome` of core_equiv.
- **`dsp/t30.cpp`**: a 4-octet frame with a good FCS gave a `memcpy` of
  `(size_t)-1`, and a short FIF gave a negative length.
- **`dsp/t30.cpp`**: in ECM, the block counter was not reset with a new
  page. Thus, the MCF to a repeated PPS could not be repeated.
- **`dsp/t30.cpp`**: without ECM, a repeated MPS, EOM or EOP (when the
  sender did not receive our MCF) got RTN, and the page came two times. Now, when the
  receiver has found the training of no page since then, the MCF goes again. Test:
  fax_test `T30Lost`, with `DSP_T30_LOSE`.
- **`dsp/t4.cpp`**: in MH, a row of zero-length runs wrote past the
  changes array. Test: `T4.ZeroRunsAreNoRow`.
- **`dsp/t4.cpp`**: for TIFF T.4 pages stored in many strips, the code kept only
  the first strip. Test: `T4Libtiff` `*_r_16`.
- **`dsp/t4.cpp`**: `T4Rx::row` used the reference row of a failed
  `malloc`.
- **`dsp/tiff.cpp`**: an IFD chain that links back on itself gave the
  same pages for ever. Now, the code reads a maximum of `TIFF_MAX_PAGES` (1024) pages.
  Test: `T4.TiffIfdCycle`.

### Data pumps

- **`dsp/dp_v34.cpp`**: INFO1a symbol rate codes after the end of the table (6 or 7,
  which are not permitted) indexed `v34_bauds[]`. Now, they cause a
  retrain.
- **`dsp/dp_v34.cpp`**: `primary_start()` ignored a failure of
  `prx.init()` (out of memory). Now, it returns `DPSTAT_ERROR`.
- **`dsp/dp_v34.cpp`**: the selection of the symbol rate by the answerer ignored the
  INFO0c of the caller (3429 supported, and the carriers on which it can send at
  3000 and 3200). Now, the answerer obeys them. It asks for the high carrier
  where the caller has only that carrier, and the caller retrains on an INFO1a
  outside them. Test: pump_test `V34Info0`, with `DSP_V34_INFO0`.
- **`dsp/dp_v34.cpp`**: rates went down to N = 1 at symbol rates
  without 2400 bit/s, and to N = 1 for the upstream of V.90 (which starts at
  4800). On a noisy line, V.34 selected 3429 baud at 2400 bit/s and failed
  (`v34_snr10`), and a renegotiation could start V.32bis during
  a call. Now:
  - rates stay at or above the lowest rate of the symbol rate;
  - the modem does not offer a symbol rate that does not have that rate;
  - errors at the lowest rate cause a retrain;
  - a call that is up retrains, and does not fall back.
- **`dsp/v90.cpp`, `dsp/v91.cpp`**: a CP whose constellations cannot
  carry its rate (an empty mask, or a drn too large for the 48-bit
  frames) gave a division by zero or a buffer overflow in the encoders.
  Now, `V90Cp::usable()` checks a CP. The receivers ignore such CPs (CP/CPt in V.90, CPu/CPus
  in V.92, CP in V.91), and when `init()` of the encoders fails,
  it leaves a harmless encoder.
- **`dsp/v92.cpp`**: the least squares scratch matrices and the
  buffers of the prefilter simulation were function `static`s, shared by
  calls in different threads (Asterisk, FreeSWITCH, libdmodem). Now, each
  call has its own, on the heap. The TRN1u reference is made one time,
  through `pthread_once`.
- **`dsp/v92.cpp`**: a `TMP_LC_MIN` test switch that stayed in the code changed the
  constellation.
- **`dsp/v92.cpp`**: in a fast parameter exchange in which the prefilter
  stayed at its rate (after the exchange of a slip had changed the precoder), the code
  built its CPd from the gap of the first design with the number of points of the new
  precoder. `pf_cpd()` also ignored a failure of `pf_points()` and
  an `lc` above `V92_LC_MAX`. Now, at the current rate, the CPd stays
  without changes, and `pf_cpd()` fails (and keeps the old CPd) when the points do not
  fit. Test: `smartlink_v92_slip_fpe`.
- **`dsp/v34_modem.cpp`**: the Ja debug dump read past the arrays of the DIL.
- **`dsp/v34_modem.cpp`**: with precoding, a renegotiation on a line
  where the training kept no good equalizer (its mse never below 0.02)
  trained from the equalizer with 1 + H(z) in it. Then E put 1 + H(z) in
  again (60 taps). Now, the modem goes back to the equalizer into which 1 + H(z) goes,
  and 1 + H(z) never goes in two times. Test: pump_test
  `V34Precoding`, with `DSP_V34_PRECODE_MIN`.
- **`dsp/v92.cpp`**: a fast parameter exchange without the prefilter
  took its rate from the data mode mse with only the margin of the training.
  From 36000 bit/s, it went to 38666 or 40000. At these rates, the B1u of the original
  library came through or not because of the data before it,
  and `smartlink_v92_fpe_remote_noprefilter` sometimes failed. Now, it
  goes up one step at most, with 2.5 times the margin (as a
  renegotiation up does). If not, the CPd stays without changes.
- **`dsp/dp_v34.cpp`**: phase 2 had no recovery for a lost INFO0
  (11.2.2/V.34, 9.2.1.2/V.90), except after the quick connect of V.92. When
  the remote did not receive our INFO0, it repeated its own INFO0, and we stayed in our
  tone until the call stopped. The PCTel modem does not receive our INFO0d at some
  starts of a call. Thus, `pctel_v90_calls` failed about one time in six full
  runs. Now, our INFO0 goes again, back to back, until the remote acknowledges
  it, in the two roles. Tests: pump_test `V34LoseInfo0`,
  `agere_34_lose_info0`, `pctel_v90_lose_info0`, `hsf_v90_lose_info0`,
  with `DSP_V34_LOSE_INFO0`.
- **`dsp/dp_v34.cpp`**: repeated INFO0s, and the signal that follows one, could
  look like an INFO1 frame (correct sync and CRC). With our INFO0d lost, an
  HSF call accepted one as its INFO1a and retrained on its symbol rate. Now, the modem
  does not accept a candidate that holds an INFO0 frame as INFO1.
- **`dsp/v90.h`**: `v90_tx_sample()` was declared but never defined.
- **`dsp/dp_fsk.cpp`, `dp_v22.cpp`, `dp_v32.cpp`**: an early return
  (a different pump, an error) did not write the remaining part of the output block.
  Thus, the old contents of the buffer of the host went to the line. Now, the block is
  cleared first.
- **`dsp/dp_v34.cpp`**: the same problem: after an early return (V.8 complete,
  a different pump, an error), the remaining part of the output block was not
  written. After CJ, that was some ms of the old contents of an earlier block.
- **`dsp/dp_v34.cpp`**: the caller went to the low speed modulation on which
  V.8 had agreed immediately after CJ, without the 75 ms of silence of V.8
  (8.1.2/V.8). The PCTel modem accepts CJ only with the gap.
  `pctel_21_open_calls` failed about half the time unpaced (1 in 10 to
  20 in real time). It passed only when the stale block above
  held a V.21 space by chance.
- **`dsp/dp_fsk.cpp`, `dp_v22.cpp`, `dp_v32.cpp`, `dp_v34.cpp`**: the
  answer tone came 3 s after off hook: the answer delay of the core (S92,
  2 s, the silent interval of V.25) and then 1 s of the pump itself. Now, the pumps
  wait the 0.2 s of V.8. Thus, the tone comes at 2.2 s (V.25: 1.8 to 2.5 s).
- **`dsp/dsp_glue.cpp`**: caller ID reported a private ('P') number or
  name as out of area ('O').
- **`dsp/dp_dummy.cpp`**: a `malloc` was not checked.
- **`dsp/modem_defs.h`**: `MFMT_BYTES()` used a macro with an incorrect spelling.

### dmodemd and the PBX modules

At that time, the name of the daemon was still slmodemd.

- **`frontend/dmodemd/dmodem.cpp`**: `dmodem_dial()` with a number of 124
  characters or more read past its buffer. Now, it returns -1.
- **`frontend/dmodemd/sip.cpp`**: the dial string went into the SIP URI without
  changes. `ATDT5551000` called `sip:T5551000@...` (NO CARRIER from
  Asterisk), and pauses and the dashes of a formatted number also went into the URI. Now, a
  telephone number is only its digits and DTMF characters. All other text is a user name,
  used without changes. Code: `sip::dial_user()`, now in
  `frontend/dmodemd/sip_dial.cpp`. Tests: `sip_dial_test`;
  `DIAL=ATDT frontend/asterisk/test/sip.sh`.
- **`frontend/dmodemd/sip.cpp`**: a `SIP_LOGIN` with an '@' in the password was
  divided at the incorrect '@'.
- **`frontend/dmodemd/sip.cpp`**: the id of the outgoing call was stored only after
  `pjsua_call_make_call()` returned. An INVITE that arrived in that time (with
  `-L`) took the line and continued to ring. Also, the
  DISCONNECTED of the call itself that arrived in that time was not recognised as ours. Thus,
  nothing cleaned up. Fixes:
  - the line is busy (DIALING) while the call is made, and the call is
    known by its user data until its id is known;
  - `audio_attach()` does not clear a hangup that came after the
    `ringing` check;
  - the port goes into the bridge in the service thread, which is the only thread that
    keeps `conf_id`.
- **`frontend/dmodemd/sip.cpp`**: one port served all calls, without its own group
  lock. From pjproject 2.15, the bridge gives such a port
  a group lock in its port pool, and the port keeps the last reference of that lock. Thus,
  the pool of each call was never freed. Also, `call_cleanup()` closed the
  socket while the media clock could still use the port, and the next call could get
  its descriptor. Now, each call has a port with its
  own pool and group lock. At the end of the call, its socket is shut down
  (the modem sees the end, and a read in which the media clock waits returns),
  and the port leaves the bridge. When the bridge has no more use for it, its
  `on_destroy` closes the socket and frees the pool. Test:
  `CALLS=3 frontend/asterisk/test/sip.sh`.
- *After that*, `frontend/dmodemd/sip.cpp` changed from pjproject to libre. Thus,
  the code of the last two entries (`pjsua_call_make_call()`,
  `audio_attach()`, `call_cleanup()`, `conf_id`, `on_destroy`) is removed.
- **`frontend/dmodemd/pipe_modem.cpp`**: `MODEM_REC` called `fclose(NULL)` at
  exit when it could not make one file, and leaked a `FILE` for each block.
  The run loop used 100% CPU after the other end of the line closed.
- **`frontend/dmodemd/pty.cpp`**: the pty master leaked when `grantpt()` or
  `unlockpt()` failed.
- **`frontend/dmodemd/dmodem.cpp`, `core/modem.cpp`**: the law of the network went to
  the pumps one time, and again only after `dmodem_dial()` or
  `dmodem_answer()`. A later call that the DTE made (ATD, ATA) or that the
  core answered (S0) ran on mu-law, independently of the configuration. Now, the
  core keeps the law (`set_line_law()`) and gives it to the pumps of each call.
  Test: `DmodemCxx.LawForEachCall`.
- **`frontend/dmodemd/pipe_modem.cpp`**: `MODEM_REC` recorded nothing with
  `PIPE_SYNC`, the method that the interop tests use to run `half_open`.
- **`frontend/dmodemd/main.cpp`**: the link of the pty was always `/dev/ttySL0`,
  for all devices. Thus, two daemons took the same link. It became
  `/dev/ttySL<n>`, where n is the digits at the end of the name of the device
  (`/dev/slamr1` gave `/dev/ttySL1`). Now, the name of the link is
  `/dev/ttyDM<n>`.
- **`frontend/dmodemd/main.cpp`, `pty.cpp`, `datafile.cpp`**: when the daemon started as root,
  it continued as nobody before it completed all the tasks that must have root.
  - The data file was opened for saving only at the end. This failed
    silently with the usual layout (`/var/lib/slmodem`, which belongs to
    root).
  - After the DTE hung up (HUPCL), the daemon could not give the new pty its
    group (`dialout`), and could not point the link in `/dev` at it. Thus,
    the link became stale (it pointed at the pty of a different program after the
    number was used again). The same occurred at the end, when the daemon could not remove
    the link.

  Now, the daemon opens (and makes) the data file for reading and writing
  while it is still root, and saves through that descriptor. A process forked before the
  privileges are dropped sets the group, mode and link of the pty. This process
  keeps the rights of root only for that task. (The daemon gives the master to it on a socket.
  Thus, it can only change the pty that the daemon holds.) That process also removes the link when the daemon stops, also
  if the daemon is killed. The daemon logs each task that it cannot do. Test:
  `frontend/dmodemd/test/root_test.py` (ctest `dmodemd_root`, as root).
- **`frontend/asterisk/app_dmodem.c`, `frontend/freeswitch/mod_dmodem.c`**: the data that the
  link delivered in the block in which it came up was lost with the
  responses of the modem.
- **`frontend/asterisk/app_dmodem.c`**: in exec mode, the program was forked with
  `ast_safe_fork(0)`. Thus, the reaper of Asterisk could collect it during the
  call, and at the hangup its pid (possibly the pid of a different process at that time)
  got SIGHUP. If the program ignored it, nothing came after that. Now, it
  is forked with `ast_safe_fork(1)`, and the module reaps it: SIGHUP at the
  hangup, SIGKILL after 5 s, collected, then `ast_safe_fork_cleanup()`,
  as the module of FreeSWITCH does. Test:
  `BBS_LINGER=1 frontend/asterisk/test/run.sh AT+MS=34 bbsexec` (a program that
  closes its pty and ignores SIGHUP).
- **`frontend/asterisk/app_dmodem.c`, `frontend/freeswitch/mod_dmodem.c`**: the TCP
  connect to the host of the profile (DNS and `connect()`) ran in the
  thread of the channel after CONNECT. Thus, the audio of the line stopped until it
  was complete. With a server that was slow to accept the connection (9 s), D-Modem
  that called over SIP got NO CARRIER. Now, the connect runs in its own thread
  and gives its socket back on a socketpair, while the line continues
  (the data that the remote sends waits in the socketpair of the DTE). Tests:
  `BBS_CONNECT_DELAY=15 frontend/asterisk/test/sip.sh` and
  `frontend/freeswitch/test/sip.sh`.
- **`frontend/asterisk/app_dmodem.c`**: `DMODEMSTATUS=FAX` was not documented.

### Tests

- **`dsp/test/t30_spandsp.cpp`**: `sp_done` was never reset. Thus, only the
  first case checked the result of spandsp.
- **`dsp/test/spandsp_test.cpp`**: `SpandspV17` passed when our receiver
  stopped before the end.
- **`dsp/test/dmodem_test.cpp`**: `DmodemPair` checked only the first of
  five lines in each direction.
- **`dsp/test/fax_docs.cpp`**: `same_pages` passed for two files without
  a readable page.
- **`dsp/test/interop_call.cpp`**: the BULK write loop had no time
  limit. The fax relays could hang at the join after a modem died.
  Children continued to run after a dead driver (now `PR_SET_PDEATHSIG`). A reaped
  pid could get a SIGKILL.
- **`dsp/test/half_hsf.cpp`**: the last character of a ';' comment in an
  .inf value was accepted as a byte.
- **`dsp/test/interop_call.cpp`**: the test found a fax page by its first 128
  bits only when all of them were correct. The V.17 receiver of the original library
  at 12000 bit/s decides one point incorrectly about one time on each page.
  This depends on how its 20 ms frames fall on the trainings. In one of
  those alignments, the incorrect point is in those bits.
  Thus, `smartlink_fax_121_open_sends` failed about half the time. Now, 8 of
  the bits can be incorrect, and the error count of the page includes them.
- **`dsp/test/interop_call.cpp`**: MIDCALL removed the data of the remote
  together with the answers of the commands: the data that came before the OK of the escape,
  and the data that followed the CONNECT of `ATO` in the same read. Thus,
  `agere_v92_moh_grant` failed one time in a full run (data immediately after the
  CONNECT).
- **`dsp/test/half_agere.cpp`, `half_sm56.cpp`, `half_pctel.cpp`,
  `half_hsf.cpp`, `half_spandsp.cpp`**: the output of the modem went to the
  non-blocking pty with a single `write()`, and the data that the pty refused (it
  accepts approximately 20 KB before the driver reads) was lost.
  `agere_v44_agere_calls` lost a block of 4646 octets in the middle of
  the 60000 of Agere one time in some full runs, when the driver was late to
  read on a loaded machine. In the other direction, the SM56 and HSF harnesses
  removed the data that they had read from the pty and that the modem did not accept,
  and the spandsp harness removed the data that did not fit the queue of its line.
  `half_dte.h` (now the `Pty` buffering in `modem_half.hpp`) keeps the two
  until they are accepted.
- **`frontend/dmodemd/pipe_modem.cpp`, `dsp/test/legacy/modem_half.cpp`** (the
  second file is now removed with `dsp/test/legacy`): with `PIPE_SYNC`, the line
  of `half_open` and `half_smartlink` did not move until the first ATD or
  ATA, when its lead silence was written. While it did not move, the commands
  were accepted as they came, and the call started at the positions where the scheduler
  had the two modems at that time. Now, the lead silence is written
  when they start, as the other harnesses do.
- **`dsp/test/interop_call.cpp`**: the fax relays continued to run the line while
  the driver had not yet answered a message of a modem. On a loaded machine,
  the `+FTH` of the Agere modem sometimes stopped with ERROR, with its frame
  seconds of line time after its CONNECT. Now, the relays hold the line
  from the output of a modem until the next poll or write of the driver.
- **`dsp/test/half_pctel.cpp`, `interop_test.cpp`**: the controller of PCTel
  compares an unterminated copy of the carrier name of `+MS` on its stack. Thus,
  `+MS=V90` selected V.90 only when the byte after the copy was
  0 by chance. That byte held the data that the calls of the harness itself had left there.
  When we added a print to the harness, or filled the stack with 0,
  `pctel_v90_calls` changed from 50666 to 45333 each time. Now, the harness
  clears the stack before each call into the objects of PCTel. When the name
  agrees, `+MS=V90` without a top rate keeps the downstream to 45333.
  Thus, the V.90 tests give one (`+MS=V90,1,300,56000`).
- **`dsp/test/half_agere.cpp`, `half_sm56.cpp`, `half_hsf.cpp`,
  `half_stack.h`** (now `stack_clear()` in `modem_half.hpp`): the modules of Agere,
  SM56 and HSF also read some of their stack before they wrote
  it (valgrind: the status that the V.32 and V.22 pumps of Agere report, the dial response of SM56,
  two of the engine routines of HSF). They found there the data that
  the earlier calls of the harness had left, which included its reads and writes
  of the pty. Now, the stack clear of the PCTel harness is shared, and the
  other three harnesses also clear the stack before each call into their
  modules. `HALF_STACK_FILL` fills it with a different byte. Their
  tests connect at the same rates with 0, 0xa5 and 0xff. `half_hsf.cpp`
  also gave `prctl(PR_SET_TSC)` only its two arguments, and valgrind
  reported the three after them as unset (the kernel does not read them).
  Now, they are 0.
- **`frontend/asterisk/test/sip_call.py`**: it showed a traceback instead of
  FAIL when the daemon gave no pty or stopped.
- **`dsp/test/half_hsf.cpp`**: the values of the INF were built in fixed
  buffers, and a long value caused an overflow. After the harness became a class,
  they are strings, but its subkeys and keys were still read into
  128-byte buffers. Thus, a longer one was cut. Now, they are read completely
  (the supplied files give the same result).
- **`dsp/test/spandsp_test.cpp`, `spandsp_tx.cpp`, `t30_spandsp.cpp`**:
  - the T.4 encoders of spandsp were never freed (one for each run of the
    V.17 page cases, two of them for each test, and one for each T.4 case). Also,
    an encoder whose first page failed was not freed;
  - a V.17 transmitter stayed when a receiver did not
    initialise, and the test did not check for a missing spandsp receiver;
  - the T.4 cases did not free their pages and receiver;
  - gtest showed the parameters of the T.30 calls byte by byte, with the padding
    (valgrind: uninitialised reads).

  Under valgrind, `spandsp_test` now frees all blocks, with no errors.
- **`dsp/test/loopback.cpp`**:
  - `LoopbackSide::ok()` accepted a call that delivered some hundred
    correct bits and then stalled. Now, it also must have 90% of the data that the
    rate of the call carries from its CONNECT to the end (the pump tests
    carry a minimum of 97%). Test: `Loopback.StalledCallFails`.
  - The resampler of the clock offset started 256 samples behind its input
    and kept 1024. Over a long run at a large offset, the reading
    overtook the input and read the stale samples of the ring, or fell behind
    it and read nothing (V.21 at 3000 ppm lost its carrier at 27 s, and
    had passed the bits before). Now, the lag and the ring have a size
    for the drift of the run. The size does not change where the old size was sufficient.
    Test: `v21_3000ppm_long`.
- **`dsp/test/netsim.cpp`**: with `NET_PLAIN`, the path down to a 9600
  Hz modem applied the law to the 9600 Hz output of the resampler (thus, each
  sample that the modem got was a G.711 level), used `NET_GAIN_UP` for
  `NET_GAIN`, and added the noise of the line to the codes. Now, the law is
  applied to the 8000 Hz samples, as on the raised cosine path, then
  `NET_GAIN`, the resampler and the noise.
- **`dsp/test/half_agere.cpp`, `half_sm56.cpp`, `dsp/test/hw/ich.hpp`**:
  the interrupt of the ICH was accepted only on its rising edge. Some
  interrupts were never accepted:
  - An interrupt that came up while the Agere library polled the clock (the bus
    masters then move slowly, and leave the interrupt for the next sample).
  - An interrupt that came before the SM56 module had its ISR.
  - An interrupt that the ISR left up after four rounds.

  Then the bus masters halted at LVI permanently.
  Now, it is level-triggered, as on the PCI bus: accepted at each sample
  while it is up, but not from inside the ISR or its bottom halves.
  Also, only the bus masters of the HSF harness continued after LVI moved past
  the LVI at which they had halted, or kept their position when paused and run
  again (RPBM). These are behaviours of the ICH. Thus, all harnesses have
  them now (`hw_test`). None of the calls met the edge case (a count
  over the Agere label found none): the bug was latent.

## Checked and left as they are

- **`dp_v34.cpp` `v90_start()`**: in the short phase 2 of V.92, bit 33 of INFO1a
  gives the carrier of the analogue modem (Table 19/V.92), and we always
  receive on the low carrier. The modems that set the bit send on the low
  carrier: the SM56 modem at 3200 baud (measured from the
  line), and the Agere modem at 3429 (which has only one carrier).
  When the modem obeys the bit, the SM56 modem retrains. The bit is logged
  ("says: high carrier"), and `sm56_v92_qc` now fails on a retrain after
  short phase 2.

## Open

We did not fix these items at that time, because they needed a design decision,
more than a local change, or a peer for tests. The review gave
each item a severity: *bug*, *likely bug*, *risk* (a failure must have unusual
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
