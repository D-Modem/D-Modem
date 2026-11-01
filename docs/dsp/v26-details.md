# V.26 ter and V.26 bis: technical details

This is the technical companion to [V.26 ter and V.26 bis](v26.md).
V.26 ter's code is `dsp/dp_v26.cpp` (`V26Pump`, `DP_V26TER`, 126);
V.26 bis has [its own section](#v26-bis) at the end. Clause numbers up
to there are V.26 ter's (1988, the Blue Book).

## The signal

| | |
|---|---|
| Carrier | 1800 Hz both ways (2.1) |
| Symbol rate | 1200 baud (2.5.1) |
| Shaping | 100% raised cosine, split between transmitter and receiver (2.4): a root raised cosine of roll-off 1 at each end |
| 2400 bit/s | a dibit a symbol, the phase change 0, 90, 180, 270 degrees for 00, 01, 11, 10, the first bit on the left (Table 1) |
| 1200 bit/s | a bit a symbol, 0 or 180 degrees (Table 2) |
| Scrambler | self-synchronizing, 1 + x^-18 + x^-23 (GPC) at the caller, 1 + x^-5 + x^-23 (GPA) at the answerer (5, 6.1.1): `scramble()` in `bits.h`, V.32's |
| Points | (+-1, +-1); the phase changes are counted in quarter turns between the points decided |

The synchronizing signal (2.7) is 32 symbols of phase reversals, then
scrambled ones: 64 bits at either rate, from the register Appendix I
gives, so that its phase changes are Table 3's (0, 180, 180, 180, 180,
0, ...). Appendix I lists the register one step before the first bit
of its pattern: the code loads it and scrambles one bit before
segment 2. `0x4FFF83` (GPC) and `0x307070` (GPA) are those registers,
newest bit in bit 0 as `bits.h` keeps them.

## The start-up

