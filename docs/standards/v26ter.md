# V.26 ter: 2400 bit/s duplex with echo cancellation

This page describes ITU-T Recommendation V.26 ter (11/1988), "2400 bits
per second duplex modem using the echo cancellation technique
standardized for use on the general switched telephone network and on
point-to-point 2-wire leased telephone-type circuits". It is our own
description. It is not a copy of the Recommendation. For the normative
text, refer to the Recommendation (`docs/specs/fetch.sh V.26ter`). For
the signal, the start-up and the timing, refer to
[the technical details](v26ter-details.md).

## Summary

V.26 ter sends data at 2400 bit/s or 1200 bit/s, in the two directions at
the same time, on a 2-wire line.

[V.22 bis](v22bis.md) gives each direction its own band of frequencies.
V.26 ter does not do this. The two modems send on the same 1800 Hz
carrier. Each modem removes the echo of its own signal from the received
signal with an echo canceller. Thus, each direction can use the full
band of the line.

- The modulation is differential phase shift keying (DPSK) at 1200 baud.
- At 2400 bit/s, each symbol carries two bits. At 1200 bit/s, each
  symbol carries one bit.
- A scrambler makes the data random. The call modem and the answer modem
  use different scramblers.

## How a V.26 ter call starts

The start-up has three sequences:

1. **Sequence A.** The answer modem sends the answer tone of
   [V.25](v25.md).
2. **Sequence B.** The modems select the rate. Each modem sends a rate
   pattern at 1200 bit/s, one modem after the other (half duplex). The
   answer modem then sends 2100 Hz for 500 ms. This disables the echo
   suppressors of the network.
3. **Sequence C.** Each modem trains its echo canceller on its own
   signal, while the other modem is silent. Then the two modems send
   scrambled bits at the same time, and go to data.

## Why V.26 ter is important

V.26 ter was the first ITU modem that used echo cancellation on the
switched network. [V.32](https://www.itu.int/rec/T-REC-V.32) uses the
same technique at higher rates.

## Related standards

- [V.25](v25.md): the answer tone.
- [V.22 bis](v22bis.md): 2400 bit/s duplex with two bands.
- [V.26 bis](v26bis.md): 2400 bit/s in one direction.
- V.54: the test loops.

## V.26 ter in D-Modem

`dsp/dp_v26.cpp` has the V.26 ter pump (`AT+MS=V26T`), with the full
duplex start-up and the two rates. D-Modem does not have the optional
half duplex mode of clause 7, or the test loops of clause 8.
