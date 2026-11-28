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
| [V.22](v22.md) ([details](v22-details.md)) | 1200 bit/s DPSK, duplex |
| [V.22 bis](v22bis.md) ([details](v22bis-details.md)) | 2400 bit/s QAM, duplex |
| [V.23](v23.md) ([details](v23-details.md)) | 1200/75 bit/s FSK |
| [V.26 bis](v26bis.md) ([details](v26bis-details.md)) | 2400/1200 bit/s, with a 75 bit/s backward channel |
| [V.26 ter](v26ter.md) ([details](v26ter-details.md)) | 2400 bit/s duplex, with echo cancellation |
| [V.27 ter](v27ter.md) ([details](v27ter-details.md)) | 4800/2400 bit/s |
| [V.29](v29.md) ([details](v29-details.md)) | 9600 bit/s |
| [V.32](v32.md) ([details](v32-details.md)) | 9600 bit/s duplex, with trellis coding |
| [V.32 bis](v32bis.md) ([details](v32bis-details.md)) | 14400 bit/s duplex |
| [V.33](v33.md) ([details](v33-details.md)) | 14400 bit/s on 4-wire leased circuits |
| [V.17](v17.md) ([details](v17-details.md)) | 14400 bit/s for fax |
| [V.34](v34.md) ([details](v34-details.md)) | 33600 bit/s duplex |
| [V.90](v90.md) ([details](v90-details.md)) | 56000 bit/s downstream, with a digital modem and an analogue modem |
| [V.91](v91.md) ([details](v91-details.md)) | PCM between two digital modems |
| [V.92](v92.md) ([details](v92-details.md)) | Quick connect, modem-on-hold and PCM upstream |
| [V.18](v18.md) ([details](v18-details.md)) | Text telephones |
| [V.61](v61.md) ([details](v61-details.md)) | Analogue simultaneous voice and data |

## Error correction and data compression

| Recommendation | Subject |
|---|---|
| [V.42](v42.md) ([details](v42-details.md)) | Error correction (LAPM) |
| [V.42 bis](v42bis.md) ([details](v42bis-details.md)) | Data compression |
| [V.44](v44.md) ([details](v44-details.md)) | Data compression (LZJH) |
| [X.75](x75.md) ([details](x75-details.md)) | LAPB on an ISDN data call |

## Voice and data together

| Recommendation | Subject |
|---|---|
| [V.70](v70.md) ([details](v70-details.md)) | Digital simultaneous voice and data (DSVD) |
| [V.75](v75.md) ([details](v75-details.md)) | The control of DSVD |
| [V.76](v76.md) ([details](v76-details.md)) | A multiplexer on LAPM |
| [H.245](h245.md) ([details](h245-details.md)) | Control messages for multimedia |
| [G.729](g729.md) ([details](g729-details.md)) | Speech coding at 8 kbit/s, with Annexes A and B |
| [G.711](g711.md) ([details](g711-details.md)) | PCM of voice frequencies (µ-law and A-law) |

## ISDN and IP networks

| Recommendation | Subject |
|---|---|
| [V.110](v110.md) ([details](v110-details.md)) | Rate adaption on an ISDN B-channel |
| [V.120](v120.md) ([details](v120-details.md)) | HDLC rate adaption on an ISDN B-channel |
| [V.150.1](v150-1.md) ([details](v150-1-details.md)) | Modem relay over IP |
| [V.152](v152.md) ([details](v152-details.md)) | Voice band data over IP |

## The interface to the computer

| Recommendation | Subject |
|---|---|
| [V.250](v250.md) ([details](v250-details.md)) | AT commands |
| [V.251](v251.md) ([details](v251-details.md)) | AT commands for V.8 and V.8 bis |
| [V.253](v253.md) ([details](v253-details.md)) | AT commands for voice |
| [V.80](v80.md) ([details](v80-details.md)) | Synchronous access mode |

## Tests and management

