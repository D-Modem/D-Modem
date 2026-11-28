# V.42 bis: data compression

This page describes ITU-T Recommendation V.42 bis (01/90), "Data
compression procedures for data circuit-terminating equipment (DCE)
using error correction procedures". It is our own description. It is not
a copy of the Recommendation. For the normative text, refer to the
Recommendation (`docs/specs/fetch.sh V.42bis`). For the codewords, the
parameters and the procedures, refer to [the technical
details](v42bis-details.md).

## Summary

V.42 bis makes the data from the computer smaller before the modem sends
it. Thus, text and many other files go through the line faster.

- The compression operates on the error-corrected link of V.42. A lost
  or damaged frame breaks the compression, so error correction is
  necessary.
- The encoder keeps a dictionary of strings that it saw before. It sends
  a codeword for each string. The decoder makes the same dictionary from
  the codewords that it receives. Thus, the dictionary itself is never
  sent.
- The dictionary has N2 codewords (512 or more, 2048 is usual). A string
  has at most N7 characters (6 to 250).
- Some data does not compress, for example a file that is already
  compressed. Then the encoder changes to transparent mode and sends
  the characters as they are.
- The two modems agree on the use and the parameters of V.42 bis in the
  XID exchange of V.42. Compression can operate in one direction or in
  the two directions.

## Where V.42 bis is used

Most modems from V.32 bis to V.90 have V.42 bis. V.92 modems also have
V.44, which compresses better.

## Related standards

- V.42: the error correction (LAPM) under V.42 bis, and the XID exchange
  that negotiates it.
- V.44: the newer compression.
- MNP 5: the older compression of Microcom.

## V.42 bis in D-Modem

`link/v42bis.cpp` has the encoder and the decoder. `link/lapm.cpp`
negotiates V.42 bis in XID, and MNP can also use it. The implementation
is described in [the data link layer](../link.md) and [its technical
details](../link-details.md).
