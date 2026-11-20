# Fax: technical details

This page gives the technical details for [Fax](fax.md), the plain-English overview. Read the overview first. This page has the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

This page uses these terms:

- **DTE**: the fax program on the computer, which communicates with the
  modem.
- **T.30**: the fax protocol (the frames DIS, DCS, CFR, MCF and other
  frames, sent on V.21 at 300 bit/s), with the pages on a faster
  modulation.
- **ECM**: the error correction mode of T.30 (T.30 Annex A, T.4 Annex A).
- **T.85**: the JBIG (T.82) coding of fax pages, with ECM only.
- **T.42**: colour and grey-scale fax pages in JPEG (T.81) of CIELAB
  samples (T.4 Annex E), with ECM only.
- **T.38**: fax over IP, as IFP packets sent in UDPTL.

## Fax (class 1)

`fax_class1.cpp` implements the T.31 class 1 commands on the modulations
in `fax_modems.cpp`:

| Command | What it does |
|---|---|
| `+FTH=3` / `+FRH=3` | Send or receive V.21 channel 2 HDLC frames: flags, bit stuffing, CRC-16, `<DLE>` shielding, `OK` or `ERROR` for each received frame |
| `+FTM=` / `+FRM=` | Send or receive on a fast modulation: 24, 48 (V.27ter); 72, 96 (V.29); 73, 74, 97, 98, 121, 122, 145, 146 (V.17; each even code is the short train) |
| `+FTS=` / `+FRS=` | Send silence for, or wait for silence of, n × 10 ms |

### The start of a call

- The answering side sends CED (2100 Hz for 3 s). Then it starts
  `+FTH=3` automatically.
- The calling side sends CNG (0.5 s of 1100 Hz each 3.5 s) while it
  waits. It starts `+FRH=3` when it hears the answer tone.

### Characters from the DTE

All characters from the DTE except XON and XOFF stop `+FRH`, `+FRM` and
`+FRS` with `OK`, also before their `CONNECT` (T.31 8.3.2, 8.3.4, 8.3.6).
For example, efax sends CAN to stop a `+FRH` that has waited too long.
While a class 1 command runs, the characters of the DTE go to the engine
(as data, or as that abort character). The modem reads them as commands
again only after the result of the command.

The modem reads the data of the DTE for `+FTH` and `+FTM` one character
at a time. It reads nothing after `<DLE><ETX>`:

- A frame that follows in the same write waits until the current frame
  has gone out.
- The data that follows the last frame or the `+FTM` data (the next
  command) stays for the command interpreter, which runs it after the
  `OK`.
- In `+FTH`, an empty frame (only `<DLE><ETX>`) ends the transmission
  with `OK` (8.3.5/T.31). A frame of one octet goes out with no change.

`core/test/scripts/fax_dte.txt` tests the three items. It runs in
`core_equiv` as `CoreScript.FaxDteUntilsCome`.

### The receivers

The receivers find the training. They compare the received phase steps
with the known sequence. The V.29 and V.27ter training sequences repeat
each 127 symbols. Thus, the end of the alternations before them tells
the receiver which repetition it receives.

A V.17 short train continues from the equalizer of the last long train
at the same rate.

### V.17 after a short train

After a short train, the original library and spandsp go directly to the
scrambled ones (segment 4), without the 64-symbol bridge that follows a
long train. This transmitter does the same. Its trellis encoder is in the
state from which the original library starts segment 4 (the original
receiver expects that state).

After a short train, the segment 4 of this transmitter is 240 symbols
(100 ms; the original library sends 144). After a long train, it is the
standard 48 symbols. After the data, 80 symbols of ones follow. The two
lengths are for the receiver of the original library, which processes in
20 ms frames. The position of its frames can cause it to start the data
before it has converged and to end it early. The extra ones prevent the
two problems.

The receiver accepts a short train with or without the bridge. The first
symbols after the training tell which. During a bridge, it decides on the
nearest training point, and does not follow the known sequence. The
bridge of the Agere modem is not that sequence. When the receiver
followed the incorrect points, the carrier loop went to a frequency
offset of about 3 Hz. This caused errors in the first seconds of data.

### A known quirk of the original receiver at 12000 bit/s

At 12000 bit/s after a short train, the receiver of the original library
sometimes decides an outer corner of the constellation as the point 8
away in its subset (a coordinate of 7 comes out as -1). That is one
incorrect bit on the line, and three after the descrambler.

Our signal is always the same. The position of the 20 ms frames of the
receiver on the long training and on the short training sets if and
where the error occurs. It occurs in three of the four alignments, about
one time for each page.

