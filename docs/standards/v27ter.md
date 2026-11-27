# V.27 ter: 4800/2400 bit/s for fax

This page describes ITU-T Recommendation V.27 ter (11/1988), "4800/2400
bits per second modem standardized for use in the general switched
telephone network". It is our own description. It is not a copy of the
Recommendation. For the normative text, refer to the Recommendation
(`docs/specs/fetch.sh V.27ter`). For the signal, the training and the
timing, refer to [the technical details](v27ter-details.md).

## Summary

V.27 ter sends data at 4800 bit/s, or at 2400 bit/s on a poor line. It
sends in one direction at a time (half duplex). An optional backward
channel at 75 baud is the same as in [V.23](v23.md).

- At 4800 bit/s, the modem sends 1600 symbols each second. Each symbol is
  one of eight phase changes, and carries three bits.
- At 2400 bit/s, the modem sends 1200 symbols each second. Each symbol is
  one of four phase changes, and carries two bits.
- The carrier is 1800 Hz. A scrambler makes the data random.
- The receiver has an adaptive equalizer.

Before each transmission, the modem sends a training sequence. The
receiver uses it to find the carrier, the clock and the equalizer. A
long sequence starts a connection. A short sequence is for each
turn-around after that.

## Where V.27 ter is used

The main use of V.27 ter now is Group 3 fax. T.30 uses V.27 ter at
4800 bit/s and 2400 bit/s for the page data. It is the slowest data
modulation of fax. A fax machine falls back to it when the line cannot
carry [V.29](https://www.itu.int/rec/T-REC-V.29) or V.17.

## Related standards

- V.27: the 4800 bit/s modem for leased lines. V.27 ter uses its 8-phase
  line code and its scrambler.
- V.26: the 2400 bit/s modem. V.27 ter at 2400 bit/s uses alternative A
  of V.26.
- [V.23](v23.md): the backward channel.
- T.30 and T.4: the fax procedure and the page coding.

## V.27 ter in D-Modem

`dsp/fax_modems.cpp` has the V.27 ter transmitter and receiver. The fax
code uses them for the pages (`AT+FTM=24` and `AT+FTM=48` in Class 1).
`dsp/dp_4wire.cpp` also uses them as a data pump for 4-wire leased lines
(`AT+MS=V27TC`), with a continuous carrier.
