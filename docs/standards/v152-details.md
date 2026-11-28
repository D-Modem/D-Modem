# V.152: technical details

This page gives the technical details for [V.152](v152.md). The clause
numbers are the clause numbers of V.152 (09/2010). This page is our own
description, not a copy of the Recommendation.

## The VBD mode (6, 6.1)

In VBD mode, an implementation must:

- use a codec that keeps the modulated signals, with its own payload
  type, negotiated as clause 7 tells;
- keep the latency from end to end constant;
- disable voice activity detection and comfort noise in the data phase;
- disable the DC removal filters of the speech coder.

It should also consider redundancy or FEC (only when negotiated), and
packet loss concealment that suits modems and fax.

Each implementation must support G.711 A-law and µ-law as VBD codecs.
An offer must have PCMA or PCMU as a VBD codec. An answer must have at
least one VBD codec.

## Echo cancellers (6.2)

An echo canceller is not a part of VBD. If one is used, it must comply
with G.168, and it disables itself on the tones of G.168.

## The transport services (6.3)

| Service | Content |
|---|---|
| Non-assured VBD | RTP only. RFC 4733 events for the tones are optional (6.3.1.2). |
| Assured VBD | RFC 4733 events for the tones, and RFC 2198 redundancy or RFC 5109 FEC for the data |

## SDP (7.1)

| Attribute | Function |
|---|---|
| `a=gpmd:<pt> vbd=yes` | Payload type pt is a VBD codec. With no such attribute, the end has no VBD. `vbd=no` says that pt is not for VBD. |
| `a=maxmptime:<t1> <t2> ...` | The longest packet time (ms) of each format of the `m=` line, in the same sequence. `-` for a format with no packet time, such as events. |

- A VBD payload type should be dynamic. A static type (PCMU 0, PCMA 8)
  that is marked as VBD must not carry voice.
- When `a=maxmptime` is there, `a=ptime` is ignored. With no
  `a=maxmptime`, `a=ptime` applies to all the formats. With neither,
  the default of RFC 3550 applies (20 ms for G.711).
- An end must not send VBD packets longer than the packet time of the
  other end (7.1.0.2).

## RFC 4733 events (8)

The tones of modems, fax and text telephones (ANS, ANSam, CNG, V.21
preamble and others) can go as RFC 4733 events. Then the far gateway
makes the tone again.

## Changes between audio and VBD (10, 11)

1. A gateway in audio mode detects a VBD signal (clause 9), such as
   2100 Hz with or without phase reversals, CNG or V.21 flags, at least
   from the telephone network to IP.
2. It changes to the VBD codec at once. It must not lose a part of the
   V.8 CI, the V.8 bis signals or a text telephone character.
3. A received RTP packet of the VBD payload type also changes the
   gateway to VBD, but only after it received packets of its previous
   state.
4. The gateway goes back to audio at the end of the modem or fax
   signals, at voice, or at silence in the two directions.
5. When the two gateways negotiated SSEs of V.150.1, they use those
   (clause 11) in place of this procedure.

## V.152 in D-Modem

| Item | D-Modem (`frontend/dmodemd/sip.cpp`) |
|---|---|
| Mode | All the audio of a modem call is VBD. There is no audio mode to change from. |
| VBD codecs | PCMU (0) and PCMA (8), both marked `a=gpmd:<pt> vbd=yes`. Static types, because no voice goes on them. |
| `a=maxmptime` | 20 ms for each G.711 format, `-` for the events, SSE and redundancy |
| Our packet time | 20 ms. When the far end gives a shorter `a=maxmptime` for the format, or a shorter `a=ptime`, our packets are 10 or 5 ms. |
| Latency | A fixed jitter buffer (60 ms, `SIP_JITTER`) |
| VAD, comfort noise, DC removal | None. A lost packet is silence. |
| Assured mode | RFC 2198 redundancy (`SIP_RED`, 2 frames again), offered on each call. RFC 4733 events for the answer tones. |
| FEC (RFC 5109) | Not done |
| Echo canceller | None |
| H.248, H.323 | Not done |
| `SIP_V152=no` | No V.152 attributes |

## Corrections (2026-11)

| Item | Before | Now |
|---|---|---|
| The packet time of the far end (7.1.0.2) | Our packets were always 20 ms. A far end with `a=maxmptime:10` or `a=ptime:10` got packets that were too long. | Our packets are no longer than the `a=maxmptime` entry of the format, or than `a=ptime`: 20, 10 or 5 ms |