- If the data is sent 1.4 dB lower, the error does not occur in the four
  alignments. But if the data is sent 0.5 dB lower, the TCF is lost.
- If TEP is sent before the training (the original library sends 200 ms
  of it), the error does not occur in two of the three alignments.

Thus, the transmitter keeps the levels of V.17 and sends no TEP.

### V.27ter and the receive level

V.27ter also sends 100 ms of ones before the data (the original library
sends about 60 ms). With the 8 symbols of the standard, the original
receiver sometimes lost the start of a page.

In data mode, the V.17 receiver corrects a clear error in the power of
the data (6 % or more, averaged over about 500 symbols). The reason is
that the data constellation of the original library comes about 1.4 dB
below its training.

### Tests against spandsp

`ctest -L spandsp` (with libspandsp-dev installed) sends the output of
the V.17 transmitter and T.4 encoder of spandsp, as the `SendFAX` of
Asterisk uses them, into these receivers.

### Class 1 over T.38

`FAX_t38_start()` (`dsplibs.h`) moves the engine from the audio to T.38,
also during a call (the call of dmodemd changes to it). The DTE sees the
same commands and results. The signals of the line become IFP packets
(`dsp/t38.h`), sent at the time that the signals would take. The modem
still calls `FAX_process()` with samples to keep that time, but its audio
is silence. A transmission in progress starts again on T.38 with the data
that it has not sent. `FAX_t38_stop()` goes back to the audio.

| Command | On T.38 |
|---|---|
| `+FTH=3` | The V.21 preamble's indicator, then a second's worth of flags' time, then each of the DTE's frames as it comes. Frames are paced at 300 bit/s in packets of at most seven octets, each ended by `hdlc-fcs-OK` (`hdlc-fcs-OK-sig-end` for the final frame). Accepting a frame that is not the final one is the `CONNECT` for the next, as on the audio. 100 ms after the final frame (or an empty one): no-signal and `OK` |
| `+FTM=` | The modulation's indicator and its training's time, then the DTE's octets at the modulation's rate, bit-reversed the T.38 way, `t4-non-ecm-sig-end` after the last, and no-signal 60 ms later. If the data starts with two HDLC flags, it is the DTE's own HDLC (in ECM, fax software frames the ECM blocks itself). The frames are then taken out of the bits and sent as HDLC data, each ended by `hdlc-fcs-OK` (or `hdlc-fcs-BAD` if the DTE's FCS was wrong), with `hdlc-sig-end` at the end |
| `+FRH=3` | `CONNECT` at the V.21 preamble (or at its first data). Each frame goes to the DTE with an FCS computed here (deliberately spoilt, with `ERROR`, for an `hdlc-fcs-BAD`). `NO CARRIER` when the signal ends without a frame |
| `+FRM=` | `CONNECT` at the first data of a fast modulation. T.4 data goes to the DTE in the DTE's bit order. HDLC frames (ECM) are rebuilt as an HDLC bit stream (flags first, zeros stuffed, the FCS, two closing flags at the end). `NO CARRIER` at the sig-end or no-signal |
| `+FRS=` | Silence is "no signal" on T.38. CED is its indicator after 200 ms of no-signal. No CNG is sent (as with the T.30 machine below) |

Tests:

- In `class1_t38_test` (label `quick`), its own DTE goes through T.30
  with these commands against our T.30 machine over T.38, with the timing
  of the packets. It includes send and receive, with and without ECM, the
  two packet versions, and a change to T.38 after some time on the audio.
  The `Class1T38Packets` cases do a check of the fields and times of the
  packets.
- `frontend/asterisk/test/efax.sh` runs efax on dmodemd against the fax of
  Asterisk over T.38 (the `SIP_T38` of dmodemd; refer to
  [Using dmodemd](../usage.md)).

### Class 1.0: V.34 half-duplex

With `+FCLASS=1.0` and `+F34=` (maxp not 0), the core calls `FAX_v34()`
(dsplibs.h) immediately after `FAX_create()`. (It does not do this on
T.38, or after the own answer tone of adaptive answer.) The line of the
engine is then a V.34 half-duplex modem (`v34_hdx_create()`,
[V.34](v34-details.md)) with the primary channel between minp and maxp
(`v34_hdx_rates()`), and V.8 first (CM when it calls, ANSam when it
answers, no CNG). When its control channel is up, the engine writes
`+F34:<p>,1` to the tty and gives `CONNECT`. From then on, the data of the
DTE is the V.34 data mode of T.31 (Annex B), as HylaFAX uses it:

- Frames each to `<DLE><ETX>` in the two directions (the frames of the
  DTE without an FCS, the frames of the engine with their FCS). `<DLE>` is
  doubled, and `<DLE><SUB>` is two of them. The frames of the DTE go out
  on the control channel with flags between them.
