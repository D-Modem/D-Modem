# V.cvm: data over mobile speech paths

`AT+MS=VCVM` runs the DCE of the proposed Recommendation in
[docs/cellular](../cellular/v_cvm.tex) ([v_cvm.pdf](../v_cvm.pdf)): data
through the speech codecs of mobile calls (AMR, AMR-WB, EVS), where V.34
and V.32 do not pass. The Recommendation has the reasons and the
measurements; this page is the implementation.

## A call

1. **V.8** (`dp_v34.cpp`). With `+MS=VCVM` the V.34 pump offers, in CM or
   JM, V.22 and V.21 as the fallback (not V.34, V.32 or V.23, which a codec
   does not carry), LAPM, and V.cvm's information field in the
   non-standard facilities category (Annex A). The answering DCE puts the
   field in JM only if CM had it. When both have it the pump hands the
   call to `DP_VCVM` (150) 75 ms after CJ. Through AMR 7.4 the answering
   DCE can miss CJ, so when V.cvm is agreed it also takes 30 ms of quiet
   after CM for CJ (the caller's INFO0 follows CJ by 175 ms). Without the
   field on both sides V.8 picks V.22 or V.21 as usual.
2. **The start-up** (`vcvm_startup.cpp`, `vcvm_msg.cpp`,
   `vcvm_probe.cpp`). Phase 2: the calling DCE's INFO0, L1, L2 and L3,
   then the answering DCE's INFO0, L1, L2, L3 and INFO1, then the calling
   DCE's INFO1. Phase 3: the MP exchange. The messages go in P20 (the
   parametric mode) when both DCEs have it, else in V.21. This takes 15 to
   21 s.
3. **Phase 4** (`vcvm_data.cpp`, `vcvm_modem.cpp`). The octets go through
   the scrambler, then RS(32,k) (`rs.cpp`), then the interleaver, onto
   the loaded tones or the lags. CONNECT comes at the coded rate with
   `/CVM` once data runs both ways. LAPM starts without V.42's detection
   phase. XID agrees on compression as on any other call: zstd where
   both offer it (Annex F; both INFO0s must indicate it), from its
   dictionary where both indicate the preset, else V.44, from the
   preset dictionary (below) where both indicate it.
