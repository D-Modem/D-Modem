# Leased lines and dry pairs: technical details

This is the technical companion to [Leased lines and dry pairs](leased.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

A leased line (a private circuit, or a dry copper pair of your own) has
no exchange: no dial tone, no ringing and no hook. D-Modem runs on one
with `AT&L1` (a 2-wire circuit) or `AT&L2` (a 4-wire one). The setting
is kept in S47 (`AT&L0` is a dialled line).

    AT&L1S0=1        the answering end, once
    AT&L1            the calling end, then ATD

## How a call starts and ends

- **Calling.** `ATD` (with any number, or none) calls at once. Call
  progress runs only to listen for the answer tone; it does not wait for
  a dial tone, listen for busy, or dial a number.
- **Answering.** `ATA` answers. With S0 above 0, the modem answers by
  itself, with no ring, whenever it is idle.
- **Starting again.** When the line ends a call (NO CARRIER), the modem
  starts the call again by itself 2 s later, dialling or answering as
  before. `ATH`, DTR, `ATZ` or `AT&L0` stop this until the next command
  line.
- **Wideband.** At 48000 Hz and above (a bare pair driven by
  pico-serial's WB stick) the modem is always a leased-line modem, in
  its wideband mode V.wb ([dsp/vwb.md](dsp/vwb.md)).

## 2-wire: AT&L1

Every voice band modulation of `+MS` runs on `AT&L1`: V.34, V.32bis,
V.32, V.22bis, V.22, Bell 212A, V.23, V.21 and Bell 103, with V.8 and
automode as on a dialled call. Both directions share the pair, so the
echo cancellers of V.34 and V.32 do the same work they do on a dialled
line.

A circuit can change under a call: a reroute on a carrier's network
brings a new echo path or delay.

- **V.34** retrains when its receiver's error grows (11.5/V.34).
- **V.32 and V.32bis** retrain too (5.5/V.32). The receiver asks for a
  retrain when its mean square error stays above its rate's limit for 2
  s while the far end's signal is still there. The limit is the rate's
  least SNR less 4 dB (7 dB at 4800 bit/s). `DSP_V32_LOST=0` turns this
  off.

## 4-wire: AT&L2

On a 4-wire circuit each end sends on its own pair, and the carrier can
stay on for the whole call. `AT&L2` with one of the 4-wire modulations
runs them as duplex data pumps on a continuous carrier
(`dsp/dp_4wire.cpp`):

| `+MS` | Recommendation | Rates (bit/s) |
|---|---|---|
| `V29` (29) | V.29 | 9600, 7200, 4800 |
| `V33` (33) | V.33 | 14400, 12000 (trellis coded) |
| `V27TC` (27) | V.27ter | 4800, 2400 |
| `V26` (26) | V.26 (alternative A) | 2400 |
| `B201` (201) | Bell 201 (V.26's alternative B) | 2400 |
| `B208` (208) | Bell 208 | 4800 |
| `B202` (202) | Bell 202 | 1200 (FSK) |

`V27TC` is V.250's name; `V29`, `V33`, `V26` and the Bell ones are
D-Modem's. The other modulations run on `AT&L2` as they do on `AT&L1`.

- **V.26 and Bell 201** use V.27ter's 2400 bit/s symbols (1200 baud,
  carrier 1800 Hz), with the data unscrambled as those modems sent it.
  The dibits' phase changes are those of V.26's alternative A (0, 90,
  180, 270 degrees) or B (45, 135, 225, 315: Bell 201). Their marks are
  phase reversals and their zeros an unmodulated carrier, so in the
  data neither means the far end is synchronizing: only the loss of
  the carrier or of the equalization does.
- **Bell 208** is V.27ter's 4800 bit/s signal (8-phase at 1600 baud,
  scrambled with 1 + x^-6 + x^-7), which V.27 took from it, at that
  rate alone.
- **Bell 202** runs on the FSK pump (`dsp/dp_fsk.cpp`): 1200 Hz mark,
  2200 Hz space, 1200 bit/s from each end on its own pair. Each end
  sends its mark at once, and the data starts once the far end's mark
  has been steady for 150 ms.
- The DPSK ones start with V.27ter's training as their synchronizing
  signal. That is D-Modem's own for V.26 and Bell 201, which had none,
  and Bell 208's own training is not V.27ter's: another D-Modem finds
  it, but an old modem of these kinds does not.

- **Start-up.** Each end sends its synchronizing signal at once
  (10/V.29, 9/V.33). It sends it again 1.2 s after its last one if the
  far end's has not come. CONNECT comes when this end's receiver has
  trained on the far end's signal and its own signal has gone out.
- **Rates.** `+MS`'s highest rate sets the rate an end sends at, and its
  lowest rate the lowest the end comes down to. Each receiver finds the
  far end's rate by itself:
  - V.29 and V.27ter have no rate signal. Parallel receivers try each
    rate of the family at once, and the one with the least training
    error wins.
  - V.33 sends its rate in its rate signal (segment 3, 8.3/V.33).

  An end that sends faster than the far end comes down to the far end's
  rate. For V.33 this is the Recommendation's rule (9/V.33); for V.29
  and V.27ter it is D-Modem's own.
- **Resynchronizing.** A receiver that loses the far end's signal or its
  equalization (its error above the rate's limit for 0.3 s) sends its
  own synchronizing signal, and the far end answers with its own. While
  this goes on, the DTE's data waits and the received data is held at
  ones.
- **Falling back.** An end that loses its equalization twice within 20
  s at its own rate takes the next rate down: V.33 14400 to 12000, V.29
  9600 to 7200 to 4800, V.27ter 4800 to 2400. It never goes below
  `+MS`'s lowest rate.
- **Carrier loss.** S10 is the time without the far end's signal before
  NO CARRIER. The call then starts again by itself, as on `AT&L1`.

## V.91 on a 4-wire digital circuit: AT&L2+MS=V91

A digital leased circuit (a 64 kbit/s DS0 or E1 time slot, or a G.711
trunk between two PBXs) carries PCM codes each way unchanged. Between
two D-Modems on such a circuit, `AT&L2+MS=V91` runs V.91, the symmetric
PCM modem: up to 62667 bit/s both ways (about 58700 bit/s of data
through V.42).

Both ends need a digital line at 8000 Hz: libdmodem with its G.711 law,
dmodemd on a PBX or SIP trunk, or the Asterisk and FreeSWITCH modules.
On an analogue line `+MS=V91` runs V.34.

- **Start-up** (8.8/V.91). There is no V.8 and no Ez. Each end sends
  INFO at once (8.2.1.2), and goes on sending it until the far end's
  comes, however late that end comes up. The rest is V.91's own
  start-up: INFO', the DIL, CP and B1. CONNECT comes when data mode is
  up both ways. The rate is the same each way unless the circuit treats
  the two directions differently.
- **Rates.** As on a switched call ([dsp/v91.md](dsp/v91.md)): 62667 on
  a clean circuit in either law, and less where the circuit robs bits
  (61333) or pads.
- **Retrains and carrier loss.** A receiver retrains (sends INFO again)
  when it decodes poorly, or when the far end's signal is gone (the same
  value for 6 ms: a cut circuit's idle code). The far end answers its
  INFO, and LAPM sends again what was lost. With no INFO back within 10
  s, the call ends with NO CARRIER and starts again by itself, as on
  `AT&L1`.
- **No V.34 escape.** There is no escape to V.34: 8.3/V.91 is for calls
  that began with V.8.

## Loop tests: AT&T (V.54, V.52)

| Command | Test |
|---|---|
| `AT&T0`, `AT&T` | end the test (after `&T7` or `&T8`: the bit errors, three digits, then OK) |
| `AT&T1` | loop 3, the local analogue loop |
| `AT&T8` | loop 3 with V.52's self-test |
| `AT&T3` | loop 2 here: the far end's data goes back to it |
| `AT&T4` / `AT&T5` | the far end may / may not ask for loop 2 (the default is `&T5`, and `ATZ` sets it again) |
| `AT&T6` | loop 2 at the far end, the remote digital loop |
| `AT&T7` | loop 2 at the far end with V.52's self-test |
| S18 | ends a test after that many seconds (0: never) |

### Loop 3

Loop 3 (`&T1`, `&T8`) runs offline, on a leased line (`&L1` or `&L2`)
with a 4-wire modulation (`+MS=V29`, `V33`, `V27TC`, `V26`, `B201` or
`B208`). The pump's
signal goes back into its own receiver, and the line is silent.

- `&T1` answers CONNECT, and the DTE's data comes back to it through the
  modulator and demodulator.
- `&T8` answers OK and stays in command state.

### Loop 2

Loop 2 (`&T6`, `&T7`, and `&T3` at the looping end) is asked for in band
from online command state (after `+++`), on a call with no error
correction (`\N0`). It uses V.54 clause 6's simplified signalling for
point-to-point circuits. The patterns are scrambled with
1 + x^-4 + x^-7 and sent at the data rate.

1. This end sends PREP: 2048 bits of scrambled 0s.
2. The far end, if it allows it (`&T4`), sends ACK (1948 bits of
   scrambled 1s) and loops what it receives.
3. This end starts the test 2148 bits after it recognises the ACK.
   `&T6` answers CONNECT and the DTE's data comes back to it; `&T7`
   answers OK.
4. `AT&T0` sends TERM (8192 bits of scrambled 1s, then 64 1s), after
   which the far end leaves its loop. `ATO` then goes back to the call.

With no ACK within 6144 bits after the PREP, the command gives ERROR.
The far end also leaves its loop when the carrier goes for 1 s.

### The self-test

The self-test (`&T7`, `&T8`) sends V.52's 511-bit pattern
(x^9 + x^5 + 1) in place of the DTE's data. The checker synchronises on
what comes back (32 bits in a row that follow the pattern), counts the
bits in error, and synchronises again after a slip (64 errors in 256
bits).

### Recognising the patterns

The recognition criteria are left to the DCE (V.54 5.1). Here, a
pattern is present in a 256-bit block when:

- at least 240 of its bits descramble to the pattern's bit, and
- the raw bits have a run of seven of the other value. (Seven 1s keep
  HDLC flags from passing for PREP.)

PREP is recognised after 4 such blocks in a row, ACK after 3 and TERM
after 16.

The code is `core/v54.h` and `core/v54.cpp`. It sits between the link
and the data pump, in `Modem::get_bits()` and `put_bits()`.

### V.250's test commands (V.58)

V.250 6.7 has AT commands for V.58's test objects. They drive the same
loops (`core/v58.cpp`):

| Command | Test |
|---|---|
| `AT+TERDL=1` / `0` | as `&T4` / `&T5` |
| `AT+TLDL=1` / `0` | loop 2 here (as `&T3`), and its end |
| `AT+TRDL=1` / `0` | loop 2 at the far end (as `&T6`), but OK when the far end loops, staying in online command state; `0` sends the TERM (OK after it) |
| `AT+TAL=1[,band]` / `0` | loop 3 (as `&T1`), but OK in online command state; `0` ends it |
| `AT+TTER=type,len,blocks,1` | the error rate test on the loop that runs (loop 2 at the far end, or loop 3), from online command state: V.52's pattern for `blocks` blocks of `len` bits, then the DTE's data again; `AT+TTER=0` stops it |
| `AT+TNUM?` | its bit errors and errored blocks (`+TNUM: bits,blocks`; V.250's text prints `+TTER:` here, a slip) |
| `AT+TALS?`, `AT+TDLS?`, `AT+TRDLS?` | the loops' status: loop 3 (0, 3), loop 2 here (1; 3 ours; 4 the far end's), loop 2 there (1, 4) |
| `AT+TSELF=0` / `1`, `AT+TRES?` | the self-test: the pattern generator, checker and recognisers check themselves (1 pass, 2 fail) |
| `AT+TE140`, `+TE141`, `+TEPDL`, `+TEPAL`, `+TMODE`, `+TADR` | 0 only: no circuits 140 and 141, no front panel, point to point |

The error rate test counts once the checker has synchronised; a block
is errored when one of its bits is. The pattern is V.52's 511-bit one
only (`pattern` 1): `+TTER`'s 63-bit, 2047-bit and fixed patterns give
ERROR.

## Simulated carrier: S48 (V.13)

On a continuous carrier the far end's carrier detect never drops. With
`ATS48=1`, on a call with no error correction, V.13's simulated carrier
control signals it in band:

- Between the DTE's bursts of data, this end sends the idle pattern:
  scrambled 1s (1 + x^-3 + x^-7). Before each burst it sends the ON
  pattern, 8 scrambled 0s, and then the data.
- The far end turns its circuit 109 (carrier detect) OFF after 56 bits
  of the idle pattern, and ON again after the ON pattern. While 109 is
  OFF its DTE gets nothing. A 109 left OFF for 2048 bits with no idle
  pattern comes ON by itself.

A pty has no RTS line, so circuit 105 (request to send) follows the
DTE's data. It turns ON with the data's first start bit, and OFF after
64 bits with nothing to send; at least 128 bits of the idle pattern go
out before it comes ON again.

The receiver's bits reach the link 64 bits late, so that none of the
idle pattern seen before 109 goes OFF reaches the DTE. Without S48 on
the far end, the idle pattern reaches its DTE as characters.

The code is `core/v13.h` and `core/v13.cpp`.

## Tests

`dsp/test/leased_call_test.cpp` runs two of libdmodem's modems at 9600
Hz on a simulated circuit: each way's delay and loss, white noise, and,
on a 2-wire circuit, each end's own signal through its hybrid.

| Test | What it checks |
|---|---|
| `TwoWire.Connects` | each voice band modulation on `AT&L1`: CONNECT and data both ways through V.42 |
| `FourWire.Connects` | V.29, V.33, V.27ter, V.26 and Bell 201, 202 and 208 on `AT&L2`, with their rates each way |
| `FourWirePlain.IdleZerosAndOnes` | V.26's, Bell 201's and Bell 202's unscrambled data idle and in long runs of zeros and ones, with no resynchronizing |
| `FourWireRates.FasterEndComesDown` | the faster end coming down to the far end's rate |
| `FourWireResync.DropoutUnderLapm` | a dropout the pumps resynchronize through |
| `FourWireResync.NoiseBringsV33Down` | noise that brings V.33 down to 12000 |
| `FourWireResync.LateAnswerer` | an answerer that comes up late |
| `FourWireResync.CallsAgainAfterACut` | a long cut, and the call again by itself |
| `TwoWireRetrain.AfterAReroute` | a reroute that V.34 and V.32bis retrain through |
| `TwoWireNoise.V32bisThroughABurst` | a burst of noise |
| `RemoteLoop.SelfTest`, `RemoteLoopData.*` | loop 2 with the self-test, the DTE's data echoed, a refused loop, `&T3` |
| `LocalLoop.SelfTestAndData`, `LocalLoop.TestTimer` | loop 3 with the self-test and with data; S18 |
| `SimulatedCarrier.BurstsAndIdle` | V.13's bursts and idle |

V.91 runs on a digital circuit instead (G.711 octets each way at 8000
Hz):

| Test | What it checks |
|---|---|
| `FourWireV91.Symmetric` | both laws at 62667 both ways |
| `FourWireV91Line.RobbedBits` | robbed bits |
| `FourWireV91Line.LateAnswerer` | a late answerer |
| `FourWireV91Line.Cuts` | a short cut it retrains through, and a long one it calls again after |

See [testing.md](testing.md) for how to run them.
