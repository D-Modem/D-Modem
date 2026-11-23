# Firmware findings for the MNP classes

This page gives the facts about MNP 1 to 5 that we got from the
Microcom DeskPorte 28.8P firmware (M042116P). The MNP 10 and MNP 10EC
facts are in the MNP 10 protocol specification and in
[mnp10.md](mnp10.md). This page does not repeat them.

## The image

- File: `microcom-deskporte/firmware-decoded/deskporte-28.8P-M042116P.bin`
  in the project files (131072 octets).
- CPU: a Z180 at 18.432 MHz. The MMU maps a bank of the image into the
  CPU address space.
- Address rules on this page: an "offset" is a position in the file. A
  "CPU address" is the address that the code uses. For the LR code in
  this page, CPU address = offset − 0xA000. For code below offset
  0x10000, the CPU address is the same as the offset.
- The TravelCard FAST image (M012315) has the same LR code at other
  addresses. We did not compare each item below with it.

We decoded the code with a Z80 disassembler (the Z180 has the Z80
instruction set and some more instructions). No firmware code is copied
here. This page gives only the behavior.

## RAM that the LR code uses

| CPU address | Contents |
|---|---|
| 0x8821 | Flags: the parameters that the peer's LR had (0x01 = parameter 2, 0x02 = parameter 3, 0x04 = parameter 4) |
| 0x8822 | The negotiated framing mode (the service class): 1, 2 or 3 |
| 0x8823 | The negotiated parameter 8 (optimizations) |
| 0x8824 | The negotiated parameter 9 (compression) |
| 0x7E79 | Constant parameter 1, as received (1 or 2) |
| 0x7E7A | N401 (two octets) |
| 0x7E7E | k |
| 0x7E76 | Link flags. The 0x08 bit is "class 1" (two-way alternate). |

## F1: the class number

The function at offset 0x0EFB0 gives the MNP class for the connect
message:

1. If the 0x10 bit of the negotiated parameter 8 is set, the class is
   10.
2. If not, and the 0x02 bit (fixed-field LT and LA) is set, the class is
   4.
3. If not, the class is the negotiated framing mode (1, 2 or 3).

Thus, for this firmware, "class 4" means the fixed-field optimization.
The 0x01 bit (256-octet information fields) alone does not make a call
class 4.

## F2: the framing mode and class 1

The LR parser starts at offset 0x14BBD (CPU address 0xABBD).

- Before it reads the parameters, it sets the framing mode to 1, and
  sets the class 1 flag (0x7E76, the 0x08 bit).
- Parameter 2 (framing mode): the parser keeps the lower of the received
  value and 3 (offset 0x14C55). If the result is 2 or 3, it clears the
  class 1 flag.
- If the LR has no parameter 2, the framing mode stays at 1.

Thus, the DeskPorte accepts class 1. It does not reject an LR that asks
for framing mode 1, and it treats an LR with no framing mode parameter
as class 1.

When the class 1 flag is set, the firmware adds waits:

- The routine that sends a link frame (offset 0x011E2) first calls the
  wait routine (offset 0x000FA) with the value 0xA0 (160).
- After the LR exchange, the code at offset 0x1494E calls the wait
  routine with the value 0x28 (40).

We did not find the unit of the wait routine. If the unit is the 12.5 ms
tick of the MNP 10 timers, the waits are 2 s and 0.5 s. These waits
agree with a two-way alternate line, where the modem must wait for the
line to change direction. This is our interpretation. We did not see it on a
line.

## F3: parameters 3 and 4

- Parameter 3 (k): the parser keeps the lower of the received k and its
  own k (offset 0x14C76).
- Parameter 4 (N401): the parser keeps the lower of the received N401
  and its own N401 (offset 0x14C8C).
- If the LR has no parameter 3, the code at offset 0x14F17 sets k to 1
  and the framing mode to 1. This occurs when the 0x18 bits of the
  flags at 0x8D42 are 0. We think that these bits mark the V.42
  upgrade of MNP 10, but we did not confirm this.

## F4: constant parameter 1

The third octet of the LR is constant parameter 1. V.42 Annex A gives
the value 2.

- The parser (offset 0x14C24) stores the received value. A value of 2
  or more is stored as 2.
- A value of 1 sets the 0x02 bit of the flags at 0x7E41. Nothing else
  changes in the LR exchange.
- The LR that the DeskPorte sends has the stored value in this octet
  (offset 0x14F5B). Thus, it answers a 1 with a 1.

Thus, a value of 1 probably marks an older MNP modem. We did not find
which older modems send it.

## F5: parameter 9

The parser reads parameter 9 at offset 0x14CEA:

- 0x01 bit: MNP 5. The result is the received bit AND the local MNP 5
  setting (offset 0x7CCE, the 0x01 bit).
- 0x04, 0x08, 0x10 bits: a short form of V.42bis. They mean P1 = 512,
  1024 or 2048, with P0 = 3 and P2 = 32. The result P1 is the lower of
  the two modems' values, and P2 is 32 or less.
- 0x02 bit and the 0x20, 0x40, 0x80 bits: the parser does not read
  them.

Thus, the DeskPorte has no MNP 7 in parameter 9. If MNP 7 has a bit in
parameter 9, it is probably the 0x02 bit, because the other low bits
have a meaning. This is a guess.

## F6: the response LR

The routine at offset 0x150CE builds the responder's LR:

- Parameters 2, 3 and 4 are in the response only if the initiator's LR
  had them.
- Parameter 8 is in the response only if the negotiated value AND 0x73
  is not 0.

## F7: other parameters

- Parameter 0x0B: Microcom security. The parser compares the 5-octet
  token with a list of 25 entries (offset 0x14D57). D-Modem does not
  send it.
- Parameter 0x0A: the MNP 6 negotiation parameter (refer to
  [MNP 6](mnp6.md#lr-parameter-0x0a)). The AX/9624c sends it with two
  octets. The DeskPorte skips a parameter 0x0A that has fewer than 3
  octets (offset 0x14D45). If it has 3 or more octets, the DeskPorte
  skips the first two octets (the MNP 6 octets), and reads the rest
  with the parameter 0x0C (MNP 10) code. The DeskPorte does not send
  parameter 0x0A. Thus, a later modem can possibly send the MNP 6
  octets and an MNP 10 field in one parameter 0x0A. We did not find a
  modem that does this.
- Parameter 0x11: an originator mark. When the local flag at 0x7C21 is
  set, the parser rejects an LR that has it (offset 0x14EA5). The MNP 10
  specification identifies this flag as "we are the originator".
- Parameter 0x82: the 0x02 bit offers link resume. The parser clears the
  bit when the local setting at 0x7CF6 is 0 (offset 0x14E92). This
  setting is probably `)H`, the reconnect time.

## How to check these findings

1. Install a Z80 disassembler, for example the Python package `z80dis`.
2. Disassemble the bank code from offset 0x14B00 to 0x15200 with the
   CPU address = offset − 0xA000.
3. Disassemble the common code from offset 0x0EF00 to 0x0F000 with the
   CPU address = offset.
4. Compare the instructions at each offset on this page with the
   statement.
