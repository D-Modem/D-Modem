# Code review (October 2026)

A review of the whole tree: every function of our own code got a comment
saying what it does, small and clearly correct fixes were made (with a
test where one could catch the bug), and the rest is listed here.
`dsp/test/legacy` (the old slmodemd core, kept for `core_equiv`) was left
as it is.

## Fixed

### Data link and core

| Where | What was wrong |
|---|---|
| `link/lapm.cpp` | A received I frame longer than N401 overflowed `rx_buf_`; frames shorter than their header gave a negative `memcpy` size (LapmBadFrames) |
| `link/v42bis.cpp` | STEPUP was allowed past N2, and a codeword ≥ the dictionary size was not a C-ERROR (V42bisStepup) |
| `link/v44.cpp` | STEPUP was accepted once past N2 |
| `link/link.cpp` | V.42bis kept our N2/N7 when the remote asked for one smaller and one larger: each is now the minimum (LapmV42bisParameters); compression in only one direction (LapmOneDirection) |
| `link/lapm.cpp` | The P0 direction bits were read the wrong way round in an XID response |
| `link/link.cpp` | The detection phase's timer kept running after a hangup: a CONNECT while hanging up (DetectHangup) |
| `link/hdlc.cpp` | A frame of fewer than 4 octets was not a size error |
| `link/lapm.cpp` | `t401_timeout()` gave up after N400 + 1 retransmissions (`> MAX_RTX`; slmodemd had the same): an unanswered XID, SABME or DISC went out 7 times, the data phase polled 6 times. V.42 (8.3.2.2, 8.4.8) allows N400: now 5 (LapmN400) |
| `link/lapm.cpp` | An unsolicited DM with F = 0 while LAPM was idle reported the link up, with neither SABME nor UA. V.42 (8.8, Table 9) has it ask for the link: now an SABME, until the link has been up in the call (after a release it is ignored) (LapmUnsolicitedDm) |
| `link/hdlc.cpp`, `link/link.cpp` | The async and detection phase transmitters, and the idle link's marks, gave items of `nbits` bits with the bits above them set (stale ones of the queue, 0xff for marks): only the pumps' own masking kept them off the line. They are masked now, as LAPM's were (BitsMasked) |
| `link/hdlc.cpp` | The originator's detection phase had no T400: against a remote without V.42 (no ADP, no flags) it sent ODPs for ever and the call never came up. The core runs the phase at the caller by default (S-register EC), so this was any such call. T400 now starts with the ODPs, and the originator goes to async when it runs out (7.2/V.42; OdpUnanswered) |
| `core/modem.cpp` | `stop_call()` left a caller ID stop armed, which then killed the next call |
| `core/at.cpp` | `AT*B`, `AT\A` and `AT+M<x>` (other than `+MS`) answered with the result of the command before them on the line, ERROR at its start. Now `*B` and `\A0`..`3` OK, `\A` with another digit and `+M<x>` ERROR (the old core did the same; `at.expect`) |
| `core/modem.cpp` | A call that ended online kept the DTE's data that had not gone out, and the AT interpreter later took it for commands: a line of it that began with AT ran (`data_drop`). It is dropped now (in online command state the buffer holds commands, and they stay) |
| `core/modem.cpp` | What the DTE sent after a fax or voice command's data ran only when more characters came; now as the command ends (`fax_dte`) |
| `core/test/script.cpp` | `\x` at the end of a script line read past the string |
| `core/test/scripts/at.txt` | `AT\N3\A2` and `AT\Q` were sent without their backslashes (`ATN3A2`, `ATQ`), the backspace line with `b`s (the script knows no `\b`): none of the three was tested |

### Fax

