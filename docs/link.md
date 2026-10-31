# The data link layer

Once two modems have connected, the sound on the line carries a stream of
bits. The data link layer decides what those bits mean. It does three
jobs:

1. **Error correction.** Phone lines add noise, and noise flips bits.
   Error correction wraps the data in numbered, checksummed packets and
   resends any that arrive damaged, so the data comes out exactly as it
   went in.
2. **Compression.** Text and many files shrink a lot, so compressing
   them before they are sent makes the connection faster in practice.
3. **Framing.** Without error correction, it sends plain characters
   with start and stop bits, as the earliest modems did.

The code is in `link/`. For how each protocol works in detail, its
settings and its tests, see [the technical details](link-details.md).

## Error correction

| Protocol | What it is |
|---|---|
| **V.42 (LAPM)** | the standard error correction, used by almost every modem since the 1990s |
| **MNP 2, 3 and 4** | Microcom's older error correction, for modems that do not have V.42 |

When a call connects, the two modems agree on which one to use. D-Modem
tries V.42 first and falls back to MNP, and to no error correction at
all, if the other modem lacks them.

MNP 10 and MNP 10EC, used by old cellular modems, are not supported:
they were never published and there is nothing left to test them
against. D-Modem still connects to such modems using V.42.

## Compression

| Protocol | What it is |
|---|---|
| **V.44** | the newest standard, used by V.92 modems; usually the best |
| **V.42bis** | the older standard, used by V.34 modems |
| **MNP 5** | Microcom's compression, used with MNP 4 |
| **zstd** | modern Zstandard compression, used only when both ends are D-Modem |

The modems agree on the best one both of them have. Between two
D-Modems zstd is used; on long runs of text it compresses several times
better than V.44.

## For special programs

- **V.80 synchronous mode** (`AT+ES=6` to call, `AT+ES=,,8` to answer)
  turns off error correction and compression and passes the program's own
  data straight through. Video phones (H.324) and some other programs
  need this because they do their own framing.
- **ISDN protocols.** On a digital ISDN channel the link layer also does
  X.75, V.120 and synchronous PPP. See [ISDN](isdn.md).

## Where it came from

The code began as Smart Link's link layer in `slmodemd`. It was rewritten
in 2026, behaving the same on the line, and several bugs of the old code
(lost characters, corrupted output, needless padding) were fixed on the
way.

## Testing it

    ctest --test-dir build -R Link

This runs every protocol without a modem: two links back to back, with
bit errors, slow receivers and lost frames. The interop tests run the
link layer against other companies' modems
([Interop against other soft modems](interop.md)).
`compress_bench` (built from `link/test/compress_bench.cpp`) measures how
well each compressor does on your own files.
