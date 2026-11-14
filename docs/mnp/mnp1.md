# MNP 1

MNP 1 is the first MNP class. It sends start-stop octets in frames, one
direction at a time (two-way alternate, or half duplex). It is for
modems with very small processors and little memory.

## What it does

- The frames are start-stop octets, as in MNP 2. Each frame starts with
  SYN DLE STX and ends with DLE ETX and a 16-bit FCS.
- Only one modem sends at a time. After a modem sends a frame, it waits
  for the acknowledgement before it sends the next frame. There is no
  window of frames (secondary source).
- The efficiency is about 70%. On a 2400 bit/s line the throughput is
  about 1690 bit/s (secondary source: Wikipedia).

## On the line

V.42 Annex A does not give framing mode 1. It gives only the modes 2 and
3, and it says that the responder answers with the lower framing mode.
The DeskPorte firmware shows how class 1 fits into the same LR
(see [firmware-findings.md](firmware-findings.md#f2-the-framing-mode-and-class-1)):

- An LR with parameter 2 = 1 asks for class 1.
- An LR with no parameter 2 is also class 1. An LR with no parameter 3
  (k) gets k = 1.
- The DeskPorte accepts class 1. It does not send an LD.
- In class 1 the DeskPorte waits before it sends a link frame, and
  after the LR exchange. These waits agree with a line that must turn
  around between the two directions.

We did not find the full data phase of class 1 in the firmware. For
example, we do not know if class 1 uses the LT and LA frames of V.42
Annex A without changes.

## D-Modem

D-Modem does not support MNP 1.

- `Mnp::rx_lr()` in `link/mnp.cpp` rejects an LR with framing mode 1.
  It sends an LD with reason 3.
- An LR with no parameter 2 is different in D-Modem and in the
  DeskPorte. D-Modem uses framing mode 2. The DeskPorte uses framing
  mode 1.

A modem that has only MNP 1 is very rare. Thus this gap is not
important for most calls.

## What is necessary to support it

1. Accept framing mode 1 in `Mnp::rx_lr()`.
2. Send the response LR with only the parameters that the initiator
   sent, as the DeskPorte does
   (see [firmware-findings.md](firmware-findings.md#f6-the-response-lr)).
3. Send one frame at a time, and wait for the acknowledgement.
4. Get a capture of a class 1 call from a real modem, to find the
   turnaround times.
