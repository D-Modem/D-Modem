# V.54: technical details

This page gives the loops and the signals of V.54 (11/88). For a
summary, refer to [V.54](v54.md).

## The loops (3)

| Loop | Location | What it does |
|---|---|---|
| 1 | In the DTE | Circuit 103 goes to circuit 104 in the DTE. Circuit 105 is OFF. |
| 3 | In DCE A, near the line | The signal of the modulator goes into the demodulator. No signal goes to the line. |
| 2 | In DCE B | Circuit 104 goes to circuit 103 in DCE B. To DTE B: circuit 104 is a steady binary 1, and circuits 106, 107 and 109 are OFF. |
| 4 | On a 4-wire line | The two pairs are connected through an attenuator, away from the DCE. |

Only one loop can be closed in a DCE at a time. Circuit 142 tells the
DTE that a loop is closed.

## The control of the loops (4)

- Manual control: a switch on the DCE.
- Automatic control through the interface: circuit 140 for loop 2 and
  circuit 141 for loop 3.
- Automatic control through the data: the four phases of clauses 5 to 8.

Do not use manual and automatic control together. A manual release has
priority.

## The patterns of synchronous DCEs (5)

The patterns are at the data rate, as if they come from circuit 103.
They are scrambled with 1 + x^-4 + x^-7 (Figure 3).

| Phase | Sent by | Pattern |
|---|---|---|
| Preparatory (5.1) | DCE A | 2048 ± 100 bits of scrambled binary 0 |
| Address (5.2.1) | DTE A | An address octet, 16 times or more (multipoint only) |
| Acknowledgement (5.2.2) | DCE B | 1948 ± 100 bits of scrambled binary 1 |
| Test (5.3) | DTE A | Not specified |
| Termination (5.4) | DCE A | 8192 ± 100 bits of scrambled binary 1, then 64 binary 1s |

- DCE A starts the test phase 2148 + 100 bits after it identifies the
  acknowledgement.
- The DCE sets its own recognition criteria. The criteria for the
  preparatory pattern must include the seven binary 1s in a row of the
  pattern, so that HDLC flags do not look like it.
- DCE B opens the loop when it identifies the termination pattern, when
  the carrier is lost for more than 1 s, or when its optional timer T1
  expires.
- A DCE that is not in a test does nothing when it receives the
  acknowledgement or the termination pattern.

## Point-to-point circuits (6)

On a point-to-point circuit with one loop 2, there is no address. DCE B
sends the acknowledgement when it identifies the preparatory pattern.

## Tandem circuits (7)

The four phases also control the loops of the modems at an intermediate
site. A loop 3 there removes the carrier to DCE B. After 1 s, DCE B
opens its loop.

## Asynchronous DCEs (8)

Start-stop DCEs use simple signals in place of the scrambled patterns:

| Phase | Signal |
|---|---|
| Preparatory | SPACE, MARK and SPACE, each for 320 ms to 400 ms |
| Acknowledgement | DCE B closes the loop, and stops its carrier for 100 ms to 150 ms |
| Termination | SPACE for 550 ms or more. DCE B opens the loop after 480 ms to 550 ms of it. |

## Modems with their own signals

V.22 and V.22 bis have their own signals for loop 2. They do not use
the patterns of V.54. Refer to [V.22](v22-details.md#tests-7).

## V.54 in D-Modem

| Item | D-Modem (`core/v54.cpp`) |
|---|---|
| Loops | Loop 3 (`AT&T1`, `AT&T8`) on a 4-wire leased line, and loop 2 (`AT&T3`, `AT&T6`, `AT&T7`, `+TRDL`, `+TLDL`) |
| Loops 1 and 4 | Not done (loop 1 is in the DTE, loop 4 is on the line) |
| Control | Through the data, on a call without error correction (`\N0`). `AT&T4` lets the far end close loop 2 here, and `AT&T5` does not. |
| Signals | The point-to-point signals of clause 6, at the data rate, between the link and the data pump |
| Recognition | A 256-bit block holds a pattern when 240 or more bits descramble to the bit of the pattern, and the raw bits have a run of seven of the other value. Preparatory after 4 blocks in a row, acknowledgement after 3, termination after 16. |
| Wait for the acknowledgement | 6144 bits after the preparatory pattern, then ERROR |
| DCE B | The data of DTE B is cut while the loop is closed. The loop opens on the termination pattern, or the call ends when the carrier is lost for 1 s. |
| V.22 and V.22 bis | The signals of the pump (7.1 of each), not the patterns of V.54 |
| Asynchronous DCEs (clause 8) | Not done. The FSK modulations (V.21, V.23) use the patterns of clause 6. |
| Circuit 142 | No interface circuit. `+TRDLS` and `+TDLS` give the state. |