- `<DLE><0x6B>` from the source DTE: when its control channel frames are
  out, the modem changes to the primary channel (`v34_hdx_primary()`). The
  frames of the DTE that follow (the ECM block, no RCP necessary) go on
  it, with `<DLE><0x6D>` after the last.
- To the DTE at the two ends: `<DLE><0x6B><DLE><0x6F+p>` when the primary
  channel is up, then the frames that it sends, then
  `<DLE><0x6D><DLE><0x6E>` when the control channel (1200 bit/s) is up
  again (also the first time).
- `<DLE><EOT>` from the DTE drops the modem: `OK`. When the line stops,
  the modem also drops: `<DLE><EOT>` to the DTE and `NO CARRIER`. The
  modem does not ask for rate renegotiation and control channel retrains.

If the other end has no V.8, the engine is class 1 again. When it calls,
the V.21 flags of the answerer (or the failure of V.8) drop the modem, and
the `CONNECT` and frames of `+FRH=3` follow. When it answers, the ANSam
was the CED, and `CONNECT` comes for the `+FTH=3` frames of the DTE.

In `class1_v34_test` (the `quick` label of ctest), its own DTE goes
through T.30 Annex F with it. It sends (to 33600, and with a limit of
maxp 6) and receives, against our T.30 machine on V.34 through G.711
with a little noise. It also calls and answers a machine without V.34.

## Fax (class 2 and 2.0)

`fax_class2.cpp` implements the class 2.0 of T.32, and the older class 2
that fax programs know (the EIA/TIA-592 draft, SP-2388). Its entry points
are `FAX2_*` in `dsplibs.h`. It is part of the T.30 library. Thus, a
program built without that library has no `+FCLASS=2`.

The modem operates the T.30 machine of the subsequent section, with the
class 2 engine as its host (the `host` of `t30_config`). The engine
controls the speed of the machine.

**Sending:**

1. After the DIS (`T30_EV_DIS`), the machine waits for `+FDT`. Then it
   sends the DCS with the session parameters of `+FIS` (`t30_session()`,
   `t30_resume()`).
2. For each page, it waits until the page of the DTE has come and is
   decoded (`next_page()` returns 2 until then).
3. It waits for the post-page command of `+FET` (`post_page()`).
4. It reports the CFR (`T30_EV_TRAINED`) and each MCF
   (`T30_EV_PAGE_SENT`).

**Receiving:** the machine operates as it does alone. The engine keeps
the received items, the session (`T30_EV_TRAINED`) and the pages with
their post-page commands (`T30_EV_POST`), for the results of `ATA` and for
each `+FDR`.

**Page data:** the pages of the DTE are MH with EOLs, in the bit order of
`+FBO`. The engine decodes them into a bitmap and codes them again for
the line, with the scan line fill, MR on receipt, and ECM in the two
directions. Thus, the DTE never has to frame ECM.

**Polling:**

- An answerer with `+FLP=1` (the `polled` of `t30_config`) sets DIS bit
  9. A DTC (with the CIG before it) then makes it the sender, as a caller
  is after the DIS.
- A caller whose remote offers a document polls with `+FDR`
  (`t30_poll()`). It sends CIG and DTC in place of the DCS, and then it is
  the receiver.
- The X bit of the frames stays that of the caller.

**T.38:** `t30_set_t38()` moves the machine to T.38 (or back) at all
points of the call. The data that was going out is sent again from its
start on the new path, as `FAX_t38_start()` does for class 1.

**V.34 fax (T.30 Annex F):** a `+FIS` bit rate (br) above 5 (16800 to
33600 bit/s) together with ECM offers V.34 half-duplex (the `v34` of
`t30_config`, `t30_v34()`). V.8 sends the call function. A caller sends,
or polls when `+FSP=1` was set before it dialled (`v34_poll`). `+FIS:`
and `+FCS:` give the V.34 rate as br 6 to 13.

**Tests** (in `core_test`):

- `Classes/CoreFax2.TwoPages/*` sends two pages between two modems in
  class 2.0, class 2, class 2.0 with ECM and the other bit order, on T.38
  from the start and from the DIS, and on V.34, and compares the bitmaps.
- `Core.Fax2Poll` polls a page, and `Fax2PollV34` does the same on V.34.
- `Core.Fax2Abort` ends a call with `+FKS`.

## Fax machine (T.30)

`t30.cpp` is a group 3 fax machine on these modulations. The fax of
libdmodem and the fax applications of the PBX modules use it. The calling
machine sends a document and the answering machine receives it.

### Error correction mode

