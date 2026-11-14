# The data link layer: technical details

This page is the technical companion to [The data link layer](link.md), the plain-English overview. Read that page first. This page gives all the details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

`link/` is the code between the characters of the DTE and the bits of
the data pump after a call connects. (The DTE, data terminal equipment,
is the computer program on the other side of the serial port or pty.)
It is C++, in `namespace dlink`.

## The protocols

### Error correction and framing

- **The V.42 detection phase** (`Detector`, `hdlc.cpp`). The originator
  sends ODPs (originator detection patterns), and the answerer answers
  with ADPs (answerer detection patterns). Or the remote does not do
  the phase and sends HDLC flags immediately.
  - T400 (750 ms) runs at the two ends. When it expires, the remote has
    no V.42, and the call continues as asynchronous characters.
  - Two HDLC flags in sequence show that the remote has V.42. The link
    ignores one flag alone, because it can be noise from a receiver
    that is not yet stable.
  - The link ignores ADPs other than "EC" and "E" + NUL, for example
    the "EM" of a cellular modem (Appendix VI/V.42). The "EC" that
    follows them decides.
- **V.42 LAPM** (`Lapm`, `lapm.cpp`), the Link Access Procedure for
  Modems, over **HDLC** framing (`Hdlc`):
  - Sequence numbers modulo 128, and windows of up to 15 frames of up to
    128 octets.
  - REJ recovery (no SREJ). If the N(R) of a frame is after all frames
    sent, LAPM thinks that the frame is noise that passed the FCS (frame
    check sequence), and ignores it.
  - RNR while the DTE does not accept the data.
  - T401 counted in transmitted bits: 2 s at the rate of the data pump,
    plus the time of three frames. With I frames outstanding in the data
    phase, a frame is the longer of the two N401s (and at least 128
    octets). Otherwise, it is 64 octets. The extra time lets an answer
    come back at the low rates of V.cvm (`Lapm::t401_bits()`).
  - N400 = 5 retransmissions (of an XID, SABME, DISC or the polls of the
    data phase) before LAPM stops.
  - Parameter negotiation in XID, with the private parameters of V.42bis
    (and the zstd set after them) and the user data subfield of V.44.
  - USRobotics HST in the user data subfield of XID, as a Courier sends
    it (`Config::hst`, for `AT+MS=HST`): the set `HST` (PI 0xFF), the
    capability (PI 0xC0: options, then the forward rates as bits 2 to
    7 for 4800 to 16800 bit/s) and a PI 0xF1 of 6. When the two modems
    offer it, this occurs:
    1. The originator answers the XID response with a DM (F = 0, with an
       FCS-16), not with an SABME.
    2. The answerer accepts that DM as the turn.
    3. The two modems stop the 2400 bit/s pump and use HST at 9600 (or
       at the nearest rate that the two have: `Owner::hst()`).
    4. After HST connects, LAPM continues without the detection phase or
       a second XID. The SABME of the originator is first ([HST](dsp/hst.md)).
  - The 32-bit FCS (10 c and 8.10.2/V.42). It is on if `Config::fcs32`
    is not off. The procedure is:
    1. The originator asks for it with bit position 17 of the HDLC
       optional functions in its XID.
    2. An answerer that agrees sets 17 and clears 16 in its XID
       response. Then it checks each frame against the two FCSs until
       the SABME comes.
    3. After that, the link uses the FCS of the SABME in the two
       directions.
    4. The originator sends its SABME and all frames after it with an
       FCS-32. It accepts the two FCSs until the first FCS-32 comes
       back. Then it accepts only FCS-32.

    An FCS-16 does not find approximately one corrupted frame in 65536.
    An FCS-32 does not find approximately one in 4 billion. A modem that
    does not set 17 in its response gets an FCS-16 as before.
