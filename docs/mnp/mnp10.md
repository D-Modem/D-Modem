# MNP 10

MNP 10 is the MNP class for adverse lines: rural lines, long distance
lines and cellular calls. It continuously measures the line, changes
the LT size, and moves the data pump to a lower or higher rate while
the link stays up. Microcom licensed MNP 10 to Rockwell in 1991
(secondary source: Wikipedia), and many Rockwell-based modems have it.

No specification is public. D-Modem's MNP 10 comes from the firmware of
the Microcom DeskPorte 28.8P (M042116P), and we compared it with the
TravelCard FAST (M012315). The full rules are in the MNP 10 and MNP
10EC protocol specification (the Claude Doc of the project). This page
gives a summary.

## On the line

MNP 10 uses the frames of MNP 4, and adds these items:

| Item | Bytes | What it does |
|---|---|---|
| LR parameter 8, the 0x10 bit | – | Offers MNP 10 |
| LR parameter 0x0C | 1 to 4 octets | The shifts that the modem allows (fallback, fall forward), the 1200 and 2400 bit/s rates, cellular mode, and the V.32bis and V.34 rate masks |
| LR parameter 0x0D | 2 octets | The receive level (G) and the eye quality (E) of the sender |
| LR parameter 0x11 | 1 octet | The originator's mark |
| Type 8 frame, signal | `02 08 S` | A rate shift signal: 02 asks for a fallback, 03 answers it, 04 asks for a fall forward, 05 accepts it, 09 refuses it |
| Type 8 frame, probe | `04 08 0A Q R` (and `G E` in cellular mode) | A line probe, once each second |
| Type 9 frame | `04 09 R N(R) N(k)` | An LA that also acknowledges a probe |

Each modem calculates the result of the LR exchange from the two LRs.
The result is the AND (or the lower value) of the two. Thus, the two
modems get the same result.

MNP 10 is active only with framing mode 3 (MNP 3 or higher). Type 8
and type 9 frames have no sequence numbers.

## What the modems do in the data phase

- **Probes.** Each modem sends a probe each second. A probe that comes
  back makes the LT size larger, up to 256 octets. A retransmission
  makes it smaller.
- **Line quality.** An average of retransmissions (X) and a count of
  small LT sizes (H) tell when the line is bad. A clean line with large
  LT frames and a low eye quality value tells when a higher rate is
  possible.
- **Rate shifts.** A fallback moves the call to a lower rate. A fall
  forward moves it to a higher rate. Hold-off times prevent too many
  shifts: 30 s after the start, 10 s after a shift, and a time that
  doubles (up to 1920 s) after each failed fall forward.
- **Cellular mode.** The probes also carry G and E. The modem changes
  its transmit level from the reports of the other modem.

## The V.42 upgrade

With `AT-K1`, a V.42 call can become MNP 10. The originator puts an MNP
group (the `FF` group with "MNPI" and the LR parameters) at the end of
its XID. An answerer with `-K1` answers in the same way. Then MNP
frames go inside HDLC frames.

## D-Modem

D-Modem supports MNP 10, as the DeskPorte does it on the line. The code
is in `link/mnp10.cpp`, and the data pump side is
`Modem::mnp10_shift()` in `core/modem.cpp`.

- `S113` bit 0 (the default) offers MNP 10.
- `S113` bit 1 (the default) adds parameter 0xC0 (refer to the list
  below).
- The LR of the caller is 38 octets or less, because some modems
  refuse a longer LR. Thus, parameters 0xC0 and 0x0D are only in the LR
  when they fit, and parameter 0xC0 can be shorter. The LR does not
  include parameter 0x11, because the PCTel modem refuses it. With
  V.42bis in MNP (`S114=1`), the LR does not offer MNP 10. Refer to
  [link-details.md](../link-details.md#mnp) and
  [interop-details.md](../interop-details.md#mnp).
- `S113` bit 2 sends the long LR: all the MNP 10 parameters and
  parameter 0x11, with no limit. This bit is 0 by default. Set it only
  if an MNP 10 modem that answers must have these parameters.
- `AT-K`, `AT-Q`, `AT)M`, `AT@M`, `AT*H` and `AT:T19` operate as on the
  DeskPorte.
- The default of `AT-K` is 0 in D-Modem (1 on the DeskPorte). Thus, a
  call between two D-Modems keeps V.42bis or V.44.
- Between two D-Modems, LR parameter 0xC0 (D-Modem's own) lets the call
  move between all the modulations that both modems have. A DeskPorte
  skips this parameter.

These items are not done:

- Link resume (LR parameter 0x82).
- Microcom security (parameter 0x0B) and the asynchronous protocol
  (parameter 0x80).
- The data pump changes of [MNP 10EC](mnp10ec.md).

Refer to [the technical details of the link layer](../link-details.md#mnp-10-and-10ec)
for the ladder of rates, the restart of the data pumps and the tests.

## What is not confirmed

The MNP 10 specification gives four details from the code, not from a
capture. A capture from the DeskPorte FAST+ must confirm them:

1. The answerer's XID in a `-K1` upgrade.
2. That the first frame after a `-K1` upgrade is the originator's LA.
3. The LT size just after a rate shift (33 octets).
4. The times of a modulation change on the line.
