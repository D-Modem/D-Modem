# V.61 (ASVD): technical details

The plain-English overview is [v61.md](v61.md). The references are to
ITU-T V.61 (08/96) and V.8 bis (11/2000).

## The parts

| Part | Where | What it does |
|---|---|---|
| `V61Pump` (`DP_V61`, 61) | `dsp/dp_v61.cpp` | The data pump: start-up, data alone, speech with data, retrain. 61 is D-Modem's own number for it (`+MS=V61`) |
| `V61AudioTx`, `V61AudioRx` | `dsp/v61_audio.h`, `dsp/v61_audio.cpp` | The speech (8000 Hz) to complex samples at the symbol rate and back (5.5.1, 5.5.2.1 to 5.5.2.2) |
| `ToneTrk` | `dsp/qam.h`, `dsp/qam.cpp` | The tone trackers of the start-up's tones and their phase reversals, shared with V.32 |
| `+ASVD`, `Modem::asvd_offered()` | `core/at.cpp`, `core/modem.cpp` | D-Modem's own `+ASVD=<on>` (0, 1): with V.8 bis on (`+A8E`'s `<v8b>` 1), ASVD is offered and chosen |
| `MDMPRM_V8BIS` (76) | `dsp/modem_param.h`, `core/interface.cpp` | Bit 2: ASVD offered. Set to 3 by `DP_V8BIS` when it chose ASVD |
| `MDMPRM_VOICE` (77), `struct dp_voice` | `dsp/modem_param.h`, `dsp/modem_defs.h`, `core/interface.cpp` | The voice path for the pump: `state()`, `mic()`, `ear()`, to `Host::dsvd_voice()`'s hooks or `Options::dsvd`'s `VoicePath`, the same path as V.70's. 0 without one |
| V.8 bis's SVD mode with V.61 | `dsp/dp_v8bis.cpp` | Below |

## V.8 bis

With `+ASVD=1` the CL's SVD mode has V.61, V.42 and V.14 (8.3.2/V.8
bis). The caller's MS chooses DSVD when both have what V.70 needs (see
[v70-details.md](../v70-details.md)), else ASVD when both have V.61:
the SVD mode with V.61 and the V.42 and V.14 bits both have, ACK(1)
asked for, no V.8 (the I field's V.8 bit clear: V.61 has its own
start-up). The answerer takes an MS with V.61 and not V.70 only if it
offered ASVD, with ACK(1); else NAK(3) and V.8 without V.8 bis.

Then both ends ask the core for `DP_V61` (`MDMPRM_DP_REQUESTED`) in
place of V.8: the answerer once its ACK(1) is out, the caller at the
ACK(1), or at 100 ms of the answerer's 2100 Hz if the ACK(1) was lost.
The V.61 answerer then starts with its own ANS.

## The line signal

| | 3000 symbols/s | 2800 symbols/s |
|---|---|---|
| Carrier | 1800 Hz | 1680 Hz |
| Frame | 70 symbols: 56 of user data, 14 of control segment | 70 symbols: 60 of user data, 10 of control segment |
| Audio carrier | 1600 Hz | 1493.3 Hz |
| Audio band (Table 4) | to 1425 Hz | to 1330 Hz |

Both have 2400 data symbols a second, so the rates are the same at
both: 2 to 6 bits a symbol for 4800 to 14400 bit/s. The transmit pulse
is a square root raised cosine of roll off 0.12 (Figure 7), over 16
symbols each side: at 6, its truncation left inter-symbol interference
at about -30 dB, which 64 points do not survive.

Data alone is uncoded QAM, the 4, 8, 16, 32 and 64 points of Figures
1 to 5 (read from the PDF's figures with `pdftotext -bbox`: they nest,
and each is unchanged by a turn of 90 degrees), with Table 1's
differential quadrant code on the first two bits of each symbol.

With speech (QADM, 5.5) a frame has the 4 points of Figure 6, at 2.5 dB
under the data-alone 4 points' level, 2 bits a symbol (4800 bit/s),
and the speech added around them:

1. The speech moved down by the audio carrier, low pass filtered and
   taken at each symbol (`V61AudioTx`: a Kaiser windowed sinc, 88 taps
   each side at 8000 Hz, evaluated at the symbol instants directly, as
   the note to 5.5.1.2 allows).
2. The frame's gain (5.5.2.3): 2^(1/4) steps, 0 to 31 for the smaller
   of the inphase and quadrature (the base) and up to 3 more for the
   other, chosen from the frame's peaks ahead of it.
3. Every other symbol inverted (5.5.2.4).
4. Each part scaled asymmetrically (5.5.2.5): a negative half that
   reaches 0.7 of the way to the quadrant's edge (0.7 * sqrt 5 times
   the points' scale), a positive half twice that.
5. Turned by the data point's angle less 45 degrees (Table 5), so the
   negative half points toward the quadrant's edges.

The receiver decides the quadrant (the data point nearest in angle) and
undoes 5 to 2 on the rest: that is the speech. `V61AudioRx` interpolates
it back to 8000 Hz and moves it up by the audio carrier. Both ends count
the audio carrier's phase from the first symbol after E.

### What D-Modem chooses

