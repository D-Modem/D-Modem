# V.8: technical details

This page gives the technical details for [V.8](v8.md). The clause
numbers are the clause numbers of V.8 (11/2000). This page is our own
description, not a copy of the Recommendation.

## Terms

| Term | Meaning |
|---|---|
| Call modem | The modem that dials. V.8 calls it the "call DCE". |
| Answer modem | The modem that answers. V.8 calls it the "answer DCE". |
| V.21(L) | The low channel of V.21: 300 bit/s FSK. The call modem sends on it. |
| V.21(H) | The high channel of V.21: 300 bit/s FSK. The answer modem sends on it. |
| ANS | The plain answer tone of V.25: 2100 Hz. |
| ANSam | The answer tone of V.8: 2100 Hz with amplitude modulation. |
| CI | Call indicator. An optional signal from the call modem that tells the type of call. |
| CM | Call menu. The list of functions of the call modem. |
| JM | Joint menu. The list of functions that the two modems share. |
| CJ | The end of CM. Three octets of zeros. |
| sigC, sigA | The first signal of the selected modulation, from the call modem (sigC) and from the answer modem (sigA). |

## The signals (7)

### ANSam

ANSam is a 2100 Hz sine wave. A 15 Hz sine wave modulates its amplitude.
The amplitude changes between 0.8 and 1.2 times its mean value.

A modem that knows only V.25 identifies ANSam as a usual answer tone. A V.8
modem identifies the 15 Hz modulation. Thus, ANSam tells a V.8 modem that
the answer modem can do V.8, and it does not cause problems for older
modems.

ANSam can also have phase reversals each 450 ms. The phase reversals
disable the echo cancellers in the network. The answer modem adds them
only if the network echo cancellers must be disabled. V.34 and the PCM
modems use their own echo cancellers, so they usually ask for phase
reversals.

If the answer modem does not receive CM or a sigC, it stops ANSam after
about 5 seconds.

### CI, CM and JM

CI, CM and JM use the same format. Each signal is a sequence of bits that
the modem sends again and again, at 300 bit/s:

1. Ten ONE bits.
2. Ten synchronization bits. These bits identify the type of sequence.
3. One or more octets of information. Each octet has a start bit (ZERO)
   before it and a stop bit (ONE) after it, as in asynchronous data.

| Synchronization bits (in the sequence of transmission) | Used for |
|---|---|
| `0000000001` | CI |
| `0000001111` | CM and JM |
| `0101010101` | V.92 (refer to V.92) |

The call modem sends CI and CM on V.21(L). The answer modem sends JM on
V.21(H). The fax procedures of T.30 also use V.21(H), with HDLC frames.
Thus, the format of V.8 prevents the HDLC flag (`01111110`) in all of the
bit stream. A fax receiver does not decode a JM as a fax frame.

### CJ

CJ is three octets of zeros, each with its start bit and its stop bit. The
call modem sends CJ to stop CM and to tell the answer modem that it
received JM.

## The procedure (8)

This is the usual sequence of a V.8 call:

1. The call modem dials. It sends no signal for 1 second. Then it can send
   CI, a call tone (CT of V.25 or CNG of T.30), or no signal.
2. The answer modem connects to the line. It sends no signal for at least
   0.2 seconds.
3. The answer modem sends ANSam.
4. The call modem detects ANSam and stops its call signal.
5. The call modem sends no signal for a time Te. Te is at least 0.5
   seconds. If the network echo cancellers must be disabled, Te is at
   least 1 second.
6. The call modem sends CM again and again.
7. The answer modem receives two identical CM sequences. It stops ANSam and
   sends JM again and again.
8. The call modem receives two identical JM sequences. It completes the
   current octet. Then it sends CJ.
9. The answer modem receives CJ and stops JM immediately. It does not
   complete the current JM sequence.
10. Each modem sends no signal for 75 ms (± 5 ms). Then the call modem
    sends sigC and the answer modem sends sigA. The selected modulation
    continues the call.

```
Call modem    (1 s)  CI CI ...        (Te)  CM CM CM ... CJ  (75 ms) sigC
Answer modem  (0.2 s)        ANSam ................  JM JM JM ...  (75 ms) sigA
```

### Other cases

