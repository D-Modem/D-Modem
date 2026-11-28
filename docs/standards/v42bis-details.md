# V.42 bis: technical details

This page gives the technical details for [V.42 bis](v42bis.md). The
clause numbers are the clause numbers of V.42 bis (01/90). This page is
our own description, not a copy of the Recommendation. The details of
the D-Modem implementation are on [the data link layer: technical
details](../link-details.md).

## The parameters (5.1)

| Parameter | Meaning | Range | Default |
|---|---|---|---|
| P0 | The directions of compression: none, transmit, receive or both | 0 to 3 | 0 (none) |
| P1 (N2) | The number of codewords in the dictionary | 512 or more | 512 |
| P2 (N7) | The maximum length of a string, in characters | 6 to 250 | 6 |

- The two modems use the lower value of P1, and the lower value of P2.
- A value out of range is a procedural error. The connection stops.
- If one modem asks for compression in two directions, the other modem
  can agree to two directions, to one direction, or to none.

## The constants and the counters

| Name | Meaning | Value |
|---|---|---|
| N3 | The character size | 8 bits |
| N4 | The number of characters | 256 |
| N5 | The first codeword of a string of two or more characters | N4 + N6 = 259 |
| N6 | The number of control codewords | 3 |
| C1 | The next empty dictionary entry | N5 to N2 − 1 |
| C2 | The size of a codeword | 9 bits at the start |
| C3 | The threshold for STEPUP | 512 at the start |

## The control codewords and the command codes (9)

| Compressed mode codeword | Function |
|---|---|
| 0 ETM | Enter transparent mode |
| 1 FLUSH | Flush the data. Zero bits follow to the octet boundary. |
| 2 STEPUP | The codewords that follow have one more bit |

| Transparent mode command code (after the escape character) | Function |
|---|---|
| 0 ECM | Enter compressed mode |
| 1 EID | The escape character is in the data |
| 2 RESET | The dictionary starts again |

- The escape character is 0 at the start. Each time the escape
  character occurs in the data, it increases by 51 (modulo 256), in the
  two modes (9.2).

## The dictionary (6)

1. **Initialization (6.2).** Each of the 256 trees has its root only.
   The codeword of a root is N6 plus the value of the character. C1 is
   N5.
2. **String matching (6.3).** The encoder adds characters to the string
   while the dictionary has the longer string.
3. **Addition (6.4).** The unmatched character makes a new string from
   the matched string. A new string is not added if it is longer than
   N7, or if it is in the dictionary.
4. **Recovery (6.5).** After each addition, C1 increases. After N2 − 1,
   it goes back to N5. C1 skips an entry that is in use and is not a
   leaf. A leaf at C1 is removed from its parent.

## The encoder (7)

- **Initial conditions (7.2).** C2 is 9, C3 is 512, transparent mode,
  escape character 0.
- **Encoding (7.4).** If a codeword is equal to or more than C3, the
  encoder sends STEPUP. Then C2 increases by 1, and C3 doubles.
- **Transfer (7.5).** The codewords are packed, the least significant
  bit first. After FLUSH and after ETM, zero bits fill the octet.
- **Compressibility test (7.8).** The method is not specified. The
  encoder compares the number of bits before and after compression.
  - To compressed mode: the dictionary update, then the escape character
    and ECM.
  - To transparent mode: the last codeword, the dictionary update, ETM,
    then zero bits to the octet boundary.
- **C-FLUSH (7.9).** In compressed mode, the encoder sends the codeword
  of the partial string. If the bits do not end on an octet boundary, it
  sends FLUSH and zero bits. In transparent mode, it sends all the
  characters that it holds.

## The decoder (8)

The decoder keeps its dictionary the same as the dictionary of the far
encoder. It also does the string matching in transparent mode. In
compressed mode, a new entry is the last decoded string plus the first
character of the string that follows it.

## V.42 bis in D-Modem

| Item | D-Modem (`link/v42bis.cpp`) |
|---|---|
| N2, N7 that we offer | 2048 codewords, strings of 34 characters |
| Directions | Transmit, receive or both |
| Compressibility test | A comparison of the bits in and the bits out |
| RESET | We never send it. We obey it from the far modem. |
| Escape character | 0, plus 51 at each occurrence, in the two modes |
| C-FLUSH | When the DTE has no more data for now |
| P1 less than 512, P2 less than 6 | A procedural error. LAPM stops the connection. |
| P2 more than 250 | We use the lower value (our 34). We do not stop the connection. |
| A parameter that the XID does not have | The default: 512 or 6 |

## Corrections (2026-11)

| Item | Before | Now |
|---|---|---|
| P1 less than 512, P2 less than 6 (5.1) | The link continued with no compression | A procedural error. The connection stops. |
