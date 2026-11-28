# V.44: technical details

This page gives the technical details for [V.44](v44.md). The clause
numbers are the clause numbers of V.44 (11/2000). This page is our own
description, not a copy of the Recommendation. The details of the
D-Modem implementation are on [the data link layer: technical
details](../link-details.md).

## The parameters (8, Table 10)

| Parameter | Meaning | Range | Default |
|---|---|---|---|
| C0 | The method of negotiation: in XID (N = 0), or after the link is set up (N = 1) | | |
| P0 | The directions of compression: none, transmit, receive or both | 00 to 11 | 11 |
| P1T, P1R (N2) | The number of codewords, with the control codes | 256 to 65535 | 1024 |
| P2T, P2R (N7) | The maximum length of a string, in characters | 32 to 255 | 255 |
| P3T, P3R (N8) | The size of the history, in characters | 512 or more | 3 × P1 |

- T is the direction of the encoder of the modem that sends the XID. R
  is the direction of its decoder (7.4).
- The two modems compare the P2T of one modem with the P2R of the other
  modem, and use the lower value. The same rule applies to P1 and P3.
  Thus, the two directions can have different values.
- A value less than the minimum is a procedural error. The connection
  stops.
- The response to P0 = 01 is P0 = 10 (or 00). Only the directions that
  the first modem asks for are possible.

## The constants and the variables (Tables 11 and 12)

| Name | Meaning | Value |
|---|---|---|
| N1 | The maximum size of a codeword, in bits | From N2 |
| N3 | The character size | 8 bits |
| N4 | The number of characters | 256 |
| N5 | The number of control codes, and the first codeword | 4 |
| C1 | The next codeword | N5 at the start |
| C2 | The size of a codeword or a control code | 6 bits at the start |
| C3 | The threshold for STEPUP (2 to the power C2) | 64 at the start |
| C4 | The position in the history | 0 at the start |
| C5 | The size of an ordinal | 7 bits at the start |

## The codes (6.6, Tables 3 to 5, 8 and 9)

Each code has a prefix. The bits go least significant bit first.

| Code | Prefix after a codeword | Prefix at other times | Bits after the prefix |
|---|---|---|---|
| Control code | 1 | 1 | C2 |
| Codeword | 1 | 1 | C2 |
| Ordinal (one character) | 00 | 0 | C5 |
| String-extension length | 01 | Not possible | 1 to 12 bits |

| Control code | Function |
|---|---|
| 0 ETM | Enter transparent mode. Zero bits follow to the octet boundary. |
| 1 FLUSH | Flush the data. Zero bits follow to the octet boundary. |
| 2 STEPUP | The next codeword (C2) or the next ordinal (C5) has one more bit |
| 3 REINIT | The dictionary and the history start again |

| Transparent mode command (after the escape character) | Function |
|---|---|
| 0 ECM | Enter compressed mode. The dictionary starts again. |
| 1 EID | The escape character is in the data |
| 2 EPM | Enter parameter mode (7.4.2) |

- The escape character is 0 at the start. In transparent mode, each
  time it occurs in the data, it increases by 51 (modulo 256). In
  compressed mode, it does not change (7.14).

### The string-extension length (Tables 3 and 4)

| Length | Subfields |
|---|---|
| 1 | 1 |
| 2 to 4 | 0, then the length − 1 in 2 bits |
| 5 to 12 | 0, 00, 0, then the length − 5 in 3 bits |
| 13 or more | 0, 00, 1, then the length − 13 in 5 to 8 bits (from N7) |

- The maximum length of an extension is the lower of 253 and N7 − 2.

## The encoder (6.3)

1. **String matching (6.3.1).** The encoder finds the longest string in
   the dictionary that starts with the input. If there is no string, it
   sends the ordinal of the first character.
2. **String extension (6.3.2).** After a codeword, the encoder compares
   the next characters with the characters that followed the string in
   the history. If one or more are the same, it sends their number as a
   string-extension length.
3. **New strings (6.3.3).** The unmatched character makes a new string
   from the previous string. A successful extension makes a new string
   from the codeword and the extension. A string is not longer than N7.

## The decoder (6.4)

- An ordinal or a codeword less than C1: the decoder writes the
  characters to the output and to the history.
- A codeword equal to C1: the string of the previous code, then its
  first character.
- A codeword more than C1: a procedural error (7.15).
- After FLUSH and STEPUP, the next code makes a new string with the code
  before them (Table 2).

## The autonomous adjustments (7.11)

| Event | Action |
|---|---|
| An ordinal of 128 or more, and C5 is 7 | STEPUP, then C5 is 8 |
| A codeword equal to or more than C3 | STEPUP, C2 increases by 1, C3 doubles. Again if necessary. |
| C1 = N2 (the dictionary is full) | REINIT |
| C4 = N8 (the history is full) | The codes for the characters in the history, then REINIT |
| The data does not compress | ETM, then transparent mode. The test method is not specified. |
| The data compresses again | The escape character and ECM, then a new dictionary |

The decoder sends C-ERROR (7.15), and the connection stops, on these
errors:

- A STEPUP that makes C2 more than N1.
- A STEPUP that makes C5 more than 8.
- A codeword more than C1.

## V.44 in D-Modem

| Item | D-Modem (`link/v44.cpp`) |
|---|---|
| P1, P2, P3 that we offer | 4096 codewords, strings of 255 characters, a history of 12288 |
| C0 | N = 0: negotiation in XID |
| Parameters for each direction | Our encoder uses our P1T and the P1R of the far modem. Our decoder uses our P1R and the P1T of the far modem. The same for P2 and P3. |
| A parameter that the XID does not have | The default of Table 10 |
| A parameter less than its minimum | A procedural error. LAPM stops the connection. |
| String matching | A hash of the first two characters, not the tree of 6.2. The result is the same: the longest string. |
| Compressibility test | The bits of the codes for the last 256 characters, against 8 bits for each character. Transparent mode above 2080 bits, compressed mode below 1984 bits. The mode stays for 512 characters after a change. FLUSH and EID count in the test. |
| FLUSH | When the DTE has no more data for now |
| EPM | We never send it. When we receive it, it is a procedural error, because our C0 asks for XID negotiation. |
| Preset dictionary | Only with V.cvm. Not in V.44. |

## Corrections (2026-11)

| Item | Before | Now |
|---|---|---|
| Compressibility test (7.11.5) | The encoder did not have a test. It stayed in compressed mode. Noise became 12 % larger. | The encoder has the test above. Noise is 0.5 % larger. With a FLUSH after each character, the data is half as large as before. |
| EPM (Table 9) | The decoder ignored EPM, and then sent the parameters to the DTE as data | A procedural error |
| Parameters for each direction (7.4) | We used the lowest of all the P1 values for the two directions. With a far modem that has different values for T and R, our decoder and its encoder did not agree. | Each direction has its own values |
| Defaults (Table 10) | A parameter that the XID did not have had no limit. We used our own value, not the default. | The default |
| Values less than the minimum (7.4) | The link continued with no compression | A procedural error. The connection stops. |
