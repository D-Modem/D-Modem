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
  with the private parameters of V.42bis and the user data subfield of
  V.44.
- **V.42bis** (`V42bis`, `v42bis.cpp`): 2048 codewords and strings of 34
  characters offered, lower when the remote asks; the encoder switches
  between transparent and compressed mode by the ratio over the last 256
  characters.
- **V.44** (`V44`, `v44.cpp`): 4096 codewords, strings of 255 and a
  history of 12288 octets offered (as the Agere modem does), lower when
  the remote asks. The caller offers V.42bis and V.44 in its XID; the
  answerer takes V.44 when both have it, and answers with what it took.
  See below.
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

`link_test` (`ctest -R link_test`) runs it without a modem: V.42bis and
V.44 round trips (text, noise, escape characters, repeats, small
dictionaries, flushes), V.44's transparent mode, HDLC framing with a bit
error, and two Links back to back bit by bit: detection, XID, V.44 both
ways, V.42bis when either has no V.44, bit errors (REJ and T401 recovery),
a slow DTE (RNR), DISC, async, and a LAPM answerer with an async caller.

## V.44

V.44's compressed data is as the Agere modem's code has it (its V.44
encoder and decoder, read from the library, agree with each other and
with this): see `v44.h` for the tokens. The negotiation in XID is the
user data subfield (0xFF) after the groups: the parameter set "V44"
(0x40), C0 (0x41, 0: the stream method), P0 (0x42, the directions, as
V.42bis's), the codewords (0x43/0x44, transmit/receive), the longest
string (0x45/0x46) and the history (0x47/0x48); we take the smaller of
each pair and the remote's, the same both ways.

Both sides start in compressed mode, codewords from 4 in 6 bits and
ordinals of 7. Our encoder looks for the longest string of the
dictionary at the next characters (by their first two), extends it with
what followed it in the history, sends STEPUP when a codeword or
character needs it, REINIT when the dictionary or the history is full
(the Agere decoder takes no more history than that), and FLUSH with the
rest of the octet when the DTE has nothing more for now; it stays in
compressed mode. The decoder also takes transparent mode (ETM, escapes
as in V.42bis, ECM back), which the Agere encoder switches to on data
that does not compress.

Against the Agere modem (`ctest -R agere_v44`), 60000 octets each way of
text, binary octets, repeats and noise go through both ways at V.34 (the
Agere modem: `CONNECT 33600 V44`), its encoder going to transparent mode
and back now and then; the other Agere and interop calls use V.44 too
(V.42bis with the original Smart Link library, which has no V.44).
