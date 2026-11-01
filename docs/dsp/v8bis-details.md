# V.8bis: technical details

The plain-English overview is [v8bis.md](v8bis.md). The references are
to ITU-T V.8 bis (11/2000) and V.251 (02/98).

## The parts

| Part | Where | What it does |
|---|---|---|
| `Tree`, `Msg`, `Caps` | `dsp/v8bis.h`, `v8bis.cpp` | Clause 8's information field: the message type and revision, the identification (I) and standard information (S) fields as parameter trees (8.2: NPar(1), SPar(1), one Par(2) block per SPar(1) bit set, NPar(3) blocks per SPar(2) bit set, with the delimiting bits 8 and 7). `Caps` is what a station has, or an MS selects: the I field's NPar(1), the first SPar(1) octet of the S field, and the data and SVD modes' NPar(2) (six bits an octet). The non-standard field is not read |
| `SigTx`, `SigRx` | same | Clause 7.1's signals. Segment 1 is a dual tone, 1375 + 2002 Hz from the initiating station or 1529 + 2225 Hz from the responding one, for 400 ms; segment 2 a single tone for 100 ms (MRe 650, MRd 1150, CRe 400, CRd 1900, ESi 980, ESr 1650 Hz). ESi and ESr have no segment 2 of their own: it is the 100 ms of V.21 mark that begins the message after them (7.2.4). The detector works on 10 ms blocks: 180 ms of a pair asked for, then within 80 ms segment 2's tone for 40 ms |
| `MsgTx`, `MsgRx` | same | Clause 7.2's messages on V.21, the initiating station on channel 1 (980/1180 Hz), the responding one on channel 2 (1650/1850 Hz): 100 ms of mark, three flags, the frame with its FCS and stuffed zeros (`HdlcTx`, `HdlcRx` of `fax_hdlc.h`), two flags. Messages queued back to back (CL then MS) follow each other with no gap. A frame of three octets or more whose FCS is wrong is an invalid frame |
| `DP_V8BIS` (108) | `dsp/dp_v8bis.cpp` | The transactions, below |
| Call progress | `dsp/dp_callprog.cpp` | With V.8 bis on, it listens for the initiating signals beside the answer tones while it waits for the answer (10.2.1), and hands the call to `DP_V8BIS` with the signal it heard (`rt->v8bis_sig`) |
| `+A8E`, `Modem::v8bis_call()`, `MDMPRM_V8BIS` (76) | `core/at.cpp`, `core/modem.cpp`, `core/interface.cpp` | V.251's `+A8E=<v8o>,<v8a>,<v8cf>,<v8b>`: `<v8b>` 1 turns V.8 bis on for a data call whose pump starts with V.8 (`+MS` V.34, V.90, V.91, V.92, V.cvm), not on a leased line, a clear channel or a relayed call. The answerer's `change_dp()` then starts `DP_V8BIS` before V.8; call progress asks for `MDMPRM_V8BIS` |

## The transactions

The answering modem is the initiating station (9.3); the station that
receives MS is the answering modem of the start-up that follows (9.9),
and D-Modem keeps that the modem that answered the call.

