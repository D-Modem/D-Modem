# V.90 digital modem: technical details

This page gives the technical details for [V.90: the digital modem](v90.md), the plain-English overview. Read the overview first. This page has the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

## Overview

`dsp/v90.cpp` is the PCM transmitter of the digital modem.
`dsp/dp_v34.cpp` does its start-up on the V.34 pump.

The modem must operate on an 8000 Hz line on a G.711 network. The pump
offers V.90 (and V.92) as the digital modem only when the line is at
8000 Hz (`v90` in `V34Pump::start()`). dmodemd operates SIP and PBX lines
at that rate (`DMODEM_RATE`). In the tests, the 8000 Hz build of the open
modem is `half_open8` (compiled with `MODEM_RATE=8000`).

The pump sends exact codewords, as their linear values. The encoder of
the network sends them on with no changes. The G.711 law comes from the
codec of the call. If there is no codec, the law is µ-law, or A-law with
`DSP_G711=alaw`.

This page uses these terms:

- **PCM**: on this page, the G.711 codes sent as exact values, not a
  modulated signal.
- **Ucode**: a PCM code number from 0 to 127.
- **DIL** (digital impairment learning): a sequence of known codes. It
  shows the analogue modem what the network does to each code.
- **mse**: mean square error, a measure of the noise of a receiver.

## The start-up

### Phases 1 and 2

- In V.8, CM or JM sends PCM availability with the bit of the digital
  modem.
- Phase 2 is the part of the V.34 *call modem*, on the side that called
  and also on the side that answered: INFO0d, tone B, probing, INFO1d.
- The INFO1a of the analogue modem asks for V.90 (symbol rate 6, with
  UINFO, the Ucode of the two points of TRN1d) or for V.34.

### Phase 3

- **In:** the S, PP, TRN and Ja of the analogue modem (Ja contains the
  DIL descriptor) go into the V.34 receiver.
