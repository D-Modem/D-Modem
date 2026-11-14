# MNP 3

MNP 3 sends the data phase in bit-oriented (HDLC) frames. There are no
start and stop bits on the line. It is framing mode 3 of V.42 Annex A.

## What it does

- The LR exchange uses start-stop octets, as in MNP 2.
- After the LR exchange, the data pump sends a synchronous bit stream.
  The frames have HDLC flags, zero insertion and the FCS of LAPM.
- Without the start and stop bits, there are 20% fewer bits on the line.
  On a 2400 bit/s line the throughput is near 2400 bit/s (secondary
  source: Wikipedia).

## On the line

V.42 Annex A.4 gives the frame:

- A flag (0x7E) starts and ends each frame.
- The transmitter adds a 0 bit after five 1 bits (zero insertion).
- The FCS is the 16-bit FCS of LAPM.
- The header and the information are the same as in MNP 2.

A modem that supports framing mode 3 must also support framing mode 2.
The responder answers with the lower framing mode of the two LRs.

## D-Modem

D-Modem supports MNP 3. It is the default.

- D-Modem's LR offers framing mode 3 (`Mnp::build_lr()` in
  `link/mnp.cpp`).
- The data phase uses the HDLC framer of LAPM (`Hdlc` in `link/hdlc.h`).
- All the interop tests with other soft modems use framing mode 3, with
  k = 8 and 256-octet LT frames. See
  [the technical details of the link layer](../link-details.md#with-the-peers).