| | Answerer (initiating) | Caller (responding) |
|---|---|---|
| 1 | 400 ms of silence (10.2.2), then MRe at 12 dB under the messages' level (7.1.4) | Call progress hears MRe (or CRe, MRd, CRd) and hands over |
| 2 | | 100 ms on, CRd (the responding pair, the messages' level) |
| 3 | On CRd: CL (its I field: the V.8 start-up; S field: data, with V.42, V.42 bis, V.34, V.32 bis, V.32, V.22 bis, V.22 and V.21) | |
| 4 | | On CL: MS with the data mode, the best modulation both have, V.42 and V.42 bis where both have them, the V.8 start-up if the CL has it, and "transmit ACK(1)" |
| 5 | On MS: ACK(1) when MS asks for it, then V.8 (`DP_V8`, ANSam after its 200 ms of silence) | On ACK(1): 100 ms of ANSam, then V.8's caller (`rt->answer_tone` set, as call progress would) |

This is transaction 10 of Table 7 (MRe, CRd, CL, MS, ACK). The answerer
takes transaction 1 too: ESr and MS straight after its MRe, answered
with ACK(1) (when MS asks for it) and V.8.

The answerer turns down with NAK(3) an MS that does not select the data
mode, asks for the short V.8 start-up (9.9.2: our V.8 answerer waits for
CM), or selects only modulations it has not offered; it then goes on to
V.8 without V.8 bis. The caller answers ESi followed by CL with its MS
(transaction 5).

### When it does not happen

| Where | What | Then |
|---|---|---|
| Answerer | No CRd, MRd or MS within 3 s of MRe's end (10.2.2) | V.8 (ANSam), as a modem without V.8 bis |
| Answerer | MRd (transaction 7: the answerer would send MS and be V.8's caller) | V.8 |
| Answerer | No MS within 5 s of the CL (9.8) | V.8 |
| Either | An invalid frame (7.2.9) | NAK(1) (9.8); the answerer then V.8, the caller the answer tone |
| Either | A message out of turn, a NAK | The answerer V.8; the caller the answer tone |
| Caller | A CL without the data mode or with no modulation in common | No MS; the answer tone |
| Caller | MS or CLR after ESi (the answerer would be V.8's caller) | The answer tone |
| Caller | No CL within 5 s, no ACK(1) within 5 s of the MS | The answer tone |

"The answer tone" is the caller waiting for 300 ms of 2100 or 2225 Hz as
call progress does, then on to the pump (S7 is the limit, as for any
handshake). After its MS the caller takes 100 ms of 2100 Hz as the
answerer's ANSam, so an ACK(1) lost on the way does not hold it up.

With `+DSVD=1` the CL offers the SVD mode too, and the caller's MS
chooses it when both have what V.70 needs; the details are in
[v70-details.md](../v70-details.md). With `+ASVD=1` the SVD mode has
V.61, and an MS with it leads both ends to V.61's start-up in place of
V.8 ([v61-details.md](v61-details.md#v8-bis)).

## `+A8E`

`+A8E=<v8o>,<v8a>,<v8cf>,<v8b>` (V.251 5.1), kept with the other
extended parameters:

| Subparameter | Values | Meaning here |
|---|---|---|
| `<v8o>` | 1 | V.8 on calls, run by the modem (the only way D-Modem has) |
| `<v8a>` | 1 | V.8 on answers, run by the modem |
| `<v8cf>` | 0 | No call function of the DTE's (V.8's CI is not sent) |
| `<v8b>` | 0, 1 | V.8 bis off, or run by the modem at the start of a data call |

V.251 has `<v8b>` 1 by default, "based on manufacturer-determined
procedures". D-Modem's default is 0: answering with V.8 bis costs a
caller without it about 3.5 s. `<v8b>` 2 (the DTE runs V.8 bis with
`+A8M` and the `+A8x` indications) and the `<cfrange>` and `<protrange>`
subparameters are `ERROR`.

## Tests

| Test | What it covers |
|---|---|
| `V8bis.*` (`dsp/test/v8bis_test.cpp`, label `quick`) | An MS against its octets by hand, ACK and NAK, a CL with an H.324 mode (SPar(2) and NPar(3)) parsed past and cut short anywhere, each signal through the detector from its station at both levels and not through the other pair's, ESi with a CL after it, CL then MS on each channel, a flipped bit caught by the FCS |
| `Core.A8e`, `Core.V8bis*` (`core/test/core_test.cpp`, label `quick`) | `+A8E`'s values and ranges; two modems back to back with V.8 bis at both (MRe, CRd, CL, MS, ACK(1), then V.8 and the fastest modulation), at the answerer alone (no answer to MRe, then V.8), at the caller alone (no V.8 bis) |

## Not done

- The other modes (H.324, V.18, T.30, telephony) are coded and read,
  but only data and SVD (V.70, with `+DSVD`; see
  [v70-details.md](../v70-details.md); V.61, with `+ASVD`; see
  [v61-details.md](v61-details.md)) are chosen.
- Transactions where the caller receives MS (2, 3, 7 to 9, 11 to 13
  from the answerer's side, 4 and 6 after ESi): D-Modem's caller would
  have to be V.8's answering modem.
- The short V.8 start-up (9.9.2) and the V.25 start-up after V.8 bis
  (9.9.3, ANS in place of ANSam).
- Message segmentation (9.10: "additional information available",
  ACK(2)), and the network type in the I field (D-Modem says the
  analogue PSTN).
- V.8 bis during a telephone call (MRd, CRd, ESi from either end), the
  outgoing message after MRe, and MRe again on a calling tone (10.2.2).
- V.251's DTE control (`<v8b>` 2, `+A8M`, `+A8x`).
