# V.cvm: data over mobile speech paths: technical details

This page gives the technical details for [V.cvm: data over mobile phone calls](vcvm.md), the plain-English overview. Read the overview first. This page keeps the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

`AT+MS=VCVM` runs the DCE (the modem) of the proposed Recommendation in
[docs/cellular](../cellular/v_cvm.tex) ([v_cvm.pdf](../v_cvm.pdf)). It
carries data through the speech codecs of mobile calls (AMR, AMR-WB,
EVS), where V.34 and V.32 cannot go through. The Recommendation gives
the reasons and the measurements. This page describes the
implementation.

Terms that this page uses:

- **Comb**: a set of tones sent together. Each tone carries data by DPSK
  (and, where permitted, amplitude rings). N200 and W200 are comb codes.
- **Parametric mode** (P10, P20): data carried as a pulse train. The period
  of the pulse train, one of 16 lags, is the symbol. An ACELP codec keeps that period
  as the lag of its long-term predictor. P20 has 20 ms symbols, and P10 has 10 ms symbols.
- **Loading**: the number of bits that each tone or lag carries, selected from the
  probing.
- **INFO0, INFO1, MP**: the messages of the start-up (capabilities, then the
  loading that each side asks for).
- **c, c-bar**: c is the normalized autocorrelation on which the
  parametric receiver decides a symbol. c-bar is its mean over 2 s, which
  the retrain rules use (clause 9.5).

## A call

### 1. V.8

V.8 runs in `dp_v34.cpp`.

- With `+MS=VCVM`, the V.34 pump offers these items in CM or JM:
  - V.22 and V.21 as the fallback (not V.34, V.32 or V.23, which a codec
    does not carry);
  - LAPM;
  - the information field of V.cvm, in the non-standard facilities category
    (Annex A).
- The answering DCE puts the field in JM only if CM had it.
- When the two sides have it, the pump gives the call to `DP_VCVM` (150)
  75 ms after CJ.
- Through AMR 7.4, the answering DCE can lose CJ. Thus, when the DCEs agree on V.cvm,
  the answering DCE also accepts 30 ms of quiet after CM as CJ. (The
  INFO0 of the caller follows CJ by only 175 ms.)
- If the two sides do not both have the field, V.8 selects V.22 or V.21 as usual.

### 2. The start-up

The start-up is in `vcvm_startup.cpp`, `vcvm_msg.cpp` and
`vcvm_probe.cpp`.

- **Phase 2**: the INFO0, L1, L2 and L3 of the calling DCE; then the INFO0, L1, L2, L3 and INFO1
  of the answering DCE; then the INFO1 of the calling DCE.
- **Phase 3**: the MP exchange.

The messages go in P20 (the parametric mode) when the two DCEs have it.
If not, they go in V.21. The start-up takes 15 to 21 s.

### 3. Phase 4 (data)

Phase 4 is in `vcvm_data.cpp` and `vcvm_modem.cpp`.

1. The octets go through the scrambler,
2. then RS(32,k) (`rs.cpp`; k is 16, 24 or 28 for code rate 1/2, 3/4 or
   7/8),
3. then the interleaver,
4. and onto the loaded tones or lags.

`CONNECT` comes at the coded rate, with `/CVM`, after data runs in the two
directions. LAPM starts without the detection phase of V.42.

XID agrees on compression as on all other calls:

- zstd where the two sides offer it (Annex F; the two INFO0s must indicate it),
  from its dictionary where the two sides indicate the preset;
- if not, V.44, from the preset dictionary (refer to the subsequent section) where the two sides indicate
  it.

### 4. Renegotiation and retrain

A **renegotiation** is the control sequence, then the MP exchange with
new loadings. These items start it:

- a request;
- a phase error of a loaded tone above the threshold of its order;
- codewords that fail.

A **retrain** is the control sequence, then MPs with the retrain bit set,
then phase 2 again. These items start it:

- a request;
- no decoded codeword of the data of the peer for 2 s (or the signal lost);
- in the parametric mode (or with no data mode): a retrain 30 s into
  phase 4 and each 60 s after that. There is no retrain while c-bar is less than 0.85, and one
  occurs immediately when c-bar increases to it. The threshold has hysteresis (below
  0.83 counts as low, 0.87 as increased). In the first 30 s of phase 4,
  the increase has no effect.

