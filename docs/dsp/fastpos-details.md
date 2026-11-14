# Point of sale: technical details

This page gives the technical details for [Point of sale](fastpos.md).
Fast connect is in `dsp/dp_v22.cpp` (`V22Pump`, its `fast` and
`flags_hs`). V.29 FastPOS is `dsp/dp_v29fp.cpp` (`V29fpPump`, `DP_V29FP`,
229). Its calling tone is in `dsp/dp_callprog.cpp`.

## Sources

No Recommendation gives a description of them. "V.22 FastConnect" and
"V.29 FastPOS" come from Hypercom. Hosts (NACs) are not all the same, and
some are different intentionally. These are the published sources:

- Silicon Labs, AN93 (the designer's guide for the ISOmodem), Appendix D
  and sections 3.1.4 to 3.1.7. It gives these data:
  - The registers U7A (bit 0 the fast connect handshake timing, bit 1
    HDLC flags for the marks in the handshake) and U80 (the answer tone
    length of the host, or bit 15: no answer tone).
  - The typical connect times (V.22 3.0 s, V.22bis 5.0 s, V.22
    FastConnect 0.6 s, V.29 FastPOS 0.5 s, without the dial).
  - "a short answer tone of at least 300 ms" (2100 Hz, 2225 Hz or the
    unscrambled ones of V.22).
  - The 980 Hz calling tone of FastPOS and its 2225 (or 2100) Hz answer
    tone, half duplex with RTS as push-to-talk, a long train and a short
    one, and its SDLC trace (SNRM, UA, RR through V.80).
- 3am Systems, "Modem quirks found on POS networks" (2014, 2018). It
  gives these data: the answer tone can decrease to 400 ms. FastConnect
  "compresses the synchronization phases of the handshake". Terminals
  possibly must send HDLC flags for the scrambled marks.

## Fast connect (S49)

S49 bit 0 enables it. Bit 1 adds the flags. S50 is the answer tone in
10 ms (0: `FAST_ANS_MS`, 400 ms). Without S49, nothing changes.

| | Answerer | Caller |
|---|---|---|
| tone | 200 ms of silence, the answer tone for S50 (2100 Hz; Bell 212A 2225 Hz), 75 ms of silence, the unscrambled ones | call progress hears 300 ms of the tone and gives control to the pump |
| V.22, Bell 212A | on 50 ms of the scrambled ones or flags of the caller, in the tone, the gap or the unscrambled ones: its own scrambled ones, and data 150 ms later | its scrambled ones (or flags) immediately; data 150 ms after 50 ms of the scrambled ones of the answerer |
| V.22bis | as in 6.3.1 of V.22bis, S1 on 40 ms of the S1 of the caller; 250 ms of scrambled ones at 1200 bit/s (500 usually), then 2400 | S1 after 50 ms of the unscrambled ones (usually 155 and then 456 ms of silence); 2400 bit/s 350 ms after the S1 of the answerer (600) |

The constants are `FAST_SB1_MS`, `FAST_DATA_MS`, `FAST_USB1_MS`,
`FAST_BIS_SB1_MS` and `FAST_ANS_MS`.

These items had to be correct:

- **Unscrambled ones descramble as ones.** The descrambler of V.22
  changes a steady 11 dibit back into ones. Thus, 50 ms of the
  unscrambled ones of the answerer look like its scrambled ones.
  `remote_sb1()` accepts the scrambled ones (or flags) only while the raw
  phase steps are not those of the unscrambled ones (`run_usb1`). A
  V.22bis caller in `C_SB1_1200` keeps the usual 270 ms before it
  identifies the answerer as a V.22 answerer. The reason is that in
  V.22bis, the unscrambled ones of the answerer go directly into its S1.
- **Flags.** `TX_FLAGS` sends 01111110 through the scrambler at 1200
  bit/s. The receiver counts flags on its 1200 bit/s path (`rx_byte12`,
  `run_flags12`), four symbols for each flag. If the far end of a side
  sends flags, the side accepts them as it accepts scrambled ones. Its
  own S49 has no effect on this.
- After CONNECT, the far end can continue to send its handshake for a
  maximum of 150 ms. For an asynchronous DTE, scrambled ones are marks.
  For a synchronous DTE (the framed sub-mode of V.80), flags are the
  idle of HDLC. But for an asynchronous DTE, flags are `~` characters.
  Thus, S49=3 is for synchronous DTEs.

### Tests

The `Pos/FastConnect` test of `pump_test` runs each modulation back to
back with S49 set (and S49=3, S50=70). It uses these conditions:

- Clean lines.
- 10 and 15 dB of SNR.
- 8000 Hz.
- A gain of 0.05 or 2, and clock offsets of 300 ppm.
- The echo of each modem 6 dB down.

Each modem must connect with a bit error rate of 1e-3 or less, in 1 s
after the start of the answerer (V.22bis 1.8 s). They connect at 0.8 to
0.95 s (V.22bis 1.36 to 1.5 s). In these tests, the normal handshakes
take 5.3 s (V.22) and 5.1 s (V.22bis).

The `FastConnect` cases of `dmodem_test` run the full modem (`\N0`,
`S92=0`) with lines of text in each direction. The modem is online
0.88 s after the dial (V.22bis 1.46 s). For the normal V.22, this time
is 5.64 s.

## V.29 FastPOS (229)

### Start

The call progress of the caller sends a steady 980 Hz tone (`fastpos`,
from S32 = 229) in place of the calling tone of V.25. It gives control to
the pump on 300 ms of the answer tone, as for all other modulations. The
pump gives CONNECT in its first sample (`FP_C_START`). The answerer sends
200 ms of silence, 2225 Hz for S50 (400 ms by default) and 75 ms of
silence. Then it gives CONNECT. The answerer does not wait for the 980 Hz
tone, because +MS already selected the modulation.

The rate is the highest rate of +MS: 9600, 7200 or 4800 bit/s. V.29 has
no rate signal, and the receiver operates at the rate of its own side.
Thus, the two ends must have the same rate.

### Turns

Each side is in one of three turns:

- `FP_LISTEN`: the receiver (the `FaxRx` of fax, for V.29) operates on
  the line.
  - A power meter (10 ms) with a noise floor tells if there is a signal.
    The noise floor goes down immediately, and goes up by doubling in
    about 0.7 s. The signal is on above 6 times the floor (and 80 in
    amplitude). It is off below 3 times (or 60).
  - The carrier detector of the receiver cannot do this on a noisy line.
    Its threshold is absolute, and line noise 25 dB below the signal
    keeps it on.
  - When the signal stops, the receiver starts again for the next burst.
  - When the line is quiet for 25 ms, the pump takes the bits of the
    link at the rate of the line and examines them (`IdleWatch`). Marks
    and HDLC flags (a 0 that is the first or last bit of a flag) are
    idle. The pump discards them, but keeps the last 32 in a ring.
  - The first other 0 is data, and the burst starts. (This 0 shows 7
    bits late, when the pump knows that it does not start a flag.)
- `FP_SEND`: the `FaxTx` of fax sends the training of V.29 (20 ms of
  silence, 128 symbols of ABAB, 384 of CDCD, 48 of scrambled ones: about
  250 ms). Then it sends the 32 bits that the pump kept. Then it sends the
  bits of the link as they come, until 32 bits in a row are idle
  (`FAX_BIT_END`). Then it sends its shutdown ones and 20 ms of silence.
  The receiver does not operate.
- `FP_ECHO`: 30 ms for the echo of our burst to decrease. Then the
  receiver starts again and the turn is `FP_LISTEN`.

The receiver sends the data bits of the far end to the link until 64
ones come in a row. A burst ends with 32 idle bits and the shutdown ones
of V.29. If the receiver did not stop, the signal that follows while the
carrier decreases goes to the DTE as noise. In a burst, the link is never
idle for that long (the burst ends first).

A collision (the two sides start at the same time) loses the two bursts.
Then each side listens again, and the protocol of the DTEs (the poll and
response of SDLC) tries again. In data, `carrier_tick(1)` keeps the call
on, whatever the line does. A half duplex line is frequently quiet.

### Not done

- The short training of Hypercom. AN93 tells that the modem selects "V29
  Long Train" or "V29 Short Train" automatically. Nobody published the
  short one. Each burst of D-Modem has the long training.
- Fallback to V.22bis when the host answers with it.
- RTS as push-to-talk. The turn changes only on the data.

### Tests

`pos_test` makes a transaction between a terminal and its host
(`libdmodem`, back to back at 8000 Hz). With SDLC, it uses the framed
sub-mode of V.80 with FCS-16 at the two ends. Six SDLC frames go in turn:

1. The SNRM of the host.
2. The UA of the terminal.
3. The information frame of the host, 122 octets.
4. The information frame of the terminal, 202 octets.
5. The DISC of the host.
6. The UA of the terminal.

Each frame goes when the frame before it is fully received. The test
does a check of each frame and of the time from the dial to the last
frame:

| Case | Online | Transaction |
|---|---|---|
| V.29 FastPOS | 0.68 s | 3.46 s |
| V.29 FastPOS, 25 dB SNR | 0.68 s | 3.28 s |
| V.29 FastPOS at 7200 | 0.68 s | 3.58 s |
| V.22, fast connect with flags (also at 12 dB SNR) | 0.90 s | 3.88 s |
| V.22, fast connect | 0.88 s | 3.86 s |
| Bell 212A, fast connect with flags | 0.90 s | 3.88 s |
| V.22, normal | 5.64 s | 8.62 s |

With plain characters (only `\N0`), the same test does the exchange of
VISA II (ENQ; STX, a request of 66 characters, ETX, LRC; ACK and an
answer of 25 characters; ACK; EOT). The results are: V.29 FastPOS
2.88 s, V.22 fast connect 2.26 s, V.22 7.02 s.

The test sends each frame 20 ms after the frame before it arrived (its
frames of samples). A turn of FastPOS takes about 300 ms for a short
frame. Almost all of this time is the training.
