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
understand D-Modem: no Telebit firmware or recording of a PEP call has
been found to check against.

## Using it

    AT+MS=PEP        (or AT+MS=255, as Telebit's S50=255)

on both modems. The answerer sends PEP's two timing tones at once (it
does not send the 2100 Hz answer tone first), the caller hears them and
the start-up follows (about 5 seconds: the line measured both ways and
each side's carriers chosen). The result is

    CONNECT FAST

as a Telebit gives it. With `W2` and the other result settings the rate
shown is the data's rate each way when both sides send all they can.
PEP's own packets are error free, and V.42 (or MNP) runs over them as
on any other modulation. V.8 has no code for PEP, so it is used only
when chosen with `AT+MS`; there is no fallback to another modulation
yet if the other modem does not answer in PEP.

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
gets most of the line. Every call ran without an error, as did calls
with a 300 ppm clock difference, gains of 2 and 0.05, echoes, a 250 ms
delay, an 8000 Hz sample rate and half a second of dropout (the packets
lost in it sent again). Below about 17 dB the start-up fails: the link
needs carriers that take 2 bits.
