# PEP: Telebit's Packetized Ensemble Protocol

**PEP** was Telebit's modem protocol, first sold in 1985 in the
TrailBlazer. Where other modems of its day sent one carrier as fast as
it would go, PEP sent hundreds of slow ones at once (an "ensemble",
what is now called OFDM or DMT), each carrying as many bits as the line
allowed at its frequency. A line with a dip, a tilt or a hum just lost
the carriers there. On a good local call it reached about 18000 bit/s
on the line, years before V.32bis gave 14400, and it shrugged off the
poor long distance lines of the time. The modems took turns on the
line ("Adaptive Duplex"), each turn a few packets with their own error
check and retransmission, so a modem with more to send got more of the
line. It was the favourite of UUCP sites and Usenet in the late 1980s.
Telebit licensed it to almost no one and never published it.

D-Modem's PEP (`dsp/pep.cpp`, the pump `dsp/dp_pep.cpp`) is built from
what Telebit did publish: its patents, the T2500's reference manual and
two technical notes of 1990 (see [the technical details](pep-details.md)).
It uses their numbers wherever they give one: the 511 carriers, the
FFT, the guard time, the timing tones, the levels, the steps of the
start-up and the way bits are shared between the carriers. What they
do not say (the packet's layout, how a turn is handed over, the record
that tells the other side its loading) is D-Modem's own. So two
D-Modems talk PEP to each other, but a real TrailBlazer would not
understand D-Modem. Five recordings of a Telebit T2000's calls confirm
the epoch, the comb, the timing tones and the start-up's order, and
show the answer tone before PEP and the short turns of an idle line
(see [the technical details](pep-details.md#what-the-t2000-recordings-show)),
but no turn of them could be decoded to bits. A T3000's firmware has
since given Telebit's constellations and the carriers of its micro
packets, which D-Modem now uses, and its start-up's steps (see
[what the firmware shows](pep-details.md#what-the-t3000s-firmware-shows)),
but not yet its packets, so the packets of a real Telebit are still
unknown.

## Using it

    AT+MS=PEP        (or AT+MS=255, as Telebit's S50=255)

The answerer sends PEP's two timing tones at once, the caller hears
them and the start-up follows (about 4 seconds: the line measured both
ways and each side's carriers chosen). The result is

    CONNECT FAST

as a Telebit gives it. With `W2` and the other result settings the rate
shown is the data's rate each way when both sides send all they can.

**Other modems.** In automode (`+MS=PEP,1`; automode is on unless
`+MS` turned it off, as Telebit's S50=0), a PEP modem calls and answers
modems that are not PEP:

- Calling, it listens before PEP's tones come. A V.8 answer tone
  (ANSam, 2100 Hz with its 15 Hz AM) hands the call to V.34 and V.8; a
  plain 2100 Hz tone, or another modulation's signal after it, to
  V.32bis and its automode (V.32, V.22bis, ...); Bell's 2225 Hz to Bell
  212A. No PEP tones within 1.5 s of the answer tone's end (or 3 s
  without one) also hands it to V.32bis.
- Answering, it sends PEP's tones and comb; no PEP caller within 2 s
  of them hands the call to V.34's answerer (ANSam, then V.8 or the
  older modulations).

`+MS=PEP,0` is PEP alone. A T2000 answers with 3.1 s of the plain
2100 Hz answer tone and 70 ms of silence before PEP's tones; D-Modem's
caller waits through that, and its answerer does the same with
`DSP_PEP_ANS=1` in the environment (for testing against a recording).

**Short turns.** A side with little to send (an acknowledgement, a key
typed, nothing at all) sends a micro turn: 24 ms of the tones and four
short epochs of 11 ms, about 90 ms in all against 330 ms for a turn of
one long packet. So typing echoes quickly and the side that is sending
a file gets almost all of the line.

**Error correction and compression.** PEP's own packets are error free,
and V.42 runs over them as on any other modulation, with its
compression: V.44, V.42bis or zstd as `+DS`, `+DS44` and `+DSZ` allow
(or MNP and MNP 5 with a modem that has only those). The calling
modem skips V.42's detection phase in PEP (PEP's turns make the round
trip longer than its 750 ms) and starts LAPM at once; the answerer
still takes MNP. The idle the link sends between its frames (flags,
marks) is left out of PEP's packets, so an idle link is micro turns.
Telebit's own compression (S110) and spoofing (S111) are not there.

**Statistics.** `ATI11` adds the line rate each way, the packets taken
and received bad, and the micro turns sent and taken. `ATI12` lists
each carrier: its frequency, its bits each way and the noise measured
on it in dBm0 (Telebit's S71, S73 and S72). After a call that was not
PEP, `ATI12` says `No PEP Call`.

## Speeds in simulation

Two D-Modems back to back on a simulated phone line (`pep_test`: a band
of 300 to 3400 Hz, a delay each way, white noise against a -9 dBm0
signal):

| Line's signal to noise | Rate on the line each way |
|---|---|
| 40 dB | 20500 bit/s |
| 30 dB | 18600 bit/s |
| 24 dB | 12300 bit/s |
| 19 dB | 7300 bit/s |

That is the rate while a side has the line. When both sides send all
they can, each gets a little under half of it (9400 bit/s each way at
40 dB), because of the turns' overhead; when only one side sends, it
gets most of the line (the other side's turns are micro turns). Every
call ran without an error, as did calls with a 300 ppm clock
difference, gains of 2 and 0.05, echoes, a 250 ms delay, an 8000 Hz
sample rate and half a second of dropout (the packets lost in it sent
again). Below about 17 dB the start-up fails: the link
needs carriers that take 2 bits.
