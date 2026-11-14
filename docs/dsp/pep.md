# PEP: Telebit's Packetized Ensemble Protocol

**PEP** was the modem protocol of Telebit. The TrailBlazer had it first,
in 1985. Other modems of that time sent one carrier at the highest
possible speed. PEP sent hundreds of slow carriers at the same time (an
"ensemble", which is now OFDM or DMT). Each carrier sent as many bits as
the line let it at its frequency. On a line with a dip, a tilt or a hum,
only the carriers at those frequencies were lost. On a good local call,
it got about 18000 bit/s on the line, years before V.32bis gave 14400.
It also operated well on the bad long distance lines of that time.

The modems used the line in turns ("Adaptive Duplex"). Each turn had a
few packets with their own error check and retransmission. Thus, a modem
with more data to send got more of the line. UUCP sites and Usenet
preferred it in the late 1980s. Telebit gave a licence for it to almost
no company, and never published it.

The PEP of D-Modem (`dsp/pep.cpp`, the pump `dsp/dp_pep.cpp`) comes from
the data that Telebit published: its patents, the reference manual of the
T2500 and two technical notes of 1990 (refer to
[the technical details](pep-details.md)). Where these documents give a
number, D-Modem uses it. Examples are the 511 carriers, the FFT, the guard
time, the timing tones, the levels, the steps of the start-up and how the
modem shares bits between the carriers. The documents do not give the
layout of the packet, how a turn goes to the other side, or the record
that tells the other side its loading. D-Modem uses its own design for
these items. Thus, two D-Modems use PEP with each other, but a real
TrailBlazer does not understand D-Modem.

Five recordings of calls of a Telebit T2000 confirm the epoch, the comb,
the timing tones and the order of the start-up. They also show the answer
tone before PEP and the short turns of an idle line (refer to
[the technical details](pep-details.md#what-the-t2000-recordings-show)).
But we could not decode a turn of them to bits. Thus, the packets of a
real Telebit are still unknown.

## How to use it

    AT+MS=PEP        (or AT+MS=255, as Telebit's S50=255)

The answerer sends the two timing tones of PEP at the same time. The
caller hears them, and the start-up follows. The start-up takes about
4 seconds. During it, the modems measure the line in the two directions
and select the carriers of each side. The result is

    CONNECT FAST

as a Telebit gives it. With `W2` and the other result settings, the
modem shows the data rate in each direction when the two sides send at
their maximum.

**Other modems.** In automode, a PEP modem calls and answers modems that
are not PEP. To select automode, use `+MS=PEP,1`. Automode is enabled
unless `+MS` disabled it, as Telebit's S50=0 does.

- When it calls, it listens before the tones of PEP come:
  - A V.8 answer tone (ANSam, 2100 Hz with its 15 Hz AM) sends the call
    to V.34 and V.8.
  - A plain 2100 Hz tone, or the signal of a different modulation after
    it, sends the call to V.32bis and its automode (V.32, V.22bis, ...).
  - The 2225 Hz tone of Bell sends the call to Bell 212A.
  - If no PEP tones come in 1.5 s after the end of the answer tone (or in
    3 s without an answer tone), the call also goes to V.32bis.
- When it answers, it sends the tones and comb of PEP. If no PEP caller
  comes in 2 s after them, the call goes to the answerer of V.34 (ANSam,
  then V.8 or the older modulations).

`+MS=PEP,0` is PEP only. A T2000 answers with 3.1 s of the plain
2100 Hz answer tone and 70 ms of silence before the tones of PEP. The
caller of D-Modem waits through that. Its answerer does the same with
`DSP_PEP_ANS=1` in the environment (for tests with a recording).

**Short turns.** A side with little data to send (an acknowledgement, a
typed key, or no data) sends a micro turn. A micro turn is 24 ms of the
tones and four short epochs of 11 ms, about 90 ms in total. A turn of one
long packet is 330 ms. Thus, typed characters come back quickly, and the
side that sends a file gets almost all of the line.

**Error correction and compression.** The packets of PEP have no errors.
V.42 operates on them as on all other modulations, with its compression:
V.44, V.42bis or zstd, as `+DS`, `+DS44` and `+DSZ` allow. (With a modem
that has only MNP and MNP 5, it uses those.) In PEP, the calling modem
skips the detection phase of V.42 and starts LAPM immediately. The
reason is that the turns of PEP make the round trip longer than the
750 ms of the detection phase. The answerer still accepts MNP. The link
sends idle data between its frames (flags, marks). PEP does not put this
idle data in its packets. Thus, an idle link sends only micro turns. The
compression (S110) and spoofing (S111) of Telebit are not available.

**Statistics.** `ATI11` adds these items:

- The line rate in each direction.
- The packets received, and the packets received with errors.
- The micro turns sent and received.

`ATI12` gives a list of all carriers: the frequency, the bits in each
direction and the noise measured on it in dBm0 (Telebit's S71, S73 and
S72). After a call that was not PEP, `ATI12` shows `No PEP Call`.

## Speeds in simulation

Two D-Modems back to back on a simulated telephone line (`pep_test`: a
band of 300 to 3400 Hz, a delay in each direction, white noise against a
-9 dBm0 signal):

| Line's signal to noise | Rate on the line each way |
|---|---|
| 40 dB | 20500 bit/s |
| 30 dB | 18600 bit/s |
| 24 dB | 12300 bit/s |
| 19 dB | 7300 bit/s |

That is the rate while one side has the line. When the two sides send at
their maximum, each side gets a little less than half of it (9400 bit/s
in each direction at 40 dB). The cause is the overhead of the turns.
When only one side sends, it gets most of the line (the turns of the
other side are micro turns).

All calls had no errors. Calls with these conditions also had no errors:

- A clock difference of 300 ppm.
- Gains of 2 and 0.05.
- Echoes.
- A delay of 250 ms.
- A sample rate of 8000 Hz.
- A dropout of half a second (the modem sent again the packets that it
  lost in it).

Below about 17 dB, the start-up fails. The reason is that the link must
have carriers that send 2 bits.
