# Leased lines and dry pairs: technical details

This page gives the technical details for [Leased lines and dry pairs](leased.md), the plain-English overview. Read the overview first. This page has the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

A leased line (a private circuit, or your own dry copper pair) has no
exchange: no dial tone, no ring signal and no hook. D-Modem operates on
such a line with `AT&L1` (a 2-wire circuit) or `AT&L2` (a 4-wire
circuit). S47 keeps the setting (`AT&L0` is a dialled line).

    AT&L1S0=1        the answering end, once
    AT&L1            the calling end, then ATD

## How a call starts and ends

- **Calls.** `ATD` (with a number or with no number) calls immediately.
  Call progress operates only to listen for the answer tone. It does not
  wait for a dial tone, listen for busy, or dial a number.
- **Answers.** `ATA` answers. With S0 above 0, the modem answers
  automatically, with no ring signal, when it is idle.
- **Start again.** When the line ends a call (NO CARRIER), the modem
  starts the call again automatically 2 s later. It dials or answers as
  before. `ATH`, DTR, `ATZ` or `AT&L0` stop this until the next command
  line.
- **Wideband.** At 48000 Hz and higher (a bare pair with the WB stick of
  pico-serial), the modem is always a leased-line modem, in its wideband
  mode V.wb ([dsp/vwb.md](dsp/vwb.md)).

## 2-wire: AT&L1

All voice band modulations of `+MS` operate on `AT&L1`: V.34, V.32bis,
V.32, V.22bis, V.22, Bell 212A, V.23, V.21 and Bell 103, with V.8 and
automode as on a dialled call. The two directions share the pair. Thus,
the echo cancellers of V.34 and V.32 do the same work as on a dialled
line.

A circuit can change during a call. For example, a reroute on the
network of a carrier gives a new echo path or delay.

- **V.34** retrains when the error of its receiver increases (11.5/V.34).
- **V.32 and V.32bis** also retrain (5.5/V.32). The receiver asks for a
  retrain when its mean square error stays above the limit of its rate
  for 2 s while the signal of the far end is still there. The limit is
  the lowest SNR of the rate minus 4 dB (7 dB at 4800 bit/s).
  `DSP_V32_LOST=0` disables this.

## 4-wire: AT&L2

On a 4-wire circuit, each end sends on its own pair. The carrier can stay
on for the full call. `AT&L2` with one of the 4-wire modulations operates
them as duplex data pumps on a continuous carrier (`dsp/dp_4wire.cpp`):