**The 6 s watch.** If the data of the peer does not start to decode 6 s into
phase 4 (or 6 s after our MP went again), the DCE renegotiates. If that
also brings no data, it retrains.

**Flags until the peer decodes.** Until the data of the peer decodes, HDLC
flags go out instead of the bits of LAPM. If not, the frames of LAPM would get
no answer, and T401 (which counts the sent bits) would expire on polls
whose answers cannot be heard.

**Repeated MPs.** The MPs of the start-up and of renegotiations continue while the
MPs of the peer continue to come: ten more of our MPs after each MP of the peer, and 30 at
most. Through AMR-WB, the last MP of an exchange is frequently lost in the two directions.

### Rates measured

| Line (interop test) | Mode | CONNECT (coded bit/s) |
|---|---|---|
| G.711 (`open_vcvm`) | N200 comb | 3500 |
| AMR 12.2 (`open_vcvm_amr122`) | N200 comb | about 1200 |
| AMR-WB 23.85, 8 kHz interface (`open_vcvm_amrwb2385`) | N200 comb | 1600 to 1760 |
| AMR 7.4 (`open_vcvm_amr74`) | parametric, P10 or P20 | 175 to 350 |

At AMR 7.4, the first loading can be P10 (400 bit/s raw) in one
direction. Then the codewords that fail at the receiver renegotiate it down to
P20 in the first minute.

## Compression: V.44 with a preset dictionary

V.44 starts with an empty dictionary and learns from the data. At the
rates of V.cvm (175 to 3500 bit/s), the data of a call is frequently some short
messages. These messages are complete before the dictionary has learnt much.

Thus, the two DCEs can start V.44 from the same text: Annex E of the Recommendation
([annexes.tex](../cellular/annexes.tex), [v_cvm.pdf](../v_cvm.pdf)).
It has 113 lines, 3505 octets, of HTTP, mail, HTML, JSON and XML, NMEA
sentences, logins and common English. It uses only 7-bit characters. Thus,
the ordinals of V.44 stay 7 bits.

- **Indication.** Octet 3 bit 4 of INFO0, sent in phase 2 (not in the field
  of V.8; refer to the subsequent sections). If the peer of a DCE does not set it, the DCE runs V.44 as
  usual. `DSP_VCVM_V44DICT=0` does not send the bit (for tests).
- **Use.** When the two sides indicate it and XID agrees on V.44, the link
  (`Link::update_config()`) primes its V.44 encoder and decoder before
  any data (`V44::prime()`). The text goes through an encoder with the
  agreed parameters, and its output goes through the decoder.
  - The two ends start from that state. A REINIT (the
    dictionary or the history full) also goes back to that state, instead of empty
    tables.
  - With small parameters, the link takes only as much of the text as the
    dictionary and half the history can hold. The two ends calculate this in the
    same method.
  - The log shows `v44: preset dictionary 1, 3505 of its 3505 characters`.
- **The gain.** Six typical short messages (an HTTP request and
  response, a mail, JSON, NMEA, a short letter; 1264 octets in total):
  - each as the first message of a call: 958 octets with only V.44, 485
    primed;
  - in sequence: 826 and 449 (`Link.V44Preset`).

  The messages are of the types for which we wrote the text. Thus, data of
  other types gains less. Long transfers gain little, because the
  dictionary quickly learns their own strings.
- **Cost.** Nothing on the line. At each end, 3.5 kB of the 12 kB
  history. Thus, the REINIT of V.44 comes that much sooner.

`link/v44_preset.cpp` has the text. The `v44_preset_check` test fails if
the text and the listing of the annex are different.

## Compression: zstd (Annex F)