| Sequence | Answerer (`A_*` states) | Caller (`C_*` states) |
|---|---|---|
| A (6.3.1.1) | 200 ms of silence, ANS (2100 Hz) for 3.3 s, 75 ms of silence | (call progress has heard the answer tone) |
| B (6.3.1.2), 1200 bit/s, half duplex | The synchronizing signal and 256 bits of its rate pattern, then silence. Again from the start after 2 s without the caller's pattern | On 4 octets of the answerer's pattern alike: 250 ms of silence, then the synchronizing signal and the pattern of the rate it chose |
| | On the caller's pattern: 250 ms of silence, 2100 Hz for 500 ms (G.164's echo suppressor disabling tone), 75 ms of silence. A rate it does not have ends the call | Silent |
| C (6.3.1.3), at the rate chosen | Its echo cancelling sequence (scrambled ones) for 1 s, its canceller learning; 25 ms of silence; the synchronizing signal and scrambled zeros | Silent until 64 zeros in a row |
| | Silent once the caller's signal has been there for 50 ms | Its echo cancelling sequence for 1 s (its canceller learning from 150 ms in, once the answerer is quiet); 25 ms of silence; the synchronizing signal and scrambled zeros |
| | On 64 zeros: the synchronizing signal and zeros, both sending | |
| | 256 zeros sent and 64 more received (or the caller's ones): ones | On 64 zeros: 109 ON, 128 ones, then data (`CONNECT`) |
| | The caller's ones (64) and 64 of its own: 109 ON, data (`CONNECT`) | |

The rate pattern (6.1.3) is an octet sent 32 times, least significant
bit first, scrambled. The receiver takes a pattern when the last 32
descrambled bits repeat every 8 and the octet is a rotation of one of
Table 7's (which 6.1.3 allows). `+MS`'s rates set ours:

| Octet | Rates | Sent by D-Modem |
|---|---|---|
| 01 | 1200 | an answerer with `+MS`'s highest rate below 2400; a caller that chose 1200 |
| 03 | 2400 | an answerer with `+MS`'s lowest rate above 1200; a caller that chose 2400 |
| 07 | 1200 and 2400 | an answerer otherwise |
| 05, 09 | 4800, 2400 and 4800 (a 4800 bit/s modem's, provisional) | never; 09 is taken as 2400 |

Where the Recommendation leaves room, D-Modem's choices are:

- **Lengths.** The echo cancelling sequence is 1 s (note 3 asks for 650
  ms at least where the network has its own echo cancellers). The
  answerer sends 256 zeros at least in its second burst, so that the
  caller finds 64 of them after its synchronizing signal.
- **Ones before data.** The answerer sends 64 ones before its data, and
  the caller's receiver passes data to the link only after 16 ones in a
  row: the zeros of the start-up would reach an asynchronous DTE as a
  break.
- **Gaps.** The receiver keeps its gain, equalizer and phase through a
  gap in the far end's signal (its symbols' power below a fifth of the
  points') once it has heard 100 symbols, for up to 200 symbols: the 25
  ms of silence in sequence C is too short for the carrier detector to
  notice, and the gain learned on silence would overdrive the signal
  that follows it.

## The receiver

`QamRx` (`qam.h`): down conversion, matched filter, AGC, Gardner timing
recovery, a 16 tap T/2 equalizer and the carrier loop, decision directed
on the four points from the first symbol (V.26 ter has no training
sequence; 2.3 asks for an equalizer that converges on data). The echo
canceller is `EchoCanceller` (128 taps, NLMS), trained only during the
modem's own echo cancelling sequence and then fixed, or turned off when
it took away less than 6 dB (no echo to cancel).

The call's statistics (ATI11, `+TMO`, V.59) get the SNR from the
decision error, the symbol rate and carrier, and the frequency and
timing offsets. V.59 lists it among the modulations it has no object
for (`V.26ter` in 0x0101).

## Not done

- The optional half-duplex mode (clause 7).
- Refinement of the echo canceller during data (6.3.1.3 c and e: it is
  trained once, before the data).
- Test loops 2 and 3 (clause 8, V.54).
- Leased lines (6.1.2): V.26 ter runs on the switched network only.
- Interop with another maker's V.26 ter modem.

## Tests

| Test | What it covers |
|---|---|
| `Line/PumpLoopback.DataBothWays/v26ter*` (`dsp/test/pump_test.cpp`) | Two pumps back to back: a clean line, 15 dB of SNR, 8000 Hz, a gain of 2 with -300 ppm, each modem's own echo at -6 dB. Each connects at 2400 bit/s and carries its data at a bit error rate of 1e-3 at most (none in these runs) |
| `Modulations/DmodemPair.DataBothWays/v26ter`, `v26ter_1200` (`dsp/test/dmodem_test.cpp`) | Two libdmodem modems with `AT+MS=V26T`: dialling, the answer tone, the start-up, text both ways; with `AT+MS=V26T,0,1200,1200` at 1200 bit/s |
| `Scripts/CoreEquiv.SameTranscript/at`, `v59` | `+MS=?` lists 126; V.59's 0x0101 lists V.26ter |

## V.26 bis

The code is `dsp/dp_v26bis.cpp` (`V26bisPump`, `DP_V26BIS`, 226).
Clause numbers here are V.26 bis's (1984, the Blue Book).

### The signal

| | |
|---|---|
| Forward channel | 1800 Hz carrier (2.1), 1200 baud (2.4.3) |
| 2400 bit/s | a dibit a symbol, V.26's alternative B: the phase change 45, 135, 225, 315 degrees for 00, 01, 11, 10, the first bit on the left (Table 1) |
| 1200 bit/s | a bit a symbol, 90 degrees for 0 and 270 for 1 (2.5.1) |
| Scrambler | none |
| Shaping | not set by V.26 bis; D-Modem uses a root raised cosine of roll-off 1 at each end, as V.26 ter's, whose spectrum (600 to 3000 Hz) leaves the backward channel clear |
| Points | 8 at 45 degree steps, radius sqrt(2): the phase changes are odd steps at 2400, even ones at 1200 |
| Backward channel | FSK at 75 baud, 390 Hz mark, 450 Hz space (4.1), the mark when idle |
| Synchronizing signal | the continuous dibit 11 (2.4.2), 225 degrees a symbol. V.26 bis gives none for 1200 bit/s; D-Modem sends continuous ones there, 270 degrees a symbol |

### The call

Table 2/V.26 bis allows a one-way forward channel with or without the
backward channel, or one that goes either way (half duplex). D-Modem
runs the one-way system with the backward channel, both channels on for
the whole call, as its V.23 runs:

| Step | Answerer (forward channel) | Caller (backward channel) |
|---|---|---|
| 1 | 200 ms of silence, ANS (2100 Hz) for 3.3 s, 75 ms of silence | (call progress has heard the answer tone) |
| 2 | The synchronizing signal at `+MS`'s highest rate (1200 when that is below 2400) | On 32 symbols of either synchronizing signal in a row (each phase change within 22.5 degrees of 225 or of 270): that rate |
| 3 | | The backward channel's mark; `CONNECT` after 150 ms of it (75/2400 or 75/1200) |
| 4 | After 1 s of the synchronizing signal (106's delay at call establishment is 750 to 1400 ms, Table 3) and 150 ms of the caller's mark: `CONNECT` (2400/75 or 1200/75) and the data | The mark for 1.5 s more, then the link's data (the answerer may still be in its synchronizing signal) |

V.42 and MNP are off on V.26 bis calls, as on V.23's: a 75 bit/s
backward channel is too slow for their acknowledgements to keep 2400
bit/s going.

### The receivers

The forward channel's is `QamRx` (`qam.h`), with no training sequence:
its carrier loop runs on the synchronizing signal (decisions on the 8
points), and its equalizer adapts on the data, decision directed. The
synchronizing signal is a single tone (a constant phase change), from
which an equalizer cannot learn the line; adapting on it spoils the
equalizer for the data. The phase change is taken from the symbol and
the one before it (the angle of z times the conjugate of the last z),
in quadrants at 2400 and halves at 1200.

The backward channel's is `FskDemod`, V.23's.

### Not done

- The half-duplex (either-way) system, with its turnarounds and
  circuit 105, 106 and 109 timings (Table 3).
- The forward channel from the caller.
- Interop with another maker's V.26 bis modem.

### Tests

| Test | What it covers |
|---|---|
| `Line/PumpLoopback.DataBothWays/v26bis*` (`dsp/test/pump_test.cpp`) | Two pumps back to back: a clean line, 15 dB of SNR, 8000 Hz, a gain of 2 with -300 ppm, each modem's own echo at -6 dB. Both connect, 2400/75, the bit error rate 1e-3 at most (none in these runs) |
| `Modulations/DmodemPair.DataBothWays/v26bis`, `v26bis_1200` (`dsp/test/dmodem_test.cpp`) | Two libdmodem modems with `AT+MS=V26B`: text both ways (31 s for it at 75 bit/s); with `AT+MS=V26B,0,1200,1200`, the caller finding 1200 bit/s |
| `Scripts/CoreEquiv.SameTranscript/at`, `v59` | `+MS=?` lists 226; V.59's 0x0101 lists V.26bis |
