# ISDN: the clear 64 kbit/s channel

ISDN is the digital telephone service that many offices and some homes
had from the late 1980s. Each ISDN channel (a "B-channel") carries
64 kbit/s. An ordinary voice call puts digitised audio in that channel,
and every D-Modem modem mode already works on that. An ISDN **data call**
is different: it puts data straight into the channel, with no sound at
all. That gives a clean 64000 bit/s each way, faster and more reliable
than any analogue modem.

D-Modem can make and take these data calls. For the full technical
description, see [the technical details](isdn-details.md).

## The basics

    AT+MS=CLEAR

This tells the modem to use the clear channel. `ATD` and `ATA` then
connect straight away, with no modem tones, and report
`CONNECT 64000`. It only works on a digital line (SIP, or a phone
system's digital channel); on an analogue line the modem uses V.34
instead.

## Protocols on the channel

ISDN equipment ran a protocol on top of the raw 64 kbit/s. `AT+B2`
picks which one, so D-Modem can talk to old ISDN terminal adapters and
software:

| Setting | Protocol | Used by |
|---|---|---|
| `AT+B2=0` (default) | X.75 | most European ISDN adapters and BBS software |
| `AT+B2=5` | synchronous PPP | internet connections over ISDN |
| `AT+B2=9` | V.120 | North American ISDN adapters |
| `AT+CBST=...` | V.110 | slower rates inside the 64 kbit/s, as GSM mobile data still uses |

The modem's own error correction (V.42) can also run on the clear
channel.

## How an ISDN data call reaches D-Modem

- **Over SIP.** Internet telephony can carry an ISDN data call using a
  format called CLEARMODE (RFC 4040). `dmodemd` offers it when it dials
  with `AT+MS=CLEAR`, and answers calls that ask for it.
  `SIP_CLEARMODE=no` turns this off.
- **With an ISDN card on Linux.** `dmodemd --misdn 0` drives a basic
  rate ISDN card (HFC-S, AVM Fritz!Card and others) through Linux's
  mISDN drivers. This still needs testing on a real card.
- **Through Asterisk.** An Asterisk phone system with an ISDN line can
  pass data calls to D-Modem's Asterisk module. A separate Asterisk
  module, `codec_clearmode`, lets Asterisk carry CLEARMODE over SIP.
- **Through libdmodem,** for programs that embed the modem.

## Old ISDN software: CAPI 2.0

A lot of ISDN software (fax servers, BBS programs, `pppd`'s ISDN
plugin) was written for CAPI 2.0, the standard ISDN programming
interface. `dmodem-capid` makes D-Modem look like a CAPI 2.0 ISDN card,
so that software runs unchanged on any line D-Modem has:

    dmodemd ...                              # makes /dev/ttyDM0
    dmodem-capid -l 6000 /dev/ttyDM0

and, where the applications run, in `/etc/capi20.conf`:

    REMOTE rcapi localhost 6000

## What is still to come

- V.42bis compression over X.75, and picking which number (MSN) to answer.
- A test of the mISDN support on a real ISDN card.
- Fax over CAPI, and a Windows version of the CAPI interface.
- An ISDN USB stick, which is being designed.
