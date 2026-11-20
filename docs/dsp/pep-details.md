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

The documents of Telebit give its micro packets 88.26 baud on some
carriers. The DSP of the T3000 has a short epoch of 64 samples and 56 of
guard (120 samples, 66.7 baud) on 32 carriers. We do not know the layout
of the packets. The micro turn of D-Modem has these parts:

1. 20 ms of silence.
2. 24 ms of the two tones (`PRE_SHORT`).
3. Four of those short epochs: one short comb, then three data
   epochs of one packet.

A short epoch uses each 16th carrier (125 Hz apart, at odd multiples of
62.5 Hz from 312.5 to 3312.5 Hz, as the micro epochs of the T3000 do). Its amplitude is 4 times the amplitude of the long epoch (16 times
the power, because the power of the long epoch is spread over 16 times as many
carriers). Thus, each carrier carries 2 bits more than the final loading gives that
carrier, 6 at most. The short symbol is the first 120 samples of the
1024 point IFFT with only those bins (their period is 128, so each 64 of
them is a window). Thus, the channel of the long comb
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

## What the T3000's firmware shows

A Telebit T3000 (LA3.00, 18 September 1991) keeps its DSP's programs in
the 68000's two EPROMs and loads one into the TMS320C25 when it is
needed. The PEP one (about 12,600 words, TI C compiler output) was
disassembled; the addresses below are its program memory's.

**Confirmed.** The FFT is 1024 points (a 512 point complex radix-4 one
and a split stage; its twiddles at 0x96EB), so 511 carriers 7.8125 Hz
apart at 8000 Hz; the timing tones are bins 184 and 216; the loading is
a table of 512 carriers, as S71 and S73 print it.

**The constellations** (the mapper at 0x8B26, the points at 0x8C0C).
Bits go out least significant first, carrier by carrier up from the
lowest bin, each carrier taking the lowest b of what is left. Of a
carrier's value v, the two lowest bits are the quadrant (bit 1 makes x
negative, bit 0 y), the rest pick the point in the first quadrant from
one nested table (at 0x8C7E, odd coordinates):

    (1,1) (1,3) (3,1) (3,3) (1,7) (1,5) (3,7) (3,5) (7,1) (7,3) (5,1)
    (5,3) (7,7) (7,5) (5,7) (5,5) (1,15) (1,13) ... (11,9) (11,11)

Its first 4 are 16-QAM's quadrant, its first 16 64-QAM's, all 64
256-QAM's; each axis runs 1, 3, 7, 5, 15, 13, 9, 11 (Gray coded). So 2
bits are QPSK, 4 16-QAM, 6 64-QAM and 8 256-QAM. 5 and 7 bits are the
cross constellations, through tables into the same one (5:
0, 1, 2, 3, 10, 5, 11, 7; 7: 0 to 15, 40 to 43, 20 to 23, 44 to 47, 28 to
31), and 3 bits an eight point star, (±1, ±1) and (0, ±3), (±3, 0).
D-Modem now maps its 2, 4, 5 and 6 bits this way (`tb_map()`); its 5
bits were a rectangle before.

**The epochs.** The mapper takes an epoch's type: all 512 bins (the long
epoch), every 8th bin (64 carriers, 62.5 Hz apart) or bins 8, 24, 40 ...
504 (32 carriers 125 Hz apart at odd multiples of 62.5 Hz). The last two
are surely the small and micro packets. A micro epoch's carriers use the
long loading's bits at those bins. D-Modem's micro turns now use the
T3000's bins (8 + 16j, 312.5 to 3312.5 Hz) instead of every 16th bin.

**Epoch lengths** (0x8D38 writes a guard, then the block). The 32
carrier epoch is a 64 sample block and 56 of guard (120 samples), the
64 carrier one a 128 sample block and 53 of guard (181). The long epoch
is 1024 and 61 (1085) when only long epochs are sent, and is cut to
1080 (9 short ones of 120) or stretched to 1086 (6 of 181) when it is
mixed with short ones, so both kinds stay on one grid. A T2000's
recordings show the same: 1085 in the start-up, 1080 in every data
turn. D-Modem's start-up epochs are 1085, its turns' 1080 and its micro
epochs 120 (they were 1088, 1088 and 90).

