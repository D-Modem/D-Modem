# The 56k client on a SIP line

D-Modem can be the **analogue end** (the client) of a 56k call on a SIP line
or on the line of a telephone system. It does not need modem hardware for
this. It calls the 56k server of an internet provider with these standards:

| Standard | Command | Speed down / up in the tests |
|---|---|---|
| V.92 | `AT+MS=92` (the default) | 56000 / 41333 bit/s |
| V.90 | `AT+MS=90` | 56000 / 33600 bit/s |
| K56flex | `AT+MS=56` | 56000 / 31200 bit/s |
| x2 | `AT+MS=X2` | 32000 / 24000 bit/s, then up to 56000 / 28800 |

The modem also stays the **digital end** (the server) on the same line.
When D-Modem calls, its V.8 CM gives only the mode that `AT+MS` selects:

- With `AT+MS=90` or `AT+MS=92`, the CM gives only the analogue end. Thus,
  the server of a provider cannot select V.91 by mistake.
- With `AT+MS=V91`, the CM gives only V.91 (up to 62666 bit/s in the two
  directions against another D-Modem).

When D-Modem answers, it offers the two ends and V.91, and the CM of the
caller selects one:

- Against a real analogue 56k modem, D-Modem is the server.
- Against another D-Modem that calls with `AT+MS=V91`, the two modems use
  V.91.

S117 selects the end on a digital line:

| S117 | End |
|---|---|
| 0 (the default) | Automatic: the analogue end when the modem calls, the two ends when it answers |
| 1 | The digital end (the server) only |
| 2 | The analogue end (the client) only |

## Why it operates on SIP

On a SIP line with G.711, the samples of the call are the digital values of
the telephone network, on the clock of the network. This is the best case for
the receiver of the client. There is no codec between them, and thus no
clock drift. (The drift occurs on modem hardware at 8000 Hz. Thus, that
case stays off if you do not set `DSP_V90A=1`.)

The call operates through a lost packet (20 ms of silence) and through a
restart of the jitter buffer of dmodemd, with no retrain.

## Limits

- We tested the client only against the server of D-Modem, on a simulated
  network. We did not try it against the server of a real provider yet.
- The path must be G.711 from end to end, with no transcoding.
- A slip of one sample in the network, before the SIP line, stops the
  56k data. The client cannot see this slip.

## Settings

- `DSP_V90A=0`: no V.90 or V.92 client (the modem is only the server).
- `DSP_V92A_QC=1`: use the quick connect of V.92 as the client on a SIP
  line. It is off by default there.
- `DSP_K56A=0`, `DSP_X2C=0`: no K56flex or x2 client.

For the technical details, refer to
[the technical details](sip-client-details.md).