Between two D-Modems (V.cvm, or any other mode with LAPM), the
compression of the call is zstd and not V.44. XID offers it in a private
parameter set, and the two sides use it when the two sides offer it (refer to
[link-details.md](../link-details.md#zstd)).

On V.cvm, octet 3 bit 3 of INFO0 indicates it in advance
(`DSP_VCVM_ZSTD=0` does not send it, for tests). If it is not in the two INFO0s,
neither side offers zstd, and V.44 runs as described above.

The dictionary is a formatted zstd dictionary whose content is the same
Annex E text. The DCE offers it where the two INFO0s have the preset bit
(`DSP_VCVM_V44DICT=0` gives zstd without it). The log shows
`zstd: window 32768, dictionary 56434d01 (3660 octets)`.

With it, the same six messages become 474 octets, each alone (V.44
primed: 485), and 414 in sequence (V.44 primed: 449). Bulk text becomes
about three times smaller than with V.44.

## Capabilities: where they go, and K56flex

- **The field of V.8 stays two octets** (Annex A). CM and JM go through the
  codec in V.21, which loses bits at the low codec modes (1 to 3 per cent
  through AMR 5.15 to 6.7). A field of three octets made CM and JM two
  octets longer. Then, through AMR 7.4, the calling DCE lost all
  JMs and fell back. The parsers accept a longer field and use its first
  two octets. Thus, a later version can still add octets.
- **Octet 1 bit 1 is always 0.** A K56flex client (sm56, HSF) thinks that a
  V.8bis non-standard block is the block of K56flex when all of these conditions are true: its country is 0xB5, its
  provider code is two octets long, o[6] bit 0 is set,
  (o[7] & 0x2f) >= 2, and o[7] bit 0x40 is set (refer to
  [k56flex.md](k56flex.md)). It does not look at the provider code.
  - In the block of V.cvm (country 0xB5, the placeholder provider 00 00), o[6]
    and o[7] are the two octets of the field. Octet 2 always has 0x40 and
    almost always the 0x2f bits.
  - Thus, only the clear octet 1 bit 1 prevents a V.cvm DCE from
    looking like a K56flex server, if the field goes into V.8bis
    (Annex A.3).
  - `VcvmCaps.NotK56flex` checks all combinations of capabilities
    against that rule.
- **Link-layer capabilities go in octet 3 of INFO0** (Table B.1):
  - bit 5: the amplitude rings permitted;
  - bit 4: the V.44 preset dictionary (and the dictionary of zstd);
  - bit 3: zstd (Annex F);
  - bit 2: reserved;
  - bit 1: reserved for an octet of more capabilities after octet 4.

  INFO0 has its own FCS, and it is repeated until it is received. The two INFO0s
  arrive a long time before the link starts.

## Files

| File | What it is |
|---|---|
| `dsp/vcvm.h`, `vcvm.cpp` | The signal set: the combs (DPSK, amplitude rings, cyclic prefix, timing) and the parametric mode (pulse trains, lag detection, the preamble search) |
| `dsp/vcvm_startup.h`, `vcvm_msg.cpp` | The capabilities (the NS field of V.8), INFO0, the loading (INFO1, MP), the HDLC frames of the messages, and the P20 and V.21 message transmitter and receivers |
| `dsp/vcvm_probe.cpp` | L1 to L3 and their measurement; the selection of the loading |
| `dsp/vcvm_startup.cpp` | Phases 2 and 3, sample by sample |
| `dsp/rs.h`, `rs.cpp` | The Reed-Solomon code (the field and generator of G.975), with errors and erasures |
| `dsp/vcvm_data.h`, `vcvm_data.cpp` | Phase 4: the transmitter and receiver, the control sequence, loss of signal |
| `dsp/vcvm_modem.h`, `vcvm_modem.cpp` | One DCE after V.8: the start-up, phase 4, renegotiations, retrains |
| `dsp/dp_vcvm.cpp` | The pump `DP_VCVM`: the bits of the core as octets (first bit the least significant), 9600 Hz lines through resamplers, `CONNECT`, the values for `+CVM?`, S44 (tones received) and S45 (renegotiations), and if the two sides have the preset dictionary |
| `link/v44_preset.h`, `v44_preset.cpp` | The text of Annex E, the preset dictionary of V.44 (`V44::prime()` in `link/v44.cpp`) |
| `link/zstd_codec.h`, `zstd_codec.cpp`, `zstd_dict.h`, `zstd_dict.cpp` | zstd (Annex F) and its dictionary |

## Receivers

- **V.21 messages.** Two filters matched over the bit (the detector that
  the measurements of the Recommendation use), with its own bit clock.
  The clock gain is 0.5 over the first 12 transitions, then 0.1 for
  transitions in ±0.3 bit of the expected boundary. A message stops
  after 30 ms of quiet (10 ms windows below −40 dBFS).
- **P20 messages.** If the FCS of a frame fails, the receiver tries again with the second
  most probable lag on the 5 least reliable symbols, one at a time and in
  pairs. If an INFO1 is lost, the receiver gets it from the MP of the peer, which carries the
  same layout.
- **L1's first symbol.** The cyclic prefix gives the symbol timing to
  one symbol. The receiver finds the first symbol from the
  onset of the signal.
  - Through AMR-WB, the codec opens slowly, and the onset can come up to
    half a symbol late. Then a small number of samples in one direction or the other gave
    one symbol in one direction or the other.
  - Then each symbol of the probing was compared with the incorrect known
    step: 70 to 80 per cent in error, no data mode was requested, and the
    start-up failed.
  - Now, the receiver tries the symbol before, the symbol that it found and the symbol
    after it. It keeps the symbol for which the steps of the first 8 symbols are nearest
    to the known steps (`VcvmProbeRx::resolve_start()`).
- **L3.** The P10 part is locked to the end of the P20 part. Through AMR 10.2 and lower,
  the receiver does not find the preamble of P10 well.
- **RS decoding** uses GMD (generalized minimum distance decoding).
  First it decodes with errors only, then with the 2, 4, … least reliable octets
  erased, up to n−k−2 erasures. It stops early when the next octet is
  not unreliable.
- **Phase 4.** Control sequences count only after a block of the data of the peer
  has decoded, and only at the level of data (with c > 0.5 in the
  parametric mode). Without this rule, the zero octets of an MP and silence both
  look like control sequences. The signal counts as lost when it is quiet,
  or when nothing decodes for max(2 s, 2.5 blocks).
- **Phase 4's start.** The data of the peer starts at the end of its one extra
  MP after our acknowledgement. Through AMR-WB, that MP is sometimes lost.
  Thus, if a transmission has no good frame while the data of the peer
  does not decode yet, the receiver accepts it as that MP. (Before this change, the receiver stayed
  aligned to the MP before it and never decoded. Then the call stopped,
  because there was no connection.)

## Where it departs from the Recommendation's first draft

These changes are now in the Recommendation:

- The sequence of phase 2 (above): the INFO1 of the calling DCE cannot carry
  measurements before it has received a signal.
- INFO0 octet 3 bit 5: the amplitude rings permitted (thus, `+CVM` can
  refuse them).
- The P10 part of L3 starts at the end of the P20 part.
- The loading selects the comb only when it carries more after the code
  than the parametric mode would carry. The loading selects P20 with up to 1 symbol in 40
  in error, at rate 1/2.
- One more MP after the acknowledgement of the peer.
- The start-up takes 15 to 21 s, not 16.
- The measured bit error ratio of V.21 is 1 to 3·10⁻² at AMR 5.15 to 6.7. Thus,
  V.21 messages are reliable only from AMR 7.4 and higher.
- The scrambler operates on the information octets before the code. (It
  was after the interleaver, where the descrambler made each line
  error three errors.)
- The comb of phase 4 sends its phase reference three times.
- Control sequences are gated as described above. The first MP in phase 4 is the
  expected extra MP of the peer, and the receiver aligns again to its end. The DCE answers only
  a second MP, which shows that the peer repeats.
- The retrain bit is MP octet 2 bit 8. After a retrain, the INFO0 of the calling
  DCE comes 3 s later.
- The retrain rule of c-bar has hysteresis (0.83/0.87) and has no effect in
  the first 30 s.
- V.8: the coding of the field in the non-standard facilities category, and
  the end of CM accepted as CJ.

## Not implemented

- The signal interface is 8000 Hz only. The modem accepts and keeps the `<rate>` 16000 of `+CVM`,
  but it does not offer the wideband modes.
- The modem accepts and keeps `+CVMR` (the `<report>` of `+CVM`), but it sends no unsolicited
  result.
- After a failed start-up, the call stops (`NO CARRIER`). It does not
  start V.8 again for V.22.
- No quick start (INFO0 octet 3 bits 7 and 6), no facsimile (Annex D),
  no V.8bis.
- EVS is not in the interop tests, because the `NET_CODEC` of netsim has only AMR and
  AMR-WB.

## Known issues

### A rare undetected error through AMR 7.4

Through AMR 7.4, after approximately 200 s of data at 175 bit/s, one run stopped
with an error from the V.44 decoder. A damaged frame had gone through the
16-bit FCS of LAPM. The Reed-Solomon decoder sometimes corrects incorrectly.
Now, LAPM negotiates a 32-bit FCS, on which two D-Modems agree. Thus, such
a frame must be found. We did not run this again for 200 s through AMR
7.4 yet. `open_vcvm_amr74` sends only one line in each direction.

### Slow start-ups through AMR-WB 23.85

P20 messages through AMR-WB 23.85 at an 8000 Hz interface are frequently lost
(c is about 0.8). The MP exchange recovers, but the start-up takes
more time.

`open_vcvm_amrwb2385` first failed in two ways. Small
changes of timing (the octets of V.8, the load of the machine) caused these failures:

- The first symbol of L1 was one symbol incorrect.
- The start of phase 4 was lost with the last MP of the peer.

We corrected the two failures (refer to [Receivers](#receivers)). After that, the test passed 7 in
10 alone and 1 in 6 under load. These are the causes, and the corrections:

- An MP was lost at some positions in the frame of the codec. Repeats a
  whole number of frames apart always came to the same positions. Thus, a renegotiation
  never was complete. Now, each MP in phase 4 and in its renegotiations goes
  6.25 ms later than the last MP, up to the fourth.
- An MP of the peer that was already on its way when our MP went again was accepted
  as a second MP and answered. This stopped our data again. Now, the DCE
  ignores it.
- A second lost MP of the peer left the data receiver at the incorrect
  position. Now, the receiver starts again after each MP.
- After a retrain, the last MP of the peer, which still came in, was read as its
  INFO0 (the first octet of an MP looks like the first octet of INFO0). Now, the receiver ignores it.

With those corrections, the test passed 23 of 24 alone and 4 of 6 under load.

One problem stays. The MPs of the start-up itself, a whole number of frames apart, can
all be lost for the timing of one call. If they got the same 6.25 ms steps,
the measured end of the last MP would move. The parametric data
receiver (with the 10 ms symbols of P10) accepts that end as the start of phase 4. Thus, the
receiver must first find its own symbol timing. The test stays on the
list of flaky tests (`cmake/flaky_tests.cmake`).

### Bulk transfers through AMR

Bulk transfers through AMR (`dsp/test/compress_calls.sh`, 32 kB in each direction)
stalled or dropped. These are the causes, and the corrections:

- After a renegotiation, the data of the peer never decoded, and LAPM stopped
  its polls. Now, the 6 s watch and the flags described above prevent this.
- The last MPs of the start-up were lost, and one side stopped immediately before
  the MPs of the other side came.
- The 2 s of T401 expired before an answer could come at 1100 to 2300 bit/s.
  Thus, the polls and go-backs continued through the data. Now, T401 has the time of three
  frames added (refer to [link-details.md](../link-details.md)).
- A frame of noise that passed the 16-bit FCS acknowledged I frames that
  were never received. Thus, they were lost. Now, the link ignores such an N(R).

With those corrections, AMR 12.2 and AMR-WB, with zstd or with no compression, passed
12 of 12 at six calls at the same time. At twelve calls at the same time, an AMR-WB call
without compression sometimes stops in one direction near its end, and
neither side logs an error.

## Tests

- `vcvm_test`: the signal set, through the codecs.
- `vcvm_startup_test`: capabilities (which include `VcvmCaps.NotK56flex`),
  messages, probing and loading, and the start-up through G.711, AMR
  12.2, AMR 7.4, AMR-WB and impaired lines.
- `vcvm_data_test`: phase 4, and `VcvmModem` through a clean line and
  AMR, with renegotiation, retrain and a cut line.
- `link_test`:
  - `Link.V44Preset`: V.44 primed with the preset dictionary, round trips
    with REINITs, and the gain on short messages;
  - `Link.Zstd`, `Link.ZstdDict`, `Link.LapmZstd`: the round trips of zstd,
    its dictionary, and its negotiation.
- `v44_preset_check`: the text is the text of the annex (and the dictionary
  header of zstd is the header of Annex F).
- `zstd_ref_test`: against libzstd, in the two directions (where libzstd is found).
- `rs_test`: the Reed-Solomon code.
- The `open.open_vcvm*` interop calls (table above): two open modems
  through netsim, with V.8 and LAPM. The AMR tests must have the AMR
  libraries.
  - `open_vcvm`: with zstd and its dictionary;
  - `open_vcvm_nodict`: without the preset on offer (zstd without a
    dictionary);
  - `open_vcvm_nozstd`: without zstd (V.44 primed);
  - `open_34_zstd` and `open_34_nozstd`: zstd over V.34.