By default, the two machines offer ECM (DIS and DCS bit 27), with frames
of 256 octets:

1. The page goes as its MH coding (or T.85, below), in blocks of up to
   256 HDLC frames (FCD) on the fast modulation, with three RCP frames
   after each block.
2. A PPS follows on V.21, with the page and block counters.
3. The receiver answers MCF, or PPR with a map of the frames to send
   again.
4. After four PPRs for one block, the sender sends CTC (and gets CTR) and
   continues.

The sender answers RNR with RR. The receiver answers RR, CTC and EOR as
T.30 tells.

The receiver ends a block at its RCP. The V.17 short training of the next
block starts from the state of the receiver at the end of this block. The
V.21 PPS after it would damage that state.

### T.85 (JBIG) pages

With ECM, the DIS also offers T.85 (bits 78 and 79: all stripe heights
L0). Its octets go to the tenth octet, with the extension bits. If the
remote of a sender offers bit 78, the sender sets it in its DCS and sends
each page as a T.85 bi-level image entity (BIE) in place of MH. The
`no_t85` of `t30_config` keeps a machine to MH and MR. The `t85` of
`t30_status` tells which coding the call has.

`jbig.cpp` is the coder (T.82 as T.85 profiles it: one layer, one
bit-plane, sequential stripes):

- **Sending:** stripes of 128 lines (the basic L0 of T.85, thus DCS bit
  79 is 0), the three-line template with the AT pixel in its default
  position (MX 0), typical prediction (TPBON), and the height of the page
  in the header (no NEWLEN). The octets of the BIE go into the ECM frames
  with no change. T.85 sends it least significant bit first, as ECM sends
  octets.
- **Receiving:** a DCS with bit 78, or with only bit 79 (as spandsp sends
  T.85 with a different L0), tells T.85. The receiver keeps the octets of
  the frames until the page ends. (They came with good FCSs. Thus, there
  are no bad rows to look for block by block.) Then it decodes them and
  scales or centres them to 1728 pels. The decoder accepts the items that
  T.85 lets a sender use:
  - All values of L0.
  - The two-line template (LRLTWO).
  - Typical prediction.
  - AT moves (ATMOVE, a lag up to MX and 127, its line counted from the
    top of the stripe that follows).
  - An unknown height (YD of all ones) ended by NEWLEN.
  - COMMENT segments, SDRST, and ABORT.

  A BIE that ends early or that ABORT cuts keeps the decoded lines. A BIE
  that is not T.85 (more layers or planes, deterministic prediction)
  loses the page. A page has a maximum of 32768 lines.

The QM coder follows the flow diagrams of T.82. The carry-free branch of
FINALWRITES writes BUFFER, not BUFFER + 1 as Figure 30 shows. The coded
data of each stripe does not include its trailing 0x00 octets. The
decoder reads past the end in all cases.

`jbig_test` compares the coder with T.82 clause 7:

- The 256-pixel arithmetic coder sequence of 7.1 (its 25 coded octets,
  and decoded back).
- The 1960 by 1951 artificial image of 7.2.1 in its three sequential
  codings: no TP with 1951-line stripes, the same with the two-line
  template, and TP with 128-line stripes and an AT move to lag 8 at
  stripe 9. Its stripe coded data, stripe data entities and BIE agree
  exactly with the byte counts of Table 29.

It also does round trips of pages of the fax type. It decodes a BIE with
YD of all ones, a comment and NEWLEN, a BIE that is cut short, a BIE with
a limit on the lines, and random data.

### T.42 (JPEG) colour and grey-scale pages

With ECM, the DIS also offers JPEG (bit 68), in full colour (69), and
accepts colour without subsampling (73). The `no_colour` of `t30_config`
disables this. The `jpeg` of `t30_status` tells what a call sends (1
grey, 2 colour).

- **Pages:** a `T4Page` can contain colour: `comps` 3 (sRGB) or 1 (grey)
  and its 8-bit samples in `pix`. `bits` is a dithered copy (an 8x8
  ordered dither of the lightness) for items that accept only black and
  white: the line without JPEG, the DTE of class 2, MH TIFF readers.
  `TiffIn` reads uncompressed 8-bit grey or RGB pages in that way, scaled
  to 1728 pels at 200 pels an inch. `TiffOut` writes such a page
  uncompressed (RGB, or grey with black zero) at 200 pels an inch.
- **Sending:** a page with colour goes in JPEG when the remote offers bit
  68 (and 15, 200 by 200). It goes in colour if the remote offers 69. If
  not, only its lightness goes. The DCS sets 68, 69 for colour, 70 (the
  tables are K.3 to K.6 of T.81) and 15. A page without colour goes as
  before. Thus, a document that mixes them sends EOM between the two
  types, and a new DCS (as for a change of resolution).
