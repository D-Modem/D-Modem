# MNP 6

MNP 6 adds two functions for the 9600 bit/s modems of the late 1980s:
Universal Link Negotiation (ULN) and statistical duplexing. ULN starts
each call at 2400 bit/s and then moves the call to a faster data pump.
Statistical duplexing makes a half duplex V.29 data pump look like a
two-way link.

No specification is public. The facts on this page come from the
firmware of a Microcom AX/9624c, "MNP Class 6 Modem Rev 1.6" (refer to
[The firmware](#the-firmware)). A statement that comes only from a
secondary source has the mark "(secondary source)". A statement that we
think is correct, but did not prove in the code, has the mark
"(probable)".

## Summary

1. The two modems connect at 2400 bit/s (V.22bis).
2. They do the MNP LR exchange. LR parameter 0x0A offers the 9600 bit/s
   functions.
3. If the two modems agree on one or more functions, they both change
   to a V.29 data pump. The link stays up.
4. On V.29, the modems send in turns. Each modem sends a burst of
   frames, stops its carrier, and listens for the burst of the other
   modem.
5. The size of a burst changes with the quantity of data and with the
   errors on the line. Thus, the modem with more data gets more of the
   line time. This is statistical duplexing.

## The firmware

| Item | Value |
|---|---|
| Modem | Microcom AX/9624c. The same ROM also has the name AX/9612c. |
| ROM | 27C256, label "0095001 REV 1.6", 32768 octets |
| File | `firmware/microcom/ax-9624c/0095001-rev1.6-27c256.bin` in the project files |
| SHA-256 | `57b58e69b2c082cb51351f064c91b156e86303ee80ce88245c0fc1b099b0d601` |
| CPU | NEC D70008 (Z80). CPU address = file offset. |

The addresses on this page are CPU addresses in this ROM. No firmware
code is copied here. This page gives only the behavior.

### Clock and timers

- The system tick is CTC channel 3, time constant 240, prescaler 256
  (offset 0x0185). With a 4.9152 MHz CPU clock, the tick is 12.5 ms
  (probable). The MNP timers of the DeskPorte also use 12.5 ms.
- The turnaround state machine also uses CTC channel 2 as a one-shot
  timer, with prescaler 256. One count is 52.08 µs at 4.9152 MHz
  (probable).
- The times on this page use these values. The raw counts are also
  given.

## Universal Link Negotiation

### LR parameter 0x0A

The originator's LR has parameter 0x0A, with two octets (offset
0x69CD). The modem adds the parameter only when its "MODEM BPS" setting
is 4800 bit/s or more.

| Octet | Contents |
|---|---|
| 1 | A bit map of the 9600 bit/s functions (refer to the table below) |
| 2 | The training length: 0 = long, 1 = short |

The bits of octet 1:

| Bit | Function | When the modem offers it |
|---|---|---|
| 0x01 | V.29 at 9600 bit/s | MODEM BPS is 9600 or 19200 |
| 0x02 | V.29 at 7200 bit/s | MODEM BPS is 7200 or more |
| 0x04 | V.29 at 4800 bit/s (probable) | Always |
| 0x08 | Rate shift (fallback) in the call. Refer to [Rate shift](#rate-shift). | Bit 3 of the `Y96T` setting is 0 |
| 0x10 | Automatic training length. Refer to [Line probe](#line-probe). | The "96 EQUALIZER" setting is A-LO or A-SH |
| 0x20 | Not known. It doubles one wait in the turnaround, and it sets one more bit in each data pump. | Bit 4 of the `Y96T` setting is 1 |
| 0x40, 0x80 | Not used | Never |

With the default settings (MODEM BPS 9600, 96 EQUALIZER A-LO, `Y96T`
0), the parameter is `0A 02 1F 00`.

The "96 EQUALIZER" setting has four values: LONG (0), SHRT (1), A-LO
(2) and A-SH (3). Bit 0 of the value goes into octet 2. Bit 1 of the
value is "automatic".

`Y96T` is a number in the SX command table of the modem (offset
0x5BD8). It is a test or service setting (probable). Its bits change the
data pump setup. Bits 3 and 4 change the LR.

### The responder

The responder reads parameter 0x0A of the originator's LR (offset
0x68FD):

1. It makes its own bit map from its MODEM BPS and 96 EQUALIZER
   settings. It does not use `Y96T` here.
2. It does an AND of its bit map and octet 1. The result is the
   agreed bit map.
3. It keeps octet 2 if octet 2 is 0 or 1. If octet 2 is more than 1,
   or is not there, it uses 0.
4. If the agreed bit map has 0x10, it adds 2 to the training value.
   Thus, values 2 and 3 mean "automatic".
5. The responder's LR has parameter 0x0A only if the agreed bit map is
   not 0. The parameter has the agreed bit map and octet 2.

The originator reads the responder's LR with the same procedure. Thus,
the two modems get the same result.

The modem reads only the first two octets of parameter 0x0A. It skips
more octets.

If a modem does not send parameter 0x0A, the agreed bit map is 0. Then
the call stays at 2400 bit/s, with MNP 2 to 5.

### The change to V.29

The change starts after the LR exchange (offsets 0x66C2 and 0x675E):

1. The originator sends its LA at 2400 bit/s. The responder waits for an
   LA or an LT.
2. Each modem sets the agreed bit map. If the map is 0, the modem stays
   at 2400 bit/s.
3. Each modem changes the data pump to V.29, and waits for a ready event
   from the data pump. If the event does not come, the modem stays at
   2400 bit/s.
4. Each modem waits 25 ms (2 ticks). Then it selects the rate: 9600
   bit/s if the map has 0x01, 7200 bit/s if the map has 0x02, else 4800
   bit/s.
5. The serial controller (a Z80 SIO) changes to HDLC: flag 0x7E, CRC on.
   The frames are MNP framing mode 3 frames (MNP 3).
6. If the training is automatic, the modems do the line probe.
7. The originator sends first. The responder listens first.

### Line probe

The line probe occurs only if the agreed bit map has 0x10 (offset
0x5C5E). It selects the long or the short V.29 training.

Each modem sends four tones, and measures the four tones of the other
modem. The originator sends first. The responder measures first.

To send, the modem:

1. Sets the transmitter to tone mode.
2. Sends 600 Hz, 1000 Hz, 1800 Hz and 3000 Hz, in that sequence. Each
   tone is 100 ms. There is 100 ms of silence after each tone.

To measure, the modem:

1. Waits 250 ms.
2. For each tone, waits for energy in three samples. It waits for
   500 ms for the first tone, and for 150 ms for the other tones.
3. Reads the receive level of each tone.
4. Compares each tone with the tone before it. A level drop more than a
   limit adds 1 to a score. A level drop less than a limit subtracts 1.
   The limits are different for each pair of tones.
5. Waits 600 ms.

The result:

- A positive score (a large loss at high frequencies) selects the long
  training.
- A negative score selects the short training.
- A score of 0 keeps the value from the LR.

The units of the receive level are not known.

## Statistical duplexing

### The turnaround cycle

A state machine controls the line (offsets 0x61A2 to 0x6606). An event
(a change of CTS or carrier from the data pump) or a timer moves it to
the next state. The originator starts at state 11. The responder starts
at state 8.

| State | What the modem does | Next state |
|---|---|---|
| 11 | Waits before it sends. The wait is 12.5 ms. With "96 ECHO SUP" set to DLAY, the wait is 112.5 ms. | 3 |
| 3 | Sets RTS on, and waits for CTS. | 4 when CTS comes. If no CTS in 325 ms: a failed turn, then 7. |
| 4 | Enables the HDLC transmitter. Waits 2.08 ms at 9600 bit/s, 6.25 ms at other rates. Bit 0x20 of the map doubles the wait. | 5 |
| 5 | Sends the frames of this turn. Refer to [The contents of a turn](#the-contents-of-a-turn). | 6 when the frames are sent. If the turn is longer than 3.2 s, the modem stops it. |
| 6 | Waits 2.08 ms (9600 bit/s) or 6.25 ms (other rates) for the last frame. | 7 |
| 7 | Stops the HDLC transmitter. Sets RTS off. Waits for CTS off. | 8 |
| 8 | Waits 5.2 ms. Then enables the receiver. | 9 |
| 9 | Waits for carrier: 1.0 s for the originator, 1.5 s for the responder. | 10 when carrier comes. Refer to [No carrier](#no-carrier) for the timeout. |
| 10 | Receives frames until the carrier stops. | 11 when the carrier stops. |

The raw timer values (B = 12.5 ms ticks, C = 52.08 µs counts) are in
the firmware at the state handlers. For example, state 3 uses B = 26,
and state 4 uses C = 40 (9600 bit/s) or C = 120.

A receive burst can be long. After 3 s of carrier, the modem gives
1 s more, 11 times. Thus, after approximately 14 s of carrier, the modem
stops the link.

Each modem has a counter of failed turns. It starts at 30. A failed
turn (no CTS, no carrier) subtracts 1. A good receive sets it to 30
again. At 0, the modem stops the link.

### No carrier

If no carrier comes in state 9:

1. The modem adds 1 to a count of silent turns. At 11, the modem stops
   the link.
2. On the first silent turn of each group of four, if "96 ECHO SUP" is
   TONE (the default), the modem sends 2100 Hz for 400 ms. This tone
   disables the echo suppressors of the line. Then it listens again
   (state 8).
3. Else, the modem takes the turn and sends (state 11). Thus, a lost
   burst does not stop the link.

### The contents of a turn

In state 5, the modem puts these frames in the transmit queue, in this
sequence (offset 0x5D9C):

1. A rate shift frame (type 8), if a rate shift is in progress.
2. The acknowledgements (LA) and LN or LNA frames that are necessary.
3. LT frames, if the modem has data. Retransmissions come before new
   frames (probable).

The modem sends LT frames only if it received a good burst in the turn
before. If not, the turn has only an LA. Then the turn ends. If the modem has no data, the turn has only an LA.
Thus, an idle modem gives the line back quickly.

The number of new LT frames in one turn is the lower of these two
values:

- The window (k) of the link.
- N = 1 + (bits 7 to 5 of (NOT E)). E is an error average, from 0 to
  255.

The error average E changes at each turn (offset 0x5DA6):

- F = E − E/16 (integer division). If F is not 0, subtract 1 from F.
- E = F + X. X is 16 if the turn before had an error (a retransmission
  or a timeout). Else, X is 0.

Thus, on a good line N is 8, and on a bad line N goes down to 1.

### Rate shift

Bit 0x08 of the agreed map enables a rate shift (offset 0x5DD6). The
modems use a new frame type, type 8:

| LI | Type | Octet 3 | Octet 4 |
|---|---|---|---|
| 03 | 08 | Code | Count |

The codes:

| Code | Meaning |
|---|---|
| 02 | Request: fall back one rate |
| 03 | Answer to 02 |
| 04 | Request: fall forward one rate |
| 05 | Answer to 04 |

The type 8 frame has no sequence number. MNP 10 uses the same codes in
its type 8 frame, but without the count octet (refer to
[MNP 10](mnp10.md)).

The rates are 9600, 7200 and 4800 bit/s. A fallback goes one rate down.
A fall forward goes one rate up.

The procedure:

1. A modem asks for a fallback when one of its two retransmission
   counters gets to 13, or when its failed turn counter is less than
   10. It
   does not ask at 4800 bit/s. It sends code 02 with count 10.
2. The other modem receives code 02 or 04. It selects the new rate. It
   answers with the code plus 1 and the count plus 1.
3. A modem that receives code 03 or 05 copies the count.
4. At each turn, each modem subtracts 1 from its count. While the count
   is not 0, it sends a type 8 frame with its code and the new count.
5. At the end of a receive burst, if the count is 1, the modem changes
   the data pump to the new rate. Then it sends at the new rate.
6. After the change, the modem does not start a new rate shift for 10
   turns.

The firmware has code for a fall forward (after 255 turns with no
error), but no code jumps to it (offset 0x5E1B). Thus, this ROM only
starts a fallback. It answers a request for a fall forward.

### Receive level

At the start of each receive burst, the modem reads the receive level.
If the level is out of range, it changes the gain of the receiver
before the next burst (offset 0x63FA). Bits 6 and 7 of `Y96T` set a
fixed gain.

## Interoperation

- The DeskPorte FAST firmware skips parameter 0x0A when it has fewer
  than 3 octets. When it has 3 or more octets, the DeskPorte skips the
  first two octets and reads the rest as parameter 0x0C (MNP 10)
  (refer to [firmware-findings.md](firmware-findings.md#f7-other-parameters)).
  The DeskPorte does not send parameter 0x0A.
- Thus, a DeskPorte and an AX/9624c connect at 2400 bit/s with MNP 2 to
  5.
- D-Modem ignores parameter 0x0A, and ignores type 8 frames when MNP 10
  is not on. Thus, the AX/9624c connects to D-Modem at 2400 bit/s.

## Modems that have MNP 6

| Modem | Source |
|---|---|
| Microcom AX/9624c | Firmware (this page) |
| Microcom AX/9612c | The model name is in the AX/9624c ROM |
| Microcom PC/9624c (internal card) | More than one secondary source |
| Microcom AX/9600 Plus | One secondary source |

One secondary source lists "MNP 6" for the Multi-Tech MultiModem 696E.
This is possibly an error in the source.

## Patents

We found one Microcom patent that is related to ULN:

- US 4,680,781. The inventors are Amundson and Melhorn. The patent was
  filed on 1985-10-30 and issued on 1987-07-14. It describes a link
  negotiation that is similar to ULN. We did not compare it with
  firmware.

We found no patent that describes statistical duplexing. Other
companies have patents for similar methods. These patents are not
Microcom patents, and they do not describe MNP 6:

- Racal: US 4,771,417 and EP 0260470.
- Hayes: US 4,894,847 and US 5,001,729.

We also examined US 5,241,565 and US 4,680,773. They are not related
to MNP 6. All these patents are older than 20 years, thus they are
expired.

## D-Modem

D-Modem does not support MNP 6.

D-Modem has some of the parts that MNP 6 needs:

- A V.29 data pump with half duplex operation (the FastPOS mode,
  `AT+MS=V29FP`, refer to [FastPOS](../dsp/fastpos.md)).
- V.22bis, for the first connection of ULN.
- The HST code changes the speed in the middle of a call, and keeps the
  link (refer to [HST](../dsp/hst.md)). This is the same type of change that
  ULN does.
- The MNP 10 code sends and receives type 8 frames with the same codes.

## What is necessary to support it

1. Send and parse LR parameter 0x0A, as in [LR parameter 0x0A](#lr-parameter-0x0a).
2. After the LA, change from V.22bis to V.29 half duplex, and change
   to HDLC framing.
3. Add the turnaround state machine, with RTS, CTS and carrier events
   from the V.29 data pump.
4. Add the burst rules and the type 8 rate shift.
5. Optional: the line probe and the 2100 Hz tone.
6. Make a capture of an AX/9624c call, to check the times on this page.

## Open questions

- The function of bit 0x20 of parameter 0x0A.
- The V.29 training sequence that the AX/9624c uses for each burst, and
  the meaning of the data pump register bits.
- States 14 to 23 have handlers, but no code in this ROM enters them.
- The exact CPU clock. The times on this page use 4.9152 MHz.
- The exact synchronization of the rate shift. Our reading of the code
  shows that one burst can be lost at the change (probable).
