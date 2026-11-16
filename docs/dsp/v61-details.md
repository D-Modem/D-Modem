# V.61 (ASVD): technical details

The plain-English overview is [v61.md](v61.md). The references are to
ITU-T V.61 (08/96) and V.8 bis (11/2000).

## The parts

| Part | Where | What it does |
|---|---|---|
| `V61Pump` (`DP_V61`, 61) | `dsp/dp_v61.cpp` | The data pump: start-up, data alone, speech with data, retrain. 61 is the own number of D-Modem for it (`+MS=V61`) |
| `V61AudioTx`, `V61AudioRx` | `dsp/v61_audio.h`, `dsp/v61_audio.cpp` | The speech (8000 Hz) to complex samples at the symbol rate and back (5.5.1, 5.5.2.1 to 5.5.2.2) |
| `ToneTrk` | `dsp/qam.h`, `dsp/qam.cpp` | The tone trackers of the tones of the start-up and their phase reversals, shared with V.32 |
| `+ASVD`, `Modem::asvd_offered()` | `core/at.cpp`, `core/modem.cpp` | The own command of D-Modem `+ASVD=<on>` (0, 1). With V.8 bis enabled (`<v8b>` of `+A8E` is 1), the modem offers and selects ASVD |
| `MDMPRM_V8BIS` (76) | `dsp/modem_param.h`, `core/interface.cpp` | Bit 2: ASVD offered. `DP_V8BIS` sets it to 3 when it selected ASVD |
| `MDMPRM_VOICE` (77), `struct dp_voice` | `dsp/modem_param.h`, `dsp/modem_defs.h`, `core/interface.cpp` | The voice path for the pump: `state()`, `mic()`, `ear()`, to the hooks of `Host::dsvd_voice()` or the `VoicePath` of `Options::dsvd`. This is the same path as that of V.70. 0 if there is no voice path |
| The SVD mode of V.8 bis with V.61 | `dsp/dp_v8bis.cpp` | Refer to the subsequent section |

## V.8 bis

With `+ASVD=1`, the SVD mode of the CL has V.61, V.42 and V.14 (8.3.2/V.8
bis). The MS of the caller selects DSVD when the two ends have the items
that V.70 must have (refer to [v70-details.md](../v70-details.md)). If
not, it selects ASVD when the two ends have V.61. The MS then has these
items:

- The SVD mode with V.61, and the V.42 and V.14 bits that the two ends
  have.
- A request for ACK(1).
- No V.8 (the V.8 bit of the I field is clear, because V.61 has its own
  start-up).

The answerer accepts an MS with V.61 and not V.70 only if it offered ASVD.
Then it sends ACK(1). If not, it sends NAK(3) and uses V.8 without V.8
bis.

Then the two ends ask the core for `DP_V61` (`MDMPRM_DP_REQUESTED`) in
place of V.8. The answerer does this after it sends its ACK(1). The caller
does this at the ACK(1), or at 100 ms of the 2100 Hz of the answerer if
the ACK(1) was lost. Then the V.61 answerer starts with its own ANS.

## The line signal

| | 3000 symbols/s | 2800 symbols/s |
|---|---|---|
| Carrier | 1800 Hz | 1680 Hz |
| Frame | 70 symbols: 56 of user data, 14 of control segment | 70 symbols: 60 of user data, 10 of control segment |
| Audio carrier | 1600 Hz | 1493.3 Hz |
| Audio band (Table 4) | to 1425 Hz | to 1330 Hz |

The two symbol rates have 2400 data symbols each second. Thus, the rates
are the same at the two symbol rates: 2 to 6 bits each symbol for 4800 to
14400 bit/s. The transmit pulse is a square root raised cosine with a
roll off of 0.12 (Figure 7), over 16 symbols on each side. At 6 symbols,
its truncation caused inter-symbol interference at about -30 dB. This is
too much for 64 points.

