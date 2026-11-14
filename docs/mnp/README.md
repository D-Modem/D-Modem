# The MNP protocol suite

MNP (Microcom Networking Protocol) is a family of link protocols for
modems. Microcom made it in the 1980s. Each step of the family is a
"class". The classes have numbers from 1 to 10, and Rockwell and
Microcom added MNP 10EC later.

This folder gives one page for each class. Each page tells what the
class does, how it goes on the line, where the data comes from, and
what D-Modem does with it now.

## Summary of the classes

| Class | What it adds | Public specification | D-Modem |
|---|---|---|---|
| [MNP 1](mnp1.md) | Start-stop octet frames, two-way alternate (half duplex) | No | Not supported. An LR for class 1 gets an LD. |
| [MNP 2](mnp2.md) | Start-stop octet frames, two-way simultaneous (full duplex) | V.42 Annex A (framing mode 2) | Supported as the responder. D-Modem offers class 3. |
| [MNP 3](mnp3.md) | Bit-oriented (HDLC) frames: no start and stop bits | V.42 Annex A (framing mode 3) | Supported (the default) |
| [MNP 4](mnp4.md) | Data phase optimization and adaptive packet assembly | V.42 Annex A (parameter 8), US 4,691,314 | Optimization supported. The LT size does not change. |
| [MNP 5](mnp5.md) | Data compression: run-length and adaptive frequency coding | No | Supported |
| [MNP 6](mnp6.md) | Universal Link Negotiation and statistical duplexing | No | Not supported |
| [MNP 7](mnp7.md) | Enhanced data compression (Huffman coding with a Markov model) | No | Not supported |
| MNP 8 | No product is known. See [MNP 7](mnp7.md#mnp-8). | No | Not applicable |
| [MNP 9](mnp9.md) | MNP 7 on V.32, with better acknowledgements | No | Not supported |
| [MNP 10](mnp10.md) | Adverse channel enhancements: line probes, packet size, rate shifts | No | Supported (from the DeskPorte firmware) |
| [MNP 10EC](mnp10ec.md) | Enhanced Cellular: changes in the data pump for cellular calls | No | Link layer only. The data pump changes are not done. |

## How a modem gives a class number

In these pages, a bit of an LR parameter has the name of its mask. For
example, "the 0x10 bit" is the bit that V.42 calls bit 5. This prevents
confusion between the numbers from 0 and the numbers from 1.


The classes are not separate protocols on the line. All classes from 2
to 10 use the same LR frame, and the LR parameters tell which class the
call uses. The DeskPorte firmware calculates the class number for its
connect message from the negotiated LR values (see
[firmware-findings.md](firmware-findings.md#f1-the-class-number)):

1. If the 0x10 bit of parameter 8 (MNP 10) is set, the class is 10.
2. If the 0x02 bit of parameter 8 (fixed-field LT and LA frames) is
   set, the class is 4.
3. If not, the class is the framing mode: 1, 2 or 3.

MNP 5 is not in this calculation. The modem reports compression
separately. MNP 5 can occur with class 2, 3, 4 or 10.

## The phases of an MNP call

All classes use the same phases. V.42 Annex A gives the full procedure
for classes 2 to 4.

1. The data pumps connect. The link layer starts in start-stop octets.
2. The initiator (the calling modem) sends an LR. The LR gives the
   framing mode, k, N401, the optimizations and the compression that
   the initiator offers.
3. The responder sends an LR with the values that both modems can use.
4. The initiator sends an LA. Then both modems are in the data phase.
5. LT frames carry the data. LA frames acknowledge them and give
   credit. LN and LNA frames carry a break.
6. An LD frame ends the connection.

Each class after class 3 adds parameters to the LR, or new frame types,
or both. A modem skips an LR parameter that it does not know, by its
length. Thus a newer modem can connect to an older modem: the two
modems use the highest class that both have.

## Sources

The sources are of three types. Each page tells which type applies to
each statement.

- **Standards.** ITU-T V.42 (10/96), Annex A, gives classes 2 to 4. The
  1988 and 1993 editions have the same procedure. V.42 (03/2002)
  deleted Annex A. Get the 10/96 edition from
  `https://www.itu.int/rec/T-REC-V.42-199610-S`.
- **Firmware.** The Microcom DeskPorte 28.8P (M042116P) and the
  TravelCard FAST (M012315) ROM images, in the project files under
  `microcom-deskporte/firmware-decoded/`. These modems have MNP 2 to 5
  and MNP 10. They do not have MNP 6, 7 or 9.
- **Patents and secondary sources.** The Microcom patents US 4,691,314
  and US 5,191,583, and books and articles about MNP. A statement from
  a secondary source is marked "(secondary source)". We did not examine
  it in firmware or on a line.

## Open questions

These items are not known. Firmware of a modem with MNP 6, 7 or 9 can
answer them. A ROM from a Microcom modem of the late 1980s is the most
probable source.

- The LR parameter or bit that offers MNP 7. The DeskPorte does not
  read the 0x02 bit of parameter 9 (see [firmware-findings.md](firmware-findings.md#f5-parameter-9)).
- The MNP 7 code tables and the model of the Huffman coder.
- The signals of Universal Link Negotiation (MNP 6), and the turnaround
  rules of statistical duplexing.
- The frame formats of the MNP 9 acknowledgement and of selective
  retransmission.
- The data pump changes of MNP 10EC (Rockwell DSP code, not in a
  controller ROM).

## Related pages

- [The data link layer](../link.md) and its
  [technical details](../link-details.md#mnp): how D-Modem's MNP code
  works, and its tests.
- [Firmware findings](firmware-findings.md): the addresses in the
  DeskPorte ROM for each statement on these pages.
- The MNP 10 and MNP 10EC protocol specification (the Claude Doc of the
  project), traced from the DeskPorte firmware.
