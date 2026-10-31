# Leased lines and dry pairs: technical details

This is the technical companion to [Leased lines and dry pairs](leased.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

A leased line (a private circuit, or a dry copper pair of your own) has
no exchange: no dial tone, no ringing, no hook. D-Modem runs on one with
`AT&L1` (a 2-wire circuit) or `AT&L2` (a 4-wire one), kept in S47:

    AT&L1S0=1        the answering end, once
    AT&L1            the calling end, then ATD

- `ATD` (any number, or none) calls at once. Call progress runs only to
  listen for the answer tone: no dial tone, busy tone or number. `ATA`
  answers. With S0 above 0, the modem answers by itself, with no ring,
  whenever it is idle.
- When the line ends a call (NO CARRIER), the modem starts it again by
  itself 2 s later, dialling or answering as before. `ATH`, DTR, `ATZ`
  or `AT&L0` stop this until the next command line.
- At 48000 Hz and above (a bare pair driven by pico-serial's WB stick)
  the modem is always a leased-line modem, in its wideband mode V.wb
  ([dsp/vwb.md](dsp/vwb.md)).

## 2-wire: AT&L1

Every voice band modulation of `+MS` runs on `AT&L1`: V.34, V.32bis,
V.32, V.22bis, V.22, Bell 212A, V.23, V.21 and Bell 103, with V.8 and
automode as on a dialled call. Both directions share the pair, so the
echo cancellers of V.34 and V.32 do the work they do on a dialled line.

A circuit can change under a call (a reroute on a carrier's network, a
new echo path or delay). V.34 retrains when its receiver's error grows
(11.5/V.34). V.32 and V.32bis retrain too (5.5/V.32): when the
receiver's mean square error stays above its rate's limit for 2 s while
the far end's signal is still there, the receiver asks for a retrain
(the rate's least SNR less 4 dB, 7 dB at 4800). `DSP_V32_LOST=0` turns
this off.

## 4-wire: AT&L2

On a 4-wire circuit each end sends on its own pair, and the carrier can
stay on for the whole call. `AT&L2` with one of the 4-wire modulations
runs them as duplex data pumps on a continuous carrier (`dsp/dp_4wire.cpp`):

| `+MS` | Recommendation | Rates (bit/s) |
|---|---|---|
| `V29` (29) | V.29 | 9600, 7200, 4800 |
| `V33` (33) | V.33 | 14400, 12000 (trellis coded) |
| `V27TC` (27) | V.27ter | 4800, 2400 |

`V27TC` is V.250's name; `V29` and `V33` are D-Modem's. The other
modulations run on `AT&L2` as on `AT&L1`.

- **Start-up.** Each end sends its synchronizing signal at once (V.29
  10, V.33 9) and sends it again 1.2 s after its last if the far end's
  has not come. CONNECT comes when this end's receiver has trained on
  the far end's signal and its own signal has gone out.
- **Rates.** `+MS`'s max sets the rate an end sends at, its min the
  lowest it comes down to. Each receiver finds the far end's rate by
  itself (parallel receivers, the one with the least training error
  wins; V.33's two rates differ in their rate signal). An end that sends
  faster than the far end comes down to its rate.
- **Resynchronizing.** A receiver that loses the far end's signal or its
  equalization (its error above the rate's limit for 0.3 s) sends its
  own synchronizing signal, and the far end answers with its own. An
  end that loses its equalization twice within 20 s at its own rate
  takes the next rate down (V.33 14400 to 12000, V.29 9600 to 7200 to
  4800, V.27ter 4800 to 2400), never below `+MS`'s min.
- **Carrier loss.** S10 is the time without the far end's signal before
  NO CARRIER; the call then starts again by itself, as on `AT&L1`.

## V.91 on a 4-wire digital circuit: AT&L2+MS=V91

A digital leased circuit (a 64 kbit/s DS0 or E1 time slot, or a G.711
trunk between two PBXs) carries PCM codes each way unchanged. Between
two D-Modems on one, `AT&L2+MS=V91` runs V.91, the symmetric PCM
modem: up to 62667 bit/s both ways (about 58700 bit/s of data through
V.42). Both ends need a digital line at 8000 Hz (libdmodem with its
G.711 law, dmodemd on a PBX or SIP trunk, or the Asterisk and
FreeSWITCH modules); on an analogue line `+MS=V91` is V.34.

- **Start-up** (8.8/V.91). There is no V.8 and no Ez: each end sends
  INFO at once (8.2.1.2) and goes on sending it until the far end's
  comes, however late that end comes up. The rest is V.91's own
  start-up: INFO', the DIL, CP and B1. CONNECT comes when data mode is
  up both ways, the rate the same each way unless the circuit treats
  the two directions differently.
- **Rates.** As on a switched call ([dsp/v91.md](dsp/v91.md)): 62667
  on a clean circuit in either law; less where it robs bits (61333) or
  pads.
- **Retrains and carrier loss.** A receiver retrains (INFO again) when
  it decodes poorly or when the far end's signal is gone (the same
  value for 6 ms: a cut circuit's idle code). The far end answers its
  INFO, and LAPM sends again what was lost. With no INFO back within
  10 s the call ends with NO CARRIER and starts again by itself, as on
  `AT&L1`. There is no escape to V.34 (8.3/V.91 is for V.8's calls).

## Loop tests: AT&T (V.54, V.52)

| Command | Test |
|---|---|
| `AT&T0`, `AT&T` | end the test (after `&T7` or `&T8`: the bit errors, three digits, then OK) |
| `AT&T1` | loop 3, the local analogue loop |
| `AT&T8` | loop 3 with V.52's self-test |
| `AT&T3` | loop 2 here: the far end's data back to it |
| `AT&T4` / `AT&T5` | the far end may / may not ask for loop 2 (the default is `&T5`, again after `ATZ`) |
| `AT&T6` | loop 2 at the far end, the remote digital loop |
| `AT&T7` | loop 2 at the far end with V.52's self-test |
| S18 | ends a test after that many seconds (0: never) |

- **Loop 3** (`&T1`, `&T8`) is offline, on a leased line with a 4-wire
  modulation: the pump's signal goes back into its own receiver and the
  line is silent. `&T1` answers CONNECT, and the DTE's data comes back
  to it through the modulator and demodulator; `&T8` answers OK and
  stays in command state.
- **Loop 2** (`&T6`, `&T7`) is asked for in band from online command
  state (after `+++`), on a call with no error correction (`\N0`): V.54
  6's simplified signalling for point-to-point circuits, its patterns
  scrambled with 1 + x^-4 + x^-7 and sent at the data rate. This end
  sends PREP (2048 bits of scrambled 0s); the far end, if it allows it
  (`&T4`), sends ACK (1948 bits of scrambled 1s) and loops what it
  receives; this end starts the test 2148 bits after the ACK. `&T6`
  answers CONNECT and the DTE's data comes back to it; `&T7` answers OK.
  No ACK within 6144 bits after the PREP: ERROR. `AT&T0` sends TERM
  (8192 bits of scrambled 1s, then 64 1s), after which the far end
  leaves its loop; `ATO` then goes back to the call.
- **The self-test** (`&T7`, `&T8`) sends V.52's 511-bit pattern
  (x^9 + x^5 + 1) in place of the DTE's data. The checker synchronises
  on what comes back (32 bits in a row that follow the pattern), counts
  the bits in error, and synchronises again after a slip (64 errors in
  256 bits).

The recognition criteria are the DCE's own (V.54 5.1): a pattern is
there in a 256-bit block when at least 240 of its bits descramble to the
pattern's bit and the raw bits have a run of seven of the other value
(seven 1s keep HDLC flags from passing for PREP); PREP is recognised
after 4 such blocks in a row, ACK after 3, TERM after 16.

The code is `core/v54.h` and `core/v54.cpp`, between the link and the
data pump in `Modem::get_bits()` and `put_bits()`.

## Simulated carrier: S48 (V.13)

On a continuous carrier the far end's carrier detect never drops. With
`ATS48=1`, on a call with no error correction, V.13's simulated carrier
control does it in band:

- Between the DTE's bursts of data this end sends the idle pattern,
  scrambled 1s (1 + x^-3 + x^-7). Before each burst it sends the ON
  pattern, 8 scrambled 0s, then the data.
- The far end turns its circuit 109 (carrier detect) OFF after 56 bits
  of the idle pattern, and ON after the ON pattern. While 109 is OFF its
  DTE gets nothing.

A pty has no RTS, so circuit 105 follows the DTE's data: ON with its
first start bit, OFF after 64 bits with nothing to send (and at least
128 bits of the idle pattern before it comes ON again). The receiver's
bits reach the link 64 bits late, so that none of the idle pattern seen
before 109 goes OFF reaches the DTE. Without S48 on the far end, the
idle pattern reaches its DTE as characters. The code is `core/v13.h`
and `core/v13.cpp`.

## Tests

`dsp/test/leased_call_test.cpp` runs two of libdmodem's modems at 9600
Hz on a simulated circuit (each way's delay and loss, white noise, and
on a 2-wire one each end's own signal through its hybrid). It covers
each modulation on `AT&L1` and `AT&L2`, the 4-wire rates and fallback,
a dropout, noise that brings V.33 down, a late answerer, a long cut and
the call again, a reroute that V.34 and V.32bis retrain through, a burst
of noise, the loop tests and the self-test, and V.13's bursts and idle.
V.91 runs on a digital circuit instead (G.711 octets each way at 8000
Hz): both laws at 62667 both ways, robbed bits, a late answerer, a
short cut it retrains through and a long one it calls again after
([testing.md](testing.md)).
