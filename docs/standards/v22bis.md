# V.22 bis: 2400 bit/s duplex QAM

This page describes ITU-T Recommendation V.22 bis (11/1988), "2400 bits
per second duplex modem using the frequency division technique
standardized for use on the general switched telephone network and on
point-to-point 2-wire leased telephone-type circuits". It is our own
description. It is not a copy of the Recommendation. For the normative
text, refer to the Recommendation (`docs/specs/fetch.sh V.22bis`). For
the signal, the handshake and the timing, refer to
[the technical details](v22bis-details.md).

## Summary

V.22 bis sends data at 2400 bit/s in the two directions at the same
time. It uses the same two channels and the same 600 baud as
[V.22](v22.md). To send twice the data, each symbol carries four bits
(a quadbit) in place of two. The modulation is 16-point quadrature
amplitude modulation (QAM):

- The first two bits change the quadrant of the point, as in V.22.
- The last two bits select one of four points in the new quadrant.

A V.22 bis modem has an adaptive equalizer, because the 16 points are
closer together than the 4 points of V.22. It also operates at
1200 bit/s, with the same signal as V.22. Thus, a V.22 bis modem can
connect to a V.22 modem.

## How a V.22 bis call starts

The start is the same as V.22, with one more signal, S1. S1 is a pattern
of unscrambled dibits 00 and 11 for 100 ms. It tells the far modem "I
can do 2400 bit/s".

1. The answer modem sends the answer tone and then unscrambled ONE bits.
2. The call modem sends S1, then scrambled ONE bits at 1200 bit/s.
3. The answer modem detects S1. At the end of S1, it sends its own S1.
4. 600 ms after the end of S1, each modem sends scrambled ONE bits at
   2400 bit/s.
5. After 200 ms, the modems are ready to send data.

If one modem does not send S1, the two modems use V.22 at 1200 bit/s.

## Retrain and rate change

If the equalizer loses the signal, a modem can start a retrain. It sends
S1, then scrambled ONE bits at 1200 bit/s and then at 2400 bit/s. The
far modem replies in the same way. An optional procedure lets the modems
change between 2400 bit/s and 1200 bit/s during the call.

## Related standards

- [V.22](v22.md): the 1200 bit/s mode and its handshake.
- [V.25](v25.md): the answer tone.
- V.14: start-stop characters.
- [V.32 bis](https://www.itu.int/rec/T-REC-V.32bis): Annex A of V.32 bis
  starts V.22 bis when the far modem does not answer as V.32.

## V.22 bis in D-Modem

`dsp/dp_v22.cpp` has the V.22 bis pump (`AT+MS=V22B`). If a V.8 call
gets no V.8 answer, D-Modem tries V.32 bis and then V.22 bis.