- **Receiving:** a DCS with bit 68 (69: colour) tells JPEG. The receiver
  keeps the frames to the end of the page as for T.85. Then it decodes
  them, changes them from CIELAB to sRGB, scales or centres them to 1728
  pels, and dithers them. A JPEG that is cut short keeps its lines up to
  that point, with 1 row counted bad.

`jpeg.cpp` is the coder:

- **Encoder:** the items in the list of T.4 E.6.2: SOI, the G3FAX APP1
  (version 1994, 200 pels an inch), DQT (K.1 and K.2 at quality 75), DHT
  (K.3 to K.6), SOF0, one interleaved scan (four L* blocks, then a* and
  b*, for 4:1:1 with a* and b* averaged over 2x2, T.4 E.6.3), EOI. As an
  option, DRI with RSTn, and DNL for an image whose length is not known
  at the start.
- **Decoder:** all baseline or extended (SOF1) 8-bit Huffman sequential
  frames of 1 or 3 components. It accepts sampling factors up to 4, one
  scan or more, 8 or 16-bit quantisation tables, restarts, and DNL (Y 0
  in the frame header). It skips APPn and COM, and ignores the octets
  after EOI (the padding of the last frame of T.4). It refuses
  progressive, lossless and arithmetic-coded frames.
- **CIELAB:** the defaults of T.4 E.6.4: D50, L = L* x 255/100,
  a = a* x 255/170 + 128, b = b* x 255/200 + 96, from sRGB through XYZ
  with the Bradford-adapted sRGB matrix.
- The octets of the JPEG go into the ECM frames with no change, as
  spandsp does.

`jpeg_test` does a check of these items:

- The markers of the encoder (and that each Huffman table is a whole code
  of the symbols of T.81).
- Round trips of grey and colour pictures (with 4:1:1 and without, odd
  sizes, restarts and DNL give the same pels).
- Damaged data and data that is cut short.
- The CIELAB conversions (white is 255/128/96).

Outside the tests, libjpeg (through the Pillow of Python) decoded the
JPEGs of the encoder at 50 dB. Also, this decoder decoded the JPEGs of
libjpeg (with optimised tables, restarts, 4:2:0) within 52 to 68 dB of
the decoding of libjpeg. The `T30Colour.SendsColourPages` of `fax_test`
sends a colour page, a black and white page and a grey page between two
machines (JPEG colour, T.85, JPEG grey: 42 and 51 dB). It sends them
again to a receiver with `no_colour` (all dithered, the same bits).

spandsp 0.0.6 is built without T.85 (`SPANDSP_SUPPORT_T85` is disabled in
its configure). Thus, the calls of `spandsp_test` stay in MH or MR. The
spandsp of FreeSWITCH (3.x) has it. Its fax test
(`frontend/freeswitch/test/fax.sh`) sends and receives T.85 pages with AT
moves (MX 127), VLENGTH and the L0 option.

### T.43 (lossless JBIG) colour and grey-scale pages

With colour enabled, the DIS also offers T.43 (bit 36, which T.30 allows
only with 68) and accepts plane-interleaved data (37). The `no_t43` of
`t30_config` removes T.43 (JPEG only). The `t43` of `t30_status` tells
what a call sends (1 grey, 2 colour).

- **Sending:** a page with colour goes in T.43 when the remote offers
  bits 68, 36 and 15 and the page has few colours:
  - In colour (the remote offers 69, and the page is RGB) when it has a
    maximum of 4096 colours. It goes as a palette (image type 16: the
    colours in the 8-bit CIELAB of T.42, the most frequent first, indices
    of the smallest number of bits that hold them).
  - If not, in grey when its grey levels (or the luma of its colours) are
    a maximum of 16, as 8-bit L* (type 32).

  Other pages go in JPEG as before. The DCS sets 36 (69 for colour; 68 0,
  37 0: stripes interleaved) and 15. A change between T.43, JPEG and
  black and white sends EOM. On the bar chart of the test (five colours,
  1728 by 400), T.43 uses 199 octets, and JPEG 25809. On its four-level
  grey chart, T.43 uses 300 and JPEG 11882.
- **Receiving:** a DCS with 36 (and not 68) tells T.43. The receiver
  keeps the frames to the end of the page, decodes them (a maximum of
  4800 lines), changes them to sRGB and sends them on as a JPEG page. It
  accepts all image types: one bit for each colour (RGB, CMY, CMYK: the
  primaries, black over all other colours), palettes of 8 or 12-bit
  tables, grey and colour of 1 to 12 bits for each component (12 bits
  rounded to 8, fewer bits stretched), and the two interleaves. It skips
  the gamut and illuminant entries (G3FAX1, G3FAX2) and uses the defaults,
  as for JPEG.

