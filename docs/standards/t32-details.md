# T.32: technical details

This page gives the commands, the reports, the data streams and the
parameters of T.32 (08/95). For a summary, refer to [T.32](t32.md).

## The action commands (Table 5)

| Command | Function |
|---|---|
| `D` | Dial, then phase B. The modem gives the DIS reports and OK. |
| `A` | Answer, then phase B. The modem gives the DCS reports and OK. |
| `+FDT` | Send a page: CONNECT, then the page data from the DTE |
| `+FDR` | Receive a page: CONNECT, then the page data to the DTE |
| `+FKS` | End the session in order (DCN) |
| `+FIP[=0]` | Set all the Class 2 parameters to their start values. `+FCLASS` does not change. |

## The reports (Table 6)

| Report | Meaning | Enabled by |
|---|---|---|
| `+FCO` | Connection to a fax machine | Always |
| `+FHS:<hsc>` | The call ended, with the status `hsc` (Table 20) | Always |
| `+FIS:`, `+FTC:` | The DIS or DTC capabilities of the remote | `+FNR` rpr |
| `+FCS:` | The DCS: the session parameters | `+FNR` tpr |
| `+FTI:`, `+FCI:`, `+FPI:` | The TSI, CSI or CIG identity of the remote | `+FNR` idr |
| `+FNF:`, `+FNS:`, `+FNC:` | The NSF, NSS or NSC frames | `+FNR` nsr |
| `+FPO` | The remote has a document to poll | Always |
| `+FPS:` | The status of a received page | Always |
| `+FET:` | The post-page message of a received page | Always |

## The data streams (Table 9)

| Pair | Direction | Meaning |
|---|---|---|
| `<DLE><DLE>` | Both | One `<DLE>` |
| `<DLE><SUB>` | Both | Two `<DLE>` |
| `<DLE><,>` | DTE to modem | End of page. More pages follow (MPS). |
| `<DLE><;>` | DTE to modem | End of document (EOM) |
| `<DLE><.>` | DTE to modem | End of the last page (EOP) |
| `<DLE><ETX>` | Modem to DTE | End of a received page |
| `<DC2>` | DTE to modem | The DTE is ready for the page data |

After `<DLE><ppm>`, the modem sends the post-page message. It gives OK
or ERROR when the remote answers. After EOP, it sends DCN, then gives
`+FHS:00` and OK (Appendix II.1).

## The parameters (Tables 7 and 8, Appendix I)

| Parameter | Start value | Range | Function |
|---|---|---|---|
| `+FCC`, `+FIS` | Manufacturer | Table 21 | The capabilities, and the values for the next session |
| `+FCS` | — | Read only | The current session |
| `+FLI`, `+FPI` | Empty | 20 characters | The local identity (TSI or CSI, CIG) |
| `+FLP`, `+FSP` | 0 | 0, 1 | A document to poll, polling enabled |
| `+FCR` | Manufacturer | 0, 1 | Can receive |
| `+FNR` | 0,0,0,0 | 0, 1 each | Report enables: rpr, tpr, idr, nsr |
| `+FIE` | 0 | 0, 1 | Procedure interrupt enable |
| `+FPS` | 1 | 1–5 | The page status for the next page received |
| `+FCQ` | 1,0 | 0–2, 0–2 | Copy quality checking |
| `+FCT` | 1Eh | 0–FFh | Phase C timeout (s) |
| `+FRY` | 0 | 0–FFh | CTC retries |
| `+FMS` | 0 | 0–5 | Minimum phase C rate |
| `+FIT` | 0,0 | 0–255, 0–1 | DTE inactivity timer |
| `+FPP` | 0 | 0, 1 | Packet protocol |
| `+FBO` | 0 | 0–3 | Bit order of the page data |
| `+FEA` | 0 | 0, 1 | EOL alignment of received data |
| `+FAP`, `+FBU`, `+FND`, `+FFC`, `+FRQ` | 0 | See Appendix I | Other optional services |

## T.32 in D-Modem

| Item | D-Modem (`dsp/fax_class2.cpp`, `core/at.cpp`) |
|---|---|
| Classes | `+FCLASS=2.0`, and `+FCLASS=2` with the Class 2 names (`+FCON`, `+FDIS`, `+FPTS`, `+FHNG` and others) |
| Page data | MH with EOLs, in the bit order of `+FBO`. The engine codes the pages again for the line (MR, ECM). |
| End of page | `<DLE><ppm>` in Class 2.0. Also `<DLE><ETX>` and then `+FET=<n>`, as in Class 2. |
| Reports | Class 2.0: as `+FNR` enables. Class 2: all. |
| `+FIP` | All the Class 2 parameters to their start values |
| Parameters | All the parameters of Appendix I. Values out of range give ERROR. Class 2.0 gives and takes them in hexadecimal. |
| Effects | `+FCC`, `+FIS`, `+FLI`, `+FPI`, `+FLP`, `+FSP`, `+FBO`, `+FNR` and `+FIT` have an effect. The others are kept. |
| Polling | `+FLP` and `+FSP` |
| V.34 and T.38 | V.34 half-duplex for a `+FIS` rate above 5 with ECM. T.38 at all points of the call. |
| Not implemented | Subaddress, selective polling and password (`+FAP` 0 only), non-standard frames (`+FNS`), procedure interrupts, the packet protocol, format conversion (`+FFC` 0 only) |

The tests `Classes/CoreFax2.*`, `Core.Fax2Abort`, `Core.Fax2Poll` and
`Core.Fax2PollV34` (`core/test/core_test.cpp`) send fax calls between
two D-Modem modems in Class 2 and Class 2.0.

## Corrections (2026-11)

| Error | Correction |
|---|---|
| Class 2.0 did not accept `<DLE><ppm>` at the end of the page data. The page did not end. | `<DLE><,>`, `<DLE><;>` and `<DLE><.>` end the page with MPS, EOM or EOP (8.3.3.7). |
| Class 2.0 gave all the reports. `+FNR` had no effect. | `+FNR` enables the reports. All are off at the start (8.5.1.11). |
| `+FIP` gave OK, but it changed no parameter | `+FIP` sets the Class 2 parameters to their start values (8.3.6) |
| `+FCQ?`, `+FCT?` and other parameters gave 0, not their start values. `=?` gave no range. | They give the values of Appendix I and their ranges. Values out of range give ERROR. |

The tests `Core.Fax2PostPageDle`, `Core.Fax2ReportsAsFnr` and
`Core.Fax2Parameters` (`core/test/core_test.cpp`) check these
corrections.
