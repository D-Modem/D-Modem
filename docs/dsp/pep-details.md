# PEP: technical details

Telebit did not publish PEP. This page records the sources of the PEP of
D-Modem (`dsp/pep.h`, `dsp/pep.cpp`, the pump `dsp/dp_pep.cpp`), and the
items that we had to invent. For an overview, refer to [PEP](pep.md).

## Where it comes from

- US 4,679,227 (Hughes-Hartogs, 1987), "Ensemble modem structure for
  imperfect transmission media": the ensemble, the eight steps of the
  start-up, the bit loading and the tracking. Most numbers below come
  from it.
- US 4,731,816 and 4,833,706 (Hughes-Hartogs), its continuations, and
  US 4,438,511 (Baran), the packet modem behind Adaptive Duplex.
- The reference manual of the T2500 (1991): the S registers that report
  the state of PEP (S70 to S77), S50, S92, the result codes.
- Two Telebit notes of 1990, the overview of Ballard and a note about PEP
  on poor lines: the rates, the packet sizes, Adaptive Duplex.

We read the firmware of a T3000 (LA3.00, 1991). Its 68000 ROM holds the
PEP program of its TMS320C25 DSP. The items that it gives are in
[What the T3000's firmware shows](#what-the-t3000s-firmware-shows). We did
not compare the other items below with a Telebit.

## The ensemble

| | Value | Source |
|---|---|---|
| Sample rate | 8000 Hz (9600 resampled to it) | '227 |
| FFT | 1024 points, 128 ms | '227 |
| Carriers | 511, bins 1 to 511, 7.8125 Hz apart | '227, S71/S73/S76 |
| Guard | the first 61 samples of the symbol, repeated after it (64 in the documents of Telebit); 56 in turns | the T3000, T2000 recordings |
| Epoch | 1085 samples, 135.6 ms, 7.37 baud; 1080 in turns | the T3000, T2000 recordings |
| Bits a carrier | 0, 2, 4, 5 or 6, the constellations of Telebit | '227, Ballard, the T3000 |
| 0 dBR | -9 dBm0 | '227 |

The two timing tones, 1437.5 Hz and 1687.5 Hz (bins 184 and 216), come
from the patent. In data epochs, D-Modem keeps their carriers as pilots
at the level of the comb (this is a decision of D-Modem).

## The start-up

The steps of the patent, in sequence:

1. The answerer sends an epoch of the two timing tones at -3 dBR each.
   Then it sends a comb of all 511 carriers at -27 dBR. (D-Modem sends 4
   comb epochs, with the signs - + - +. The receiver locks its timing to
   the last epoch. The other epochs measure the stability of each
   carrier.)
2. The two sides measure the noise of the line in silence (3 FFTs).
3. The caller sends its tones and comb in the same method.
4. The caller sends a phase coded epoch at -28 dBR: 180 degrees on the
   carriers that can carry 2 bits in the direction of the answerer, 0 on
   the other carriers. (D-Modem sends it two times, for the margin.)
5. The answerer replies with the carriers that can carry 2 bits in the
   two directions. (D-Modem sends this in the same form, at the start of
   a turn.)
6. On those carriers (the "link"), each side sends to the other, in
   packets, the bits and power of each carrier in its direction.

A carrier carries b bits when its SNR is the necessary SNR of the QAM for
b bits plus a margin of 3 dB. The SNR is the power of the channel
compared with the larger of these two values: the noise of the silence
and the instability of the comb. The loading uses the method of
Hughes-Hartogs. The next bit always goes where it costs the least power,
until the loading uses all the power of the comb.

The loading record (an invention of D-Modem) is 'L' and 10 bits for each
carrier: 4 for its bits, 6 for its power compared with the power of the
comb (from -8 dB, in 0.25 dB steps). The call connects (`CONNECT`) after
each side has received a turn of the other side on its own loading.

## Turns and packets (D-Modem's own)

A turn has these parts:

1. 20 ms of silence.
2. 32 ms of the two timing tones.
3. One comb epoch. Its sign tells the loading: + for the link, - for the
   final loading.
4. One packet for each epoch, minimum 1 and maximum 10 (`turn_max`, about
   3 KB at the highest rates).

The receiver finds the end of the tones in 128 sample blocks, then the
timing and channel of the comb. The receiver calculates the timing and
phase of each data epoch from the decided points by least squares. From
these values, it follows the channel and the clock of the far side. The
receiver interpolates the window on the clock of the far side.

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
acknowledge. A side with no data to send sends one fill packet (after the
final loading, a micro turn: refer to the subsequent section). Thus, the
other side gets the line back immediately (Adaptive Duplex). If no turn
comes (the far side is lost), the caller takes the line again after
0.6 s, and the answerer after 0.9 s. This does not occur if the signal on
the line is above its quiet level. (Then the far side is in a turn whose
tones the receiver did not hear.)

## Micro turns (D-Modem's own)

The documents of Telebit give its micro packets 88.26 baud on some
carriers. The DSP of the T3000 has a short epoch of 64 samples and 56 of
guard (120 samples, 66.7 baud) on 32 carriers. We do not know the layout
of the packets. The micro turn of D-Modem has these parts:

1. 20 ms of silence.
2. 24 ms of the two tones (`PRE_SHORT`).
3. Four of those short epochs: one short comb, then three data
   epochs of one packet.

A short epoch uses each 16th carrier (125 Hz apart, at odd multiples of
62.5 Hz from 312.5 to 3312.5 Hz, as the micro epochs of the T3000 do).
Its amplitude is 4 times the amplitude of the long epoch. This is 16
times the power, because the long epoch has the same power on 16 times
more carriers. Thus, each carrier carries 2 bits more than the final
loading gives that carrier, 6 at most.

The short symbol is the first 120 samples of the 1024 point IFFT with
only those bins. (Their period is 128. Thus, each 64 of them is a
window.) Thus, the channel of the long comb applies to it without
changes.

A side sends a micro turn when it has no packet to send again, no
control data, and less data than one micro packet holds (about 50
octets at 30 dB). On the final loading, a receiver first tries each turn
as a micro turn:

1. It examines 160 samples from the end of the tones for the short comb
   (the phase steps of the adjacent carriers, compared with the channel,
   coherent above 0.8).
2. It refines its timing from their mean, and gets the common phase from
   it.
3. It decodes the packet.

If the CRC is bad (a long turn), the receiver continues to the long comb.
A micro turn takes about 90 ms. A turn of one long packet takes 330 ms.

## The pump and the link

The pump (`dp_pep.cpp`) gives the bits of the link layer to PEP as octets.
The link can frame them (V.42, MNP or start-stop characters). The core
tells this with `dp_runtime_set_framed()`. In this condition, the pump
takes them at the rate that the link got at `CONNECT`, up to two long
turns ahead. Thus, the bit timers of the link keep the correct time. The
pump removes the idle of the link:

- Ones after 9 kept ones (no character or frame has more).
- Flags after 16 kept flags. (The answerer of V.42 must have some flags
  to see one as a character. Asynchronous characters never repeat one
  8 bit pattern three times.)

The received octets go to the link at the line rate of the far side, not
all the octets of a turn at the same time. The bits of a synchronous call
are a raw stream, and they go without changes.

In PEP, the caller does not do the detection phase of V.42. It starts
LAPM immediately, because the ODPs and the answer to them would take two
turns in each direction. This is more than T400 (750 ms). The answerer
keeps its detection phase. This phase stops on the flags of the caller,
or accepts the LR of MNP.

## Automode and the answer tone

Before the tones of PEP, the caller monitors the line in 10 ms blocks:
the power at 2100 Hz, at the two tones, and the envelope of the answer
tone. Its AM depth over 400 ms shows the difference between ANSam and a
plain answer tone. (The AM depth is the envelope minus its mean, compared
with the mean. ANSam is above 0.1.) The caller of V.34 measures it in the
same method. The answer tone continues through the phase reversals of
ANSam (gaps of up to 5 blocks).

After the answer tone, 150 ms of a signal that is not the tones of PEP
and not 2100 Hz is a different modulation. The pump gives the call to a
different pump with `DPSTAT_CHANGEDP`:

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
- For each of the 512 bins, the bits in each direction and the noise
  measured in the start-up (`noise_dbm0()`).

`ATI11` shows the counts, and `ATI12` shows the table. (The S70 to S75
of Telebit would conflict with the S70, S71 and S92 of D-Modem. Thus,
they are not S-registers.)

## What the T3000's firmware shows

A Telebit T3000 (LA3.00, 18 September 1991) keeps the programs of its DSP
in the two EPROMs of its 68000. It loads one program into the TMS320C25
when it is necessary. We disassembled the PEP program (about 12,600
words, output of the TI C compiler). The addresses below are addresses in
its program memory.

**Confirmed.** The FFT has 1024 points (a 512 point complex radix-4 FFT
and a split stage, with its twiddles at 0x96EB). Thus, there are 511
carriers, 7.8125 Hz apart at 8000 Hz. The timing tones are bins 184 and
216. The loading is a table of 512 carriers, as S71 and S73 show it.

**The constellations** (the mapper at 0x8B26, the points at 0x8C0C). The
bits go out least significant first, carrier by carrier, from the lowest
bin. Each carrier takes the lowest b bits of the remaining bits. For the
value v of a carrier, the two lowest bits are the quadrant (bit 1 makes x
negative, bit 0 makes y negative). The other bits select the point in the
first quadrant from one nested table (at 0x8C7E, odd coordinates):

    (1,1) (1,3) (3,1) (3,3) (1,7) (1,5) (3,7) (3,5) (7,1) (7,3) (5,1)
    (5,3) (7,7) (7,5) (5,7) (5,5) (1,15) (1,13) ... (11,9) (11,11)

The first 4 points are the quadrant of 16-QAM. The first 16 points are
the quadrant of 64-QAM, and all 64 points are the quadrant of 256-QAM.
Each axis has the sequence 1, 3, 7, 5, 15, 13, 9, 11 (Gray coded). Thus,
2 bits are QPSK, 4 bits are 16-QAM, 6 bits are 64-QAM and 8 bits are
256-QAM.

- 5 and 7 bits are cross constellations. They go through tables into the
  same table (5: 0, 1, 2, 3, 10, 5, 11, 7; 7: 0 to 15, 40 to 43, 20 to 23,
  44 to 47, 28 to 31).
- 3 bits are an eight point star: (±1, ±1) and (0, ±3), (±3, 0).

D-Modem maps its 2, 4, 5 and 6 bits in this method (`tb_map()`).

**The epochs.** The mapper gets the type of an epoch:

- All 512 bins (the long epoch).
- Each 8th bin (64 carriers, 62.5 Hz apart).
- Bins 8, 24, 40 to 504 (32 carriers, 125 Hz apart, at odd multiples of
  62.5 Hz).

The last two types are almost certainly the small packets and the micro
packets. The carriers of a micro epoch use the bits of the long loading
at those bins. The micro turns of D-Modem use the bins of the T3000
(8 + 16j, 312.5 to 3312.5 Hz).

**Epoch lengths** (0x8D38 writes a guard, then the block):

| Epoch | Block | Guard | Total |
|---|---|---|---|
| 32 carriers | 64 | 56 | 120 |
| 64 carriers | 128 | 53 | 181 |
| Long, alone | 1024 | 61 | 1085 |
| Long, with 32-carrier epochs | 1024 | 56 | 1080 (9 short epochs) |
| Long, with 64-carrier epochs | 1024 | 62 | 1086 (6 short epochs) |

Thus, the two types of epoch stay on one grid. The recordings of a T2000
show the same: 1085 in the start-up, 1080 in each data turn. The epochs
of D-Modem have the same lengths: 1085 in the start-up, 1080 in turns and
120 in micro turns.

**Data mode.** The 68000 gives the DSP one epoch at a time: a word with a
long-epoch flag and a 4-bit tag, then the octets. The DSP sends what it
received back in the same method. An epoch holds a whole number of
octets: the sum of the bits of its carriers, rounded up to octets. There
is no scrambler between the octets and the mapper. A received tag of 3
ends the turn of the far side. The tag goes on the carriers of the timing
tones.

**The carriers of the timing tones** never carry the bits of the packet.
Each one carries 2 bits of the 4-bit field of the epoch (bin 184 its bits
0 and 1, bin 216 bits 2 and 3), the same in each epoch type. The 68000
gives the field with each block of octets. The value 3 marks the last
epoch of a turn. We do not know the other values. D-Modem still sends
these carriers as pilots.

**The comb.** The training comb has all carriers at one amplitude. The
T3000 sends it in two forms:

- All carriers at phase 0.
- Bins 1 to 451 turned by pseudo-random phases (0xAE8A, 0xB089). A 15 bit
  LFSR starts from 0. The new bit of each carrier is not (bit 14 xor
  bit 3). Its 4 lowest bits select one of 16 phases (at 0xB069, in
  degrees: 116.0, 120.9, 113.2, 50.6, -56.2, 10.6, -85.1, -26.0, 61.2,
  100.6, 41.5, 13.4, -171.6, -73.8, 156.1, -21.8).

The peak of this comb is about 14 dB above its RMS. The peak of the comb
with the phases of Newman is 3.4 dB above its RMS. The comb of D-Modem is
at -27 dBR for each carrier. At that level, the comb of the T3000 clips
on a line that is 6 dB too loud. Thus, D-Modem keeps the phases of
Newman.

**The start-up** is a script of 59 steps (0xAAAE, with a second script
at 0xAC30 that receives). Each step is one epoch or gap. The script of one
side has these steps:

1. The two timing tones.
2. The comb at phase 0.
3. Tone 184 alone until the other side answers.
4. 8 combs with the phases of the LFSR.
5. The comb at phase 0.
6. 3 silent epochs.
7. 1085 samples of 2125 Hz.
8. The comb at phase 0.
9. An epoch with the carriers that can carry 2 bits at 180 degrees (as
   in the patent).
10. One more epoch with 16 bits on its first 16 of these carriers also.
11. 3 silent epochs.

The script of the other side is the same, with 9 combs and a few steps
in a different sequence. The answerer sends first: the 68000 selects
the script from whether it dialled (ATD). The 16 bits are the constant
0xF001 in this firmware. We do not know the levels yet.

**Not found.** There is no CRC. The program of the DSP and the PEP code
of the 68000 do not calculate one. (The 68000 has a CRC-16 of the 0x8005
family, seed 0x0707, but no code calls it.) The header of the packets,
the acknowledgements and the retransmission are not in this firmware.
The PEP code of the 68000 only sends "TELEBIT 2 bit test message." and
two other fixed frames, and it discards what it receives. The T3000
shipped without PEP. PEP came later in the WorldBlazer. Its firmware
(sold as a T3000 upgrade of two ROMs and a PAL) must hold the packet
layer. To find the packets, the firmware of a WorldBlazer or a T2500 is
necessary, or a recording decoded to bits.

## What the T2000 recordings show

We measured five recordings of a Telebit T2000 (Wikimedia Commons: call
set-up, duplex and simplex data, XMODEM with and without spoofing):

- The answerer sends 3.1 s of plain 2100 Hz and 70 ms of silence before
  the tones of PEP.
- The epoch is 1024 samples at 8000 Hz plus a guard. The guard is a
  copy of the start of the symbol, after the symbol. The epoch is 1085
  samples in the start-up, and 1080 (56 of guard) in each data turn,
  simplex or duplex. The firmware of the T3000 does this when it sends
  long epochs together with micro epochs. The comb (E0) is the same in the two
  directions. The 9 training epochs are also the same.
- The carriers of the T2000 stop at bin 434 (3390.6 Hz).
- A turn starts with one E0 epoch and no tones. On the 2-bit link, the
  data is QPSK multiplied by the phases of E0.
- Long turns are about 1.8 s: about 390 samples, then 13 epochs of
  1080. An idle line is a burst of 360 samples in each direction each 960
  samples (120 ms). A burst is three micro epochs of 120 (64 and a guard
  of 56). Their carriers are at odd multiples of 62.5 Hz, so the guard is
  the start of the symbol, negated. The first epoch is a flat comb. The
  next two have about twelve strong carriers at 0 or 180 degrees to it.
  The two sides send the same burst, and all bursts are the same.

We did not decode a turn to bits. Thus, we do not know the packets, the
scrambler and the loading record of a real Telebit.

## Not done

- The lead tone of S36, S120; the small packets of Telebit.
- The compression of Telebit (S110), UUCP g, Kermit and XMODEM spoofing
  (S111).
- Turbo PEP (the PEP of the WorldBlazer, 7 bits for each carrier, 23 kbit/s).
- Communication with a real Telebit (we do not know its packets).
- The comb phases of the T3000, its start-up script, and the 4-bit field
  on the carriers of the timing tones (refer to
  [What the T3000's firmware shows](#what-the-t3000s-firmware-shows)).
