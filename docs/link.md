# The data link layer

After two modems connect, the sound on the line contains a stream of bits.
The data link layer sets what these bits mean. It does three tasks:

1. **Error correction.** Telephone lines add noise, and noise changes
   bits. Error correction puts the data in packets with numbers and
   checksums. It sends again the packets that arrive with damage. Thus,
   the data that comes out is exactly the same as the data that went in.
2. **Compression.** Text and many files become much smaller. Thus, if the
   modem compresses them before it sends them, the connection is faster.
3. **Framing.** Without error correction, the link layer sends plain
   characters with start and stop bits, as the first modems did.

The code is in `link/`. For a detailed description of each protocol, its
settings and its tests, refer to [the technical details](link-details.md).

## Error correction

| Protocol | What it is |
|---|---|
| **V.42 (LAPM)** | the standard error correction, which almost all modems have used since the 1990s |
| **MNP 2, 3 and 4** | the older error correction of Microcom, for modems that do not have V.42 |

When a call connects, the two modems agree on the protocol to use.
D-Modem tries V.42 first. If the other modem does not have V.42, D-Modem
falls back to MNP. If the other modem does not have MNP, D-Modem uses no
error correction.

D-Modem does not support MNP 10 and MNP 10EC, which old cellular modems
used. Nobody published them, and there is no equipment to test them
with. D-Modem connects to these modems with V.42.

## Compression

| Protocol | What it is |
|---|---|
| **V.44** | the newest standard, which V.92 modems use; usually the best |
| **V.42bis** | the older standard, which V.34 modems use |
| **MNP 5** | the compression of Microcom, used with MNP 4 |
| **zstd** | modern Zstandard compression, used only when the two ends are D-Modem |

The modems agree on the best protocol that the two modems have. Between
two D-Modems, the modems use zstd. On long text, it compresses some
times better than V.44.

## For special programs

- **V.80 synchronous mode** (`AT+ES=6` to call, `AT+ES=,,8` to answer)
  disables error correction and compression. It sends the data of the
  program directly through. Video phones (H.324) and some other programs
  must have this mode, because they make their own frames.
- **The multiplexer of V.76** for voice and data together (V.70, DSVD).
  When V.8bis selects DSVD, the data goes on one channel of V.76 and the
  voice (G.729 Annex A) goes on a different channel. The messages of V.75
  open each channel. Plain V.42 is not used. Refer to [V.70](v70.md).
- **ISDN protocols.** On a digital ISDN channel, the link layer also does
  X.75, V.120 and synchronous PPP. Refer to [ISDN](isdn.md).

## Where it came from

The code started as the link layer of Smart Link in `slmodemd`. We wrote
it again in 2026, with the same behaviour on the line. During this work,
we repaired some bugs of the old code (lost characters, corrupted output,
padding that was not necessary).

## Testing it

    ctest --test-dir build -R Link

This command tests all protocols without a modem. It uses two links back
to back, with bit errors, slow receivers and lost frames. The interop
tests use the link layer with the modems of other companies
([Interop against other soft modems](interop.md)).
`compress_bench` (built from `link/test/compress_bench.cpp`) measures the
results of each compressor on your own files.
