# Fax

## Fax (class 1)

`fax_class1.cpp` implements the T.31 class 1 commands on top of the
modulations in `fax_modems.cpp`:

| Command | |
|---|---|
| `+FTH=3` / `+FRH=3` | V.21 channel 2 HDLC frames: flags, bit stuffing, CRC-16, `<DLE>` shielding, `OK`/`ERROR` per received frame |
| `+FTM=` / `+FRM=` | 24, 48 (V.27ter), 72, 96 (V.29), 73, 74, 97, 98, 121, 122, 145, 146 (V.17, long and short train) |
| `+FTS=` / `+FRS=` | Silence for / wait for silence of n * 10 ms |

The answering side sends CED (2100 Hz for 3 s) and then starts `+FTH=3` by
itself; the calling side sends CNG while it waits for an answer and starts
`+FRH=3` when the answer tone is heard. Any character from the DTE but
XON and XOFF stops `+FRH`, `+FRM` and `+FRS` with `OK`, before their
`CONNECT` too (T.31 8.3; efax aborts a `+FRH` that waited too long with a
CAN): while a class 1 command runs, the DTE's characters are the
engine's, its data or that character, and commands again only after
its result.

The DTE's data for `+FTH` and `+FTM` is read a character at a time, and
nothing after `<DLE><ETX>`: a frame that follows in the same write waits
until this one goes out, and what follows the last frame or the `+FTM`
data (the next command) is left for the command interpreter, which
runs it after the `OK`. In `+FTH` an empty frame, `<DLE><ETX>` alone, ends the transmission
with `OK` (8.3.5/T.31); a frame of one octet goes out as it is.
`core/test/scripts/fax_dte.txt` (core_equiv's `FaxDteUntilsCome`) tries
all three.

The receivers find the training by matching the received phase steps
against the known sequence. The V.29 and V.27ter training sequences repeat
every 127 symbols, so the end of the alternations before them tells which
repetition is being received. A V.17 short train continues from the
equalizer of the last long train at the same rate. After a short train
the original library and spandsp go on with the scrambled ones (segment
4) at once, without the 64 symbol bridge of a long train; so does this
transmitter, with the trellis encoder in the state the original library
starts segment 4 from (its receiver expects it). Its segment 4 is 240
symbols (the original library sends 144) and 80 symbols of ones follow
the data: the original receiver, depending on where its 20 ms frames
fall, starts the data before it has converged and ends it early, and
the ones take both. The receiver takes a short train with or without the
bridge: the first symbols after the training tell which. During a bridge
it decides on the nearest training point instead of the known sequence:
the Agere modem's bridge is not that sequence, and following the wrong
points wound the carrier loop up to a frequency offset of some 3 Hz that
spoiled the first seconds of the data. At 12000 bit/s after a short
train the original receiver decides an outer corner of the constellation
now and then as the point 8 away in its subset (a coordinate of 7 comes
out as -1): one wrong bit on the line, three after the descrambler. Our
signal is the same every time; whether and where it happens depends
only on where the receiver's 20 ms frames fall on the long training and
on the short one, and it does in three of the four alignments, about
once a page. The data 1.4 dB down clears all four (0.5 dB down it loses
the TCF instead), and TEP before the training (the original library
sends 200 ms of it) two of the three, so the transmitter keeps V.17's
levels and no TEP. V.27ter, too, leads the data
with 100 ms of ones (the original library sends some 60 ms): with the 8
symbols of the standard its receiver lost the start of a page now and
then. In data mode the V.17 receiver follows a clear
error in the data's power (6 % or more, over some 500 symbols): the
original library's constellation comes about 1.4 dB below its training.

`ctest -L spandsp` (with libspandsp-dev) puts spandsp's V.17 transmitter
and T.4 encoder, as Asterisk's `SendFAX` uses them, into these receivers.

### Class 1 over T.38

`FAX_t38_start()` (dsplibs.h) puts the engine on T.38 instead of the
audio, also in the middle of a call (dmodemd's call goes over to it):
the same commands and results for the DTE, the line's signals as IFP
packets (`dsp/t38.h`) in their time, which `FAX_process()`'s samples
still give (its audio is silence then). A transmission in progress
starts over on T.38 with what has not gone yet; `FAX_t38_stop()` goes
back to the audio.

- `+FTH=3`: the V.21 preamble's indicator, a second of flags' time, then
  each of the DTE's frames as it comes, paced at 300 bit/s in packets
  of seven octets at most, its end `hdlc-fcs-OK` (`-sig-end` for the
  final one); taking a frame that is not the final one is the `CONNECT`
  for the next, as on the audio. 100 ms after the final frame (or an
  empty one): no-signal and `OK`.
- `+FTM=`: the modulation's indicator and its training's time, then the
  DTE's octets at its rate, the T.38 way round, `t4-non-ecm-sig-end`
  after the last and no-signal 60 ms later. If the data starts with two
  HDLC flags it is the DTE's own HDLC (error correction mode: fax
  software frames the ECM blocks itself): its frames are taken out of
  the bits and go as HDLC data, each with `hdlc-fcs-OK` (`-BAD` if the
  DTE's FCS was wrong), `hdlc-sig-end` at the end.
- `+FRH=3`: `CONNECT` at the V.21 preamble (or its first data), each
  frame to the DTE with an FCS of its own (spoilt, and `ERROR`, for an
  `hdlc-fcs-BAD`), `NO CARRIER` when the signal goes without one.
- `+FRM=`: `CONNECT` at the first data of a fast modulation, T.4 data to
  the DTE the DTE's way round, HDLC frames (ECM) as an HDLC bit stream
  of their own (flags first, zeros stuffed, the FCS, two closing flags
  at the end), `NO CARRIER` at its sig-end or no-signal.
- `+FRS=`: silence is no signal on T.38; CED is its indicator after
  200 ms of no-signal; no CNG (as T.30's machine above).

`class1_t38_test` (ctest's `quick`) has a DTE of its own go through T.30
with these commands, sending and receiving, without and with ECM, both
packet versions, and T.38 after a while on the audio, against our T.30
machine over T.38, with the packets' timing; `asterisk/test/efax.sh`
has efax on dmodemd against Asterisk's fax over T.38 (dmodemd's
`SIP_T38`, [README](../../README.md)).

## Fax (class 2 and 2.0)

`fax_class2.cpp` is T.32's class 2.0, and the older class 2 that fax
programs know (`FAX2_*` in `dsplibs.h`, in the T.30 library: a program
without it has no `+FCLASS=2`). The modem runs the T.30 machine below,
paced by the engine as its host (`t30_config`'s `host`): sending, the
machine waits after the DIS (`T30_EV_DIS`) for `+FDT` to send the DCS
with `+FIS`'s session (`t30_session()`, `t30_resume()`), for each page
(`next_page()` says 2 until the DTE's page has come and been decoded),
and for `+FET`'s post-page command (`post_page()`); it tells the CFR
(`T30_EV_TRAINED`) and each MCF (`T30_EV_PAGE_SENT`). Receiving, it runs
as it would by itself and the engine keeps what came, the session
(`T30_EV_TRAINED`) and the pages with their post-page commands
(`T30_EV_POST`), for `ATA`'s results and each `+FDR`. The DTE's pages
are MH with EOLs in `+FBO`'s bit order; they are decoded into a bitmap
and coded again for the line (with the scan line fill, MR received, ECM
either way), so the DTE never frames ECM.

Polling: an answerer with `+FLP=1` (`t30_config`'s `polled`) sets DIS
bit 9; a DTC (and the CIG before it) makes it the sender, as a caller
is after the DIS. A caller whose remote offers a document polls with
`+FDR` (`t30_poll()`): CIG and DTC instead of the DCS, then it is the
receiver. The frames' X bit stays the caller's. T.38: `t30_set_t38()`
moves the machine to T.38 (or back) at any point of the call, what was
going out sent again from its start there, as `FAX_t38_start()` does
for class 1.

`core_test`'s `CoreFax2` cases send two pages between two modems in
class 2.0, class 2, class 2.0 with ECM and the other bit order, and on
T.38 from the start and from the DIS on, the bitmaps compared;
`Fax2Poll` polls a page; `Fax2Abort` ends a call with `+FKS`.

## Fax machine (T.30)

`t30.cpp` is a group 3 fax machine on these modulations (libdmodem's fax,
the modules' fax applications): the calling machine sends a document,
the answering one receives it. With error correction mode (T.30 Annex A,
T.4 Annex A), which both offer by default (DIS and DCS bit 27, frames of
256 octets), the page goes as its MH coding in blocks of up to 256 HDLC
frames (FCD) on the fast modulation, three RCP after each, then PPS on
V.21 with the page and block counters; the receiver answers MCF or PPR
with a map of the frames to send again. After four PPRs for a block the
sender asks CTC (CTR) and goes on; it answers RNR with RR, and the
receiver its RR, CTC and EOR as T.30 says. The receiver ends a block at
its RCP: the V.17 short training of the next block starts from the
receiver's state at the end of this one, and the V.21 PPS after it would
spoil that.

Without ECM the receiver answers each post-page command (MPS, EOM, EOP)
with MCF or RTN. When the sender misses the MCF it sends the command
again after T4 (3 s); if no page's training came since (the fast
receiver never found one) that is a repeat, and it gets MCF again, as
spandsp's receiver does. RTN there would have the sender send the page
the receiver already has once more.

`fax_test`'s `T30Pair` cases put two of them back to back, at each rate,
with ECM, and with ECM on a noisy line (at 19 dB ECM sends a few frames
again and the page comes right; without it two thousand rows come
damaged); `spandsp_test`'s `T30Spandsp` cases (label `spandsp`) run one
against spandsp's fax machine both ways, with and without ECM, and with
line hits. The `T30Lost` cases lose the receiver's MCF to an EOM, an MPS
and the last page's EOP. `DSP_T30_LOSE=<fcf in hex>[:<n>]` sends the nth
message (the first by default) whose last frame is that type as silence,
lost on the line, its timing kept (`8c` is MCF).

## T.38

With `t30_config.t38` the same machine is a T.38 terminal: it sends and
takes IFP packets (`dsp/t38.cpp`, T.38 Annex A) instead of the
modulations. Each transmission is a no-signal indicator and 75 ms, the
modulation's indicator and its training's time (the V.21 preamble: a
second of flags; ECM: 200 ms of them too), then the data, paced as the
modulation would carry it: HDLC frames without their FCS (seven octets a
packet on V.21, as T.38 7.5 has it), each ended by `hdlc-fcs-OK` and the
last by `hdlc-fcs-OK-sig-end`; T.4 data (the TCF's zeros, transferred, and
the page's bits) ended by `t4-non-ecm-sig-end`; then 100 or 60 ms and
no-signal. The octets are the line's bits with the first in the top bit
(7.1.2), the other way round from the frames as T.30 writes them. CED is
its indicator after 200 ms of no-signal; the calling side sends no CNG
(the call goes over to T.38 because it is a fax call, and a gateway
playing CNG holds back what follows until it hears the other machine's
V.21, which may be a DIS already gone by). What comes in is
taken the same way, a sig-end field or a no-signal indicator for the
carrier's end. `t30_t38_time()` is the time going by; the timers are the
same.

The IFP packets are in the encoding of the 1998 ASN.1 (T38FaxVersion 0:
the field types in four bits, a field without data leaving its octet's
low half to the next) or of the 2002 one (1 to 3), as spandsp and
Asterisk have them; a field without data goes last in our packets, so
both read the same. UDPTL numbers them and repeats the ones before
(redundancy); on receipt the lost ones come back from the next packet's,
in their order, a repeated one once only.

`t38_test` has the encodings (Appendix V's DIS), UDPTL with packets lost,
late and repeated, and two machines over T.38 (versions 0 to 3, 14400 to
4800 bit/s, ECM, every fifth packet lost); `spandsp_test`'s `T38Spandsp`
cases run ours against spandsp's T.38 terminal both ways.  Class 1 over
T.38 (dmodemd's) is in [Fax (class 1)](#class-1-over-t38).
