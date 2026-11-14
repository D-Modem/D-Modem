# MNP 10EC

MNP 10EC (Enhanced Cellular) is a 1996 addition to MNP 10 by Microcom
and Rockwell. It changes the data pump for cellular calls: fades, hand
offs and the limiter of the radio channel. The link layer is the same
as in [MNP 10](mnp10.md).

## On the line

The link layer adds very little to MNP 10:

- **LR parameter 8, the 0x20 bit.** The sender's data pump is in
  cellular EC mode. The DeskPorte firmware uses this bit only to select
  the "CELLULAR EC" text in its connect message. It has no other effect
  on the protocol.
- **The V.42 detection phase.** An MNP 10EC answerer sends the answer
  detection pattern "EM" five times or more, then "EC" (V.42, Appendix
  VI).

The data pump changes are in the DSP code of the Rockwell chips, not
in the controller firmware. Thus the DeskPorte ROM does not contain
them.

## What the data pump changes probably do

No document gives the data pump changes of MNP 10EC. The patents of
the period give the methods that cellular modems used (see
`mnp10ec/patent-research.md` in the project files):

- Transmit de-emphasis and receive post-emphasis, to cancel the
  pre-emphasis and the limiter of the AMPS radio channel (Celeritas,
  US 5,386,590 and US 5,533,048). Rockwell shipped de-emphasis in its
  cellular chipsets from 1995.
- A transmit level that changes with the reports of the other modem
  (Microcom, US 5,241,565). MNP 10 already does this in the link layer.
- A longer time before a carrier loss stops the call, for hand offs
  (Spectrum, US 4,697,281).

These are possible parts of MNP 10EC, not confirmed parts.

## D-Modem

D-Modem has the link layer of MNP 10EC, but not the data pump changes.

- D-Modem never sets the 0x20 bit of parameter 8. It ignores the bit
  from a peer.
- As the V.42 originator, D-Modem ignores the "EM" patterns and accepts
  the "EC" pattern after them (`Link.AdpCellular`). As the answerer, it
  sends "EC" only.
- `AT)M1` and `AT@M` give the cellular mode of MNP 10: the line reports
  in the probes and the transmit level control.
- `AT-SEC` is accepted, but has no effect.

A cellular call to D-Modem is usually on a digital network now. Thus
the radio channel effects that MNP 10EC is for do not occur on most
calls.
