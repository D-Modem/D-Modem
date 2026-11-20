# ZMODEM with zstd

This page is the specification of the zstd mode of ZMODEM. The D-Modem
BBS, GUI and app use this mode. Two programs that use this mode compress
each file with zstd (RFC 8878). When one program does not use this mode,
the transfer is normal ZMODEM.

The base protocol is "The ZMODEM Inter Application File Transfer
Protocol" (Chuck Forsberg, 1988). This page tells only the differences.
In this page, "must" is a requirement and "can" is an option.

## 1. Summary

- The receiver sets the bit `CANZSTD` in its ZRINIT.
- The sender selects zstd for each file with the transport option
  `ZTZSTD` in the ZFILE header.
- The data of the file goes as a sequence of independent zstd frames.
- All positions on the line are offsets in the file. They are not
  offsets in the compressed data. Thus ZRPOS, ZACK, ZEOF and crash
  recovery operate as in normal ZMODEM.

## 2. Values

| Name | Field | Value | Meaning |
| --- | --- | --- | --- |
| `CANZSTD` | ZRINIT, ZF1 | `0x40` | The receiver can decompress zstd frames. |
| `ZTZSTD` | ZFILE, ZF2 (transport option) | `8` | The data of this file is zstd frames. |

ZF0 is the fourth byte after the frame type (ZP3). ZF1 is the third byte
(ZP2). ZF2 is the second byte (ZP1). lrzsz uses the bits `0x01`, `0x02`,
`0x08` and `0x10` of ZF1 in ZRINIT. The ZMODEM specification uses the
values 1, 2 and 3 of ZF2 in ZFILE (`ZTLZW`, `ZTCRYPT`, `ZTRLE`).

## 3. Negotiation

1. The receiver sends ZRINIT. If it can decompress zstd, it sets
   `CANZSTD`.
2. If `CANZSTD` is not set, the sender must not use zstd. The transfer
   is normal ZMODEM.
3. For each file, the sender decides to compress the file or not. The
   sender can compress a file only when the size of the file is more
   than 0.
4. To compress a file, the sender sets ZF2 to `ZTZSTD` in the ZFILE
   header. The other fields of ZFILE do not change.
5. The receiver replies to ZFILE as usual (ZRPOS, ZSKIP or ZFERR).

These rules also apply:

- A receiver that did not set `CANZSTD` must reply ZSKIP to a ZFILE with
  ZF2 set to `ZTZSTD`.
- The size in the ZFILE subpacket is the size of the file. It is not
  the size of the compressed data.
- A program must have a setting that stops zstd. When zstd is stopped,
  the receiver does not set `CANZSTD` and the sender does not set
  `ZTZSTD`.

### 3.1 When not to compress

The sender should not compress a file that is compressed now. The D-Modem
programs use two tests:

1. The extension of the name. For example: `.zip`, `.gz`, `.zst`,
   `.7z`, `.rar`, `.arj`, `.arc`, `.lzh`, `.zoo`, `.jpg`, `.png`,
   `.gif`, `.mp3`, `.mp4`, `.pdf`.
2. A test compression of the first 64 KiB of the file. If the compressed
   frame is more than 95% of the 64 KiB, the sender does not compress
   the file.

## 4. Frames

The sender cuts the file into chunks. It compresses each chunk as one
zstd frame. Each frame must obey these rules:

- The frame is a zstd frame as RFC 8878 section 3.1.1 specifies. It is
  not a skippable frame.
- The frame has the field `Frame_Content_Size`.
- The frame has the content checksum (`Content_Checksum_flag` is 1).
- The frame has no dictionary (`Dictionary_ID_flag` is 0).
- The content of the frame is not more than 256 KiB (262144 bytes).
- The window is not more than 256 KiB.
- Each frame is independent. It does not refer to data in other frames.

The size of a chunk is the decision of the sender. The D-Modem programs
use 64 KiB. After each ZRPOS, they use half of the last size, to a
minimum of 1 KiB. After each ZACK, they use two times the last size, to
a maximum of 64 KiB. Thus a line with many errors gets small frames.

