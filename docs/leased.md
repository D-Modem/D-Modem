# Leased lines and dry pairs

Most modems dial through a telephone exchange. A **leased line** is
different: it is a private circuit with no exchange at all, so there is
no dial tone, no ringing, and nothing to dial. A **dry pair** (a bare
pair of copper wires you own, run between two buildings for example)
works the same way. D-Modem can use both.

For the full technical description, see
[the technical details](leased-details.md).

## Getting started

Choose the kind of circuit:

- `AT&L1` for a **2-wire** circuit (one pair of wires, both directions
  share it, like an ordinary phone line).
- `AT&L2` for a **4-wire** circuit (two pairs, one for each direction).

Then set one end to answer and the other to call:

    AT&L1S0=1        the answering end, once
    AT&L1            the calling end, then ATD

On a leased line `ATD` connects straight away; the number does not
matter. If the connection drops, the modem reconnects by itself two
seconds later. `ATH`, `ATZ` or `AT&L0` stop it doing that.

## What runs on each kind of line

- **2-wire (`AT&L1`):** every ordinary modem standard D-Modem supports,
  from 300 bit/s up to V.34's 33600 bit/s. If the circuit changes in the
  middle of a call, V.34 and V.32 retrain to the new conditions.
- **4-wire (`AT&L2`):** as well as the above, the classic leased-line
  modems: V.29 (up to 9600 bit/s), V.33 (up to 14400 bit/s), V.27ter
  (up to 4800 bit/s), V.26 and Bell 201 (2400 bit/s), Bell 208 (4800
  bit/s) and Bell 202 (1200 bit/s). These keep their signal on for the
  whole call. If the line gets worse, V.29, V.33 and V.27ter step down
  to a slower speed by themselves. Two D-Modems connect with any of
  them; the old Bell and V.26 modems themselves start up differently,
  so D-Modem may not connect to one.
- **4-wire digital (`AT&L2+MS=V91`):** on a digital circuit, such as a
  64 kbit/s channel between two phone systems, two D-Modems can use V.91
  to reach about 62000 bit/s in both directions.
- **A bare pair with the pico-serial WB stick:** the modem uses its own
  much faster wideband mode instead. See [V.wb](dsp/vwb.md).

## Line tests

Leased lines are often checked with loop tests, where one end sends data
and it comes back to it. D-Modem has the standard ones (V.54 and V.52):

| Command | What it does |
|---|---|
| `AT&T1` | loop the signal back inside this modem |
| `AT&T6` | ask the far modem to send our data back |
| `AT&T7`, `AT&T8` | the same loops, sending a test pattern and counting errors |
| `AT&T4` / `AT&T5` | allow / refuse the far end's request for a loop (refuse is the default) |
| `AT&T0` | end the test (after a pattern test, it reports the error count) |

The same loops also answer to V.250's standard test commands
(`AT+TRDL`, `AT+TAL`, `AT+TLDL`), which add an error rate test you can
run on a loop that is already up (`AT+TTER`, its counts with
`AT+TNUM?`).

## Simulated carrier (S48)

On a 4-wire line the signal never stops, so the far computer never sees
the "carrier detect" light go off between bursts of data. Some equipment
needs that. `ATS48=1` turns on V.13's simulated carrier, which tells the
far end when data starts and stops, so its carrier detect goes on and
off as it would on an old half-duplex line. Both ends need it set.

## Testing it

A test program runs two modems on a simulated circuit and checks each
standard, the speed fallback, drop-outs, noise, the loop tests and the
simulated carrier. See [Building and testing](testing.md).
