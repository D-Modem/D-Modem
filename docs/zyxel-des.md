# ZyXEL encryption (S102, ATCK)

The ZyXEL Elite 2864I can encrypt the data of an ISDN call with DES or
triple DES. D-Modem does this encryption on X.75 and V.120 calls
(`AT+MS=CLEAR`, `AT+B2=0` or `AT+B2=9`). The data comes from a trace of
the Elite 2864I 1.11 firmware (`zyxel/firmware-trace.md`, section 8, in
the project files). No test with a real Elite was done. Thus the
procedure is correct D-Modem to D-Modem, and probably correct with an
Elite.

## Commands

| Command | Function |
|---|---|
| `ATS102=n` | Bit 0: offer the encryption. Bit 1: triple DES. Bit 2: no key exchange (use the ATCK key). Bit 3: disconnect if the remote does not agree. The default is 0. |
| `ATCK<key>` | The key for bit 2. The key is the characters up to `>` or the end of the line. Without `<`, ERROR. `ATZ` keeps the key. |

In the Elite, S102 also has bit 6 (the metering pulse) and bit 7 (the
bundle of two B channels, `&J1`). D-Modem does not use these bits.

Examples:

- `ATS102=1` on the two modems: single DES with a key from the key
  exchange (PKDS). You do not need a key.
- `ATS102=7CK<my key>` on the two modems: triple DES with the ATCK key.
  The two keys must be the same.

The modem removes the spaces from the command line. Thus a key cannot
have spaces.

## The procedure

1. The link (X.75 or V.120) comes up as usual (SABM or SABME, then UA).
2. Each modem sends an XID command. The XID has the group 0xF0 (the name
   of the protocol, "X75" or "V120", and the Elite's parameters 1 to 3)
   and the ZyXEL group 0xFF:
   - PI 0xFF, length 5: `ZyXEL`.
   - PI 0xF1, length 1: S102 AND 7.
   - PI 0xF2, length 16: the PKDS public value. This parameter is not
     sent when S102 bit 2 is 1.
3. Each modem answers the XID command of the other modem with an XID
   response. The response has PI 0xFE in place of 0xF1, and PI 0xFD in
   place of 0xF2. A modem without S102 bit 0 sends the response without
   these two parameters.
4. When a modem has the options of the remote, it decides:
   - Encryption: the remote has bit 0.
   - Triple DES: the two modems have bit 1.
   - The PKDS key: the two modems have bit 2 = 0, and the remote sent its
     value. Else the ATCK key.
5. The I frames wait until the decision. If no XID response comes after
   4 tries (T1 each), the modem continues without encryption. With bit
   3, the modem disconnects.

The Elite reads the options only from the XID response. D-Modem reads
them from the command or the response. D-Modem uses triple DES only when
the two modems have bit 1. The Elite uses triple DES when the remote has
bit 1. Thus set bit 1 on the two modems, or on neither.

## The key

PKDS is a Diffie-Hellman exchange with 128-bit numbers (big-endian):

- p = `A07D4039 89FE7A69 1E99F822 1AD64BD5` (a prime)
- g = `9E348B1A 11DF8D3F 9D426536 DF4F535B`

Each modem sends y = g^x mod p, with x a random secret. The shared value
K = y_remote^x mod p gives the two keys: A = K bytes 0 to 7, B = K bytes
8 to 15.

ATCK makes A and B from the key text:

1. A = B = `01 23 45 67 89 AB CD EF`.
2. The modem adds each character to the next byte: characters 1 to 8 to
   A, characters 9 to 16 to B.
3. After character 8: for each bit k of character 8 that is 1, A[k] =
   -A[k].
4. After character 16: for each bit k of character 16 that is 1, B[k] =
   NOT B[k]. Then A[k] = A[k] + B[k] for all k. Character 17 starts at
   A[0] again.

A key has 56 bits in bytes 0 to 6. Each 7 bits are one byte of a
standard DES key without its parity bit. Byte 7 is not used.

The DSP of the Elite then examines the keys (A only for single DES with
the ATCK key; A and B for triple DES and for PKDS):

1. The last 16-bit word of B increases by 1.
2. If a key is in the table of 64 weak keys, the DSP changes the 16
   bytes with its LFSR and goes back to step 1.
3. Byte 7 of each key becomes 0.

`link/zcrypt.cpp` has the table and the LFSR. Three entries of the table
are not DES weak keys. D-Modem uses the table as the Elite has it.

## The cipher

- Single DES: key A. Triple DES: encrypt with A, decrypt with B, encrypt
  with A.
- IV: bytes 0 to 3 = A[0..3] - B[0..3], bytes 4 to 7 = A[4..7] + B[4..7]
  (32-bit, big-endian, modulo 2^32).
- 64-bit CFB. Each block: keystream = E(R). Transmit: C = P XOR
  keystream, then R = C. Receive: R = C, then P = C XOR keystream.
- The last n < 8 bytes of a frame use the first n bytes of the
  keystream. Then R moves n bytes to the left and the n cipher bytes go
  after it.
- R continues from frame to frame. Each direction has its own R. The two
  start at the IV.
- The modem encrypts the information field of each I frame (on V.120,
  after the terminal adaption header). The address, the control field
  and the FCS are clear. A frame that the modem sends again has the same
  cipher text.

## Not known

- The offset of the encrypted data in the Elite frames (D-Modem uses the
  start of the information field).
- The third key of the Elite triple DES (D-Modem uses A).
