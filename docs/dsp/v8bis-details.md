# V.8bis: technical details

The plain-English overview is [v8bis.md](v8bis.md). The references are
to ITU-T V.8 bis (11/2000) and V.251 (02/98).

## The parts

| Part | Where | What it does |
|---|---|---|
| `Tree`, `Msg`, `Caps` | `dsp/v8bis.h`, `v8bis.cpp` | The information field of clause 8: the message type and revision, the identification (I) and standard information (S) fields as parameter trees (8.2: NPar(1), SPar(1), one Par(2) block for each SPar(1) bit that is set, NPar(3) blocks for each SPar(2) bit that is set, with the delimiting bits 8 and 7). `Caps` is what a station has, or what an MS selects: the NPar(1) of the I field, the first SPar(1) octet of the S field, and the NPar(2) of the data and SVD modes (six bits in each octet). The code does not read the non-standard field |
| `SigTx`, `SigRx` | same | The signals of clause 7.1. Segment 1 is a dual tone for 400 ms: 1375 + 2002 Hz from the initiating station, or 1529 + 2225 Hz from the responding station. Segment 2 is a single tone for 100 ms (MRe 650, MRd 1150, CRe 400, CRd 1900, ESi 980, ESr 1650 Hz). ESi and ESr have no segment 2 of their own. Their segment 2 is the 100 ms of V.21 mark at the start of the message after them (7.2.4). The detector operates on 10 ms blocks: 180 ms of the requested pair, then, in 80 ms, the tone of segment 2 for 40 ms |
| `MsgTx`, `MsgRx` | same | The messages of clause 7.2 on V.21. The initiating station uses channel 1 (980/1180 Hz), and the responding station uses channel 2 (1650/1850 Hz). Each message is 100 ms of mark, three flags, the frame with its FCS and stuffed zeros (`HdlcTx`, `HdlcRx` of `fax_hdlc.h`), and two flags. Queued messages (CL then MS) follow each other with no gap. A frame of three octets or more with an incorrect FCS is an invalid frame |
| `DP_V8BIS` (108) | `dsp/dp_v8bis.cpp` | The transactions (refer to the subsequent section) |
| Call progress | `dsp/dp_callprog.cpp` | When V.8 bis is enabled, it listens for the initiating signals together with the answer tones while it waits for the answer (10.2.1). Then it gives the call to `DP_V8BIS` with the signal that it heard (`rt->v8bis_sig`) |
| `+A8E`, `Modem::v8bis_call()`, `MDMPRM_V8BIS` (76) | `core/at.cpp`, `core/modem.cpp`, `core/interface.cpp` | The `+A8E=<v8o>,<v8a>,<v8cf>,<v8b>` of V.251. With `<v8b>` 1, V.8 bis is enabled for a data call where the pump starts with V.8 (`+MS` V.34, V.90, V.91, V.92, V.cvm). It is not enabled on a leased line, a clear channel or a relayed call. Then the `change_dp()` of the answerer starts `DP_V8BIS` before V.8. Call progress asks for `MDMPRM_V8BIS` |

## The transactions

The answering modem is the initiating station (9.3). The station that
receives MS is the answering modem of the subsequent start-up (9.9).
D-Modem makes sure that this station is the modem that answered the call.

| | Answerer (initiating) | Caller (responding) |
|---|---|---|
| 1 | 400 ms of silence (10.2.2), then MRe at -22 dBm0 (7.1.4: 12 to 15 dB below the nominal permitted level of -9 to -10 dBm0) | Call progress hears MRe (or CRe, MRd, CRd) and gives the call to `DP_V8BIS` |
| 2 | | After 100 ms, CRd (the responding pair, at the level of the messages) |
| 3 | On CRd: CL (its I field: the V.8 start-up; S field: data, with V.42, V.42 bis, V.34, V.32 bis, V.32, V.22 bis, V.22 and V.21) | |
| 4 | | On CL: MS with the data mode, the best modulation that the two stations have, V.42 and V.42 bis where the two stations have them, the V.8 start-up if the CL has it, and "transmit ACK(1)" |
| 5 | On MS: ACK(1) if MS asks for it, then V.8 (`DP_V8`, ANSam after its 200 ms of silence) | On ACK(1): 100 ms of ANSam, then the caller of V.8 (`rt->answer_tone` set, as call progress sets it) |

This is transaction 10 of Table 7 (MRe, CRd, CL, MS, ACK). The answerer
also accepts transaction 1: ESr and MS immediately after its MRe. The answerer answers
with ACK(1) (if MS asks for it) and V.8.

The answerer refuses an MS with NAK(3) in these conditions:

- The MS does not select the data mode.
- The MS asks for the short V.8 start-up (9.9.2: our V.8 answerer waits for
  CM).
- The MS selects only modulations that the answerer did not offer.

Then the answerer continues to V.8 without V.8 bis. The caller answers ESi and a subsequent CL with its MS
(transaction 5).

### When it does not happen

| Where | What | Then |
|---|---|---|
| Answerer | No CRd, MRd or MS in 3 s after the end of MRe (10.2.2) | V.8 (ANSam), as a modem without V.8 bis |
| Answerer | MRd (transaction 7: the answerer would send MS and be the caller of V.8) | V.8 |
| Answerer | No MS in 5 s after the CL (9.8) | V.8 |
| Either | An invalid frame (7.2.9) | NAK(1) (9.8); then the answerer goes to V.8, and the caller waits for the answer tone |
| Either | A message at the incorrect time, a NAK | The answerer goes to V.8; the caller waits for the answer tone |
| Caller | A CL without the data mode, or with no common modulation | No MS; the answer tone |
| Caller | MS or CLR after ESi (the answerer would be the caller of V.8) | The answer tone |
| Caller | No CL in 5 s, no ACK(1) in 5 s after the MS | The answer tone |

