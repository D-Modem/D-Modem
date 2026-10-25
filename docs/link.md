# The data link layer

`link/` is what goes on between the DTE's characters and the data pump's
bits once a call has connected (C++, `namespace dlink`):

- **V.42 detection phase** (`Detector`, `hdlc.cpp`): the originator's ODPs,
  the answerer's ADPs, or the HDLC flags of a modem that skips the phase;
  T400 at both ends (async when it runs out: the remote has no V.42).
  Other ADPs than "EC" and "E" + NUL (such as a cellular modem's "EM",
  Appendix VI/V.42) are passed over: the "EC" after them decides.
- **V.42 LAPM** (`Lapm`, `lapm.cpp`) over **HDLC** framing (`Hdlc`): modulo
  128, windows of up to 15 frames of up to 128 octets, REJ recovery,
  RNR while the DTE does not take the data, T401 counted in transmitted
  bits (2 s at the data pump's rate) and N400 = 5 retransmissions (XID,
  SABME, DISC, the data phase's polls) before giving up, and parameter
  negotiation in XID,
  with the private parameters of V.42bis and the user data subfield of
  V.44.
- **MNP 2 to 4** (`Mnp`, `mnp.cpp`), V.42's "alternative procedure" (Annex
  A of V.42 (10/96)), with the modems that have no LAPM: the LR, LR, LA
  establishment in start-stop octets (`MnpAsync`), the data phase in HDLC
  frames (framing mode 3) or octets (mode 2), LT frames of up to 256
  octets, 8 outstanding, LA acknowledgment with credit, retransmission on
  T401 or a repeated N(R), N400 = 12, LN and LNA, LD. See below.
- **MNP 5** (`Mnp5`, `mnp5.cpp`): run-length and adaptive frequency
  coding, MNP's compression (not in V.42), as the peers' code has it.
- **V.42bis** (`V42bis`, `v42bis.cpp`): 2048 codewords and strings of 34
  characters offered, lower when the remote asks; the encoder switches
  between transparent and compressed mode by the ratio over the last 256
  characters.
- **V.44** (`V44`, `v44.cpp`): 4096 codewords, strings of 255 and a
  history of 12288 octets offered (as the Agere modem does), lower when
  the remote asks. The caller offers V.42bis and V.44 in its XID; the
  answerer takes V.44 when both have it, and answers with what it took.
  See below.
- **Asynchronous characters** (`Async`): start bit, 8 data bits, stop bit,
  for the modulations without V.42 and when the remote has no LAPM.
- **V.80 Synchronous Access Mode** (`SyncAccess`, `v80.cpp`), instead of
  all the above when the DTE asks for it (`AT+ES=6`, `AT+ES=,,8`
  answering): the DTE's octets, with in-band commands after EM, as a
  synchronous bit stream, transparent or as HDLC frames. See below.

