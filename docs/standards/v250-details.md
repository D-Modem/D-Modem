# V.250: technical details

This page gives the technical details for [V.250](v250.md). The clause
numbers are the clause numbers of V.250 (07/2003). This page is our own
description, not a copy of the Recommendation. The commands of D-Modem
are on [the core: technical details](../core-details.md).

## The command line (5.2)

- A command line starts with `AT` or `at` (`aT` and `At` are also
  correct). It ends with S3, the carriage return.
- Spaces in the line have no effect, but spaces in a string constant
  stay.
- S5 (the backspace) removes the last character from the line.
- With `ATE1`, the modem echoes each character that it receives in
  command state.
- `A/` runs the last command line again. It has no `AT` and no S3.
- The modem must accept a line of 40 characters or more.

## The syntax of the commands (5.3 and 5.4)

| Type | Form | Example |
|---|---|---|
| Basic command | A letter (or `&` and a letter), then an optional number | `ATE0`, `AT&C1` |
| Read an S-parameter | `S<n>?` | `ATS0?` gives `000` |
| Set an S-parameter | `S<n>=<value>` | `ATS0=2` |
| Extended action | `+<name>` or `+<name>=<values>` | `AT+GMI` |
| Set an extended parameter | `+<name>=<values>` | `AT+MS=V34` |
| Read an extended parameter | `+<name>?` | `AT+MS?` |
| Test an extended parameter | `+<name>=?` | `AT+MS=?` gives the ranges |

- A semicolon is necessary after an extended command, before the next
  command on the same line.
- The read of an S-parameter gives exactly three digits, with leading
  zeros (5.3.2).
- A value that is not correct gives one of three results (5.6.2). The
  results are: ERROR and no change, OK and no change, or OK and the
  nearest correct value. Each command can tell which result it uses.
- When a command gives ERROR, the modem does not run the remainder of the
  line.

## The result codes (5.7)

| Word (`ATV1`) | Number (`ATV0`) | Meaning |
|---|---|---|
| OK | 0 | The command line ran |
| CONNECT | 1 | A connection |
| RING | 2 | A ring signal from the line |
| NO CARRIER | 3 | No connection, or the end of the connection |
| ERROR | 4 | The line has an error |
| NO DIALTONE | 6 | No dial tone |
| BUSY | 7 | The busy signal |
| NO ANSWER | 8 | The `@` modifier did not find five seconds of quiet |

- With `ATV1`, a result code and information text start with S3 S4 and
  end with S3 S4. With `ATV0`, a result code ends with S3 only.
- `ATQ1` stops the result codes.
- Extended result codes (`+<name>: <values>`) have no number. `ATV0` and
  `ATQ1` have an effect on them, but `ATX` does not.

## The S-parameters (6.2 and 6.3)

| Register | Function | Range | Default |
|---|---|---|---|
| S0 | Answer on this ring (0: no automatic answer) | 0 to 255 | 0 |
| S3 | The end of a command line | 0 to 127 | 13 (CR) |
| S4 | The character after S3 in the responses | 0 to 127 | 10 (LF) |
| S5 | The backspace | 0 to 127 | 8 (BS) |
| S6 | The pause before a blind dial, in seconds | 2 to 10 | 2 |
| S7 | The time limit for the connection, in seconds | 1 to 255 | The manufacturer's |
| S8 | The pause for `,` in a dial string, in seconds | 0 to 255 | 2 |
| S10 | The time from the loss of carrier to the hang-up, in 0.1 s | 1 to 254 | The manufacturer's |

## The dial string (6.3.1)

| Item | Function |
|---|---|
| `0` to `9`, `*`, `#`, `A` to `D` | Digits to dial |
| `,` | Pause for S8 seconds |
| `T`, `P` | Tone dialling, pulse dialling |
| `!` | A hook flash |
| `W` | Wait for a dial tone |
| `@` | Wait for five seconds of quiet after the ring signal |
| `;` | Go back to command state after the dial |
| `S=<n>` | Dial the stored number n |

## The reports before CONNECT (6.2.13, 6.4.3, 6.5.5, 6.6.3)

If the parameter of a report is 1, the modem sends the report as an
intermediate result code before CONNECT. The order is the order of the
table.

| Parameter | Report | Values |
|---|---|---|
| `+MR=1` | `+MCR: <carrier>` | The modulation, for example `V34` |
| `+MR=1` | `+MRR: <rate>[,<rx rate>]` | The rate of the line, in bit/s |
| `+ER=1` | `+ER: <type>` | `NONE`, `LAPM`, `ALT` (MNP) |
| `+DR=1` | `+DR: <type>` | `NONE`, `V42B`, `V42B RD`, `V42B TD`, `V44`, `V44 RD`, `V44 TD` |
| `+ILRR=1` | `+ILRR: <rate>` | The rate of the DTE interface |

`RD` is compression in the receive direction only. `TD` is compression in
the transmit direction only.

## +GCAP (6.1.9)

`AT+GCAP` gives each group of extended commands that the modem has:
`+FCLASS` (fax), `+MS` (modulation), `+MV18S` (V.18), `+ES` (error
control) and `+DS` (compression).

## V.250 in D-Modem

| Item | D-Modem (`core/at.cpp`, `core/modem.cpp`) |
|---|---|
| The command line | 63 characters at most |
| S-parameter read | Three digits with leading zeros |
| A value out of range | ERROR, and the register does not change |
| `ATS<n>=` with no value | The register is set to 0 |
| S7, S10 defaults | 60 s, 0.7 s |
| `+MCR:` | The names of `+MS`, also our own names for modulations that V.250 does not have (for example `V91`) |
| `+ER:` | `LAPM` for LAPM and V.76. `ALT` for MNP. `NONE` for HST, PEP and all other modes |
| `+DR:` | `V42B` or `V44`. `NONE` for MNP 5, zstd and ADC, because V.250 has no word for them |
| `+ILRR:` | Only when `+IPR` has a fixed rate. A pty has no rate of its own |
| `+GCAP` | `+FCLASS,+MS,+MV18S,+ES,+DS` |
| Commands that we do not have | `+MA`, `+EB`, `+ESR`, `+ETBM`, `+EWIND`, `+ICLOK`, `+IRTS`, `+ASTO`, `+PQC`, `+PSS`, `+PMHF` (all optional) |

## Corrections (2026-11)

| Item | Before | Now |
|---|---|---|
| `+MR`, `+ER`, `+DR`, `+ILRR` (6.4.3, 6.5.5, 6.6.3, 6.2.13) | Accepted, but no reports | The reports before CONNECT |
| S-parameter read (5.3.2) | `13` | `013` |
| A value more than 255, or more than 127 for S3 to S5 (5.6.2, 6.2.1 to 6.2.3) | The low 8 bits were kept | ERROR, and no change |
| `ATS3=?` | S3 was set to 0. Then the modem did not find the end of a command line | ERROR, and no change |
| `+GCAP` (6.1.9) | `+FCLASS,+MS` | `+FCLASS,+MS,+MV18S,+ES,+DS` |
