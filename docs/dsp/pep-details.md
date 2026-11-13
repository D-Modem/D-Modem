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

A T3000's firmware (LA3.00, 1991) has since been read: its 68000 ROM
holds the PEP program of its TMS320C25 DSP, which the T3000's manual
says speaks the PEP of the TrailBlazer, T1000, T2000 and T2500. What it
settles is in [What the T3000's firmware shows](#what-the-t3000s-firmware-shows);
the rest below is still unchecked against a Telebit.

## The ensemble

| | Value | Source |
|---|---|---|
| Sample rate | 8000 Hz (9600 resampled to it) | '227 |
| FFT | 1024 points, 128 ms | '227 |
| Carriers | 511, bins 1 to 511, 7.8125 Hz apart | '227, S71/S73/S76 |
| Guard | the symbol's first 61 samples repeated after it (64 in Telebit's documents) | the T3000, T2000 recordings |
| Epoch | 1085 samples, 135.6 ms, 7.37 baud | the T3000, T2000 recordings |
| Bits a carrier | 0, 2, 4, 5 or 6, Telebit's constellations | '227, Ballard, the T3000 |
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
acknowledged. A side with nothing to send sends one fill packet (a micro
turn, below, once on the final loading), so the other side gets the
line back at once (Adaptive Duplex). If no turn
comes (the far side's lost), the caller takes the line again after
0.6 s, the answerer after 0.9 s, unless the line carries more than its
quiet level (the far side in a turn whose tones were missed).

## Micro turns (D-Modem's own)

Telebit's documents give its micro packets 88.26 baud on a few
carriers; the T3000's DSP has a short epoch of 64 samples and 56 of
guard (120 samples, 66.7 baud) on 32 carriers. D-Modem's micro turn is
20 ms of silence, 24 ms of both tones (`PRE_SHORT`), then four of those
short epochs: one short comb, then three data
epochs of one packet. A short epoch uses every 16th carrier (125 Hz
apart, at odd multiples of 62.5 Hz from 312.5 to 3312.5 Hz, as the
T3000's micro epochs do), 4 times the long epoch's amplitude (16 times
the power, as the long epoch's power is spread over 16 times as many
carriers), so each takes 2 bits more than the final loading gives that
carrier, 6 at most. The short symbol is the first 120 samples of the
1024 point IFFT with only those bins (their period 128, so any 64 of
them a window), so the long comb's channel
applies to it unchanged.

A side sends a micro turn when it has no packet to send again, no
control data and less data than one micro packet holds (about 50
octets at 30 dB). A receiver on the final loading tries every turn as a
micro turn first: it scans 160 samples from the tones' end for the
short comb (the adjacent carriers' phase steps, against the channel,
coherent above 0.8), refines its timing from their mean, takes the
common phase from it and decodes the packet; a bad CRC (a long turn)
goes on to the long comb. A micro turn takes about 90 ms against 330 ms
for a turn of one long packet.

## The pump and the link

The pump (`dp_pep.cpp`) gives PEP the link layer's bits as octets.
When the link frames them (V.42, MNP or start-stop characters, which
the core says with `dp_runtime_set_framed()`), the pump takes them at
the rate the link was given at `CONNECT` (so its bit timers keep time),
up to two long turns ahead, and leaves out its idle: ones after 9 ones
kept (no character or frame has more) and flags after 16 kept (V.42's
answerer needs a few to see one as a character, and asynchronous
characters never repeat one 8 bit pattern three times). The received
octets go to the link at the far side's line rate, not a turn's at
once. A synchronous call's bits are a raw stream and go as they are.

The caller skips V.42's detection phase in PEP and starts LAPM at
once: the ODPs and the answer to them would take two turns each way,
past T400 (750 ms). The answerer keeps its detection phase, which ends
on the caller's flags, or takes MNP's LR.

## Automode and the answer tone

The caller's watch, before PEP's tones, takes the line in 10 ms
blocks: the power at 2100 Hz, at the two tones, and the answer tone's
envelope. Its AM depth over 400 ms (the envelope less its mean, against
it; above 0.1) tells ANSam from a plain answer tone, which V.34's
caller now measures the same way. The answer tone survives ANSam's
phase reversals (gaps of up to 5 blocks). After it, 150 ms of a signal
that is neither PEP's tones nor 2100 Hz is another modulation. The pump
hands the call over with `DPSTAT_CHANGEDP` (V.34 for ANSam and for the
answerer, V.32bis with its automode otherwise, Bell 212A for 2225 Hz),
and the call's statistics are no longer PEP's.

## Statistics

At `CONNECT` and on each block after it the pump fills the call's
statistics: the line rates, the packets' counts, the micro turns, and
for each of the 512 bins the bits each way and the noise measured in
the start-up (`noise_dbm0()`). `ATI11` shows the counts, `ATI12` the
table (Telebit's S70 to S75 would clash with D-Modem's own S70, S71 and
S92, so they are not S-registers).

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
mixed with short ones, so both kinds stay on one grid. D-Modem's long
and micro epochs now have these lengths (1085 and 120; they were 1088
and 90).

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
and what it receives goes nowhere (the T3000 was sold without PEP). The
packet layer needs a T2500's or WorldBlazer's firmware, or a recording
decoded to bits.

## What the T2000 recordings show

Five recordings of a Telebit T2000 (Wikimedia Commons: call set-up,
duplex and simplex data, XMODEM with and without spoofing) were
measured:

- The answerer sends 3.1 s of plain 2100 Hz and 70 ms of silence before
  PEP's tones.
- The epoch is 1024 samples at 8000 Hz plus a guard of about 61 (64 in
  Telebit's documents; the lossy audio makes it rough), the guard a
  copy of the symbol's start after it. The comb (E0) is the same both
  ways, and so are the 9 training epochs.
- The T2000's carriers end at bin 434 (3390.6 Hz).
- A turn starts with one E0 epoch and no tones; on the 2-bit link the
  data is QPSK times E0's phases.
- Long turns are about 1.8 s. An idle line is a 46 ms burst each way
  every 120 ms, about a dozen carriers each; their epoch could not be
  found.

No turn was decoded to bits, so the packets, the scrambler and the
loading record of a real Telebit are still unknown.

## Not done

- S36's lead tone, S120; Telebit's small packets.
- Telebit's compression (S110), UUCP g, Kermit and XMODEM spoofing
  (S111).
- Turbo PEP (the WorldBlazer's, 7 bits a carrier, 23 kbit/s).
- Talking to a real Telebit (its packets are not known).
- The T3000's comb phases, its start-up script, the 4-bit field on the
  timing tones' carriers (above).