The receiver must refuse a frame that does not obey the rules. It must
do the same steps as for a bad CRC (section 6).

## 5. Data

The frames go in ZDATA subpackets, with ZDLE escapes and a CRC, as
normal data. Also:

- A ZDATA header must be at the start of a frame. Its position is the
  offset in the file of the first byte of the content of that frame.
- A frame can go in more than one subpacket. A subpacket can hold the
  end of one frame and the start of the next frame.
- A subpacket that ends with ZCRCW or ZCRCE must end at the end of a
  frame.
- ZEOF gives the size of the file.

The receiver keeps the bytes of a frame until the frame is complete. To
find the end of a frame, the receiver reads the frame header and the
block headers (RFC 8878 sections 3.1.1.1 and 3.1.1.2). Then it
decompresses the frame and examines the content checksum. Then it
writes the content to the file.

The receiver writes only complete, correct frames. Its position is the
offset in the file after the last frame that it wrote. The receiver
sends this position in ZACK (after ZCRCQ and ZCRCW) and in ZRPOS.

## 6. Errors

The receiver does these steps for a bad CRC, a bad frame, a decompression
error or a bad content checksum:

1. Discard the bytes of the incomplete frame.
2. Send ZRPOS with its position (section 5).

The sender then starts a new ZDATA at that position, with a new frame.
The cost of one error is less than one frame.

## 7. Resume (crash recovery)

A receiver that has the first L bytes of a file can reply ZRPOS(L) to
ZFILE. The sender then starts with a new frame at offset L. Each frame is
independent, thus any offset is correct.

The receiver writes only complete frames. Thus a partial file is always
correct for its full length.

## 8. Test vectors

### 8.1 Headers

ZRINIT, hex header, with `CANFDX`, `CANOVIO`, `CANFC32` and `CANZSTD`.
The frame type is 1, ZF1 is `0x40` and ZF0 is `0x23`:

```
2a 2a 18 42  "0100004023b39c"  0d 8a 11
```

ZFILE with `ZTZSTD`, as a hex header. The frame type is 4 and ZF2 is
`0x08`. A sender usually sends ZFILE as a binary header, with the same
five bytes (`04 00 08 00 00`):

```
2a 2a 18 42  "040008000020a7"  0d 8a 11
```

### 8.2 Frame

The content is these 102 bytes. The SHA-256 of the content is
`888b38d76e5ecb717949daa035976535ec86f6490249dc70d5aee338445b52ce`:

```
D-Modem ZMODEM zstd test vector.<CR><LF>  (3 times)
```

The reference zstd 1.5.5 (`zstd -19 --content-size --check`) gives this
frame of 55 bytes:

```
28 b5 2f fd 24 66 55 01 00 24 02 44 2d 4d 6f 64
65 6d 20 5a 4d 4f 44 45 4d 20 7a 73 74 64 20 74
65 73 74 20 76 65 63 74 6f 72 2e 0d 0a 01 00 8a
82 aa 27 32 21 69 9d
```

The parts of the frame:

| Bytes | Value | Meaning |
| --- | --- | --- |
| 0 to 3 | `28 b5 2f fd` | The magic number |
| 4 | `24` | The frame header descriptor: single segment, checksum, `Frame_Content_Size` in 1 byte |
| 5 | `66` | `Frame_Content_Size` = 102 |
| 6 to 8 | `55 01 00` | The block header: the last block, compressed, 42 bytes |
| 9 to 50 | | The compressed block |
| 51 to 54 | `32 21 69 9d` | The content checksum |

A different encoder can give a different frame for the same content. A
receiver must decompress all frames that obey section 4. It must not
compare the frame with this vector.

## 9. Implementations

| Program | Code | Library |
| --- | --- | --- |
| D-Modem BBS | `internal/xfer/zmodem.go`, `internal/xfer/zstd.go` | `github.com/klauspost/compress/zstd` (BSD-3-Clause) |
| D-Modem app | `shared/.../transfer/ZModem.kt` | libzstd (BSD-3-Clause) |
| D-Modem GUI | the C core of the GUI | libzstd (BSD-3-Clause) |
