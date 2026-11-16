# MNP 5

MNP 5 is the data compression of MNP. It compresses the data from the
DTE before the link puts it into LT frames. It is not part of V.42.
Microcom licensed it to other modem makers free of charge (secondary
source: Wikipedia). Most modems with MNP 4 also have MNP 5.

## What it does

MNP 5 has two parts. The transmitter applies them in this sequence:

1. **Run-length coding.** After three equal characters, the
   transmitter sends a count of the equal characters that follow (0 to
   250). At 250, a new run starts.
2. **Adaptive frequency coding.** The transmitter keeps a table of the
   256 characters, in the sequence of how frequently they occur. A
   character moves up the table past the characters that occurred
   fewer times. When the count of the top character gets to 255, all
   counts are divided by 2.

Each character is sent as the code of its position (rank) in the table:

| Ranks | Code (header, then the bits of the rank) | Bits |
|---|---|---|
| 0, 1 | `000x` | 4 |
| 2, 3 | `001x` | 4 |
| 4 to 7 | `010xx` | 5 |
| 8 to 15 | `011xxx` | 6 |
| 16 to 31 | `100xxxx` | 7 |
| 32 to 63 | `101xxxxx` | 8 |
| 64 to 127 | `110xxxxxx` | 9 |
| 128 to 255 | `111xxxxxxx` | 10 |

- Rank 255 has one more bit, 0, after its code.
- A run count is sent as the code of its value. It does not use the
  table, and it does not change the table.
- The codes fill each octet from the most significant bit.
- The flush is eleven 1 bits. The transmitter sets the other bits
  of the octet to 1. The transmitter flushes when the DTE has no more
  data.
- The compressed stream continues from one LT frame to the next.

Text becomes about two thirds of its size. Random data becomes a
little larger, because the ranks from 128 to 255 take 10 bits.
Microcom gave about 2:1 on the best data and 1.6:1 on typical text
(secondary source: Wikipedia).

## On the line

- LR parameter 9, the 0x01 bit, offers MNP 5. The responder sets the
  bit only if both modems want MNP 5.
- The DeskPorte accepts MNP 5 with any class. Its parser does not
  examine the framing mode or parameter 8 when it reads parameter 9.
- The DeskPorte also uses the 0x04, 0x08 and 0x10 bits of parameter 9
  for V.42bis in MNP (refer to
  [firmware-findings.md](firmware-findings.md#f5-parameter-9) and
  [V.42bis in MNP](#v42bis-in-mnp)).

## D-Modem

D-Modem supports MNP 5. The code is `Mnp5` in `link/mnp5.h` and
`link/mnp5.cpp`.

- The code follows the PCTel modem's code (`MnpCompress`,
  `MnpDecompress` and `MnpEncSort` in its `control.o`).
- D-Modem decodes the Agere modem's MNP 5 octets. For the same text,
  D-Modem's encoder sends the same octets as the Agere modem
  (`Link.Mnp5`).
- `AT%C1` and `AT%C3` offer MNP 5. `AT%C2` gives MNP 4 with no MNP 5
  (and V.42bis, if the two modems agree on it: refer to
  [V.42bis in MNP](#v42bis-in-mnp)).
- The interop tests examine MNP 5 in the two directions with the Agere,
  PCTel, SM56, HSF, Broadcom, Intel and ESS soft modems.

## V.42bis in MNP

Some modems use V.42bis compression on an MNP link in place of MNP 5.
The LR has two forms for it. We read the two forms in the LR code of
the DeskPorte (Z180) and of the SM56 modem (`ecs_ac_send_lr`,
`ecs_de_check_param9` and `ecs_de_ini_param_negot` in `sm56.ko`). The
two modems agree.

**Parameter 14 (0x0E), the long form.** The length is 4:

| Octet | Contents |
|---|---|
| 1 | P0, the directions: the 0x01 bit is the initiator to the responder, the 0x02 bit is the responder to the initiator |
| 2, 3 | P1, the dictionary size (N2), the high octet first |
| 4 | P2, the largest string (N7) |

- The DeskPorte refuses an LR in which parameter 14 has a different
  length, a P1 less than 512, or a P2 less than 6 or more than 250. It
  uses the lower P1 and P2 of the two modems, and the directions that
  the two modems have.
- The SM56 refuses (with an LD) a response with a P1 or a P2 that is
  more than its own, and an LR from an initiator with a P2 more than 32.
- The SM56 sends P0 = 3, P1 = 2048 and P2 = 32, with parameter 9 =
  0x01 (MNP 5).

**Parameter 9, the short form.** The 0x04, 0x08 and 0x10 bits offer
V.42bis with P1 = 512, 1024 and 2048. The short form is for the two
directions, with P2 = 32.

- An initiator sets a bit for each size that it can use. The HSF modem
  sends 0x1D (MNP 5, and the three sizes).
- A responder sets one bit: the largest size that the two modems can
  use.

**The two modems agree on one compression.** When the LRs agree on
V.42bis, the call does not use MNP 5. A responder answers in the form
of the initiator: parameter 14 if the initiator sent it, if not
parameter 9 with one V.42bis bit. A responder does not send the two
forms (the SM56 refuses a response with the two forms).

**The modems of the interop tests:**

| Modem | Its LR as initiator | V.42bis in MNP |
|---|---|---|
| SM56 | parameter 9 = 0x01, parameter 14 = 03 08 00 20 | Yes, the long form |
| HSF | parameter 9 = 0x1D | Yes, the short form |
| Agere, PCTel, Broadcom, Intel, ESS | parameter 9 = 0x01 | No |

As responders:

- The PCTel modem does not answer an LR in which parameter 9 has the
  short form and no parameter 14.
- The ESS modem agrees on MNP 5. It retrains approximately 0.2 s into
  the call, and then it waits for a new LR. Our caller sends its LR
  again, and the call completes. Refer to
  [interop-details.md](../interop-details.md#mnp).
- When the LR has the two forms, the SM56 and the HSF modems agree on
  V.42bis. The Agere, PCTel, Broadcom and Intel modems agree on MNP 5.

### D-Modem and V.42bis in MNP

D-Modem uses V.42bis on an MNP link. The code is in `link/mnp.cpp`
(`Mnp::v42_lr()`, `Mnp::rx_lr_v42bis()`) and `link/link.cpp`
(`Link::mnp_v42bis()`).

- `AT%C2` and `AT%C3` (the default) let the link use V.42bis. `AT%C1`
  gives MNP 5 only.
- **As responder**, D-Modem always accepts V.42bis in the two forms. It
  uses V.42bis in place of MNP 5 when the initiator offers the two.
- **As initiator**, D-Modem offers V.42bis only when `S114` = 1. The
  default is 0, because the ESS modem does not complete a call with
  that LR. With `S114` = 1, the LR has the two forms: parameter 9 with
  the short form, and parameter 14 with P2 = 32.
- D-Modem refuses (with an LD) a response that has a P1 or a P2 that is
  more than its offer, or directions that it did not offer.
- `ATI11` shows `Protocol Negotiation Result MNP` and
  `Data Compression Result V42bis`.
- Tests: `Link.MnpV42bisLr`, `Link.MnpV42bisResponder`,
  `Link.MnpV42bis` and `Link.MnpV42bisRefused` in
  `link/test/link_test.cpp`, the script `call_mnp_v42bis`, and the
  interop calls `sm56_mnp5_sm56_calls`, `hsf_mnp5_hsf_calls`,
  `sm56_mnp_v42bis_open_calls` and `hsf_mnp_v42bis_open_calls`.
