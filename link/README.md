# The data link layer

`link/` is what goes on between the DTE's characters and the data pump's
bits once a call has connected (C++, `namespace dlink`):

- **V.42 detection phase** (`Detector`, `hdlc.cpp`): the originator's ODPs,
  the answerer's ADPs, or the HDLC flags of a modem that skips the phase;
  T400 at the answerer.
- **V.42 LAPM** (`Lapm`, `lapm.cpp`) over **HDLC** framing (`Hdlc`): modulo
  128, windows of up to 15 frames of up to 128 octets, REJ recovery,
  RNR while the DTE does not take the data, T401 counted in transmitted
  bits (2 s at the data pump's rate), and parameter negotiation in XID,
  with the private parameters of V.42bis.
- **V.42bis** (`V42bis`, `v42bis.cpp`): 2048 codewords and strings of 34
  characters offered, lower when the remote asks; the encoder switches
  between transparent and compressed mode by the ratio over the last 256
  characters.
- **Asynchronous characters** (`Async`): start bit, 8 data bits, stop bit,
  for the modulations without V.42 and when the remote has no LAPM.

`Link` (`link.h`) puts them together. The modem core is its `Owner`: it
gives and takes the DTE's characters, hears of the link's progress
(`Status::Detected`, `Up`, `Released`, `Error`) and of what XID agreed, and
gets the log lines. The data pump calls `get_bits()` and `put_bits()`. The
link does not depend on the modem core or the data pumps.

The protocol code is Smart Link's slmodemd link layer (`modem_pack.c`,
`modem_ec.c`, `modem_comp.c`) as classes, with the same behaviour on the
line; the interop tests (`ctest -L "interop|agere"`) run it against the
original library and the Agere modem. Two faults of the old glue between
V.42bis and the DTE are gone: a character could be lost when the encoder's
pending output filled a frame exactly, and a decoder flush could overwrite
decoded characters the DTE had not taken yet.

`link_test` (`ctest -R link_test`) runs it without a modem: V.42bis round
trips (text, noise, escape characters, small dictionaries), HDLC framing
with a bit error, and two Links back to back bit by bit: detection, XID,
V.42bis both ways, bit errors (REJ and T401 recovery), a slow DTE (RNR),
DISC, async, and a LAPM answerer with an async caller.
