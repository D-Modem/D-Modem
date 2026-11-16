# ISDN: the clear 64 kbit/s channel

ISDN is the digital telephone service that many offices and some homes
had from the late 1980s. Each ISDN channel (a "B-channel") carries
64 kbit/s. An ordinary voice call puts digitised audio in that channel,
and all the modem modes of D-Modem already operate on it. An ISDN **data call**
is different. It puts data directly into the channel, with no sound.
This gives a clean 64000 bit/s in each direction. This is faster and more reliable
than any analogue modem.

D-Modem can make and accept these data calls. For the full technical
description, refer to [the technical details](isdn-details.md).

## The basics

    AT+MS=CLEAR

This command tells the modem to use the clear channel. Then `ATD` and `ATA`
connect immediately, with no modem tones, and report
`CONNECT 64000`. This operates only on a digital line (SIP, or the digital channel
of a phone system). On an analogue line, the modem uses V.34
instead.

## Protocols on the channel

ISDN equipment used a protocol on top of the raw 64 kbit/s. `AT+B2`
selects the protocol. Thus, D-Modem can communicate with old ISDN terminal adapters and
software:

| Setting | Protocol | Used by |
|---|---|---|
| `AT+B2=0` (default) | X.75 | most European ISDN adapters and BBS software |
| `AT+B2=5` | synchronous PPP | internet connections over ISDN |
| `AT+B2=9` | V.120 | North American ISDN adapters |
| `AT+CBST=...` | V.110 | slower rates inside the 64 kbit/s, as GSM mobile data still uses |

The error correction of the modem (V.42) can also operate on the clear
channel.

## How an ISDN data call reaches D-Modem

- **Over SIP.** Internet telephony can carry an ISDN data call with a
  format that has the name CLEARMODE (RFC 4040). `dmodemd` offers it when it dials
  with `AT+MS=CLEAR`, and it answers calls that ask for it.
  `SIP_CLEARMODE=no` disables this. It also operates over encrypted SIP
  calls (SRTP).
- **With an ISDN card on Linux.** `dmodemd --misdn 0` operates a basic
  rate ISDN card (HFC-S, AVM Fritz!Card and others) through the
  mISDN drivers of Linux. We must still test this on a real card.
- **Through Asterisk.** An Asterisk phone system with an ISDN line can
  send data calls to the Asterisk module of D-Modem. A separate Asterisk
  module, `codec_clearmode`, lets Asterisk carry CLEARMODE over SIP.
- **Through libdmodem,** for programs that embed the modem.

## Old ISDN software: CAPI 2.0

Much ISDN software (fax servers, BBS programs, the ISDN
plugin of `pppd`) uses CAPI 2.0, the standard ISDN programming
interface. `dmodem-capid` makes D-Modem look like a CAPI 2.0 ISDN card.
Thus, that software operates without changes on any line that D-Modem has:

    dmodemd ...                              # makes /dev/ttyDM0
    dmodem-capid -l 6000 /dev/ttyDM0

Then, on the computer where the applications run, put this line in `/etc/capi20.conf`:

    REMOTE rcapi localhost 6000

## What is still to come

- V.42bis compression over X.75, and the selection of the number (MSN) to answer.
- A test of the mISDN support on a real ISDN card.
- Fax over CAPI, and a Windows version of the CAPI interface.
- An ISDN USB stick. Its design is in progress.
