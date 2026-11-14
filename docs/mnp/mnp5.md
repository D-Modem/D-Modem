# MNP 5

MNP 5 is the data compression of MNP. It compresses the data from the
DTE before the link puts it into LT frames. It is not part of V.42.
Microcom licensed it to other modem makers free of charge (secondary
source: Wikipedia). Most modems with MNP 4 also have MNP 5.

## What it does

MNP 5 has two parts. The transmitter applies them in this sequence:

1. **Run-length coding.** After three equal characters, the
   transmitter sends a count of the equal characters that follow (0 to
   250). At 250, a new run starts.
2. **Adaptive frequency coding.** The transmitter keeps a table of the
   256 characters, in the sequence of how frequently they occur. A
   character moves up the table past the characters that occurred
   fewer times. When the count of the top character gets to 255, all
   counts are divided by 2.

Each character is sent as the code of its position (rank) in the table:

| Ranks | Code (header, then the bits of the rank) | Bits |
|---|---|---|
| 0, 1 | `000x` | 4 |
| 2, 3 | `001x` | 4 |
| 4 to 7 | `010xx` | 5 |
| 8 to 15 | `011xxx` | 6 |
| 16 to 31 | `100xxxx` | 7 |
| 32 to 63 | `101xxxxx` | 8 |
| 64 to 127 | `110xxxxxx` | 9 |
| 128 to 255 | `111xxxxxxx` | 10 |

- Rank 255 has one more bit, 0, after its code.
- A run count is sent as the code of its value. It does not use the
  table, and it does not change the table.
- The codes fill each octet from the most significant bit.
- The flush is eleven 1 bits. The transmitter sets the other bits
  of the octet to 1. The transmitter flushes when the DTE has no more
  data.
- The compressed stream continues from one LT frame to the next.

Text becomes about two thirds of its size. Random data becomes a
little larger, because the ranks from 128 to 255 take 10 bits.
Microcom gave about 2:1 on the best data and 1.6:1 on typical text
(secondary source: Wikipedia).

## On the line

- LR parameter 9, the 0x01 bit, offers MNP 5. The responder sets the
  bit only if both modems want MNP 5.
- The DeskPorte accepts MNP 5 with any class. Its parser does not
  examine the framing mode or parameter 8 when it reads parameter 9.
- The DeskPorte also uses the 0x04, 0x08 and 0x10 bits of parameter 9
  for V.42bis in MNP (refer to
  [firmware-findings.md](firmware-findings.md#f5-parameter-9)).

## D-Modem

D-Modem supports MNP 5. The code is `Mnp5` in `link/mnp5.h` and
`link/mnp5.cpp`.

- The code follows the PCTel modem's code (`MnpCompress`,
  `MnpDecompress` and `MnpEncSort` in its `control.o`).
- D-Modem decodes the Agere modem's MNP 5 octets. For the same text,
  D-Modem's encoder sends the same octets as the Agere modem
  (`Link.Mnp5`).
- `AT%C1` and `AT%C3` offer MNP 5. `AT%C2` gives MNP 4 with no MNP 5.
- The interop tests examine MNP 5 in the two directions with the Agere,
  PCTel, SM56, HSF, Broadcom, Intel and ESS soft modems.

## V.42bis in MNP

Some modems use V.42bis compression on an MNP link in place of MNP 5:

- The DeskPorte sends the V.42bis parameters in LR parameter 0x0E (P0,
  P1 and P2), or in the short form in parameter 9.
- The SM56 modem sends parameter 14 (0x0E) with P0 = 3, P1 = 2048 and
  P2 = 32.

D-Modem does not use V.42bis on an MNP link. It ignores parameter 0x0E
and the V.42bis bits of parameter 9. Thus, an MNP call between D-Modem
and the DeskPorte uses MNP 5, or no compression. A V.42 call between
them uses V.42bis.