`t43.cpp` is the format (T.43 clause 7): the magic number ff a8, the
G3FAX0 entry (version 1997, resolution, JBIG, image type, bits for each
component), G3FAX3 for a palette (ff e3, a 4-octet length), the ECIH,
one BIE of all the bit-planes, then ff a9. Grey and colour samples are
Gray coded (7.3.1). Thus, a small change of a sample changes one plane.
The planes of L* go first (from its most significant bit), then those of
a* and b*. A palette index goes from its most significant bit. For one
bit for each colour, the order is red, green, blue (or cyan, magenta,
yellow, black).

`jbig.cpp` codes the planes: one BIE of P planes (a maximum of 36), each
with its own probabilities, typical prediction and AT pixel, in the order
that T.82 Table 11 gives for one layer. T.43 sends ILEAVE and SMID
(stripe interleave, the basic one: a stripe of each plane in turn, L0
128). It accepts only ILEAVE (plane interleave: each plane whole). An
ATMOVE moves the AT pixel of the plane of the next stripe. The single
plane of T.85 is the same code with P 1.

`t43_test` does a check of these items:

- The header against the examples of T.43 (the lengths of G3FAX0 and
  G3FAX3, the table ID and count of the palette).
- Gray code against Table 6 (the planes of 0 to 15).
- Round trips of all image types and numbers of bits in the two
  interleaves and with short stripes.
- The colours that each type gives.
- Data that is cut short and damaged data.
- The six stripe orders of JBIG (and the two that T.82 does not allow).

The `T30Colour.SendsChartsLossless` of `fax_test` sends a colour chart, a
black and white page and a grey chart between two machines. In T.43, the
colour chart comes back within 1 level of each sample (the CIELAB
rounding of T.42), and the grey chart also. To a receiver with `no_t43`,
they go in JPEG (worst errors 83 and 36 levels, at the edges).

The T.43 of spandsp (the 3.x of FreeSWITCH) has its encoder compiled out,
and mod_spandsp does not offer it. Thus, there is no T.43 machine to call
outside these tests.

### T.44 (mixed raster content) pages

With colour enabled, the DIS also offers the mode 1 of T.44 (bits 92 to 94
as 1, 0, 0, which T.30 allows only with 68) and stripes as long as the
page (95). `no_mrc` removes it. The `mrc` of `t30_status` tells what a
call sends (1 grey, 2 colour). An MRC page is a mask (bi-level, at the 200
pels an inch of the page) that selects, pel by pel, between a background
and a foreground. Each of these is a JPEG or a T.43 image at its own
resolution and position, or one colour. Text over a picture keeps its
edges sharp in the mask, while the picture goes in JPEG at half the
resolution.

- **Sending:** a colour or grey page goes in T.44 when the remote offers
  mode 1 (and 68 and 15) and the page has text or lines on it. Dark pels
  with a light pel within 3 pels make the mask. A minimum of 0.1 % of the
  page must be mask. (A page of few colours still goes in T.43. A page
  without text goes in JPEG.) Each stripe of 256 lines is one of these
  items:
  - The mask only (the foreground and background each one colour: black
    text on white).
  - The mask with a background JPEG at 100 pels an inch (the pels under
    the mask filled from their neighbours).
  - The same with a foreground JPEG at 100 also, when the text has more
    than one colour.
  - A background JPEG only at 200, when the stripe has no text.

  The mask goes in T.85 if the remote offers 78. If not, it goes in MH.
  The DCS sets 92, 68 (69 for colour), 70, 78 if the mask is T.85, and the
  extension bits up to 92. On the page of text over a picture of the test
  (1728 by 600 in colour), the call takes 63 s, and in JPEG 275 s. The
  page comes back at 42.6 dB (in JPEG 29.6 dB). The edges of its text are
  within 1.7 levels (in JPEG 7.5). With the mask in MH, it takes 96 s.
- **Receiving:** a DCS with one of 92 to 94 tells T.44. The receiver
  decodes the page at its end (a maximum of 4800 lines). It sends it on as
  a JPEG page, in grey when all layers and colours are grey. It accepts
  all of mode 1:
  - Masks in MH, MR, MMR or T.85.
  - Layers in JPEG or T.43 at all resolutions that divide that of the
    mask, and their offsets.
  - Stripes of one, two or three layers.
  - The 4-octet lengths.
  - The optional segments (skipped: the gamut and illuminant are the
    defaults).

  It does not accept modes 2 and higher (whose start of stripe is
  different) and JBIG2.

