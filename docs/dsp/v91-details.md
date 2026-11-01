# V.91 digital to digital: technical details

This is the technical companion to [V.91: digital to digital](v91.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

## Overview

`dsp/v91.cpp` implements ITU-T V.91 (05/99): a modem on a 4-wire digital
connection talking to another digital modem. Typical cases are D-Modem
calling D-Modem, or D-Modem calling the Asterisk module over SIP. Both
ends send PCM codes (G.711 octets as exact values), and both run the same
start-up (8.2.1/V.91).

The V.34 pump (`dp_v34.cpp`) hands over to V.91 at the end of V.8 when
both the CM and the JM have the V.91 bit (b7, `V8_PCM_V91`) set in their
PCM modem availability octet (`v8_v91()`). The pump only offers V.91
when:

- `+MS` selects V.90, V.92 or V.91;
- the line runs at 8000 Hz (a digital network's line, not an analogue
  one);
- `DSP_V91` is not set to `0`.

If V.91 fails to start, both modems fall back to V.34 (see
[Escape to V.34](#escape-to-v34) below).

## Start-up sequence

The terms used below:

- **Ucode**: a PCM code number from 0 to 127 (the G.711 octet without its
  sign), as V.90 defines it.
- **DIL** (digital impairment learning): a sequence of known codes that
  lets the receiver see what the network does to each code.
- **Data frame interval**: one of the 6 symbol positions in a data frame.

The steps, as each modem sends them:

1. **Ez**: 24 symbols of −Ucode 66 (after V.8's silence).
2. **INFO and INFO'**: INFO (Table 6/V.91) is 62 bits, coded
   differentially into the sign of Ucode 66 and repeated. Once the
   remote's INFO has arrived, the modem sends INFO' instead (INFO with
   bit 28 set).
   - We always ask for the default DIL (Table 5/V.91, bit 26 clear).
   - We never ask for transparent mode (bit 40 clear). Through dmodemd's
     16-bit linear audio, the µ-law codes +0 and −0 become the same
     value, so transparent mode cannot work.
   - Bit 39 gives our G.711 law; bit 27 asks for the control channel
     (see [Data mode](#data-mode)).
3. **Eu, or J + PHIL + Em**: the remote decides which DIL it wants from
   us.
   - **Eu** (12 zeros, coded as INFO is) means the default DIL follows.
   - **J** (Table 12/V.90, the same format as V.90's Ja) describes a DIL
     of the remote's own. It is followed by **PHIL** (scrambled ones) and
     **Em** (12 scrambled zeros). We then send the DIL that J described.
4. **DIL** (as 8.4.1/V.90). While the remote's DIL comes in, the
   receiver records, for each of the 6 data frame intervals, the value
   that each code and sign arrives as.
5. **SCR**: scrambled ones, until the remote's DIL has been analysed.
6. **CP and CP'**: CP (Table 4/V.91) has the same layout as V.90's CP.
   It carries the constellations we can receive and the rate. CP' (CP
   with its acknowledge bit set) follows once the remote's CP has
   arrived.
7. **Es and B1**: Es is 12 scrambled zeros; B1 is two data frames of
   ones.
8. **Data mode** (below).

### What the DIL analysis keeps

For each data frame interval, the receiver (`V91::choose()`) goes
through the codes from the loudest down. A code is usable when:

- both of its signs were seen, and each arrived as the same value every
  time;
- its two signs arrived as different values; and
- neither value is already taken by a louder code that was kept.

Where the network makes several codes arrive as one value (robbed bits,
a digital loss pad, or a conversion between µ-law and A-law), only one
of them is kept. The codes kept are the constellations the receiver asks
for in its CP. In data mode, each received value is decoded as the code
whose DIL value is nearest (`V91Map::decode()`).

### The rate

K is the number of bits per data frame that go through the modulus
encoder. It is the most that the product of the six intervals'
constellation sizes can carry. K is at most 41, which gives
(41 + 6) × 8000 / 6 = 62667 bit/s. 64000 bit/s would need all 128 codes
in every interval, and so transparent mode. A K below 15 means V.91
cannot run.

## Data mode

The transmitter is the V.90 encoder (5.4/V.90) with Sr = 0 (no spectral
shaping) and S = 6: K bits through the modulus encoder and 6 sign bits,
coded differentially (`V91Enc`). The receiver is its inverse (`V91Dec`).

The **control channel** (bit 27 of INFO) uses the sign bits of every
64th frame. It is sent only when the remote asks for it. Setting
`DSP_V91_CC` (to any value) makes our INFO ask for it.

The core's DC remover leaves the received codes alone while V.91 runs:
the pump reports exact PCM through `dp_runtime_pcm()`.

## Retrains

A retrain in V.91 means sending INFO again. V.42 carries on across a
retrain. The modem starts one when:

- more than 5% of the symbols in a second (400 of 8000) are off the
  constellation; or
- in data mode, the same value is received 6 ms in a row. That happens
  with silence, or with the idle code of a digital circuit that has been
  cut; scrambled data never does it.

The modem also answers the remote's INFO when it arrives in data mode.
If a retrain does not reach data mode again within 10 s, the call ends.

The DTE's retrain and rate renegotiation (`ATO1`, `ATO3`) are refused
with V.91 (`Modem::link_req()` in `core/modem.cpp`).

## Escape to V.34

If there is no INFO' within 1.5 s, or no data mode within 10 s, the
modems go to V.34 phase 2, starting from INFO0 (8.3/V.91).

## 4-wire leased circuits

On a 4-wire digital leased circuit (8.8/V.91, `AT&L2+MS=V91`), the pump
starts V.91 at once. There is no V.8 and no Ez: the modem sends INFO
until the far end's INFO arrives, then has 10 s to reach data mode.
There is no escape to V.34; the call ends instead. See
[leased lines](../leased.md).

## Settings

| Variable | Effect |
|---|---|
| `DSP_V91=0` | leaves V.91 out of V.8 |
| `DSP_V91_K=<n>` | limits K to n (1 to 41; other values mean 41) |
| `DSP_V91_CC` | set to anything: ask for the control channel |
| `DSP_V91_RETRAIN=<ms>[a\|c]` | test: our own retrain that long after connecting (`a` or `c`: only the answerer or the caller) |
| `DSP_V91_MUTE=a\|c` | test: that modem sends nothing, so both escape to V.34 |

## Tests

`dmodem_test` puts a G.711 network between two D-Modems (the `V91`
suite in `dsp/test/dmodem_test.cpp`; run them with `ctest -R v91`):

| Test | Network | Codes per interval | Rate |
|---|---|---|---|
| `v91_g711` | G.711, µ-law | 124 (+0 and −0 are one) | 62667 |
| `v91_alaw` | G.711, A-law | 125 | 62667 |
| `v91_rbs` | robbed bits: the LSB of every 6th octet set to 1 | 124, and 62 in one interval | 61333 |
| `v91_pad` | a 3 dB pad, µ-law (A-law) | 99 (105) | 60000 (61333) |
| `v91_xlaw` | the caller on µ-law, the answerer on A-law, converted | 117 | 62667 |

The tests check a rate of at least 60000 bit/s (62666 for `v91_alaw`);
the exact rates above are what the calls reach. Other cases in the same
suite:

- `v91_bulk`: with the default `AT+MS=92`, 200 kB of data that does not
  compress goes across V.42 at about 58700 bit/s. (V.34 at 33600 bit/s
  manages 31400.)
- `ms56`: K56flex's `AT+MS=56` finds V.91 as well.
- `v91_retrain`: the caller retrains 3 s into the data; V.42 carries on.
- `v91_cc`: the control channel, on the robbed-bit network.
- `v91_escape`: one modem silent in V.91; both escape to V.34 at 33600.

Over SIP, `frontend/asterisk/test/sip.sh` (dmodemd calling DModem in Asterisk,
also with `CODEC=alaw` and with `INCOMING=1`) connects at 62666/62666.