| Choice | Value | Why |
|---|---|---|
| Speech peak | 0.6 of the swing step 4 allows (`V61_PEAK`) | At 0.95 a steady tone took the 4 points' margin: errors below 30 dB, and lost control segments at 25 dB with a 1000 Hz tone. At 0.6, 4800 bit/s is clean to 20 dB |
| Silence | under -50 dB of full scale for 3 frames | Then data alone (5.3.2), if the far end's rate word 3 allows it |
| Lock bit | set when the speech's peak after the gain is over 0.25 | The far receiver's equalizer adapts only while it is clear |
| Pre-emphasis | index 20, flat (Tables 2 and 3) | No adaptive filter |
| Auxiliary channel | not used (AC 0, C7 to C0 `01110111`) | |

## Control segments

Table 6's bits, one dibit a control symbol: symbol k carries bit 2k+2
first, then 2k+1. In data alone the third bit of each control symbol is
a copy of D, the rest 0 (5.6.2). The receiver checks the inverted
copies (D and D-bar, L and L-bar, G4 to G2 and their inverses, the
fixed bits) and decides D by the majority of D, not D-bar and the copies,
a tie being data alone (Note 1 to Table 6). After a data-alone frame the
gain is 0 and the lock bit counts as set.

## Start-up

| Step | Answerer | Caller |
|---|---|---|
| 1 | ANS (2100 Hz) | ANS heard |
| 2 | | S (A B A B, 256T) or AA (2800/s), then DCBA |
| 3 | Silence on S or AA; then its own S (or AA) for 256T after DCBA | |
| 4 | | G (1500 Hz, 1400 at 2800/s) on the answerer's S |
| 5 | F (2100 Hz, 1960 at 2800/s) on G; its reversal after 128T + 64T | |
| 6 | | G's reversal 64T after F's |
| 7 | F's second reversal 64T after G's: MT counted | Silence on F's second reversal: NT counted |
| 8 | S S-bar TRN R1 (until the caller's S) | |
| 9 | | S (NT + 256T) S-bar TRN R2 (after R1, until R3) |
| 10 | Silence, then S S-bar TRN R3 (after R2), then E | E (after R3) |
| 11 | 50 symbols of B1, the first control segment, data | the same |

R1, R2 and R3 are rate words 1, 2 and 3 (Tables 10 to 12), sent one
after the other and repeated: 16 bits each, most significant (B0)
first, header 0000 for R and 1111 for E, sync bits 0111, rate word 1's
ID bits B4 and B8 set, rate word 2's B4, rate word 3's B8. Rate word 1
has the rates, rate word 2 bit B5 (A+D) where the sender has a voice
path, rate word 3 bit B14 (silence switching). The caller scrambles with
GPC (tap 18), the answerer with GPA (tap 5).

Each receiver offers the rates its training measured:

| SNR at training | Highest rate offered |
|---|---|
| 26 dB or more | 14400 |
| 23 dB | 12000 |
| 20 dB | 9600 |
| 17 dB | 7200 |
| below 17 dB | 4800 |

The echo canceller (256 taps) trains on our own TRN. Its step is 0.25,
then 0.05 halfway through TRN and 0.02 at three quarters: with a far
echo at -15 dB it takes 28 dB away.

The equalizer (48 taps at T/2) adapts on decisions in TRN (a step of
0.04 while its error is over 0.01, then 0.02), on known symbols in B1
(0.04: the far end's scrambled ones, predicted from our descrambler) and
on decisions in data alone (0.02). With speech it adapts slowly (0.002)
while the lock bit is clear and not at all while it is set; the carrier
loop follows throughout. With a smaller step in data (0.004) the error
stayed at -24 dB for seconds after the start, too high for 64 points.

## Retrain

On the far end's G or F for 128T in data, or when the receiver has lost
its equalization while the signal is still there (its error 4 dB short
of the rate's SNR for 2 s, 7 dB at 4800 bit/s, or the control segments
failing their checks for 1 s), the caller sends G and the answerer F,
and the start-up runs again from step 4.

## Tests

| Test | What it covers |
|---|---|
| `Line/PumpLoopback.DataBothWays/v61*` (`dsp/test/pump_test.cpp`, label `quick`) | Data alone both ways: a clean and a 15 dB line, 8000 Hz, gain 2 with -300 ppm, near and far echoes |
| `V61.*` (`dsp/test/pump_test.cpp`, label `quick`) | Speech both ways (a 440 Hz and a 700 Hz tone): 4800 bit/s without an error and each end hearing more than 85% of the other's tone, at 30 and 20 dB, at 8000 Hz, at 2800 symbols/s; the answerer's microphone silent, its frames data alone |
| `Core.Asvd*` (`core/test/core_test.cpp`, label `quick`) | Two modems back to back: V.8 bis chooses ASVD and V.61 follows with V.42 (data alone without a voice path; speech both ways and V.42's data with one, the voice path opened and closed); ASVD at the caller alone (V.8); DSVD chosen where both offer both |
| `dsp_loopback 61 <snr> <secs> [srate gain ppm echo far]` | By hand: `VOICE=<caller Hz>:<answerer Hz>` puts a tone on each microphone (a negative number a silent one), `DSP_V61_BAUD=2800` asks for 2800 symbols/s, `DSP_V61_VOICE=0` offers no voice |

## Not done

- Rate renegotiation (clause 11): a retrain instead.
- Adaptive pre-emphasis (5.5.1.2): the flat filter only.
- The auxiliary control channel (5.6.3).
- Changing to ASVD during a telephone call (V.8 bis in the middle of a
  conversation).
- Tried only between two D-Modems.