| Recommendation | Subject |
|---|---|
| [V.54](v54.md) ([details](v54-details.md)) | Loop test devices |
| [V.52 and O.153](v52.md) ([details](v52-details.md)) | The 511-bit test pattern |
| [V.58](v58.md) ([details](v58-details.md)) | The management information model of a DCE |
| [V.59](v59.md) ([details](v59-details.md)) | Managed objects for diagnostic information |

## Fax

| Recommendation | Subject |
|---|---|
| [T.30](t30.md) ([details](t30-details.md)) | The procedures of a Group 3 fax call |
| [T.4](t4.md) ([details](t4-details.md)) | Group 3 fax pages: MH and MR coding |
| [T.6](t6.md) ([details](t6-details.md)) | MMR coding |
| [T.31](t31.md) ([details](t31-details.md)) | Class 1 fax AT commands |
| [T.32](t32.md) ([details](t32-details.md)) | Class 2 and Class 2.0 fax AT commands |
| [T.38](t38.md) ([details](t38-details.md)) | Fax over IP |
| [T.82](t82.md) ([details](t82-details.md)) | JBIG coding of bi-level images |
| [T.85](t85.md) ([details](t85-details.md)) | JBIG for fax pages |
| [T.42](t42.md) ([details](t42-details.md)) | Colour fax with JPEG |
| [T.43](t43.md) ([details](t43-details.md)) | Lossless colour and grey-scale fax |
| [T.44](t44.md) ([details](t44-details.md)) | Mixed raster content |
| [T.45](t45.md) ([details](t45-details.md)) | Run-length colour encoding |
| [T.434](t434.md) ([details](t434-details.md)) | Binary file transfer |

## Related standards

The pages above refer to these standards. D-Modem does not implement
them as a whole, or they are only a part of a different standard. They
have no pages yet.

| Recommendation | Subject | Referred to by |
|---|---|---|
| V.2 | The power levels for data transmission on telephone lines | V.21, V.25, V.26 bis, V.26 ter, V.27 ter, V.29, V.32, V.33, V.61 |
| V.14 | Asynchronous characters on a synchronous channel | V.22, V.22 bis, V.26 ter, V.32, V.42, V.59, V.61, V.150.1, H.245 |
| V.24 | The circuits between the DTE and the DCE | V.13, V.21, V.23, V.26 bis, V.58, V.61, V.110, V.120 |
| V.25 bis | Dialling with a synchronous or asynchronous DTE | V.25 |
| V.26 | 2400 bit/s on 4-wire leased circuits | V.26 bis, V.27 ter, V.29 |
| V.27 | 4800 bit/s on leased circuits | V.27 ter, V.29 |
| V.28 | The electrical characteristics of the V.24 circuits | V.21, V.23, V.26 bis, V.27 ter, V.29, V.61 |
| G.164, G.165, G.168 | Echo suppressors and echo cancellers | V.25, V.26 ter, V.32, V.61, V.152 |
| G.191 | Software tools for speech and audio coding | G.711 |
| G.721, G.726 | ADPCM speech coding | G.711, V.253 |
| H.223, H.324 | Multimedia terminals on the telephone line | V.8, V.8 bis, V.80, V.251, H.245 |
| H.248, H.323 | Media gateway control and multimedia over IP | V.152, H.245 |
| I.460 | Rate adaption to 64 kbit/s | V.110 |
| M.1020, M.1025 | The quality of leased circuits | V.26 ter, V.29 |
| M.3100, X.722 | The generic network information model, and GDMO | V.58 |
| Q.921, Q.922 | LAPD and the frame relay data link | V.120 |
| Q.931 | ISDN call control | V.120 |
| T.35 | Country codes and manufacturer codes | V.8 |
| T.66, T.101 | Codes for the V.8 call functions: Videotex and others | V.8, V.8 bis |
| T.90 | ISDN terminals for telematic services | X.75 |
| X.30 | ISDN support of X.21 terminals | V.110 |
| X.25 | Packet mode DTEs | X.75 |
| X.691 | The packed encoding rules (PER) of ASN.1 | H.245, V.75 |
