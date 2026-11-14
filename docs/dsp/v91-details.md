# V.91 digital to digital: technical details

This page gives the technical details for [V.91: digital to digital](v91.md), the plain-English overview. Read the overview first. This page has the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

## Overview

`dsp/v91.cpp` implements ITU-T V.91 (05/99). This is a modem on a 4-wire
digital connection that communicates with a different digital modem.
Typical examples are a call from D-Modem to D-Modem, or a call from
D-Modem to the Asterisk module through SIP. The two ends send PCM codes
(G.711 octets as exact values). The two ends use the same start-up
(8.2.1/V.91).

At the end of V.8, the V.34 pump (`dp_v34.cpp`) gives control to V.91 if
the CM and the JM have the V.91 bit (b7, `V8_PCM_V91`) set in their PCM
modem availability octet (`v8_v91()`). The pump offers V.91 only in these
conditions:

- `+MS` selects V.90, V.92 or V.91.
- The line operates at 8000 Hz (the line of a digital network, not an
  analogue line).
- `DSP_V91` is not set to `0`.

If V.91 does not start, the two modems fall back to V.34 (refer to
[Escape to V.34](#escape-to-v34) below).

## Start-up sequence

This section uses these terms:

- **Ucode**: a PCM code number from 0 to 127 (the G.711 octet without its
  sign), as V.90 defines it.
- **DIL** (digital impairment learning): a sequence of known codes. With
  it, the receiver sees what the network does to each code.
- **Data frame interval**: one of the 6 symbol positions in a data frame.

Each modem sends these steps:

1. **Ez**: 24 symbols of −Ucode 66 (after the silence of V.8).
2. **INFO and INFO'**: INFO (Table 6/V.91) is 62 bits. It is coded
   differentially into the sign of Ucode 66 and repeated. When the INFO
   of the remote modem arrives, the modem sends INFO' in its place (INFO
   with bit 28 set).
   - We always ask for the default DIL (Table 5/V.91, bit 26 clear).
   - We never ask for transparent mode (bit 40 clear). The 16-bit linear
     audio of dmodemd changes the µ-law codes +0 and −0 into the same
     value. Thus, transparent mode cannot operate.
   - Bit 39 gives our G.711 law. Bit 27 asks for the control channel
     (refer to [Data mode](#data-mode)).
3. **Eu, or J + PHIL + Em**: the remote modem selects the DIL that it
   wants from us.
   - **Eu** (12 zeros, coded as INFO is) tells that the default DIL
     follows.
   - **J** (Table 12/V.90, the same format as the Ja of V.90) gives a DIL
     that the remote modem selects. **PHIL** (scrambled ones) and **Em**
     (12 scrambled zeros) follow it. Then we send the DIL that J gave.
4. **DIL** (as 8.4.1/V.90). While the DIL of the remote modem comes in,
   the receiver records the value at which each code and sign arrives,
   for each of the 6 data frame intervals.
5. **SCR**: scrambled ones, until the analysis of the DIL of the remote
   modem is complete.
6. **CP and CP'**: CP (Table 4/V.91) has the same layout as the CP of
   V.90. It contains the constellations that we can receive and the
   rate. CP' (CP with its acknowledge bit set) follows when the CP of the
   remote modem arrives.
7. **Es and B1**: Es is 12 scrambled zeros. B1 is two data frames of
   ones.
8. **Data mode** (refer to the subsequent section).

### What the DIL analysis keeps

For each data frame interval, the receiver (`V91::choose()`) examines the
codes from the loudest down. A code is usable in these conditions:

- The receiver saw its two signs, and each sign arrived at the same value
  each time.
- Its two signs arrived at different values.
- A louder code that the receiver kept does not already use one of these
  values.

The network can make some codes arrive at one value (robbed bits, a
digital loss pad, or a conversion between µ-law and A-law). In this
condition, the receiver keeps only one of them. The codes that the
receiver keeps are the constellations that it asks for in its CP. In data
mode, the receiver decodes each received value as the code whose DIL
value is nearest (`V91Map::decode()`).

### The rate

K is the number of bits in each data frame that go through the modulus
encoder. It is the maximum that the product of the constellation sizes of
the six intervals can send. The maximum K is 41. This gives
(41 + 6) × 8000 / 6 = 62667 bit/s. For 64000 bit/s, all 128 codes in
each interval are necessary, and thus transparent mode. If K is less than
15, V.91 cannot operate.

## Data mode

The transmitter is the V.90 encoder (5.4/V.90) with Sr = 0 (no spectral
shaping) and S = 6. K bits go through the modulus encoder, and 6 sign
bits are coded differentially (`V91Enc`). The receiver does the inverse
(`V91Dec`).

The **control channel** (bit 27 of INFO) uses the sign bits of each 64th
frame. The modem sends it only when the remote modem asks for it. If you
set `DSP_V91_CC` (to a value), our INFO asks for it.

While V.91 operates, the DC remover of the core does not change the
received codes. The pump tells the core about exact PCM through
`dp_runtime_pcm()`.

## Retrains

In V.91, a retrain is a new transmission of INFO. V.42 continues through
a retrain. The modem starts a retrain in these conditions:

- More than 5% of the symbols in a second (400 of 8000) are not on the
  constellation.
- In data mode, the modem receives the same value for 6 ms in a row. This
  occurs with silence, or with the idle code of a digital circuit that is
  cut. Scrambled data never does this.

The modem also answers the INFO of the remote modem when it arrives in
data mode. If a retrain does not get to data mode again in 10 s, the call
stops.

With V.91, the modem refuses the retrain and the rate renegotiation of the
DTE (`ATO1`, `ATO3`) (`Modem::link_req()` in `core/modem.cpp`).

## Escape to V.34

If there is no INFO' in 1.5 s, or no data mode in 10 s, the modems go to
V.34 phase 2. They start from INFO0 (8.3/V.91).

## 4-wire leased circuits

On a 4-wire digital leased circuit (8.8/V.91, `AT&L2+MS=V91`), the pump
starts V.91 immediately. There is no V.8 and no Ez. The modem sends INFO
until the INFO of the far end arrives. Then it has 10 s to get to data
mode. There is no escape to V.34. In its place, the call stops. Refer to
[leased lines](../leased.md).

## Settings

| Variable | Effect |
|---|---|
| `DSP_V91=0` | removes V.91 from V.8 |
| `DSP_V91_K=<n>` | sets the limit of K to n (1 to 41; other values give 41) |
| `DSP_V91_CC` | set to a value: ask for the control channel |
| `DSP_V91_RETRAIN=<ms>[a\|c]` | test: our own retrain at that time after the connection (`a` or `c`: only the answerer or the caller) |
| `DSP_V91_MUTE=a\|c` | test: that modem sends nothing. Thus, the two modems escape to V.34 |

## Tests

`dmodem_test` puts a G.711 network between two D-Modems (the `V91` suite
in `dsp/test/dmodem_test.cpp`). To run them, use `ctest -R v91`:

| Test | Network | Codes per interval | Rate |
|---|---|---|---|
| `v91_g711` | G.711, µ-law | 124 (+0 and −0 are one) | 62667 |
| `v91_alaw` | G.711, A-law | 125 | 62667 |
| `v91_rbs` | robbed bits: the LSB of every 6th octet set to 1 | 124, and 62 in one interval | 61333 |
| `v91_pad` | a 3 dB pad, µ-law (A-law) | 99 (105) | 60000 (61333) |
| `v91_xlaw` | the caller on µ-law, the answerer on A-law, converted | 117 | 62667 |

The tests do a check of a rate of 60000 bit/s or more (62666 for
`v91_alaw`). The exact rates above are the rates that the calls get. The
same suite has these other cases:

- `v91_bulk`: with the default `AT+MS=92`, 200 kB of data that does not
  compress goes through V.42 at about 58700 bit/s. (V.34 at 33600 bit/s
  gets 31400.)
- `ms56`: the `AT+MS=56` of K56flex also finds V.91.
- `v91_retrain`: the caller retrains 3 s into the data. V.42 continues.
- `v91_cc`: the control channel, on the robbed-bit network.
- `v91_escape`: one modem is silent in V.91. The two modems escape to
  V.34 at 33600.

Through SIP, `frontend/asterisk/test/sip.sh` connects at 62666/62666. In
this test, dmodemd calls DModem in Asterisk, also with `CODEC=alaw` and
with `INCOMING=1`.