- If the call modem receives ANS and not ANSam, the answer modem cannot do
  V.8. The call modem then uses an older start-up, for example Annex A of
  V.32bis or T.30.
- The answer modem can receive the sigC of a modulation during ANSam. An
  example is the signal of a V.32 call modem. Then the answer modem can
  start that modulation without V.8.
- If the answer modem receives no CM and no sigC, it stops ANSam and uses
  an older start-up.
- If JM shows no shared modulation, the two modems can disconnect after CJ.

### Rules for the JM

The answer modem makes the JM from the CM that it receives:

- The call function is the same as in CM. If the answer modem cannot do
  that function, JM can show a different function. In that case, JM shows
  no modulations.
- The modulations are the modulations that are in CM and that the answer
  modem can also use. If there are no shared modulations, JM has the same
  number of modulation octets as CM, with all bits set to zero.
- The answer modem includes a protocol octet only if CM has one.
- The answer modem includes the PCM octet only if CM has one.

## The octets of information (5, 6)

### Two types of octet

There are two types of octet:

- **Category octet.** It starts a new category of information. Bits b0 to
  b3 are a tag that identifies the category (b0 is the least significant
  bit). Bit b4 is ZERO. Bits b5, b6 and b7 are three option bits.
- **Extension octet.** It adds five more option bits to the category
  before it. Bits b3 and b5 are ZERO. Bit b4 is ONE. The option bits are
  b0, b1, b2, b6 and b7.

The bits go on the line in the sequence b0 to b7. The fixed ZERO bits make
sure that the bit stream never has six ONE bits in a sequence. Thus, an
HDLC flag cannot occur.

In the table that follows, the "value" is the octet as a hex number, with
b0 as the least significant bit. This is how a program usually stores it.

### The categories

| Category | Tag (b0 to b3) | Value of the tag | Contents |
|---|---|---|---|
| Call function | `1000` | `0x01` | The type of call. This category is always first. |
| Modulation modes | `1010` | `0x05` | The modulations that the modem can use. |
| Protocols | `0101` | `0x0A` | The error correction protocol (LAPM of V.42). |
| PSTN access | `1011` | `0x0D` | The type of telephone line at each end. |
| PCM modem availability | `1110` | `0x07` | The PCM modes of V.90, V.91 and V.92. |
| Non-standard facilities | `1111` | `0x0F` | Data of a manufacturer. |
| T.66 | `0111` | `0x0E` | Codes for fax (refer to T.66). |

### Call function

The three option bits (b5, b6, b7) give the type of call:

| b5 b6 b7 | Type of call | Octet value |
|---|---|---|
| `011` | Data, with a V-series modem | `0xC1` |
| `001` | Fax: the call terminal sends | `0x81` |
| `101` | Fax: the call terminal receives | `0xA1` |
| `010` | Text telephone (V.18) | `0x41` |
| `100` | Multimedia terminal (H.324) | `0x21` |
| `110` | Videotex (T.101) | `0x61` |
| `111` | The type is in an extension octet | `0xE1` |

### Modulation modes

The first octet of this category and two extension octets have one bit for
each modulation:

| Octet | Bit | Meaning when the bit is ONE |
|---|---|---|
| First (category) | b5 | A PCM modem availability octet is also in the menu. |
| First (category) | b6 | V.34 duplex |
| First (category) | b7 | V.34 half-duplex (for fax) |
| Second (extension) | b0 | V.32bis or V.32 |
| Second (extension) | b1 | V.22bis or V.22 |
| Second (extension) | b2 | V.17 |
| Second (extension) | b6 | V.29 half-duplex (for fax) |
| Second (extension) | b7 | V.27ter |
| Third (extension) | b0 | V.26ter |
| Third (extension) | b1 | V.26bis |
| Third (extension) | b2 | V.23 duplex |
| Third (extension) | b6 | V.23 half-duplex |
| Third (extension) | b7 | V.21 |

A modem can stop the category after the first or second octet if it has no
modulations in the next octets.

### Selection of the modulation

If the JM has a PCM modem availability octet, the call continues as V.90,
V.91 or V.92. If not, the two modems use the first modulation in the
table above that the JM shows. Thus, V.34 duplex has the highest priority,
and V.21 has the lowest.

