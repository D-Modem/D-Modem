# The ITU-T standards in Simplified Technical English

This folder has a description of each ITU-T (formerly CCITT)
Recommendation that D-Modem implements. Each page tells what the standard
does and how it operates, in ASD-STE100 Simplified Technical English.

The pages are our own descriptions. They are not copies or translations of
the Recommendations, because ITU-T does not let persons share copies. For
the normative text, download the Recommendations with
`docs/specs/fetch.sh`. For the location of each standard in the code,
refer to [ITU-T Recommendations](../specs/README.md).

Each standard has two pages. The first page is a summary: what the standard
does, the main steps and where D-Modem uses it. The second page,
"technical details", gives the signals, the timing, the codes and the
implementation in D-Modem.

A standard that has no link has no pages yet.

## Start-up and call control

| Recommendation | Subject |
|---|---|
| [V.8](v8.md) ([details](v8-details.md)) | The start of a data call: ANSam, CM, JM and CJ |
| [V.8 bis](v8bis.md) ([details](v8bis-details.md)) | Identification and selection of a mode before V.8 |
| [V.25](v25.md) ([details](v25-details.md)) | Answer tones, call tones and echo canceller disabling |
| [V.13](v13.md) ([details](v13-details.md)) | Simulated carrier control |

## Modulations for the telephone line

| Recommendation | Subject |
|---|---|
| [V.21](v21.md) ([details](v21-details.md)) | 300 bit/s FSK, duplex |
| V.22 | 1200 bit/s DPSK, duplex |
| V.22bis | 2400 bit/s QAM, duplex |
| V.23 | 1200/75 bit/s FSK |
| V.26 bis | 2400/1200 bit/s, with a 75 bit/s backward channel |
| V.26 ter | 2400 bit/s duplex, with echo cancellation |
| V.27ter | 4800/2400 bit/s |
| V.29 | 9600 bit/s |
| V.32 | 9600 bit/s duplex, with trellis coding |
| V.32bis | 14400 bit/s duplex |
| V.33 | 14400 bit/s on 4-wire leased circuits |
| V.17 | 14400 bit/s for fax |
| V.34 | 33600 bit/s duplex |
| V.90 | 56000 bit/s downstream, with a digital modem and an analogue modem |
| V.91 | PCM between two digital modems |
| V.92 | Quick connect, modem-on-hold and PCM upstream |
| V.18 | Text telephones |
| V.61 | Analogue simultaneous voice and data |

## Error correction and data compression

| Recommendation | Subject |
|---|---|
| V.42 | Error correction (LAPM) |
| V.42bis | Data compression |
| V.44 | Data compression (LZJH) |
| X.75 | LAPB for packet-switched signalling |

## Voice and data together

| Recommendation | Subject |
|---|---|
| V.70 | Digital simultaneous voice and data (DSVD) |
| V.75 | The control of DSVD |
| V.76 | A multiplexer on LAPM |
| H.245 | Control messages for multimedia |
| G.729 | Speech coding at 8 kbit/s, with Annexes A and B |
| G.711 | PCM of voice frequencies (µ-law and A-law) |

## ISDN and IP networks

| Recommendation | Subject |
|---|---|
| V.110 | Rate adaption on an ISDN B-channel |
| V.120 | HDLC rate adaption on an ISDN B-channel |
| V.150.1 | Modem relay over IP |
| V.152 | Voice band data over IP |

## The interface to the computer

| Recommendation | Subject |
|---|---|
| V.250 | AT commands |
| V.251 | AT commands for V.8 and V.8 bis |
| V.253 | AT commands for voice |
| V.80 | Synchronous access mode |

## Tests and management

| Recommendation | Subject |
|---|---|
| V.54 | Loop test devices |
| V.52 and O.153 | The 511-bit test pattern |
| V.58 | The management information model of a DCE |
| V.59 | Managed objects for diagnostic information |

## Fax

| Recommendation | Subject |
|---|---|
| T.30 | The procedures of a Group 3 fax call |
| T.4 | Group 3 fax pages: MH and MR coding |
| T.6 | MMR coding |
| T.31 | Class 1 fax AT commands |
| T.32 | Class 2 and Class 2.0 fax AT commands |
| T.38 | Fax over IP |
| T.82 | JBIG coding of bi-level images |
| T.85 | JBIG for fax pages |
| T.42 | Colour fax with JPEG |
| T.43 | Lossless colour and grey-scale fax |
| T.44 | Mixed raster content |
| T.434 | Binary file transfer |
