# V.32 bis: 14400 bit/s duplex on the telephone network

This page describes ITU-T Recommendation V.32 bis (02/91), "A duplex
modem operating at data signalling rates of up to 14 400 bit/s for use
on the general switched telephone network and on leased point-to-point
2-wire telephone-type circuits". It is our own description. It is not a
copy of the Recommendation. For the normative text, refer to the
Recommendation (`docs/specs/fetch.sh V.32bis`). For the codes, the rate
signals and the rate renegotiation, refer to [the technical
details](v32bis-details.md).

## Summary

V.32 bis is [V.32](v32.md) with more rates. It sends data at 14400,
12000, 9600, 7200 or 4800 bit/s in the two directions at the same time.

- The carrier, the symbol rate, the scramblers and the echo cancellation
  are the same as in V.32.
- All the rates above 4800 bit/s use the trellis code of V.32. At 14400
  bit/s, each symbol carries six bits on 128 points.
- The start-up and the retrain are the same as in V.32. The rate words
  have more bits for the new rates.
- A rate renegotiation changes the rate during the data, without a
  retrain. It takes a short preamble and a rate signal from each modem.

## Where V.32 bis is used

V.32 bis was the standard 14400 bit/s modem before V.34. A modern modem
uses it after V.8, when the far modem does not have V.34. [V.17](v17.md) (fax) and
[V.33](v33.md) (leased lines) use its trellis code and its points.

## Related standards

- [V.32](v32.md): the base of V.32 bis. A V.32 bis modem operates with a
  V.32 modem at the rates of V.32.
- [V.25](v25.md): the answer tone before the start-up.
- [V.22 bis](v22bis.md): Annex A gives an automode start-up that finds
  V.22 bis and V.22 modems.
- [V.17](v17.md) and [V.33](v33.md): the same trellis code and points.
- V.54: the test loops.

## V.32 bis in D-Modem

`dsp/dp_v32.cpp` has the V.32 bis data pump (`AT+MS=132`). It also has
V.32terbo of AT&T (16800 and 19200 bit/s) and the ZyX modes of ZyXEL, on
the same start-up. The pump starts after V.8 when the far modem does not
have V.34. MNP 10, `ATO3` and V.80 use the rate renegotiation.
