# T.38: technical details

This page gives the IFP packets, their coding, UDPTL, the data rate
management and the SDP parameters of T.38 (11/2015). For a summary,
refer to [T.38](t38.md).

## The IFP packet (7.2, Annex A)

An IFP packet has a type, and for data a list of fields.

| Item | Values |
|---|---|
| Type | `t30-indicator` (Table 3) or `t30-data` (Table 4) |
| Indicators | no-signal, CNG, CED, V.21 preamble, V.27 ter 2400/4800 training, V.29 7200/9600 training, V.17 7200 to 14400 short and long training. Version 1 and later: V.8 ANSam, V.8 signal, V.34 control channel, V.34 primary channel, V.34 CC retrain, V.33 12000/14400. |
| Data types | V.21, V.27 ter 2400/4800, V.29 7200/9600, V.17 7200 to 14400. Version 1 and later: V.8, V.34 primary rate, V.34 CC 1200, V.34 primary channel, V.33. |
| Field types (Table 5) | hdlc-data, hdlc-sig-end, hdlc-fcs-OK, hdlc-fcs-BAD, hdlc-fcs-OK-sig-end, hdlc-fcs-BAD-sig-end, t4-non-ecm-data, t4-non-ecm-sig-end. Version 1 and later: cm-message, jm-message, ci-message, v34rate. |

## The coding (Annex A, ASN.1 aligned PER)

| Item | Coding |
|---|---|
| First octet | Bit 7: a data field follows. Bit 6: t30-data, else t30-indicator. Bit 5: an extension value. Bits 4 to 1: the value. |
| Extension value | Bit 4 is 0, then 6 bits: bits 3 to 0, and bits 7 and 6 of the next octet |
| Field count | A length determinant (one octet below 128) |
| Field, version 0 | Bit 7: data follows. Bits 6 to 4: the type. A field without data leaves the low half of the octet to the next field. |
| Field, version 1+ | Bit 7: data follows. Bit 6: an extension. Bits 5 to 3: the type. |
| Field data | The length less 1 in two octets, then the octets |
| Bit order (7.1.1) | The first bit on the line is the MSB of each octet |

## The data (7.4, 7.5)

- An HDLC frame goes without flags, bit stuffing and FCS. A field
  hdlc-fcs-OK (or hdlc-fcs-BAD) ends each frame.
  hdlc-fcs-OK-sig-end ends the last frame of the signal.
- A V.21 packet has 7 octets of frame data at most. A longer frame goes
  in more packets.
- T.4 data (and TCF on UDP) goes as t4-non-ecm-data, with
  t4-non-ecm-sig-end at the end.

## The data rate management (8)

| Method | Use |
|---|---|
| Method 1 | The receiving gateway makes the TCF. Used for TCP, optional for UDP. |
| Method 2 | The TCF goes through to the far fax machine. Mandatory for UDP. |

## UDPTL (9.1, Annex A)

| Item | Coding |
|---|---|
| Sequence number | 2 octets, 0 to 65535, one more for each packet |
| Primary IFP packet | A length, then the IFP packet |
| Error recovery | A choice bit: 0 secondary IFP packets, 1 FEC |
| Secondary packets | A count, then each packet before with its length, newest first. The sequence numbers are contiguous. |

A gateway need not send redundant packets. A receiving gateway can
ignore them.

## The SDP parameters (Annex D)

| Parameter | Meaning |
|---|---|
| `T38FaxVersion` | The version. The answer gives the same or a lower version. |
| `T38MaxBitRate` | The fastest modulation |
| `T38FaxRateManagement` | `localTCF` (method 1) or `transferredTCF` (method 2) |
| `T38FaxMaxBuffer`, `T38FaxMaxDatagram` | The buffer and the largest UDPTL packet of the end that sends the parameter |
| `T38FaxUdpEC` | `t38UDPNoEC`, `t38UDPRedundancy` or `t38UDPFEC`. If the answerer supports the offered value, it gives the same value in the answer. |

## T.38 in D-Modem

| Item | D-Modem |
|---|---|
| Coding (`dsp/t38.cpp`) | IFP packets in version 0 and in versions 1 to 3. UDPTL with redundancy. |
| Terminals | The T.30 machine (`dsp/t30.cpp`), Class 1 (`dsp/fax_class1.cpp`) and Class 2 (`dsp/fax_class2.cpp`). Each sends the signals as packets at the speed of the modulation. |
| V.21 packets | 7 octets of frame data at most |
| TCF | Method 2 (`transferredTCF`) |
| SDP (`frontend/dmodemd/sip.cpp`, `frontend/ari/sip.cpp`) | Version 0 to 3, `T38MaxBitRate:14400`, `T38FaxMaxDatagram:400`. `T38FaxUdpEC`: the offered value for `t38UDPNoEC` and `t38UDPRedundancy`, else `t38UDPRedundancy`. |
| Redundancy | Sent when the far end gives `t38UDPRedundancy` or `t38UDPFEC`. Received and used to recover lost packets. |
| Not implemented | Sending FEC. A received FEC packet gives only its primary packet. V.34 over T.38. TCP and RTP transport. |

The tests `T38Ifp.*`, `T38Udptl.*` and `T38Pair.*`
(`dsp/test/t38_test.cpp`) check the coding and send fax calls over
T.38. The tests in `dsp/test/class1_t38_test.cpp` and
`dsp/test/t38_spandsp.cpp` send calls between D-Modem and its own
Class 1, and between D-Modem and spandsp. The CI sends T.38 calls
through Asterisk and FreeSWITCH.

## Corrections (2026-11)

| Error | Correction |
|---|---|
| An answer to a `t38UDPNoEC` offer gave `t38UDPRedundancy` (D.2.3: the same value, when the answerer supports it) | The answer gives `t38UDPNoEC` |
| For a far end with `t38UDPFEC`, D-Modem sent no redundancy, but its answer said `t38UDPRedundancy` | It sends redundancy (Table D.2: either scheme) |

The test `T38Sdp.UdpEc` (`dsp/test/t38_test.cpp`) checks these
corrections.