"The answer tone" means that the caller waits for 300 ms of 2100 or 2225 Hz, as
call progress does, and then continues to the pump. (S7 is the limit, as for all
handshakes.) After its MS, the caller accepts 100 ms of 2100 Hz as the
ANSam of the answerer. Thus, if an ACK(1) is lost, the caller does not stop.

With `+DSVD=1`, the CL also offers the SVD mode. The MS of the caller
selects it when the two stations have what V.70 must have. The details are in
[v70-details.md](../v70-details.md). With `+ASVD=1`, the SVD mode has
V.61. An MS with V.61 starts the start-up of V.61 at the two ends, instead of
V.8 ([v61-details.md](v61-details.md#v8-bis)).

## Levels

| Signal | Level | Constant |
|---|---|---|
| MRe, CRe | -22 dBm0 (the two tones of segment 1 have half of the power each) | `AMP_E` in `dsp/v8bis.h` |
| MRd, CRd, ESi, ESr, the messages | Approximately -14.5 dBm0 (the level of V.8) | `AMP_MSG` in `dsp/v8bis.h` |

Clause 7.1.4 sets the level of MRe and CRe 12 to 15 dB below the
nominal permitted level, not below the level of the modem. The nominal
permitted level is -9 to -10 dBm0 (V.2). Thus, the correct range is
-21 to -25 dBm0. Before, MRe was 12 dB below the level of the messages,
at -26.5 dBm0. That is below the range, and a line with loss can make
it too weak for a caller. The K56flex server (`dsp/v34_k56.h`) sends
its MRe at the level of the SM56 tests (approximately -17.5 dBm0), and
this change does not touch it.

## Against other modems

| Modem | Answer to our MRe |
|---|---|
| D-Modem | CRd, then transaction 10 |
| SM56 (Motorola), as a K56flex client | CRd, then transaction 10 ([k56flex-details.md](k56flex-details.md)) |
| Conexant HSF (hsfmodem 7.80), V.90 or V.92 | No answer |

**Conexant modems.** The HSF modem of Conexant does not answer MRe,
MRd, CRe or CRd. It is silent until our ANSam. Then it connects with
V.90. Its `+A8E?` gives `1,1,C1,1`: V.8 bis is enabled. But it has no
K56flex (`+MS=?` has no `K56`) and no V.70. Its configuration has V.8
bis together with K56flex and V.70. Thus, it possibly answers V.8 bis
only when it can offer one of these modes. The tests
`hsf_v8bis_mre_unanswered` and `hsf_k56flex_mre_unanswered` (label
`hsf`) show this behaviour. Other Conexant modems of the V.92 period (HSF,
HDA and HCF) possibly operate in the same way. With these modems,
V.8 bis and K56flex do not occur. The call continues with V.8, 3.5 s
later than without V.8 bis.

## `+A8E`

`+A8E=<v8o>,<v8a>,<v8cf>,<v8b>` (V.251 5.1). The modem keeps it with the other
extended parameters:

| Subparameter | Values | Meaning here |
|---|---|---|
| `<v8o>` | 1 | V.8 on calls, done by the modem (the only method that D-Modem has) |
| `<v8a>` | 1 | V.8 on answers, done by the modem |
| `<v8cf>` | 0 | No call function of the DTE (the modem does not send the CI of V.8) |
| `<v8b>` | 0, 1 | V.8 bis disabled, or done by the modem at the start of a data call |

In V.251, the default of `<v8b>` is 1, "based on manufacturer-determined
procedures". The default of D-Modem is 0, because an answer with V.8 bis makes a
caller without V.8 bis wait about 3.5 s more. These values give `ERROR`:

- `<v8b>` 2 (the DTE does V.8 bis with `+A8M` and the `+A8x` indications).
- The `<cfrange>` and `<protrange>` subparameters.

## Tests

| Test | What it covers |
|---|---|
| `V8bis.*` (`dsp/test/v8bis_test.cpp`, label `quick`) | The level of MRe (-21 to -25 dBm0); an MS compared with its octets written manually; ACK and NAK; a CL with an H.324 mode (SPar(2) and NPar(3)), parsed past and cut short at all positions; each signal through the detector from its station at the two levels and at the level of MRe less 12 dB, and not through the pair of the other station; ESi with a subsequent CL; CL then MS on each channel; a changed bit that the FCS finds |
| `Core.A8e`, `Core.V8bis*` (`core/test/core_test.cpp`, label `quick`) | The values and ranges of `+A8E`. Two modems back to back with V.8 bis at the two ends (MRe, CRd, CL, MS, ACK(1), then V.8 and the fastest modulation), at the answerer only (no answer to MRe, then V.8), and at the caller only (no V.8 bis) |

## Not done

- The code encodes and reads the other modes (H.324, V.18, T.30, telephony),
  but it selects only data and SVD (V.70, with `+DSVD`: refer to
  [v70-details.md](../v70-details.md); V.61, with `+ASVD`: refer to
  [v61-details.md](v61-details.md)).
- Transactions where the caller receives MS (2, 3, 7 to 9, 11 to 13
  from the side of the answerer, 4 and 6 after ESi). For these, the caller of D-Modem
  would have to be the answering modem of V.8.
- The short V.8 start-up (9.9.2), and the V.25 start-up after V.8 bis
  (9.9.3, ANS instead of ANSam).
- Message segmentation (9.10: "additional information available",
  ACK(2)), and the network type in the I field (D-Modem always gives the
  analogue PSTN).
- V.8 bis during a telephone call (MRd, CRd, ESi from the two ends), the
  outgoing message after MRe, and MRe again on a calling tone (10.2.2).
- The DTE control of V.251 (`<v8b>` 2, `+A8M`, `+A8x`).