| Where | What was wrong |
|---|---|
| `dsp/fax_class1.cpp` | No stuffed zero after an FCS ending in five ones: the frame was lost at the receiver |
| `dsp/fax_modems.cpp` | The V.21 receiver did not count `carrier_run`, so +FRH never answered NO CARRIER |
| `dsp/fax_class1.cpp` | `read_dte()` read past `<DLE><ETX>`: in `+FTH` the next frame's octets went into the frame that was waiting to go out, in `+FTM` the DTE's next command was thrown away; and a frame under 2 octets, or an empty one, got no result code at all (T.31 ends `+FTH` on an empty frame with OK). Now a character at a time, stopping at `<DLE><ETX>` (core_equiv `FaxDteUntilsCome`) |
| `dsp/t30.cpp` | A 4-octet frame with a good FCS gave a `memcpy` of `(size_t)-1`; a short FIF a negative length |
| `dsp/t30.cpp` | ECM: the block counter was not reset with a new page, so the MCF to a repeated PPS could not be repeated |
| `dsp/t30.cpp` | Without ECM a repeated MPS, EOM or EOP (the sender missed our MCF) got RTN, and the page came twice. With no page's training found since, the MCF now goes again (fax_test `T30Lost`, with `DSP_T30_LOSE`) |
| `dsp/t4.cpp` | MH: a row of zero-length runs wrote past the changes array (T4.ZeroRunsAreNoRow) |
| `dsp/t4.cpp` | TIFF T.4 pages in several strips kept only the first strip (T4Libtiff `*_r_16`) |
| `dsp/t4.cpp` | `T4Rx::row` used the reference row of a failed `malloc` |
| `dsp/tiff.cpp` | An IFD chain that links back gave the same pages for ever (T4.TiffIfdCycle); at most `TIFF_MAX_PAGES` (1024) pages are read |

### Data pumps

| Where | What was wrong |
|---|---|
| `dsp/dp_v34.cpp` | INFO1a symbol rate codes past the table (6 or 7 where they are not allowed) indexed `v34_bauds[]`: now a retrain |
| `dsp/dp_v34.cpp` | `primary_start()` ignored a failing `prx.init()` (out of memory): now DPSTAT_ERROR |
| `dsp/dp_v34.cpp` | The answerer's choice of symbol rate ignored the caller's INFO0c (3429 supported, the carriers it can send on at 3000 and 3200): it now keeps to them, asking for the high carrier where the caller has only that, and the caller retrains on an INFO1a outside them (pump_test `V34Info0`, with `DSP_V34_INFO0`) |
| `dsp/dp_v34.cpp` | Rates went down to N = 1 at symbol rates without 2400 bit/s, and to N = 1 for V.90's upstream (it starts at 4800): on a noisy line V.34 chose 3429 baud at 2400 bit/s and failed (`v34_snr10`), and a renegotiation could start V.32bis in the middle of a call. Rates now stay at the symbol rate's least, a symbol rate without even that isn't offered, errors at the least rate retrain, and a call that is up retrains instead of falling back |
| `dsp/v90.cpp`, `v91.cpp` | A CP whose constellations cannot carry its rate (an empty mask, or drn too large for the 48-bit frames) gave a division by zero or a buffer overflow in the encoders. `V90Cp::usable()` checks a CP; such CPs (CP/CPt in V.90, CPu/CPus in V.92, CP in V.91) are now ignored where they are received, and the encoders' `init()` leave a harmless encoder when it fails |
| `dsp/v92.cpp` | The least squares scratch matrices and the prefilter simulation's buffers were function `static`s, shared by calls in different threads (Asterisk, FreeSWITCH, libdmodem); now per call on the heap. The TRN1u reference is made once through `pthread_once` |
| `dsp/v92.cpp` | A leftover `TMP_LC_MIN` test switch changed the constellation |
| `dsp/v92.cpp` | A fast parameter exchange with the prefilter that stayed at its rate (after a slip's exchange had changed the precoder) built its CPd from the first design's gap with the new precoder's number of points; `pf_cpd()` ignored `pf_points()` failing and `lc` past `V92_LC_MAX`. At the rate it has the CPd now stays as it is, and `pf_cpd()` fails (the old CPd kept) when the points don't fit (`smartlink_v92_slip_fpe`) |
| `dsp/v34_modem.cpp` | The Ja debug dump read past the DIL's arrays |
| `dsp/v34_modem.cpp` | With precoding, a renegotiation on a line where the training kept no good equalizer (mse never under 0.02) trained from the equalizer with 1 + H(z) in it, and E put 1 + H(z) in again (60 taps). The equalizer 1 + H(z) goes into is now the one to go back to, and it never goes in twice (pump_test `V34Precoding`, with `DSP_V34_PRECODE_MIN`) |
| `dsp/v92.cpp` | A fast parameter exchange without the prefilter took its rate from the data mode mse with the training's margin and no more: from 36000 bit/s it went to 38666 or 40000, where the original library's B1u came through or not by the data before it, and `smartlink_v92_fpe_remote_noprefilter` failed now and then. It now goes up a step at most, with 2.5 times the margin (as a renegotiation up does), else the CPd stays as it is |
| `dsp/dp_v34.cpp` | Phase 2 had no recovery for a lost INFO0 (11.2.2/V.34, 9.2.1.2/V.90) but after V.92's quick connect: when the remote missed ours it repeated its own INFO0 and we stayed in our tone until the call gave up. The PCTel modem misses our INFO0d at some starts of a call, and `pctel_v90_calls` failed so about once in six full runs. Ours now goes again, back to back, until the remote acknowledges it, in both roles (pump_test `V34LoseInfo0`, `agere_34_lose_info0`, `pctel_v90_lose_info0`, `hsf_v90_lose_info0`, with `DSP_V34_LOSE_INFO0`) |
| `dsp/dp_v34.cpp` | Repeated INFO0s and what follows one could pass for an INFO1 frame (sync and CRC right): with our INFO0d lost, an HSF call took one for its INFO1a and retrained on its symbol rate. A candidate that holds an INFO0 frame is no longer taken for INFO1 |
| `dsp/v90.h` | `v90_tx_sample()` was declared but never defined |
| `dsp/dp_fsk.cpp`, `dp_v22.cpp`, `dp_v32.cpp` | An early return (another pump, an error) left the rest of the output block unwritten: whatever the host's buffer held went to the line. The block is now cleared first |
| `dsp/dp_v34.cpp` | The rest of the output block after an early return (V.8 done, another pump, an error) was left unwritten, as it was in the other pumps: what the host's buffer held went out. After CJ that was a few ms of whatever an earlier block left there |
| `dsp/dp_v34.cpp` | The caller went on to the low speed modulation V.8 agreed on right after CJ, without V.8's 75 ms of silence (8.1.2/V.8). The PCTel modem takes CJ only with the gap: `pctel_21_open_calls` failed about half the time unpaced (1 in 10 to 20 in real time), passing only when the stale block above happened to hold a V.21 space |
| `dsp/dp_fsk.cpp`, `dp_v22.cpp`, `dp_v32.cpp`, `dp_v34.cpp` | The answer tone came 3 s after going off hook: the core's answer delay (S92, 2 s: V.25's silent interval) and then 1 s of the pump's own. The pumps now wait V.8's 0.2 s, the tone at 2.2 s (V.25: 1.8 to 2.5 s) |
| `dsp/dsp_glue.cpp` | Caller ID: a private ('P') number or name was reported as out of area ('O') |
| `dsp/dp_dummy.cpp` | `malloc` not checked |
| `dsp/modem_defs.h` | `MFMT_BYTES()` used a misspelt macro |

