# The data link layer: technical details

This is the technical companion to [The data link layer](link.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

`link/` is the code that runs between the DTE's characters and the data
pump's bits once a call has connected. (The DTE, data terminal
equipment, is the computer program on the other side of the serial
port or pty.) It is C++, in `namespace dlink`.

## The protocols

### Error correction and framing

- **The V.42 detection phase** (`Detector`, `hdlc.cpp`). The originator
  sends ODPs (originator detection patterns), the answerer answers with
  ADPs (answerer detection patterns), or the remote skips the phase and
  sends HDLC flags straight away.
  - T400 (750 ms) runs at both ends. When it runs out, the remote has no
    V.42 and the call goes on as asynchronous characters.
  - Two HDLC flags in a row count as the remote's V.42; one alone is
    passed over, as noise from a receiver that is still settling.
  - ADPs other than "EC" and "E" + NUL, such as a cellular modem's "EM"
    (Appendix VI/V.42), are passed over: the "EC" that follows them
    decides.
- **V.42 LAPM** (`Lapm`, `lapm.cpp`), the Link Access Procedure for
  Modems, over **HDLC** framing (`Hdlc`):
  - sequence numbers modulo 128, and windows of up to 15 frames of up to
    128 octets;
  - REJ recovery (no SREJ). A frame whose N(R) is past every frame sent
    is taken for noise that passed the FCS (frame check sequence), and
    ignored;
  - RNR while the DTE does not take the data;
  - T401 counted in transmitted bits: 2 s at the data pump's rate plus
    the time of three frames. With I frames outstanding in the data
    phase, a frame is the longer of the two N401s (and at least 128
    octets); otherwise it is 64 octets. The extra time lets an answer
    arrive at V.cvm's low rates (`Lapm::t401_bits()`);
  - N400 = 5 retransmissions (of an XID, SABME, DISC or the data phase's
    polls) before it gives up;
  - parameter negotiation in XID, with V.42bis's private parameters
    (and zstd's set after them) and V.44's user data subfield;
  - USRobotics HST in XID's user data subfield, as a Courier sends it
    (`Config::hst`, for `AT+MS=HST`): the set `HST` (PI 0xFF), the
    capability (PI 0xC0: options, then the forward rates as bits 2 to
    7 for 4800 to 16800 bit/s) and a PI 0xF1 of 6. When both offer it,
    the originator answers the XID response with a DM (F = 0, with an
    FCS-16) instead of an SABME, the answerer takes that DM as the
    turn, and both leave the 2400 bit/s pump for HST at 9600 (else the
    nearest rate both have: `Owner::hst()`). Once HST connects, LAPM
    goes on without the detection phase or a second XID: the
    originator's SABME first ([HST](dsp/hst.md));
  - the 32-bit FCS (10 c and 8.10.2/V.42), on unless `Config::fcs32`
    is off. The originator asks for it with bit position 17 of the HDLC
    optional functions in its XID. An answerer that agrees sets 17 and
    clears 16 in its XID response, then checks each frame against both
    FCSs until the SABME arrives. The SABME's FCS is what the link uses
    both ways from then on. The originator sends its SABME and every
    frame after it with an FCS-32. It takes either FCS until the first
    FCS-32 comes back, then only FCS-32. An FCS-16 misses about one
    corrupted frame in 65536; an FCS-32 about one in 4 billion. A modem
    that does not set 17 in its response gets an FCS-16 as before.
- **MNP 2 to 4** (`Mnp`, `mnp.cpp`), V.42's "alternative procedure"
  (Annex A of V.42 (10/96)), for modems that have no LAPM. The LR, LR,
  LA establishment goes in start-stop octets (`MnpAsync`); the data
  phase in HDLC frames (framing mode 3) or octets (mode 2). LT frames
  carry up to 256 octets, 8 outstanding; LA acknowledges with credit;
  frames are sent again on T401 or a repeated N(R), with N400 = 12; LN
  and LNA; LD. See [MNP](#mnp) below.
- **HST's own link** (`HstLink`, `hst_link.cpp`), the one a USRobotics
  Courier runs once both modems agreed on HST in V.42's XID and left
  V.22bis for it (`Link::hst_switch()`). Its frames have no address: a
  type octet, then N(S), N(R) or both, modulo 256. Data frames of up to
  64 octets (up to 256 taken), 8 outstanding, cumulative RRs, REJ and
  T1 recovery, and the asks and grants that turn HST's channels around
  (`Owner::hst_turn()`). No compression. See
  [HST's technical details](dsp/hst-details.md).
- **Asynchronous characters** (`Async`): a start bit, 8 data bits and a
  stop bit, for the modulations without V.42 and when the remote has no
  LAPM.

### Compression

- **MNP 5** (`Mnp5`, `mnp5.cpp`): run-length and adaptive frequency
  coding. It is MNP's compression (it is not in V.42), written as the
  peers' code has it.
- **V.42bis** (`V42bis`, `v42bis.cpp`): we offer 2048 codewords and
  strings of 34 characters, lower when the remote asks. The encoder
  switches between transparent and compressed mode by the ratio over the
  last 256 characters.
- **V.44** (`V44`, `v44.cpp`): we offer 4096 codewords, strings of 255
  and a history of 12288 octets (as the Agere modem does), lower when
  the remote asks. The caller offers both V.42bis and V.44 in its XID;
  the answerer takes V.44 when both have it, and answers with what it
  took. See [V.44](#v44) below.
- **zstd** (`Zstd`, `zstd_codec.cpp`; Zstandard, RFC 8878): D-Modem's
  own. It is offered beside V.42bis and V.44 in every mode with LAPM,
  and taken when both modems offer it (so between two D-Modems);
  otherwise V.44 or V.42bis is used as before. See [zstd](#zstd) below.

### Instead of all the above

- **V.80 Synchronous Access Mode** (`SyncAccess`, `v80.cpp`), when the
  DTE asks for it (`AT+ES=6`, or `AT+ES=,,8` answering): the DTE's
  octets go as a synchronous bit stream, transparent or as HDLC frames,
  with in-band commands after an EM character. See [V.80](#v80) below.
- **V.76's multiplexer** (`Mux`, `v76.cpp`), when V.8 bis chose V.70's
  DSVD (`Link::start_v76()`): the DLCs share the HDLC link by the DLCI
  in their address octet; the data channel is a `Lapm` with its DLCI
  (V.76's ERM), opened and closed with V.75's H.245 messages
  (`h245.cpp`, aligned PER) in its SABME, UA, DM and DISC. With a voice
  path (`Owner::voice()`), a voice channel beside it: G.729 Annex A
  (`g729a.h`, D-Modem's own coder) in UNERM UI frames, one every 10 ms
  of line bits, ahead of the data; in silence Annex B's SID frames and
  comfort noise, so the data has the line. No detection phase, XID or
  compression. See [v70-details.md](v70-details.md).
- **X.75** (`Lapb`, `lapb.cpp`) over HDLC, on a clear 64 kbit/s channel
  with `AT+B2=0`. This is LAPB in basic mode (modulo 8): a window of 7 I
  frames of up to 256 octets (up to 2048 taken), REJ recovery, and N2 =
  4 tries. T1 is 1 s plus the time of our longest frame and two of the
  remote's longest. A hangup sends DISC. See [isdn.md](isdn.md).
- **V.120** (`Lapb` again) over HDLC, with `AT+B2=9`: the same
  procedures in extended mode (modulo 128, SABME), Q.921's two-octet
  address for logical link 256, and V.120's terminal adaption header
  (`0x83`) before each I frame's information.
- **Sync PPP** (`PppSync`, `ppp_sync.cpp`) over HDLC, with `AT+B2=5`.
  The DTE's PPP frames come in asynchronous framing (RFC 1662); they are
  unescaped, their FCS is checked and taken off, and each one goes on
  the line as an HDLC frame. The line's frames go back to the DTE
  escaped, with an FCS.

## How it fits together

`Link` (`link.h`) puts the protocols together.

- The modem core is its `Owner`. The owner gives and takes the DTE's
  characters, hears of the link's progress (`Status::Detected`, `Up`,
  `Released`, `Error`) and of what XID agreed, and gets the log lines.
- The data pump calls `get_bits()` and `put_bits()`.
- The link does not depend on the modem core or the data pumps.

`stats()` counts a call, for the core's `ATI11`:

- the DTE's characters each way;
- the frames LAPM or MNP sent (by `Hdlc` and `MnpAsync`), and the
  information frames among them that were sent again (after LAPM's REJ
  or poll, or MNP's retransmissions);
- the frames received intact, and those with a bad FCS;
- in the data phase, the times T401 ran out, and the remote's requests
  to send frames again (LAPM's REJ or SREJ, MNP's repeated LA);
- the characters received without error correction that the DTE did not
  take (lost).

## Where the code came from

The protocol code began as Smart Link's slmodemd link layer
(`modem_pack.c`, `modem_ec.c`, `modem_comp.c`), turned into classes.
Its HDLC framing, LAPM and V.42bis were written anew in 2026 from
specifications of what they do, with the same behaviour on the line.
The interop tests (`ctest -L "smartlink|agere|pctel|hsf|sm56|broadcom"`)
run it against the other modems.

Faults of the old code are gone:

- a character could be lost when V.42bis's pending output filled a frame
  exactly;
- a decoder flush could overwrite decoded characters the DTE had not
  taken yet;
- an idle link in compressed mode sent a FLUSH and padding each time
  LAPM asked for data.

## Tests

`link_test` (`ctest -R Link`, GoogleTest) runs the link layer without a
modem. Most cases connect two `Link`s back to back, bit by bit.

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

V.44's compressed data is as the Agere modem's code has it. Its V.44
encoder and decoder, read from the library, agree with each other and
with ours. `v44.h` describes the tokens.

**Negotiation.** V.44 is negotiated in XID's user data subfield (0xFF),
after the groups:

| PI | Parameter |
|---|---|
| 0x40 | the parameter set "V44" |
| 0x41 | C0 (0: the stream method) |
| 0x42 | P0, the directions (as V.42bis's) |
| 0x43 / 0x44 | the codewords, transmit / receive |
| 0x45 / 0x46 | the longest string, transmit / receive |
| 0x47 / 0x48 | the history, transmit / receive |

For each pair, each side takes the smaller of its own value and the
remote's, the same both ways.

**Coding.** Both sides start in compressed mode, with codewords from 4
in 6 bits and ordinals of 7 bits.

- Our encoder looks for the longest dictionary string at the next
  characters (found by their first two), and extends it with what
  followed it in the history.
- It sends STEPUP when a codeword or character needs it, and REINIT when
  the dictionary or the history is full (the Agere decoder takes no more
  history than that).
- It sends FLUSH, with the rest of the octet, when the DTE has nothing
  more for now. It stays in compressed mode.
- The decoder also takes transparent mode (ETM, escapes as in V.42bis,
  ECM back), which the Agere encoder switches to on data that does not
  compress.

**Against the Agere modem** (`ctest -R agere_v44`), 60000 octets each
way of text, binary octets, repeats and noise go through in both
directions at V.34 (the Agere modem reports `CONNECT 33600 V44`). Its
encoder goes to transparent mode and back now and then. The other Agere
and interop calls use V.44 too, except those with the original Smart
Link library, which has no V.44 (they use V.42bis).

**Preset dictionary** (V.cvm only). `V44::prime()` loads a text into the
dictionary and history as if it had been encoded and decoded; REINIT
then goes back to that state rather than to empty ones. On a V.cvm call
where both DCEs indicate it, the core tells the link
(`Link::set_v44_preset()`), and both ends prime with
`link/v44_preset.cpp`'s text once XID agrees on V.44
([vcvm.md](dsp/vcvm-details.md#compression-v44-with-a-preset-dictionary)).

## zstd

A small Zstandard codec of our own, written from RFC 8878
(`link/zstd_codec.{h,cpp}`, about 1700 lines, no libzstd), and its
negotiation in XID. Annex F of the V.cvm Recommendation
(`docs/v_cvm.pdf`) is its specification, but it is not used on V.cvm
alone.

### Negotiation

The XID command carries, in the private parameter group after V.42bis's
parameters, a set `ZST` (PI 0x60) with:

- P0 (0x61), the directions, as V.42bis's;
- the window's log (0x62): 15, a 32 kB window;
- the dictionary's ID (0x63, 4 octets): 0x56434D01, or 0 for none.

A responder that has zstd answers with that set alone: the directions,
the smaller window, and the dictionary if both named it. Both ends then
use zstd in place of V.44. Other modems pass over the set and answer for
V.44 or V.42bis.

The set was first in V.44's user data subfield. Intel's 537EP code drops
the call when that subfield holds a set it does not know
(`intel_v44_open_calls`), so the set moved.

`AT+DSZ=<on>,<dict>` (default `1,1`) offers zstd and its dictionary. On
V.cvm the INFO0 bits can take either away (`Link::set_zstd()`). CONNECT
says `/ZSTD` (numeric compression result 70).

### Frames

Each direction is a single frame from the first data on. The frame has
no content size and no checksum (LAPM has its FCS), and it is never
ended.

- The encoder puts out a block whenever the DTE has nothing more
  waiting, and at 1024 characters at most. LAPM asks for characters only
  when it can send a frame, so bulk data makes large blocks and typing
  makes small ones.
- The decoder takes any frame RFC 8878 allows. Where libzstd is
  installed, `zstd_ref_test` checks it both ways against libzstd at
  levels -5 to 19.

### Encoder

- Hash chains (48 deep) over a 32 kB window, the repeat offsets tried
  first, and a lazy match.
- Literals raw, RLE or Huffman coded (a new table, or the last one
  again).
- Each sequence table predefined, repeated, or a new FSE table of the
  block's codes, whichever costs least.
- A raw block when coding gains nothing.

On text it compresses about as well as libzstd's level 3.

### Dictionary

`link/zstd_dict.cpp` holds a formatted dictionary (RFC 8878 section 5).
Its content is V.44's preset text (Annex E). Its entropy tables (155
octets) were made once from that text by `Zstd::make_dict()` and then
frozen; Annex F lists them, and `v44_preset_check` compares them.
libzstd loads the dictionary for compression and decompression alike.

| 1264 octets of six short messages | V.44 | V.44 primed | zstd | zstd + dictionary |
|---|---|---|---|---|
| each on its own | 958 | 485 | 1121 | 474 |
| in a row | 826 | 449 | 829 | 414 |

On 50 kB of text without flushes, zstd comes to 1325 octets and V.44 to
4881. A block costs at least 3 octets of header, so characters flushed
one at a time cost more than V.44's. That happens with typing, where it
does not matter at those rates.

### Compression benchmark

`compress_bench` (`link/test/compress_bench.cpp`) runs each compressor
on files as the link does (XID's largest parameters, V.cvm's preset and
dictionary). It reports the ratio and the speed, decodes every stream
again and compares it, and shows libzstd's figures beside them where
libzstd is found.

- `compress_bench --check` runs the round trips alone (the ctest
  `compress_bench_check`).
- `dsp/test/compress_calls.sh` makes V.cvm calls between two open modems
  through netsim, with files both ways (interop_call's `BULK_FILE`), for
  each line and compression.

**Ratios** (input over output) on 192 to 256 kB of each kind, with a
flush at the end only (bulk data, where the DTE always has more):

| | V.42bis | V.44 | V.44 + preset | zstd | zstd + dict | libzstd 3 | libzstd 19 |
|---|---|---|---|---|---|---|---|
| docs (Markdown) | 1.86 | 2.05 | 2.00 | 2.34 | 2.39 | 2.31 | 2.74 |
| C++ source | 2.10 | 2.54 | 2.41 | 2.93 | 3.04 | 2.88 | 3.48 |
| HTML | 3.32 | 5.08 | 4.64 | 8.33 | 8.66 | 6.02 | 10.58 |
| BBS screens (ANSI) | 11.66 | 18.80 | 14.86 | 21.32 | 21.42 | 23.32 | 37.22 |
| PPP, HTTP in IP | 3.11 | 4.31 | 4.00 | 6.01 | 6.12 | 5.00 | 7.26 |
| ELF binary | 3.73 | 4.48 | 4.13 | 5.69 | 5.73 | 6.01 | 6.98 |
| gzip, random | 1.00 | 0.90 | 0.90 | 1.00 | 1.00 | 1.00 | 1.00 |

- **Bulk data.** zstd is 14 to 65% ahead of V.44, and ahead of libzstd's
  level 3 at the same 32 kB window; libzstd's level 19 is another 15 to
  25% better. The dictionary adds 1 to 4% here. The preset costs V.44 a
  little, because it fills half the history.
- **Short sessions** (each 256 octets a call of its own). The
  dictionary is what counts: text 1.16 to 1.54, HTML 1.34 to 1.84
  (V.44's preset: 1.39 to 1.60 and 1.67 to 2.00). At 4 kB, zstd with its
  dictionary is ahead again, except on the BBS screens.
- **A flush every 64 characters** (typing). V.44 is ahead (text 1.88
  against 1.68, BBS screens 8.05 against 5.47), because each zstd block
  has its header and tables. A flush every 1500 characters changes
  little.
- **Incompressible data.** zstd's raw blocks keep it at 1.00. V.44 has
  no transparent mode here and loses 10%.
- **Speed.** On one core of a 2.1 GHz Xeon, zstd encodes at 6 to 27 MB/s
  and decodes at 58 to 275 MB/s; V.44 at 14 to 144 and 13 to 183 MB/s.
  V.cvm needs half a kB/s.

**V.cvm calls over G.711 alone** (CONNECT 3500, 32 kB both ways, both
checked): the data's rate on the call's clock, in kbit/s.

| | none | V.44 | V.44 + preset | zstd | zstd + dict |
|---|---|---|---|---|---|
| docs | 3.32 | 6.60 | 6.60 | 7.05 | 7.52 |
| HTML | 3.30 | 14.62 | 14.05 | 16.34 | 18.16 |
| BBS screens | 3.32 | 36.51 | 32.81 | 34.20 | 35.52 |
| PPP, HTTP in IP | 3.30 | 12.48 | 12.42 | 14.07 | 15.15 |
| ELF binary | 3.31 | 13.02 | 12.42 | 13.43 | 14.05 |
| random | 3.19 | 2.90 | 2.90 | 3.26 | 3.25 |

**Through AMR 12.2 and AMR-WB 23.85** (three Markdown files of 32 kB,
both ways, six calls at once), in kbit/s each way.
[vcvm.md](dsp/vcvm.md) describes what made these calls drop before.

| | none | zstd + dict |
|---|---|---|
| AMR 12.2 (CONNECT 1125 and about 1600) | 0.8 to 1.1, 0.6 | 2.4, 1.4 |
| AMR-WB 23.85 (1687 to 2012) | 1.4 to 1.6 | 1.8 and 2.4; 0.6 and 1.0 |

AMR-WB's rates move with the renegotiations the codec brings. Two of
its three zstd calls ended up at 300 bit/s after a retrain.

## MNP

MNP 2 to 4 (Microcom Networking Protocol) is V.42's "alternative
procedure" (Annex A of V.42 (10/96)): the error correction of the
modems from before LAPM, which V.42 (1988) took in for them.

- V.42 (03/2002) deleted it. Its text is in the superseded editions
  (`T-REC-V.42-199610-S` at itu.int, which `docs/specs/fetch.sh` does
  not fetch).
- MNP 5 is MNP's compression, not part of V.42 at all. It is here as the
  peers' code has it.
- The other soft modems of the interop tests ([interop.md](interop.md))
  all have both: the Agere, PCTel, Motorola SM56, Conexant HSF,
  Broadcom, Intel and ESS modems.
- Smart Link's slmodemd, which this modem started from, had neither: its
  link layer was LAPM only.

### When a call takes it

- **The originator with V.42** (`\N2`, `\N3`, `+ES=3`) sends its LR when
  the answerer sends no ADP within T400 (7.2.1.2/V.42: it "may attempt
  to detect the presence of an alternative error-correcting
  capability"), or when its LAPM goes unanswered. It does not after an
  ADP that says no error correction ("E" NUL).
- **With MNP first or alone** (`\N5`, `+ES=4`), the originator skips the
  detection phase and sends its LR after 64 characters of marks.
- **The answerer** takes an LR in the detection phase (SYN DLE STX among
  its characters) as the start of the protocol phase (7.2.1.3/V.42);
  MNP's framer takes the rest of the frame from there.
- **With MNP allowed**, the answerer's detection phase lasts three times
  T400 (2.25 s). The SM56 modem's MNP sends its LR 1.1 s after the
  connection, and a V.42 originator sends its LR after its ODPs (an
  answerer with MNP alone does not answer them).
- **With no LR answered at all** (A.7.2.2), the call goes on without
  error correction, unless error correction is required (`\N2`, `\N4`,
  `\N5`, or `+ES` with a required fallback). Then the link fails (NO
  CARRIER).

### The establishment

Our LR matches the peers' octet for octet:

- framing mode 3, k = 8, N401 = 64;
- both data phase optimizations: the first (256-octet information
  fields) and the second (fixed-field LT and LA);
- parameter 9 = 1 (MNP 5) when we want MNP 5.

The PCTel modem takes no LR with N401 256 (it drops the call 2 s
later). The other peers offer 64 with the 256-octet optimization too
(the HSF modem offers 256). All send and take LTs of 256 octets once
both ends have the optimization.

The responder answers with what was agreed (the lower mode, k and N401,
and the optimizations both have), and only with the parameters that the
initiator's LR had and it knows. SM56's parameter 14 (V.42bis's P0, P1
and P2 in MNP: 3, 2048, 32) is passed over.

The establishment exchange goes in start-stop octets. The data phase
goes in HDLC frames (mode 3: LAPM's flags, zero insertion and FCS) or in
octets (mode 2). An LR is sent three times, with T401 at 2 s plus the
time of three LRs. A responder whose LA is lost takes the initiator's
first data phase frame as the LA.

### The data phase

LT frames carry the DTE's data (or MNP 5's), numbered modulo 256. Up to
k of them may be unacknowledged, within the credit of the remote's last
LA.

We send an LA:

- for an out-of-sequence LT (the first V(R) - 1 repeated is ignored);
- for a bad frame;
- when k / 2 LTs are unacknowledged;
- after an LT, when we have nothing to send;
- when T402 (half of T401) or T404 (3 s, or 7 s below 2400 bit/s) runs
  out.

While the DTE does not take an LT's data, the LA's credit is 0 and later
LTs are discarded, until an LA gives credit again.

An LA with the N(R) of the last one sends again from the first
unacknowledged LT. (The LAs that the LTs sent after a lost one bring are
passed over.) T401 does the same; it is the time of k frames of N401
octets at the line's rate, plus 1 s. After twelve tries (N400) without
progress, we send LD.

An LN gets its LNA; the break is not passed to the DTE, and we send no
LN ourselves. We hang up with an LD (reason 255, user-initiated), and
the link is released once it is out.

The HSF modem sends LTs with no information now and then with MNP 5
(A.6.6.1 has none); they are taken in sequence. At the end of a call the
log has the counts of LTs sent, sent again and received.

### MNP 5

MNP 5 (`mnp5.h`) is as PCTel's code has it (`MnpCompress`,
`MnpDecompress` and `MnpEncSort` in its `control.o`). The Agere modem's
octets decode with it, and our encoder sends the same octets for the
same text (`Link.Mnp5`).

- **Run-length coding.** After three equal characters comes a count of
  those after them (0 to 250; at 250 a new run starts).
- **Adaptive frequency coding.** The characters are ranked by how often
  they came: a character moves up past those with fewer, and all counts
  are halved when the top one reaches 255. Each character is sent as its
  rank's code: a 3-bit header h, then h bits (1 bit for h = 0). Ranks 0
  and 1 are `000x`, 2 and 3 `001x`, 4 to 7 `010xx`, and so on up to 128
  to 255 `111xxxxxxx`; rank 255 has a 0 after it. A count is coded as
  the rank of its value, outside the ranking.
- **Bit order and flush.** The codes fill each octet from its most
  significant bit. Eleven 1s are the flush, and the rest of its octet is
  filled with 1s. The encoder flushes when the DTE has nothing more (if
  there were characters since the last flush). The stream goes on from
  one LT to the next.

Text shrinks to about two thirds; noise grows a little (ranks of 128
and up take 10 bits). `%C1` and `%C3` offer MNP 5 (parameter 9 of the
LR).

### With the peers

`ctest -R mnp` covers each peer both ways, with 20000 octets each way of
text, binary octets, repeats and noise, and MNP 5:

- the peer calls with MNP alone (`+ES=4,4,6`) and our default answerer
  takes its LR;
- we call with MNP alone (`\N5`) and the peer answers with MNP alone
  (`+ES=3,0,6`).

All of these run in framing mode 3, k = 8, with 256-octet LTs, and no
retransmission on netsim's clean line. MNP 4 alone (our `%C2`, no MNP
5) is tested both ways with the Agere modem, and with the ESS modem
calling.

| The peer | It calls (MNP alone) | It answers (MNP alone) |
|---|---|---|
| Agere | CONNECT 33600 MNP5 | CONNECT 33600 MNP5 |
| PCTel | CONNECT 115200 (MNP 5) | CONNECT 115200 (MNP 5) |
| SM56 | CONNECT 33600/MNP/MNP5 | CONNECT 33600/MNP/MNP5 |
| HSF | CONNECT 115200 (MNP 5) | CONNECT 115200 (MNP 5) |
| Broadcom | CONNECT 33600/MNP/MNP5 | CONNECT 33600/MNP/MNP5 |

The Intel and ESS modems are tested the same way; their CONNECT
messages are not listed here.

With their defaults (`+ES=3,0,2`: V.42 with the detection phase, then
MNP), every call between a peer and us is V.42, as with any modem that
has V.42. MNP is for the modems that have only MNP (from before V.42,
1988 and earlier) or are set that way. Our CONNECT messages (`AT&A`)
say `Protocol: MNP` and `Compression: MNP5`.

### MNP 10 and 10EC

**What they are.** MNP 10 is Microcom's protocol for adverse lines:
"MNP 10 protocol and MNP Extended Services enhance performance under
adverse channel conditions such as those found in rural, long distance,
or cellular environments. An MNP 10 connection is established when an
MNP 2-4 connection is negotiated with a remote modem supporting MNP
10". With MNP Extended Services (`-K1`), a V.42 connection switches to
MNP 10 when both modems have it (Rockwell's RCV336ACF designer's guide,
1105, 2.6). MNP 10EC ("Enhanced Cellular", Rockwell's) adds its own
tuning of the modulations for cellular fades and hand-offs.

Neither was published. D-Modem's MNP 10 is built from the firmware of
Microcom's DeskPorte 28.8P (M042116P), checked against the TravelCard
FAST's (M012315), which runs the same code. The wire rules (frames,
parameters, the exchanges) are the firmware's exactly, so a DeskPorte
or another MNP 10 modem sees a modem of its own kind. The choices of
when to shift and how big a packet to send are the firmware's as well,
tuned where only the result shows. The code is in `link/mnp10.cpp`
(`Mnp`), the pumps' side in `core/modem.cpp` (`Modem::mnp10_shift()`).

**The negotiation.** Our LR offers MNP 10 (parameter 8 bit 4) when
S113 bit 0 is on (the default), with its parameters:

- `0x0C`: the shifts (fallback and fall forward), the 1200 and 2400
  bit/s rungs (`-Q`), cellular (`)M`), and the V.32bis and V.34 rates
  up to the top speed (`+MS`). Each side keeps what both have.
- `0x0D`: our receive level (G) and eye quality (E).
- `0x11`: the originator's mark.
- `0xC0`, D-Modem's own (S113 bit 1, the default): the modulations our
  pumps can run on this line, the top speed and `-Q`. A DeskPorte skips
  it by its length. When both sides send it, the call may move between
  every modulation both have.

With `-K1` a V.42 originator puts the same parameters in its XID (the
user data subfield's `FF` group with "MNPI"), and an answerer with
`-K1` answers in kind. The link then runs MNP 10 in HDLC frames, from
the originator's first LA. D-Modem's default is `-K0`, so that a call
between two D-Modems keeps V.42bis or V.44. The firmware's is `-K1`.

**In the data phase:**

- **Probes** (type 8, `08 0A S R [G E]`, answered by a type 9 LA)
  measure the line. The answer's arithmetic grows the LTs' size P from
  64 octets up to 256 on a clean line and shrinks it when they go again.
- **Errors**: X, the LTs sent again averaged, and H, a health count,
  say when the line is bad. A clean line with P at its top for long
  enough, a low X and a low eye quality (mean squared error), asks for
  a fall forward.
- **Shifts** go by type 8 signals: 02 asks for a fallback, 03 answers
  it, 04 asks for a fall forward, 05 answers it, 09 refuses. Hold-offs
  keep a call from going up and down: 30 s after the start, doubling
  up to 1920 s after each failed fall forward, 10 s after any shift.
- **Cellular** (`)M1`): the probes carry G and E, the other end's
  reports set our transmit level (0 to 25 dB down, starting at `@M`'s),
  and the fallbacks come sooner.
- `:T19`: at 1200 bit/s, no frame from the other end for 120 s hangs up.

**The ladder.** The rungs are the rates of each modulation, highest
first. With another maker's MNP 10 the call moves within its class:
V.34's rates, or V.32bis, V.22bis and V.22. Between two D-Modems it is
every modulation both have registered on this line: V.92 and V.90 only
while the call runs one (their server side is digital), V.34, V.FC,
V.32terbo, V.32bis, V.26ter, V.22bis, V.22, Bell 212A, V.23, V.21 and
Bell 103, and on a 4-wire leased line V.33, V.29, V.27ter, V.26 and the
Bell 201, 202 and 208. HST is left out: its start-up is V.22bis's and
its link is its own. A step goes to the nearest rate, the same
modulation first.

**The pumps' change.** Within V.32bis, V.32terbo and V.34 a shift is a
retrain (V.34 going up: a rate renegotiation) with the new rate as the
cap (`MDMPRM_MAX_RATE`). V.34 and PCM going up have no cap: they pick
their own rate. Only the side that asked starts it, and the
other's pump follows. Into another modulation the pumps start again, as
the DeskPorte does:

1. Both stop sending. The pump is gone and the link's timers stand
   still.
2. The silence lasts S10/8 + 3 s (4 s with S10's 14), then 250 ms, and
   the caller waits 600 ms more.
3. The new pump starts in the call's roles. The answerer's skips its
   answer tone and goes to its handshake's next step at once
   (`dp_runtime_set_restart()`). V.34, V.FC, V.90 and V.92 start through
   V.8 again.
4. It has 25 s to connect. The link goes on from where it was, the
   LTs outstanding sent again.

A start-up that fails goes back (a fall forward), or further down.
Between two D-Modems the modulation that would not start is not tried
again in the call. In cellular mode the call drops to its lowest rung.
More than 4 failures in a row end the call. `*H1` and `*H2` start a
caller at V.22, or at V.32bis's 4800 bit/s, and the fall forwards climb
from there.

**What is not there:**

- MNP 10EC's physical layer (Rockwell's tuning of its own modulations):
  LR parameter 8's bit 5 is never sent, and a 10EC modem's is ignored.
  In the detection phase a 10EC answerer sends "EM" five times or more
  and then "EC" (Appendix VI/V.42). As the originator D-Modem passes
  over the "EM" ADPs and takes the "EC" after them (LAPM;
  `Link.AdpCellular`), and as the answerer it sends "EC" alone.
- V.32bis's rate renegotiation. A DeskPorte shifts within V.32bis by
  renegotiating, and D-Modem by retraining with a cap. Both end at the
  new rate, but a DeskPorte that renegotiates is answered by a retrain.
- Link resume (LR parameter `0x82`), MNP security and the async
  protocol (`0x0B`, `0x80`) are not offered, and are ignored when
  received.
- `-SEC` and `-C` are accepted with no effect.

**Tests:** `Link.Mnp10Lr` (our LR's bytes), `Link.Mnp10` (the probes,
data both ways), `Link.Mnp10Fallback`, `Link.Mnp10ClimbAcross` (V.22 up
to V.34 between two D-Modems), `Link.Mnp10NoExtension`,
`Link.Mnp10Cellular`, `Link.Mnp10Upgrade` (`-K1`) and, with the real
pumps, `Core.Mnp10Climbs` (V.22 up to V.34 at 33600 bit/s, through V.22bis,
V.32bis and V.32terbo).

## V.80

V.80's Synchronous Access Mode (8.8/V.80) is for DTEs that do their own
framing over a modem: H.324 videophones (V.8's call function, H.223
frames), or V.42 and other protocols run in the DTE. The modem gives up
error correction and compression and moves the DTE's octets as a
synchronous bit stream: no start and stop bits on the line. In-band
commands go in the DTE's stream after an EM character (19h), both ways
(Table 9/V.80).

`AT+ES=6` (calling) or `AT+ES=,,8` (answering) asks for it. The call
reports CONNECT once the data pump is up, as it does without error
correction. The mode then starts in its transparent sub-mode.

### Transparent sub-mode

- The DTE's octets go out least significant bit first. The line's bits
  come back as octets at whatever alignment they have.
- When the DTE has nothing to send, `+ESA`'s `<syn1>` goes out
  (`<syn1>` and `<syn2>` in turn with `<trans_idle>` 2), and the DTE
  hears `<EM><under>` (once, until it sends again).
- With `<trans_idle>` 1 or 2 the receiver hunts (8.8.1.2/V.80,
  `SyncAccess::syn_scan()`). It takes the bits one at a time until the
  last 8 are `<syn1>` (or the last 16 are `<syn1>` then `<syn2>`), and
  from there gives the DTE octets aligned on them, the SYN first.
- It hunts at the start of the mode, on each return to transparent, and
  on the DTE's `<EM><hunt>` (which also flushes what waits for the
  DTE). With `<trans_idle>` 0, `<EM><hunt>` does nothing.

### Framed sub-mode

The framed sub-mode starts with the DTE's first `<EM><flag>`. It sends
HDLC frames with zero insertion.

**Sending:**

- The DTE ends each frame with `<EM><flag>`. The DCE adds the FCS if
  `+ESA`'s `<crc_type>` asks for one (1: FCS-16, 2: FCS-32;
  8.1.1.6/V.42), and the flag.
- Between frames it sends flags or marks (`<framed_idle>`).
- A frame the DTE leaves unfinished (an underrun) ends in an abort
  (`<framed_un_ov>` 0), or in a flag without FCS (1). In the second case
  the DTE's octets are then dropped until `<EM><resume>`, which
  `<EM><unum>` answers with their count. Both cases report
  `<EM><under>`.
- `<EM><err>` from the DTE sends an abort.

**Receiving:**

- The receiver hunts for a flag (`<EM><err>` when it finds one), then
  gives each frame's octets followed by `<EM><flag>` (whole octets, a
  good FCS) or `<EM><err>`.
- When the DCE checks the FCS it keeps the FCS back and drops it (the
  DTE sent none either).
- Seven 1s are reported as `<EM><mark>` and start a new hunt.
- `<EM><hunt>` starts a hunt; `<EM><mark>` goes back to transparent.

### Shielding and buffering

- Towards the DTE, EM and 99h are always shielded (`<EM><t1>`,
  `<EM><t2>`), and DC1 and DC3 too when the DTE has DC1/DC3 flow control
  from the modem (`+IFC=,1`).
- From the DTE, all the codes T1 to T20 are taken. Bare DC1 and DC3 are
  dropped when the DTE flows the modem off with them (`+IFC=1`).
- What the DTE does not take waits in a ring of 2048 octets. Past that,
  received octets are lost and `<EM><rover>` marks the place.

### What the core does

The core (`core/modem.cpp`, the link's owner) handles the commands:

| From the DTE | Effect |
|---|---|
| `<EM><ecs>` | the escape (`+++` is data here): answers `<EM><ecs>` and OK in online command state; `ATO` goes back |
| `<EM><eot>` | answers `<EM><eot>` and hangs up (NO CARRIER) |
| `<EM><rate>` | sets `+MS`'s highest rate from its `<tx>` code, and the highest rate to receive at (`MDMPRM_MAX_RX_RATE`) from its `<rx>`, for the next retrain or renegotiation |
| `<EM><rtn>`, `<EM><rrn>` | ask the pump for a retrain or a renegotiation (`Modem::link_req()`, as `ATO1` and `ATO3`; V.34, V.90 and V.92 only) |

Towards the DTE:

- When the remote's carrier is gone, the DTE gets `<EM><eot>` before NO
  CARRIER.
- For each retrain and renegotiation the pump counts in `dp_stats_`,
  other than those the DTE asked for itself (that is, the remote's and
  the pump's own), the DTE gets `<EM><rtn>` or `<EM><rrn>`, each
  followed by `<EM><rate>`.
- `<EM><rate>` is also sent when the pump's rates change during the
  call. Table 10's codes stop at 33600; faster rates are sent as 2Eh.
- `+ITF`'s `<off>` limits what the DTE's buffer takes (`dte_space()`),
  and its `<report_period>` sends `<EM><bnum>`, with what waits in the
  buffer (EM codes counted as octets), every so many 10 ms.

### Not supported

- NRZI, V.34 half duplex (`<hd_auto>`, `<EM><pri>`, `<ctl>`...), and
  staying off hook after `<EM><eot>`.
- `+ES` values 5 and 7 (Synchronous Mode, Frame Tunnelling Mode) are not
  taken.

### Against the other modems

`ctest -R _v80_` ([interop.md](interop-details.md#v80)):

- The Agere and Broadcom modems (which share one V.80 code base) and the
  SM56 modem take our frames whole, with no FCS and with FCS-16, and we
  take theirs.
- The Intel modem takes ours, but its own frames end in 16 1s.
- Agere and Broadcom drop the FCS as we do (with a stray EM when an FCS
  octet needed shielding); the SM56 modem hands it to its DTE.
- In the transparent sub-mode, the Agere and Broadcom modems hunt for
  our SYN, 8 and 16 bit, and we for theirs (`_v80_syn`). Their DCE loses
  the first of two octets in a row that it shields for its DTE.
