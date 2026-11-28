# V.32 bis: technical details

This page gives the technical details for [V.32 bis](v32bis.md). The
clause numbers are the clause numbers of V.32 bis (02/91). This page is
our own description, not a copy of the Recommendation. The items that
are the same as in V.32 are on [the V.32 page](v32-details.md).

## The signal (2)

These items are the same as in V.32: the carrier (1800 Hz ± 1 Hz), the
symbol rate (2400 baud ± 0.01 %), the spectrum and the scramblers.

| Rate | Bits each symbol | Points | Code |
|---|---|---|---|
| 14400 bit/s | 6 | 128 | Trellis |
| 12000 bit/s | 5 | 64 | Trellis |
| 9600 bit/s | 4 | 32 | Trellis |
| 7200 bit/s | 3 | 16 | Trellis |
| 4800 bit/s | 2 | 4 (A, B, C, D) | Differential only |

For the trellis rates, Q1 and Q2 go through the differential code of
Table 1. The convolutional encoder of Figure 1 (eight states) makes the
redundant bit Y0 from Y1 and Y2. Y0, Y1, Y2 and the uncoded bits Q3 to
Q6 select the point (Figures 2-1 to 2-4). At 4800 bit/s, the code is the
code of V.32 (Table 2).

V.32 bis has no 9600 bit/s mode without the trellis code. A V.32 bis
modem that operates with a V.32 modem uses the rules of V.32.

## The rate word (5.3, Table 5)

| Bits | Meaning |
|---|---|
| B0 to B3 | 0000. In E: 1111. |
| B7, B11, B15 | 1: the synchronization bits |
| B4, B8 | 1 in all the words of V.32 bis |
| B5 | 4800 bit/s |
| B6 | 9600 bit/s |
| B9 | 7200 bit/s |
| B10 | 12000 bit/s |
| B12 | 14400 bit/s |
| B13, B14 | 0. Reserved: a receiver ignores them. |
| B5, B6, B9, B10, B12 all 0 | A request to clear the call (a GSTN cleardown) |

If B4 or B8 is 0 in a word that a modem sends or receives, the two
modems use V.32 only. In E, only the rate of the data that follows is 1.

## Start-up and retrain (5, 6, 7)

The start-up and the retrain are the same as in V.32 (Figures 3 and 4).
These items are added:

- R2 contains only the rates of R1. R3 contains one rate of R2.
- If R3 asks for a cleardown, the answerer sends R3 for at least 64
  symbols before it clears the call (Note 6).
- Circuit 107 stays ON during a retrain.

## Rate renegotiation (8, Figure 5)

Either modem can change the rate during the data, without a retrain.

### The preamble and the words

| Item | Caller | Answerer |
|---|---|---|
| Preamble | AA for 56 symbols, then CC for 8 symbols | AC for 56 symbols, then CA for 8 symbols |
| Rate signal | Starts with a scrambler of zeros. The differential code starts from the last point of the preamble. | The same |

### The initiator (8.1)

1. It turns circuit 106 OFF. It sends its preamble, then R4. R4 gives
   the rate that it wants and all the lower rates that it can use.
2. When the far preamble comes, it holds circuit 104 at ONE and waits
   for R5.
3. When R5 comes, and when R4 has continued for at least 64 symbols, it
   completes the current word of R4. Then it sends E with the fastest
   rate of R4 and R5.
4. It sends B1 for 24 symbols, with the trellis encoder at zero. Then it
   sends data.
5. 24 symbols after the far E, it releases circuit 104.

### The responder (8.2)

1. A modem in data always looks for a preamble. When one comes, it holds
   circuit 104 at ONE and waits for R4.
2. When R4 comes, it turns circuit 106 OFF and sends its preamble.
3. It sends R5 for 64 symbols. R5 gives the rate that it wants and all
   the lower rates that it can use, whatever R4 contains.
4. It sends E with the fastest rate of R4 and R5, then B1 for 24
   symbols, then data.
5. 24 symbols after the far E, it releases circuit 104.

If R4 or R5 asks for a cleardown, the modem sends E for at least 64
symbols and clears the call. It does the same if the two words have no
rate in common. If a
renegotiation fails, the far modem can be a V.32 modem that uses B4 for
a different function.

## Test loops (9)

The modem must have the test loops 2 and 3 of V.54.

## Automode (Annex A)

Annex A is the same as Annex A of V.32, but two times are different:

- The answerer sends USB1 for 3000 ms ± 50 ms (V.32: 1500 ms).
- A caller that hears USB1 waits more than 3100 ms before it continues
  with V.22 bis (V.32: 1600 ms).

## V.32 bis in D-Modem

| Item | D-Modem (`dsp/dp_v32.cpp`) |
|---|---|
| Rates | 14400, 12000, 9600, 7200 and 4800 bit/s. V.32terbo adds 16800 and 19200 bit/s with B13 and B14 (`AT+MS=V32T` only). |
| Trellis code and points | The tables of `dsp/qam_tables.cpp`. The V.17 receiver of the fax code uses the same tables, and `SpandspV17` tests them against spandsp. |
| Rate words | B4 = 1 and B8 = 1 in all the words, also at 4800 bit/s alone. To a far modem without B4 and B8, the words of V.32. |
| Rate selection | From the error of the receiver on TRN (`v32_min_snr`) |
| Preamble detection | 24 symbols in a row on one training point (AA), or on two opposite points in turn (AC), at any of the four phases of the data |
| Retrain detection | AA or AC for more than 128 symbols. Thus a preamble does not start a retrain. |
| Renegotiation | As in clause 8. A retrain if it is not complete in 3 s. |
| Cleardown | R3 for 64 symbols, then the call ends |
| Not implemented | The test loops of V.54, the E of a failed renegotiation (the modem retrains), the answerer's USB1 of Annex A |

The responder sends data until R4 comes, as clause 8.2 tells. The
initiator gives the far data to the DTE until the far preamble comes.

### Corrections (2026-11)

| Error | Correction |
|---|---|
| The renegotiation used S (128 symbols), S-bar and a short TRN in place of the preamble, and B1 for 128 symbols. The responder looked for S in the data. R5 contained only the rates of R4. Other modems answered with a retrain. | The preamble of clause 8 (AA and CC, or AC and CA), R4 and R5 from a scrambler of zeros, R4 and R5 for 64 symbols, and 24 symbols of B1. The responder looks for the preamble and sends data until R4 comes. R5 is independent of R4. |
| The far AA or AC started a retrain after 20 ms (48 symbols), shorter than a preamble. | A retrain needs more than 128 symbols of the tone (7.1, 7.2). |
| A word for 4800 bit/s alone had B8 = 0. A V.32 bis modem then uses V.32 only. An E for 4800 bit/s with B8 = 1 was not accepted. | B8 = 1 in all the words. The receiver accepts E with B8. |
| A cleardown R3 was not detected, and the answerer did not send one. | The two modems detect a cleardown. The answerer sends R3 for 64 symbols, then clears the call. |

The tests `V32bis.Renegotiation`, `V32.RateWords` and `V32.Cleardown`
(`dsp/test/pump_test.cpp`) check these corrections. In `Core.Mnp10Climbs`,
MNP 10 renegotiates up from 4800 bit/s.
