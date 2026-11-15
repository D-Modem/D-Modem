# MNP 7

MNP 7 is the second data compression of MNP. Microcom calls it
Enhanced Data Compression. It replaces MNP 5 on a call where both
modems have it. No specification is public, and we have no firmware
that has MNP 7.

All the statements on this page come from secondary sources, unless
the statement names the DeskPorte firmware. Wikipedia gives only the
3:1 claim. The Huffman coding and the Markov model come from other
descriptions of MNP, and we did not confirm them.

## What it does

- MNP 7 uses Huffman coding. The code of a character depends on the
  character before it (a first-order Markov model). Thus, there are
  many code tables, one for each previous character.
- The tables adapt to the data during the call, as in MNP 5.
- MNP 7 also has run-length coding, as in MNP 5.
- Microcom claimed up to 3:1 on text files (secondary source:
  Wikipedia). V.42bis, at the same time, gave up to 4:1.

The difference from MNP 5 is the Markov model. MNP 5 has one table for
all characters. MNP 7 knows, for example, that "u" occurs very
frequently after "q". Thus, the code for "u" after "q" can be very short.

## On the line

We do not know how MNP 7 is negotiated.

- MNP 5 is the 0x01 bit of LR parameter 9.
- The DeskPorte firmware uses the 0x04, 0x08 and 0x10 bits of
  parameter 9 for V.42bis. It does not read the 0x02 bit (refer to
  [firmware-findings.md](firmware-findings.md#f5-parameter-9)).
- Thus, the 0x02 bit of parameter 9 is a probable place for MNP 7. This
  is a guess. A Microcom ROM with MNP 7 can confirm it.

## Modems that have MNP 7

The sources are product reviews, catalogues, Datapro reports and
advertisements of the period. We found no ROM dump for these modems.

| Modem | Source |
|---|---|
| Microcom QX/12K | More than one source |
| Microcom QX/2400t | One secondary source (Datapro) |
| Multi-Tech MT224EM7 (MNP 5 and MNP 7) | One secondary source |

The Microcom QX/4232 family (about 1990) possibly also has MNP 7. This
is not confirmed. The V.32 modems with MNP 9 also have MNP 7 (refer to
[MNP 9](mnp9.md)).

## Patents

We found no patent that describes the MNP 7 compression. Two Microcom
patents describe the MNP 5 coder:

- US 4,748,638.
- US 5,027,376.

These two patents describe an adaptive code of order 0. That is, the
code of a character does not use the previous character. They do not
describe a Markov model. Thus, they do not give the MNP 7 method. Both
patents are expired.

## D-Modem

D-Modem does not support MNP 7. In parameter 9 of the LR, D-Modem
sets the 0x01 bit (MNP 5) and, with S114=1, the V.42bis bits. A peer
that offers MNP 7 and MNP 5 gets MNP 5 or V.42bis.

## What is necessary to support it

1. Get the firmware of a modem with MNP 7, for example a Microcom
   modem from the late 1980s.
2. Find the LR bit that offers MNP 7.
3. Find the initial code tables, the adaptation rule and the flush.
4. Write the codec next to `Mnp5` in `link/`, with the same interface.
5. Make test vectors from the firmware, as for MNP 5.

## MNP 8

No modem with MNP 8 is known. Wikipedia says that "there was apparently
no MNP 8 released" (secondary source). Other secondary sources say
that MNP 8 was to add MNP 7 to half duplex links. We found no
specification, no product and no firmware for it.
