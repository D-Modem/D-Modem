# MNP 6

MNP 6 adds two features for the 9600 bit/s modems of the late 1980s:
Universal Link Negotiation (ULN) and statistical duplexing. Both
features are in the data pump layer and in the link layer. No
specification is public, and we have no firmware that has MNP 6.

All the statements on this page come from secondary sources. We did
not examine them in firmware or on a line.

## Universal Link Negotiation

Before ULN, a fast modem tried each modulation from the fastest to the
slowest. With V.32 and later, this could take more than 10 seconds
(secondary source: Wikipedia).

ULN does the negotiation at one speed:

1. The two modems always connect first at 2400 bit/s (V.22bis), with no
   error correction.
2. Each modem sends an identification string. The string tells the
   modulations that the modem has.
3. The two modems select the fastest modulation that both have.
4. The caller starts the call again in that modulation.

ULN does not connect at 1200 bit/s. Thus a modem with ULN does not
connect to a 1200 bit/s modem in this way.

V.8 (1994) and V.8 bis later gave the same function in a standard
form, with V.21 signals in place of a V.22bis connection.

## Statistical duplexing

The 9600 bit/s modems of Microcom used V.29, which is a one-way
(half duplex) modulation on a 2-wire line. Statistical duplexing makes
V.29 look like a two-way link:

- The modem with more data to send gets the line for more time.
- The modems change the direction of the line as the data flow
  changes.

Thus one direction can get nearly all the bandwidth. Wikipedia gives
"up to 19,200 bit/s" in one direction (secondary source). We do not know
the conditions of this figure.

Other protocols of the same period did a similar thing: USRobotics HST
(a fast channel and a slow back channel), Telebit PEP, the Hayes Express
96 and the CompuCom SpeedModem.

## D-Modem

D-Modem does not support MNP 6.

D-Modem has some of the parts that MNP 6 needs:

- A V.29 data pump with half duplex operation (the FastPOS mode,
  `AT+MS=V29FP`, see [FastPOS](../dsp/fastpos.md)).
- V.22bis, for the first connection of ULN.
- The HST code changes the speed in the middle of a call, and keeps the
  link (see [HST](../dsp/hst.md)). This is the same type of change that
  ULN does.

## What is necessary to support it

1. Get the firmware of a 9600 bit/s Microcom modem with MNP 6.
2. Find the ULN identification string and the time of the restart.
3. Find the turnaround rules of statistical duplexing: who asks for the
   line, and when.
4. Add a V.29 half duplex mode for dial-up calls, with the turnaround.
