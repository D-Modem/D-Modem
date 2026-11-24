# Hayes V-series protocols: technical details

This document is for developers. It tells how D-Modem does the bespoke
protocols of the Hayes V-series Smartmodem 9600, firmware 1.4. For a
summary, refer to [Hayes V-series protocols](hayes.md).

The source of each rule is the firmware analysis of the modem ("FA" in
this document) and the implementation specification of the project. The
firmware addresses are in the comments of the code. A rule that FA gives
as "probable" is "probable" here too.

| Part | Code | Tests |
|---|---|---|
| 1. Hayes ADC | `link/hayes_adc.h`, `link/hayes_adc.cpp` | `hayes_test` (`Hayes.Adc*`) |
| 2. XID sets and the selection of the compression | `link/hayes_xid.h`, `link/hayes_xid.cpp`, `link/lapm.cpp`, `link/link.cpp` | `hayes_test` (`Hayes.Xid*`), `link_test` (`Link.LapmHayes*`) |
| 3. AFT framing | `link/aft.h`, `link/aft.cpp` | `hayes_test` (`Hayes.Aft*`) |
| 4. Express 96 link rules | `link/e96_burst.h`, `link/e96_burst.cpp` | `hayes_test` (`Hayes.E96*`) |
| 5. Express 96 modulation | none | none (blocked) |

To run the tests:

    ctest --test-dir build -R 'hayes_test|link_test'

## 1. Hayes ADC

ADC (Adaptive Data Compression) is an adaptive order-0 rank code. Its
unit is the nibble (4 bits). One encoder runs for the transmit direction
and one decoder runs for the receive direction. Each keeps its model for
all of the connection. The model is not set again for each frame.

### 1.1 The model

- `count[r]`: the frequency of the rank r.
- `rank[c]`: the rank of the character c.
- `char[r]`: the character at the rank r.
- `t1`, `t2`, `off`: the thresholds of the code.
- `prev`, `run`: the last coded character and the run counter.

The initial values are: all counts 0, `rank[c]` = c, `char[r]` = r,
`t1` = 4, `t2` = BFh, `off` = 3Ch, `prev` = 0, `run` = −2.

### 1.2 The code of a rank

| Rank | Nibbles that go out |
|---|---|
| r < `t1` | one nibble: r |
| `t1` ≤ r ≤ `t2` | the octet (r + `off`) mod 256, high nibble first |
| r > `t2` | the nibble 15, then the octet r |

After each coded character, the model changes:

1. Each 128 coded characters, the encoder calculates `t1`, `t2` and
   `off` again from the counts. Then it divides all counts by 2.
2. The count of the rank of the character increases by 1.
3. The character moves up past the first of the 16 ranks above it that
   has a smaller count.

### 1.3 Runs

After three equal characters, the encoder does not code the next equal
characters. It counts them. It sends the count as one nibble when a
different character comes, or at the end of the frame. A count of 15 goes
out at once, and the count starts again at 0.

### 1.4 Octets and frames

- Two nibbles make one octet, high nibble first.
- The octet FAh goes out as FAh 03h.
- Each frame ends on an octet boundary. If a pad nibble 15 is necessary,
  FAh FFh follows it. The decoder then drops the code that is not
  complete.
- FAh 00h or FAh 01h sets the model of the decoder to the initial values.

### 1.5 ADC in a V.42 call

`Link::comp_read_adc()` fills each I frame. It adds characters while the
code of one more character (4 octets at most) and the flush (3 octets at
most) fit in N401. Then it flushes the encoder. Thus each I frame ends on
an octet boundary, as the firmware sends it. `HayesAdc::out()` never
gives FAh without the octet after it. Thus FAh and its second octet are
always in the same frame.

### 1.6 Tests

1. `Hayes.AdcFirmwareVectors`: 5 connections (43 frames). The expected
   output is the output of the **firmware** encoder in a Z80 emulator
   (`link/test/hayes_adc_vectors.h`, made by
   `tools/gen-hayes-adc-vectors.py`). The D-Modem encoder agrees octet
   for octet. The decoder gives back the input of each frame.
2. `Hayes.AdcRoundTrip`: random data, runs of 15, 16, 30, 31 and more
   equal octets, many FAh octets, and a small alphabet.
3. `Hayes.AdcDecoderReset`: FAh 00h and FAh 01h in a stream.
4. `Hayes.AdcEscapeNotSplit`: the encoder output never stops between
   FAh and its second octet.

## 2. The XID sets and the selection of the compression

### 2.1 The sets

A Hayes modem ends its XID with the user data subfield (GI FFh, no group
length). The subfield has two sets. Each set is a parameter FFh with a
3-letter name, then its parameters as (PI, PL, PV):