Data alone is uncoded QAM, with the 4, 8, 16, 32 and 64 points of Figures
1 to 5. We read these points from the figures of the PDF with
`pdftotext -bbox`. The constellations nest, and a turn of 90 degrees does
not change them. The differential quadrant code of Table 1 is on the
first two bits of each symbol.

With speech (QADM, 5.5), a frame has the 4 points of Figure 6, at 2.5 dB
below the level of the 4 points of data alone, with 2 bits each symbol
(4800 bit/s). The speech is added around these points:

1. The speech is moved down by the audio carrier, low pass filtered and
   sampled at each symbol (`V61AudioTx`: a Kaiser windowed sinc, 88 taps
   on each side at 8000 Hz, calculated directly at the symbol instants,
   as the note to 5.5.1.2 allows).
2. The gain of the frame (5.5.2.3): steps of 2^(1/4), 0 to 31 for the
   smaller of the inphase and quadrature (the base), and up to 3 more for
   the other. The modem selects it from the peaks of the frame before it
   sends the frame.
3. Each second symbol is inverted (5.5.2.4).
4. Each part is scaled asymmetrically (5.5.2.5). The negative half goes
   0.7 of the distance to the edge of the quadrant (0.7 * sqrt 5 times
   the scale of the points). The positive half goes two times that
   distance.
5. The result is turned by the angle of the data point minus 45 degrees
   (Table 5). Thus, the negative half points to the edges of the
   quadrant.

The receiver selects the quadrant (the data point that is nearest in
angle). Then it reverses steps 5 to 2 on the remaining signal. The result
is the speech. `V61AudioRx` interpolates it back to 8000 Hz and moves it
up by the audio carrier. The two ends count the phase of the audio
carrier from the first symbol after E.

### What D-Modem selects

| Choice | Value | Why |
|---|---|---|
| Speech peak | 0.6 of the swing that step 4 allows (`V61_PEAK`) | At 0.95, a steady tone used all of the margin of the 4 points. There were errors below 30 dB, and lost control segments at 25 dB with a 1000 Hz tone. At 0.6, 4800 bit/s is clean to 20 dB |
| Silence | below -50 dB of full scale for 3 frames | Then data alone (5.3.2), if the rate word 3 of the far end allows it |
| Lock bit | set when the peak of the speech after the gain is more than 0.25 | The equalizer of the far receiver adapts only while it is clear |
| Pre-emphasis | index 20, flat (Tables 2 and 3) | No adaptive filter |
| Auxiliary channel | not used (AC 0, C7 to C0 `01110111`) | |

## Control segments

The bits of Table 6, one dibit for each control symbol. Symbol k sends
bit 2k+2 first, then 2k+1. In data alone, the third bit of each control
symbol is a copy of D, and the other bits are 0 (5.6.2). The receiver
does a check of the inverted copies (D and D-bar, L and L-bar, G4 to G2
and their inverses, the fixed bits). It selects D by the majority of D,
not D-bar and the copies. If the result is equal, it selects data alone
(Note 1 to Table 6). After a data-alone frame, the gain is 0 and the lock
bit counts as set.

## Start-up

| Step | Answerer | Caller |
|---|---|---|
| 1 | ANS (2100 Hz) | ANS heard |
| 2 | | S (A B A B, 256T) or AA (2800/s), then DCBA |
| 3 | Silence on S or AA; then its own S (or AA) for 256T after DCBA | |
| 4 | | G (1500 Hz, 1400 at 2800/s) on the S of the answerer |
| 5 | F (2100 Hz, 1960 at 2800/s) on G; its reversal after 128T + 64T | |
| 6 | | The reversal of G 64T after the reversal of F |
| 7 | The second reversal of F 64T after the reversal of G: MT counted | Silence on the second reversal of F: NT counted |
| 8 | S S-bar TRN R1 (until the S of the caller) | |
| 9 | | S (NT + 256T) S-bar TRN R2 (after R1, until R3) |
| 10 | Silence, then S S-bar TRN R3 (after R2), then E | E (after R3) |
| 11 | 50 symbols of B1, the first control segment, data | the same |

