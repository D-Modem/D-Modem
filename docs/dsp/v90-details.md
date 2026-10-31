# V.90 digital modem: technical details

This is the technical companion to [V.90: the digital modem](v90.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

## Overview

`dsp/v90.cpp` is the digital modem's PCM transmitter. `dsp/dp_v34.cpp`
runs its start-up on top of the V.34 pump.

The modem must run on an 8000 Hz line on a G.711 network: the pump only
offers V.90 (and V.92) as the digital modem when the line is at 8000 Hz
(`v90` in `V34Pump::start()`). dmodemd runs SIP and PBX lines at that
rate (`DMODEM_RATE`); in the tests the 8000 Hz build of the open modem
is `half_open8` (compiled with `MODEM_RATE=8000`).

The pump sends exact codewords, as their linear values, which the
network's encoder passes on unchanged. The G.711 law comes from the
call's codec; without one, it is µ-law, or A-law with `DSP_G711=alaw`.

Terms used on this page:

- **PCM**: here, the G.711 codes sent as exact values, rather than a
  modulated signal.
- **Ucode**: a PCM code number from 0 to 127.
- **DIL** (digital impairment learning): a sequence of known codes that
  shows the analogue modem what the network does to each code.
- **mse**: mean square error, a measure of a receiver's noise.

## The start-up

### Phases 1 and 2

- In V.8, CM or JM carries PCM availability with the digital modem's
  bit.
- Phase 2 is the V.34 *call modem's* part, whichever side called:
  INFO0d, tone B, probing, INFO1d.
- The analogue modem's INFO1a asks either for V.90 (symbol rate 6,
  with UINFO, the Ucode of TRN1d's two points) or for V.34.

### Phase 3

- **In:** the analogue modem's S, PP, TRN and Ja (Ja carries the DIL
  descriptor) go into the V.34 receiver.
