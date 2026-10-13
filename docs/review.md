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
| `core/modem.cpp` | `stop_call()` left a caller ID stop armed, which then killed the next call |
| `core/test/script.cpp` | `\x` at the end of a script line read past the string |

### Fax

| Where | What was wrong |
|---|---|
| `dsp/fax_class1.cpp` | No stuffed zero after an FCS ending in five ones: the frame was lost at the receiver |
| `dsp/fax_modems.cpp` | The V.21 receiver did not count `carrier_run`, so +FRH never answered NO CARRIER |
| `dsp/t30.cpp` | A 4-octet frame with a good FCS gave a `memcpy` of `(size_t)-1`; a short FIF a negative length |
| `dsp/t30.cpp` | ECM: the block counter was not reset with a new page, so the MCF to a repeated PPS could not be repeated |
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
| `dsp/v34_modem.cpp` | The Ja debug dump read past the DIL's arrays |
| `dsp/v90.h` | `v90_tx_sample()` was declared but never defined |
| `dsp/dp_fsk.cpp`, `dp_v22.cpp`, `dp_v32.cpp` | An early return (another pump, an error) left the rest of the output block unwritten: whatever the host's buffer held went to the line. The block is now cleared first |
| `dsp/dsp_glue.cpp` | Caller ID: a private ('P') number or name was reported as out of area ('O') |
| `dsp/dp_dummy.cpp` | `malloc` not checked |
| `dsp/modem_defs.h` | `MFMT_BYTES()` used a misspelt macro |

### slmodemd and the PBX modules

| Where | What was wrong |
|---|---|
| `slmodemd/dmodem.cpp` | `dmodem_dial()` with a number of 124 characters or more read past its buffer; now -1 |
| `slmodemd/sip.cpp` | `SIP_LOGIN` with an '@' in the password was split at the wrong '@' |
| `slmodemd/pipe_modem.cpp` | `MODEM_REC`: `fclose(NULL)` at exit when one file could not be made, and a `FILE` leaked per block; the run loop spun at 100% CPU once the line's other end closed |
| `slmodemd/pty.cpp` | The master leaked when `grantpt()`/`unlockpt()` failed |
| `asterisk/app_dmodem.c`, `freeswitch/mod_dmodem.c` | The data the link delivered in the block where it came up was thrown away with the modem's responses |
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
| `asterisk/test/sip_call.py` | A traceback instead of FAIL when slmodemd gave no pty or went away |

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

- **likely bug** — `v92.cpp` `pf_cpd()` uses `pf_gap` from an earlier
  precoder after a slip FPE, ignores `pf_points()` failing and does not
  check `lc <= V92_LC_MAX`: an uninitialized or overlong CPd can go out.
  Re-run `pf_rate()` before `pf_cpd()` in `fpe_design()` and keep the old
  CPd on failure.
- **risk** (low confidence) — `v34_modem.cpp`: with precoding, a
  renegotiation without a good saved equalizer applies 1 + H(z) twice.
- **risk** — the FSK, V.22 and V.32 answerers wait 1 s before the answer
  tone; V.25 asks for 1.8 to 2.5 s.

### Fax

- **likely bug** — `t30.cpp` (non-ECM): a repeated MPS/EOP (our MCF was
  lost) is answered with RTN, so the page is received twice. Remember the
  last post-page response and repeat it.
- **risk** — `fax_class1.cpp` `read_dte()`: in FTH, bytes after
  <DLE><ETX> in the same read are appended to the ready frame; a frame
  under 2 octets gets no result code; in FTM the rest of the read is
  dropped.

### Data link and core

- **likely bug** (low confidence) — `lapm.cpp`: an unsolicited DM with
  F = 0 while idle reports the link up without SABME/UA.
- **risk** — `lapm.cpp` `t401_timeout()`: `> MAX_RTX` allows one
  retransmission more than N400 (kept as slmodemd had it).
- **risk** — `hdlc.cpp`: the originator's detection phase has no T400
  (the core never uses it that way).
- **likely bug** — `core/at.cpp`: `AT*B`, `AT\A` and `AT+M<x>` (other
  than `+MS`) answer with the previous command's result. slmodemd does the
  same, and `core/test/scripts/at.txt` checks `AT*B` against the old core
  (`core_equiv`), so a fix has to change that script.
- **cleanup** — `hdlc.cpp`: the async and detector transmitters do not
  mask the bytes to `nbits` (every pump masks them itself).

### slmodemd and the PBX modules

- **likely bug** — `slmodemd/sip.cpp`: the outgoing call's id is stored
  only after `pjsua_call_make_call()` returns, so an INVITE (with `-L`) or
  a DISCONNECTED in between leaves a stuck call; `audio_attach()` clears
  `hangup_req` after the `ringing` check; `conf_id` is shared between
  threads without a lock.
- **risk** — `slmodemd/sip.cpp`: the conference port has no group lock of
  its own, so with pjproject ≥ 2.15 each call leaks the bridge's port
  pool.
- **risk** (low confidence) — `slmodemd/sip.cpp` `call_cleanup()` closes
  the call's socket while the media thread may still use it.
- **risk** — `app_dmodem.c`, `mod_dmodem.c`: the TCP connect to the
  profile's host (DNS and `connect()`) blocks the channel thread after
  CONNECT, so the line's audio stops meanwhile.
- **risk** — `app_dmodem.c`: in exec mode the program's pid gets SIGHUP
  after Asterisk's reaper may have collected it, and nothing escalates to
  SIGKILL.
- **cleanup** — `slmodemd/main.cpp`: the ttySL link is always
  `/dev/ttySL0`, whatever the device's number.
- **risk** — `slmodemd/main.cpp`: the data file is saved after dropping
  root, which fails silently with the usual layout.

### Tests

- **intermittent** — `pctel_21_open_calls`: after V.8 agrees on V.21,
  the PCTel modem sometimes never starts its V.21 answer carrier, and
  our caller times out after 60 s. It failed 2 of 20 runs before this
  review's changes and about 2 of 40 after them.
- **intermittent** — `fax_interop_121_open_sends`: under heavy parallel
  load (`ctest -j8` with the Agere tests) the original library's V.17
  receiver sometimes doesn't find the start of our page; it passes when
  run alone.
- **likely bug** (low confidence) — `interop_call.cpp` MIDCALL: data the
  remote sent around "+++" is dropped with the commands' answers.
- **likely bug** — `half_agere.c`, `half_sm56.c`: an interrupt raised
  while the library polls the clock is never delivered (edge, not level,
  triggered); a bus master halted at LVI does not resume when LVI moves.
- **likely bug** — `netsim.cpp` NET_PLAIN: from an 8000 Hz to a 9600 Hz
  modem, G.711 is applied after the resampler, at 9600 Hz.
- **risk** — `loopback.cpp`: `ok()` accepts a call that delivered 300
  bits and then stalled; `line_resample()` reads stale samples at large
  clock offsets (`dsp_loopback` only).
- **cleanup** — `half_hsf.c` `subst()`/`nvm_load()`: very long values are
  cut or overrun the buffer (not with the shipped files);
  `spandsp_test`/`spandsp_tx` leak spandsp states.
