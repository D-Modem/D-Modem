# Provenance of the interop test binaries

This file tells where each binary file in `dsp/test/` comes from. The
licence terms of these files are in [NOTICE.md](NOTICE.md) and in
[LICENSING.md](../../LICENSING.md). The terms of this repository do not
apply to these files.

Use the SHA-256 values to make sure that a copy is the same as the copy
in this repository.

## Linux soft modem drivers

| File | Owner | Package | SHA-256 |
|---|---|---|---|
| `smartlink/dsplibs.o` | Smart Link Ltd. | Debian `sl-modem` 2.9.11~20110321 | `1f3e56d0dfae1a6aaf4eb6fcc4875a4524905e010d5758114cde288b3cf0b379` |
| `agere/agrsm_core.o` | Agere Systems | agrsm-20070804 (Agere SoftModem 2.1.40) | `a00a7fe7a21c2b20a353bf373042c3e30599ff4a9238a061acb494bb559e171c` |
| `sm56/sm56.ko` | Motorola, Inc. | sm56-06.09.05 for Fedora Core 3 | `a757fd85bc27cc4bd816073efa9d03591307973de5c5aabcb37e0bb225ba516a` |
| `pctel/dsp.o` | PCTel, Inc. | pctel-0.9.7-9-rht-6 | `15d8e5ab1ac4019a029fd9e562e0e74b3f233aa3ab42621334506721e66f82b1` |
| `pctel/control.o` | PCTel, Inc. | pctel-0.9.7-9-rht-6 | `38d46cabd67f675f8c011afd4725a41a1a813075bae4b516c94cda1b4dfec69d` |
| `hsf/hsfengine.o` | Conexant Systems | Linuxant hsfmodem-7.80.02.06oem | `6d570356f37e62153374f2f50b5d3a3e4b1246285199cbba2aa7d631a5f894fc` |
| `hsf/hsfmc97ich.o` | Conexant Systems | Linuxant hsfmodem-7.80.02.06oem | `b6c18aab4585498700b401b0948161f25ee18e6863af48b2b2f35b0536b60c25` |
| `hsf/hsfsoar.o` | Conexant Systems | Linuxant hsfmodem-7.80.02.06oem | `3be23c8e6d6ee8f6bc92b89913590ad4828cf2013ba4cf9abc3be854c22ecde9` |
| `hsf/hsfusbcd2.o` | Conexant Systems | Linuxant hsfmodem-7.80.02.06oem | `19d1fdf623415c6ec88b0fd3b61e15f9561267631b8f8bb1542dd6366d381970` |
| `hsf/hsfmc97ich.inf` | Conexant Systems | Linuxant hsfmodem-7.80.02.06oem | `0d7401ddcefd2c362adcdc19fb0b828312983a8e3970c2389596e2ad271a970c` |
| `hsf/hsfcadmus2.inf` | Conexant Systems | Linuxant hsfmodem-7.80.02.06oem | `346fd4a7260ad7cd03b590a5cb6c6466454f8d6b5cf9aaa2003cbde053ca7f81` |
| `hsf/hsfcadmus2smart.inf` | Conexant Systems | Linuxant hsfmodem-7.80.02.06oem | `22a8566aac63280ffd085c2d9b374d9b58c613778a0e11425d88d36daa8a4c35` |
| `hsf/hsf.cty` | Conexant Systems | Linuxant hsfmodem-7.80.02.06oem | `bf205eda7dc753a416c347179e2058cdb1a15f053afbc609fe25995ca00eba34` |
| `intel/537core.lib` | Intel Corporation | Intel537EP 2.37.50.1 for Fedora Core 1 | `e81d02cbe185191e1bf9ed46672117edb02ccfb7f3d493800c1aa428f9d6f49f` |
| `intel/wwh_dflt.c` | Intel Corporation | Intel537EP 2.37.50.1 for Fedora Core 1 | `4d308a3988ae17c9348341feab0f05432e3d6d2483f512667ff09d03a7ccf1e8` |
| `intel/license.txt` | Intel Corporation | Intel537EP 2.37.50.1 for Fedora Core 1 (Intel's licence) | `87e01e6b9bf0b40ecf0d665d033348c61f964a1793124e0516908afd4b1518fa` |

## Windows soft modem drivers

| File | Owner | Package | SHA-256 |
|---|---|---|---|
| `broadcom/BCMSM.sys` | Broadcom Corporation | Dell "V.92 Soft Modem (Data Fax) Driver" A05 (BCM V.92 56K Modem 3.4.19) | `033c6716ef63b1984a8f7a7b36c8efce1c7d9ec622dd97132d54eb5b8a5c0662` |
| `broadcom/BCMSMwxp.inf` | Broadcom Corporation | Same package | `34f4059d27803bb1b4947ff6dc1f4b872230eaf738551f8c267ba6f50e363ca8` |
| `ess/es56hpi.sys` | ESS Technology, Inc. | ESS ES56H-PI modem driver 5.43.070 (`ess54370.exe`), Windows XP files | `dc0f3b3614dc3cb168759526d47a306977616318e1bc15102987dd1a415d576f` |
| `ess/mdmzn.inf` | ESS Technology, Inc. | Same package | `18c4114cfc3c78581791e6d719e1c7e4a41fdde52beca95695ed96c55b4db8ba` |

## Modem firmware

| File | Owner | Source | SHA-256 |
|---|---|---|---|
| `3com/tc110.bin` | 3Com Corporation (formerly Megahertz Corporation) | The TC110 flash update for the XJ5560 and CC5560 PC Cards (1998-02), `tc110.exe`, from the USRobotics support site, Megahertz PC Cards page. This file is the 8 flash pages in `Tc110.upf` (Intel HEX), as binary. | `2d3917228f80e725c6510f07b3ff30f7a73996905f703d9abea406c9a6f86bee` |
| `usr/IDSDL302.ROM` | U.S. Robotics (later 3Com); modified by Igor Diagilev (ID_SDL) | The ID_SDL 3.02 flash image of a Courier V.Everything (supervisor 7.3.14, DSP 3.0.13, 06/06/98), from the ID_SDL 3.02 update (`IDSDL302.XMD`). We got it from [courier-emu](https://github.com/D-Modem/courier-emu). | `49f4182cc961aef983ff43468b7b7e55c03205c9dba80e9689fe20aa6ff2ccc5` |
| `usr/c51-mask.rom` | Texas Instruments and U.S. Robotics | The on-chip ROM of the TMS320C51 DSP of a 20.16 MHz Courier V.Everything board, read from the board. We got it from [courier-emu](https://github.com/D-Modem/courier-emu). | `d57bc46e1bcd6d4dc8872b97bba2d98ba8fb6b8661440c566b534f0b3f82fac9` |

## Files that we made

| File | How we made it | SHA-256 |
|---|---|---|
| `usr/courier-nvram.bin` | The emulated Courier (`half_courier`) wrote it on an erased part with `AT&F` and `AT&W`. We then wrote the settings records that enable the options of the Courier. It is not a copy of a part of a person or company. | `4b21d145baffed2a686c3597845fd0eac1aa13ca7ec5b88391151940d5de63d9` |
