# Point of sale: technical details

This is the technical companion to [Point of sale](fastpos.md). Fast
connect is in `dsp/dp_v22.cpp` (`V22Pump`, its `fast` and `flags_hs`);
V.29 FastPOS is `dsp/dp_v29fp.cpp` (`V29fpPump`, `DP_V29FP`, 229), with
its calling tone in `dsp/dp_callprog.cpp`.

## Sources

No Recommendation describes either: "V.22 FastConnect" and "V.29
FastPOS" are Hypercom's, and hosts (NACs) vary, some of them on purpose.
What is published:

- Silicon Labs, AN93 (the ISOmodem designer's guide), Appendix D and
  sections 3.1.4 to 3.1.7: the registers U7A (bit 0 the fast connect
  handshake timing, bit 1 HDLC flags for the marks in the handshake),
  U80 (the host's answer tone length, or bit 15: none at all), the
  typical connect times (V.22 3.0 s, V.22bis 5.0 s, V.22 FastConnect
  0.6 s, V.29 FastPOS 0.5 s, not counting the dial), "a short answer
  tone of at least 300 ms" (2100 Hz, 2225 Hz or V.22's unscrambled
  ones), FastPOS's 980 Hz calling tone and its 2225 (or 2100) Hz answer
  tone, half duplex with RTS as push-to-talk, a long train and a short
  one, and its SDLC trace (SNRM, UA, RR through V.80).
- 3am Systems, "Modem quirks found on POS networks" (2014, 2018): the
  answer tone can be cut to 400 ms; FastConnect "compresses the
  synchronization phases of the handshake"; terminals may need to send
  HDLC flags for the scrambled marks.

## Fast connect (S49)

S49 bit 0 turns it on, bit 1 adds the flags; S50 is the answer tone in
10 ms (0: `FAST_ANS_MS`, 400 ms). Without S49 nothing changes.

| | Answerer | Caller |
|---|---|---|
| tone | 200 ms of silence, the answer tone for S50 (2100 Hz; Bell 212A 2225 Hz), 75 ms of silence, the unscrambled ones | call progress hears 300 ms of the tone and hands over |
| V.22, Bell 212A | on 50 ms of the caller's scrambled ones or flags, in the tone, the gap or the unscrambled ones: its own, data 150 ms later | its scrambled ones (or flags) at once; data 150 ms after 50 ms of the answerer's |
| V.22bis | as V.22bis's 6.3.1, S1 on 40 ms of the caller's; 250 ms of scrambled ones at 1200 bit/s (500 normally), then 2400 | S1 after 50 ms of the unscrambled ones (155 and then 456 ms of silence normally); 2400 bit/s 350 ms after the answerer's S1 (600) |

The constants are `FAST_SB1_MS`, `FAST_DATA_MS`, `FAST_USB1_MS`,
`FAST_BIS_SB1_MS` and `FAST_ANS_MS`.

Things that had to be right:

- **Unscrambled ones descramble as ones.** V.22's descrambler turns a
  steady 11 dibit back into ones, so 50 ms of the answerer's unscrambled
  ones look like its scrambled ones. `remote_sb1()` takes the scrambled
  ones (or flags) only while the raw phase steps are not those of the
  unscrambled ones (`run_usb1`). A V.22bis caller in `C_SB1_1200` keeps
  the normal 270 ms before it takes the answerer for a V.22 one, since
  the answerer's unscrambled ones run straight into its S1 there.
- **Flags.** `TX_FLAGS` sends 01111110 through the scrambler at 1200
  bit/s. The receiver counts flags on its 1200 bit/s path (`rx_byte12`,
  `run_flags12`), four symbols a flag. A side whose far end sends flags
  takes them as it takes scrambled ones, whatever its own S49 says.
- After CONNECT the far end may still send its handshake for up to
  150 ms. Scrambled ones are marks to an asynchronous DTE; flags are
  HDLC's idle to a synchronous one (V.80's framed sub-mode) but `~`
  characters to an asynchronous one, so S49=3 is for synchronous DTEs.

### Tests

`pump_test`'s `Pos/FastConnect` runs each modulation back to back with
S49 set (and S49=3, S50=70): on clean lines, at 10 and 15 dB of SNR, at
8000 Hz, with a gain of 0.05 or 2 and 300 ppm clock offsets, and with
each modem's echo 6 dB down. Each must connect with a bit error rate of
1e-3 at most within 1 s of the answerer's start (V.22bis 1.8 s); they
connect at 0.8 to 0.95 s (V.22bis 1.36 to 1.5 s). The normal handshakes
take 5.3 s (V.22) and 5.1 s (V.22bis) there.

