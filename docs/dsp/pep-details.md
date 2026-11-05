# PEP: technical details

Telebit never published PEP. This page records what D-Modem's version
(`dsp/pep.h`, `dsp/pep.cpp`, the pump `dsp/dp_pep.cpp`) is based on and
what it had to make up. For an overview, see [PEP](pep.md).

## Where it comes from

- US 4,679,227 (Hughes-Hartogs, 1987), "Ensemble modem structure for
  imperfect transmission media": the ensemble, the start-up's eight
  steps, the bit loading, the tracking. Most numbers below are its.
- US 4,731,816 and 4,833,706 (Hughes-Hartogs), its continuations, and
  US 4,438,511 (Baran), the packet modem behind Adaptive Duplex.
- The T2500's reference manual (1991): the S registers that report
  PEP's state (S70 to S77), S50, S92, the result codes.
- Two Telebit notes of 1990, Ballard's overview and one on PEP on poor
  lines: the rates, the packet sizes, Adaptive Duplex.

No firmware image of a TrailBlazer, T1000, T2000 or T2500 has been
found, nor a recording of a call made with the line demodulated, so
nothing below has been checked against a Telebit.

## The ensemble

| | Value | Source |
|---|---|---|
| Sample rate | 8000 Hz (9600 resampled to it) | '227 |
| FFT | 1024 points, 128 ms | '227 |
| Carriers | 511, bins 1 to 511, 7.8125 Hz apart | '227, S71/S73/S76 |
| Guard | the symbol's first 64 samples (8 ms) repeated after it | '227 |
| Epoch | 1088 samples, 136 ms, 7.35 baud | '227, Ballard |
| Bits a carrier | 0, 2, 4, 5 or 6 (QAM, each axis Gray coded) | '227, Ballard |
| 0 dBR | -9 dBm0 | '227 |

The two timing tones, 1437.5 Hz and 1687.5 Hz (bins 184 and 216), are
the patent's. In data epochs D-Modem keeps their carriers as pilots at
the comb's level (its own choice).

## The start-up

The patent's steps, in order:

1. The answerer sends an epoch of the two timing tones at -3 dBR each,
   then a comb of all 511 carriers at -27 dBR (D-Modem: 4 comb epochs,
   their signs - + - +, so that the last is the one the receiver's
   timing locks to and the others measure how steady each carrier is).
2. Both measure the line's noise in silence (3 FFTs).
3. The caller sends its tones and comb the same way.
4. The caller sends a phase coded epoch at -28 dBR: 180 degrees on the
   carriers that take 2 bits the answerer's way, 0 on the others
   (D-Modem sends it twice, for the margin).
5. The answerer replies with the carriers that take 2 bits both ways
   (D-Modem: in the same form, at the start of a turn).
6. On those carriers (the "link"), each side sends the other, in
   packets, the bits and power of each carrier its way.

A carrier takes b bits when its SNR (the channel's power against the
larger of the silence's noise and the comb's unsteadiness) is the QAM's
need for b bits plus a margin of 3 dB. The loading follows
Hughes-Hartogs: the next bit always goes where it costs the least
power, until the comb's power is spent. The loading record (D-Modem's
own) is 'L' and 10 bits a carrier: 4 for its bits, 6 for its power
against the comb's, -8 dB up in 0.25 dB steps. The call is up
(`CONNECT`) once each side has had a turn of the other's on its own
loading.

## Turns and packets (D-Modem's own)

A turn is 20 ms of silence, 32 ms of both timing tones, one comb epoch
(its sign telling the loading: + the link, - the final one) and then
one packet an epoch, at least 1 and at most 10 (`turn_max`, about 3 KB
at the top rates). The receiver finds the tones' end in 128 sample
blocks, then the comb's timing and channel; each data epoch's timing
and phase come from the decided points by least squares, and the
channel and the far side's clock are followed from them. The window is
interpolated on the far side's clock.

A packet is

| Octets | |
|---|---|
| 1 | type (data, fill, control) and the length's top 4 bits |
| 1 | the length's low 8 bits |
| 1 | sequence number |
| 1 | epochs left in the turn |
| 1 | the next sequence number expected |
| 2 | which of the 16 after it have arrived |
| n | payload |
| 2 | CRC-16 (CCITT) |

all scrambled. The window is 64 packets, with selective
retransmission: a turn starts with the packets the far side has not
acknowledged. A side with nothing to send sends one fill packet, so the
other side gets the line back at once (Adaptive Duplex). If no turn
comes (the far side's lost), the caller takes the line again after
0.6 s, the answerer after 0.9 s, unless the line carries more than its
quiet level (the far side in a turn whose tones were missed).

## Not done

- The answer sequence of a T2500 (PEP tones, then the V.25 answer tone
  and the other modulations, S92), so a PEP modem can answer anything.
- The micro and small packets (88.26 baud, for typing), S36's lead
  tone, S120.
- Compression (S110), UUCP g, Kermit and XMODEM spoofing (S111), the
  S70 to S77 statistics.
- Turbo PEP (the WorldBlazer's, 7 bits a carrier, 23 kbit/s).
