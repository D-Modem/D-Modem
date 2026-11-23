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
  (offset 0x0185). The tick is 12.5 ms, thus the CPU clock is
  4.9152 MHz. The CPU is a 6 MHz part, but it runs at this lower clock.
- Proof of the tick: a counter reloads with 80 ticks (offset 0x1A86) and
  then advances a seconds, minutes and hours counter for the call time
  (offset 0x3CA8). Thus, 80 ticks = 1 s.
- The serial clock agrees: 4.9152 MHz / 16 = 307.2 kHz, and the SIO
  time constant 2 gives 9600 bit/s with a x16 clock.
- The turnaround state machine also uses CTC channel 2 as a one-shot
  timer, with prescaler 256. One count is 52.08 µs.

### The data pump

The 9600 bit/s board has a Rockwell R96 chip set (R5314, R5315, R5316,
and 10463). The register use of the code agrees with the R96DP data
sheet (Rockwell 1987 Modem Products Data Book, page 2-68):

| CPU address | R96DP chip | Registers that the MNP 6 code uses |
|---|---|---|
| 0x8000 to 0x800F | Chip 0, transmitter | 2 and 3 (tone frequency), 4 (transmit level), 5 (cable equalizer), 6 (configuration), 7 (RTS, mark hold), E (setup bit TSB) |
| 0xA000 to 0xA00F | Chip 1, receiver | 3 (RAM data), 6 (configuration), 7 (energy threshold RTH, T2), 9 (fast energy detector FED), E (setup bit RSB) |
| 0x9000 to 0x900F | Chip 2, receiver | 0 and 2 (RAM data) |

The configuration codes are the R96DP codes: 0x14 = V.29 9600, 0x12 =
V.29 7200, 0x11 = V.29 4800, 0x80 = tone. A tone frequency is
F = 0.146486 × N Hz. Status port 0x24 bit 7 is CTS and bit 6 is RLSD
(carrier). Both are 0 when active.

Registers 8 to D at 0x8000 and 0xA000 are not R96DP registers. The
2400 bit/s code uses them. They agree with the R2424 transmitter and
receiver maps (probable).

The code does not set the echo protector tone bit (EPT) or the training
disable bit. Thus, each burst starts with the full V.29 training. The
R96DP data sheet gives 253 ms from RTS to CTS for this training.

## Universal Link Negotiation

### LR parameter 0x0A

The originator's LR has parameter 0x0A, with two octets (offset
0x69CD). The modem adds the parameter only when its "MODEM BPS" setting
is 4800 bit/s or more.

| Octet | Contents |
|---|---|
| 1 | A bit map of the 9600 bit/s functions (refer to the table below) |
| 2 | The cable equalizer: 0 = long line (on), 1 = short line (off) |

The bits of octet 1:

| Bit | Function | When the modem offers it |
|---|---|---|
| 0x01 | V.29 at 9600 bit/s | MODEM BPS is 9600 or 19200 |
| 0x02 | V.29 at 7200 bit/s | MODEM BPS is 7200 or more |
| 0x04 | V.29 at 4800 bit/s. No code tests this bit alone. It makes the map not 0, and with no 0x01 or 0x02 the rate is 4800 bit/s. | Always |
| 0x08 | Rate shift (fallback) in the call. Refer to [Rate shift](#rate-shift). | Bit 3 of the `Y96T` setting is 0 |
| 0x10 | Automatic cable equalizer. Refer to [Line probe](#line-probe). | The "96 EQUALIZER" setting is A-LO or A-SH |
| 0x20 | A test option. Refer to [Bit 0x20](#bit-0x20). | Bit 4 of the `Y96T` setting is 1. It is 0 by default. |
| 0x40, 0x80 | Not used | Never |

With the default settings (MODEM BPS 9600, 96 EQUALIZER A-LO, `Y96T`
0), the parameter is `0A 02 1F 00`.

The "96 EQUALIZER" setting has four values: LONG (0), SHRT (1), A-LO
(2) and A-SH (3). Bit 0 of the value goes into octet 2. Bit 1 of the
value is "automatic".

The equalizer is the R96DP compromise cable equalizer (the CEQ field,
register 0:5 bits 4 and 5, offset 0x50EF). A value of 0 (long) selects
CEQ code 3: the equalizer for 7.2 km of cable, with +4.4 dB at 3000 Hz.
A value of 1 (short) selects CEQ code 0: no equalizer. The CEQ field
controls the transmitter and the receiver of the modem. The two modems
use the same value.

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
4. If the agreed bit map has 0x10, it adds 2 to the equalizer value.
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
   bit/s if the map has 0x01, 7200 bit/s if the map has 0x02, if not 4800
   bit/s.
5. The serial controller (a Z80 SIO) changes to HDLC: flag 0x7E, CRC on.
   The frames are MNP framing mode 3 frames (MNP 3).
6. If the equalizer is automatic, the modems do the line probe.
7. The originator sends first. The responder listens first.

### Line probe

The line probe occurs only if the agreed bit map has 0x10 (offset
0x5C5E). It selects the cable equalizer from the loss of the line at
high frequencies.

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
3. Reads the receive level of each tone. The level is a word from the
   RAM of receiver chip 1. The code does not set the RAM address, thus
   it is the default word after reset. It is probably the average
   energy, because a higher value is a stronger tone.
4. Compares each tone with the tone before it. A level drop more than a
   limit adds 1 to a score. A level drop less than a limit subtracts 1.
   The limits are different for each pair of tones.
5. Waits 600 ms.

The result:

- A positive score (a large loss at high frequencies) selects the cable
  equalizer (CEQ code 3).
- A negative score selects no cable equalizer (CEQ code 0).
- A score of 0 keeps the value from the LR.

## Statistical duplexing

### The turnaround cycle

A state machine controls the line (offsets 0x61A2 to 0x6606). An event
(a change of CTS or carrier from the data pump) or a timer moves it to
the next state. The originator starts at state 11. The responder starts
at state 8.

| State | What the modem does | Next state |
|---|---|---|
| 11 | Waits before it sends. The wait is 12.5 ms. With "96 ECHO SUP" set to DLAY, the wait is 112.5 ms. | 3 |
| 3 | Sets RTS on, and waits for CTS. The data pump sends the V.29 training (253 ms). | 4 when CTS comes. If no CTS in 325 ms: a failed turn, then 7. |
| 4 | Removes the mark hold (MHLD), and enables the HDLC transmitter. Waits 2.08 ms at 9600 bit/s, 6.25 ms at other rates. Bit 0x20 of the map doubles the wait. | 5 |
| 5 | Sends the frames of this turn. Refer to [The contents of a turn](#the-contents-of-a-turn). | 6 when the frames are sent. If the turn is longer than 3.2 s, the modem stops it. |
| 6 | Waits 2.08 ms (9600 bit/s) or 6.25 ms (other rates) for the last frame. | 7 |
| 7 | Stops the HDLC transmitter. Sets RTS off and mark hold on. Waits for CTS off. | 8 |
| 8 | Waits 5.2 ms. Then enables the HDLC receiver, and clears bit 1 of receiver register 7 (refer to [Bit 0x20](#bit-0x20)). | 9 |
| 9 | Waits for carrier: 1.0 s for the originator, 1.5 s for the responder. | 10 when carrier comes. Refer to [No carrier](#no-carrier) for the timeout. |
| 10 | Receives frames until the carrier stops. | 11 when the carrier stops. |

The raw timer values (B = 12.5 ms ticks, C = 52.08 µs counts) are in
the firmware at the state handlers. For example, state 3 uses B = 26,
and state 4 uses C = 40 (9600 bit/s) or C = 120.

A receive burst can be long. After 3 s of carrier, the modem gives
1 s more, 11 times. Thus, after about 14 s of carrier, the modem
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
3. If not, the modem takes the turn and sends (state 11). Thus, a lost
   burst does not stop the link.

### The contents of a turn

In state 5, the modem puts these frames in the transmit queue, in this
sequence (offset 0x5D9C):

1. The acknowledgements (LA) and LN or LNA frames that are necessary.
2. LT frames, if the modem has unacknowledged data.

If a rate shift is in progress, the turn has only the type 8 frame.
Refer to [Rate shift](#rate-shift).

The LT frames of a turn always start at the oldest unacknowledged LT
(offset 0x6078). Thus, a turn sends the retransmissions first, and then
the new frames. This is a go-back-N procedure in each turn. An LA from
the other modem removes the acknowledged frames from the queue (offset
0x5A9C).

The modem sends LT frames only if it received a good burst in the turn
before. If not, the turn has only an LA. If the modem has no data, the
turn has only an LA. Thus, an idle modem gives the line back quickly.

The number of LT frames in one turn is the lowest of these three
values:

- The number of unacknowledged LT frames in the queue.
- The window (k) of the link.
- N = 1 + (bits 7 to 5 of (NOT E)). E is an error average, from 0 to
  255.

The error average E changes at each turn (offset 0x5DA6):

- F = E − E/16 (integer division). If F is not 0, subtract 1 from F.
- E = F + X. X is 16 if the turn before had an error (a retransmission
  or a timeout). If not, X is 0.

Thus, on a good line N is 8, and on a bad line N decreases to 1.

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

1. A modem asks for a fallback in one of these conditions:
   - 13 turns in sequence with an LT that has no acknowledgement.
   - 13 turns in sequence with an LN that has no LNA.
   - 20 failed turns in sequence (the failed turn counter is less than
     10).

   It does not ask at 4800 bit/s. It sends code 02 with count 10.
2. The other modem receives code 02 or 04. It selects the new rate. It
   answers with the code plus 1 and the count plus 1.
3. A modem that receives code 03 or 05 copies the count.
4. At each turn, each modem subtracts 1 from its count. While the count
   is not 0, it sends a type 8 frame with its code and the new count.
   That turn has no LA and no LT.
5. At the end of a receive burst, if the count is 1, the modem changes
   the data pump to the new rate. Then it sends at the new rate.
6. After the change, the modem does not start a new rate shift for 10
   turns.

The requester sends `02 n`, and the other modem answers `03 n`, with
the same n. Each round trip decreases n by 1. The requester gets to a
count of 1 first. It changes the rate at the end of the next receive
burst, and then sends at the new rate. The other modem is still at the
old rate during that burst. It changes the rate only at the end of that
burst. Thus, the first burst at the new rate is lost, and the
go-back-N procedure sends it again. This sequence comes from the code.
We did not see it on a line.

During a rate shift, about 10 turns have no data. Each turn has a full
V.29 training. Thus, a fallback stops the data for about 5 s.

The firmware has code for a fall forward (after 255 turns with no
error), but no code jumps to it (offset 0x5E1B). Thus, this ROM only
starts a fallback. It answers a request for a fall forward.

### Receive threshold

At the start of each receive burst, the modem reads the receive level.
If the level is out of range, it changes the energy detector threshold
of the receiver (the RTH field: −43, −33, −26 or −16 dBm) before the
next burst (offset 0x63FA). If bits 6 and 7 of `Y96T` are not 0, they
set a fixed threshold.

### Bit 0x20

Bit 0x20 is a test option. The modem offers it only when bit 4 of
`Y96T` is 1, and `Y96T` is 0 by default. When the two modems agree on
it (offsets 0x5110 and 0x62B8):

- The wait in state 4 (after CTS, before the first frame) is two times
  longer.
- The modem sets bit 2 of receiver register 7 (chip 1). It also sets
  bit 3 of transmitter register 9 (chip 0).

The R96DP data sheet gives these names: register 1:7 bit 2 is BWRT (RAM
write for chip 2), bit 1 is T2 (the T/2 equalizer), and bit 0 is RTDIS
(training disable). Transmitter register 9 is reserved. These names do
not agree with the code: the code clears bit 1 in each receive turn,
and sets it in each transmit turn. This use agrees with RTDIS, not with
T2. Thus, the R5315 receiver chip possibly has the bits one place
higher than the R96DP data sheet. Then bit 1 is training disable, and
bit 2 (bit 0x20) is the T/2 equalizer. A T/2 equalizer needs more time
to converge, and the longer wait in state 4 agrees with this. We have no
data sheet for the R5315 to confirm it.

D-Modem must not offer bit 0x20. An AX/9624c with the default settings
does not offer it. If an AX/9624c offers it, D-Modem can clear it in its
answer.

### States 14 to 23

The state machine has ten more states, 14 to 23 (offsets 0x6535 to
0x6606). No code in this ROM enters them. They use the RTS input from
the computer (port 0x18 bit 0), and they set the DCD and CTS outputs to
the computer. Thus, they are probably a V.29 half duplex mode with no
MNP, where the computer controls the turnaround. This mode is not part
of MNP 6.

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
5. Optional: the line probe and the 2100 Hz tone. The equalizer
   choice changes only the local data pump. D-Modem can send its
   preferred value in octet 2, and send no 0x10 bit.
6. Make a capture of an AX/9624c call, to check the times on this page.

## Items to check on a line

The firmware gives all the rules of MNP 6. These items come from the
code, and a capture of an AX/9624c call can confirm them:

- The meaning of bit 0x20 (refer to [Bit 0x20](#bit-0x20)). A test:
  set `Y96T` bit 4 on two AX/9624c modems, and read the receiver
  register 7 with a logic analyzer. This item is not necessary for
  D-Modem.
- The times of the turnaround cycle on a real line.
- The lost burst at a rate shift.
