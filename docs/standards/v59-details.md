# V.59: technical details

This page gives the structure of the objects of V.59 (11/2000), and the
objects that D-Modem gives. For a summary, refer to [V.59](v59.md).

## Compliance (5.1)

An implementation of V.59 must give all the mandatory objects. An
object that does not apply to the last connection has the value NULL.

## The high-level objects (6.3)

| Tag | Object | Tag | Object |
|---|---|---|---|
| 00xx | ModemIdentity | 10xx | V21Diag |
| 01xx | ModemCapability | 15xx | V34HdxDiag |
| 02xx | ModeSelected | 16xx | V17Diag |
| 07xx | V8Diag | 17xx | V29faxDiag |
| 08xx | V8bisDiag | 18xx | V27tfaxDiag |
| 09xx | V90Diag | 23xx | V18Diag |
| 0Axx | V91Diag | 24xx, 25xx | V70Diag, V61Diag |
| 0Bxx | V92ModDiag | 26xx | NSMDiag |
| 0Cxx | V34Diag | 2Cxx | CnxDiag |
| 0Dxx | V32Diag | 2Dxx | LineConDiag |
| 0Exx | V22Diag | 2Exx | CallProgress |
| 0Fxx | V23Diag | 2Fxx, 30xx, 31xx | ErrorControl, Compression, DTEDCE |

## The data types (6.2)

| Type | Value |
|---|---|
| simpleText | Text, for example `-15.0dBm0` or `60s` |
| dataSequence | Octets in hexadecimal, with `;` between them |
| BIT STRING | Bit flags in one codeword, in hexadecimal |
| BOOLEAN | `TRUE` or `FALSE` |
| NULL | The object does not apply |

## The transmit power (6.4, tags 45 to 47)

| Tag | Object | Value | Mandatory |
|---|---|---|---|
| 45 | initialTxPower | The power at the start, as the modem set it | Yes |
| 46 | txPowerDrop | The power reduction, if the modulation has one | No |
| 47 | txPower | An estimate of the transmit power | Yes |

## The V.92 objects (6.8.5)

| Tag | Object | Value |
|---|---|---|
| 0B10 | sP1Enable | TRUE if the short phase 1 (quick connect) is enabled |
| 0B11 | lastCallSP1 | TRUE if the last call used the short phase 1 |
| 0B20 | cWDEnable | TRUE if call waiting detection is enabled |
| 0B21 | mOHEnable | TRUE if modem-on-hold is enabled |
| 0B22 | lastMOHaction | The result of the last modem-on-hold transaction (Table 12) |
| 0B23 | mOHTimeout | The time on hold that the digital modem gave |
| 0B24 | lastMOHduration | The time of the last hold |

## v42featureNegotiation (6.12, tag 2F02, Table 19)

| Bit | Mnemonic | It is set when |
|---|---|---|
| 0, 1 | SingleSREJ, MultipleSREJ | SREJ is enabled |
| 2 | HDLCBalMode | The normal procedure is enabled |
| 3 | TestFrameSup | The DCE supports the TEST frame |
| 4, 5 | fcs16, fcs32 | The FCS has 16 bits or 32 bits |
| 6 | v42phaseDetused | The detection phase was used |
| 7 | odpAdpbypassed | An other procedure (for example V.92) bypassed ODP and ADP |
| 8, 9, 10 | v42fallbackDisc, v42fallbackBuf, v42fallbackUnBuf | If V.42 fails: disconnect, V.14 buffered, or V.14 unbuffered |

## V.59 in D-Modem

| Item | D-Modem (`core/v59.cpp`) |
|---|---|
| initialTxPower (xx45) | The mean transmit power in the first second of data, in dBm0. NULL before the first call. |
| txPower (xx47) | The mean transmit power over the call, in dBm0 |
| sP1Enable, lastCallSP1 | From the V.92 pump. NULL if the last call was not V.92. |
| cWDEnable | FALSE if `+PCW=2` (ignore call waiting), else TRUE |
| lastMOHaction | 0 granted (MHack), 2 denied (MHnack), 3 clear down (MHcda). NULL if there was no hold. |
| mOHTimeout | The T1 of the MHack (V.92 Table 33), for example `60s`, or `unlimited` |
| lastMOHduration | The time from the grant of the hold (MHack) to its end (the call back, or the CM of the remote), for example `5.0s` |
| v42featureNegotiation | Bits 2 and 3 always. Bit 4 or 5 from the FCS. Bit 6 if the detection phase is on (`+ES`), else bit 7 if V.92 quick connect was used. Bit 8, 9 or 10 from the `+ES` fallback of the side of the call. NULL without V.42. |
| Not given | The exchanged sequences that the core does not record: the CI of V.8, the INFO and MP of V.34, the CP of V.90, the QC of V.92, the sequences of V.8 bis, and the rate sequences of V.32 and V.22. v18used (2301). |

## Corrections (2026-11)

| Item | Before | Now |
|---|---|---|
| initialTxPower (6.4, mandatory) | Not given | Given for each modulation, from the first second of data |
| sP1Enable, lastCallSP1, cWDEnable, lastMOHaction, mOHTimeout, lastMOHduration (6.8.5, mandatory) | Not given | Given from the V.92 pump and `+PCW` |
| v42featureNegotiation (6.12, mandatory) | Not given | Given as the bits of Table 19 |
| `+TMO 3=?` (the low-level objects) | Only txPower | initialTxPower and txPower |

The transcript `v59` (`core/test/scripts/v59.expect`) checks
initialTxPower and v42featureNegotiation. The interop test
`open_v92a_moh_tmo` checks the V.92 objects after a modem-on-hold.