### slmodemd and the PBX modules

| Where | What was wrong |
|---|---|
| `slmodemd/dmodem.cpp` | `dmodem_dial()` with a number of 124 characters or more read past its buffer; now -1 |
| `slmodemd/sip.cpp` | `SIP_LOGIN` with an '@' in the password was split at the wrong '@' |
| `slmodemd/sip.cpp` | The outgoing call's id was stored only once `pjsua_call_make_call()` returned: an INVITE in between (with `-L`) took the line and was left ringing, and the call's own DISCONNECTED in between was not ours, so nothing cleaned up. The line is now busy (DIALING) while the call is made, and the call is known by its user data until its id is. `audio_attach()` no longer clears a hangup that came after the `ringing` check. The port goes into the bridge in the service thread, which alone keeps `conf_id` |
| `slmodemd/sip.cpp` | One port for every call, without a group lock of its own: pjproject 2.15 on gives it one in the bridge's port pool, whose last reference the port keeps, so each call's pool was never freed. And `call_cleanup()` closed the socket while the media clock could still use the port, whose descriptor the next call could get. Each call now has a port with its own pool and group lock; at the call's end its socket is shut down (the modem sees the end, a read the media clock waits in returns) and it leaves the bridge, and once the bridge is done with it its `on_destroy` closes the socket and frees the pool (`CALLS=3 asterisk/test/sip.sh`) |
| `slmodemd/pipe_modem.cpp` | `MODEM_REC`: `fclose(NULL)` at exit when one file could not be made, and a `FILE` leaked per block; the run loop spun at 100% CPU once the line's other end closed |
| `slmodemd/pty.cpp` | The master leaked when `grantpt()`/`unlockpt()` failed |
| `slmodemd/main.cpp` | The pty's link was always `/dev/ttySL0`, whatever the device: two slmodemd took the same link. It is `/dev/ttySL<n>` now, n the digits the device's name ends with (`/dev/slamr1`: `/dev/ttySL1`) |
| `slmodemd/main.cpp`, `pty.cpp`, `datafile.cpp` | Started as root, slmodemd went on as nobody before what needed root was done: the data file was opened for the save only at the end, which failed silently with the usual layout (`/var/lib/slmodem` root's); and after the DTE hung up (HUPCL), the new pty could not be given its group (`dialout`) nor the link in `/dev` pointed at it, so the link went stale (another program's pty once the number was taken again), as it did at the end, when it could not be removed. The data file is now opened (and made) for reading and writing while still root, and saved through that descriptor; the pty's group, mode and link are set by a process forked before the privileges go, which keeps root's rights for that alone (the master passed to it on a socket: it can only touch the pty slmodemd holds), and removes the link when slmodemd ends, even killed. What can't be done is logged. `slmodemd/test/root_test.py` (ctest `slmodemd_root`, as root) |
| `asterisk/app_dmodem.c`, `freeswitch/mod_dmodem.c` | The data the link delivered in the block where it came up was thrown away with the modem's responses |
| `asterisk/app_dmodem.c` | In exec mode the program was forked with `ast_safe_fork(0)`, so Asterisk's reaper could collect it during the call, and at the hangup its pid (perhaps another process's by then) got SIGHUP; nothing came after that if the program ignored it. It is forked with `ast_safe_fork(1)` now, ours to reap: SIGHUP at the hangup, SIGKILL after 5 s, collected, then `ast_safe_fork_cleanup()`, as FreeSWITCH's module does (`BBS_LINGER=1 asterisk/test/run.sh AT+MS=34 bbsexec`: a program that closes its pty and ignores SIGHUP) |
| `asterisk/app_dmodem.c`, `freeswitch/mod_dmodem.c` | The TCP connect to the profile's host (DNS and `connect()`) ran in the channel's thread after CONNECT, so the line's audio stopped until it was done: with a server slow to take the connection (9 s), D-Modem calling over SIP got NO CARRIER. It runs in a thread of its own now, its socket handed back on a socketpair, the line going on meanwhile (and what the remote sends waiting in the DTE's socketpair). `BBS_CONNECT_DELAY=15 asterisk/test/sip.sh` and `freeswitch/test/sip.sh` |
| `asterisk/app_dmodem.c` | DMODEMSTATUS=FAX was not documented |