### Protocols

If the option bits are `100` (b5 = 1), the modem asks for LAPM (V.42). If
the CM and the JM both show LAPM, V.42 can skip its own detection phase
(the ODP and ADP exchange). Some modems show LAPM here but still need the
ODP and ADP exchange. Thus, do not skip that phase unless the
modulation tells you to skip it (for example, V.92).

### PSTN access

| Bit | Meaning when the bit is ONE |
|---|---|
| b5 | The call modem is on a mobile telephone connection. |
| b6 | The answer modem is on a mobile telephone connection. |
| b7 | This modem is on a digital network connection (for example, an ISDN B-channel). |

### PCM modem availability

| Bit | Meaning when the bit is ONE |
|---|---|
| b5 | The modem can be the analogue modem of V.90 or V.92. |
| b6 | The modem can be the digital modem of V.90 or V.92. |
| b7 | The modem can do V.91. |

If a menu has this octet, these rules apply:

- The menu must also have a PSTN access octet.
- Bit b5 of the first modulation octet must be ONE.
- If b5 or b6 is ONE, the V.34 duplex bit must also be ONE. V.90 and V.92
  use V.34 for their start-up and for the direction that is not PCM.

### Non-standard facilities

A manufacturer can add its own data after the standard octets. The block
gives a length, a country code from T.35, a manufacturer code and the
data. The data goes in extension octets, five bits in each octet.

## Rules for compatibility (10)

- A receiver must ignore each category, octet and bit that it does not
  know. Later editions of V.8 can add new codes.
- A manufacturer must put its own data in the non-standard facilities
  category only. Other private codes can cause problems with later
  editions.
- A modem that does not use CI must not fail if it receives CI.

## V.8 in D-Modem

`dsp/dp_v34.cpp` has the menus that D-Modem sends. Each menu in the code
starts with the octet `0xE0`. With a start bit and a stop bit, this octet
makes the synchronization bits `0000001111`. The ten ONE bits go before
it.

| Menu | Octets after the preamble | Contents |
|---|---|---|
| `v8_msg` (data) | `0xC1 0x45 0x13 0x94 0x2A 0x0D` | Data call. V.34 duplex. V.32bis, V.22bis. V.23 duplex, V.21. LAPM. PSTN access (analogue). |
| `v8_fax_msg` (fax, V.34 half-duplex) | `0x81 0x85 0xD4 0x90 0x0D` | Fax call (the code changes the call function to `0xA1` for a poll). V.34 half-duplex. V.17, V.29, V.27ter. V.21. PSTN access. |
| `v8_x2c_msg` (the x2 client) | `0xC1 0x45 0x10 0x10 0x0D` | Data call. V.34 duplex only. No protocol octet. PSTN access. This is the CM of a Courier. |

With V.90 or V.92, the code sets bit b5 of the first modulation octet and
adds a PCM modem availability octet (`V8_CAT_PCM`). It also sets the
digital network bit of the PSTN access octet on a digital line.

As the answer modem, D-Modem makes its JM from the CM that it receives:

- The modulation octets show only the modulations that are also in the CM.
- The PCM modem availability octet shows only the PCM modes that agree
  with the CM. For example, the digital modem bit is ONE only if the CM
  shows the analogue modem.
- The protocol octet is in the JM only if the CM asks for LAPM.
- Bit b5 of the PSTN access octet is the same as bit b5 in the CM.

### Corrections (2026-11)

A check against V.8 found three errors. They are now correct:

| Error | Correction |
|---|---|
| The second modulation octet of `v8_fax_msg` was `0x74`. This value set bit b5, which must be ZERO in an extension octet. Also, it did not show V.27ter. | The octet is `0xD4`: V.17, V.29 and V.27ter. |
| The JM always had a protocol octet (LAPM), also when the CM had no protocol octet. | The JM has the protocol octet only if the CM asks for LAPM (Table 6, 7.4). |
| Bit b5 of the PSTN access octet in the JM was always ZERO. | The JM copies bit b5 from the CM (7.4). |

The test `Core.V8MenusConform` (`core/test/core_test.cpp`) makes a V.34
call between two D-Modems. It examines the CM and the JM in `AT#UD`.