`t44.cpp` is the format (T.44 clause 9):

1. ff d8 and the start of page (ff ed, "MRC" 0, version 2, mode 1, the
   coders, the resolution of the mask, the width), ff d9.
2. Then for each stripe, its start (ff ed, "MRC" 1, the layers in it, the
   two colours in the CIELAB octets of T.42, the offsets, the height, the
   length of the mask) and its layers in the order mask, background,
   foreground.
3. ff d9 ff d9 ends the page.

The background and foreground end themselves (a JPEG at its EOI, a T.43
BCIE at ff a9). Thus, `jpeg_decode()` and `t43_decode()` tell how many
octets they used.

`t44_test` does a check of these items:

- The octets of the start of page.
- Round trips of text over a picture in colour and grey with the two mask
  types (the colour page 63 KB, and in JPEG 302 KB; the grey page 16 KB,
  and in JPEG 281 KB).
- Each type of stripe (black text on white comes back exactly).
- A page written manually: an optional segment, a background JPEG at 100
  pels an inch at an offset over its colour, an MMR mask over a T.43
  foreground, and a stripe of only a foreground.
- Pages that are cut short and damaged pages.

The `T30Colour.SendsMrcPages` of `fax_test` sends a colour page and a grey
page of text over a picture between two machines, with the mask in T.85
and in MH. It also sends them to a receiver without T.44 (JPEG).

### Binary files (T.434)

A document can send binary files between its pages: a `T4Page` whose
`file` holds a T.434 BINARY-DATA message. (`t434_encode()` makes one from
a `BftFile`: its name, MIME type, modification time and octets.) With the
`bft` of `t30_config` and ECM, the DIS offers BFT (bit 53). The DCS of a
file sets 53 (and 27). A change between files and pages sends EOM.

The message goes as one ECM page (T.30 V.4 case a), in blocks as the coded
data of a page does. Its last frame is short (T.4 B.6: no pad octets).
Then PPS-MPS, -EOM or -EOP follows. The receiver keeps the octets of a
file across pages (case b, the file of another sender). It sends each
whole message to `page_received()` as a page with `file` (more than one in
a page: case c). `t434_decode()` gives the files back.

The `bft` of `t30_status` tells that a file is in progress. `files` counts
the files (they are not in `pages`). `files_left_out` counts the files
that the sender did not send because the remote has no BFT or no ECM. A
document of only those files ends with `T30_ERR_INCOMPATIBLE`. An FDM
from the receiver (it does not accept the file in its current form) ends
the call, because we have no other form of the file to send. A received
name comes from the remote. Use only its last component before you use it
as a path.

`t434.cpp` is the message in BER (X.690): [APPLICATION 23] holding a
SEQUENCE of attributes for each file. We send protocol-version [28]
(version 3), filename [0], date-and-time-of-last-modification [5],
filesize [13], mime-media-type [32] (a high tag number) and
data-file-content [30] as an OCTET STRING. We accept those in all orders,
with definite or indefinite lengths, strings in pieces, and content as
EXTERNAL. We skip all other attributes.

`t434_test` does a check of the octets of a small file, a round trip with
all attributes (lengths of three octets), a message as other coders write
it, the length as the octets come, and messages that are cut short or
damaged. The `T30Bft.SendsFiles` of `fax_test` sends a page, a file of 37
octets, a file of 100000 (two blocks) and a page. It sends them to a
receiver that accepts files and to a receiver that does not (the two pages
still go). `T30Bft.NeedsEcm` sends a file without ECM. The
`T38Pair.SendsFiles` of `t38_test` sends files over T.38 with bursts of
lost packets (the receiver asks for the frames again).

### Without ECM

The receiver answers each post-page command (MPS, EOM, EOP) with MCF or
RTN.

If the sender does not receive the MCF, it sends the command again after
T4 (3 s). If no page training has arrived since then (the fast receiver
never found one), the receiver identifies the command as a repeat and
answers MCF again, as the receiver of spandsp does. An RTN there would make
the sender send again a page that the receiver already has.

### V.34 half-duplex (T.30 Annex F)