**Data mode.** The 68000 hands the DSP one epoch at a time: a word with
a long-epoch flag and a 4-bit tag, then the octets; the DSP sends what it
received back the same way. An epoch holds a whole number of octets,
the sum of its carriers' bits rounded up to octets. There is no
scrambler between the octets and the mapper. A received tag of 3 ends
the far side's turn. The tag rides on the timing tones' carriers.

**The timing tones' carriers** never carry the packet's bits: each
carries 2 bits of a 4-bit field given with each epoch (bin 184 its bits
0 and 1, bin 216 bits 2 and 3), the same in every epoch type. The
68000 gives the field with each block of octets it hands the DSP; 3
marks the last epoch of a turn (the other values are not known). D-Modem
still sends them as pilots.

**The comb.** The training comb is every carrier at one amplitude; the
T3000 sends it two ways, all at phase 0 and with bins 1 to 451 turned by
pseudo-random phases (0xAE8A, 0xB089): a 15 bit LFSR from 0, each
carrier's new bit not (bit 14 xor bit 3), its 4 lowest bits one of 16
phases (at 0xB069, in degrees 116.0, 120.9, 113.2, 50.6, -56.2, 10.6,
-85.1, -26.0, 61.2, 100.6, 41.5, 13.4, -171.6, -73.8, 156.1, -21.8). Its
peak is about 14 dB over its RMS against Newman's 3.4, so D-Modem, whose
comb is at -27 dBR a carrier, keeps Newman's: the T3000's at that level
clips on a line 6 dB hot.

**The start-up** is a script of 59 steps (0xAAAE; a second at 0xAC30
receives), one epoch or gap a step. One side's: both timing tones; the
comb at phase 0; tone 184 alone until the other side answers; 8 combs
with the LFSR's phases; the comb at phase 0; 3 silent epochs; 1085
samples of 2125 Hz; the comb at phase 0; an epoch with the carriers
fit for 2 bits at 180 degrees (the patent's); one more with 16 bits on
its first 16 such carriers as well; 3 silent epochs. The other side's is
the same with 9 combs and a few steps in another order. The answerer
sends first: the 68000 picks the script from whether it dialled (ATD).
The 16 bits are the constant 0xF001 in this firmware; the levels are not
settled yet.

**Not found.** No CRC: neither the DSP's program nor the 68000's PEP
code computes one (the 68000's has a CRC-16 of the 0x8005 family, seed
0x0707, that nothing calls). The packets' header, acknowledgements and
retransmission are not in this firmware at all: its 68000's PEP code
only sends "TELEBIT 2 bit test message." and two other canned frames,
and what it receives goes nowhere. The T3000 shipped without PEP; PEP
came later in the WorldBlazer, whose firmware (sold as a T3000 upgrade of
two ROMs and a PAL) must hold the packet layer. It needs a WorldBlazer's
or a T2500's firmware, or a recording decoded to bits.

## What the T2000 recordings show

We measured five recordings of a Telebit T2000 (Wikimedia Commons: call set-up,
duplex and simplex data, XMODEM with and without spoofing):

- The answerer sends 3.1 s of plain 2100 Hz and 70 ms of silence before
  the tones of PEP.
- The epoch is 1024 samples at 8000 Hz plus a guard. The guard is a
  copy of the start of the symbol, after the symbol. The epoch is 1085
  samples in the start-up, and 1080 (56 of guard) in each data turn,
  simplex or duplex. The firmware of the T3000 does this when long epochs
  are mixed with micro epochs. The comb (E0) is the same in the two
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

We did not decode a turn to bits. Thus, we do not know yet the packets, the scrambler and the
loading record of a real Telebit.

## Not done

- The lead tone of S36, S120; the small packets of Telebit.
- The compression of Telebit (S110), UUCP g, Kermit and XMODEM spoofing
  (S111).
- Turbo PEP (the PEP of the WorldBlazer, 7 bits for each carrier, 23 kbit/s).
- Communication with a real Telebit (we do not know its packets).
- The comb phases of the T3000, its start-up script, and the 4-bit field on
  the carriers of the timing tones (refer to the T3000 section).
