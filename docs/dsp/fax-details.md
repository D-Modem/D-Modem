# Fax: technical details

This is the technical companion to [Fax](fax.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

Terms used on this page:

- **DTE**: the fax program on the computer, which talks to the modem.
- **T.30**: the fax protocol itself (the frames DIS, DCS, CFR, MCF and so
  on, sent on V.21 at 300 bit/s), with the pages on a faster modulation.
- **ECM**: T.30's error correction mode (T.30 Annex A, T.4 Annex A).
- **T.38**: fax over IP, as IFP packets carried in UDPTL.

## Fax (class 1)

`fax_class1.cpp` implements the T.31 class 1 commands on top of the
modulations in `fax_modems.cpp`:

| Command | What it does |
|---|---|
| `+FTH=3` / `+FRH=3` | Send or receive V.21 channel 2 HDLC frames: flags, bit stuffing, CRC-16, `<DLE>` shielding, `OK` or `ERROR` for each received frame |
| `+FTM=` / `+FRM=` | Send or receive on a fast modulation: 24, 48 (V.27ter); 72, 96 (V.29); 73, 74, 97, 98, 121, 122, 145, 146 (V.17; each even code is the short train) |
| `+FTS=` / `+FRS=` | Send silence for, or wait for silence of, n × 10 ms |

### The start of a call

- The answering side sends CED (2100 Hz for 3 s) and then starts
  `+FTH=3` by itself.
- The calling side sends CNG (0.5 s of 1100 Hz every 3.5 s) while it
  waits, and starts `+FRH=3` when the answer tone is heard.

### Characters from the DTE

Any character from the DTE other than XON and XOFF stops `+FRH`, `+FRM`
and `+FRS` with `OK`, even before their `CONNECT` (T.31 8.3.2, 8.3.4,
8.3.6). efax, for example, aborts a `+FRH` that has waited too long by
sending CAN. While a class 1 command runs, the DTE's characters belong to
the engine (as data, or as that abort character). They are read as
commands again only after the command's result.

The DTE's data for `+FTH` and `+FTM` is read one character at a time, and
nothing is read past `<DLE><ETX>`:

- A frame that follows in the same write waits until the current one has
  gone out.
- Whatever follows the last frame or the `+FTM` data (the next command)
  is left for the command interpreter, which runs it after the `OK`.
- In `+FTH`, an empty frame (`<DLE><ETX>` alone) ends the transmission
  with `OK` (8.3.5/T.31). A frame of one octet goes out as it is.

`core/test/scripts/fax_dte.txt` tries all three; it runs in `core_equiv`
as `CoreScript.FaxDteUntilsCome`.

### The receivers

The receivers find the training by matching the received phase steps
against the known sequence. The V.29 and V.27ter training sequences
repeat every 127 symbols, so the end of the alternations before them
tells the receiver which repetition it is receiving.

A V.17 short train continues from the equalizer of the last long train
at the same rate.

### V.17 after a short train

After a short train, the original library and spandsp go straight on to
the scrambled ones (segment 4), without the 64-symbol bridge that
follows a long train. This transmitter does the same, with its trellis
encoder in the state from which the original library starts segment 4
(the original receiver expects that state).

After a short train, this transmitter's segment 4 is 240 symbols
(100 ms; the original library sends 144). After a long train it is the
standard 48 symbols. After the data, 80 symbols of ones follow. Both
lengths are for the original library's receiver, which processes in
20 ms frames: depending on where its frames fall, it starts the data
before it has converged and ends it early, and the extra ones cover both.

The receiver takes a short train with or without the bridge; the first
symbols after the training tell which. During a bridge it decides on
the nearest training point instead of following the known sequence. The
Agere modem's bridge is not that sequence, and following the wrong
points wound the carrier loop up to a frequency offset of some 3 Hz,
which spoiled the first seconds of data.

### A known quirk of the original receiver at 12000 bit/s

At 12000 bit/s after a short train, the original library's receiver now
and then decides an outer corner of the constellation as the point 8
away in its subset (a coordinate of 7 comes out as -1). That is one wrong
bit on the line, and three after the descrambler.

Our signal is the same every time. Whether and where the error happens
depends only on where the receiver's 20 ms frames fall on the long
training and on the short one. It happens in three of the four
alignments, about once a page.

- Sending the data 1.4 dB lower clears all four alignments, but 0.5 dB
  lower loses the TCF instead.
- Sending TEP before the training (the original library sends 200 ms of
  it) clears two of the three.

So the transmitter keeps V.17's levels and sends no TEP.

### V.27ter and the receive level

V.27ter also leads the data with 100 ms of ones (the original library
sends some 60 ms). With the standard's 8 symbols, the original receiver
lost the start of a page now and then.

In data mode the V.17 receiver corrects a clear error in the data's
power (6 % or more, averaged over some 500 symbols), because the original
library's data constellation comes about 1.4 dB below its training.

### Tests against spandsp

`ctest -L spandsp` (with libspandsp-dev installed) feeds spandsp's V.17
transmitter and T.4 encoder, as Asterisk's `SendFAX` uses them, into
these receivers.

### Class 1 over T.38

`FAX_t38_start()` (`dsplibs.h`) moves the engine from the audio to T.38,
also in the middle of a call (dmodemd's call switches to it). The DTE
sees the same commands and results. The line's signals become IFP
packets (`dsp/t38.h`), sent at the time the signals would take;
`FAX_process()` is still called with samples to keep that time, but its
audio is silence. A transmission in progress starts over on T.38 with
whatever has not been sent yet. `FAX_t38_stop()` goes back to the audio.

| Command | On T.38 |
|---|---|
| `+FTH=3` | The V.21 preamble's indicator, then a second's worth of flags' time, then each of the DTE's frames as it comes. Frames are paced at 300 bit/s in packets of at most seven octets, each ended by `hdlc-fcs-OK` (`hdlc-fcs-OK-sig-end` for the final frame). Accepting a frame that is not the final one is the `CONNECT` for the next, as on the audio. 100 ms after the final frame (or an empty one): no-signal and `OK` |
| `+FTM=` | The modulation's indicator and its training's time, then the DTE's octets at the modulation's rate, bit-reversed the T.38 way, `t4-non-ecm-sig-end` after the last, and no-signal 60 ms later. If the data starts with two HDLC flags, it is the DTE's own HDLC (in ECM, fax software frames the ECM blocks itself). The frames are then taken out of the bits and sent as HDLC data, each ended by `hdlc-fcs-OK` (or `hdlc-fcs-BAD` if the DTE's FCS was wrong), with `hdlc-sig-end` at the end |
| `+FRH=3` | `CONNECT` at the V.21 preamble (or at its first data). Each frame goes to the DTE with an FCS computed here (deliberately spoilt, with `ERROR`, for an `hdlc-fcs-BAD`). `NO CARRIER` when the signal ends without a frame |
| `+FRM=` | `CONNECT` at the first data of a fast modulation. T.4 data goes to the DTE in the DTE's bit order. HDLC frames (ECM) are rebuilt as an HDLC bit stream (flags first, zeros stuffed, the FCS, two closing flags at the end). `NO CARRIER` at the sig-end or no-signal |
| `+FRS=` | Silence is "no signal" on T.38. CED is its indicator after 200 ms of no-signal. No CNG is sent (as with the T.30 machine below) |

Tests:

- `class1_t38_test` (label `quick`) has a DTE of its own go through T.30
  with these commands against our T.30 machine over T.38, with the
  packets' timing. It covers sending and receiving, with and without
  ECM, both packet versions, and a switch to T.38 after a while on the
  audio. The `Class1T38Packets` cases check the packets' fields and
  times.
- `frontend/asterisk/test/efax.sh` runs efax on dmodemd against Asterisk's fax
  over T.38 (dmodemd's `SIP_T38`; see the [README](../../README.md)).

### Class 1.0: V.34 half-duplex

With `+FCLASS=1.0` and `+F34=` (maxp not 0), the core calls `FAX_v34()`
(dsplibs.h) right after `FAX_create()` (not on T.38, nor after adaptive
answer's own answer tone): the engine's line is a V.34 half-duplex
modem (`v34_hdx_create()`, [V.34](v34-details.md)) with the primary
channel between minp and maxp (`v34_hdx_rates()`), V.8 first (CM
calling, ANSam answering, no CNG). Once its control channel is up the
engine writes `+F34:<p>,1` to the tty and gives `CONNECT`; from then on
the DTE's data is T.31's V.34 data mode (Annex B), as HylaFAX speaks it:

- frames each to `<DLE><ETX>` both ways (the DTE's without an FCS, the
  engine's with theirs), `<DLE>` doubled, `<DLE><SUB>` two of them;
  the DTE's go out on the control channel with flags between them;
- `<DLE><0x6B>` from the source DTE: once its control channel frames
  are out the modem turns to the primary channel
  (`v34_hdx_primary()`); the DTE's frames that follow (the ECM block,
  no RCP needed) go on it, `<DLE><0x6D>` after the last;
- to the DTE at either end: `<DLE><0x6B><DLE><0x6F+p>` when the primary
  channel is up, the frames it carries, `<DLE><0x6D><DLE><0x6E>` when
  the control channel (1200 bit/s) is up again (also the first time);
- `<DLE><EOT>` from the DTE drops the modem: `OK`. The line going drops
  it too: `<DLE><EOT>` to the DTE and `NO CARRIER`. Rate renegotiation
  and control channel retrains are not asked for.

Without V.8 at the other end the engine is class 1 again: calling, the
answerer's V.21 flags (or V.8's failure) drop the modem and `+FRH=3`'s
`CONNECT` and frames follow; answering, the ANSam was the CED and
`CONNECT` comes for the DTE's `+FTH=3` frames.

`class1_v34_test` (ctest's `quick`) has a DTE of its own go through
T.30 Annex F with it, sending (to 33600, and capped with maxp 6) and
receiving, against our T.30 machine on V.34 through G.711 with a little
noise, and with a machine without V.34 calling and answering.

## Fax (class 2 and 2.0)

`fax_class2.cpp` implements T.32's class 2.0, and the older class 2 that
fax programs know (the EIA/TIA-592 draft, SP-2388). Its entry points are
`FAX2_*` in `dsplibs.h`. It is part of the T.30 library, so a program
built without that library has no `+FCLASS=2`.

The modem runs the T.30 machine described below, with the class 2 engine
as its host (`t30_config`'s `host`). The engine paces the machine.

**Sending:**

1. After the DIS (`T30_EV_DIS`), the machine waits for `+FDT`, then sends
   the DCS with `+FIS`'s session parameters (`t30_session()`,
   `t30_resume()`).
2. For each page, it waits until the DTE's page has come and been
   decoded (`next_page()` returns 2 until then).
3. It waits for `+FET`'s post-page command (`post_page()`).
4. It reports the CFR (`T30_EV_TRAINED`) and each MCF
   (`T30_EV_PAGE_SENT`).

**Receiving:** the machine runs as it would on its own. The engine keeps
what came in, the session (`T30_EV_TRAINED`) and the pages with their
post-page commands (`T30_EV_POST`), for `ATA`'s results and for each
`+FDR`.

**Page data:** the DTE's pages are MH with EOLs, in `+FBO`'s bit order.
They are decoded into a bitmap and coded again for the line, with the
scan line fill, MR on receipt, and ECM either way. So the DTE never has
to frame ECM.

**Polling:**

- An answerer with `+FLP=1` (`t30_config`'s `polled`) sets DIS bit 9. A
  DTC (with the CIG before it) then makes it the sender, as a caller is
  after the DIS.
- A caller whose remote offers a document polls with `+FDR`
  (`t30_poll()`): it sends CIG and DTC instead of the DCS, and then it is
  the receiver.
- The frames' X bit stays the caller's.

**T.38:** `t30_set_t38()` moves the machine to T.38 (or back) at any
point of the call. Whatever was going out is sent again from its start
on the new path, as `FAX_t38_start()` does for class 1.

**V.34 fax (T.30 Annex F):** a `+FIS` bit rate (br) above 5 (16800 to
33600 bit/s) together with ECM offers V.34 half-duplex (`t30_config`'s
`v34`, `t30_v34()`). V.8 carries the call function: a caller sends, or
polls when `+FSP=1` was set before it dialled (`v34_poll`). `+FIS:` and
`+FCS:` report the V.34 rate as br 6 to 13.

**Tests** (in `core_test`):

- `Classes/CoreFax2.TwoPages/*` sends two pages between two modems in
  class 2.0, class 2, class 2.0 with ECM and the other bit order, on
  T.38 from the start and from the DIS on, and on V.34, with the bitmaps
  compared.
- `Core.Fax2Poll` polls a page, and `Fax2PollV34` does the same on V.34.
- `Core.Fax2Abort` ends a call with `+FKS`.

## Fax machine (T.30)

`t30.cpp` is a group 3 fax machine on these modulations. It is used by
libdmodem's fax and the PBX modules' fax applications. The calling
machine sends a document and the answering one receives it.

### Error correction mode

Both machines offer ECM by default (DIS and DCS bit 27), with frames of
256 octets:

1. The page goes as its MH coding, in blocks of up to 256 HDLC frames
   (FCD) on the fast modulation, with three RCP frames after each block.
2. A PPS follows on V.21, with the page and block counters.
3. The receiver answers MCF, or PPR with a map of the frames to send
   again.
4. After four PPRs for one block, the sender sends CTC (and gets CTR) and
   carries on.

The sender answers RNR with RR, and the receiver answers RR, CTC and EOR
as T.30 says.

The receiver ends a block at its RCP. The V.17 short training of the
next block starts from the receiver's state at the end of this block,
and the V.21 PPS after it would spoil that state.

### Without ECM

The receiver answers each post-page command (MPS, EOM, EOP) with MCF or
RTN.

When the sender misses the MCF, it sends the command again after T4
(3 s). If no page training has arrived since (the fast receiver never
found one), the receiver treats the command as a repeat and answers MCF
again, as spandsp's receiver does. Answering RTN there would make the
sender send again a page the receiver already has.

### V.34 half-duplex (T.30 Annex F)

With `t30_config`'s `v34` (`t30_v34()`), the machine uses the half-duplex
V.34 modem ([V.34's details](v34-details.md#half-duplex-clause-12-for-fax)),
which has the line from the start of the call:

- ANSam takes the place of CED, and V.8's CM that of CNG. The CM carries
  the call function: T.30 transmit, or receive when the caller polls.
- The T.30 frames go on V.34's control channel at 1200 bit/s, with flags
  between them.
- The DIS has bit 6 (V.8) set. The DCS gives no rate (bits 11 to 14 are
  0), and there is no TCF: the receiver answers the DCS with CFR.
- ECM is the only mode. Each block goes on the primary channel
  (`v34_hdx_primary()`), its frames after a few flags, and the PPS
  follows on the control channel once that is back.
- A remote without V.8 gets V.21 and the usual fax modulations. An
  answerer whose ANSam draws no CM within 5 s sends its DIS on V.21, and
  a caller that hears a DIS on V.21 while V.8 is still waiting drops the
  V.34 modem.
- V.8 fixes which side sends on the primary channel, so a poll that V.8
  did not announce (`t30_poll()` with us as the source) is refused.

`t30_v34_test` runs two machines on V.34 back to back through G.711
(two pages at 31200 bit/s), and one machine with V.34 against one
without it, both ways round (they fall back to V.17).

### Tests

- `fax_test`'s `Rates/T30Pair.*` cases put two machines back to back, at
  each rate from 14400 down to 2400, with ECM, and with ECM on a noisy
  line. At 19 dB, ECM sends a few frames again and the page comes out
  right; without ECM, some two thousand rows came out damaged.
- `spandsp_test`'s `Calls/T30Spandsp.*` cases (label `spandsp`) run one
  machine against spandsp's fax machine in both directions, with and
  without ECM, and with line hits.
- `fax_test`'s `Mcf/T30Lost.*` cases lose the receiver's MCF to an EOM,
  an MPS and the last page's EOP.

`DSP_T30_LOSE=<fcf in hex>[:<n>]` sends the nth message (the first by
default) whose last frame has that type as silence, as if lost on the
line, keeping its timing (`8c` is MCF).

## T.38

With `t30_config.t38` set, the same machine is a T.38 terminal. It sends
and receives IFP packets (`dsp/t38.cpp`, in Annex A's encoding) instead
of the modulations.

### A transmission

Each transmission is:

1. A no-signal indicator, then 75 ms.
2. The modulation's indicator, then its training's time (for the V.21
   preamble, a second of flags; for ECM, 200 ms of flags too).
3. The data, paced as the modulation would carry it:
   - HDLC frames without their FCS (seven octets a packet on V.21, as
     T.38 7.5 has it), each ended by `hdlc-fcs-OK` and the last by
     `hdlc-fcs-OK-sig-end`;
   - T.4 data (the TCF's zeros, and the page's bits), ended by
     `t4-non-ecm-sig-end`.
4. 100 ms (HDLC) or 60 ms (T.4), then no-signal.

The octets carry the line's bits with the first bit in the top bit
(7.1.2): the other way round from the frames as T.30 writes them.

CED is its indicator after 200 ms of no-signal. The calling side sends
no CNG. The call has moved to T.38 because it is already known to be a
fax call, and a gateway that plays CNG holds back what follows until it
hears the other machine's V.21, which may be a DIS that has already gone
by.

What comes in is taken the same way: a sig-end field or a no-signal
indicator marks the end of the carrier. `t30_t38_time()` tells the
machine how much time has gone by; the timers are the same as on audio.

### Packet encodings and UDPTL

The IFP packets use the encoding of either:

- the 1998 ASN.1 (T38FaxVersion 0): field types in four bits, with a
  field without data leaving the low half of its octet to the next; or
- the 2002 ASN.1 (versions 1 to 3),

as spandsp and Asterisk have them. In our packets a field without data
always goes last, so both versions read the same.

UDPTL numbers the packets and repeats the previous ones in each packet
(redundancy). On receipt, lost packets are recovered from the next
packet's redundancy, in their order, and a repeated packet is taken only
once. A packet that uses FEC instead of redundancy is read for its
primary packet only.

### Tests

- `t38_test` covers the encodings (`T38Ifp.*`, including Appendix V's
  DIS), UDPTL with packets lost, late and repeated (`T38Udptl.*`), and
  two machines over T.38 (`Nets/T38Pair.*`): versions 0 to 3, 14400 to
  4800 bit/s, with and without ECM, every fifth packet lost, bursts
  longer than the redundancy can cover, and a relay that renumbers the
  packets.
- `spandsp_test`'s `Calls/T38Spandsp.*` cases run ours against spandsp's
  T.38 terminal in both directions.
- Class 1 over T.38 (dmodemd's) is covered in
  [Fax (class 1)](#class-1-over-t38).