- **Out:** Sd, TRN1d, Jd, Jd' and the DIL that Ja asked for go out as
  PCM. TRN1d is 1 s long (`trn1d_len`, refer to
  [the Agere modem](#other-analogue-modems) below).
- While it receives the DIL, the analogue modem is silent or sends TRN1a.
  The modem looks for the S that ends the DIL after the two cases.

### Phase 4

- **In:** CPt, CP and E.
- **Out:** Ri, TRN2d, MP, Ed and B1d go out through the encoder, first
  with the CPt constellations and then with the CP constellations.

## Data mode

**Downstream** is the encoder of 5.4/V.90: the modulus encoder, the
mappers, the sign coding, and the spectral shaper with its look-ahead.
The shaper makes the exact selection of 5.4.5.5, because the analogue
modem compares TRN2d with it.

**Upstream** is the V.34 data mode receiver, with the trellis code and
the precoding that our MP asks for.

## The echo canceller

The echo canceller uses our PCM signal (converted to 9600 Hz) as its
reference. It trains while Sd, TRN1d and Jd go out, because the analogue
modem must be silent at that time.

It stays disabled unless that training removed 6 dB or more (`bypass` in
`dp_v34.cpp`). On a 4-wire network there is no echo. A canceller trained
on the signal of the remote modem only adds noise.

With the hybrid echo of netsim:

| Echo | Result |
|---|---|
| 20 dB down | connects at 56000/31200 |
| 12 dB down, 200 ms late | connects at 56000/31200 |
| 12 dB down, 20 ms late | fails at times: the echo of the DIL's loudest codes hides the analogue modem's S |

### The precoding coefficients in MP

The modem calculates the precoding coefficients of our MP one time for
each exchange. It uses the error of the equaliser after the echo
canceller started. Before that point, our own echo changes the error, and
the predictor finds a gain of 3 to 6 dB in it.

Each MP of the exchange asks for the same coefficients, and the data mode
receiver uses those coefficients. This is important because the HSF modem
keeps the coefficients of the first request. Before, our receiver
calculated its own coefficients at the start of the data mode. Then its
mse was 0.66, not 0.14.

### Against the HSF modem with echo

The test `hsf_v90_echo` puts the hybrid echo of the HSF modem 20 dB down
and 50 ms late (`NET_ECHO=-20:50`):

- The canceller removes about 30 dB (the test accepts 20 to 39 dB).
- The call connects at 52000/33600, sometimes after a retrain.
- With the echo 10 dB down, it also connects, with a data mode mse of
  0.18. Without echo, the mse is 0.14. The cause of the difference is the
  G.711 quantisation of the echo together with the upstream signal.
- With the echo after 20 ms in place of 50, netsim did not include a
  sixth of the echo, and the upstream renegotiated down to 28800 or
  lower. The echo delay must be longer than the time that the modem sends
  before it receives. Refer to `NET_ECHO` in
  [testing](../testing-details.md).

The HSF modem causes the retrains. After its S in phase 3, it becomes
silent without the S-bar that answers our Jd'. Then it sends tone A. It
does not take echo into itself. The difference is *when* we answer it:

- **20 dB down:** our V.21 receiver gets the carrier some milliseconds
  later, with the echo of our ANSam on the line. It does not find the
  first CM, because it does not have all ten ones before the sync. Thus,
  it sends JM 300 ms later. If it accepted the sync after six ones, this
  problem would not occur. But the SM56 modem selects 45333 downstream in
  place of 50666 when JM comes one CM earlier, at all alignments that we
  tried. (With six ones, we would also find the first CM of the SM56
  modem.) Thus, the sync detector still must have all ten ones.
- **10 dB down:** the HSF modem retrains also when JM is on time. The
  only difference from a call without echo is a millisecond in phase 2.
  The call connects after one or two retrains.

## Interop with analogue modems

### The Smart Link library

The original library operates as an analogue V.90 modem that calls
through the simulated network (`ctest -R smartlink_v90`). It connects at
56000 bit/s downstream and 31200 upstream, in µ-law and A-law, on a line
without noise.

Its phase 4 check operates as follows:

1. It compares the error of its equaliser output with the levels that it
   expects for the codes of TRN2d, over the first 7200 symbols of TRN2d.
2. Then it replaces the levels that it got from the DIL with the levels
   that it measured in TRN2d.
3. It compares the error over the next 7200 symbols. If the second error
   is more than about 1.5 times the first, it retrains (silence and
   tone A). The digital modem follows it back to phase 2 at the tones, as
   in a V.34 retrain.

On a line with no noise, the error is only one or two units of its
16-bit scale. Its levels from the DIL have an error of about that size
(they are 4 times the G.711 values of the codes, plus or minus 2). Thus,
with µ-law the check fails each time: 1.96 against 1.23, and 1.78 on the
second training. It accepts the second training, because it retrains a
maximum of one time. The codes come through exactly as sent. Thus, all
digital modems get the same result.

With a noise floor of −81 dBFS (`NET_FLOOR=3` of netsim, still much below
the noise of a real line), the error is that of the noise. The ratio is
then 1.2 to 1.3 at all alignments of the call in the two laws. The call
connects at 54666/31200 without the retrain, 15 s faster. The µ-law V.90
tests (`smartlink_v90_ulaw`, `smartlink_v90_reneg`,
`smartlink_v90_reneg_silence`) use that floor.

The debug output of the original library helped here (a key hides its
`edprintf` lines). It gives the levels that it measures in the DIL, the
digital pad that it finds and the reason for a retrain.

### Other analogue modems

The Agere modem as the analogue modem (`ctest -R agere_v90`) connects at
46666 bit/s downstream. It trains on TRN1d for 0.6 s. Then it makes
decisions on it for 0.9 s more before it looks for Jd. This is why TRN1d
is 1 s long. Before, it was 300 ms, which was sufficient only for the
original library.

### Over SIP

Through Asterisk and DModem (`frontend/asterisk/test/sip.sh AT+MS=90`, with
`CODEC=alaw` for A-law), the original library connected at 56000/31200
in the two laws. At that time, dmodemd (then slmodemd) still operated the
original library at 9600 Hz, and its SIP side did the 8000/9600 Hz
conversion. (The resampler of pjmedia removes the signal below 4000 Hz.
This removes the part of the PCM signal from which the analogue modem
recovers its timing.) dmodemd now operates at the 8000 Hz of the call.

## Rate renegotiation

Rate renegotiation (9.6/V.90) changes the rates during a call without a
full retrain.

### The analogue modem's

The analogue modem sends S, S-bar, SCR and CP with the downstream rate
that it wants. We answer its S-bar with these items:

1. **Rd**: + + + − − − of the largest data mode Ucode of each interval,
   for 384 symbols, on a data frame boundary. Then **Rd-bar** (the
   opposite) for 24 symbols.
2. **TRN2d**.
3. **MP**, with the upstream rate that our mse allows. Then **MP'**.
4. **Ed** and **B1d**.

The encoder uses the constellations of CPt until B1d.

### Ours

Our renegotiations start from the upstream mse, as in V.34, with Rd
first. `DSP_V34_RENEG=<ms>:<N>[a|c]` starts one in a test: `<ms>` into
the data mode, to N × 2400 bit/s.

The DTE can also ask for one with `ATO3` (and for a retrain with `ATO1`).
Refer to `open_v90_dte_reneg` in `dsp/test/interop_test.cpp`.

### The silent period (CPs)

A CP with bit 30 set (CPs) asks for a silent period. During this period,
the analogue modem can retrain its echo canceller. We then send these
items:

1. MP' until CPs' arrives, then Ed.
2. Ucode 0, with the data frames, until a CP with bit 30 clear arrives.
3. **Rt** (the largest Ucodes of CPt), **Rt-bar** and MP.

If no E arrives in 5 s after Rd (plus the round trips), we retrain.

### Tests

| Test | What happens | Result |
|---|---|---|
| `smartlink_v90_reneg` | ours, 5 s into the data, upstream to 24000 | 56000/24000 |
| `smartlink_v90_reneg_silence` | noise on the PCM codes from 40 s on | the library renegotiates, with CPs and the silent period: 54666 or 50666 down, B1d without errors |
| `agere_v90_reneg_open` | ours, upstream to 19200 | the Agere modem accepts it |

The original library renegotiates automatically when the downstream
becomes worse, always with the silent period. If the noise is still
there, it sometimes selects 56000 again, and then its data has errors.
Thus, the test stops the noise of netsim during our silence
(`NET_NOISE_QUIET`).

The Agere modem also renegotiates automatically, without the silent
period. It decreases one step at a time as the noise increases (45333,
46666, 44000 in one call). But it does this only after about 40 s, or not
at all. Thus, there is no test for it.
