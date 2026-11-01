# ITU-T Recommendations

These are the standards (ITU-T "Recommendations") that D-Modem
implements, and where each one lives in the code.

ITU-T publishes them free of charge at itu.int, but does not allow
copies to be shared, so they are not in the repository. Run
`docs/specs/fetch.sh` to download the current English edition of each
into this folder (`fetch.sh V.34 T.30` fetches just those two). The
"Local copy" links below then work.

| Recommendation | Subject | In D-Modem | Local copy |
|---|---|---|---|
| [G.711](https://www.itu.int/rec/T-REC-G.711) | PCM of voice frequencies (µ-law, A-law) | the network codec of V.90/V.91/V.92, netsim, the PBX modules | [G.711.pdf](G.711.pdf) |
| [T.4](https://www.itu.int/rec/T-REC-T.4) | Group 3 facsimile: MH, MR coding | `dsp/t4.cpp` | [T.4.pdf](T.4.pdf) |
| [T.6](https://www.itu.int/rec/T-REC-T.6) | MMR coding | `dsp/t4.cpp` | [T.6.pdf](T.6.pdf) |
| [T.30](https://www.itu.int/rec/T-REC-T.30) | Group 3 facsimile procedures, ECM | `dsp/t30.cpp` | [T.30.pdf](T.30.pdf) |
| [T.31](https://www.itu.int/rec/T-REC-T.31) | Class 1 fax AT commands | `dsp/fax_class1.cpp` | [T.31.pdf](T.31.pdf) |
| [T.32](https://www.itu.int/rec/T-REC-T.32) | Class 2 and 2.0 fax AT commands | `dsp/fax_class2.cpp` | [T.32.pdf](T.32.pdf) |
| [T.38](https://www.itu.int/rec/T-REC-T.38) | Fax over IP: IFP packets, UDPTL | `dsp/t38.cpp`, `dsp/t30.cpp` (T.38 terminal), `frontend/ari/` | [T.38.pdf](T.38.pdf) |
| [V.8](https://www.itu.int/rec/T-REC-V.8) | Start-up: ANSam, CM/JM/CJ | `dsp/dp_v34.cpp` | [V.8.pdf](V.8.pdf) |
| [V.13](https://www.itu.int/rec/T-REC-V.13) | Simulated carrier control | `core/v13.cpp` (S48) | [V.13.pdf](V.13.pdf) |
| [V.17](https://www.itu.int/rec/T-REC-V.17) | 14400 bit/s fax modem | `dsp/fax_modems.cpp` | [V.17.pdf](V.17.pdf) |
| [V.21](https://www.itu.int/rec/T-REC-V.21) | 300 bit/s FSK | `dsp/dp_fsk.cpp`, V.8 and fax HDLC | [V.21.pdf](V.21.pdf) |
| [V.22](https://www.itu.int/rec/T-REC-V.22) | 1200 bit/s DPSK | `dsp/dp_v22.cpp` | [V.22.pdf](V.22.pdf) |
| [V.22bis](https://www.itu.int/rec/T-REC-V.22bis) | 2400 bit/s QAM | `dsp/dp_v22.cpp` | [V.22bis.pdf](V.22bis.pdf) |
| [V.18](https://www.itu.int/rec/T-REC-V.18) | Text telephones: 5-bit, EDT, automoding | `dsp/dp_text.cpp` | [V.18.pdf](V.18.pdf) |
| [V.23](https://www.itu.int/rec/T-REC-V.23) | 1200/75 bit/s FSK | `dsp/dp_fsk.cpp` | [V.23.pdf](V.23.pdf) |
| [V.25](https://www.itu.int/rec/T-REC-V.25) | Answer and calling tones | `dsp/dp_callprog.cpp` | [V.25.pdf](V.25.pdf) |
| [V.27ter](https://www.itu.int/rec/T-REC-V.27ter) | 4800/2400 bit/s modem (fax, 4-wire leased) | `dsp/fax_modems.cpp`, `dsp/dp_4wire.cpp` | [V.27ter.pdf](V.27ter.pdf) |
| [V.29](https://www.itu.int/rec/T-REC-V.29) | 9600 bit/s modem (fax, 4-wire leased) | `dsp/fax_modems.cpp`, `dsp/dp_4wire.cpp` | [V.29.pdf](V.29.pdf) |
| [V.32](https://www.itu.int/rec/T-REC-V.32) | 9600 bit/s duplex, trellis coded | `dsp/dp_v32.cpp` | [V.32.pdf](V.32.pdf) |
| [V.32bis](https://www.itu.int/rec/T-REC-V.32bis) | 14400 bit/s duplex | `dsp/dp_v32.cpp` | [V.32bis.pdf](V.32bis.pdf) |
| [V.33](https://www.itu.int/rec/T-REC-V.33) | 14400 bit/s on 4-wire leased circuits | `dsp/dp_4wire.cpp`, `dsp/fax_modems.cpp` | [V.33.pdf](V.33.pdf) |
| [V.8 bis](https://www.itu.int/rec/T-REC-V.8bis) | Identification and selection of a mode before V.8 (MR, CR, ES, MS, CL, ACK) | `dsp/v8bis.cpp`, `dsp/dp_v8bis.cpp` ([v8bis-details.md](../dsp/v8bis-details.md)) | [V.8bis.pdf](V.8bis.pdf) |
| [V.34](https://www.itu.int/rec/T-REC-V.34) | 33600 bit/s duplex | `dsp/dp_v34.cpp`, `v34_modem.cpp`, `v34_data.cpp` | [V.34.pdf](V.34.pdf) |
| [X.75](https://www.itu.int/rec/T-REC-X.75) | Packet-switched signalling (its LAPB, an ISDN data call's) | `link/lapb.cpp` (`+B2`, `docs/isdn.md`) | [X.75.pdf](X.75.pdf) |
| [V.42](https://www.itu.int/rec/T-REC-V.42) | Error correction (LAPM) | `link/lapm.cpp`, `link/hdlc.cpp` | [V.42.pdf](V.42.pdf) |
| [V.42bis](https://www.itu.int/rec/T-REC-V.42bis) | Data compression | `link/v42bis.cpp` | [V.42bis.pdf](V.42bis.pdf) |
| [V.44](https://www.itu.int/rec/T-REC-V.44) | Data compression (LZJH) | `link/v44.cpp` | [V.44.pdf](V.44.pdf) |
| [V.52](https://www.itu.int/rec/T-REC-V.52) | Test patterns (the 511-bit pattern) | `core/v54.cpp` (`AT&T7`, `AT&T8`) | [V.52.pdf](V.52.pdf) |
| [V.54](https://www.itu.int/rec/T-REC-V.54) | Loop test devices (loops 2 and 3) | `core/v54.cpp` (`AT&T`) | [V.54.pdf](V.54.pdf) |
| [V.80](https://www.itu.int/rec/T-REC-V.80) | Synchronous access mode (in-band DTE control) | `link/v80.cpp`, `core/modem.cpp` | [V.80.pdf](V.80.pdf) |
| [V.90](https://www.itu.int/rec/T-REC-V.90) | 56000 bit/s downstream, digital modem | `dsp/v90.cpp`, `dsp/dp_v34.cpp` | [V.90.pdf](V.90.pdf) |
| [V.91](https://www.itu.int/rec/T-REC-V.91) | PCM between two digital modems | `dsp/v91.cpp` | [V.91.pdf](V.91.pdf) |
| [V.92](https://www.itu.int/rec/T-REC-V.92) | Quick connect, modem-on-hold, PCM upstream | `dsp/v92.cpp`, `dsp/dp_v34.cpp` | [V.92.pdf](V.92.pdf) |
| [V.110](https://www.itu.int/rec/T-REC-V.110) | Rate adaption on an ISDN B-channel | `dsp/v110.cpp`, `dsp/dp_clear.cpp` | [V.110.pdf](V.110.pdf) |
| [V.120](https://www.itu.int/rec/T-REC-V.120) | HDLC rate adaption on an ISDN B-channel | `link/lapb.cpp`, `core/modem.cpp` (`+B2`) | [V.120.pdf](V.120.pdf) |
| [V.150.1](https://www.itu.int/rec/T-REC-V.150.1) | Modem-over-IP: modem relay (SSE, SPRT, the MoIP messages, SDP) | `dsp/dp_relay.cpp`, `frontend/dmodemd/v150.cpp`, `frontend/dmodemd/sip.cpp` ([v150-details.md](../v150-details.md)) | [V.150.1.pdf](V.150.1.pdf) |
| [V.152](https://www.itu.int/rec/T-REC-V.152) | Voice band data over IP (`a=gpmd`) | `frontend/dmodemd/sip.cpp` | [V.152.pdf](V.152.pdf) |
| [V.250](https://www.itu.int/rec/T-REC-V.250) | AT commands | `core/at.cpp` | [V.250.pdf](V.250.pdf) |
| [V.70](https://www.itu.int/rec/T-REC-V.70) | Digital simultaneous voice and data (DSVD) | `dsp/dp_v8bis.cpp`, `link/v76.cpp` ([v70-details.md](../v70-details.md)) | [V.70.pdf](V.70.pdf) |
| [V.75](https://www.itu.int/rec/T-REC-V.75) | DSVD's control: H.245's messages in V.76's frames | `link/v76.cpp`, `link/h245.cpp` | [V.75.pdf](V.75.pdf) |
| [V.76](https://www.itu.int/rec/T-REC-V.76) | A multiplexer on V.42 LAPM's procedures | `link/v76.cpp`, `link/lapm.cpp` | [V.76.pdf](V.76.pdf) |
| [G.729](https://www.itu.int/rec/T-REC-G.729) | CS-ACELP speech coding at 8 kbit/s; Annex A, its reduced complexity coder (DSVD's voice); Annex B, silence compression and comfort noise | `link/g729a_*.cpp`, `link/g729b.cpp` ([v70-details.md](../v70-details.md#g729-annex-a)) | [G.729.pdf](G.729.pdf) |
| [H.245](https://www.itu.int/rec/T-REC-H.245) | Multimedia control messages (OpenLogicalChannel and others, in aligned PER) | `link/h245.cpp` | [H.245.pdf](H.245.pdf) |
| [V.251](https://www.itu.int/rec/T-REC-V.251) | AT commands for V.8 and V.8 bis (`+A8E`) | `core/at.cpp`, `core/modem.cpp` | [V.251.pdf](V.251.pdf) |
| [V.253](https://www.itu.int/rec/T-REC-V.253) | Voice AT commands | `dsp/dsp_glue.cpp` (voice) | [V.253.pdf](V.253.pdf) |

G.729's software (its test vectors among it) is a separate download:
`docs/specs/fetch_g729.sh` puts Annex A's and Annex B's test vectors in `g729a/`,
where `link/test/g729a_test.cpp` checks D-Modem's coder against them
bit for bit (the test is skipped without them). Only the vectors are
used, as data; the reference code is not built.

V.42's Annex A, the alternative procedure (MNP 4), which D-Modem
implements (`link/mnp.cpp`), was deleted from V.42 in 2002; it is in the
superseded V.42 (10/96), `T-REC-V.42-199610-S` at itu.int
([link-details.md](../link-details.md#mnp)).

Microsoft's Unimodem Diagnostics Command Reference Specification,
version 1.0 (2 July 1998), is here as [umud10.rtf](umud10.rtf): `AT#UD`
(`core/modem.cpp`, `unimodem_diag()`) follows it.  Microsoft grants the
right to reproduce and distribute it for making hardware that complies
with it.

Not from the ITU: Bell 103 and Bell 212A (AT&T), Bell 202 caller ID (Telcordia
GR-30), TIFF-F (RFC 3949), Contact ID (SIA DC-05, `dsp/dp_contactid.cpp`), fixed-line SMS (ETSI ES 201 912 protocol 1, `dsp/dp_sms.cpp`; its messages are 3GPP TS 23.040's SMS-SUBMIT and SMS-DELIVER in TS 23.038's alphabets, `dsp/sms_pdu.cpp`). Section references in the code and in these
documents, such as "9.6.2/V.34", are to these editions.
