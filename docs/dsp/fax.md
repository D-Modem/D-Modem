# Fax

## Fax (class 1)

`fax_class1.cpp` implements the T.31 class 1 commands on top of the
modulations in `fax_modems.cpp`:

| Command | |
|---|---|
| `+FTH=3` / `+FRH=3` | V.21 channel 2 HDLC frames: flags, bit stuffing, CRC-16, `<DLE>` shielding, `OK`/`ERROR` per received frame |
| `+FTM=` / `+FRM=` | 24, 48 (V.27ter), 72, 96 (V.29), 73, 74, 97, 98, 121, 122, 145, 146 (V.17, long and short train) |
| `+FTS=` / `+FRS=` | Silence for / wait for silence of n * 10 ms |

The answering side sends CED (2100 Hz for 3 s) and then starts `+FTH=3` by
itself; the calling side sends CNG while it waits for an answer and starts
`+FRH=3` when the answer tone is heard. Any character from the DTE stops
`+FRH`/`+FRM` once they reported `CONNECT`.

The receivers find the training by matching the received phase steps
against the known sequence. The V.29 and V.27ter training sequences repeat
every 127 symbols, so the end of the alternations before them tells which
repetition is being received. A V.17 short train continues from the
equalizer of the last long train at the same rate. After a short train
the original library and spandsp go on with the scrambled ones (segment
4) at once, without the 64 symbol bridge of a long train; so does this
transmitter, with the trellis encoder in the state the original library
starts segment 4 from (its receiver expects it). Its segment 4 is 240
symbols (the original library sends 144) and 80 symbols of ones follow
the data: the original receiver, depending on where its 20 ms frames
fall, starts the data before it has converged and ends it early, and
the ones take both. The receiver takes a short train with or without the
bridge: the first symbols after the training tell which. During a bridge
it decides on the nearest training point instead of the known sequence:
the Agere modem's bridge is not that sequence, and following the wrong
points wound the carrier loop up to a frequency offset of some 3 Hz that
spoiled the first seconds of the data. At 12000 bit/s after a short
train the original receiver decides an outer corner of the constellation
now and then as the point 8 away in its subset (a coordinate of 7 comes
out as -1): one wrong bit on the line, three after the descrambler. Our
signal is the same every time; whether and where it happens depends
only on where the receiver's 20 ms frames fall on the long training and
on the short one, and it does in three of the four alignments, about
once a page. The data 1.4 dB down clears all four (0.5 dB down it loses
the TCF instead), and TEP before the training (the original library
sends 200 ms of it) two of the three, so the transmitter keeps V.17's
levels and no TEP. V.27ter, too, leads the data
with 100 ms of ones (the original library sends some 60 ms): with the 8
symbols of the standard its receiver lost the start of a page now and
then. In data mode the V.17 receiver follows a clear
error in the data's power (6 % or more, over some 500 symbols): the
original library's constellation comes about 1.4 dB below its training.

`ctest -L spandsp` (with libspandsp-dev) puts spandsp's V.17 transmitter
and T.4 encoder, as Asterisk's `SendFAX` uses them, into these receivers.

## Fax machine (T.30)

`t30.cpp` is a group 3 fax machine on these modulations (libdmodem's fax,
the modules' fax applications): the calling machine sends a document,
the answering one receives it. With error correction mode (T.30 Annex A,
T.4 Annex A), which both offer by default (DIS and DCS bit 27, frames of
256 octets), the page goes as its MH coding in blocks of up to 256 HDLC
frames (FCD) on the fast modulation, three RCP after each, then PPS on
V.21 with the page and block counters; the receiver answers MCF or PPR
with a map of the frames to send again. After four PPRs for a block the
sender asks CTC (CTR) and goes on; it answers RNR with RR, and the
receiver its RR, CTC and EOR as T.30 says. The receiver ends a block at
its RCP: the V.17 short training of the next block starts from the
receiver's state at the end of this one, and the V.21 PPS after it would
spoil that.

Without ECM the receiver answers each post-page command (MPS, EOM, EOP)
with MCF or RTN. When the sender misses the MCF it sends the command
again after T4 (3 s); if no page's training came since (the fast
receiver never found one) that is a repeat, and it gets MCF again, as
spandsp's receiver does. RTN there would have the sender send the page
the receiver already has once more.

`fax_test`'s `T30Pair` cases put two of them back to back, at each rate,
with ECM, and with ECM on a noisy line (at 19 dB ECM sends a few frames
again and the page comes right; without it two thousand rows come
damaged); `spandsp_test`'s `T30Spandsp` cases (label `spandsp`) run one
against spandsp's fax machine both ways, with and without ECM, and with
line hits. The `T30Lost` cases lose the receiver's MCF to an EOM, an MPS
and the last page's EOP. `DSP_T30_LOSE=<fcf in hex>[:<n>]` sends the nth
message (the first by default) whose last frame is that type as silence,
lost on the line, its timing kept (`8c` is MCF).