- **MNP 2 to 4** (`Mnp`, `mnp.cpp`), the "alternative procedure" of
  V.42 (Annex A of V.42 (10/96)), for modems that have no LAPM:
  - The LR, LR, LA establishment goes in start-stop octets
    (`MnpAsync`).
  - The data phase goes in HDLC frames (framing mode 3) or octets
    (mode 2).
  - LT frames carry up to 256 octets, 8 outstanding. LA acknowledges
    with credit.
  - The link sends frames again on T401 or a repeated N(R), with
    N400 = 12.
  - LN and LNA. LD.

  Refer to [MNP](#mnp) below.
- **The link of HST** (`HstLink`, `hst_link.cpp`). A USRobotics Courier
  uses this link after the two modems agreed on HST in the XID of V.42
  and stopped V.22bis for it (`Link::hst_switch()`):
  - Its frames have no address: a type octet, then N(S), N(R) or the
    two, modulo 256.
  - Data frames of up to 64 octets (up to 256 accepted), 8 outstanding.
  - Cumulative RRs, REJ and T1 recovery.
  - The asks and grants that turn the HST channels around
    (`Owner::hst_turn()`).
  - No compression.

  Refer to [the technical details of HST](dsp/hst-details.md).
- **Asynchronous characters** (`Async`): a start bit, 8 data bits and a
  stop bit. The link uses them for the modulations without V.42 and when
  the remote has no LAPM.

### Compression

- **MNP 5** (`Mnp5`, `mnp5.cpp`): run-length and adaptive frequency
  coding. It is the compression of MNP (it is not in V.42). We wrote it
  as the code of the peers has it.
- **V.42bis** (`V42bis`, `v42bis.cpp`): we offer 2048 codewords and
  strings of 34 characters, or lower values when the remote asks. The
  encoder changes between transparent and compressed mode by the ratio
  over the last 256 characters.
- **V.44** (`V44`, `v44.cpp`): we offer 4096 codewords, strings of 255
  and a history of 12288 octets (as the Agere modem does), or lower
  values when the remote asks. The caller offers V.42bis and V.44 in its
  XID. The answerer selects V.44 when the two modems have it, and
  answers with what it selected. Refer to [V.44](#v44) below.
- **zstd** (`Zstd`, `zstd_codec.cpp`; Zstandard, RFC 8878): a D-Modem
  addition. The link offers it with V.42bis and V.44 in all modes with
  LAPM. The link selects it when the two modems offer it (thus, between
  two D-Modems). Otherwise, the link uses V.44 or V.42bis as before.
  Refer to [zstd](#zstd) below.

### Instead of all the above

- **V.80 Synchronous Access Mode** (`SyncAccess`, `v80.cpp`), when the
  DTE asks for it (`AT+ES=6`, or `AT+ES=,,8` to answer): the octets of
  the DTE go as a synchronous bit stream, transparent or as HDLC frames,
  with in-band commands after an EM character. Refer to [V.80](#v80)
  below.
- **The multiplexer of V.76** (`Mux`, `v76.cpp`), when V.8 bis selected
  the DSVD of V.70 (`Link::start_v76()`):
  - The DLCs share the HDLC link by the DLCI in their address octet.
  - The data channel is a `Lapm` with its DLCI (the ERM of V.76). The
    H.245 messages of V.75 (`h245.cpp`, aligned PER) in its SABME, UA,
    DM and DISC open and close it.
  - With a voice path (`Owner::voice()`), there is also a voice channel:
    G.729 Annex A (`g729a.h`, the coder of D-Modem) in UNERM UI frames,
    one for each 10 ms of line bits, before the data. In silence, Annex B
    SID frames and comfort noise go, so the data has the line.
  - No detection phase, XID or compression.

  Refer to [v70-details.md](v70-details.md).
- **X.75** (`Lapb`, `lapb.cpp`) over HDLC, on a clear 64 kbit/s channel
  with `AT+B2=0`. This is LAPB in basic mode (modulo 8):
  - A window of 7 I frames of up to 256 octets (up to 2048 accepted).
  - REJ recovery, and N2 = 4 tries.
  - T1 is 1 s plus the time of our longest frame and two of the longest
    frames of the remote.
  - A hangup sends DISC.

  Refer to [isdn.md](isdn.md).
- **V.120** (`Lapb` again) over HDLC, with `AT+B2=9`. It uses the same
  procedures in extended mode (modulo 128, SABME), the two-octet address
  of Q.921 for logical link 256, and the terminal adaption header of
  V.120 (`0x83`) before the information of each I frame.
- **Sync PPP** (`PppSync`, `ppp_sync.cpp`) over HDLC, with `AT+B2=5`:
  1. The PPP frames of the DTE come in asynchronous framing (RFC 1662).
  2. The link removes the escapes, checks the FCS and removes it.
  3. Each frame goes on the line as an HDLC frame.
  4. The frames from the line go back to the DTE with escapes and an
     FCS.

## How it fits together

`Link` (`link.h`) puts the protocols together.

- The modem core is its `Owner`. The owner gives and gets the characters
  of the DTE. It gets the progress of the link (`Status::Detected`, `Up`,
  `Released`, `Error`), the result of the XID negotiation, and the log
  lines.
- The data pump calls `get_bits()` and `put_bits()`.
- The link does not depend on the modem core or the data pumps.

`stats()` counts these items for a call, for the `ATI11` of the core:

- The characters of the DTE in each direction.
- The frames that LAPM or MNP sent (by `Hdlc` and `MnpAsync`), and the
  information frames in them that the link sent again (after the REJ or
  poll of LAPM, or the retransmissions of MNP).
- The frames received intact, and the frames with a bad FCS.
- In the data phase, the number of times T401 expired, and the requests
  of the remote to send frames again (the REJ or SREJ of LAPM, the
  repeated LA of MNP).
- The characters received without error correction that the DTE did not
  accept (lost).

## Where the code came from

The protocol code started as the slmodemd link layer of Smart Link
(`modem_pack.c`, `modem_ec.c`, `modem_comp.c`), changed into classes.
In 2026, we wrote its HDLC framing, LAPM and V.42bis again from
specifications of what they do, with the same behaviour on the line.
The interop tests (`ctest -L "smartlink|agere|pctel|hsf|sm56|broadcom"`)
run it against the other modems.

These faults of the old code are removed:

- A character could be lost when the pending output of V.42bis filled a
  frame exactly.
- A decoder flush could overwrite decoded characters that the DTE did
  not accept yet.
- An idle link in compressed mode sent a FLUSH and padding each time
  LAPM asked for data.

## Tests

`link_test` (`ctest -R Link`, GoogleTest) runs the link layer without a
modem. Most tests connect two `Link`s back to back, bit by bit.

| Area | Tests | What they check |
|---|---|---|
| Compression | `V44`, `V44Preset`, `Zstd`, `ZstdDict`, `V42bis`, `V42bisStepup` | V.42bis, V.44 and zstd round trips (text, noise, escape characters, repeats, small dictionaries, flushes); V.44's transparent mode; V.cvm's preset dictionary and zstd dictionary; two V.42bis STEPUPs taken and a third a C-ERROR |
| HDLC | `Hdlc`, `HdlcFcs32`, `BitsMasked` | 200 frames through a bit error; CRC-32's check value, frames with an FCS-32 (runs of 1s among them) to receivers that take FCS-32, FCS-16 or both, and one with a bit error dropped; every item to the pump `nbits` wide with nothing above it, in every mode |
| LAPM | `Lapm`, `LapmDirections`, `LapmZstd`, `LapmFcs32`, `LapmV42bis`, `LapmNoCompression`, `LapmOneDirection`, `LapmV42bisParameters` | detection, XID, V.44 both ways; V.42bis when either side has no V.44; `+DS`/`+DS44` directions; zstd when both offer it; the 32-bit FCS (the caller's XID with and without it, the answerer's response, a SABME with each FCS, FCS-16 when either side has it off, and the data with bit errors); no compression; one direction only; each V.42bis parameter the smaller of the two |
| HST | `LapmHst` | a recorded Courier's XID command and response: the answerer's response with HST and its turn on the DM; the caller's DM (FCS-16) and its SABME first once HST is up; the starting rate between two Links; without HST, LAPM as before |
| LAPM faults | `LapmBadFrames`, `LapmNrPastVs`, `LapmUnsolicitedDm`, `LapmBitErrors`, `LapmSlowDte`, `LapmCompressedSlowDte`, `LapmN400` | frames LAPM must drop; an N(R) past V(S) ignored; an unsolicited DM (F = 0) while idle, which asks for the link (8.8/V.42: an SABME, not the link up; ignored after a release); REJ and T401 recovery; a slow DTE (RNR); five retransmissions of an unanswered XID and five polls with the line cut, no more |
| Detection and async | `DetectHangup`, `AdpCellular`, `Async`, `V14`, `OdpUnanswered`, `LapmCallerAsyncRemote` | a hangup during T400; an originator against a cellular answerer's ADPs, "EM" then "EC" (LAPM) or "EM" alone (async after T400); async; an answerer or caller falling back to async after T400; a LAPM answerer with an async caller |
| MNP | `MnpLr`, `MnpResponder`, `MnpResponderNoLr`, `Mnp`, `MnpAfterOdp`, `MnpOctets`, `MnpBitErrors`, `MnpSlowDte`, `MnpRequired`, `MnpN400` | our LR octet for octet as the Agere modem's, FCS and all, three of them unanswered, then async; our responder against the Agere modem's LR, then an LA, an LT, an LN (LNA) and an LD as HDLC frames; against SYN DLE STX with no LR after it (async after its wait, nothing sent); MNP first, and after ODPs a V.42 answerer did not answer; framing mode 2 and no optimizations; bit errors in both modes; a slow DTE (no credit); MNP required against a modem without it; N400 with the line cut |
| MNP 5 | `Mnp5`, `MnpMnp5` | round trips (text, noise, runs, every value, flushes); the Agere modem's MNP 5 octets decoded and coded again; MNP with MNP 5 both ways |
| V.76 and H.245 | `H245Messages`, `H245Extensions`, `V76`, `V76Refused`, `V76Voice`, `V76VoiceHeader`, `V76VoiceSilence` | H.245's messages against an ASN.1 compiler's octets, extension additions passed over, messages cut short; the data channel opened, the data both ways with and without bit errors, the DISC; the channels and frames a responder refuses; the voice channel beside the data, each side hearing the other's tone, the audio header's sequence numbers, lost frames concealed, silence sent as SID frames and heard as comfort noise ([v70-details.md](v70-details.md#tests)) |
| V.80 | `V80Transparent`, `V80Shielding`, `V80Framed`, `V80BadFcs`, `V80Abort`, `V80Underrun`, `V80Commands`, `V80SynHunt`, `V80ReceiveOverrun` | transparent octets both ways with the first underrun reported; all twenty shielding codes, and DC1/DC3 with and without flow control; frames with no FCS, FCS-16 and FCS-32, with flags or marks between them; a bad FCS; the DTE's abort; underruns in a frame (an abort, or a flag and `<EM><resume>`'s count); the commands for the core; `<EM><bnum>` reports; the SYN hunt; a receive overrun (`<EM><rover>`) |

## V.44

The compressed data of V.44 is as the code of the Agere modem has it.
The V.44 encoder and decoder of that modem, read from the library, agree
with each other and with ours. `v44.h` describes the tokens.

**Negotiation.** The modems negotiate V.44 in the user data subfield
(0xFF) of XID, after the groups:

| PI | Parameter |
|---|---|
| 0x40 | the parameter set "V44" |
| 0x41 | C0 (0: the stream method) |
| 0x42 | P0, the directions (as V.42bis's) |
| 0x43 / 0x44 | the codewords, transmit / receive |
| 0x45 / 0x46 | the longest string, transmit / receive |
| 0x47 / 0x48 | the history, transmit / receive |

For each pair, each side uses the smaller of its own value and the value
of the remote. The result is the same in the two directions.

**Coding.** The two sides start in compressed mode, with codewords from
4 in 6 bits and ordinals of 7 bits.

- Our encoder finds the longest dictionary string at the next characters
  (from their first two). It extends this string with the characters
  that followed it in the history.
- It sends STEPUP when a codeword or character needs it. It sends REINIT
  when the dictionary or the history is full (the Agere decoder accepts
  no more history than that).
- It sends FLUSH, with the remaining bits of the octet, when the DTE has
  no more data at that time. It stays in compressed mode.
- The decoder also accepts transparent mode (ETM, escapes as in V.42bis,
  ECM back). The Agere encoder changes to this mode on data that does
  not compress.

**Against the Agere modem** (`ctest -R agere_v44`), 60000 octets of
text, binary octets, repeats and noise go through in each direction at
V.34 (the Agere modem reports `CONNECT 33600 V44`). Its encoder goes to
transparent mode and back from time to time. The other Agere and
interop calls also use V.44. The exceptions are the calls with the
original Smart Link library, which has no V.44 (they use V.42bis).

**Preset dictionary** (V.cvm only). `V44::prime()` loads a text into the
dictionary and history, as if the text was encoded and decoded. After
that, REINIT goes back to that state, not to empty ones. On a V.cvm call
where the two DCEs indicate it, the core tells the link
(`Link::set_v44_preset()`). When XID agrees on V.44, the two ends prime
with the text of `link/v44_preset.cpp`
([vcvm.md](dsp/vcvm-details.md#compression-v44-with-a-preset-dictionary)).

## zstd

zstd is a small Zstandard codec that we wrote from RFC 8878
(`link/zstd_codec.{h,cpp}`, approximately 1700 lines, no libzstd), with
its negotiation in XID. Annex F of the V.cvm Recommendation
(`docs/v_cvm.pdf`) is its specification, but the link also uses it
outside V.cvm.

### Negotiation

The XID command has a set `ZST` (PI 0x60) in the private parameter
group, after the V.42bis parameters. The set contains:

- P0 (0x61), the directions, as for V.42bis;
- the log of the window (0x62): 15, a 32 kB window;
- the ID of the dictionary (0x63, 4 octets): 0x56434D01, or 0 for none.

A responder that has zstd answers with only that set: the directions,
the smaller window, and the dictionary if the two modems named it. Then
the two ends use zstd and not V.44. Other modems ignore the set and
answer for V.44 or V.42bis.

Before, the set was in the user data subfield of V.44. The Intel 537EP
code disconnects the call when that subfield has a set that it does not
know (`intel_v44_open_calls`). Thus, we moved the set.

`AT+DSZ=<on>,<dict>` (default `1,1`) offers zstd and its dictionary. On
V.cvm, the INFO0 bits can remove the two (`Link::set_zstd()`). CONNECT
shows `/ZSTD` (numeric compression result 70).

### Frames

Each direction is one frame from the first data. The frame has no
content size and no checksum (LAPM has its FCS), and it never ends.

- The encoder sends a block each time the DTE has no more data in the
  queue, and at a maximum of 1024 characters. LAPM asks for characters
  only when it can send a frame. Thus, bulk data makes large blocks and
  typed text makes small blocks.
- The decoder accepts all frames that RFC 8878 allows. If libzstd is
  installed, `zstd_ref_test` checks it in the two directions against
  libzstd at levels -5 to 19.

### Encoder

- Hash chains (48 deep) over a 32 kB window. The encoder tries the
  repeat offsets first, and uses a lazy match.
- Literals are raw, RLE or Huffman coded (a new table, or the last table
  again).
- Each sequence table is predefined, repeated, or a new FSE table of the
  codes of the block. The encoder uses the one with the lowest cost.
- A raw block when coding does not make the data smaller.

On text, it compresses approximately as well as level 3 of libzstd.

### Dictionary

`link/zstd_dict.cpp` contains a formatted dictionary (RFC 8878 section
5). Its content is the preset text of V.44 (Annex E). We made its
entropy tables (155 octets) one time from that text with
`Zstd::make_dict()`, and then froze them. Annex F lists them, and
`v44_preset_check` compares them. libzstd loads the dictionary for
compression and for decompression.

| 1264 octets of six short messages | V.44 | V.44 primed | zstd | zstd + dictionary |
|---|---|---|---|---|
| each on its own | 958 | 485 | 1121 | 474 |
| in a row | 826 | 449 | 829 | 414 |

On 50 kB of text without flushes, zstd gives 1325 octets and V.44 gives
4881. A block costs a minimum of 3 octets of header. Thus, characters
flushed one at a time cost more than with V.44. That occurs with typed
text, where it is not important at those rates.

### Compression benchmark

`compress_bench` (`link/test/compress_bench.cpp`) runs each compressor
on files as the link does (the largest parameters of XID, the preset and
dictionary of V.cvm). It reports the ratio and the speed. It decodes
each stream again and compares it. Where libzstd is found, it also shows
the libzstd values.

- `compress_bench --check` runs only the round trips (the ctest
  `compress_bench_check`).
- `dsp/test/compress_calls.sh` makes V.cvm calls between two open modems
  through netsim, with files in the two directions (the `BULK_FILE` of
  interop_call), for each line and compression.

**Ratios** (input divided by output) on 192 to 256 kB of each type,
with a flush at the end only (bulk data, where the DTE always has more):

| | V.42bis | V.44 | V.44 + preset | zstd | zstd + dict | libzstd 3 | libzstd 19 |
|---|---|---|---|---|---|---|---|
| docs (Markdown) | 1.86 | 2.05 | 2.00 | 2.34 | 2.39 | 2.31 | 2.74 |
| C++ source | 2.10 | 2.54 | 2.41 | 2.93 | 3.04 | 2.88 | 3.48 |
| HTML | 3.32 | 5.08 | 4.64 | 8.33 | 8.66 | 6.02 | 10.58 |
| BBS screens (ANSI) | 11.66 | 18.80 | 14.86 | 21.32 | 21.42 | 23.32 | 37.22 |
| PPP, HTTP in IP | 3.11 | 4.31 | 4.00 | 6.01 | 6.12 | 5.00 | 7.26 |
| ELF binary | 3.73 | 4.48 | 4.13 | 5.69 | 5.73 | 6.01 | 6.98 |
| gzip, random | 1.00 | 0.90 | 0.90 | 1.00 | 1.00 | 1.00 | 1.00 |

- **Bulk data.** zstd is 14 to 65% better than V.44. It is also better
  than level 3 of libzstd at the same 32 kB window. Level 19 of libzstd
  is 15 to 25% better again. Here, the dictionary adds 1 to 4%. The
  preset makes V.44 a little worse, because it fills half the history.
- **Short sessions** (each 256 octets is a different call). The
  dictionary is the important item: text 1.16 to 1.54, HTML 1.34 to
  1.84 (the V.44 preset: 1.39 to 1.60 and 1.67 to 2.00). At 4 kB, zstd
  with its dictionary is better again, but not on the BBS screens.
- **A flush each 64 characters** (typed text). V.44 is better (text
  1.88 against 1.68, BBS screens 8.05 against 5.47), because each zstd
  block has its header and tables. A flush each 1500 characters causes
  only a small change.
- **Incompressible data.** The raw blocks of zstd keep it at 1.00. V.44
  has no transparent mode here and loses 10%.
- **Speed.** On one core of a 2.1 GHz Xeon, zstd encodes at 6 to 27 MB/s
  and decodes at 58 to 275 MB/s. V.44 encodes at 14 to 144 MB/s and
  decodes at 13 to 183 MB/s. V.cvm needs half a kB/s.

**V.cvm calls over G.711 alone** (CONNECT 3500, 32 kB in the two
directions, with the two directions checked): the data rate on the clock
of the call, in kbit/s.

| | none | V.44 | V.44 + preset | zstd | zstd + dict |
|---|---|---|---|---|---|
| docs | 3.32 | 6.60 | 6.60 | 7.05 | 7.52 |
| HTML | 3.30 | 14.62 | 14.05 | 16.34 | 18.16 |
| BBS screens | 3.32 | 36.51 | 32.81 | 34.20 | 35.52 |
| PPP, HTTP in IP | 3.30 | 12.48 | 12.42 | 14.07 | 15.15 |
| ELF binary | 3.31 | 13.02 | 12.42 | 13.43 | 14.05 |
| random | 3.19 | 2.90 | 2.90 | 3.26 | 3.25 |

**Through AMR 12.2 and AMR-WB 23.85** (three Markdown files of 32 kB,
in the two directions, six calls at the same time), in kbit/s in each
direction. [vcvm.md](dsp/vcvm.md) tells what caused these calls to
disconnect before.

| | none | zstd + dict |
|---|---|---|
| AMR 12.2 (CONNECT 1125 and about 1600) | 0.8 to 1.1, 0.6 | 2.4, 1.4 |
| AMR-WB 23.85 (1687 to 2012) | 1.4 to 1.6 | 1.8 and 2.4; 0.6 and 1.0 |

The AMR-WB rates change with the renegotiations that the codec causes.
After a retrain, two of its three zstd calls went down to 300 bit/s.

## MNP

MNP 2 to 4 (Microcom Networking Protocol) is the "alternative
procedure" of V.42 (Annex A of V.42 (10/96)). It is the error correction
of the modems from before LAPM. V.42 (1988) added it for these modems.

- V.42 (03/2002) deleted it. Its text is in the superseded editions
  (`T-REC-V.42-199610-S` at itu.int). `docs/specs/fetch.sh` does not
  download this text.
- MNP 5 is the compression of MNP. It is not part of V.42. We wrote it
  as the code of the peers has it.
- All the other soft modems of the interop tests ([interop.md](interop.md))
  have the two: the Agere, PCTel, Motorola SM56, Conexant HSF, Broadcom,
  Intel and ESS modems.
- The slmodemd of Smart Link, which this modem started from, had
  neither of them. Its link layer was LAPM only.

### When a call takes it

- **The originator with V.42** (`\N2`, `\N3`, `+ES=3`) sends its LR in
  these conditions:
  - The answerer sends no ADP in T400 (7.2.1.2/V.42: it "may attempt
    to detect the presence of an alternative error-correcting
    capability").
  - Its LAPM gets no answer.

  It does not send its LR after an ADP that tells that there is no
  error correction ("E" NUL).
- **With MNP first or alone** (`\N5`, `+ES=4`), the originator does not
  do the detection phase. It sends its LR after 64 characters of marks.
- **The answerer** accepts an LR in the detection phase (SYN DLE STX in
  its characters) as the start of the protocol phase (7.2.1.3/V.42).
  The framer of MNP gets the remaining part of the frame from there.
- **With MNP allowed**, the detection phase of the answerer is three
  times T400 (2.25 s). The reasons are:
  - The MNP of the SM56 modem sends its LR 1.1 s after the connection.
  - A V.42 originator sends its LR after its ODPs (an answerer with MNP
    alone does not answer them).
- **With no LR answered** (A.7.2.2), the call continues without error
  correction. But if error correction is required (`\N2`, `\N4`,
  `\N5`, or `+ES` with a required fallback), the link fails (NO
  CARRIER).

### The establishment

Our LR is the same as the LR of the peers, octet for octet:

- framing mode 3, k = 8, N401 = 64;
- the two data phase optimizations: the first (256-octet information
  fields) and the second (fixed-field LT and LA);
- parameter 9 = 1 (MNP 5) when we want MNP 5.

The PCTel modem does not accept an LR with N401 256 (it disconnects the
call 2 s later). The other peers also offer 64 with the 256-octet
optimization (the HSF modem offers 256). All the peers send and accept
LTs of 256 octets when the two ends have the optimization.

The responder answers with the agreed values: the lower mode, k and
N401, and the optimizations that the two ends have. It answers only
with the parameters that were in the LR of the initiator and that it
knows. It ignores parameter 14 of SM56 (the P0, P1 and P2 of V.42bis in
MNP: 3, 2048, 32).

The establishment exchange goes in start-stop octets. The data phase
goes in HDLC frames (mode 3: the flags, zero insertion and FCS of LAPM)
or in octets (mode 2). The link sends an LR three times, with T401 at
2 s plus the time of three LRs. If the LA of a responder is lost, the
responder accepts the first data phase frame of the initiator as the LA.

### The data phase

LT frames carry the data of the DTE (or of MNP 5), numbered modulo 256.
Up to k of them can be unacknowledged, in the credit of the last LA of
the remote.

We send an LA in these conditions:

- for an out-of-sequence LT (we ignore the first repeated V(R) - 1);
- for a bad frame;
- when k / 2 LTs are unacknowledged;
- after an LT, when we have no data to send;
- when T402 (half of T401) or T404 (3 s, or 7 s below 2400 bit/s)
  expires.

While the DTE does not accept the data of an LT, the credit of the LA
is 0. We discard the LTs that come after it, until an LA gives credit
again.

An LA with the N(R) of the last LA causes a retransmission from the
first unacknowledged LT. (We ignore the LAs that the LTs sent after a
lost LT cause.) T401 does the same. T401 is the time of k frames of
N401 octets at the line rate, plus 1 s. After twelve tries (N400)
without progress, we send LD.

An LN gets its LNA. We do not send the break to the DTE, and we do not
send an LN. We hang up with an LD (reason 255, user-initiated). The link
is released when the LD is sent.

From time to time, the HSF modem sends LTs with no information with
MNP 5 (A.6.6.1 has none). We accept them in sequence. At the end of a
call, the log has the counts of LTs sent, sent again and received.

### MNP 5

MNP 5 (`mnp5.h`) is as the code of PCTel has it (`MnpCompress`,
`MnpDecompress` and `MnpEncSort` in its `control.o`). The octets of the
Agere modem decode with it, and our encoder sends the same octets for
the same text (`Link.Mnp5`).

- **Run-length coding.** After three equal characters, a count of the
  equal characters after them follows (0 to 250; at 250 a new run
  starts).
- **Adaptive frequency coding.** The characters have ranks from how
  frequently they came:
  - A character moves up past the characters with lower counts.
  - When the top count gets to 255, all counts are divided by 2.
  - Each character goes as the code of its rank: a 3-bit header h, then
    h bits (1 bit for h = 0). Ranks 0 and 1 are `000x`, 2 and 3 `001x`,
    4 to 7 `010xx`, and so on up to 128 to 255 `111xxxxxxx`. Rank 255
    has a 0 after it.
  - A count is coded as the rank of its value, outside the ranking.
- **Bit order and flush.** The codes fill each octet from its most
  significant bit. Eleven 1s are the flush, and 1s fill the remaining
  bits of its octet. The encoder flushes when the DTE has no more data
  (if there were characters after the last flush). The stream continues
  from one LT to the next.

Text becomes approximately two thirds of its size. Noise becomes a
little larger (ranks of 128 and higher use 10 bits). `%C1` and `%C3`
offer MNP 5 (parameter 9 of the LR).

### With the peers

`ctest -R mnp` tests each peer in the two directions, with 20000 octets
of text, binary octets, repeats and noise in each direction, and MNP 5:

- The peer calls with MNP alone (`+ES=4,4,6`), and our default answerer
  accepts its LR.
- We call with MNP alone (`\N5`), and the peer answers with MNP alone
  (`+ES=3,0,6`).

All of these calls use framing mode 3, k = 8, with 256-octet LTs, and
no retransmission on the clean line of netsim. We test MNP 4 alone (our
`%C2`, no MNP 5) in the two directions with the Agere modem, and with
the ESS modem as the caller.

| The peer | It calls (MNP alone) | It answers (MNP alone) |
|---|---|---|
| Agere | CONNECT 33600 MNP5 | CONNECT 33600 MNP5 |
| PCTel | CONNECT 115200 (MNP 5) | CONNECT 115200 (MNP 5) |
| SM56 | CONNECT 33600/MNP/MNP5 | CONNECT 33600/MNP/MNP5 |
| HSF | CONNECT 115200 (MNP 5) | CONNECT 115200 (MNP 5) |
| Broadcom | CONNECT 33600/MNP/MNP5 | CONNECT 33600/MNP/MNP5 |

We test the Intel and ESS modems in the same way. This page does not
list their CONNECT messages.

With their defaults (`+ES=3,0,2`: V.42 with the detection phase, then
MNP), all calls between a peer and us are V.42, as with all modems that
have V.42. MNP is for the modems that have only MNP (from before V.42,
1988 and before) or that have that configuration. Our CONNECT messages
(`AT&A`) show `Protocol: MNP` and `Compression: MNP5`.

### MNP 10 and 10EC

**What they are.** MNP 10 is the protocol of Microcom for adverse lines:
"MNP 10 protocol and MNP Extended Services enhance performance under
adverse channel conditions such as those found in rural, long distance,
or cellular environments. An MNP 10 connection is established when an
MNP 2-4 connection is negotiated with a remote modem supporting MNP
10". With MNP Extended Services (`-K1`), a V.42 connection changes to
MNP 10 when the two modems have it (the RCV336ACF designer's guide of
Rockwell, 1105, 2.6).

MNP 10EC ("Enhanced Cellular", from Rockwell) does more:

- at the physical layer, "optimizing [the modulation's] responses to
  sudden changes in the cellular connection" (fades, hand-offs);
- at the protocol layer, "dynamically adjusting speed and packet size
  based on signal quality and data error performance" (2.7).

`-SEC=1[,<tx level>]` sets it to on, with a transmit level of 0 to -30
dBm. These commands are related to it: `)M` (power level adjustment),
`@M` (initial cellular power level), `*H` (link negotiation speed) and
`-Q` (fallback to V.22bis/V.22). The guide of a cellular modem (the
CDS-9022 of Data Remote, 2002) lists `)M`, `@M`, `*H` and `-Q` as
"included only for compatibility and [performing] no function". In the
detection phase, a 10EC answerer sends "EM" five times or more, and then
"EC" (Appendix VI/V.42: "a proprietary cellular phone modem protocol").

**Why D-Modem does not support them:**

- Microcom and Rockwell did not publish MNP 10 or 10EC. They gave
  licences for them to modem manufacturers. The physical layer of 10EC
  is in the modulations of Rockwell.
- There is no modem to test against. None of the soft modems above has
  a sign of MNP 10 or 10EC (no code, no strings, no `-SEC` command).
  They were in Rockwell and Conexant controller-based modems (RCV336ACF
  and similar) and in cellular data devices.
- The modems negotiate MNP 10 as an MNP 2 to 4 connection (which D-Modem
  now has) with the parameters of MNP 10. These parameters are not
  published.
- Its use was data over analogue (AMPS) cellular, which is gone (in the
  US since 2008). Digital cellular voice codecs do not carry modem
  signals. Circuit-switched data used the modems of the network (the
  interworking function). On the lines of D-Modem (G.711 over SIP or a
  PBX), the impairments that 10EC corrects do not occur at our end.

**What D-Modem does with a 10EC modem** is V.42:

- As the originator, it ignores the "EM" ADPs and accepts the "EC" after
  them (LAPM; `Link.AdpCellular`).
- As the answerer, it sends only "EC". A 10EC originator accepts this as
  a V.42 answerer.
- D-Modem accepts the Rockwell cellular commands (`-SEC`, `-K`, `-Q`,
  `)M`, `@M`, `*H`), but they have no effect. A setup string for a
  cellular-ready modem, for example the CDS-9022 string
  `AT&F;+MS=V32,1,4800,9600;-SEC=1,12;)M1;\N2`, gives OK (refer to the
  AT reference, "Setup strings from other modems").

## V.80

The Synchronous Access Mode of V.80 (8.8/V.80) is for DTEs that do their
own framing over a modem: H.324 videophones (the call function of V.8,
H.223 frames), or V.42 and other protocols that run in the DTE. The
modem does not do error correction and compression. It moves the octets
of the DTE as a synchronous bit stream: no start and stop bits on the
line. In-band commands go in the stream of the DTE after an EM character
(19h), in the two directions (Table 9/V.80).

`AT+ES=6` (to call) or `AT+ES=,,8` (to answer) asks for it. The call
reports CONNECT when the data pump is up, as it does without error
correction. Then the mode starts in its transparent sub-mode.

### Transparent sub-mode

- The octets of the DTE go out least significant bit first. The bits
  from the line come back as octets at the alignment that they have.
- When the DTE has no data to send, the `<syn1>` of `+ESA` goes out
  (`<syn1>` and `<syn2>` in turn with `<trans_idle>` 2). The DTE gets
  `<EM><under>` (one time, until it sends again).
- With `<trans_idle>` 1 or 2, the receiver hunts (8.8.1.2/V.80,
  `SyncAccess::syn_scan()`). It examines the bits one at a time until
  the last 8 are `<syn1>` (or the last 16 are `<syn1>` then `<syn2>`).
  From there, it gives the DTE octets aligned on them, with the SYN
  first.
- It hunts at the start of the mode, each time it goes back to
  transparent, and on the `<EM><hunt>` of the DTE (which also flushes
  the data that waits for the DTE). With `<trans_idle>` 0, `<EM><hunt>`
  does nothing.

### Framed sub-mode

The framed sub-mode starts with the first `<EM><flag>` of the DTE. It
sends HDLC frames with zero insertion.

**Transmission:**

- The DTE ends each frame with `<EM><flag>`. The DCE adds the FCS if the
  `<crc_type>` of `+ESA` asks for one (1: FCS-16, 2: FCS-32;
  8.1.1.6/V.42), and the flag.
- Between frames, it sends flags or marks (`<framed_idle>`).
- If the DTE does not complete a frame (an underrun), the frame ends in
  an abort (`<framed_un_ov>` 0), or in a flag without FCS (1). In the
  second case, the DCE discards the octets of the DTE until
  `<EM><resume>`. `<EM><unum>` answers it with their count. The two
  cases report `<EM><under>`.
- `<EM><err>` from the DTE sends an abort.

**Reception:**

- The receiver hunts for a flag (`<EM><err>` when it finds one). Then it
  gives the octets of each frame followed by `<EM><flag>` (whole octets,
  a good FCS) or `<EM><err>`.
- When the DCE checks the FCS, it does not send the FCS to the DTE (the
  DTE also sent none).
- The DCE reports seven 1s as `<EM><mark>` and starts a new hunt.
- `<EM><hunt>` starts a hunt. `<EM><mark>` goes back to transparent.

### Shielding and buffering

- To the DTE, EM and 99h are always shielded (`<EM><t1>`, `<EM><t2>`).
  DC1 and DC3 are also shielded when the DTE has DC1/DC3 flow control
  from the modem (`+IFC=,1`).
- From the DTE, the DCE accepts all the codes T1 to T20. It discards
  bare DC1 and DC3 when the DTE uses them to stop the flow from the
  modem (`+IFC=1`).
- Data that the DTE does not accept waits in a ring of 2048 octets.
  After that, received octets are lost, and `<EM><rover>` marks the
  position.

### What the core does

The core (`core/modem.cpp`, the owner of the link) processes the
commands:

| From the DTE | Effect |
|---|---|
| `<EM><ecs>` | the escape (`+++` is data here): answers `<EM><ecs>` and OK in online command state; `ATO` goes back |
| `<EM><eot>` | answers `<EM><eot>` and hangs up (NO CARRIER) |
| `<EM><rate>` | sets the highest rate of `+MS` from its `<tx>` code, and the highest receive rate (`MDMPRM_MAX_RX_RATE`) from its `<rx>`, for the next retrain or renegotiation |
| `<EM><rtn>`, `<EM><rrn>` | ask the pump for a retrain or a renegotiation (`Modem::link_req()`, as `ATO1` and `ATO3`; V.34, V.90 and V.92 only) |

To the DTE:

- When the carrier of the remote stops, the DTE gets `<EM><eot>` before
  NO CARRIER.
- The pump counts retrains and renegotiations in `dp_stats_`. For each
  one that the DTE did not ask for (that is, those of the remote and of
  the pump), the DTE gets `<EM><rtn>` or `<EM><rrn>`, each followed by
  `<EM><rate>`.
- The core also sends `<EM><rate>` when the rates of the pump change
  during the call. The codes of Table 10 stop at 33600. The core sends
  higher rates as 2Eh.
- The `<off>` of `+ITF` limits the data that the buffer of the DTE
  accepts (`dte_space()`). Its `<report_period>` sends `<EM><bnum>`,
  with the data that waits in the buffer (EM codes counted as octets),
  at that interval in units of 10 ms.

### Not supported

- NRZI, V.34 half duplex (`<hd_auto>`, `<EM><pri>`, `<ctl>`...), and
  off hook after `<EM><eot>`.
- The DCE does not accept the `+ES` values 5 and 7 (Synchronous Mode,
  Frame Tunnelling Mode).

### Against the other modems

`ctest -R _v80_` ([interop.md](interop-details.md#v80)):

- The Agere and Broadcom modems (which share one V.80 code base) and the
  SM56 modem accept our full frames, with no FCS and with FCS-16. We
  accept their frames.
- The Intel modem accepts our frames, but its frames end in 16 1s.
- Agere and Broadcom remove the FCS as we do (with an unwanted EM when
  an FCS octet needed shielding). The SM56 modem sends it to its DTE.
- In the transparent sub-mode, the Agere and Broadcom modems hunt for
  our SYN, 8 and 16 bit, and we hunt for their SYN (`_v80_syn`). Their
  DCE loses the first of two sequential octets that it shields for its
  DTE.
