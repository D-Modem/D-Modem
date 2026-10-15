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

`Link` (`link.h`) puts them together. The modem core is its `Owner`: it
gives and takes the DTE's characters, hears of the link's progress
(`Status::Detected`, `Up`, `Released`, `Error`) and of what XID agreed, and
gets the log lines. The data pump calls `get_bits()` and `put_bits()`. The
link does not depend on the modem core or the data pumps.

The protocol code is Smart Link's slmodemd link layer (`modem_pack.c`,
`modem_ec.c`, `modem_comp.c`) as classes, with the same behaviour on the
line; the interop tests (`ctest -L "interop|agere"`) run it against the
original library and the Agere modem. Two faults of the old glue between
V.42bis and the DTE are gone: a character could be lost when the encoder's
pending output filled a frame exactly, and a decoder flush could overwrite
decoded characters the DTE had not taken yet.

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
(async after T400; `AdpCellular`).

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

D-Modem has no MNP class: neither MNP 2 to 4's error correction (V.42's
"alternative procedure"), nor MNP 5's compression, nor MNP 10 or 10EC.
Its error correction is V.42 LAPM, its compression V.42bis and V.44. Of
the MNP commands of the Smart Link and Rockwell command sets, `\A0` to
`\A3` (the MNP block size) are taken and change nothing, and `\N1` to
`\N5` all mean V.42 when the remote has it (Rockwell's `\N2` is a
reliable link, MNP or V.42, its `\N5` MNP only); `%C` and `&K` choose
between V.42bis and V.44 (Rockwell's `%C1` is MNP 5). Smart Link's
slmodemd, which this modem started from, had no MNP either: its link
layer was LAPM only, and its DSP library (`dsp/test/smartlink/dsplibs.o`)
has no MNP in it.

### MNP modems calling and answering

The other soft modems of the interop tests (`docs/interop.md`) all have
MNP: the Agere, PCTel, Motorola SM56, Conexant HSF and Broadcom modems'
code has MNP 2 to 4 (the frames of V.42 (10/96) Annex A: LR, LA, LT, LN,
LNA, LD) and MNP 5. With their defaults (V.250's `+ES=3,0,2`: V.42 with
the detection phase, then MNP, then none) every call with us is V.42,
and that is what MNP modems in general do: V.42 (1988) put LAPM first
and took MNP 4 in as its Annex A, for the modems that had only that. Forced to MNP with `+ES` (`interop_call` by hand, the
peer calling with `AT+MS=V34`, us answering with `AT+MS=34`):

| The peer | Result |
|---|---|
| calls with MNP only (`+ES=4,4,6`): Agere, PCTel, SM56, HSF, Broadcom | NO CARRIER at the peer. It sends LR frames (start-stop framing: SYN DLE STX, the header, DLE ETX, the FCS); our answerer sees no ODP, goes to async after T400, and the LRs reach its DTE as data; the peer gives up |
| calls with MNP first, then none (`+ES=4,0,2`): Agere | CONNECT, no error correction either side; two of its LRs reach our DTE ahead of the data |
| answers with MNP only (`+ES=3,0,6`): Agere | CONNECT 33600 V44: it answers our ODPs with V.42 all the same |

Their LRs (A.6.4.1/V.42 (10/96)) offer bit-oriented framing (mode 3), 8
outstanding LT frames, data phase optimization 3 (256-octet information
fields and fixed-field LT and LA, that is MNP 4) and parameter 9, MNP 5
compression (not in V.42): the Agere, PCTel and Broadcom modems with
information fields of 64 octets, HSF of 256 (and parameter 9 = 0x1d),
SM56's with a parameter 14 more (03 08 00 20) that looks like V.42bis's
P0, P1 and P2 (3, 2048, 32).

So an MNP modem that has V.42 (all of these) connects with us with
LAPM; one with MNP only (modems from before V.42, 1988 and earlier, or
one set so) connects without error correction or not at all. V.42
(03/2002) deleted Annex A; its text, the published part of MNP, is in
the superseded editions (V.42 (10/96), `T-REC-V.42-199610-S` at
itu.int), which `docs/specs/fetch.sh` does not fetch.

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
- MNP 10 is negotiated as an MNP 2 to 4 connection, which D-Modem has
  not got.
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

What could be done:

- MNP 2 to 4 (Annex A of V.42 (10/96)) and MNP 5, for modems that have
  MNP and not V.42; the five peers above would test it both ways. With
  it the answerer would also take an LR in the detection phase as the
  start of the protocol phase (7.2.1.3/V.42) instead of passing it to
  its DTE.