| Set | Parameter | Value |
|---|---|---|
| `AFT` | P1 | the AFT capabilities (Hayes: S44 AND S45) |
| `AFT` | P2 | the AFT escape options (Hayes: S33) |
| `HMP` | P1 | the protocol selection (Hayes: S46). Bit 1 is the compression. |
| `HMP` | P2 | 01h with compression, else 00h |

D-Modem sends the two sets when S46 is not 0. They are first in the user
data subfield, as in the Hayes XID. The V.44 set and the other D-Modem
sets come after them. In an XID response, D-Modem sends the sets only if
the XID command of the remote modem had the HMP set. If the compression
of the call is not in the two directions (`AT%C`, `+DS`), D-Modem clears
bit 1 of HMP P1, because ADC is always in the two directions.

`hayes_xid_template()` makes the full XID of a Hayes modem (the template
at bank 3 765Fh). The tests use it as the remote modem.

### 2.2 The parser

`HayesSetParser` gets each parameter of the user data subfield in order.
A parameter FFh with the name `AFT` or `HMP` starts a set. A parameter
FFh with a different name starts an unknown set, and the parser ignores
its parameters. The set identifier of V.44 (40h) ends the set. If the
XID has no HMP set, the remote modem is not a Hayes modem. Then the
normal V.42 rules apply.

### 2.3 The results

| Parameter | Result |
|---|---|
| AFT P1 | the AND of the two values (`Config::aft_caps`) |
| AFT P2 | the OR of the two values (`Config::aft_opts`) |
| HMP P1 | the AND of the two values. Bit 1 is the compression flag. |

`Link::update_config()` then selects the compression (FA 6.8.1):

1. If the compression flag is 0, the call has no compression. This is
   also true when the two modems agreed V.42bis.
2. If the flag is 1 and the XID agreed V.42bis, the call uses V.42bis.
3. If the flag is 1 and the XID did not agree V.42bis, the call uses ADC
   in the two directions (`Config::adc`).

Between two D-Modems with S46, V.44 and zstd come before these rules, as
in a normal call. A Hayes modem does not offer them.

The core shows `COMPRESSION: ADC`. The numeric result code is 68, the
Hayes number for ADC. Note that D-Modem also uses 68 for V.44.

### 2.4 Tests

1. `Hayes.XidTemplate`: the XID with S69 = 7, P0 = 03h and S46 = 138 is
   the template of FA 6.9, octet for octet.
2. `Hayes.XidParser`: the sets, an unknown set between them, a V.44 set
   after them, and an XID without the sets.
3. `Hayes.XidSelection`: the three rules.
4. `Link.LapmHayesAdc`: two links back to back with S46 138 or 136 at
   each side, with and without V.42bis. The data goes through in the two
   directions with ADC, with V.42bis, and without compression.
5. `Link.LapmHayesPeer`: the XID command of a Hayes modem to D-Modem with
   and without S46. The XID of a D-Modem caller with S46.

## 3. AFT framing

AFT (asynchronous framing technique) sends the V.42 frames on start-stop
characters. It is the method of RFC 1662 (PPP in HDLC-like framing):

1. Each octet is a character: a start bit, 8 data bits, a stop bit.
2. The flag is 7Eh. The control escape is 7Dh. An escaped octet goes out
   as 7Dh, then the octet XOR 20h.
3. 7Dh and 7Eh are always escaped. With option bit 1 (`AFT_OPT_CTRL`),
   00h–1Fh, 7Fh, 80h–9Fh and FFh are escaped too. The receiver then
   ignores these characters when they come without 7Dh.
4. The FCS is the FCS-16 of V.42. Its complement goes out low octet
   first (confirmed: fixed ROM 2150h and 2193h).
5. 7Dh 7Eh aborts a frame.

`Aft` is a framer like `Hdlc`: it gets frames from a `Framer` and gives
the received frames to it. `set_options()` takes the OR of the two AFT P2
values.

**Open items:**

- The 7-bit mode (option bit 2) is not done. The firmware puts a parity
  bit from the table at A032h in bit 7 of each character. FA does not
  show how 8-bit data goes through this mode.
- The link layer does not select AFT yet. FA does not show when the
  firmware uses AFT in place of synchronous HDLC (result codes 73, 76
  and 79). Thus `Aft` is tested alone.

Tests: `Hayes.AftFcs` (the FCS test values of RFC 1662), `Hayes.AftLine`
(the 256 octet values on the line in the two escape modes),
`Hayes.AftLoopBack` (frames from one `Aft` to another, a bit error, an
abort, XON and XOFF from the line).

## 4. Express 96 link rules

Express 96 is half-duplex. One modem at a time sends a burst:

    marks (16 or more 1s)   flags   up to 7 frames   marks (7 or more 1s)