### Tests

| Where | What was wrong |
|---|---|
| `dsp/test/t30_spandsp.cpp` | `sp_done` was never reset, so only the first case checked spandsp's result |
| `dsp/test/spandsp_test.cpp` | SpandspV17 passed when our receiver stopped short of the end |
| `dsp/test/dmodem_test.cpp` | DmodemPair checked only the first of five lines each way |
| `dsp/test/fax_docs.cpp` | `same_pages` passed for two files without a readable page |
| `dsp/test/interop_call.cpp` | The BULK write loop had no time limit; the fax relays could hang at the join after a modem died; children outlived a dead driver (now `PR_SET_PDEATHSIG`); a reaped pid could get a SIGKILL |
| `dsp/test/half_hsf.c` | The last character of a ';' comment in an .inf value was taken as a byte |
| `dsp/test/interop_call.cpp` | A fax page was found by its first 128 bits only when all were right. The original library's V.17 receiver at 12000 bit/s decides one point wrong about once a page, depending on how its 20 ms frames fall on the trainings, and in one of those alignments it is in those bits: `smartlink_fax_121_open_sends` failed about half the time. Now 8 of them may be wrong; the page's error count takes them |
| `dsp/test/interop_call.cpp` | MIDCALL dropped the remote's data with the commands' answers: what came ahead of the escape's OK, and what followed `ATO`'s CONNECT in the same read. `agere_v92_moh_grant` failed so once in a full run (data right behind the CONNECT) |
| `dsp/test/half_agere.c`, `half_sm56.c`, `half_pctel.c`, `half_hsf.c`, `half_spandsp.c` | The modem's output went to the non-blocking pty with one `write()`, and what the pty refused (it takes some 20 KB ahead of the driver's reading) was lost: `agere_v44_agere_calls` lost a block of 4646 octets in the middle of Agere's 60000 once in a few full runs, the driver late to read on a loaded machine. The other way, the SM56 and HSF harnesses dropped what they had read from the pty and the modem did not take, and the spandsp one what did not fit its line's queue. `half_dte.h` keeps both until they are taken |
| `slmodemd/pipe_modem.cpp`, `dsp/test/legacy/modem_half.cpp` | With `PIPE_SYNC` the line of `half_open` and `half_smartlink` stood still until the first ATD or ATA (its lead silence written then): while it stood, the commands were taken as they came, and the call began wherever the scheduler had the two modems at that moment. The lead is now written when they start, as the other harnesses' is |
| `dsp/test/interop_call.cpp` | The fax relays ran the line on while the driver had yet to answer what a modem said: on a loaded machine the Agere modem's `+FTH` ended in ERROR now and then, its frame seconds of the line after its CONNECT. The relays now hold the line from a modem's output until the driver's next poll or write |
| `dsp/test/half_pctel.c`, `interop_test.cpp` | PCTel's controller compares an unterminated copy of `+MS`'s carrier name on its stack, so `+MS=V90` took V.90 only when the byte after the copy happened to be 0. That byte was what the harness's own calls had left: a print added to the harness, or the stack filled with 0, turned `pctel_v90_calls` from 50666 to 45333 every time. The harness now clears the stack before each call into PCTel's objects. With the name matched, `+MS=V90` without a top rate keeps the downstream to 45333, so the V.90 tests give one (`+MS=V90,1,300,56000`) |
| `dsp/test/half_agere.c`, `half_sm56.c`, `half_hsf.c`, `half_stack.h` | Agere's, SM56's and HSF's modules read some of their stack before writing it too (valgrind: the status Agere's V.32 and V.22 pumps report, SM56's dial response, two of HSF's engine routines), and found there what the harness's calls before had left, its reads and writes of the pty among them. The PCTel harness's clear is now `half_stack.h`'s, and the other three clear the stack before each call into their modules as well. `HALF_STACK_FILL` fills it with another byte: their tests connect at the same rates with 0, 0xa5 and 0xff. `half_hsf.c` gave `prctl(PR_SET_TSC)` its two arguments only, and valgrind reported the three after them unset (the kernel does not read them): now 0 |
| `asterisk/test/sip_call.py` | A traceback instead of FAIL when slmodemd gave no pty or went away |
| `dsp/test/netsim.cpp` | With `NET_PLAIN`, the way down to a 9600 Hz modem put the law on the resampler's 9600 Hz output (every sample the modem got was a G.711 level), took `NET_GAIN_UP` for `NET_GAIN`, and added the line's noise to the codes. The law is on the 8000 Hz samples now, as on the raised cosine's way, then `NET_GAIN`, the resampler and the noise |
| `dsp/test/half_agere.cpp`, `half_sm56.cpp`, `hw/ich.hpp` | The ICH's interrupt was taken on its rising edge only: one that came up while the Agere library polled the clock (the bus masters creep on then, the interrupt left for the next sample), or before the SM56 module had its ISR, or that the ISR left up after four rounds, was never taken, and the bus masters halted at LVI for good. It is a level now, as on the PCI bus: taken at each sample while it is up, but not from within the ISR or its bottom halves. And only the HSF harness's bus masters went on once LVI moved past the one they had halted at, or kept their place when paused and run again (RPBM): those are the ICH's, so every harness has them now (`hw_test`). None of the calls met the edge case (a count over the Agere label found none): latent |

