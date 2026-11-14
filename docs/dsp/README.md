# The data pumps: D-Modem's signal processing

A modem changes data into sound, and sound back into data. The part that
does this is the **data pump**. Each modem standard (V.21, V.34, V.90 and
other standards) is a different method to do this. Each standard has its
own pump. All of the pumps of D-Modem are open source, in C++, in `dsp/`.
They operate on all processors.

They replace the closed, 32-bit-only DSP library of Smart Link. They have
the same programming interface as that library. Thus, it was not
necessary to change the remaining parts of the modem. We keep the original
library only as a test partner.

For the programming interface, the tables of each pump and the internal
operation of the receiver, refer to [the technical details](dsp-details.md).

## The standards

| Standard | Top speed | What it is |
|---|---|---|
| Bell 103, V.21 | 300 bit/s | the first modems |
| V.23 | 1200 bit/s one way, 75 the other | used by Minitel and videotex |
| Bell 212A, V.22 | 1200 bit/s | with a [fast connect](fastpos.md) for card terminals |
| V.22bis | 2400 bit/s | |
| [V.29 FastPOS](fastpos.md) | 9600 bit/s, one way at a time | card terminals and vending machines that call the host of their bank |
| [V.26 ter](v26.md) | 2400 bit/s | duplex on a dial-up line with echo cancellation, mostly in Europe |
| [V.26 bis](v26.md) | 2400 bit/s one way, 75 the other | the older dial-up version, which operates as V.23 does |
| V.32, V.32bis | 9600, 14400 bit/s | |
| [V.32terbo](v32terbo.md) | 19200 bit/s | the unpublished extension of V.32bis by AT&T, made again from a modem that has it |
| [HST](hst.md) | 16800 bit/s one way, 300 the other | the protocol of USRobotics for bulletin boards, made again from the DSP firmware of the Courier |
| [PEP](pep.md) | about 20000 bit/s, one way at a time | the TrailBlazer protocol of Telebit with hundreds of carriers, made again from its patents |
| [V.61](v61.md) | 4800 bit/s with speech, 14400 without | speech and data at the same time, with the voice added onto the carrier of the modem (ASVD) |
| [V.FC](vfc.md) | 28800 bit/s | the protocol of Rockwell before V.34. Its data mode is made again from the DSP firmware of the Courier |
| V.34 | 33600 bit/s | the fastest modem that is only analogue |
| [V.90](v90.md) | 56000 bit/s down, 33600 up | "56k" modems |
| [V.92](v92.md) | 56000 down, up to 48000 up | V.90 with a faster start-up, call waiting and a faster upload |
| [V.91](v91.md) | about 62000 bit/s both ways | two modems that are both on digital lines |
| V.29, V.33, V.27ter | 9600, 14400, 4800 bit/s | classic modems for 4-wire [leased lines](../leased.md) |
| V.26, Bell 201, 202, 208 | 2400, 2400, 1200, 4800 bit/s | older 4-wire [leased line](../leased.md) modems |
| [V.cvm](vcvm.md) | 175 to 3500 bit/s | the proposal of D-Modem for data through mobile telephone calls |
| [V.wb](vwb.md) | about 149000 bit/s each way | the own mode of D-Modem for a bare pair of wires |
| Text telephones | 45 to 1200 bit/s | TTY (Baudot), DTMF, EDT, Bell 103, V.23 (Minitel), V.21 and the own mode of V.18, with V.18 automatic detection, for deaf users and users who are hard of hearing |
| Contact ID | (DTMF digits) | alarm panels that send reports to a monitoring centre. D-Modem can be the two ends |
| Fixed-line SMS | 1200 bit/s | text messages to and from home telephones that have an SMS button, which some European telephone companies offered. D-Modem can be the telephone or the message centre |
| [V.8bis](v8bis.md) | (a start-up) | agreement on the type of call before V.8. This is the first part of voice and data together (V.70, V.61) |
| Modem relay (V.150.1) | the far modem's speed | data calls through a gateway that answers the far modem, sent as bytes through the internet (refer to [Modem relay](../v150.md)) |
| Clear channel | 64000 bit/s | a fully digital ISDN data call, with no modulation (refer to [ISDN](../isdn.md)) |

There are also pumps for **call progress** (dial tone, dialling, busy,
ring signal, the answer tone), **caller ID**, **voice** (play and record
sound), and **fax** (refer to [Fax](fax.md)).

When a modem calls, the V.8 negotiation lets the two ends agree on the
best standard that they share. The page on [V.8 and V.34](v34.md) gives
a description of it. [V.8bis](v8bis.md) can occur before it. With
V.8bis, the two ends agree on the type of call (`AT+A8E=1,1,,1`).

K56flex is an older 56k standard from before V.90. It operates at the two
ends. D-Modem answers a K56flex modem with it, and calls a K56flex server
with it. Refer to [its own page](k56flex.md).

x2 of USRobotics is the other early 56k standard. The work on it is not
complete. D-Modem answers an x2 modem as the server, and the start-up
completes. But the x2 modem does not accept the data now. Refer to
[its own page](x2.md).

## The two ends of a 56k call

V.90 and V.92 are not symmetrical. One end, the **digital modem**, is on a
fully digital telephone connection. In the 1990s, this was the equipment
of the internet provider. The other end, the **analogue modem**, is the
modem of the customer on a usual telephone line. The digital end sends
exact digital values. The telephone network changes these values into
sound for the analogue end. This is how the connection gets 56000 bit/s.

D-Modem can be **the two ends**:

- The digital modem on a SIP line or on the line of a telephone system
  (refer to [V.90](v90.md)).
- The analogue modem on a real telephone line, through modem hardware
  (refer to [V.90 analogue modem](v90a.md)).

## Testing

`ctest --test-dir build -L quick` tests each pump against itself through
a simulated line. Refer to [Building and testing](../testing.md) and
[Interop against other soft modems](../interop.md).