| Rule | Value | Source |
|---|---|---|
| Frame format | HDLC flags 7Eh, zero-bit insertion, FCS-16 | FA 6.3, 6.7 |
| Information field | 128 octets at most (N401) | FA 6.9 |
| Frames per burst | 7 at most. 4 or 2 when the link reports errors. | FA 6.6.4 |
| Start of receive | 16 or more 1s, then a flag | FA 6.3 |
| End of a burst | 7 or more 1s | FA 6.3 |
| Empty burst | flags only, for 1 ms (4800) or 4 ms (9600); 35 ms for the first 5 bursts | FA 6.6.3 |

### 4.1 The burst manager

`E96Burst` is the state machine of the firmware (bank 4 6260h, FA
6.6.5). It uses the numbers of the firmware states:

| State | Event | Action, and the next state |
|---|---|---|
| 1 | long timer | Line off. Start the silence time. → 3 |
| 3 | turnaround timer | Transmitter on. Budget 7 (4 or 2). A frame waits: send it → 4. Else (and for the first 5 bursts): → 6 |
| 4 | frame sent | Budget left and a frame waits: send it. Else: transmitter off → 5 |
| 5 | transmitter idle | Start the time after the transmitter. → 7 |
| 6 | turnaround timer | Transmitter off. Start the time after the transmitter. → 7 |
| 7 | turnaround timer | Tell the data pump to receive. Long timer 100. → 8 |
| 8 | the line turned | Start the receive delay. → 9 |
| 9 | turnaround timer | Receiver on. → 10 |
| 10 | 16 marks | Keep the time of the last good receive. → 11 |
| 11 | flags | Long timer 500. → 12 |
| 12 | 7 marks | Receiver off. Start the silence time. → 3 |

When a long timer ends in the states 8 to 12, the manager counts a
failure. It waits 260 ticks, then goes to state 1 to send a burst again.
After `E96Times::max_failures` failures in a row, the link fails. The
retrain states 13 to 16 are not done.

The turnaround times (FA 6.6.3, ms probable) are in
`E96Times::for_rate()`:

| Time | 4800 bit/s | 9600 bit/s |
|---|---|---|
| Silence before this modem transmits | 20 ms | 25 ms |
| The line turned, before the receiver is on | 15 ms | 30 ms |
| The transmitter on in an empty burst | 1 ms | 4 ms |
| The transmitter off, before the data pump is told to receive | 12 ms | 15 ms |

All times are in `E96Times`, because the unit of 1 ms is probable. The
tick of the long timer is not known. The default is 10 ms.

### 4.2 The loop-back line

`E96Line` makes and receives the bits of the bursts with an `Hdlc`. It
is an `E96Port` for the burst manager. The modulation of Express 96 is
not known. Thus:

- The leading marks of each burst (40 ms by default) stand in for the
  training of the data pump. They always go out whole.
- The line has 0s when no modem transmits (no carrier).
- A modem does not receive while it transmits (a half-duplex pump).
- The "line turned" event comes only after our own burst is out.

### 4.3 Tests

`Hayes.E96LoopBack` connects two `E96Line` instances on one simulated
half-duplex line, at 4800 and 9600 bit/s. Data goes in the two
directions. The test makes sure that:

- all frames arrive, in order;
- the two modems never transmit at the same time;
- no burst has more than 7 frames;
- each burst ends with 7 or more 1s on the line, and the other modem
  sees each end;
- the first 5 bursts are empty.

`Hayes.E96Budget` tests the budget of 7, 4 and 2 frames.
`Hayes.E96Failure` tests the failures when the remote modem does not
answer.

**Open items:**

- The firmware rule for a collision (the two modems transmit after a
  failure at the same time) is not known.
- The burst manager is not connected to LAPM in a call yet, because
  there is no Express 96 data pump (section 5).

## 5. Express 96 modulation (blocked)

The controller ROM does not have the modulation. The data pump (U21, TI
04-00197, probably a TMS320C25 with a mask ROM) does the training, the
rate requests and the 2400-baud modulation. The patent gives the
structure, but not the carrier frequency, the scrambler or the lengths
of the sequences.

Do not start this part before one of these items is available:

1. A stereo recording of a call between two Hayes V-series 9600 modems
   (or a V-series modem and an ULTRA 96), with the two directions on
   separate channels.
2. A dump of the U21 mask ROM.

## 6. Other open items

- If S46 is 0, D-Modem does not send the HMP set. FA does not fully show
  what a Hayes modem does when the HMP set is missing. If the Hayes modem
  then uses rule 1 (no compression) and D-Modem uses V.42bis, the data
  is not correct. With a Hayes modem, set `ATS46=138`.
- D-Modem has no AT command for the AFT values (Hayes S33, S44 and S45).
  It sends 0 in AFT P1 and P2.
- "ALT" (result code 80) is MNP. It is in the MNP code, not here.