## Checked and left as they are

- `dp_v34.cpp` `v90_start()`: in V.92's short phase 2 INFO1a's bit 33
  names the analogue modem's carrier (Table 19/V.92), and we always
  receive on the low one. The modems that set it send on the low one
  all the same (the SM56 modem at 3200 baud, measured from the line;
  the Agere modem at 3429, which has one carrier), and following the bit
  makes the SM56 modem retrain. The bit is in the log ("says: high
  carrier"), and `sm56_v92_qc` now fails on a retrain after short phase
  2.

## Open

Not fixed: they need a design decision, more than a local change, or a
peer to test against. Severity as the review gave it: *bug*, *likely
bug*, *risk* (a failure needs unusual input or timing) or *cleanup*.

### V.34, V.90, V.92

None left.

### Fax

None left.

### Data link and core

None left.

### slmodemd and the PBX modules

None left.

### Tests

- **risk** — `loopback.cpp`: `ok()` accepts a call that delivered 300
  bits and then stalled; `line_resample()` reads stale samples at large
  clock offsets (`dsp_loopback` only).
- **cleanup** — `half_hsf.c` `subst()`/`nvm_load()`: very long values are
  cut or overrun the buffer (not with the shipped files);
  `spandsp_test`/`spandsp_tx` leak spandsp states.