With the `v34` of `t30_config` (`t30_v34()`), the machine uses the
half-duplex V.34 modem
([V.34's details](v34-details.md#half-duplex-clause-12-for-fax)), which
has the line from the start of the call:

- ANSam replaces CED, and the CM of V.8 replaces CNG. The CM sends the
  call function: T.30 transmit, or receive when the caller polls.
- The T.30 frames go on the control channel of V.34 at 1200 bit/s, with
  flags between them.
- The DIS has bit 6 (V.8) set. The DCS gives no rate (bits 11 to 14 are
  0), and there is no TCF. The receiver answers the DCS with CFR.
- ECM is the only mode. Each block goes on the primary channel
  (`v34_hdx_primary()`), its frames after a few flags. The PPS follows on
  the control channel when that channel is back.
- A remote without V.8 gets V.21 and the usual fax modulations. If the
  ANSam of an answerer gets no CM in 5 s, the answerer sends its DIS on
  V.21. If a caller hears a DIS on V.21 while V.8 still waits, it drops
  the V.34 modem.
- V.8 sets which side sends on the primary channel. Thus, the machine
  refuses a poll that V.8 did not announce (`t30_poll()` with us as the
  source).

`t30_v34_test` operates two machines on V.34 back to back through G.711
(two pages at 31200 bit/s). It also operates one machine with V.34
against one machine without it, in the two directions (they fall back to
V.17).

### Tests

- The `Rates/T30Pair.*` cases of `fax_test` put two machines back to
  back, at each rate from 14400 down to 2400, with ECM (in T.85, and one
  time in MH: `14400_ecm_mh`), and with ECM on a noisy line. At 19 dB,
  ECM sends a few frames again, and the page comes out correctly. Without
  ECM, about two thousand rows came out damaged. The two test pages take
  88 s at 14400 bit/s with ECM in T.85, and 175 s in MH.
- The `Calls/T30Spandsp.*` cases of `spandsp_test` (label `spandsp`)
  operate one machine against the fax machine of spandsp in the two
  directions, with and without ECM, and with line hits.
- The `Mcf/T30Lost.*` cases of `fax_test` lose the MCF of the receiver to
  an EOM, an MPS and the EOP of the last page.

`DSP_T30_LOSE=<fcf in hex>[:<n>]` sends the nth message (by default the
first) whose last frame has that type as silence, as if the line lost it.
It keeps its timing (`8c` is MCF).

## T.38

With `t30_config.t38` set, the same machine is a T.38 terminal. It sends
and receives IFP packets (`dsp/t38.cpp`, in the encoding of Annex A) in
place of the modulations.

### A transmission

Each transmission has these steps:

1. A no-signal indicator, then 75 ms.
2. The indicator of the modulation, then the time of its training (for
   the V.21 preamble, a second of flags; for ECM, also 200 ms of flags).
3. The data, sent at the speed at which the modulation would send it:
   - HDLC frames without their FCS (seven octets in each packet on V.21,
     as in T.38 7.5), each ended by `hdlc-fcs-OK` and the last by
     `hdlc-fcs-OK-sig-end`.
   - T.4 data (the zeros of the TCF, and the bits of the page), ended by
     `t4-non-ecm-sig-end`.
4. 100 ms (HDLC) or 60 ms (T.4), then no-signal.

The octets send the bits of the line with the first bit in the top bit
(7.1.2). This is the opposite of the frames as T.30 writes them.

CED is its indicator after 200 ms of no-signal. The calling side sends no
CNG. The call moved to T.38 because it is already known as a fax call. A
gateway that plays CNG holds back what follows until it hears the V.21 of
the other machine. This can be a DIS that has already gone by.

The machine accepts incoming data in the same way: a sig-end field or a
no-signal indicator marks the end of the carrier. `t30_t38_time()` tells
the machine how much time has gone by. The timers are the same as on
audio.

### Packet encodings and UDPTL

The IFP packets use one of these encodings:

- The 1998 ASN.1 (T38FaxVersion 0): field types in four bits. A field
  without data leaves the low half of its octet to the next field.
- The 2002 ASN.1 (versions 1 to 3).

spandsp and Asterisk use the same encodings. In our packets, a field
without data always goes last. Thus, the two versions read the same.

UDPTL gives numbers to the packets and repeats the previous packets in
each packet (redundancy). On receipt, the terminal recovers lost packets
from the redundancy of the next packet, in their order. It accepts a
repeated packet only one time. For a packet that uses FEC in place of
redundancy, it reads only the primary packet.

### Tests

- `t38_test` includes these items:
  - The encodings (`T38Ifp.*`, with the DIS of Appendix V).
  - UDPTL with packets that are lost, late and repeated (`T38Udptl.*`).
  - Two machines over T.38 (`Nets/T38Pair.*`): versions 0 to 3, 14400 to
    4800 bit/s, with and without ECM, each fifth packet lost, bursts that
    are longer than the redundancy can cover, and a relay that gives new
    numbers to the packets.
- The `Calls/T38Spandsp.*` cases of `spandsp_test` operate ours against
  the T.38 terminal of spandsp in the two directions.
- [Fax (class 1)](#class-1-over-t38) includes class 1 over T.38 (of
  dmodemd).
