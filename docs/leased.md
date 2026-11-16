# Leased lines and dry pairs

Most modems dial through a telephone exchange. A **leased line** is
different. It is a private circuit with no exchange. Thus, it has no dial
tone, no ring signal, and no number to dial. A **dry pair** operates in
the same way. A dry pair is a bare pair of copper wires that you own, for
example between two buildings. D-Modem can use the two types.

For the full technical description, refer to
[the technical details](leased-details.md).

## Getting started

Select the type of circuit:

- `AT&L1` for a **2-wire** circuit (one pair of wires that the two
  directions share, as on a usual telephone line).
- `AT&L2` for a **4-wire** circuit (two pairs, one for each direction).

Then set one end to answer and the other end to call:

    AT&L1S0=1        the answering end, once
    AT&L1            the calling end, then ATD

On a leased line, `ATD` connects immediately. The number has no effect.
If the connection stops, the modem connects again automatically two
seconds later. To stop this, use `ATH`, `ATZ` or `AT&L0`.

## What operates on each type of line

- **2-wire (`AT&L1`):** all usual modem standards that D-Modem supports,
  from 300 bit/s up to the 33600 bit/s of V.34. If the circuit changes
  during a call, V.34 and V.32 retrain for the new conditions.
- **4-wire (`AT&L2`):** the standards above, and also the classic
  leased-line modems. These are V.29 (up to 9600 bit/s), V.33 (up to
  14400 bit/s), V.27ter (up to 4800 bit/s), V.26 and Bell 201 (2400
  bit/s), Bell 208 (4800 bit/s) and Bell 202 (1200 bit/s). These modems
  keep their signal on for the full call. If the line becomes worse,
  V.29, V.33 and V.27ter decrease to a slower speed automatically. Two
  D-Modems connect with all of these modems. But the old Bell and V.26
  modems start in a different way. Thus, D-Modem possibly does not
  connect to one of them.
- **4-wire digital (`AT&L2+MS=V91`):** on a digital circuit, for example a
  64 kbit/s channel between two telephone systems, two D-Modems can use
  V.91. This gives about 62000 bit/s in the two directions.
- **A bare pair with the pico-serial WB stick:** the modem uses its own
  wideband mode, which is much faster. Refer to [V.wb](dsp/vwb.md).

## Line tests

Loop tests are a usual check of leased lines. In a loop test, one end
sends data and the data comes back to it. D-Modem has the standard loop
tests (V.54 and V.52):

| Command | What it does |
|---|---|
| `AT&T1` | loop the signal back in this modem |
| `AT&T6` | ask the far modem to send our data back |
| `AT&T7`, `AT&T8` | the same loops, with a test pattern and a count of the errors |
| `AT&T4` / `AT&T5` | accept / refuse the request of the far end for a loop (refuse is the default) |
| `AT&T0` | stop the test (after a pattern test, it gives the error count) |

The same loops also operate with the standard test commands of V.250
(`AT+TRDL`, `AT+TAL`, `AT+TLDL`). These commands add an error rate test,
which you can do on a loop that is already started (`AT+TTER`, with its
counts in `AT+TNUM?`).

## Simulated carrier (S48)

On a 4-wire line, the signal does not stop. Thus, the far computer does
not see the "carrier detect" light go off between bursts of data. Some
equipment must have this. `ATS48=1` enables the simulated carrier of
V.13. This tells the far end when data starts and stops. Thus, its
carrier detect goes on and off, as on an old half-duplex line. Set it on
the two ends.

## Testing it

A test program operates two modems on a simulated circuit. It does a
check of each standard, the speed fallback, interruptions, noise, the
loop tests and the simulated carrier. Refer to
[Building and testing](testing.md).