`Link` (`link.h`) puts them together. The modem core is its `Owner`: it
gives and takes the DTE's characters, hears of the link's progress
(`Status::Detected`, `Up`, `Released`, `Error`) and of what XID agreed, and
gets the log lines. The data pump calls `get_bits()` and `put_bits()`. The
link does not depend on the modem core or the data pumps. `stats()`
counts a call (for the core's `ATI11`): the DTE's characters each way,
the frames LAPM or MNP sent (by `Hdlc` and `MnpAsync`) and the information
frames among them sent again (LAPM's REJ or poll, MNP's retransmissions),
the frames received intact and those with a bad FCS.

The protocol code began as Smart Link's slmodemd link layer
(`modem_pack.c`, `modem_ec.c`, `modem_comp.c`) as classes. Its HDLC
framing, LAPM and V.42bis were written anew in 2026 from specifications
of what they do, with the same behaviour on the line; the interop tests
(`ctest -L "smartlink|agere|pctel|hsf|sm56|broadcom"`) run it against the
other modems. Faults of the old code are gone: a character could be lost
when V.42bis's pending output filled a frame exactly, a decoder flush
could overwrite decoded characters the DTE had not taken yet, and an idle
link in compressed mode sent a FLUSH and padding each time LAPM asked.

`link_test` (`ctest -R Link`, GoogleTest) runs it without a modem: V.42bis and
V.44 round trips (text, noise, escape characters, repeats, small
dictionaries, flushes), V.44's transparent mode, HDLC framing with a bit
error, and two Links back to back bit by bit: detection, XID, V.44 both
ways, V.42bis when either has no V.44, bit errors (REJ and T401 recovery),
a slow DTE (RNR), DISC, async, a LAPM answerer with an async caller,
frames LAPM must drop, an unsolicited DM (F = 0) while idle, which
asks for the link (8.8/V.42: an SABME, not the link up; ignored after a
release), and N400: five retransmissions of an unanswered XID, five
polls with the line cut, no more (`LapmN400`), and an originator
against a cellular answerer's ADPs, "EM" then "EC" (LAPM) or "EM" alone
(async after T400; `AdpCellular`). For MNP: our LR octet for octet as
the Agere modem's, FCS and all, three of them unanswered, then async
(`MnpLr`); our responder against the Agere modem's LR, then an LA, an LT,
an LN (LNA) and an LD as HDLC frames (`MnpResponder`), and against SYN
DLE STX with no LR after it (async after its wait, nothing sent:
`MnpResponderNoLr`); two Links with MNP
first, after ODPs a V.42 answerer did not answer, in framing mode 2 and
without the optimizations, with bit errors in both modes, a slow DTE (no
credit), MNP required against a modem without it, N400 with the line
cut; MNP 5's round trips (text, noise, runs, every value, flushes) and
the Agere modem's MNP 5 octets decoded and coded again (`Mnp5`). For
V.80 (`V80*`): transparent octets both ways with the first underrun
reported, all twenty shielding codes and DC1/DC3 with and without flow
control, frames with no FCS, FCS-16 and FCS-32 with flags or marks
between them, a bad FCS, the DTE's abort, underruns in a frame (an abort,
or a flag and `<EM><resume>`'s count), the commands for the core,
`<EM><bnum>` reports, and a receive overrun (`<EM><rover>`).

## V.44

V.44's compressed data is as the Agere modem's code has it (its V.44
encoder and decoder, read from the library, agree with each other and
with this): see `v44.h` for the tokens. The negotiation in XID is the
user data subfield (0xFF) after the groups: the parameter set "V44"
(0x40), C0 (0x41, 0: the stream method), P0 (0x42, the directions, as
V.42bis's), the codewords (0x43/0x44, transmit/receive), the longest
string (0x45/0x46) and the history (0x47/0x48); we take the smaller of
each pair and the remote's, the same both ways.

Both sides start in compressed mode, codewords from 4 in 6 bits and
ordinals of 7. Our encoder looks for the longest string of the
dictionary at the next characters (by their first two), extends it with
what followed it in the history, sends STEPUP when a codeword or
character needs it, REINIT when the dictionary or the history is full
(the Agere decoder takes no more history than that), and FLUSH with the
rest of the octet when the DTE has nothing more for now; it stays in
compressed mode. The decoder also takes transparent mode (ETM, escapes
as in V.42bis, ECM back), which the Agere encoder switches to on data
that does not compress.

Against the Agere modem (`ctest -R agere_v44`), 60000 octets each way of
text, binary octets, repeats and noise go through both ways at V.34 (the
Agere modem: `CONNECT 33600 V44`), its encoder going to transparent mode
and back now and then; the other Agere and interop calls use V.44 too
(V.42bis with the original Smart Link library, which has no V.44).

## MNP

MNP 2 to 4 is V.42's "alternative procedure" (Annex A of V.42 (10/96)),
the error correction of the modems from before LAPM, which V.42 (1988)
took in for them; V.42 (03/2002) deleted it, and its text is in the
superseded editions (`T-REC-V.42-199610-S` at itu.int, which
`docs/specs/fetch.sh` does not fetch). MNP 5 is MNP's compression, not
in V.42 at all; it is here as the peers' code has it. The other soft
modems of the interop tests (`docs/interop.md`) all have both: the
Agere, PCTel, Motorola SM56, Conexant HSF and Broadcom modems. Smart
Link's slmodemd, which this modem started from, had neither (its link
layer was LAPM only).

### When a call takes it

- The originator with V.42 (`\N2`, `\N3`, `+ES=3`) sends its LR when the
  answerer sends no ADP within T400 (7.2.1.2/V.42: "may attempt to
  detect the presence of an alternative error-correcting capability"),
  or when its LAPM goes unanswered; not after an ADP saying no error
  correction ("E" NUL). With MNP first or alone (`\N5`, `+ES=4`) it skips
  the detection phase and sends its LR after 64 characters of marks.
- The answerer takes an LR in the detection phase (SYN DLE STX among its
  characters) as the start of the protocol phase (7.2.1.3/V.42); MNP's
  framer takes the rest of the frame from there. With MNP allowed its
  detection phase lasts three times T400 (2.25 s): the SM56 modem's
  MNP sends its LR 1.1 s after the connection, and a V.42 originator its
  LR after its ODPs (an answerer with MNP alone does not answer them).
- With no LR answered at all (A.7.2.2) the call goes on without error
  correction, unless error correction is required (`\N2`, `\N4`, `\N5`,
  `+ES` with a required fallback): then the link fails (NO CARRIER).

### The establishment

Our LR is the peers' octet for octet: framing mode 3, k = 8, N401 = 64
and both data phase optimizations (the first, 256-octet information
fields, the second fixed-field LT and LA), and parameter 9 = 1 (MNP 5)
when we want MNP 5. The PCTel modem takes no LR with N401 256 (it drops
the call 2 s later); the others offer 64 with the 256-octet
optimization too (the HSF modem 256), and send and take LTs of 256
octets once both have it. The responder answers with what was agreed
(the lower mode, k and N401, the optimizations both have) and only the
parameters the initiator's LR had and it knows; SM56's parameter 14
(V.42bis's P0, P1, P2 in MNP: 3, 2048, 32) is passed over. The
establishment exchange goes in start-stop octets, the data phase in
HDLC frames (mode 3, LAPM's flags, zero insertion and FCS) or octets.
An LR is sent three times (T401: 2 s and the time of three LRs); the
responder whose LA is lost takes the initiator's first data phase frame
for it.

### The data phase

LT frames carry the DTE's data (or MNP 5's), numbered modulo 256, up to
k unacknowledged within the credit of the remote's last LA. We send an
LA for an out-of-sequence LT (the first V(R) - 1 again ignored), a bad
frame, k / 2 LTs unacknowledged, an LT when we have nothing to send,
T402 (half of T401) and T404 (3 s, or 7 s below 2400 bit/s). While the
DTE does not take an LT's data, the LA's credit is 0 and later LTs are
discarded, until an LA with credit again. An LA with the N(R) of the
last sends again from the first unacknowledged LT (the LAs the LTs sent
after a lost one bring are passed over), as T401 does (k frames of
N401 octets at the line's rate, and 1 s); twelve times (N400) without
progress, then LD. An LN gets its LNA (the break is not passed to the
DTE); we send none. We hang up with an LD (user-initiated, 255),
released once it is out. The HSF modem sends LTs with no information
now and then with MNP 5 (A.6.6.1 has none): they are taken in sequence.
At the end of a call the log has the LTs sent, sent again and received.

### MNP 5

MNP 5 (`mnp5.h`) is as PCTel's code has it (`MnpCompress`,
`MnpDecompress`, `MnpEncSort` in its `control.o`), and the Agere
modem's octets decode with it and are what our encoder sends for the
same text (`Link.Mnp5`):

- Run-length coding: after three equal characters a count of those after
  them (0 to 250; at 250 a new run starts).
- Adaptive frequency coding: the characters ranked by how often they
  came (a character moves up past those with fewer; all counts are
  halved when the top one reaches 255), each sent as its rank's code: a
  3-bit header h and h bits (1 for h = 0), ranks 0 and 1 `000x`, 2 and
  3 `001x`, 4 to 7 `010xx` up to 128 to 255 `111xxxxxxx`, rank 255 with
  a 0 after it. A count is coded as the rank of its value, outside the
  ranking.
- The codes fill each octet from its most significant bit; eleven 1s
  are the flush, the rest of its octet 1s. The encoder flushes when the
  DTE has nothing more (after characters since the last flush); the
  stream goes on from one LT to the next.

Text takes about two thirds, noise a little more (ranks of 128 and up
take 10 bits). `%C1` and `%C3` offer it (parameter 9 of the LR).

### With the peers

`ctest -R mnp`: each of the five calls with MNP alone (`+ES=4,4,6`) and
our default answerer takes its LR; we call with MNP alone (`\N5`) and it
answers so (`+ES=3,0,6`); 20000 octets each way of text, binary octets,
repeats and noise, with MNP 5 (and MNP 4 alone with the Agere modem).
All in framing mode 3, k = 8, 256-octet LTs, without a retransmission
on netsim's clean line.

| The peer | It calls (MNP alone) | It answers (MNP alone) |
|---|---|---|
| Agere | CONNECT 33600 MNP5 | CONNECT 33600 MNP5 |
| PCTel | CONNECT 115200 (MNP 5) | CONNECT 115200 (MNP 5) |
| SM56 | CONNECT 33600/MNP/MNP5 | CONNECT 33600/MNP/MNP5 |
| HSF | CONNECT 115200 (MNP 5) | CONNECT 115200 (MNP 5) |
| Broadcom | CONNECT 33600/MNP/MNP5 | CONNECT 33600/MNP/MNP5 |

With their defaults (`+ES=3,0,2`: V.42 with the detection phase, then
MNP) every call with us is V.42, as with any modem that has V.42; MNP
is for the modems that have only MNP (from before V.42, 1988 and
earlier) or are set so. Our CONNECT messages (`AT&A`) say `Protocol:
MNP` and `Compression: MNP5`.

### MNP 10 and 10EC

MNP 10 is Microcom's protocol for adverse lines: "MNP 10 protocol and MNP
Extended Services enhance performance under adverse channel conditions
such as those found in rural, long distance, or cellular environments.
An MNP 10 connection is established when an MNP 2-4 connection is
negotiated with a remote modem supporting MNP 10", and with MNP
Extended Services (`-K1`) a V.42 connection switches to MNP 10 when both
modems have it (Rockwell's RCV336ACF designer's guide, 1105, 2.6). MNP
10EC ("Enhanced Cellular", Rockwell's) goes further, at the physical
layer, "optimizing [the modulation's] responses to sudden changes in the
cellular connection" (fades, hand-offs), and at the protocol layer,
"dynamically adjusting speed and packet size based on signal quality and
data error performance" (2.7). `-SEC=1[,<tx level>]` turns it on (with a
transmit level of 0 to -30 dBm), and `)M` (power level adjustment), `@M`
(initial cellular power level), `*H` (link negotiation speed) and `-Q`
(fallback to V.22bis/V.22) go with it; a cellular modem's own guide
(Data Remote's CDS-9022, 2002) lists `)M`, `@M`, `*H` and `-Q` as
"included only for compatibility and [performing] no function". In the
detection phase a 10EC answerer sends "EM" five times or more and then
"EC" (Appendix VI/V.42: "a proprietary cellular phone modem protocol").

Supporting it is not feasible here:

- Neither MNP 10 nor 10EC was published; they were Microcom's and
  Rockwell's, licensed to modem makers, and 10EC's physical layer lives
  in Rockwell's own modulations.
- There is nothing to test against: none of the five soft modems above
  has a trace of MNP 10 or 10EC (no code, no strings, no `-SEC`
  command); they were in Rockwell and Conexant controller-based modems
  (RCV336ACF and such) and in cellular data devices.
- MNP 10 is negotiated as an MNP 2 to 4 connection (which D-Modem now
  has) with MNP 10's own parameters, unpublished.
- Its use was data over analogue (AMPS) cellular, which is gone (in the
  US since 2008). Digital cellular voice codecs do not carry modem
  signals, and circuit-switched data had the network's own modems
  (the interworking function). On D-Modem's lines (G.711 over SIP or a
  PBX) the impairments 10EC handles do not arise at our end.

What D-Modem does do with a 10EC modem is V.42: as the originator it
passes over the "EM" ADPs and takes the "EC" after them (LAPM,
`Link.AdpCellular`), and as the answerer it sends "EC" alone, which a
10EC originator takes as a V.42 answerer. The Rockwell cellular commands
(`-SEC`, `-K`, `-Q`, `)M`, `@M`, `*H`) are taken with no effect, so that
a setup string written for a cellular-ready modem (CDS-9022's
`AT&F;+MS=V32,1,4800,9600;-SEC=1,12;)M1;\N2`) ends in OK (the AT
reference, "Setup strings from other modems").

## V.80

V.80's Synchronous Access Mode (8.8/V.80) is for DTEs that do their own
framing over a modem: H.324 videophones (V.8's call function, H.223
frames), V.42 or other protocols run in the DTE. The modem gives up error
correction and compression and moves the DTE's octets as a synchronous
bit stream: no start and stop bits on the line, and in-band commands in
the DTE's stream after an EM (19h), both ways (Table 9/V.80).

`AT+ES=6` (calling) or `AT+ES=,,8` (answering) asks for it; the call
reports CONNECT once the data pump is up, as without error correction,
and then the mode starts transparent:

- **Transparent**: the DTE's octets go out least significant bit first;
  the line's bits come back as octets at whatever alignment they have.
  When the DTE has nothing, `+ESA`'s `<syn1>` goes out and the DTE hears
  `<EM><under>` (once, until it sends again). The DCE does not hunt for
  SYN octets (`<trans_idle>` 0, the mandatory value).
- **Framed** (from the DTE's first `<EM><flag>`): HDLC frames with zero
  insertion. The DTE ends each frame with `<EM><flag>`; the DCE adds the
  FCS if `+ESA`'s `<crc_type>` asks (1: FCS-16, 2: FCS-32, 8.1.1.6/V.42)
  and the flag. Between frames: flags or marks (`<framed_idle>`). A frame
  the DTE leaves unfinished ends in an abort (`<framed_un_ov>` 0) or a
  flag without FCS, the DTE's octets then dropped until `<EM><resume>`,
  which `<EM><unum>` answers with their count (1); both with
  `<EM><under>`. `<EM><err>` from the DTE sends an abort. The receiver
  hunts for a flag (`<EM><err>` when it finds one), then gives each
  frame's octets and `<EM><flag>` (whole octets, a good FCS) or
  `<EM><err>`; when the DCE checks the FCS it keeps it back and drops it
  (the DTE sent none either). Seven 1s are `<EM><mark>` and a new hunt.
  `<EM><hunt>` starts one; `<EM><mark>` goes back to transparent.

Towards the DTE, EM and 99h are always shielded (`<EM><t1>`, `<t2>`), DC1
and DC3 too when it has DC1/DC3 flow control from the modem (`+IFC=,1`).
From the DTE all the codes T1 to T20 are taken, and bare DC1 and DC3 are
dropped when it flows the modem off with them (`+IFC=1`). What the DTE
does not take waits in a ring of 2048 octets; past that the received
octets are lost and `<EM><rover>` marks the place.

The core (`core/modem.cpp`, the link's owner) does the rest: `<EM><ecs>`
(the escape: `+++` is data here) answers `<EM><ecs>` and OK in online
command state, and `ATO` goes back; `<EM><eot>` answers `<EM><eot>` and
hangs up (NO CARRIER); the remote's carrier gone is `<EM><eot>` before NO
CARRIER; `<EM><rate>` sets `+MS`'s highest rate from its `<tx>` code and the
highest to receive at (`MDMPRM_MAX_RX_RATE`) from its `<rx>`, for the
next retrain or renegotiation; `<EM><rtn>` and `<EM><rrn>` ask the pump
for a retrain or a renegotiation (`Modem::link_req()`, as `ATO1` and
`ATO3`; V.34, V.90 and V.92 only). To the DTE: `<EM><rtn>` and
`<EM><rrn>` for each retrain and renegotiation the pump counts in
`dp_stats_` but the DTE's own (the remote's and the pump's), each
followed by `<EM><rate>`, sent as well when the pump's rates change in
the call (Table 10's codes stop at 33600, faster rates are 2Eh).
`+ITF`'s `<off>` limits what the DTE's buffer takes (`dte_space()`),
and its `<report_period>` sends `<EM><bnum>` with what waits in the buffer
(EM codes counted as octets) every so many 10 ms.

Not there: the transparent receiver's hunt for SYN (`<trans_idle>` 1, 2),
NRZI, V.34 half duplex (`<hd_auto>`, `<EM><pri>`, `<ctl>`...), and
staying off hook after `<EM><eot>`. +ES's 5 and 7 (Synchronous Mode, Frame Tunnelling
Mode) are not taken.

Against the other modems (`ctest -R _v80_`, [interop.md](interop.md#v80)):
the Agere and Broadcom modems (one V.80 code) and the SM56 modem take
our frames whole, with no FCS and with FCS-16, and we take theirs; the
Intel modem takes ours, but its own end in 16 1s. Agere and Broadcom drop
the FCS as we do (with a stray EM when an FCS octet needed shielding);
the SM56 modem hands it to its DTE.