4. **Renegotiation and retrain.** A renegotiation is the control sequence,
   then the MP exchange with new loadings. It is triggered by request, by
   a loaded tone's phase error, or by failing codewords. A retrain is the
   control sequence, then MPs with the retrain bit set, then phase 2
   again. It is triggered by request, by the peer's data lost for 2 s, or
   by the parametric mode's timers and c-bar. If the peer's data has not
   begun decoding 6 s into phase 4 (or after our MP went again), a
   renegotiation; if that brings no data either, a retrain. Until the
   peer's data decodes, HDLC flags go out in place of LAPM's bits: its
   frames would go unanswered, and T401 (which counts the bits sent)
   would run out on polls whose answers cannot be heard. The start-up's
   and renegotiations' MPs go on while the peer's keep coming (ten more
   of ours after each of the peer's, 30 at most), as through AMR-WB the
   last of an exchange is often lost both ways.

| Line (interop test) | Mode | CONNECT (coded bit/s) |
|---|---|---|
| G.711 (`open_vcvm`) | N200 comb | 3500 |
| AMR 12.2 (`open_vcvm_amr122`) | N200 comb | about 1200 |
| AMR-WB 23.85, 8 kHz interface (`open_vcvm_amrwb2385`) | N200 comb | 1600 to 1760 |
| AMR 7.4 (`open_vcvm_amr74`) | parametric, P10 or P20 | 175 to 350 |

At AMR 7.4 the first loading can be P10 (400 bit/s raw) in one direction.
The receiver's failing codewords then renegotiate it down to P20 within
the first minute.

## Compression: V.44 with a preset dictionary

V.44 starts with an empty dictionary and learns from the data. At V.cvm's
rates (175 to 3500 bit/s) a call's data is often a few short messages,
over before the dictionary has learnt much. So both DCEs can start V.44
from the same text, the Recommendation's Annex E
([annexes.tex](../cellular/annexes.tex), [v_cvm.pdf](../v_cvm.pdf)): 113 lines, 3505 octets, of
HTTP, mail, HTML, JSON and XML, NMEA sentences, logins and common English,
7-bit characters only (so that V.44's ordinals stay 7 bits).

- **Indication.** INFO0's octet 3 bit 4, in phase 2 (not V.8's field: see
  below). A DCE whose peer does not set it runs V.44 as it is.
  `DSP_VCVM_V44DICT=0` leaves it out (a test).
- **Use.** Both indicate it and XID agrees on V.44: the link
  (`Link::update_config()`) primes its V.44 encoder and decoder before
  any data (`V44::prime()`): the text through an encoder of the agreed
  parameters, its output through the decoder. That state is where both
  ends start, and where a REINIT (the dictionary or history full) goes
  back to, in place of empty ones. With small parameters only as much of
  the text as the dictionary and half the history hold is taken (both
  ends work it out alike). The log says `v44: preset dictionary 1, 3505
  of its 3505 characters`.
- **The gain.** Six typical short messages (an HTTP request and
  response, a mail, JSON, NMEA, a short letter; 1264 octets) come to 958
  octets with V.44 alone and 485 primed, each as a call's first; 826 and
  449 in a row (`Link.V44Preset`). The messages are of the kinds the
  text was written for, so data of other kinds gains less. Long
  transfers gain little: the dictionary learns their own strings soon
  enough.
- **Cost.** Nothing on the line; 3.5 kB of the 12 kB history at each end
  (V.44's REINIT comes that much sooner).

`link/v44_preset.cpp` has the text, and the `v44_preset_check` test
fails if it and the annex's listing ever differ.

## Compression: zstd (Annex F)

Between two D-Modems (V.cvm or any other mode with LAPM) the call's
compression is zstd rather than V.44: XID offers it in a private set,
and both take it when both offer it ([link.md](../link.md#zstd)). On
V.cvm INFO0's octet 3 bit 3 indicates it ahead (`DSP_VCVM_ZSTD=0` leaves
it out, a test): without it in both INFO0s neither offers it, and V.44
runs as above. The dictionary is a formatted zstd dictionary whose
content is the same Annex E text, offered where both INFO0s have the
preset bit (`DSP_VCVM_V44DICT=0`: zstd without it). The log says
`zstd: window 32768, dictionary 56434d01 (3660 octets)`. The same six
messages come to 474 octets with it, each on its own (V.44 primed: 485),
414 in a row (449); bulk text about three times smaller than V.44's.

## Capabilities: where they go, and K56flex

- **V.8's field stays two octets** (Annex A). CM and JM go through the
  codec in V.21, which loses bits at the low modes (1 to 3 per cent
  through AMR 5.15 to 6.7); a field of three octets made CM and JM two
  octets longer, and through AMR 7.4 the calling DCE then missed every JM
  and fell back. The parsers take a longer field and use its first two,
  so that a later version can still add octets.
- **Octet 1 bit 1 is always 0.** A K56flex client (sm56, HSF) takes a
  V.8bis non-standard block as K56flex's when its country is 0xB5, its
  provider code two octets long, o[6] bit 0 set, (o[7] & 0x2f) >= 2
  and o[7] bit 0x40 ([k56flex.md](k56flex.md)); it does not look at the
  provider code. In V.cvm's block (country 0xB5, the placeholder provider
  00 00) o[6] and o[7] are the field's two octets, and octet 2 always has
  0x40 and nearly always the 0x2f bits. Octet 1 bit 1 clear is all that
  keeps a V.cvm DCE from looking like a K56flex server should the field
  go into V.8bis (Annex A.3). `VcvmCaps.NotK56flex` checks every
  combination of capabilities against that rule.
- **Link-layer capabilities go in INFO0's octet 3** (Table B.1): bit 4
  the V.44 preset dictionary (and zstd's dictionary), bit 3 zstd
  (Annex F), bit 2 reserved, bit 1 reserved for an octet of further
  capabilities after octet 4. INFO0 has its FCS and comes again until
  received, and both INFO0s are in well before the link starts.

## Files

| File | What it is |
|---|---|
| `dsp/vcvm.h`, `vcvm.cpp` | The signal set: the combs (DPSK, amplitude rings, cyclic prefix, timing) and the parametric mode (pulse trains, lag detection, the preamble search) |
| `dsp/vcvm_startup.h`, `vcvm_msg.cpp` | The capabilities (V.8's NS field), INFO0, the loading (INFO1, MP), the messages' HDLC frames, the P20 and V.21 message transmitter and receivers |
| `dsp/vcvm_probe.cpp` | L1 to L3 and their measurement; the choice of the loading |
| `dsp/vcvm_startup.cpp` | Phases 2 and 3, sample by sample |
| `dsp/rs.h`, `rs.cpp` | The Reed-Solomon code (G.975's field and generator), errors and erasures |
| `dsp/vcvm_data.h`, `vcvm_data.cpp` | Phase 4: transmitter and receiver, the control sequence, loss of signal |
| `dsp/vcvm_modem.h`, `vcvm_modem.cpp` | One DCE after V.8: the start-up, phase 4, renegotiations, retrains |
| `dsp/dp_vcvm.cpp` | The pump `DP_VCVM`: the core's bits as octets (first bit the least significant), 9600 Hz lines through resamplers, CONNECT, the figures for `+CVM?`, S44 and S45, whether both have the preset dictionary |
| `link/v44_preset.h`, `v44_preset.cpp` | Annex E's text, V.44's preset dictionary (`V44::prime()` in `link/v44.cpp`) |
| `link/zstd_codec.h`, `zstd_codec.cpp`, `zstd_dict.h`, `zstd_dict.cpp` | zstd (Annex F) and its dictionary |

## Receivers

- **V.21 messages.** Two filters matched over the bit (the detector of the
  Recommendation's measurements) and its own bit clock. The clock gain is 0.5 over the first 12
  transitions, then 0.1 within a window of ±0.3 bit. A message ends at
  30 ms of quiet (10 ms windows under −40 dBFS).
- **P20 messages.** A frame whose FCS fails is retried with the second
  most likely lag on the 5 least reliable symbols, one at a time and in
  pairs. A lost INFO1 is recovered from the peer's MP, which has its
  layout.
- **L1's first symbol.** The cyclic prefix gives the symbol timing to a
  symbol; which symbol is the first is taken from the signal's onset.
  Through AMR-WB the codec opens up slowly and the onset comes as much as
  half a symbol late, so a few samples either way meant a symbol either
  way, and every symbol of the probing then read against the wrong
  known step (70 to 80 per cent in error, no data mode asked for, the
  start-up failed). The receiver now tries the symbol before, the one
  found and the one after, and keeps the one whose first 8 symbols' steps
  are nearest the known ones (`VcvmProbeRx::resolve_start()`).
- **L3.** The P10 part is locked to where the P20 part ends. Its own
  preamble is found poorly through AMR 10.2 and below.
- **RS decoding** uses GMD: errors alone first, then with the 2, 4, … least
  reliable octets erased, up to n−k−2.
- **Phase 4.** Control sequences count only after a block of the peer's
  data has decoded, and only at the level of data (with c > 0.5 in the
  parametric mode). Without this, an MP's zero octets and silence both
  read as control sequences. The signal counts as lost when it is quiet,
  or when nothing decodes for max(2 s, 2.5 blocks).
- **Phase 4's start.** The peer's data begins at the end of its one more
  MP after our acknowledgement. Through AMR-WB that MP is lost now and
  then; a transmission without a good frame, while the peer's data does
  not decode yet, is taken for it (before, the receiver stayed aligned
  to the MP before it and never decoded, and the call ended for want of
  a connection).

## Where it departs from the Recommendation's first draft

These are written into the Recommendation now:

- the order of phase 2 (above): the calling DCE's INFO1 cannot carry
  measurements before it has received anything;
- INFO0 octet 3 bit 5: the amplitude rings allowed (so that `+CVM` can
  refuse them);
- L3's P10 part starts where the P20 part ends;
- the loading picks the comb only when it carries more after the code
  than the parametric mode would; P20 is taken with up to 1 symbol of 40
  in error, at rate 1/2;
- one more MP after the peer's acknowledgement;
- the start-up takes 15 to 21 s, not 16;
- V.21's measured bit error ratio is 1 to 3·10⁻² at AMR 5.15 to 6.7, so
  V.21 messages are reliable only from AMR 7.4 up;
- the scrambler works on the information octets before the code (it was
  after the interleaver): the descrambler triples each line error;
- phase 4's comb sends its phase reference three times;
- control sequences are gated as above. The first MP in phase 4 is the
  peer's expected extra one, and the receiver realigns to its end. Only a
  second one, meaning the peer is repeating, is answered;
- the retrain bit is MP octet 2 bit 8; after a retrain the calling DCE's
  INFO0 comes 3 s on;
- c-bar's retrain rule has hysteresis (0.83/0.87) and does nothing in
  the first 30 s;
- V.8: the field's coding in the non-standard facilities category, and
  the end of CM taken for CJ.

## Not implemented

- The signal interface is 8000 Hz only. `+CVM`'s `<rate>` of 16000 is
  accepted and kept, but the wideband modes are not offered.
- `+CVMR` (`+CVM`'s `<report>`) is accepted and kept, but no unsolicited
  result is sent.
- After a failed start-up the call ends (NO CARRIER). It does not restart
  V.8 for V.22.
- No quick start (INFO0 octet 3 bits 7 and 6), no facsimile (Annex D), no
  V.8bis.
- EVS is not in the interop tests: netsim's `NET_CODEC` has AMR and
  AMR-WB.

## Known issues

- Through AMR 7.4, after some 200 s of data at 175 bit/s, one run ended
  with V.44 reporting a decoder error: a corrupted frame got past LAPM's
  16-bit FCS. The Reed-Solomon decoder miscorrects now and then, and LAPM
  has no 32-bit FCS yet. `open_vcvm_amr74` sends one line each way.
- P20 messages through AMR-WB 23.85 at an 8000 Hz interface are lost
  often (c is about 0.8). The MP exchange recovers, but the start-up
  takes longer. `open_vcvm_amrwb2385` failed in two ways that a small change
  of timing (V.8's octets, the load) brought about: L1's first symbol
  one off, and phase 4's start lost with the peer's last MP. Both are
  fixed (Receivers, above). Then, as a pass rate of 7 in 10 alone and 1
  in 6 under load: an MP lost from a few positions in the codec's frame,
  and repeats a whole number of frames apart kept landing there, so a
  renegotiation never finished (each MP in phase 4 and its renegotiations
  now goes 6.25 ms later than the last, to the fourth); an MP of the
  peer's already on its way when ours went again taken for a second one
  and answered, our data broken off again (now let by); a second lost MP
  of the peer's left the data receiver at the wrong place (now restarted
  after each); and after a retrain the peer's last MP, still coming in,
  read as its INFO0 (an MP's first octet passes for INFO0's; now passed
  over). With those, 23 of 24 alone and 4 of 6 under load. What is left:
  the start-up's own MPs, a whole number of frames apart, can all be lost
  for one call's timing; giving them the same 6.25 ms steps moves the
  last MP's measured end, which the parametric data receiver (P10's 10 ms
  symbols) takes as phase 4's start, so it needs that receiver to find
  its own symbol timing first. The test stays among the flaky ones.
- Bulk transfers through AMR (`dsp/test/compress_calls.sh`, 32 kB each
  way) stalled or dropped: after a renegotiation the peer's data never
  decoded and LAPM gave up its polls (now the 6 s watch and the flags
  above); the start-up's last MPs were lost and one side gave up just
  before the other's came; T401's 2 s ran out before an answer could come
  at 1100 to 2300 bit/s, so the polls and go-backs went on through the
  data (T401 now has three frames' time added, docs/link.md); and a frame
  of noise passing the 16-bit FCS acknowledged I frames never received,
  which were lost (such an N(R) is now ignored). With those, AMR 12.2 and
  AMR-WB, with zstd or none, went through 12 of 12 at six calls at once.
  At twelve at once an AMR-WB call without compression now and then stops
  one way near its end, with neither side logging an error.

## Tests

- `vcvm_test`: the signal set, through the codecs.
- `vcvm_startup_test`: capabilities, messages, probing and loading, the
  start-up through G.711, AMR 12.2, AMR 7.4, AMR-WB and impaired lines.
- `vcvm_data_test`: phase 4, and `VcvmModem` through a clean line and AMR,
  with renegotiation, retrain and a cut line.
- `link_test`'s `Link.V44Preset`: V.44 primed with the preset dictionary,
  round trips with REINITs, and the gain on short messages;
  `v44_preset_check`: the text is the annex's (and zstd's dictionary
  header Annex F's). `Link.Zstd`, `Link.ZstdDict`, `Link.LapmZstd`:
  zstd's round trips, its dictionary, its negotiation; `zstd_ref_test`:
  against libzstd, both ways.
- `rs_test`: the code.
- The `open.open_vcvm*` interop calls (table above): two open modems
  through netsim, V.8 and LAPM included; `open_vcvm` with zstd and its
  dictionary, `open_vcvm_nodict` without the preset on offer (zstd
  without a dictionary), `open_vcvm_nozstd` without zstd (V.44 primed);
  `open_34_zstd` and `open_34_nozstd` zstd over V.34.
