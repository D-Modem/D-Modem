# V.26 bis: 2400/1200 bit/s with a backward channel

This page describes ITU-T Recommendation V.26 bis (11/1988), "2400/1200
bits per second modem standardized for use in the general switched
telephone network". It is our own description. It is not a copy of the
Recommendation. For the normative text, refer to the Recommendation
(`docs/specs/fetch.sh V.26bis`). For the signal and the timing, refer to
[the technical details](v26bis-details.md).

## Summary

V.26 bis sends data at 2400 bit/s, or at 1200 bit/s on a poor line, in
one direction at a time. This is the forward channel. An optional
backward channel sends at 75 baud in the other direction, as in
[V.23](v23.md).

- The forward channel uses differential phase shift keying (DPSK) on an
  1800 Hz carrier at 1200 baud. At 2400 bit/s, each symbol carries two
  bits. At 1200 bit/s, each symbol carries one bit.
- V.26 bis has no scrambler.
- Before the data, the transmitter sends a synchronizing signal. This
  lets the receiver find the clock and the phase.

V.26 bis is a provisional standard. When the CCITT wrote it, many
international connections could not carry 2400 bit/s. The Recommendation
tells users to test a connection before they use the service.

## How a V.26 bis call operates in D-Modem

1. The call modem dials. The answer modem answers with the answer tone of
   [V.25](v25.md).
2. The answer modem sends the synchronizing signal on the forward
   channel for 1 s or more.
3. The call modem finds the synchronizing signal, and its rate. It sends
   the mark on the backward channel.
4. The answer modem sends data at 2400 bit/s or 1200 bit/s. The call
   modem sends data at 75 bit/s.

## Related standards

- [V.25](v25.md): the answer tone.
- V.26: the same modulation, on leased 4-wire lines. V.26 bis uses
  alternative B of V.26.
- [V.26 ter](v26ter.md): 2400 bit/s duplex with echo cancellation.
- [V.23](v23.md): the same backward channel.

## V.26 bis in D-Modem

`dsp/dp_v26bis.cpp` has the V.26 bis pump (`AT+MS=V26B`). It operates
in one direction only: the answer modem sends the forward channel, and
the call modem sends the backward channel. The two channels operate for
the full call. Thus, the line does not turn around. D-Modem does not
have the half duplex use of the forward channel.