R1, R2 and R3 are the rate words 1, 2 and 3 (Tables 10 to 12). The modem
sends them one after the other and repeats them. Each rate word has these
properties:

- 16 bits, most significant (B0) first.
- Header 0000 for R and 1111 for E.
- Sync bits 0111.
- ID bits: B4 and B8 set for rate word 1, B4 for rate word 2, B8 for
  rate word 3.

Rate word 1 has the rates. Rate word 2 has bit B5 (A+D) if the sender
has a voice path. Rate word 3 has bit B14 (silence switching). The
caller scrambles with GPC (tap 18). The answerer scrambles with GPA
(tap 5).

Each receiver offers the rates that its training measured:

| SNR at training | Highest rate offered |
|---|---|
| 26 dB or more | 14400 |
| 23 dB | 12000 |
| 20 dB | 9600 |
| 17 dB | 7200 |
| below 17 dB | 4800 |

The echo canceller (256 taps) trains on our own TRN. Its step is 0.25,
then 0.05 at the half of TRN and 0.02 at three quarters. With a far echo
at -15 dB, it removes 28 dB.

The equalizer (48 taps at T/2) adapts in these conditions:

- On decisions in TRN: a step of 0.04 while its error is more than 0.01,
  then 0.02.
- On known symbols in B1: 0.04. These are the scrambled ones of the far
  end, which our descrambler predicts.
- On decisions in data alone: 0.02.
- With speech: slowly (0.002) while the lock bit is clear, and not at all
  while it is set. The carrier loop follows at all times.

With a smaller step in data (0.004), the error stayed at -24 dB for
seconds after the start. This is too high for 64 points.

## Retrain

These conditions start a retrain:

- The G or F of the far end for 128T in data.
- The receiver lost its equalization while the signal is still there:
  - Its error is 4 dB below the SNR of the rate for 2 s (7 dB at 4800
    bit/s).
  - Or the control segments fail their checks for 1 s.

Then the caller sends G and the answerer sends F, and the start-up runs
again from step 4.

## Tests

| Test | What it covers |
|---|---|
| `Line/PumpLoopback.DataBothWays/v61*` (`dsp/test/pump_test.cpp`, label `quick`) | Data alone in the two directions: a clean line and a 15 dB line, 8000 Hz, gain 2 with -300 ppm, near and far echoes |
| `V61.*` (`dsp/test/pump_test.cpp`, label `quick`) | Speech in the two directions (a 440 Hz and a 700 Hz tone): 4800 bit/s without an error, and each end hears more than 85% of the tone of the other end. At 30 and 20 dB, at 8000 Hz, at 2800 symbols/s. The microphone of the answerer silent, and its frames data alone |
| `Core.Asvd*` (`core/test/core_test.cpp`, label `quick`) | Two modems back to back. V.8 bis selects ASVD, and V.61 follows with V.42 (data alone without a voice path; speech in the two directions and the data of V.42 with a voice path, which opens and closes). ASVD only at the caller (V.8). DSVD selected where the two ends offer the two modes |
| `dsp_loopback 61 <snr> <secs> [srate gain ppm echo far]` | Manual test. `VOICE=<caller Hz>:<answerer Hz>` puts a tone on each microphone (a negative number gives a silent microphone). `DSP_V61_BAUD=2800` asks for 2800 symbols/s. `DSP_V61_VOICE=0` offers no voice |

## Not done

- Rate renegotiation (clause 11). The modem does a retrain in its place.
- Adaptive pre-emphasis (5.5.1.2). Only the flat filter is available.
- The auxiliary control channel (5.6.3).
- A change to ASVD during a telephone call (V.8 bis during a
  conversation).
- We tried it only between two D-Modems.
