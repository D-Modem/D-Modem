# Point of sale: fast connect and V.29 FastPOS

Card terminals, vending machines and parking meters that dial their
bank's host send only a few hundred bytes a call: a request, an answer,
an acknowledgement. Most of a normal modem call's time goes on the
start-up, not the data, so the terminals and their hosts use shorter
start-ups of their own:

- **Fast connect** cuts V.22's (1200 bit/s), Bell 212A's and V.22bis's
  (2400 bit/s) handshake from about 3 to 5 seconds to under 1.
- **V.29 FastPOS** runs V.29, the 9600 bit/s fax modem, half duplex on
  a dial-up line: one side sends at a time, and each turn starts with
  V.29's short training.

Neither is an ITU Recommendation. Both are Hypercom's (now Verifone's)
and became de facto standards; hosts vary in their timing. D-Modem
follows what is published about them, chiefly Silicon Labs' AN93
(Appendix D, "EPOS applications"). Two D-Modems connect with each
other with them; neither has been tried against a real terminal or
host yet.

For the full technical description, see
[the technical details](fastpos-details.md).

## Fast connect

    ATS49=1          fast connect (V.22, V.22bis, Bell 212A)
    ATS49=3          the same, with HDLC flags in the handshake
    ATS50=40         the answer tone's length, in 10 ms (0: 400 ms)

with the usual `AT+MS=22`, `AT+MS=122` or `AT+MS=212`. Both ends need
it. The answering modem sends a short answer tone, the calling modem
answers it at once, and each starts the data 150 ms after it has heard
50 ms of the other. `S49=3` sends HDLC flags in place of the scrambled
ones that the handshake normally has, which some hosts want; D-Modem
takes either from the far end.

| | Online after the answer |
|---|---|
| V.22, normal | 5.6 s |
| V.22 or Bell 212A, fast connect | 0.9 s |
| V.22bis, fast connect | 1.4 s |

(In simulation, two D-Modems, `S92=0`; the normal V.22 includes its
3.3 s answer tone.)

## V.29 FastPOS

    AT+MS=V29FP      (or AT+MS=229, AT+MS=V29FASTPOS)

Both ends need it. The calling modem sends a steady 980 Hz tone while
it waits, the answering modem answers with a 2225 Hz tone (S50 sets its
length), and both say CONNECT. Then whichever side's computer has data
sends it, and the other listens; the turn passes when the data stops.
`AT+MS=V29FP,0,4800,7200` runs it at 7200 bit/s; V.29 has no way to
agree a speed, so both ends need the same setting.

## The data: SDLC frames

Payment terminals frame their data as SDLC (HDLC) frames: the host
polls, the terminal answers. With D-Modem the computer at each end does
this through V.80's synchronous access mode, which carries whole frames
with their checksum:

    AT\N0+ES=6,,8+ESA=0,0,0,,1

then `<EM><flag>` (bytes 19h B1h) to start the framed mode, and each
frame's bytes followed by `<EM><flag>`. A whole transaction of six
frames (the host's SNRM, the terminal's UA, a request and its answer,
DISC and UA) takes, in simulation from the dial:

| | Transaction |
|---|---|
| V.22, normal | 8.6 s |
| V.22, fast connect | 3.9 s |
| V.29 FastPOS | 3.5 s |

Plain characters (`AT\N0` without `+ES`) go through both as well, as
the VISA II exchange of older terminals has them (the host's ENQ, the
request, the host's ACK and answer, ACK, EOT):

| | VISA II transaction |
|---|---|
| V.22, normal | 7.0 s |
| V.22, fast connect | 2.3 s |
| V.29 FastPOS | 2.9 s |

For such short messages V.29's training at each turn costs more than
its speed saves; FastPOS pays off on longer ones.

## Not there yet

- Hypercom's short training for V.29 FastPOS (each turn sends the full
  V.29 training, about 250 ms) and its fallback to V.22bis.
- A fast connect caller with no answer tone at all (Silicon Labs'
  U80 bit 15), and the timings of particular hosts.
