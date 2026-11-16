# V.26 ter and V.26 bis: technical details

This page gives the technical details for [V.26 ter and V.26 bis](v26.md).
The code of V.26 ter is `dsp/dp_v26.cpp` (`V26Pump`, `DP_V26TER`, 126).
V.26 bis has [its own section](#v26-bis) at the end. Up to that section, the clause numbers
are the clause numbers of V.26 ter (1988, the Blue Book).

## The signal

| | |
|---|---|
| Carrier | 1800 Hz in the two directions (2.1) |
| Symbol rate | 1200 baud (2.5.1) |
| Shaping | 100% raised cosine, divided between transmitter and receiver (2.4): a root raised cosine of roll-off 1 at each end |
| 2400 bit/s | a dibit for each symbol; the phase change is 0, 90, 180, 270 degrees for 00, 01, 11, 10, the first bit on the left (Table 1) |
| 1200 bit/s | a bit for each symbol, 0 or 180 degrees (Table 2) |
| Scrambler | self-synchronizing, 1 + x^-18 + x^-23 (GPC) at the caller, 1 + x^-5 + x^-23 (GPA) at the answerer (5, 6.1.1): `scramble()` in `bits.h`, the scrambler of V.32 |
| Points | (+-1, +-1); the phase changes are counted in quarter turns between the decided points |

The synchronizing signal (2.7) is 32 symbols of phase reversals, then
scrambled ones. There are 64 bits at the two rates, from the register that Appendix I
gives. Thus, its phase changes are the phase changes of Table 3 (0, 180, 180, 180, 180,
0, ...). Appendix I gives the register one step before the first bit
of its pattern. The code loads it and scrambles one bit before
segment 2. `0x4FFF83` (GPC) and `0x307070` (GPA) are those registers,
with the newest bit in bit 0, as `bits.h` keeps them.

## The start-up

| Sequence | Answerer (`A_*` states) | Caller (`C_*` states) |
|---|---|---|
| A (6.3.1.1) | 200 ms of silence, ANS (2100 Hz) for 3.3 s, 75 ms of silence | (call progress has heard the answer tone) |
| B (6.3.1.2), 1200 bit/s, half duplex | The synchronizing signal and 256 bits of its rate pattern, then silence. It starts again after 2 s without the pattern of the caller | On 4 equal octets of the pattern of the answerer: 250 ms of silence, then the synchronizing signal and the pattern of the rate that it selected |
| | On the pattern of the caller: 250 ms of silence, 2100 Hz for 500 ms (the echo suppressor disabling tone of G.164), 75 ms of silence. If the answerer does not have the rate, the call stops | Silent |
| C (6.3.1.3), at the selected rate | Its echo cancelling sequence (scrambled ones) for 1 s, while its canceller learns; 25 ms of silence; the synchronizing signal and scrambled zeros | Silent until 64 zeros in sequence |
| | Silent after the signal of the caller has been present for 50 ms | Its echo cancelling sequence for 1 s (its canceller learns from 150 ms after the start, when the answerer is quiet); 25 ms of silence; the synchronizing signal and scrambled zeros |
| | On 64 zeros: the synchronizing signal and zeros; the two modems send | |
| | After 256 zeros sent and 64 more received (or the ones of the caller): ones | On 64 zeros: 109 ON, 128 ones, then data (`CONNECT`) |
| | After the ones of the caller (64) and 64 of its own ones: 109 ON, data (`CONNECT`) | |

The rate pattern (6.1.3) is an octet sent 32 times, least significant
bit first, scrambled. The receiver accepts a pattern when the last 32
descrambled bits repeat each 8 bits and the octet is a rotation of one of
the octets of Table 7 (6.1.3 lets the receiver do this). The rates of `+MS` set our pattern:

| Octet | Rates | Sent by D-Modem |
|---|---|---|
| 01 | 1200 | an answerer with `+MS`'s highest rate below 2400; a caller that chose 1200 |
| 03 | 2400 | an answerer with `+MS`'s lowest rate above 1200; a caller that chose 2400 |
| 07 | 1200 and 2400 | an answerer otherwise |
| 05, 09 | 4800, 2400 and 4800 (a 4800 bit/s modem's, provisional) | never; 09 is taken as 2400 |

Where the Recommendation lets the modem select, D-Modem selects these items:

- **Lengths.** The echo cancelling sequence is 1 s (note 3 asks for a minimum of 650
  ms where the network has its own echo cancellers). The
  answerer sends a minimum of 256 zeros in its second burst. Thus, the
  caller finds 64 of them after its synchronizing signal.
- **Ones before data.** The answerer sends 64 ones before its data. The
  receiver of the caller sends data to the link only after 16 ones in
  sequence, because the zeros of the start-up would go to an asynchronous DTE as a
  break.
- **Gaps.** After the receiver has heard 100 symbols, it keeps its gain, equalizer and phase
  through a gap in the signal of the far end (when the power of its symbols is less than a fifth of the
  power of the points), for up to 200 symbols. The 25
  ms of silence in sequence C is too short for the carrier detector to
  find. Also, a gain learned on silence would overdrive the subsequent signal.

## The receiver

`QamRx` (`qam.h`) has these parts: down conversion, matched filter, AGC, Gardner timing
recovery, a 16 tap T/2 equalizer and the carrier loop. These are decision directed
on the four points from the first symbol. (V.26 ter has no training
sequence. 2.3 asks for an equalizer that converges on data.) The echo
canceller is `EchoCanceller` (128 taps, NLMS). It trains only during the
echo cancelling sequence of the modem, and then it is fixed. If it
removed less than 6 dB (no echo to cancel), it is disabled.

The statistics of the call (ATI11, `+TMO`, V.59) get the SNR from the
decision error, the symbol rate and carrier, and the frequency and
timing offsets. V.59 gives V.26 ter in the list of the modulations that have no object
(`V.26ter` in 0x0101).

## Not done

- The optional half-duplex mode (clause 7).
- Refinement of the echo canceller during data (6.3.1.3 c and e: it
  trains one time, before the data).
- Test loops 2 and 3 (clause 8, V.54).
- Leased lines (6.1.2): V.26 ter runs only on the switched network.
- Interop with a V.26 ter modem from a different manufacturer.

## Tests

| Test | What it covers |
|---|---|
| `Line/PumpLoopback.DataBothWays/v26ter*` (`dsp/test/pump_test.cpp`) | Two pumps back to back: a clean line, 15 dB of SNR, 8000 Hz, a gain of 2 with -300 ppm, the echo of each modem at -6 dB. Each connects at 2400 bit/s and carries its data at a maximum bit error rate of 1e-3 (none in these runs) |
| `Modulations/DmodemPair.DataBothWays/v26ter`, `v26ter_1200` (`dsp/test/dmodem_test.cpp`) | Two libdmodem modems with `AT+MS=V26T`: dialling, the answer tone, the start-up, text in the two directions; with `AT+MS=V26T,0,1200,1200` at 1200 bit/s |
| `Scripts/CoreEquiv.SameTranscript/at`, `v59` | `+MS=?` gives 126; 0x0101 of V.59 gives V.26ter |

## V.26 bis

The code is `dsp/dp_v26bis.cpp` (`V26bisPump`, `DP_V26BIS`, 226).
In this section, the clause numbers are the clause numbers of V.26 bis (1984, the Blue Book).

### The signal

| | |
|---|---|
| Forward channel | 1800 Hz carrier (2.1), 1200 baud (2.4.3) |
| 2400 bit/s | a dibit for each symbol, the alternative B of V.26: the phase change is 45, 135, 225, 315 degrees for 00, 01, 11, 10, the first bit on the left (Table 1) |
| 1200 bit/s | a bit for each symbol, 90 degrees for 0 and 270 for 1 (2.5.1) |
| Scrambler | none |
| Shaping | V.26 bis does not set it. D-Modem uses a root raised cosine of roll-off 1 at each end, as for V.26 ter. Its spectrum (600 to 3000 Hz) keeps the backward channel clear |
| Points | 8 at 45 degree steps, radius sqrt(2): the phase changes are odd steps at 2400, even steps at 1200 |
| Backward channel | FSK at 75 baud, 390 Hz mark, 450 Hz space (4.1), the mark when idle |
| Synchronizing signal | the continuous dibit 11 (2.4.2), 225 degrees for each symbol. V.26 bis gives none for 1200 bit/s. At that rate, D-Modem sends continuous ones, 270 degrees for each symbol |

### The call

Table 2/V.26 bis permits a one-way forward channel with or without the
backward channel, or a forward channel that can go in the two directions (half duplex). D-Modem
uses the one-way system with the backward channel. The two channels are on for
all of the call, as in its V.23:

| Step | Answerer (forward channel) | Caller (backward channel) |
|---|---|---|
| 1 | 200 ms of silence, ANS (2100 Hz) for 3.3 s, 75 ms of silence | (call progress has heard the answer tone) |
| 2 | The synchronizing signal at the highest rate of `+MS` (1200 when that rate is below 2400) | On 32 symbols in sequence of one of the synchronizing signals (each phase change in 22.5 degrees of 225 or of 270): that rate |
| 3 | | The mark of the backward channel; `CONNECT` after 150 ms of it (75/2400 or 75/1200) |
| 4 | After 1 s of the synchronizing signal (the delay of 106 at call establishment is 750 to 1400 ms, Table 3) and 150 ms of the mark of the caller: `CONNECT` (2400/75 or 1200/75) and the data | The mark for 1.5 s more, then the data of the link (the answerer can still send its synchronizing signal) |

V.42 and MNP are disabled on V.26 bis calls, as on V.23 calls. A 75 bit/s
backward channel is too slow for their acknowledgements to keep 2400
bit/s in operation.

### The receivers

The receiver of the forward channel is `QamRx` (`qam.h`), with no training sequence.
Its carrier loop operates on the synchronizing signal (decisions on the 8
points), and its equalizer adapts on the data, decision directed. The
synchronizing signal is a single tone (a constant phase change). An equalizer cannot learn the line
from it, and adaptation on it makes the
equalizer bad for the data. The modem calculates the phase change from the symbol and
the symbol before it (the angle of z multiplied by the conjugate of the last z),
in quadrants at 2400 and halves at 1200.

The receiver of the backward channel is `FskDemod`, the receiver of V.23.

### Not done

- The half-duplex (two-direction) system, with its turnarounds and the timings
  of circuits 105, 106 and 109 (Table 3).
- The forward channel from the caller.
- Interop with a V.26 bis modem from a different manufacturer.

### Tests

| Test | What it covers |
|---|---|
| `Line/PumpLoopback.DataBothWays/v26bis*` (`dsp/test/pump_test.cpp`) | Two pumps back to back: a clean line, 15 dB of SNR, 8000 Hz, a gain of 2 with -300 ppm, the echo of each modem at -6 dB. The two connect, 2400/75, with a maximum bit error rate of 1e-3 (none in these runs) |
| `Modulations/DmodemPair.DataBothWays/v26bis`, `v26bis_1200` (`dsp/test/dmodem_test.cpp`) | Two libdmodem modems with `AT+MS=V26B`: text in the two directions (31 s for it at 75 bit/s); with `AT+MS=V26B,0,1200,1200`, the caller finds 1200 bit/s |
| `Scripts/CoreEquiv.SameTranscript/at`, `v59` | `+MS=?` gives 226; 0x0101 of V.59 gives V.26bis |
