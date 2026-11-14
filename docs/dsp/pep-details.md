# PEP: technical details

Telebit did not publish PEP. This page records the sources of the version of D-Modem
(`dsp/pep.h`, `dsp/pep.cpp`, the pump `dsp/dp_pep.cpp`), and
the items that we had to invent. For an overview, refer to [PEP](pep.md).

## Where it comes from

- US 4,679,227 (Hughes-Hartogs, 1987), "Ensemble modem structure for
  imperfect transmission media": the ensemble, the eight
  steps of the start-up, the bit loading, the tracking. Most numbers below come from it.
- US 4,731,816 and 4,833,706 (Hughes-Hartogs), its continuations, and
  US 4,438,511 (Baran), the packet modem behind Adaptive Duplex.
- The reference manual of the T2500 (1991): the S registers that report
  the state of PEP (S70 to S77), S50, S92, the result codes.
- Two Telebit notes of 1990, the overview of Ballard and a note about PEP on poor
  lines: the rates, the packet sizes, Adaptive Duplex.

We did not find a firmware image of a TrailBlazer, T1000, T2000 or T2500.
We also did not find a recording of a call with the demodulated line. Thus,
we did not compare any item below with a Telebit.

## The ensemble

| | Value | Source |
|---|---|---|
| Sample rate | 8000 Hz (9600 resampled to it) | '227 |
| FFT | 1024 points, 128 ms | '227 |
| Carriers | 511, bins 1 to 511, 7.8125 Hz apart | '227, S71/S73/S76 |
| Guard | the first 64 samples of the symbol (8 ms), repeated after it | '227 |
| Epoch | 1088 samples, 136 ms, 7.35 baud | '227, Ballard |
| Bits a carrier | 0, 2, 4, 5 or 6 (QAM, each axis Gray coded) | '227, Ballard |
| 0 dBR | -9 dBm0 | '227 |

The two timing tones, 1437.5 Hz and 1687.5 Hz (bins 184 and 216), come from
the patent. In data epochs, D-Modem keeps their carriers as pilots at
the level of the comb (this is the selection of D-Modem).

## The start-up

The steps of the patent, in sequence:

1. The answerer sends an epoch of the two timing tones at -3 dBR each.
   Then it sends a comb of all 511 carriers at -27 dBR. (D-Modem sends 4 comb epochs,
   with the signs - + - +. The receiver locks its timing to the last epoch, and the
   other epochs measure the stability of each carrier.)
2. The two sides measure the noise of the line in silence (3 FFTs).
3. The caller sends its tones and comb in the same method.
4. The caller sends a phase coded epoch at -28 dBR: 180 degrees on the
   carriers that can carry 2 bits in the direction of the answerer, 0 on the others
   (D-Modem sends it two times, for the margin).
5. The answerer replies with the carriers that can carry 2 bits in the two directions
   (D-Modem: in the same form, at the start of a turn).
6. On those carriers (the "link"), each side sends to the other, in
   packets, the bits and power of each carrier in its direction.

A carrier carries b bits when its SNR is the necessary SNR of the QAM for b bits plus a margin of 3 dB.
The SNR is the power of the channel compared with the larger of the noise of the silence and the
instability of the comb. The loading uses the method of
Hughes-Hartogs. The next bit always goes where it costs the least
power, until all the power of the comb is used. The loading record (an invention of D-Modem)
is 'L' and 10 bits for each carrier: 4 for its bits, 6 for its power
compared with the power of the comb, -8 dB up in 0.25 dB steps. The call is up
(`CONNECT`) after each side has received a turn of the other side on its own
loading.

## Turns and packets (D-Modem's own)

A turn has these parts:

1. 20 ms of silence.
2. 32 ms of the two timing tones.
3. One comb epoch. Its sign tells the loading: + for the link, - for the final loading.
4. One packet for each epoch, minimum 1 and maximum 10 (`turn_max`, about 3 KB
   at the top rates).

The receiver finds the end of the tones in 128 sample
blocks, then the timing and channel of the comb. The timing
and phase of each data epoch come from the decided points by least squares. The receiver follows
the channel and the clock of the far side from them. The window is
interpolated on the clock of the far side.

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

All of the packet is scrambled. The window is 64 packets, with selective
retransmission. A turn starts with the packets that the far side did not
acknowledge. A side with no data to send sends one fill packet (a micro
turn, refer to the subsequent section, after the final loading). Thus, the other side gets the
line back immediately (Adaptive Duplex). If no turn
comes (the far side is lost), the caller takes the line again after
0.6 s, and the answerer after 0.9 s. This does not occur if the line carries more than its
quiet level (the far side is in a turn whose tones the receiver did not hear).

## Micro turns (D-Modem's own)

The micro packets of Telebit use 88.26 baud on some carriers. We do not know their
layout. The micro turn of D-Modem has these parts:

1. 20 ms of silence.
2. 24 ms of the two tones (`PRE_SHORT`).
3. Four short epochs of 64 samples and 26
   of guard (90 samples, 88.9 baud): one short comb, then three data
   epochs of one packet.

A short epoch uses each 16th carrier (125 Hz
apart, 375 to 3375 Hz). Its amplitude is 4 times the amplitude of the long epoch (16 times
the power, because the power of the long epoch is spread over 16 times as many
carriers). Thus, each carrier carries 2 bits more than the final loading gives that
carrier, 6 at most. The short symbol is the first 64 samples of the
1024 point IFFT with only those bins. Thus, the channel of the long comb
applies to it without changes.

A side sends a micro turn when it has no packet to send again, no
control data, and less data than one micro packet holds (about 50
octets at 30 dB). On the final loading, a receiver first tries each turn as a
micro turn:

1. It scans 160 samples from the end of the tones for the
   short comb (the phase steps of the adjacent carriers, compared with the channel,
   coherent above 0.8).
2. It refines its timing from their mean, and gets the
   common phase from it.
3. It decodes the packet.

If the CRC is bad (a long turn), the receiver continues to the long comb. A micro turn
takes about 90 ms. A turn of one long packet takes 330 ms.

## The pump and the link

The pump (`dp_pep.cpp`) gives the bits of the link layer to PEP as octets.
The link can frame them (V.42, MNP or start-stop characters; the
core tells this with `dp_runtime_set_framed()`). In this condition, the pump takes them at
the rate that the link got at `CONNECT` (thus, its bit timers keep the correct time),
up to two long turns ahead. The pump removes its idle:

- Ones after 9 kept ones (no character or frame has more).
- Flags after 16 kept flags. (The answerer of V.42 must have some flags to see one as a character, and
  asynchronous characters never repeat one 8 bit pattern three times.)

The received octets go to the link at the line rate of the far side, not all the octets of a turn
at the same time. The bits of a synchronous call are a raw stream, and they go without changes.

In PEP, the caller does not do the detection phase of V.42. It starts LAPM
immediately, because the ODPs and the answer to them would take two turns in each direction,
more than T400 (750 ms). The answerer keeps its detection phase. This phase stops
on the flags of the caller, or accepts the LR of MNP.

## Automode and the answer tone

Before the tones of PEP, the caller monitors the line in 10 ms
blocks: the power at 2100 Hz, at the two tones, and the envelope of the answer tone.
Its AM depth over 400 ms (the envelope minus its mean, compared with
the mean; above 0.1) shows the difference between ANSam and a plain answer tone. The caller of V.34
now measures it in the same method. The answer tone continues through the
phase reversals of ANSam (gaps of up to 5 blocks). After it, 150 ms of a signal
that is not the tones of PEP and not 2100 Hz is a different modulation. The pump
gives the call to a different pump with `DPSTAT_CHANGEDP`:

- V.34 for ANSam and for the answerer.
- V.32bis with its automode for other answer tones.
- Bell 212A for 2225 Hz.

Then the statistics of the call are not the statistics of PEP.

## Statistics

At `CONNECT` and on each subsequent block, the pump fills the
statistics of the call:

- The line rates.
- The counts of the packets.
- The micro turns.
- For each of the 512 bins, the bits in each direction and the noise measured in
  the start-up (`noise_dbm0()`).

`ATI11` shows the counts, and `ATI12` shows the
table. (The S70 to S75 of Telebit would conflict with the S70, S71 and
S92 of D-Modem. Thus, they are not S-registers.)

## What the T2000 recordings show

We measured five recordings of a Telebit T2000 (Wikimedia Commons: call set-up,
duplex and simplex data, XMODEM with and without spoofing):

- The answerer sends 3.1 s of plain 2100 Hz and 70 ms of silence before
  the tones of PEP.
- The epoch is 1024 samples at 8000 Hz plus a guard of about 61 (64 in
  the documents of Telebit; the lossy audio makes the measurement rough). The guard is a
  copy of the start of the symbol, after the symbol. The comb (E0) is the same in the two
  directions. The 9 training epochs are also the same.
- The carriers of the T2000 stop at bin 434 (3390.6 Hz).
- A turn starts with one E0 epoch and no tones. On the 2-bit link, the
  data is QPSK multiplied by the phases of E0.
- Long turns are about 1.8 s. An idle line is a 46 ms burst in each direction
  each 120 ms, with about twelve carriers each. We could not find their epoch.

We did not decode a turn to bits. Thus, we do not know yet the packets, the scrambler and the
loading record of a real Telebit.

## Not done

- The lead tone of S36, S120; the small packets of Telebit.
- The compression of Telebit (S110), UUCP g, Kermit and XMODEM spoofing
  (S111).
- Turbo PEP (the PEP of the WorldBlazer, 7 bits for each carrier, 23 kbit/s).
- Communication with a real Telebit (we do not know its packets).
