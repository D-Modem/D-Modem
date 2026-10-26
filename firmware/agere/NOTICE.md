# Agere USB modem ROM patches

Copyright (c) 2002, 2003, 2004, 2005, 2006 Agere Systems, Inc. All rights reserved.

**The binary contents of the `.bin` files in this directory are Agere Systems' software. They are
licensed by Agere under the terms in [`LICENSE`](LICENSE), Agere's Soft Modem End User Software
Licence Agreement as it ships with agrsm 2.1.73, and are not under D-Modem's licences (the
AGPL-3.0 or the commercial licence).** They are redistributed unaltered, under that licence's
section 2.3 ("limited redistribution ... in source and binary forms"), with its notices. Agere
keeps the right to refuse redistribution (2.3.1); if it does, they will be removed.

These files are not linked or compiled into any D-Modem program. dmodemd reads them at run time
and downloads them into an Agere USB 2.0 soft modem (USB `047e:2892`, `047e:2828`) at each start,
the only use the licence grants (1.1). They are byte for byte the patches in every Agere driver
for that modem: agrsm 2.1.73's `agrmodemlib.o` and `agrmodem.ko`, and `AGRSM.sys` and
`AGRSM64.sys` 2.1.70 to 2.2.98, under these symbol names in `agrmodemlib.o`:

| file | octets | for |
|---|---|---|
| `bulk_patch_v106.bin` | 748 | ROM 1.0.6 |
| `bulk_patch_v121_flash.bin` | 748 | ROM 1.2.1 in flash |
| `bulk_patch_v121_rom.bin` | 1156 | ROM 1.2.1 in ROM |
| `async_clockchange.bin` | 508 | ROM 1.0.6's clock change command |

`SHA256SUMS` has their hashes. What they do is in
[`docs/agere-usb.md`](../../docs/agere-usb.md#what-the-patches-do).
