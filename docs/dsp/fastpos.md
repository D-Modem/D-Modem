# Point of sale: fast connect and V.29 FastPOS

Card terminals, vending machines and parking meters dial the host of
their bank. They send only a few hundred bytes in each call: a request,
an answer, an acknowledgement. In a usual modem call, the start-up takes
most of the time, not the data. Thus, the terminals and their hosts use
their own shorter start-ups:

- **Fast connect** decreases the handshake of V.22 (1200 bit/s), Bell
  212A and V.22bis (2400 bit/s) from about 3 to 5 seconds to less than 1.
- **V.29 FastPOS** operates V.29, the 9600 bit/s fax modem, half duplex
  on a dial-up line. One side sends at a time. Each turn starts with the
  short training of V.29.

They are not ITU Recommendations. They come from Hypercom (now Verifone),
and they became de facto standards. The timing of hosts is not always the
same. D-Modem follows the published data about them, mainly AN93 of
Silicon Labs (Appendix D, "EPOS applications"). Two D-Modems connect with
each other with them. Nobody has tried them with a real terminal or host.

For the full technical description, refer to
[the technical details](fastpos-details.md).

## Fast connect

    ATS49=1          fast connect (V.22, V.22bis, Bell 212A)
    ATS49=3          the same, with HDLC flags in the handshake
    ATS50=40         the answer tone's length, in 10 ms (0: 400 ms)

Use these with the usual `AT+MS=22`, `AT+MS=122` or `AT+MS=212`. Set them
on the two ends. The answering modem sends a short answer tone. The
calling modem answers it immediately. Each modem starts the data 150 ms
after it hears 50 ms of the other modem. `S49=3` sends HDLC flags in
place of the scrambled ones that the handshake usually has. Some hosts
want this. D-Modem accepts the two types from the far end.

| | Online after the answer |
|---|---|
| V.22, normal | 5.6 s |
| V.22 or Bell 212A, fast connect | 0.9 s |
| V.22bis, fast connect | 1.4 s |

(In simulation, two D-Modems, `S92=0`. The normal V.22 includes its
3.3 s answer tone.)

## V.29 FastPOS

    AT+MS=V29FP      (or AT+MS=229, AT+MS=V29FASTPOS)

Set it on the two ends. The calling modem sends a steady 980 Hz tone
while it waits. The answering modem answers with a 2225 Hz tone (S50 sets
its length). Then the two modems give CONNECT. After this, the side whose
computer has data sends it, and the other side listens. The turn goes to
the other side when the data stops. `AT+MS=V29FP,0,4800,7200` operates
it at 7200 bit/s. V.29 has no method to agree on a speed. Thus, set the
same setting on the two ends.

## The data: SDLC frames

Payment terminals put their data in SDLC (HDLC) frames. The host polls,
and the terminal answers. With D-Modem, the computer at each end does
this through the synchronous access mode of V.80. This mode sends full
frames with their checksum:

    AT\N0+ES=6,,8+ESA=0,0,0,,1

Then send `<EM><flag>` (bytes 19h B1h) to start the framed mode. Then
send the bytes of each frame, followed by `<EM><flag>`. A full
transaction has six frames: the SNRM of the host, the UA of the terminal,
a request and its answer, DISC and UA. In simulation, from the dial, it
takes:

| | Transaction |
|---|---|
| V.22, normal | 8.6 s |
| V.22, fast connect | 3.9 s |
| V.29 FastPOS | 3.5 s |

Plain characters (`AT\N0` without `+ES`) also go through the two modes.
The VISA II exchange of older terminals uses them (the ENQ of the host,
the request, the ACK and answer of the host, ACK, EOT):

| | VISA II transaction |
|---|---|
| V.22, normal | 7.0 s |
| V.22, fast connect | 2.3 s |
| V.29 FastPOS | 2.9 s |

For these short messages, the training of V.29 at each turn takes more
time than its speed saves. FastPOS gives an advantage on longer messages.

## Not there yet

- The short training of Hypercom for V.29 FastPOS (each turn sends the
  full V.29 training, about 250 ms) and its fallback to V.22bis.
- A fast connect caller with no answer tone (U80 bit 15 of Silicon Labs),
  and the timings of specific hosts.