- **Out:** Sd, TRN1d, Jd, Jd' and the DIL that Ja asked for go out as
  PCM. TRN1d lasts 1 s (`trn1d_len`, see
  [the Agere modem](#other-analogue-modems) below).
- While it takes in the DIL, the analogue modem is either silent or
  sends TRN1a. The S that ends the DIL is looked for after either.

### Phase 4

- **In:** CPt, CP and E.
- **Out:** Ri, TRN2d, MP, Ed and B1d go out through the encoder, first
  with the CPt constellations and then with the CP ones.

## Data mode

**Downstream** is the encoder of 5.4/V.90: the modulus encoder, the
mappers, the sign coding, and the spectral shaper with its look-ahead.
The shaper makes the exact choice of 5.4.5.5, because the analogue modem
checks TRN2d against it.

**Upstream** is the V.34 data mode receiver, with the trellis code and
the precoding that our MP asks for.

## The echo canceller

The echo canceller takes our PCM signal (converted to 9600 Hz) as its
reference. It trains while Sd, TRN1d and Jd go out, because the
analogue modem is meant to be silent then.

It stays off unless that training took at least 6 dB away
(`bypass` in `dp_v34.cpp`). On a 4-wire network there is no echo, and a
canceller trained on the remote's signal would only add noise.

With netsim's hybrid echo:

| Echo | Result |
|---|---|
| 20 dB down | connects at 56000/31200 |
| 12 dB down, 200 ms late | connects at 56000/31200 |
| 12 dB down, 20 ms late | fails at times: the echo of the DIL's loudest codes hides the analogue modem's S |

### The precoding coefficients in MP

Our MP's precoding coefficients are found once per exchange, from the
equaliser's error since the echo canceller took over. Before that point
our own echo colours the error, and the predictor reads a gain of 3 to
6 dB into it.

Every MP of the exchange asks for the same coefficients, and the data
mode receiver uses those. This matters because the HSF modem keeps what
it was asked for first. When our receiver worked out its own
coefficients at the start of the data mode (as it once did), its mse was
0.66 instead of 0.14.

### Against the HSF modem with echo

The test `hsf_v90_echo` puts the HSF modem's hybrid echo 20 dB down and
50 ms late (`NET_ECHO=-20:50`):

- The canceller takes about 30 dB away (the test accepts 20 to 39 dB).
- The call connects at 52000/33600, sometimes after a retrain.
- With the echo 10 dB down it also connects, with a data mode mse of
  0.18 against 0.14 without echo. The difference is the G.711
  quantisation of the echo together with the upstream signal.
- With the echo after 20 ms instead of 50, netsim left a sixth of the
  echo out, and the upstream renegotiated down to 28800 or lower. The
  echo delay has to be longer than the modem sends ahead of what it
  receives; see `NET_ECHO` in [testing](../testing-details.md).

The retrains are the HSF modem's own. After its S in phase 3 it goes
silent without sending the S-bar that answers our Jd', then sends
tone A. It takes no echo into itself; what changes is *when* we answer
it:

- **20 dB down:** our V.21 receiver gets the carrier some milliseconds
  later, with the echo of our ANSam on the line. It misses the first CM
  because it lacks all ten ones before the sync, so it sends JM 300 ms
  later. Accepting the sync after six ones would fix that. But the SM56
  modem (whose first CM we would then catch too) chooses 45333
  downstream instead of 50666 when JM comes one CM earlier, at every
  alignment tried. So the sync detector still wants all ten ones.
- **10 dB down:** the HSF modem retrains even with JM on time. What we
  send differs from a call without echo only by a millisecond in
  phase 2. The call connects after one or two retrains.

## Interop with analogue modems

### The Smart Link library

The original library, as an analogue V.90 modem calling through the
simulated network (`ctest -R smartlink_v90`), connects at 56000 bit/s
downstream and 31200 upstream, in µ-law and A-law, on a line without
noise.

Its phase 4 check works like this:

1. It compares the error of its equaliser output against the levels it
   expects for TRN2d's codes, over the first 7200 symbols of TRN2d.
2. In between, it replaces the levels it took from the DIL with those it
   measured in TRN2d.
3. It compares the error over the next 7200 symbols. If the second is
   more than about 1.5 times the first, it retrains (silence and
   tone A). The digital modem follows it back to phase 2 at the tones,
   as in a V.34 retrain.

On a line with no noise at all, the error is only a unit or two of its
16-bit scale. Its levels from the DIL are off by about that much (they
are 4 times the G.711 values of the codes, give or take 2). So with
µ-law the check fails every time: 1.96 against 1.23, and 1.78 on the
second training, which it lets pass (it retrains at most once). The
codes come through exactly as sent, so any digital modem would see the
same.

With a noise floor of −81 dBFS (netsim's `NET_FLOOR=3`, still far below
a real line's), the error is that of the noise. The ratio is then 1.2 to
1.3 at every alignment of the call in both laws, and the call connects
at 54666/31200 without the retrain, 15 s sooner. The µ-law V.90 tests
(`smartlink_v90_ulaw`, `smartlink_v90_reneg`,
`smartlink_v90_reneg_silence`) use that floor.

The original library's own debug output helped here (its `edprintf`
lines are hidden behind a key). It reports the levels it measures in the
DIL, the digital pad it concludes and why it retrains.

### Other analogue modems

The Agere modem as the analogue modem (`ctest -R agere_v90`) connects at
46666 bit/s downstream. It trains on TRN1d for 0.6 s and then decides on
it for another 0.9 s before it looks for Jd. That is why TRN1d lasts 1 s;
it used to be 300 ms, which was enough only for the original library.

### Over SIP

Through Asterisk and DModem (`asterisk/test/sip.sh AT+MS=90`, with
`CODEC=alaw` for A-law), the original library connected at 56000/31200
in both laws. That was while dmodemd (then slmodemd) still ran the
original library at 9600 Hz, and its SIP side did the 8000/9600 Hz
conversion itself. (pjmedia's resampler cuts below 4000 Hz, which takes
away the part of the PCM signal that the analogue modem recovers its
timing from.) dmodemd now runs at the call's 8000 Hz.

## Rate renegotiation

Rate renegotiation (9.6/V.90) changes the rates in the middle of a call
without a full retrain.

### The analogue modem's

The analogue modem sends S, S-bar, SCR and CP with the downstream rate
it wants. We answer its S-bar with:

1. **Rd**: + + + − − − of each interval's largest data mode Ucode, for
   384 symbols, on a data frame boundary; then **Rd-bar** (the opposite)
   for 24 symbols.
2. **TRN2d**.
3. **MP**, with the upstream rate our mse allows; then **MP'**.
4. **Ed** and **B1d**.

The encoder uses CPt's constellations until B1d.

### Ours

Ours start from the upstream mse, as in V.34, with Rd first.
`DSP_V34_RENEG=<ms>:<N>[a|c]` starts one in a test: `<ms>` into the
data mode, to N × 2400 bit/s.

The DTE can also ask for one with `ATO3` (and for a retrain with
`ATO1`); see `open_v90_dte_reneg` in `dsp/test/interop_test.cpp`.

### The silent period (CPs)

A CP with bit 30 set (CPs) asks for a silent period, so that the
analogue modem can retrain its echo canceller. We then send:

1. MP' until CPs' arrives, then Ed;
2. Ucode 0, keeping the data frames, until a CP with bit 30 clear
   arrives;
3. **Rt** (CPt's largest Ucodes), **Rt-bar** and MP.

If no E arrives within 5 s of Rd (plus the round trips), we retrain.

### Tests

| Test | What happens | Result |
|---|---|---|
| `smartlink_v90_reneg` | ours, 5 s into the data, upstream to 24000 | 56000/24000 |
| `smartlink_v90_reneg_silence` | noise on the PCM codes from 40 s on | the library renegotiates, with CPs and the silent period: 54666 or 50666 down, B1d without errors |
| `agere_v90_reneg_open` | ours, upstream to 19200 | the Agere modem goes along |

The original library renegotiates on its own when the downstream gets
worse, always with the silent period. With the noise still there it
now and then picks 56000 again, and its data then has errors. So the
test stops netsim's noise during our silence (`NET_NOISE_QUIET`).

The Agere modem renegotiates on its own as well, without the silent
period, a step down at a time as the noise rises (45333, 46666, 44000 in
one call). But it only does so after 40 s or so, if at all, so there is
no test for it.