`dmodem_test`'s `FastConnect` cases run the whole modem (`\N0`, `S92=0`)
with lines of text each way: online 0.88 s after the dial (V.22bis 1.46
s), against 5.64 s for the normal V.22.

## V.29 FastPOS (229)

### Start

The caller's call progress sends a steady 980 Hz tone (`fastpos`, from
S32 = 229) instead of V.25's calling tone, and hands over on 300 ms of
the answer tone as for any other modulation. The pump reports CONNECT
in its first sample (`FP_C_START`). The answerer sends 200 ms of
silence, 2225 Hz for S50 (400 ms by default), 75 ms of silence, and
reports CONNECT. The answerer does not wait for the 980 Hz tone: +MS
chose the modulation already.

The rate is +MS's highest: 9600, 7200 or 4800 bit/s. V.29 has no rate
signal and the receiver runs at the side's own rate, so both ends need
the same.

### Turns

Each side is in one of three turns:

- `FP_LISTEN`: the receiver (fax's `FaxRx`, for V.29) runs on the line.
  A power meter (10 ms) with a noise floor (down at once, up doubling in
  about 0.7 s) says whether there is a signal: on above 6 times the
  floor (and 80 in amplitude), off below 3 times (or 60). The
  receiver's own carrier detector cannot do it on a noisy line: its
  threshold is absolute, and line noise 25 dB below the signal keeps
  it on. When the signal ends the receiver starts afresh for the next
  burst.
  Once the line has been quiet for 25 ms the pump takes the link's bits
  at the line's rate and looks at them (`IdleWatch`): marks, and HDLC
  flags (a 0 that is a flag's first or last bit), are idle and dropped,
  but kept in a ring of the last 32. The first other 0 (it shows 7
  bits late, once it is known not to begin a flag) is data: the burst
  starts.
- `FP_SEND`: fax's `FaxTx` sends V.29's training (20 ms of silence, 128
  symbols of ABAB, 384 of CDCD, 48 of scrambled ones: some 250 ms), then
  the 32 bits kept, then the link's bits as they come, until 32 in a
  row are idle (`FAX_BIT_END`), then its shutdown ones and 20 ms of
  silence. The receiver is not run.
- `FP_ECHO`: 30 ms for the echo of our burst to die away; then the
  receiver starts afresh and the turn is `FP_LISTEN`.

The receiver passes the far end's data bits to the link until 64 ones
come in a row: a burst ends with 32 idle bits and V.29's shutdown ones,
and what follows as the carrier dies would otherwise reach the DTE as
noise. Within a burst the link never idles that long (the burst would
have ended first).

A collision (both sides starting at once) loses both bursts: each side
then listens again, and the DTEs' protocol (SDLC's poll and response)
retries. In data `carrier_tick(1)` keeps the call up whatever the line
does: a half duplex line is often quiet.

### Not done

- Hypercom's short training. AN93 says the modem chooses "V29 Long
  Train" or "V29 Short Train" by itself; the short one is not
  published. Each D-Modem burst has the long training.
- Falling back to V.22bis when the host answers with it.
- RTS as push-to-talk: the turn passes on the data alone.

### Tests

`pos_test` makes a transaction between a terminal and its host
(`libdmodem`, back to back at 8000 Hz). With SDLC: V.80's framed sub-mode with
FCS-16 at both, six SDLC frames in turn (the host's SNRM, the
terminal's UA, the host's information frame of 122 octets, the
terminal's of 202, the host's DISC, the terminal's UA), each sent when
the one before has come whole. It checks every frame and the time from
the dial to the last one:

| Case | Online | Transaction |
|---|---|---|
| V.29 FastPOS | 0.68 s | 3.46 s |
| V.29 FastPOS, 25 dB SNR | 0.68 s | 3.28 s |
| V.29 FastPOS at 7200 | 0.68 s | 3.58 s |
| V.22, fast connect with flags (also at 12 dB SNR) | 0.90 s | 3.88 s |
| V.22, fast connect | 0.88 s | 3.86 s |
| Bell 212A, fast connect with flags | 0.90 s | 3.88 s |
| V.22, normal | 5.64 s | 8.62 s |

With plain characters (`\N0` alone) the same test runs VISA II's
exchange (ENQ; STX, a 66 character request, ETX, LRC; ACK and a 25
character answer; ACK; EOT): V.29 FastPOS 2.88 s, V.22 fast connect
2.26 s, V.22 7.02 s.

The test sends each frame 20 ms after the one before it arrived (its
frames of samples). A turn of FastPOS takes about 300 ms for a short
frame, nearly all of it the training.
