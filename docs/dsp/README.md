# The data pumps: D-Modem's signal processing

A modem turns data into sound and sound back into data. The part that
does this is called the **data pump**. Each modem standard (V.21, V.34,
V.90 and so on) is a different way of doing it, and each has its own
pump. D-Modem's pumps are all open source, written in C++, in `dsp/`.
They run on any processor.

They replace Smart Link's closed, 32-bit-only DSP library. They offer the
same programming interface that library had, so the rest of the modem
did not need to change. The original library is kept only as a test
partner.

For the programming interface, the tables of every pump and how the
receiver works inside, see [the technical details](dsp-details.md).

## The standards

| Standard | Top speed | What it is |
|---|---|---|
| Bell 103, V.21 | 300 bit/s | the earliest modems |
| V.23 | 1200 bit/s one way, 75 the other | used by Minitel and videotex |
| Bell 212A, V.22 | 1200 bit/s | |
| V.22bis | 2400 bit/s | |
| V.32, V.32bis | 9600, 14400 bit/s | |
| V.34 | 33600 bit/s | the fastest purely analogue modem |
| [V.90](v90.md) | 56000 bit/s down, 33600 up | "56k" modems |
| [V.92](v92.md) | 56000 down, up to 48000 up | V.90 plus faster start-up, call waiting and a faster upload |
| [V.91](v91.md) | about 62000 bit/s both ways | two modems that are both on digital lines |
| V.29, V.33, V.27ter | 9600, 14400, 4800 bit/s | classic modems for 4-wire [leased lines](../leased.md) |
| V.26, Bell 201, 202, 208 | 2400, 2400, 1200, 4800 bit/s | older 4-wire [leased line](../leased.md) modems |
| [V.cvm](vcvm.md) | 175 to 3500 bit/s | D-Modem's proposal for data through mobile phone calls |
| [V.wb](vwb.md) | about 149000 bit/s each way | D-Modem's own mode for a bare pair of wires |
| Text telephones | 45 to 300 bit/s | TTY (Baudot), EDT and V.18 automatic detection, for deaf and hard of hearing users |
| Contact ID | (DTMF digits) | alarm panels reporting to a monitoring centre; D-Modem can be either end |
| Fixed-line SMS | 1200 bit/s | text messages to and from home phones that have an SMS button, as some European phone companies offered; D-Modem can be the phone or the message centre |
| [V.8bis](v8bis.md) | (a start-up) | agreeing on the kind of call before V.8, the first part of voice and data together (V.70) |
| Modem relay (V.150.1) | the far modem's speed | data calls through a gateway that answers the far modem itself, sent as bytes over the internet (see [Modem relay](../v150.md)) |
| Clear channel | 64000 bit/s | a fully digital ISDN data call, with no modulation at all (see [ISDN](../isdn.md)) |

There are also pumps for **call progress** (dial tone, dialling, busy,
ringing, the answer tone), **caller ID**, **voice** (playing and
recording sound), and **fax** (see [Fax](fax.md)).

When a modem calls, the V.8 negotiation lets both ends agree on the best
standard they share. The page on [V.8 and V.34](v34.md) explains it.
[V.8bis](v8bis.md) can come before it, for the two ends to agree on the
kind of call (`AT+A8E=1,1,,1`).
K56flex, an older 56k standard from before V.90, is not supported yet;
what is known of it is on [its own page](k56flex.md).

## The two ends of a 56k call

V.90 and V.92 are lopsided. One end, the **digital modem**, sits on a
fully digital phone connection; in the 1990s that was the internet
provider's equipment. The other, the **analogue modem**, is the
customer's modem on an ordinary phone line. The digital end sends exact
digital values that the phone network turns into sound for the analogue
end, which is how it reaches 56000 bit/s.

D-Modem can be **either end**:

- as the digital modem on a SIP or phone-system line (see [V.90](v90.md));
- as the analogue modem on a real phone line, through modem hardware
  (see [V.90 analogue modem](v90a.md)).

## Testing

`ctest --test-dir build -L quick` runs every pump against itself through
a simulated line. See [Building and testing](../testing.md) and
[Interop against other soft modems](../interop.md).