| `+MS` | Recommendation | Rates (bit/s) |
|---|---|---|
| `V29` (29) | V.29 | 9600, 7200, 4800 |
| `V33` (33) | V.33 | 14400, 12000 (trellis coded) |
| `V27TC` (27) | V.27ter | 4800, 2400 |
| `V26` (26) | V.26 (alternative A) | 2400 |
| `B201` (201) | Bell 201 (V.26's alternative B) | 2400 |
| `B208` (208) | Bell 208 | 4800 |
| `B202` (202) | Bell 202 | 1200 (FSK) |

`V27TC` is the name from V.250. `V29`, `V33`, `V26` and the Bell names
are names of D-Modem. The other modulations operate on `AT&L2` as on
`AT&L1`.

- **V.26 and Bell 201** use the 2400 bit/s symbols of V.27ter (1200 baud,
  carrier 1800 Hz). The data is unscrambled, as those modems sent it.
  The phase changes of the dibits are those of the alternative A of V.26
  (0, 90, 180, 270 degrees) or B (45, 135, 225, 315: Bell 201). Their
  marks are phase reversals and their zeros are an unmodulated carrier.
  Thus, in the data, neither tells that the far end synchronizes. Only
  the loss of the carrier or of the equalization tells this.
- **Bell 208** is the 4800 bit/s signal of V.27ter (8-phase at 1600
  baud, scrambled with 1 + x^-6 + x^-7), which V.27 took from it, only at
  that rate.
- **Bell 202** operates on the FSK pump (`dsp/dp_fsk.cpp`): 1200 Hz mark,
  2200 Hz space, 1200 bit/s from each end on its own pair. Each end sends
  its mark immediately. The data starts when the mark of the far end is
  steady for 150 ms.
- The DPSK modulations start with the training of V.27ter as their
  synchronizing signal. For V.26 and Bell 201, which had no training,
  this is a design of D-Modem. The original training of Bell 208 is not
  that of V.27ter. Another D-Modem finds it, but an old modem of these
  types does not.

- **Start-up.** Each end sends its synchronizing signal immediately
  (10/V.29, 9/V.33). If the signal of the far end did not come, it sends
  it again 1.2 s after its last one. CONNECT comes when the receiver of
  this end has trained on the signal of the far end and its own signal
  has gone out.
- **Rates.** The highest rate of `+MS` sets the rate at which an end
  sends. Its lowest rate sets the lowest rate to which the end decreases.
  Each receiver finds the rate of the far end automatically:
  - V.29 and V.27ter have no rate signal. Parallel receivers try each
    rate of the family at the same time. The receiver with the smallest
    training error is selected.
  - V.33 sends its rate in its rate signal (segment 3, 8.3/V.33).

  If an end sends faster than the far end, it decreases to the rate of
  the far end. For V.33, this is the rule of the Recommendation (9/V.33).
  For V.29 and V.27ter, it is a rule of D-Modem.
- **Resynchronizing.** A receiver can lose the signal of the far end or
  its equalization (its error above the limit of the rate for 0.3 s).
  Then it sends its own synchronizing signal, and the far end answers
  with its own. During this time, the data of the DTE waits, and the
  received data is held at ones.
- **Fallback.** If an end loses its equalization two times in 20 s at its
  own rate, it selects the next lower rate: V.33 14400 to 12000, V.29
  9600 to 7200 to 4800, V.27ter 4800 to 2400. It never goes below the
  lowest rate of `+MS`.
- **Carrier loss.** S10 is the time without the signal of the far end
  before NO CARRIER. Then the call starts again automatically, as on
  `AT&L1`.

## V.91 on a 4-wire digital circuit: AT&L2+MS=V91

A digital leased circuit (a 64 kbit/s DS0 or E1 time slot, or a G.711
trunk between two PBXs) sends PCM codes in each direction with no
changes. Between two D-Modems on such a circuit, `AT&L2+MS=V91` operates
V.91, the symmetric PCM modem. This gives up to 62667 bit/s in the two
directions (about 58700 bit/s of data through V.42).

The two ends must have a digital line at 8000 Hz: libdmodem with its
G.711 law, dmodemd on a PBX or SIP trunk, or the Asterisk and FreeSWITCH
modules. On an analogue line, `+MS=V91` operates V.34.

- **Start-up** (8.8/V.91). There is no V.8 and no Ez. Each end sends INFO
  immediately (8.2.1.2). It continues to send INFO until the INFO of the
  far end comes, also if that end starts very late. The remaining part is
  the start-up of V.91: INFO', the DIL, CP and B1. CONNECT comes when data
  mode operates in the two directions. The rate is the same in each
  direction, unless the circuit operates the two directions differently.
- **Rates.** As on a switched call ([dsp/v91.md](dsp/v91.md)): 62667 on a
  clean circuit in the two laws. The rate is less where the circuit robs
  bits (61333) or has pads.
- **Retrains and carrier loss.** A receiver retrains (sends INFO again)
  when it decodes badly, or when the signal of the far end is gone (the
  same value for 6 ms: the idle code of a cut circuit). The far end
  answers its INFO, and LAPM sends again the data that was lost. If no
  INFO comes back in 10 s, the call ends with NO CARRIER and starts again
  automatically, as on `AT&L1`.
- **No V.34 escape.** There is no escape to V.34, because 8.3/V.91 is for
  calls that started with V.8.

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

Loop 3 (`&T1`, `&T8`) operates offline, on a leased line (`&L1` or `&L2`)
with a 4-wire modulation (`+MS=V29`, `V33`, `V27TC`, `V26`, `B201` or
`B208`). The signal of the pump goes back into its own receiver, and the
line is silent.

- `&T1` answers CONNECT. The data of the DTE comes back to it through the
  modulator and demodulator.
- `&T8` answers OK and stays in command state.

### Loop 2

The DTE asks for loop 2 (`&T6`, `&T7`, and `&T3` at the looping end) in
band from online command state (after `+++`), on a call with no error
correction (`\N0`). It uses the simplified signalling of V.54 clause 6
for point-to-point circuits. The patterns are scrambled with
1 + x^-4 + x^-7 and sent at the data rate.

1. This end sends PREP: 2048 bits of scrambled 0s.
2. If the far end allows it (`&T4`), it sends ACK (1948 bits of scrambled
   1s) and loops the data that it receives.
3. This end starts the test 2148 bits after it identifies the ACK. `&T6`
   answers CONNECT, and the data of the DTE comes back to it. `&T7`
   answers OK.
4. `AT&T0` sends TERM (8192 bits of scrambled 1s, then 64 1s). After
   TERM, the far end stops its loop. Then `ATO` goes back to the call.

If no ACK comes in 6144 bits after the PREP, the command gives ERROR. The
far end also stops its loop when the carrier is lost for 1 s.

### The self-test

The self-test (`&T7`, `&T8`) sends the 511-bit pattern of V.52
(x^9 + x^5 + 1) in place of the data of the DTE. The checker synchronises
on the data that comes back (32 bits in a row that follow the pattern).
It counts the bits in error. After a slip (64 errors in 256 bits), it
synchronises again.

### Recognising the patterns

V.54 5.1 lets the DCE set the recognition criteria. In D-Modem, a pattern
is present in a 256-bit block when:

- 240 or more of its bits descramble to the bit of the pattern, and
- the raw bits have a run of seven of the other value. (Seven 1s prevent
  the identification of HDLC flags as PREP.)

The modem identifies PREP after 4 such blocks in a row, ACK after 3 and
TERM after 16.

The code is `core/v54.h` and `core/v54.cpp`. It is between the link and
the data pump, in `Modem::get_bits()` and `put_bits()`.

### V.250's test commands (V.58)

V.250 6.7 has AT commands for the test objects of V.58. They operate the
same loops (`core/v58.cpp`):

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

The error rate test starts to count when the checker has synchronised. A
block has errors when one of its bits has an error. The pattern is only
the 511-bit pattern of V.52 (`pattern` 1). The 63-bit, 2047-bit and fixed
patterns of `+TTER` give ERROR.

## Simulated carrier: S48 (V.13)

On a continuous carrier, the carrier detect of the far end never goes
off. With `ATS48=1`, on a call with no error correction, the simulated
carrier control of V.13 signals it in band:

- Between the bursts of data of the DTE, this end sends the idle pattern:
  scrambled 1s (1 + x^-3 + x^-7). Before each burst, it sends the ON
  pattern, 8 scrambled 0s, and then the data.
- The far end sets its circuit 109 (carrier detect) to OFF after 56 bits
  of the idle pattern. It sets it to ON again after the ON pattern. While
  109 is OFF, its DTE gets nothing. If 109 stays OFF for 2048 bits with
  no idle pattern, it goes ON automatically.

A pty has no RTS line. Thus, circuit 105 (request to send) follows the
data of the DTE. It goes ON with the first start bit of the data. It goes
OFF after 64 bits with no data to send. A minimum of 128 bits of the idle
pattern go out before it goes ON again.

The bits of the receiver get to the link 64 bits late. Thus, none of the
idle pattern that comes before 109 goes OFF gets to the DTE. If S48 is
not set on the far end, the idle pattern gets to its DTE as characters.

The code is `core/v13.h` and `core/v13.cpp`.

## Tests

`dsp/test/leased_call_test.cpp` operates two modems of libdmodem at
9600 Hz on a simulated circuit. The circuit has the delay and loss of
each direction, white noise and, on a 2-wire circuit, the own signal of
each end through its hybrid.

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

V.91 operates on a digital circuit (G.711 octets in each direction at
8000 Hz):

| Test | What it checks |
|---|---|
| `FourWireV91.Symmetric` | both laws at 62667 both ways |
| `FourWireV91Line.RobbedBits` | robbed bits |
| `FourWireV91Line.LateAnswerer` | a late answerer |
| `FourWireV91Line.Cuts` | a short cut it retrains through, and a long one it calls again after |

For how to run them, refer to [testing.md](testing.md).
