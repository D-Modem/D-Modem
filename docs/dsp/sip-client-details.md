# The 56k client on a SIP line: technical details

This page gives the details of
[the 56k client on a SIP line](sip-client.md). The receivers are the same as
on modem hardware. Refer to [V.90 analogue modem](v90a-details.md),
[K56flex](k56flex-details.md) and [x2](x2-details.md).

## The digital line

A **digital line** is a line at 8000 Hz that is not analogue
(`Host::analog_line()` is false). SIP and the modules for telephone systems
give such a line. Its samples are the G.711 values of the network, on the
clock of the network.

The receivers of the client take the samples of the line through
`V90Interp` at 0.5 sample for each output. On a digital line, the samples
are at phase 0 of the symbols of the network, and the clock offset is 0 ppm.
This is the best phase (refer to
[8000 Hz lines](v90a-details.md#8000-hz-lines)). Thus, all of the clients
are on by default there:

| Item | Analogue line at 9600 Hz | Analogue line at 8000 Hz | Digital line |
|---|---|---|---|
| V.90 and V.92 client (`v90a`) | on (`DSP_V90A=0`: off) | only with `DSP_V90A=1` | on (`DSP_V90A=0`: off) |
| K56flex client (`k56c`) | on (`DSP_K56A=0`: off) | off | on (`DSP_K56A=0`: off) |
| x2 client (`x2c`) | on (`DSP_X2C=0`: off) | off | on (`DSP_X2C=0`: off) |
| V.90 server, V.91 | off | off | on |
| Quick connect of V.92 as the client | on | on | only with `DSP_V92A_QC=1` |

`V34Pump::start()` sets `digital_line`. On an 8000 Hz line,
`V34Pump::process()` gives the samples of the line to the receiver of the
client (`v90rx`, `k56rx` or `x2rx`), after the echo canceller at 8000 Hz.
The 8000 Hz to 9600 Hz converter of the pump cuts the band below 4000 Hz,
and thus does not supply these receivers.

## The two ends in V.8

On a digital line, the PCM bits of the CM and the JM of D-Modem come from
`AT+MS` and S117 (`SREG_PCM_END`, in `V34Pump::start()`):

| Modem | `AT+MS` | S117 | PCM bits |
|---|---|---|---|
| Caller | `90`, `92` | 0 or 2 | analogue |
| Caller | `90`, `92` | 1 | digital |
| Caller | `V91` | any | V.91 |
| Answerer | `90`, `92` | 0 | digital, analogue, V.91 |
| Answerer | `90`, `92` | 1 | digital |
| Answerer | `90`, `92` | 2 | analogue |
| Answerer | `V91` | any | V.91 |

The digital network access bit goes in the CM only with the digital bit
or the V.91 bit. The JM gives only the bits that are also in the CM.

Before this change, the caller also sent the digital bit and the V.91 bit.
A server that has V.91 then took the caller for a digital modem, and the
call failed (issue #176).

These rules give the result (`v8_v90()`, `v8_v90a()`, `v8_v91()`,
`v8_v90_both()`):

1. **V.91**, if the two modems offer it, and the remote modem is not only an
   analogue modem.
2. If not, **we are the digital modem** if the remote modem offers the
   analogue end.
3. If not, **we are the analogue modem** if the remote modem offers the
   digital end.
4. If the two modems offer the two ends, the **caller is the analogue
   modem**.

| Remote modem | Its PCM bits | Result |
|---|---|---|
| Server of a provider | digital (and V.91) | D-Modem is the client (V.92 or V.90) |
| Analogue 56k modem | analogue | D-Modem is the server |
| D-Modem that calls with `AT+MS=V91` | V.91 | V.91 |
| D-Modem that calls with S117=1 | digital | D-Modem is the client |
| D-Modem on modem hardware | analogue | D-Modem is the server |

### Quick connect

The quick connect of V.92 (QC1a) as the client is off on a digital line,
if `DSP_V92A_QC` does not set it. Without the quick connect, the start of a
call to a provider is about 1 s longer.

### K56flex and x2

The K56flex client and the x2 client use V.8 for V.34 only. On a digital
line, the modem also removes the digital modes from its CM:

- The K56flex client does this when the server acknowledges K56flex
  (V.8bis ACK(1)).
- The x2 client does this from the start of the call.

## V.92 PCM upstream at 8000 Hz

The PCM transmitter of V.92 (`V92ATx`, `patx`) makes one sample for each
symbol at 8000 Hz. Before this change, the pump called it in
`pump_tx_sample()`, at 9600 Hz, and sent the result through the converter.
Thus, the symbols went out at 9600 a second, and the digital modem did not
find Ja ("no Sd-bar after Ja").

Now, on an 8000 Hz line, `process()` calls `pam_sample()` once for each
sample of the line. Thus:

- The levels go out at the rate of the network, without the filter of the
  converter.
- The output of the converter is added to the levels. It is silent while the
  pump sends PCM upstream. But it still has the end of INFO1a in its
  filter, when the PCM upstream starts.

This repairs V.92 PCM upstream on analogue lines at 8000 Hz too (the
`open_v92a_8k` test).

## Lost packets

A SIP packet that does not arrive is silence in the jitter buffer of
dmodemd (refer to `frontend/dmodemd/sip.cpp`). Before this change, the
receivers trained on this silence, and the error of the data mode went up.
Then the V.92 client started a renegotiation, which failed. The call then
retrained to V.90 without PCM upstream. The x2 client and the K56flex
client stopped the call.

Now, the receivers hold during a gap:

- **Start of a gap.** The signal is 30 dB below its mean power, for one
  frame (6 symbols for V.90 and x2, 8 for K56flex).
- **Hold.** The receiver does not change its equaliser, its timing loop
  or its levels. It also does not add the errors to the error of the
  data mode, which starts the renegotiations and the retrains.
- **End.** The hold continues for the span of the equaliser (64 symbols)
  and one more frame after the gap.

The receiver of V.90 does this per frame (`V90Rx::gap_hold`). K56flex and
x2 do it per symbol (`PcmGap` in `pcm_eq.h`). The bits of the gap go to
V.42, which sends the bad frames again.

The digital modem of D-Modem sees the gap in the PCM upstream of V.92.
It then makes a fast parameter exchange, which keeps the speeds.

## Slips of the jitter buffer

The jitter buffer of dmodemd restarts when its backlog is more than 2 s,
when the source jumps in time, or when it has no samples (the clock of the
far end is slower). Each restart moves the output of the buffer by some
samples: a slip.

A slip of one sample moves the frames of the PCM signal by one symbol. The
receiver of the client cannot see this, because each sample is still a
correct value of the network. Its frames are then incorrect, and all of
the data is incorrect. The remote modem stops the call when V.42 cannot
send its frames.

Thus, a restart of the jitter buffer for the same source now moves the
output by a multiple of 24 samples (`Jitter::FRAME_SAMPLES`). The delay
increases by 23 samples (3 ms) or less. 24 is a multiple of the frame of
V.90, V.92, x2 and V.91 (6 symbols), of the robbed bits (6), and of the
frame of K56flex (8). After the slip, the frames are correct again. The
samples that were lost or added are a short gap (refer to
[Lost packets](#lost-packets)).

A slip in the network before dmodemd is not a multiple of 24. The client
cannot repair it. Real networks are on one clock, and such slips are rare.

## Tests

`netsim` has two items for these tests:

- `NET_SLIP=<s>:<samples>[:a|b]`: one slip on the digital line toward modem
  A (or B). A positive count adds that many samples of silence. A negative
  count removes samples.
- `NET_DROP=<s>:<ms>`: silence in the two directions (a lost packet).

`interop_call` has `ENV_A` and `ENV_B`. They set environment variables for
one modem only (for example, `ENV_B=DSP_V90A=0 DSP_V91=0` gives a digital
modem without V.91, as the server of a provider).

| Test | What it covers |
|---|---|
| `open_v92a_8k` | V.92 PCM upstream on an analogue line at 8000 Hz |
| `open_digital_v91` | two D-Modems on digital lines, `AT+MS=V91`: V.91 |
| `open_digital_auto` | a caller against an answerer with V.91 and the two ends: V.92 (#176) |
| `open_digital_s117_digital` | a caller with S117=1: the caller is the server |
| `open_digital_s117_analog` | an answerer with S117=2: no PCM, V.34 |
| `open_digital_v92a` | the V.92 client against a server only |
| `open_digital_v92a_alaw` | the same on an A-law network |
| `open_digital_v90a` | the V.90 client (`AT+MS=90`) |
| `open_digital_both` | two D-Modems without V.91: the caller is the client |
| `open_digital_k56flex` | the K56flex client |
| `open_digital_x2` | the x2 client |
| `open_digital_v92a_gap` | a lost packet in the data, no retrain |
| `open_digital_v92a_slip` | a restart of the jitter buffer (168 samples), no retrain |
| `open_digital_k56flex_gap` | a lost packet and a slip of 24 samples |
| `open_digital_x2_gap` | a lost packet and a slip of 168 samples |
