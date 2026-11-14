# Interop tests against other soft modems: technical details

This page is the technical companion to [Interop against other soft modems](interop.md), the plain-English overview. Read that page first. This page gives all the details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

## The harnesses

The interop tests make calls between the open modem and other soft
modems:

- the soft modem of Smart Link, on our modem core;
- the Agere, SM56 (Motorola), PCTel, HSF (Conexant) and Intel soft
  modems, each in user space on an emulation of its hardware;
- the Windows drivers of Broadcom and ESS, on our own Windows kernel
  and an emulation of their chips;
- the modems of spandsp;
- a USRobotics Courier V.Everything, the full modem emulated with its
  own firmware;
- the open modem itself, on an emulated HD-audio modem codec.

[Building and testing](testing-details.md) describes the call driver
(`interop_call`), the network simulator (`netsim`) and how the calls
keep time.

Each modem has a harness program, `half_<name>`. A harness reads the
audio of the line on file descriptor 3 and writes its own audio on file
descriptor 4. It is the DTE of the modem (the data terminal, the
computer side) on a pty. Each modem also has its own ctest label. Thus,
`ctest -L <label> -j16` runs its calls.

### The shared program: `modem_half.hpp`

The harnesses of the closed modems and of spandsp are C++ classes on
`dsp/test/modem_half.hpp`. `half::HalfModem` is the program itself. It
does these tasks:

- It reads its settings from the environment: `<NAME>_DEBUG`,
  `<NAME>_RATE` (the sample rate of the line, 8000 by default),
  `<NAME>_SYNC` and `<NAME>_SYNC_PHASE` (refer to [the calls' options](#the-calls-options)).
- It opens the pty.
- It runs the line one sample at a time, and serves the DTE each 48
  samples.

A harness derives from it and supplies three items:
- the start of the modem (`start()`);
- the processing of a line sample (`line_sample()` or `line_block()`);
- its DTE service (`dte_poll()`).

### The emulated hardware: `dsp/test/hw/`

The hardware that a harness emulates is in `dsp/test/hw/`, one header
for each type. Each is a mixin: a class template that the harness also
derives from, with the harness as its template parameter. Thus, the
harness itself supplies all that the hardware needs from its board. For
example, the ICH controller reads and writes the registers of its codec
through the `codec_read()` and `codec_write()` of the harness. The x86
traps do their `in` and `out` instructions through its `io_in()` and
`io_out()`. The declaration of the SM56 harness is:

    class Sm56 : public HalfModem, public Ac97Codec<Sm56>, public Ich<Sm56>,
                 public PciDevice<Sm56>, public X86Traps<Sm56>,
                 public CodecLine<Sm56>

| Header | Hardware | Harnesses |
|---|---|---|
| `ac97_codec.hpp` | an AC'97 modem codec's registers (`Ac97Codec`) | Agere (Si3038), SM56 (Motorola), HSF (Conexant) |
| `ich.hpp` | the Intel ICH AC'97 modem controller (`Ich`) | Agere, SM56, HSF |
| `pci.hpp` | the PCI configuration space of one device, and its ports (`PciDevice`) | SM56, HSF, Broadcom, ESS |
| `x86_traps.hpp` | `in`, `out`, `cli`, `sti` and moves to and from control registers in a driver's own code (`X86Traps`) | SM56, the Windows kernel |
| `tsc.hpp` | `rdtsc` on the line's clock (`LineTsc`) | HSF, the Windows kernel |
| `codec_line.hpp` | the line and a codec running at a rate of its own: the resamplers and the codec's noise (`CodecLine`, `<NAME>_NOISE`) | SM56, PCTel, HSF, Broadcom, Intel, ESS, HDA |
| `smartdaa.hpp` | Conexant's SmartDAA behind its codec (`SmartDaa`) | HSF |
| `si3034.hpp` | Silicon Labs' Si3034 DAA behind a serial port (`Si3034`) | Broadcom |
| `bcm4212.hpp` | Broadcom's BCM4212 modem chip: its registers and DMA (`Bcm4212`) | Broadcom |
| `i537ep.hpp` | Intel's 537EP modem chip: its registers (plain memory) and DMA rings (`I537ep`) | Intel |
| `es2838.hpp` | ESS's ES2838 modem chip: its ports, its codec's registers and its sample FIFO (`Es2838`) | ESS |
| `hda.hpp` | an Intel HD Audio controller: its reset, the immediate command interface, and one input and one output stream on buffer descriptor lists (`HdaController`) | HDA |
| `i80186.hpp` | an Intel 80186 core: the 8086's instructions and the 80186's, real mode; memory in 4 KiB pages, I/O and the integrated peripherals the harness's (`I80186`) | Courier |
| `tms320c5x.hpp` | a TI TMS320C5x core, the C51's instruction set and memory map as the Courier has them (`C5x`) | Courier |
| `courier.hpp` | the whole Courier: the 80C186EB's timers, interrupt controller and serial port, the ASIC (hook, lamps, ring, DIP switches, the DSP's mailbox and data lanes), the NVRAM, the DSP and its codec (`Courier`) | Courier |
| `si3054_hda.hpp` | a Silicon Labs Si3054 HD-audio modem codec: its root node and modem function group, its registers as nodes 2 to 21, the hook and the line monitor (`Si3054Hda`) | HDA |

The members of each mixin have its name (`ich_sample()`, `pci_cfg`,
`si3034_rate`, ...). Thus, a harness can derive from all combinations of
them without name clashes.

`CodecLine` puts a windowed sinc resampler in each direction and adds
noise to the input of the codec, because a real line is never digitally
silent. The noise is uniform and the same on each run. Its rms is 5 by
default (approximately -76 dB of full scale). `<NAME>_NOISE` sets a
different value.

## The peers' binaries

The vendor modems are their own binaries, as their Linux drivers
supplied them, in `dsp/test/<peer>/`. They are i386 objects, and the
harnesses link them in (built `-m32`). The Broadcom and ESS binaries are
their Windows drivers. `half_broadcom` and `half_ess` load them at run
time.

Each file is the file of a known release. Where that release is still
available, we compared the file byte for byte with it. The files belong
to their owners, and the owners give the licences for them. The terms of
this repository do not apply to them
([dsp/test/NOTICE.md](../dsp/test/NOTICE.md)).

| Peer | Files | Release | Built | Byte-identical to |
|---|---|---|---|---|
| Smart Link | `smartlink/dsplibs.o` | slmodem-2.9.11-20110321 | 22 Sep 2005, GCC 3.4.2 | `modem/dsplibs.o` of Debian's `sl-modem_2.9.11~20110321.orig.tar.gz` |
| Agere | `agere/agrsm_core.o` | Agere SoftModem 2.1.40, from agrsm-20070804 | 16 Nov 2005, GCC 3.3.5 (Debian) | none to be had (below) |
| SM56 | `sm56/sm56.ko` | Motorola sm56-06.09.05 for Fedora Core 3 (2.6.9-1.667) | 17 May 2005, GCC 3.4.2 (Fedora Core 3) | `usr/tmp/sm56.ko` of `sm56-06.09.05-1-fc3-2.6.9-1.667.i686.rpm` |
| PCTel | `pctel/dsp.o`, `pctel/control.o` | pctel-0.9.7-9 (Linux driver K2.4.7.V92.001) | pumps 21 Jun 2001, GCC 2.95.3; controller dated 26 Mar 2003, GCC 2.96 (Red Hat 7.1) | `src/lib/amr/dsp.a`, `src/lib/control.a` of pctel-0.9.7-9-rht-6 |
| HSF | `hsf/hsfengine.o`, `hsfmc97ich.o`, `hsfsoar.o`, `hsfusbcd2.o`, `hsfmc97ich.inf`, `hsfcadmus2.inf`, `hsfcadmus2smart.inf`, `hsf.cty` | Linuxant hsfmodem-7.80.02.06oem | 10 May 2010, GCC 3.4.6 (Red Hat) | `modules/imported/<name>-i386.O`, `nvm/inf/<name>.inf`, `nvm/inf/hsf.cty` of `hsfmodem-7.80.02.06oem.tar.gz` |
| Intel | `intel/537core.lib`, `intel/wwh_dflt.c` | Intel537EP 2.37.50.1 for Fedora Core 1 SMP (core "Release 4.65 - Jan 17 2004") | 17 Jan 2004, GCC 3.2 (its IPP parts Intel C++ 7.0 Beta) | `coredrv/537core.lib`, `coredrv/wwh_dflt.c` of `intel-537ep_secure-2.37.50.1-fc1-smp.tgz` |
| Broadcom | `broadcom/BCMSM.sys`, `broadcom/BCMSMwxp.inf` | BCM V.92 56K Modem 3.4.19, Dell's "V.92 Soft Modem (Data Fax) Driver" A05 | 26 Jul 2002, Microsoft linker 7.0 | the hashes in the package's catalog, signed by Microsoft (WHQL, 31 Jul 2002) |
| ESS | `ess/es56hpi.sys`, `ess/mdmzn.inf` | ESS ES56H-PI modem driver 5.43.070 (`ess54370.exe`, for Windows 98 to XP) | 24 Mar 2003, Microsoft linker 5.12 | the hashes in the package's catalog, signed by Microsoft (WHQL, 2 Apr 2003) |
| spandsp | (the system's libspandsp) | spandsp 0.0.6 (release 20110122) | | |
| Courier | `usr/IDSDL302.ROM`, `usr/c51-mask.rom`, `usr/courier-nvram.bin` | ID_SDL v3.02 (USRobotics supervisor 7.3.14, DSP 3.0.13) | 13 Mar 1998 (USRobotics), 6 Jun 1998 (ID_SDL) | none to be had (below) |

The SHA-256 hashes of the files, and what we compared each one with:

- **Smart Link.** `dsplibs.o`:
  `1f3e56d0dfae1a6aaf4eb6fcc4875a4524905e010d5758114cde288b3cf0b379`.
  The tarball
  (`cea496e9c34a16cf347124b27ed673f3a6e87089f83e15ab5e0b0365a56fd253`)
  is at `https://deb.debian.org/debian/pool/non-free/s/sl-modem/`. The
  library came with slmodemd 2.9.11 in the first commit of D-Modem.
- **Agere.** `agrsm_core.o`:
  `a00a7fe7a21c2b20a353bf373042c3e30599ff4a9238a061acb494bb559e171c`.
  - It contains "Agere SoftModem Version 2.1.40" and "Built on
    11/16/2005 16:43:35". For its PCM module, it contains "AGERE PCMLL
    V2.1.37.105, seed = 1220, 2/1/2004".
  - agrsm-20070804 is a dated package of the linmodems "sv92" series
    (from linmodems.technion.ac.il, which is now gone). It contains the
    2.1.40 of Agere. Its successor, agrsm-20080203, answers `ATI` with
    the same version.
  - The builds of 2.1.40 that Agere made for Mandrake, Red Hat and SuSE
    (December 2005, `agrmodemlib.o` on the Kaiomy Pinball-C driver CD at
    archive.org) have the same 3132 global symbols. But other compilers
    built them, and their `LXHardwareInfoCreate` and
    `LXHardwareInfoDestroy` are global.
  - In our file, those two symbols are weak, because the harness
    supplies its own (the Makefile of the package makes them weak).
    Thus, the object was changed after Agere linked it, and no file from
    Agere is the same byte for byte.
- **SM56.** `sm56.ko`:
  `a757fd85bc27cc4bd816073efa9d03591307973de5c5aabcb37e0bb225ba516a`,
  with `vermagic=2.6.9-1.667 686 REGPARM 4KSTACKS gcc-3.4`, "SM56 Rel.
  6.09 Build 05" and "Copyright 1998-2004, Motorola Inc.". The RPM
  (`1f24ba64dae3901de3fe497ec47f6490b7756558cd9ec26f568cc475b0da13d0`;
  vendor Motorola, built 2005-05-17, its checksums good, unsigned) is
  no longer available. Motorola gave it only to distributors and OEMs,
  and its copies at linux-xp-brasil.com.br and linmodems.technion.ac.il
  are gone.
- **PCTel.** `dsp.o`:
  `15d8e5ab1ac4019a029fd9e562e0e74b3f233aa3ab42621334506721e66f82b1`;
  `control.o`:
  `38d46cabd67f675f8c011afd4725a41a1a813075bae4b516c94cda1b4dfec69d`.
  - These are the `.a` files of upstream with new names (they are plain
    objects).
  - The `timeStamp` of the pumps is "Jun 21 2001,14:27:03". The
    controller contains "Linux Driver K2.4.7.V92.001 (03/26/2003)". For
    the DSP, it contains "LCE-336V version .2.15.4.".
  - The copy that we compared
    (`f1304bbcefd1b88dc1733421f7850598334eaf62c5b132c8d129af3405d1ddfb`)
    is
    `https://archive.org/download/pctel-0.9.7-9-rht-6_for_Ubuntu-2.6.15-23-386/`:
    the 0.9.7-9-rht-6 of Robert Thornburrow of 2006, as a third party
    uploaded it. Its ChangeLog changes no binaries after 0.9.7-8. Thus,
    0.9.7-9 and its rht releases contain the same two files.
- **HSF.** `hsfengine.o`:
  `6d570356f37e62153374f2f50b5d3a3e4b1246285199cbba2aa7d631a5f894fc`;
  `hsfmc97ich.o`:
  `b6c18aab4585498700b401b0948161f25ee18e6863af48b2b2f35b0536b60c25`;
  `hsfsoar.o`:
  `3be23c8e6d6ee8f6bc92b89913590ad4828cf2013ba4cf9abc3be854c22ecde9`;
  `hsfusbcd2.o`:
  `19d1fdf623415c6ec88b0fd3b61e15f9561267631b8f8bb1542dd6366d381970`;
  `hsfmc97ich.inf`:
  `0d7401ddcefd2c362adcdc19fb0b828312983a8e3970c2389596e2ad271a970c`;
  `hsfcadmus2.inf`:
  `346fd4a7260ad7cd03b590a5cb6c6466454f8d6b5cf9aaa2003cbde053ca7f81`;
  `hsfcadmus2smart.inf`:
  `22a8566aac63280ffd085c2d9b374d9b58c613778a0e11425d88d36daa8a4c35`;
  `hsf.cty`:
  `bf205eda7dc753a416c347179e2058cdb1a15f053afbc609fe25995ca00eba34`.
  - The tarball
    (`7388bf4338f434aa65363f2097a9f9c2c87f523ac1a549764de831b68f37846c`,
    released 9 May 2010) is at
    `https://www.linuxant.com/drivers/hsf/oem/downloads.php`.
  - The symbols of the objects are `cnxthsf_7800206oem_<name>`. The
    symbols of the "full" build are different (`cnxthsf_7800206full_`),
    and Linuxant no longer supplies that build.
  - The `.inf` is the Linux_Intel_SmartMC.inf of Conexant, "Version:
    5.03.27". The country file contains "Version: V7.72.00".
- **Broadcom.** `BCMSM.sys`:
  `033c6716ef63b1984a8f7a7b36c8efce1c7d9ec622dd97132d54eb5b8a5c0662`;
  `BCMSMwxp.inf`:
  `34f4059d27803bb1b4947ff6dc1f4b872230eaf738551f8c267ba6f50e363ca8`.
  - They come from the Dell package "Communications:V.92 Soft Modem
    (Data Fax) Driver", version A05 (OEM version 3.4.19.1.1, for the
    Dimension 2200 to 8300, created 27 Aug 2002; Windows 2000 and XP).
  - The driver answers `ATI3` with "BCM V.92 56K Modem Version 3.4.19".
    Its debug path is
    `C:\releases\3_4_19_1_1\softmodem\build\Win32\opt-dell-wdm-pentium-mmx\bin\BCMSM.pdb`,
    and the `DriverVer` of the INF is 05/21/2002,3.4.19.
  - The catalog of the package, `BCMSMwxp.cat` (signed by Microsoft
    Windows Hardware Compatibility Publisher, 31 Jul 2002), lists the
    Authenticode SHA-1 of the driver
    (`3261275f43309bd1155178d2549b1496ac9552f2`) and the SHA-1 of the INF
    (`74a33b6de90b0d12e3b688a29c94cc1dd399ea51`). Thus, the two files are
    the files that Microsoft signed. We did not compare the Dell download
    of the package.
- **Intel.** `537core.lib`:
  `e81d02cbe185191e1bf9ed46672117edb02ccfb7f3d493800c1aa428f9d6f49f`;
  `wwh_dflt.c`:
  `4d308a3988ae17c9348341feab0f05432e3d6d2483f512667ff09d03a7ccf1e8`;
  and the `license.txt` of the archive with them
  (`87e01e6b9bf0b40ecf0d665d033348c61f964a1793124e0516908afd4b1518fa`).
  - Someone gave us the archive
    (`bfc42b0917e1f15cf974de0ddeff7dbf2617316e017888eec1c25574cd5b25c0`,
    "Intel(R) 537 V9x DF modem Linux driver", for 2.4.22-1.2115.nptlsmp).
    We did not compare it with a download from Intel.
  - Its licence is a BSD licence (redistribution in binary form with the
    notice).
  - The core answers `ATI3` with "537EP".
- **ESS.** `es56hpi.sys`:
  `dc0f3b3614dc3cb168759526d47a306977616318e1bc15102987dd1a415d576f`;
  `mdmzn.inf`:
  `18c4114cfc3c78581791e6d719e1c7e4a41fdde52beca95695ed96c55b4db8ba`.
  - They are from the `WinXP` directory of the ESS installer
    `ess54370.exe`
    (`d9d87c53d3a3dd48c11990b8823ab51755861004fbb4fd565e998551726385e7`).
    Its `Win2k` and `Win98` directories contain the same driver.
  - The driver is "ES56H-PI Modem Card", "ESS Telephony Driver", file
    version V5.43.070. It answers `ATI3` with "V5.43.070-H-PI" and `ATI1`
    with "ES2838".
  - The catalog of the package, `slm.cat` (signed by Microsoft Windows
    Hardware Compatibility Publisher, 2 Apr 2003), lists the
    Authenticode SHA-1 of the driver
    (`7ccbff2b0b602f8fddec4557ab6505dde6dc0899`) and the SHA-1 of the INF
    (`5dc97a690730326335a15cc2ba6e8392f2935796`). Thus, the two files are
    the files that Microsoft signed.
  - Someone gave us the installer. We did not compare it with a download
    from ESS.
  - An older build of the driver, 5.43.064 of March 2002
    (`ES2838_2839_XP_583211.zip`, which its own catalog also matches), is
    not in the tree.
- **spandsp.** Not in the tree: the build uses the library of the system
  (pkg-config `spandsp`), which is 0.0.6 in Debian and Ubuntu.

## The closed modules' stack

The Agere, SM56, PCTel and HSF modules read some of their stack before
they write it. Valgrind memcheck shows where. (We ran it on the
harnesses linked `-static`. On the dynamic harnesses, it needs the debug
symbols of i386 glibc. Under Valgrind, the `cli`, `in` and `out` of SM56
get to the harness as SIGILL, not as SIGSEGV.)

- **Agere.** `ATProcessorStatusHandler`, called from
  `LinkLayerStatusHandler`, tests a value of the status that
  `V32DataPumpReportStatus` and `V22DataPumpReportStatus` make on their
  stack and do not set. The text of `CodecStatusHandler` (`StatusParser`,
  through its `sprintf`) uses unset bytes of the stack of
  `ReportConnectionStatus`.
- **SM56.** `CM_ConfigReqEvent` tests a value that `ACU_DialResponse`
  does not set on its stack (in four of the five calls that we ran under
  Valgrind).
- **HSF.** Two of the routines of the engine (`hsfengine564_`, under
  its timer, and `hsfengine2004_`) test locals that they have not set.
- **PCTel.** Its `+MS` command ([below](#against-the-pctel-hsp-soft-modem)).

They read the data that the calls before them put there, which includes
the calls of the harness itself. A read or write of the pty, with a
timing that depends on the driver, can put other bytes there. Thus, the
same line samples can give a different call.

Thus, each harness clears 8 KB of the stack below its frame before each
call into its module (`half::stack_clear()`). A module finds the same
data there each time. `HALF_STACK_FILL=<byte>` fills that stack with the
given byte and not with 0. With 0, 0xa5 and 0xff, the Agere, SM56 and HSF
tests (and the PCTel tests, with their top rate given) all pass. They
connect at the same rates and retrain in the same way. Thus, the unset
data that those modules read does not decide their calls here. For the
PCTel `+MS`, it did.

## Against the Smart Link soft modem

`half_smartlink` runs the soft modem of Smart Link: its DSP library
(`test/smartlink/dsplibs.o`, binary-only, i386). slmodemd was written
for this library, and the open DSP keeps its interface
(`dsp/dsplibs.h`). It runs on our modem core and replaces the open
pumps: `half_smartlink` is `half_open` built with the library and
`test/smartlink_glue.cpp`, not with `dsp/`.

It needs no hardware. The core gives it the samples of the line at
9600 Hz, as it gives the open pumps their samples. The library does not
operate at 8000 Hz.

The glue (`smartlink_glue.cpp`) contains the small number of items that
the library needs in addition to the interface of the core (its binary
log records). It also contains the handles of the tests on the library,
`SMARTLINK_V92FULL` and `SMARTLINK_FPE_AT`
([dsp/v92-details.md](dsp/v92-details.md)). These use the `--wrap` of
the linker on `dp_runtime_create` and `modem_dp_register`.

The library also asks for the delay of the line (`MDMPRM_IODELAY`: the
lead silence of PipeModem). It can ask for a change of the delay
(`MDMPRM_UPDATE_DELAY`: input removed or silence added). The core sends
the two requests to its host (`Host::io_delay`, `Host::update_delay`).

Until October 2026, the library ran on the old C core of slmodemd, which
stayed in the tree for it. That core is removed. The 60 calls of that
time gave the same results on our core, in the run-to-run variation of
the old harness.

`ctest -L smartlink -j16` runs the tests. `smartlink_log.py` decodes the
debug lines of the library ([dsp/v92-details.md](dsp/v92-details.md)).

The library is the library of slmodem-2.9.11-20110321 (refer to the
table above). Its parts report their own versions:

- the V.90 modem 2.98 (25-Mar-04, "Modified Quick Connect without Memory
  + Train Time + Constel Power");
- the V.92 modem 1.1 (9-Apr-01, "Memory cleanups + dil descriptor crash
  fix");
- a "V.92 Lite" build.

The changes to slmodem after 2.9.11 are to its drivers only.

Results at the time of writing:

| `AT+MS=` | Smart Link caller → open answerer | open caller → Smart Link answerer |
|---|---|---|
| 34 (V.34) | CONNECT 33600 | CONNECT 33600 |
| 132 (V.32bis) | CONNECT 14400 | CONNECT 14400 |
| 122 (V.22bis) | CONNECT 2400 | CONNECT 2400 |
| 22 (V.22) | CONNECT 1200 | CONNECT 1200 |
| 212 (Bell 212A) | CONNECT 1200 | CONNECT 1200 |
| 103 (Bell 103) | CONNECT 300 | CONNECT 300 |
| 23 (V.23) | CONNECT 1200 | CONNECT 1200 |
| 21 (V.21) | CONNECT 300 | fails: its V.21 answerer only sends 2225 Hz, and does not connect to itself either (not in the tests) |

V.34 tests in addition to plain connects:

- **A line hit.** 300 ms of silence in the two directions in the middle
  of the data (`smartlink_34_hit_*`): the two modems retrain, and V.42
  continues.
- **Rate renegotiation.** It answers our V.34 rate renegotiation
  (`smartlink_34_reneg_*`).

**V.90.** As the analogue modem, it calls our modem (the digital modem,
at 8000 Hz) through the G.711 network of netsim. It connects with V.90,
µ-law and A-law (`smartlink_v90_*`). On the line of the µ-law tests
(with the noise floor of netsim, `NET_FLOOR=3`), the result is
54666/31200 ([dsp/v90-details.md](dsp/v90-details.md)). We also test the
rate renegotiations in the two directions.

**V.92.** Its V.92 is a "V.92 Lite" build, which retrains when it gets
an offer of PCM upstream. `SMARTLINK_V92FULL=1` sets its full V.92 bit.
Then it does PCM upstream with us:

- 56000/38666 (µ-law) and 40000 upstream (A-law) with our prefilter;
- quick connect;
- rate renegotiations and fast parameter exchanges in the two directions
  (`SMARTLINK_FPE_AT=<s>` starts its own exchange that number of seconds
  into the data);
- modem-on-hold;
- the exchange for its timing slip.

These are the `smartlink_v92_*` tests ([dsp/v92-details.md](dsp/v92-details.md)).

**Class 1 fax** in the two directions at 9600 Hz (`smartlink_fax_*`):

| Modulation | Smart Link sender → open receiver | open sender → Smart Link receiver |
|---|---|---|
| V.27ter 2400, 4800 | pass | pass |
| V.29 7200, 9600 | pass | pass |
| V.17 7200, 9600, 12000, 14400 (long train for TCF, short for the page) | pass | pass |

Notes on its fax:

- It makes some bit errors with some of its own signals. Thus, the
  driver allows 1 bit error in 1000 on a page.
- Its V.27ter training contains unwanted 180 degree phase flips. The
  open receiver follows them.
- Its V.17 receiver accepts a short train or not, as a function of the
  position of its 20 ms frames. Thus, the fax calls give the commands of
  the DTE to the modems only each 192 samples (`PIPE_SYNC=192`, refer
  to [testing-details.md](testing-details.md)).

## Against the Agere soft modem

`half_agere` runs the soft modem library of Agere Systems
(`test/agere/agrsm_core.o`, Agere SoftModem 2.1.40 from agrsm-20070804;
refer to above) in user space, on an emulated Intel ICH modem controller
and a Silicon Labs Si3038 codec. Its serial side is a virtual 16550 UART
between the library and the pty. `AGERE_DEBUG=1` prints its log and the
port accesses that the harness does not know.

`ctest -L agere -j8` calls it from and to the open modem. Results at the
time of writing:

| Agere `AT+MS=` / ours | Agere caller → open answerer | open caller → Agere answerer |
|---|---|---|
| V34 / 34 | CONNECT 33600 (our transmitter; we receive at 24000) | CONNECT 33600 / 24000 |
| V32B / 132, V32 / 32 | CONNECT 14400, 9600 | CONNECT 14400, 9600 |
| V32T / V32T ([V.32terbo](dsp/v32terbo.md)) | CONNECT 19200 | CONNECT 19200 |
| V22B / 122, V22 / 22, B212 / 212 | CONNECT 2400, 1200, 1200 | the same |
| V23C / 23, V21 / 21, B103 / 103 | CONNECT 1200/75, 300, 300 | the same |
| V90 / 90 (ours as the digital modem), µ-law and A-law | CONNECT 46666 | - |

The other Agere tests:

- V.44 in the two directions, 60000 octets in each direction
  (`agere_v44_*`);
- our V.34 rate renegotiation in the two directions, and a much noisier
  line 10 s after CONNECT, which causes a retrain (`agere_34_reneg_*`,
  `agere_34_noise`);
- the recovery of phase 2 when our INFO0a is lost
  (`agere_34_lose_info0`);
- V.92 quick connect with short phase 2 on the second call
  (`agere_v92_qc`);
- V.92 modem-on-hold: its request granted (`agere_v92_moh_grant`) and
  denied (`agere_v92_moh_deny`), and our request, which it thinks is a
  retrain (`agere_v92_moh_request`);
- V.90 at the alignment `AGERE_SYNC_PHASE=96` (`agere_v90_phase96`), and
  our V.90 rate renegotiation (`agere_v90_reneg_open`);
- MNP ([below](#mnp)), V.80 ([below](#v80)) and the retrains and
  renegotiations of the DTE ([below](#retrains-and-renegotiations-on-request)).

**Class 1 fax** at 8000 Hz (`agere_fax_*`): V.27ter, V.29 and V.17 (7200
to 14400, short-train pages) pass in the two directions.

The Agere library, in its default V.92 mode, does V.8 with automode off,
and connects to the open modem at V.34.

## Against spandsp's data modems

`half_spandsp` is a modem around the pumps of spandsp (the libspandsp
0.0.6 of the system, the version that Asterisk and FreeSWITCH use). It
has:

- V.21, V.23, Bell 103 and V.22/V.22bis;
- asynchronous 8N1 characters;
- some AT commands;
- V.8 by default, or the modulation forced with `AT+MS=`.

spandsp has no modem of its own. The `data_modems` of its later versions
is not complete: its V.32bis (the pumps of V.17 with the scramblers of
V.32, no handshake) and its V.34 (no data receiver yet) cannot connect.

`ctest -L spanpeer -j8` calls it from and to the open modem at each
modulation that it has, with `AT+MS=` forced on the two sides. (This
label is not part of the `spandsp` label of CI, which has only quick
tests.)

| `AT+MS=` | spandsp caller → open answerer | open caller → spandsp answerer |
|---|---|---|
| 122 (V.22bis) | CONNECT 2400 | CONNECT 2400 (after our LAPM attempt, about 20 s) |
| 22 (V.22) | CONNECT 1200 | CONNECT 1200 (the same) |
| 23 (V.23) | CONNECT 1200/75 | CONNECT 1200/75 |
| 21 (V.21), 103 (Bell 103) | CONNECT 300 | CONNECT 300 |

The modem of spandsp has no error correction. This found two faults in
our modem:

- Our V.42 detection phase counted T400 (750 ms) in bits at a nominal
  28800 bit/s. Thus, it was 9 s at 2400 bit/s, 18 s at 1200 and 72 s at
  300. It now counts at the rate of the pump (`link/hdlc.cpp`).
- Our caller went to LAPM without the detection phase. (V.42 allows
  this. It now has the phase, because of the Courier calls below.) When
  the answerer had no V.42, it sent XID N400 times and then waited for
  nothing. It now continues in asynchronous mode (the `link_test` test
  `LapmCallerAsyncRemote`).

We do not test V.8 between the two. When we wrote this, it did not
connect. Our CM offered only V.34 and V.32bis. On a CM without V.34, our
answerer went to V.32bis automode and did not send a JM for the
modulation that the two had. The two items are changed now:
- Our CM and JM now contain V.34, V.32bis, V.22bis, V.23 and V.21
  (`dsp/dp_v34.cpp`, `v8_msg`).
- Our answerer selects the best modulation that the two have
  (`v8_choice()`; refer to [the HSF modem](#against-the-conexant-hsf-soft-modem)).

spandsp does these things:

- It sends a V.92 octet before its CM (a QC1a with all fields 0, which
  our modem answers), also with V.92 off.
- In 0.0.6, it sends the modulations of the CM back in its JM, if the
  application does not remove them (`half_spandsp` removes them, in
  `v8_cb()`).

`SPANDSP_V8_CALL=<n>` sets the call function that its V.8 sends
(Table 3/V.8; 6, data, by default; 4 is a fax machine that sends). The
adaptive answer tests use it ([below](#adaptive-answer)).
`SPANDSP_DEBUG=1` prints the logs of spandsp and of the harness.

## Against the Motorola SM56 soft modem

`half_sm56` runs the SM56 soft modem of Motorola: the `sm56.ko` of
sm56-06.09.05, built for the kernel 2.6.9 of Fedora Core 3, with its
controller, data pumps and V.92. It runs in user space:

- The module is linked in without changes. At build time,
  `sm56_obj.cmake` gives its kernel imports the names `k_<name>` (into
  `sm56k.o`), and the harness supplies them.
- The harness emulates an Intel ICH modem controller with a Motorola
  "MOTf" codec, on port I/O that traps and that the harness does.
- It patches the small number of reads of `current` in the module.
- It resamples between the 8000 Hz of the line and the rate of the
  codec. The module sets this rate for each modulation, from 7200 to
  12000 Hz (7200 Hz idle, 9600 Hz for V.90).

The module must be `test/sm56/sm56.ko` exactly as Motorola supplied it
(SHA-256 `a757fd85...`): the harness checks the places that it patches.
`ctest -L sm56 -j8` runs the tests. `SM56_DEBUG=1` prints its `printk`s
and the port accesses that the harness does not know. `SM56_DEBUG=2`
prints all port accesses.

**The FPU save.** The interrupt and deferred routines of the module save
the FPU (`fnsave`) to `.rodata.cst4` + 0x640 and + 0x644. This goes
past the end of the section into the data that the linker puts after
it. With the module linked first, this data is its own constants. Then
its V.90 interpolation reads far outside its buffers. The harness
points those four instructions (`fnsave` and `frstor`) at its own
buffers.

| SM56 `AT+MS=` / ours | SM56 caller → open answerer | open caller → SM56 answerer |
|---|---|---|
| V34 / 34 | CONNECT 33600/LAPM (we receive at 24000) | CONNECT 33600 / 24000 |
| V32B,0 / 132 | CONNECT 14400 | CONNECT 14400 |
| V92 (its automode) / 122; V22B,0 / 122 | CONNECT 2400/LAPM | CONNECT 2400 |
| V22,0 / 22, V21,0 / 21 | CONNECT 1200, 300 | the same |
| V90 / 90 (ours as the digital modem), µ-law and A-law | CONNECT 50666/31200 | - |

**V.22bis.** Below V.34, its `+MS` needs automode off: `AT+MS=V22`
alone is an error. When it is forced to V.22bis in that way
(`AT+MS=V22B,0`), it does not connect as the caller to a V.22bis
answerer that sends the answer tone (our modem, or the Agere modem):

- Its unscrambled ones detector starts during the 2100 Hz tone and
  thinks that the tone is unscrambled ones.
- It sends S1 456 ms after the tone ends. With the 75 ms of silence of
  V.22bis, it is then already in training on the unscrambled ones of
  the answerer. Thus, it does not hear the S1 of the answerer.
- It continues at 1200 bit/s, and sends S1 again each 2 s.

With 600 ms of silence, or no silence (the original library answers
without the tone), it connects at 2400. In its automode, which is its
default, it calls our V.22bis answerer at 2400. Thus, the test uses
automode.

**The codec's noise.** The harness adds noise to the input of the codec
(`SM56_NOISE`, rms 5, approximately -76 dB of full scale). On exact
zeros, the frequency estimate of the module stays at its initial
1800 Hz. Its answering automode thinks that this is the AA tone of a
V.32 caller. Then it stops ANSam in 40 ms for the AC of V.32, and a
V.34 caller never hears an answer tone. Rms 3 is not sufficient. Rms 10
makes V.90 one step lower (45333).

**Its V.92.** It sends QC1a in V.8 on each call. On the second call
(`CALLS=2`), it does quick connect with short phase 2 (`sm56_v92_qc`).
Its upstream stays V.34:

- The module has PCM upstream (`UAPCMP3_*`), but it selects it only in
  the INFO1a of quick connect (`SC_QC_Phase2_Set_Info_1A`), from the PCM
  upstream bit of INFO1d.
- Its full phase 2 (`SC_Phase2_Create_Tone_A_Set_1A`) clears that bit
  after it reads it.
- When we force that path on, the module crashes (in `SC_Modulate`).

The INFO1a of its short phase 2 (3200 baud) tells high carrier (bit 33,
Table 19/V.92), but it transmits on the low carrier. The spectrum of its
upstream has its centre at the same 1850 Hz as after a full phase 2,
where our INFO1d asked for the low carrier (1829 Hz; the high carrier is
1920 Hz). A receiver on the high carrier never finds its S, and it
retrains. `sm56_v92_qc` fails if there is a retrain after short phase 2.

**Modem-on-hold.**

- We grant its own request (`AT+PMHR`, `sm56_v92_moh_grant`). It goes
  on hold and comes back with quick connect.
- It thinks that our request is a retrain, as the Agere modem does
  (`sm56_v92_moh_request`).
- It answers MHreq only at the start of its phase 2, in place of INFO0d
  (`SC_Phase2_Detect_Info_0` looks for the two).
- An MHreq sent at its retrain reversal gets an answer: MHnack with
  `+PMHT=0` (its default), and then MHcda disconnects the call. With
  `+PMHT` set, it waits for an accept from its platform (the handset,
  `MO_HoldStatePlatformInput`).
- After MHfrr, it does not connect again with V.8, and it expects a
  different signal after the silence.

Until it answers MHreq, it operates as a remote that does not know
modem-on-hold. Also, an MHreq sent at the reversal stops the original
library and the Agere modem (they think that it is our reversal). Thus,
we continue to follow the retrain.

**K56flex.** It is the only K56flex client that we have. `sm56_k56flex`
calls us with `AT+MS=K56FLEX` (we answer with `AT+MS=56` as the digital
modem, on the 8000 Hz line). It connects at 44000 bit/s down and 31200
up, with LAPM. To get this result, we had to do these items (refer to
[K56flex](dsp/k56flex-details.md)):

- a TRN1B that its own unused transmitter (`isdn_tx`) does not send;
- the 16-level Js, because its detector cannot accept the 4-level Js,
  and then 16-point TRN, MP and E from it in phase 4;
- its J of 24 bits (J and a robbed-bit octet), heard in time: it waits
  only approximately 0.75 s for our second R;
- the sign of our codes as its decoder uses them (bit 7 positive).

## Against the PCTel HSP soft modem

`half_pctel` runs the HSP soft modem of PCTel in user space:
pctel-0.9.7-9, "HSP56 MicroModem ... K2.4.7.V92.001 (03/26/2003)". It
uses the Intel AMR build of the data pumps (`lib/amr/dsp.a`) and the
controller (`lib/control.a`).

These objects need nothing from a kernel, but the Agere and SM56 modems
do. The two objects (`test/pctel/dsp.o` and `control.o`, the PCTel files
with new names) import only their hardware layer and the registers and
queues of the virtual UART. The harness supplies these in C, from the
`i8xxhal.s` and `vuart.s` of the driver.

- Its interrupt runs `dspMain()` and `modem_main()` one time for each
  block of codec samples.
- The pumps get the block with `RdASIC` and give their block with
  `WrASIC`, at the rate and block size that they set (7200 Hz and 24
  samples when idle).
- `GlobalTimer` is the time of the blocks in ms.

`ctest -L pctel -j8` runs the tests. `PCTEL_DEBUG=1` prints the
requests to the HAL (hardware layer), and the pump that the controller
asks for (`sharedRam` + 0x1c, + 0x20) against the pump in operation
(`curModem`).

**`+MS` read its stack.** The controller reads some of its stack before
it writes it. `+MS` (`plus_ms`) copies the name of the carrier (`ext_ms`
+ 0x1b) to its stack without the terminating 0. Then it compares the
copy with "V21" ... "V90", "V92", "K56FLEX".

- The name matched only when the byte after the copy was 0 by chance.
  Otherwise, `+MS` did not change the carrier.
- The harness clears the stack below its frame before each call into
  the objects (they use approximately 1 KB of it; it clears 8 KB). Thus,
  the copy ends in 0, as its designers intended.
- The data that was there before came from the calls of the harness
  itself. A print added to the harness, or the stack filled with 0,
  changed `pctel_v90_calls` from 50666 to 45333 each time.

When the name matches, `+MS=V90` and `+MS=V92` get the top rate from the
command (`ext_ms` + 0x86):

- Without a top rate (0, which is less than 28000), they set S37 to 0
  (automode) and keep the downstream at drn 14, 45333.
- `+MS=V90,1,300,56000` sets S37 to 14 (V.90), and the call trains at
  50666. The tests give the top rate.

**A missed INFO0d.** At some call starts, it does not hear our INFO0d,
and it sends its INFO0a again, unacknowledged, each 82 ms. In a sweep of
the call start over nine periods of the commands, one start did this,
with the codec noise of the harness at its 5 rms. At 0 or 2 rms, the
same start trains. We had no recovery for this outside quick connect,
and the call waited in phase 2 until the driver stopped. Thus,
`pctel_v90_calls` failed approximately one time in six full runs, where
the load of the machine moved the call start (the setup commands of the
driver come one period later when it answers late). Now, we send our
INFO0d again until the remote accepts one
([dsp/v34-details.md](dsp/v34-details.md)). `pctel_v90_lose_info0`
loses our first INFO0d intentionally.

| PCTel `AT+MS=` / ours | PCTel caller → open answerer | open caller → PCTel answerer |
|---|---|---|
| V34 / 34 | CONNECT (we receive at 24000) | CONNECT 24000 |
| V32B,0 / 132 | CONNECT 14400 | CONNECT 14400 |
| V90,1,300,56000 / 90 (V.90, the open modem digital) | CONNECT 50666/33600 | - |
| (its automode) / 122 | CONNECT 2400 | CONNECT 2400 |
| (its automode) / 22 | CONNECT 1200 | CONNECT 1200 |
| (its automode) / 23 | CONNECT 1200/75 | CONNECT 1200/75 |
| (its automode) / 21 | CONNECT 300 | CONNECT 300 |

Its CONNECT reports the rate of the DTE (115200). Against itself and
the SM56 modem, it connects at V.34 in the two directions (33600 with
SM56).

**Low speeds.** Its low-speed modulations operate in its automode (no
`+MS`), in the two directions, against our forced V.21, V.22, V.22bis
and V.23 modems. When it answers, it sends the unscrambled ones of
V.22bis after its answer tone. When it is forced to one of them
(`+MS=V21,0` and so on), they do not operate: when it answers, it is
silent after its answer tone. (These were the open failures while the
tests forced it. Our V.22bis answerer, which before needed 10 s for its
V.22bis caller, now connects in 8 s.) With V.8, it always offers V.34,
whatever the `+MS` value is.

**V.34 with it as the answerer** failed at first: each time, it
retrained phase 2 and did not answer our INFO1c with INFO1a. Its INFO
decoder (state in `g34`) never found the start of the frame. We went
from L2 directly into INFO1c. Thus, the first bit of the frame was a
phase change from our last control channel symbol before L1. We now
send two symbols of B first, as a phase reference. (This was not
important to the Agere and SM56 modems and the original library.)

**V.90** with it as the analogue modem connects at 50666/33600, µ-law
and A-law. Before, it retrained in phase 4, 200 ms into our TRN2d, and
the call continued at V.34:

- Its receiver measured TRN2d at 20 dB (it needs 32,
  `VpcmARxHandshake`). Later, it found no usable constellation
  (`VpcmSelectCon`). Its CPt already asked for a low value (drn 6).
- The cause was the harness. During V.90, the modem runs its codec at
  the "10287 Hz" of FreqTable, which is the 3429 baud of V.34 times 3,
  that is 72000 / 7 Hz. The harness ran it at exactly 10287 Hz, 125 ppm
  away from the 8000 Hz of the line.
- The sampling of a V.90 analogue modem must follow the clock of the
  network, and this modem does not track such an offset. At
  72000 / 7 Hz, its CPt asks for drn 14 and the call trains.

**No V.92 to test.** When we ask for V.92 (`AT+MS=V92`), it connects
with V.90, with no quick connect or PCM upstream offer, on the first
call and also on a second call. This build has V.92 only in its
controller:

- It accepts `+MS=V92`, but it keeps S37 (the modulation, `act[0x25]`)
  at 0, automode. Thus, V.8 selects V.90.
- The controller uses its V.92 path (`go_v8_v92`: its V.8 for V.92,
  then `set_dsp_interface_v92`) with S37=15 and S19=2. `+PQC` and `+PIG`
  are on by default (`V92QuickConnectFlag`, `V92UpStreamFlag`).
- But with `ATS37=15 S19=2`, it asks the DSP for modem 15. The DSP
  (`dsp.o`, "LCE-336V 2.15.4") knows only modems 2 to 14 (the switch of
  `initSys`; 14 is V.90). It sends a CM (with V.23 duplex added) and no
  INFO0a after it, and the call ends in phase 2.
- Also, the DSP has none of the signals of V.92 (QC1a, QTS, ANSpcm, the
  V.92 bits of INFO0a) and no PCM upstream transmitter (Ru, TRN1u, the
  precoder and prefilter). It has only the signals of V.90.

**Hanging up and calling again.** It answers ATH after +++ with NO
CARRIER. `interop_call` accepts this as the hang-up, as it accepts OK.
After the usual 2 s on hook of the driver, its second call stops in
phase 2. Its DSP never leaves the state after INFO0, and never changes
its codec to 9600 Hz for it. With 3 s or more on hook, it trains (in
real time, as the tests run it; `HANGUP_WAIT`). `pctel_v90_calls` makes
two calls with 5 s on hook between them: 50666/33600 the two times.

## Against the Conexant HSF soft modem

`half_hsf` runs the HSF soft modem of Conexant (the hsfmodem
7.80.02.06oem of Linuxant) in user space. It has three of the i386
objects of the driver, as they are in `test/hsf/`:

- its engine (`hsfengine`: the controller and the data pumps);
- its AC'97 ICH codec module (`hsfmc97ich`);
- its soft DAA module (`hsfsoar`).

It also has the `.inf` and country file of the modem, for its NVM (its
stored configuration).

**Linking.** CMake gives the `.ctors` and `.gnu.linkonce.*` sections of
each object a name for that object (`hsf_obj.cmake`). The reason is
that each object has the same C++ names, and otherwise the linker keeps
only one. It also makes the microsecond busy wait (`hsfmc97ich41_`) and
the print function (`hsfmc97ich57_`) of the ICH module weak symbols.
Thus, the harness can supply its own (below).

**What the harness provides.**

- The OS layer of the engine (`cnxthsf_7800206oem_Os*`): memory, timers
  on the clock of the line, I/O ports, the interrupt.
- Its NVM: the `HKR` lines of the `.inf` and the lines of the country in
  `hsf.cty` (`HSF_NVM=<dir>` reads them from a different directory).
- Models of the hardware that the modules operate:
  - the AC'97 bus master of an ICH (PCM in and out, pause and resume at
    the position where it stopped);
  - a secondary MC'97 codec (Conexant, 0x4358/0x5429) at 15999 Hz;
  - the SmartDAA behind register 0x7a of the codec. Its line side
    answers commands as the DAA library sends them (its type 0x12;
    refer to [hardware-details.md](hardware-details.md#conexant-ac97-codecs)).
    Thus, the library gives power to the DAA, sets up the line and puts
    it off hook.
- The `ComCtrl` interface of the modem is the DTE, on a pty.

**The TSC.** The engine reads the TSC (the time stamp counter of the
processor). The harness traps `rdtsc` (`PR_SET_TSC`) and gives the time
of the line at 1000 MHz. It adds 0.1 µs for each read while the line
does not move (otherwise a busy wait never ends). The ICH module waits
approximately 37 µs in that way approximately 80 times for each second
of the line, with 370 trapped reads each time. Most of the processor
time of the modem was used by those signals. The `hsfmc97ich41_` of the
harness moves the counter over the wait immediately, to the value that
those reads would give, with the same result (a call trains the same,
sample for sample). The HSF calls now need approximately a quarter of
the time that they needed before.

**Running it.** `ctest -L hsf -j8` runs the tests. Its settings:

| Variable | Effect |
|---|---|
| `HSF_DEBUG=1` | the modem's own messages, the port accesses the harness does not know, and the DAA library's own messages (through `hsfmc97ich57_`, which the harness provides: the ICH module's own is empty) |
| `HSF_DEBUG=2` | also every port access |
| `HSF_DEBUG=3` | also every SmartDAA access |
| `HSF_COUNTRY=<T.35 code>` | the country (the USA, 0xb5, by default) |
| `HSF_LSD_VERSION=<n>` | the simulated line side's version (5 by default) |
| `HSF_DAA_FILL=<byte>` | the line side's registers' starting value, to see which bits the library's writes keep |
| `HSF_ADC=<on\|off>:<channel>:<reading>,...` | its monitor ADC's readings on and off hook (none by default; see [hardware-details.md](hardware-details.md)) |
| `HSF_KEEP8=1` | keeps the "8 samples" flag (below) |
| `HSF_NVM=<dir>` | the `.inf` and `.cty` from that directory |
| `HSF_CD2_PLAIN=1` | `half_hsf_usb`: a Cadmus 2 board without the SmartDAA (and its `.inf`, `hsfcadmus2.inf`) |
| `HSF_INF=<file>` | `half_hsf_usb`: another `.inf` |
| `HSF_CODEC_RATE=<Hz>` | the codec's rate (`half_hsf_usb`: 16000) |

| HSF `AT+MS=` / ours | HSF caller → open answerer | open caller → HSF answerer |
|---|---|---|
| V34 / 34 | CONNECT | CONNECT |
| V32B,0 / 132 | CONNECT | CONNECT |
| V22B / 122 | CONNECT | CONNECT |
| V21,0 / 21 | CONNECT | - |
| V21, V23C, V22B, V32B (automode) / 34 (V.8) | CONNECT: V.21, V.23, V.22bis, V.32bis | CONNECT: V.21, V.23, V.22bis, V.32bis |
| V90 / 90 (V.90, the open modem digital) | CONNECT 52000/33600 | - |
| V92 / 92 (V.92, PCM upstream) | CONNECT 52000/41333 | - |

**The USB back end.** `half_hsf_usb` is the same modem on its USB back
end (`hsfusbcd2`, for the "Cadmus 2" USB bridge of Conexant: 0572:1300
and the others), in place of the ICH back end. The harness supplies the
`OsUsb*` layer of Linuxant and a model of the bridge (`hw/cadmus2.hpp`):
- It runs the scripts of the module (byte code that the module sends
  with vendor request 2: the registers of the bridge, the words of the
  codec, the registers of the line side).
- It sends their replies on the interrupt pipe.
- The samples of the line go in frames of 128 bytes (64 samples, 16 bit,
  16000 Hz) in each direction. A transmitted frame is complete when the
  codec plays it.

As a model of a SmartDAA board, its calls are the same as the calls of
the ICH (`hsf_usb_*`: V.21 to V.34, in the two directions). As a board
with only the codec (`HSF_CD2_PLAIN=1`), it calls at V.22bis and below.
Above that, its receiver fails, possibly because it does not have the
filters of the real codec, which the module expects.

Other HSF tests:
- our forced V.21, V.23, V.22 and V.22bis callers against its automode
  (`hsf_low_*`);
- V.90 with our INFO0d lost (`hsf_v90_lose_info0`);
- our echo canceller against the echo of our signal from the hybrid of
  its line, in V.90 and V.92 (`hsf_v90_echo`, `hsf_v92_echo`);
- our fast parameter exchange, which it does not have (no RM to our Rf).
  Thus, we renegotiate (`hsf_v92_fpe`).

**V.90's CJ.** V.90 with it as the analogue modem connects at
52000/33600 over µ-law and A-law. At first, it failed: we waited for its
CJ while it continued with INFO0a. It stops its CM early for CJ, and it
stops its carrier immediately after the last data bit of the third zero
octet of CJ. Thus, that octet has no stop bit, and our V.8 receiver
counted only two. Now, two zero octets, plus the start bit and eight
zero bits of a third, count as CJ (CM and JM never have two zero octets
in sequence).

**The ring buffer.** At first, the ICH module went past the end of its
ring buffer each 3.27 s. A word of its device (offset 0x280, set to 2
when the engine runs) makes it add 8 samples to each position that it
reads after sample 700, as for a codec that runs fast. The harness
clears that word before each interrupt (`HSF_KEEP8=1` keeps it).

**Its V.21 and V.23 answerers operate only after V.8.** When forced
(`+MS=V21,0`, `V23C,0`), it stays silent after ATA: no answer tone and
no mark for a minute. That occurs:

- against our caller, the callers of the Agere, SM56, spandsp and PCTel
  modems, and its own caller (all of which wait for the answerer, as
  V.25 tells them to);
- against a 980 Hz V.21 originate mark on the line from the start (by
  netsim, as a test);
- with a different country (`+GCI`), or with the rates given
  (`V21,0,300,300`). Also, `ATB` is not available.

The harness is not the cause. When forced, its V.22, V.22bis and
V.32bis answerers operate, and its V.21 caller operates (the same FSK
transmitter). The V.21 and V.23 answerers of its automode also operate,
through V.8.

In automode, it sends ANSam. When no CM comes, it sends its 1650 Hz
mark, but it stops the mark 2 s after the mark of the caller (also
against itself, with `+MS=V21,0` on the caller). Thus, our V.21 caller
(`AT+MS=21`) did not connect to it until our caller also used V.8 on its
ANSam. This is for V.21 only. Our V.22, V.22bis and V.23 callers connect
to it without V.8.

**The V.8 changes this led to.**

- The CM of our V.8 caller now offers V.22bis, V.23 and V.21, in
  addition to V.34 and V.32bis. A JM with one of these (the JM of the
  HSF modem, when it answers with `+MS=V21` and so on) gets CJ and that
  modulation, with the carrier of the FSK caller first (as the caller of
  the HSF modem does).
- When we answer, a CM with no modulation higher than one of these gets
  our JM of that modulation and, after CJ, that modulation.
- We also found this fault: the PCTel modem accepts our CJ only when the
  75 ms without signal of V.8 follows it (`dsp/dp_v34.cpp`, `V8C_GAP`).
  Before, our V.8 caller started the carrier of the agreed modulation
  immediately after CJ. The PCTel modem continued with JM until it
  stopped. Approximately half the calls connected by chance. The V.8
  pump did not clear the remaining part of the block that it ended in.
  The old data in the buffer of the host went out, sometimes a short
  time of V.21 space, which the PCTel modem accepted as the end of CJ.
  Our caller now keeps the 75 ms gap.

**Its V.32bis caller.** Its V.32bis (and V.32) caller became silent
after the AC/CA exchange of our answerer, and never answered our R1.
After the reversal that answers the CC of the caller, its own answerer
continues with AC for 256 symbols before the silence and S. Our
answerer stopped after 64, and the HSF modem did not hear our S. With
128 or more, it trains. We now send 256, as it does (this is not
important to the other modems).

**V.92 with PCM upstream** connects at 52000/41333, µ-law and A-law,
with quick connect (it sends QC1a already on the first call) and
without it (`+PQC=3`, `hsf_v92_pcmup_noqc`). At first, it failed in
phase 3: we waited for the reversal of its Su while it waited for our
Jp.

- Its Su, Su-bar and Su come as one short burst: Su for 144 symbols,
  Su-bar for only 24, then Su again until Jp.
- Its Su has the shape of Sd, period 6, but its energy is almost all at
  4000 Hz (with quick connect, all of it; without quick connect, the Su
  that it holds is a sine).
- Our receiver accepted only a sine of period 6 as Su. It looked for
  reversals between two 12-symbol windows 6 apart: too far apart for a
  Su-bar of 24 symbols.

Now, while we send Jd, a signal of period 6 with some of its energy at
1333 Hz counts as Su. The reversals of Su are the phase of each period
against the phase of Su.

## Against the Broadcom soft modem (Windows)

`half_broadcom` runs the V.92 soft modem of Broadcom for Windows,
`BCMSM.sys` 3.4.19. This is the WDM (Windows Driver Model) driver of the
"BCM V.92 56K Modem". Its controller, data pumps, V.42, V.44 and fax are
all in that one driver, as Dell supplied it for its Dimension desktops
in 2002 (refer to above). It runs on `interop_windows.cpp`, our own
Windows kernel.

### Why not Wine

Wine could not run it. The kernel of Wine (`ntoskrnl.exe`, master of
October 2026) has 726 of its 1716 exports as stubs, and its HAL has 71
of 92 as stubs:

- Its `KeInsertQueueDpc` puts nothing in the queue.
- `IoConnectInterrupt`, `IoGetDmaAdapter` and `KeSynchronizeExecution`
  are stubs.
- `MmMapIoSpace` returns NULL, and port reads return 0.
- START_DEVICE comes without resources.
- Timers run in real time on a thread pool.

A soft modem needs all of these, and here it must run on the clock of
the line.

### Our Windows kernel: `interop_windows.cpp`

`interop_windows.cpp` is a Windows kernel for the WDM driver of a soft
modem on a PCI device. The Broadcom driver was the first driver that it
ran.

A harness describes its device in `struct win_dev` (`interop_windows.h`):

- the files of the driver and the model of the INF;
- the PCI configuration space (from the `PciDevice` of the harness);
- the registers (the memory or port I/O of BAR 0, which includes the
  configuration ports of PCI), as calls;
- the interrupt line;
- the rate of the line, and how to move the line on while the kernel
  waits.

Its interface:

- `win_start()` loads and starts the driver and opens its port.
- `win_tick()`, called after each line sample, moves the time of the
  kernel.
- `win_serial_read()` and `win_serial_write()` are the reads and writes
  of the port.

All other items that the driver needs come from the INF: the registry,
the service, and the description, manufacturer and class of the device.
The IDs of the device come from its configuration space.

The kernel is part of a 32-bit program. It loads the PE image at its
base (0x10000), relocates it and binds its imports by name. The Broadcom
driver has 114 imports: 102 from `ntoskrnl.exe` (one of them data), 7
from `HAL.dll`, 3 from `WMILIB.SYS` and 2 from `ks.sys`, stdcall and
fastcall as in Windows. If the kernel does not have an import, the
program stops and shows the name of the import.

All items run on one thread, on the clock of the line:

- The interrupt time and the system time are the samples of the line.
  While the line does not move, each read adds 1 µs to them, because
  the driver does busy waits on them of some µs at a time.
- Timers and their DPCs (deferred procedure calls) run between samples.
  The ISR runs when the interrupt of the device comes up (for the
  BCM4212, a status bit that it enables).
- The system threads of the driver are coroutines (`ucontext`). Each one
  runs when the event or timer that it waits for is signalled.
- The kernel obeys IRQL: no interrupt at or above the IRQL of the
  device, no DPC at DISPATCH_LEVEL and above, and threads at
  PASSIVE_LEVEL only.
- The registry is in memory: the `AddReg` sections of the INF, as the
  modem class installer writes them, and `PortName`.
- fs points at a KPCR (a TLS entry of the GDT, set with
  `set_thread_area`). The driver checks its remaining stack against the
  limit of its NT_TIB (fs:8), which the kernel sets for each coroutine.
- `in`, `out`, `cli`, `sti`, `rdtsc` (`PR_SET_TSC`) and moves to and
  from control registers (the driver reads CR0 and CR4) cause a fault,
  and the kernel does them.

We are the remaining part of the device stack:

- Under the driver, we are the PCI bus driver: its PDO (physical device
  object), with the configuration space, the PnP IRPs that the driver
  sends down, and its resources.
- We are also the PnP manager: DriverEntry, AddDevice, and START_DEVICE
  with the memory BAR and interrupt of the device.
- Above the driver, we are the `modem.sys` of Windows. The INF makes the
  driver a lower filter of `modem.sys`. For us, the driver is a serial
  port:
  1. We open it (IRP_MJ_CREATE).
  2. We set it up with the serial IOCTLs (baud rate, line control,
     handflow, timeouts, DTR, RTS).
  3. We read and write it with IRPs that carry the bytes of the pty (the
     DTE of `half_broadcom`).

### The BCM4212 chip

The hardware of the driver is the BCM4212, the PCI modem chip of
Broadcom (14e4:4212, Dell subsystem 1028:0001). This build
(`opt-dell-wdm`) has codec code only for CodecType 0x15, 0x19 and 0x1c,
the types of the BCM4212. The INF gives 0x15. The driver also looks on
the PCI bus for AC'97 controllers (the ICHs that the other harnesses
emulate). But for the codec types of those controllers, its codec
operations are a stub that fails. Then the port does not open, with
STATUS_INSUFFICIENT_RESOURCES. Thus, `half_broadcom` emulates the
BCM4212.

There is no datasheet. Thus, the registers below show what the driver
does with them:

| Offset | Register |
|---|---|
| 0x00 | control: 0x1000 reset, 0x60 DMA on |
| 0x20 | interrupt status, cleared by writing a 1: 0x1000000 TX and 0x10000 RX (a descriptor done), 0x4000 and 0x8000 FIFO errors, 0x40 DAA |
| 0x24 | interrupt mask (the driver sets 0x101c000) |
| 0x38 | the DAA's serial port: the DAA register in bits 8 to 12, 0x2000 for a read, the data in bits 0 to 7 |
| 0x3c | the DAA read's result (bit 15: busy) |
| 0xb0, 0xb4 | two control words the driver sets to 0x80 (0x81 with `UseGPIOHook`); bit 2 is cleared in 0xb0 before each reset, and with `DaisyChainMode` it stays clear in 0xb0 and is set in 0xb4 |
| 0xc8, 0xca | 16 bit, both set to 0x3f at reset |
| 0x200, 0x204, 0x208, 0x20c | TX DMA: control, the descriptor ring's address, the last descriptor posted, status (0x1000 active or 0x2000 idle, with the current descriptor's offset) |
| 0x210 to 0x21c | the same for RX |

The registers are a 16 KB memory BAR, which the driver maps with
`MmMapIoSpace`. The kernel gives it a window that it cannot access.
Thus, each access causes a fault. Then the kernel does these steps:
1. It decodes the instruction.
2. It opens the window with the value of the register in position (from
   `half_broadcom`).
3. It single-steps the instruction (with the trap flag, TF).
4. At the trap after the instruction, it gives the device the written
   value and closes the window again.

The DMA engine is the engine of Broadcom, from the BCM4401 Ethernet (the
`b44` driver of Linux knows it). Its rings contain 8-octet descriptors:
a control word (the length in bits 0 to 12, 0x10000000 for the end of
the ring, 0x20000000 for an interrupt when done) and an address. The
samples are 16 bit.

- TX: 64 descriptors of 96 octets (6 ms), with an interrupt on the last
  descriptor. The engine stops at the descriptor that the driver posted
  last.
- RX: 128 descriptors of 4 and 92 octets in turn, end to end, with an
  interrupt for each pair.

The line side is a Silicon Labs Si3034 DAA behind the serial port of the
chip:

- Its PLL sets the sample rate (registers 7 to 9: 4 MHz · M1 M2 / (N1 N2)
  / 5120). It stays at 8000 Hz in the calls here, also for V.90.
- Bit 0 of register 5 is the hook.
- Register 12 is the state of the line: frame detect, and the loop
  current in its low four bits. 1111 there is an overload, and the
  driver hangs up immediately (it went off hook and immediately back on).
  The harness reports 1000, half the scale.

### How the driver starts the chip

This data comes from the code of `BCMSM.sys`, read with the disassembly
of REA, and from its register trace under `BROADCOM_DEBUG=2`, which
prints each access with the address of the instruction.

1. It maps the BAR and reads its settings from the registry: `SiSetup`
   (20 bytes of DAA setup; without it, it calculates the PLL itself for
   a 4 MHz clock), `UseGPIOHook`, `DaisyChainMode` and
   `OffHookWatchdogTimeout`.
2. Reset (CodecType 0x15): control (0x00) = 0x1001, a short wait, then
   0x2000; 0xb0 and 0xb4 = 0x80; 0xc8 and 0xca = 0x3f; the two DMA
   channels (0x200, 0x210) and the interrupt mask (0x24) to 0. Then the
   driver clears bit 2 of 0xb0, sets control = 0x1000 for ten reads of
   it, and sets control = 0x2000. The driver tries this for up to 100 ms
   until the DAA answers.
3. The setup of the DAA. The driver reads back each write and tries it
   again up to 25 times:
   - register 1 = 0x80 (soft reset);
   - the PLL (7 = 0x18, 8 = 0xff, 10 = 0, 9 = 0: 8000 Hz);
   - 6 = 0xe0; 5 = 0;
   - it waits for frame detect (register 12, bit 6), then reads register
     13. If bit 6 is set, the DAA is an Si3034 ("International"): the
     driver sets bit 1 of register 18 and writes the registers 16, 17
     and 18 of the country (for the US: 0x08, 0x00, 0x02). If bit 6 is
     clear, the DAA is an Si3035 ("USA Only"), which uses only the US
     (`AT+GCI=?` lists only B5).

   The model is an Si3035 by default. `BROADCOM_DAA=3034` makes it an
   Si3034. `broadcom_si3034_broadcom_calls` tests that: off hook,
   register 18 is then 0x03, not 0x01. But `AT+GCI=?` still lists only
   B5. The values of the other countries come from the registry
   (`SOFTWARE\BCMSM\Modem\Country\`), and this INF does not fill it.
4. DMA: the two channels off, then on (0x200, 0x210 = 1), the addresses
   of the rings (0x204, 0x214), the interrupt mask (0x24 = 0x101c000),
   the last descriptors posted (0x208, 0x218 = 0xff8), and
   control |= 0x60.

A DAA write waits until bit 15 of 0x3c is clear, then writes `reg << 8 |
value` to 0x38. A read writes `0x2000 | reg << 8`, waits again and gets
the low byte of 0x3c. The driver polls register 12 (the state of the
line) approximately 500 times a second.

Off hook, the driver sets bit 0 of register 18, then 5 = 0x01 (the
hook), 17 = 0x20, 20 = 0x60 and then 0, and 6 = 0xe3. On hook, it
reverses them: bit 0 of 18 clear, 5 = 0, 17 = 0, 6 = 0xe0.

CodecType 0x19 and 0x1c are other chips: a Sonics backplane similar to
the backplane of the BCM4401. The driver resets its V.90 core (core ID
0x807 at 0xffc) through 0xf98 (0x30001, then 0x30000). 0xf94 routes its
interrupt, and bits 16 and up of control are a serial clock divider
(from `BackplaneFreq` and `MasterClockDiv`). The BCM4212 has none of
this.

`half_broadcom` resamples between the rate of the codec and the rate of
the line (`BROADCOM_RATE`, 8000 Hz), and adds the noise of the codec
(`BROADCOM_NOISE`, rms 5).

Two special habits of the driver:

- It reads each setting first with the version of Windows XP added
  (`CodecFlags__5_1`), then without it.
- It reads `DisableTxRxDpc` with a handle that is not a key (an address
  in its device extension). Windows answers STATUS_INVALID_HANDLE, and
  our kernel does the same.

### Running it, and the results

`ctest -L broadcom -j16` runs the tests. Its settings:

| Variable | Effect |
|---|---|
| `BROADCOM_DAA=3034` or `3035` | the DAA model (3035 by default) |
| `BROADCOM_DEBUG=1` | the driver's calls of note, the hook, the rate, and the line's levels each second |
| `BROADCOM_DEBUG=2` | also every register, DAA and port access |
| `BROADCOM_DEBUG=3` | also every call the driver makes into the kernel |
| `BROADCOM_SYS`, `BROADCOM_INF` | other files for the driver and its INF |

| Broadcom `AT+MS=` / ours | Broadcom caller → open answerer | open caller → Broadcom answerer |
|---|---|---|
| V34 / 34 | CONNECT 33600 (we receive at 24000) | CONNECT 33600 / 24000 |
| V32B / 132, V32 / 32 | CONNECT 14400, 9600 | the same |
| V22B / 122, V22 / 22, B212 / 212 | CONNECT 2400, 1200, 1200 | the same |
| V23C / 23, V21 / 21, B103 / 103 | CONNECT 1200/75, 300, 300 | the same |
| V90 / 90 (ours as the digital modem), µ-law and A-law | CONNECT 50666/LAPM/V44 | - |
| V92 / 92 | quick connect from the first call; short phase 2 on the second (`CALLS=2`) | - |
| V92 with `+PIG=0` (PCM upstream) / 92 | phases 3 and 4, but no B1u: after a retrain, CONNECT 50666 (V.90) | - |

We receive its V.34 at 24000 bit/s because of our line probe on this
µ-law line (the same as with the Agere and SM56 modems). It receives
our V.34 at 33600. Its LAPM calls agree on V.44, and `broadcom_v44_*`
sends 60000 octets in each direction through it.

**Class 1 fax** at 8000 Hz (`broadcom_fax_*`): V.27ter, V.29 and V.17
(7200 to 14400) pass in the two directions.

**Its V.92** sends QC1a on each call, and its upstream stays V.34. By
default, it does not offer PCM upstream (`+PIG:1`). With `AT+PIG=0`, it
selects PCM upstream in INFO1a (`broadcom_v92_pcmup`):

- At first, phase 3 failed. Its Su is 96T long before Su-bar, but V.92
  has 144T. Our receiver accepted Su only after 96 coherent samples,
  which came in its Su-bar. Thus, it never saw the reversal. The modem
  then waited 4 s for our Jp and stopped. Su now counts after 48
  samples, and phases 3 and 4 are successful.
- After E2u, it sends no B1u (48 frames of scrambled ones). With our
  usual CPd (a prefilter of 64 taps), its output becomes periodic (48
  symbols) some samples after E2u. With no prefilter
  (`DSP_V92_PREFILTER=0`), it becomes silent: exact zeros, which no V.92
  precoder can make, because the constellations do not include zero.
  Neither of the two decodes as ones. We retrain, and the call connects
  at V.90 (50666/33600).

PCM upstream is possibly not complete in this build, because it is off
by default.

## Against the ESS soft modem (Windows)

`half_ess` runs the V.92 soft modem of ESS for Windows, `es56hpi.sys`
5.43.070. This is the WDM driver of the "ESS ES56H-PI Data Fax Modem".
Its controller, data pumps (fixed point and MMX), V.42, V.44, MNP and fax
are in that one driver, as ESS supplied it in 2003 (refer to above).

It runs on `interop_windows.cpp`, as the Broadcom driver does. It uses
some parts of the kernel that the Broadcom driver did not need:

- Its BAR 0 is ports, not memory. START_DEVICE gives it a port resource
  (`win_dev::io_size`), and its port accesses go to the harness. They are
  16 bit: the port is the port in dx, whatever the driver gives above it.
- 20 more imports: C library functions (`strstr`, `strncmp`,
  `_strnicmp`, `atoi`, `wcsstr`, ...), the 64-bit helpers of the
  compiler (`_allmul`, `_alldiv`, and `_allshl` and `_allshr`, which take
  edx:eax and cl), `DbgPrint`, `RtlInitString`, the fast mutexes, and
  `KeTickCount`, a data import (a KSYSTEM_TIME that the kernel moves
  forward each 10 ms of the line).
- AddReg lines of the harness after the lines of the INF
  (`win_dev::addreg`). With `ESS_DEBUG=2`, they set the `DebugLevel` of
  the driver, and its `DbgPrint`s print.

### The ES2838 chip

Its hardware is the ES2838 of ESS (PCI 125d:2838, the subsystem of ESS;
the model `ModemPV` of the INF), a host signal processing modem. The
chip has a codec, a sample FIFO and an interrupt, and the driver does
all other tasks. There is no datasheet. Thus, the ports below
(`hw/es2838.hpp`) show what the driver does with them:

| Port | Register |
|---|---|
| 0x00, 0x02 | an index and its register's data: the chip's registers below 0x100, its codec's above (0x14c the GPIO configuration, 0x154 the GPIO status, 0x15a the codec's rate in bits 12 to 15) |
| 0x04 | the FIFO: the driver writes four DAC samples, then reads four ADC samples, and again |
| 0x06 | the number of ADC samples in the FIFO (more than 0x1ff is an overrun) |
| 0x08, 0x0a | a DAA's registers behind a serial port (0x08 the command, bit 7 a read, bit 0 busy; 0x0a the data); this codec type leaves them alone |
| 0x0c | control: bit 0 the FIFO runs, bit 1 its interrupt |
| 0x0e | status: bit 1 the FIFO's interrupt, bit 0 the line's |

- The interrupt comes with 16 samples in the FIFO. The driver takes 12
  at a time (16 at its V.34 rates), a block each 1/600 s.
- The rate of the codec is in its register 0x5a (index 0x15a). The
  driver sets it from a table of the rates of its pumps:
  - 6 for 7200 Hz (V.8, V.21 to V.32bis, fax);
  - 5, 4, 3, 2 and 1 for 8229, 8400, 9000, 9600 and 10286 Hz (the
    symbol rates of V.34 at three samples a symbol, and 2400 baud at
    four);
  - 11 for 8000 Hz (call progress and V.90).
- Bit 9 of the GPIO status is the loop current. Off hook without it, the
  driver counts and then shows OVERLOAD (its over-current protection).

`half_ess` resamples between the rate of the codec and the rate of the
line (`ESS_RATE`, 8000 Hz), and adds the noise of the codec
(`ESS_NOISE`, rms 5).

### Running it, and the results

`ctest -L ess -j16` runs the tests. Its settings:

| Variable | Effect |
|---|---|
| `ESS_DEBUG=1` | the driver's calls of note, the codec's rate as it changes, the line's levels each second, the FIFO's state, and what goes to and from the DTE |
| `ESS_DEBUG=2` | also every port and register access (with the driver's address of each), and the driver's own debug output |
| `ESS_DEBUG=3` | also every call the driver makes into the kernel |
| `ESS_DUMP=<file>` | writes the codec's samples, DAC and ADC in pairs |
| `ESS_SYS`, `ESS_INF` | other files for the driver and its INF |

| ESS `AT+MS=` / ours | ESS caller → open answerer | open caller → ESS answerer |
|---|---|---|
| V34 / 34 | CONNECT 33600 (we receive at 24000) | CONNECT 24000 / 33600 |
| V32B / 132, V32 / 32 | CONNECT 14400, 9600 | the same |
| V22B / 122, V22 / 22 | CONNECT 2400, 1200 | the same |
| V23C / 23, V21 / 21 | CONNECT 1200/75, 300 | the same |
| B103 / 103 | CONNECT 300 (answered after it dials: see below) | CONNECT 300 |
| V90 / 90 (ours as the digital modem), µ-law and A-law | CONNECT 50666 (31200 upstream) | - |
| V92 / 92, as it comes | CONNECT 49333 (V.90) | - |
| V92 with `+PIG=0` (PCM upstream) / 92, µ-law and A-law | CONNECT 49333/41333 (our prefilter) | - |

These also pass: V.8 in its automode (V.34, with it as the caller), V.44
in the two directions (60000 octets), MNP 5 in the two directions, and,
as with the other peers:

- V.8 agreement on V.21, V.23, V.22bis and V.32bis in the two
  directions (`ess_v8_*`), and our forced V.21, V.23, V.22 and V.22bis
  callers against its automode (`ess_low_*`);
- our V.34 rate renegotiations in the two directions, the line 10 dB
  noisier after 10 s (a retrain), and the recovery of phase 2 from our
  lost INFO0a;
- V.90 with our INFO0d lost; our renegotiation (after its own to 26400
  upstream, which it starts at each V.90 connect); and the echo of our
  PCM signal 20 dB down (our echo canceller removes approximately
  31 dB);
- V.92 with the echo 10 dB down, PCM upstream on A-law lines, and our
  modem-on-hold request (it thinks that our tone B is the tone B of a
  retrain and retrains, as the Agere modem does);
- MNP 4 alone, with it as the caller.

**Class 1 fax** at 8000 Hz with it as the receiver
(`ess_fax_*_open_sends`) passes at all ten modulations. It shows CONNECT
a short time before it receives. At 300 bit/s, its first octet or more
are lost. Thus, its FSK calls send their data one second after CONNECT
(`DATA_WAIT=1`). Its V.29 at 9600 sometimes gets the first three octets
of the page wrong. The fax check accepts this (it finds the page by its
next 128 bits).

**Bell 103.** As the Bell 103 caller, it ignores an answer tone (2225 Hz)
that is already on the line when it dials, and it sends nothing. Thus,
`ess_103_ess_calls` answers 2 s after it dials (`DIAL`, `ANSWER_AFTER`),
as after rings. Then it calls at 300 bit/s.

### What it does not do here

- After it goes from the data state of a call to command state, its
  parser accepts no command. After `+++`, it shows OK. Then `ATI` gets no
  answer, and `ATO` goes out on the line as data. As the fax sender (the
  caller), it accepts the frames of the answerer and shows OK. But it
  does not accept `AT+FTH=3` after them, or `AT` (with our answerer and
  also with the answerers of Agere and Broadcom), until the call ends.
  Thus, there are no fax calls with it as the sender, and no
  modem-on-hold of its own (`AT+PMHR` needs the escape). A WAIT_ON_MASK
  kept in progress, as modem.sys does, does not change this.
- No Bell 212A: `AT+MS=B212` is ERROR (and `ATB1` with V.22 is not
  212A).
- Its V.92 INFO0a has short phase 2 off: no quick connect, with or
  without `+PQC=0`.
- When it answers with MNP (`+ES=3,0,6`), it frequently sets our
  direction to 14400 bit/s or less (7200 at some answer timings). Then
  it retrains up one or two seconds into the call, as an MNP 10 modem
  does. MNP 5 continues through the retrain. With MNP 4 alone (our
  `%C2`), it gives its DTE no CONNECT after the retrain. Thus, that test
  runs only with it as the caller.
- Plain V.34 answers get 33600 at most timings, but 14400 or 7200 at
  some: approximately one call in ten with `ESS_SYNC=0`, where the ATA
  comes at the time that the scheduler gives it. These were the
  intermittent limits of full-suite runs. The `ESS_SYNC` of the tests
  (the default, the 4800 samples of `PIPE_SYNC`) keeps them at a timing
  that gets 33600.

### Its PCM upstream

With `+PIG=0`, its INFO1a asks for PCM upstream. Phases 3 and 4 are
successful: Ru, TRN1u, Ja, TRN2u, SUVu, its CPu (6 constellations),
SUVu' and E2u. We had to change three items in our modem.

**The CPd's filter sections.** Its INFO1a has filter sections 0 (p1 and
z2 alone, Table 18/V.92). Our CPd had 4 taps of z1 and p2 in it (for the
Smart Link library, which has filters with all four). It never accepted
that CPd, and retrained after half a second. Our CPd now has z1 and p2
only where the INFO1a has them.

**Its points.** Then its points are different from the points of V.92 in
two ways. Our receiver now corrects the two (`v92.cpp`,
`data_symbol()`):

- Without precoding, its negative points are one grid step (16 G)
  farther out than the mirror of its positive points: they are on the
  even grid units. The negative points of the first 24 symbols show
  this. After that, we move them one step in.
- Its precoder rounds its feedback down to the grid unit below: it sends
  x = w - ceil(fb), where fb is the sum of p1 over its previous x. Thus,
  we receive the point minus a fraction of a unit. The decisions are at
  the edge of their regions, and B1u had approximately 5 % of its bits
  wrong. We make its x again from our decisions, and thus fb (near a
  unit, on one of the two sides, the side nearer a point: its sum is not
  exactly our sum). The grid mse of the first 48 symbols with this
  correction and without it tells which applies.

**The trellis frame's 4th symbol.** With our prefilter (in z2, the
precoder in p1), its points are on our levels. But on the 4th symbol of
the trellis frame, approximately half of the time, it sent 0. There the
classes are of 2M (eta = 2K + 2zM + parity), and its precoder goes past
the M (1 + sum |b|) points that the CPd of our prefilter had (42 for
M 36). There it sends 0 and not the last point. (The HSF and Smart Link
modems stay in them.) The CPd now has a minimum of 2M points, as the
design without the prefilter had. The HSF and Smart Link rates are the
same with them.

The PCM upstream of ESS now connects at 41333 (µ-law and A-law), with
modem-on-hold, and also with the echo 10 dB down (33333).

**The fallback.** If a PCM upstream with the prefilter fails on a modem
with p1 and z2 alone, we retrain and try one more time without the
prefilter or trellis decoding: precoding alone, with the corrections
above. ESS gets 32000 or 33333 in that way. Its points, which are not on
the levels of G.711, carry its quantization noise (which the mse of
TRN2u does not measure). Thus, the margin renegotiates down, each time
below the last rate (`drn_cap`), to approximately 30666.

### What it showed in ours

Our V.32 and V.32bis caller sent S for 256 symbols after R1, then S-bar.
V.32 (5.4.1) has S first for NT, the round trip delay that the caller
measured from its CC, and then the 256 symbols of the conditioning
signal. The ESS answerer stopped R1 at our S and never sent its S, TRN
and R3. (The caller of Broadcom, with NT, was successful.) Our caller
now measures NT (from its CC reversal to the reversal of the answerer,
minus the 64 symbols of the answerer) and sends S for NT + 256 symbols.

## Against the Intel 537EP soft modem

`half_intel` runs the core of the Intel 537EP driver for Linux
(`537core.lib`, "Release 4.65", of the Intel537EP 2.37.50.1 driver for
Fedora Core 1). The core has its controller (AT commands, V.42, V.42bis,
V.44, MNP, fax) and its data pumps up to V.92 (the pumps of Intel, on
IPP, the Integrated Performance Primitives of Intel). The core runs on
the host. Its "DSP" is a software comm RAM.

It needs no kernel. It needs only what the open glue of the driver (BSD
licensed) gave it, and the harness supplies this again in C++ (89
symbols):

- the queues and lines of the virtual UART (`uart.c`), with the DTE on
  the pty;
- the real-time service (`rts.c`). Each 10 ms of the line, it runs the
  timers of the controller (`linux_sys_isr_timer_ticker`, `acu_timer`).
  At the correct time, it also runs the RTS task `clm_bg()`, on its own
  stack (a `ucontext`). The `RSATaskYield()` of the tasker stops the task
  until the next time. In the kernel, the time slice of the task expired
  while the timer ticked. Here, no time passes in the task. Thus, it
  stops at each yield (`fax1_execute_command` waits in that way for
  `AT+FTS`);
- the precise dialer (pulse dialling, hook flash), a task on its own
  stack that waits with `linux_schedule()`, resumed each ms;
- the locks (none: there is one thread), but the resources of the core
  (`linux_RSAGetExclusive`, try-locks in `locks.c`) as they were: the RTS
  task can yield while it holds one, and `cp_dsp` gets none from a stub;
- the memory of the DMA rings (`AllocateDmaResources`), the country
  tables (`wwh_dflt.c`, the data of Intel without changes),
  `gettimeofday` and `rdtsc` on the clock of the line, and none of the
  persistent settings (the requests of the hamregistry time out
  immediately).

### The chip

The chip (8086:1080, `hw/i537ep.hpp`) is an Si3052 DAA with its own DMA.
The core gets to its registers through four of its own accessors on a
base pointer (the `iobase` of the driver). Thus, here they are plain
memory, and the chip does its work between the calls of the driver:

- two rings of 32-bit words (a sample in the low half of each), 32
  periods of 96 words: the ring of the ADC at 0x08 to 0x14 and the ring
  of the DAC at 0x18 to 0x24 (base, end, interrupt point, current word);
- the interrupt status at 0x04 (written back to clear: the handler
  acknowledges what it read);
- the rate of the codec at 0x37 (7200 to 16000 Hz; 9600 by default, and
  72000 / 7 for V.34);
- the clock lock of the codec and the loop current at 0x3c;
- the hook at 0x35.

The interrupt of each period runs the handler of the chip (`AfeIRQ`),
the handler of the comm RAM, and then the DSP tasklet `DspTask()`, with a
clean FPU as the kernel gave it (`fnsave`).

### IPP's processor

IPP selects the code of its pumps by `cpuid` (px, m6, a6, w7). The
harness makes it a Pentium 4 (`ippCoreGetCpuType` returns 6, w7) on all
hosts. `INTEL_CPU=<type>` selects a different type. The cycle counter of
IPP (`ipp_get_pentium_counter`, a bare `rdtsc`) runs on the clock of the
line. The two symbols are weak in the object, and its own `exit`
(empty), `index` and `pow` have new names (`CMakeLists.txt`). With the
other code paths (`INTEL_CPU=0`, 3, 5: px, a6, m6), its V.34, V.32bis,
V.90 and V.17 fax calls also pass.

`INTEL_DEBUG=1` prints the DMA of the chip and the rate of the codec.
`INTEL_DEBUG=2` also prints the messages of the core.

### The results

Its line runs at 9600 Hz (`INTEL_RATE=9600`). V.90 connects at 53333 in
that way, but at 45333 through the resampling to 8000 Hz. Against the
open modem (`ctest -L intel`):

- V.34 (33600), V.32bis (14400), V.22bis, V.22, V.21, V.23 and Bell 103,
  each forced, in the two directions;
- V.8 with the two modems in their automodes (V.34, with it as the
  caller); V.44 in the two directions (60000 octets); MNP 5 in the two
  directions;
- V.90 as the analogue modem, the two laws (53333/31200);
- class 1 fax at all ten modulations, in the two directions.

**Its V.92** (`+MS=V92`, the default): its INFO0a tells V.92, but its
INFO1a asks for V.90 (no PCM upstream), and the call is V.90
(`intel_v92`). With `+PIG=0`, it asks for PCM upstream. It completes
phase 3 (Ru, Ja, CPt, E1u), TRN2u and SUVu, and acknowledges our CPd
(SUVu'). But it sends no CPu: 0.4 s later it retrains (tone A), with or
without our prefilter, with a 16- or 64-state trellis. The call is V.90
after the retrain (`intel_v92_pcmup`).

**What it showed in ours.** Our V.32bis answerer reversed its CA to AC
immediately when it saw the CC of the caller. But 6.2/V.32bis puts the
reversal 64 symbols after. The caller measures the round trip from that
reversal. Thus, the Intel modem, as the caller, did not hear our
training at most alignments of the line (as with the Agere answerer at
6 alignments of 8). Our answerer now reverses 38 symbols after the
detection of its detector (which is 26 symbols late), as our caller does
its CC. It also sends AC until the tone of the caller stops (16 symbols
of silence, then S), not for 256 symbols.

## Against the USRobotics Courier (emulated)

The Courier V.Everything is a desktop modem of the 1990s that does its
work on its own processors. Thus, there is no driver to run:
`half_courier` emulates the full board and runs the firmware of
USRobotics on it.

- **The board.**
  - An Intel 80C186EB at 20.16 MHz runs the supervisor (the AT commands,
    V.42 and MNP, the link of HST, the start-ups) from a 512 KiB flash.
  - A TI TMS320C51 runs the data pumps. The supervisor downloads them
    from the same flash (a resident image and overlays: V.34, V.FC).
  - An ASIC connects them (a mailbox and six data lanes) and operates
    the hook, the lamps and the lines of the DTE.
  - A 93C66 holds the profile.

  `hw/courier.hpp` models them as the firmware uses them.
  `hw/i80186.hpp` and `hw/tms320c5x.hpp` are the two cores.
- **Time.** The samples of the codec drive all items. For each sample,
  the 80186 runs its part of 3.4 M instructions a second, its timers
  their part of 5.04 MHz, and the DSP its part of up to 20.16 M
  instructions a second, until its main loop is complete. The DSP sets
  the rate of the codec (7200 to 10266.67 Hz). `CodecLine` resamples to
  the 8000 Hz of the line (`COURIER_RATE`).
- **Short cuts.** The harness does the downloads of the supervisor to
  the DSP (through the loader of the C51, in its mask ROM) by a direct
  copy of the image into the memory of the DSP.
- **The files.** `IDSDL302.ROM` is ID_SDL v3.02, a widely distributed
  patch of the supervisor 7.3.14 and DSP 3.0.13 of USRobotics for the
  20.16 MHz external Courier. `c51-mask.rom` is the on-chip ROM of the
  C51, read from such a board (its first 2 K words are used). There is
  no release to compare them with. `courier-nvram.bin` is the `AT&F`
  and `AT&W` of the firmware (with `ATQ0 V1 X1 &H1 &R2`) on an erased
  part, plus the six settings records (words 94 to 102) that enable
  V.FC, V.34, x2 and V.90. `ATI7` then lists them.
  - `IDSDL302.ROM`:
    `49f4182cc961aef983ff43468b7b7e55c03205c9dba80e9689fe20aa6ff2ccc5`.
  - `c51-mask.rom`:
    `d57bc46e1bcd6d4dc8872b97bba2d98ba8fb6b8661440c566b534f0b3f82fac9`.
  - `courier-nvram.bin`:
    `4b21d145baffed2a686c3597845fd0eac1aa13ca7ec5b88391151940d5de63d9`.
- **Settings.** The DIP switches are `COURIER_DIPS` (3, 5 and 8 on:
  result codes, no auto answer, AT commands). The S registers of the
  Courier select its modulation: S27=4 no V.32, S34=128 no V.32terbo,
  S54=128 no V.8, S56=96 no V.34, 224 also no V.FC, S58=33 no x2 or
  V.90. `&N3` fixes the link at 2400 bit/s. `COURIER_DEBUG` (1 to 3)
  logs the traffic of the DTE, the hook, the rate of the codec and the
  messages of the mailbox. `COURIER_DUMP` writes the samples of the
  codec.

The calls (label `courier`):

| Test | Calls | Result |
|---|---|---|
| `courier_hst_pair` | Courier to Courier, HST | 16800/ARQ |
| `courier_v32t_pair` | Courier to Courier, V.32terbo | 21600/ARQ |
| `courier_v32_pair` | Courier to Courier, V.32bis | 14400/ARQ |
| `courier_hst_courier_calls` | Courier to the open modem, HST | 9600/300, HST's link |
| `courier_hst_open_calls` | the open modem to a Courier, HST | 16800/ARQ |
| `courier_v32_courier_calls` | Courier to the open modem, V.32bis | 14400, V.42 |
| `courier_v22_courier_calls` | Courier to the open modem, V.22bis | 2400, V.42 |
| `courier_v32_open_calls` | the open modem to a Courier, V.32bis | 14400/ARQ |
| `courier_v22_open_calls` | the open modem to a Courier, V.22bis | 2400/ARQ |

The calls of the open modem to a Courier found two faults in our modem:

- As the caller, it did not do the detection phase of V.42 (the LAPM of
  its V.92 quick connect, used for all calls), and the Courier as the
  answerer waits for the ODPs. The caller now does the phase on the
  modulations older than V.8 (an answerer without it sends flags, which
  end the phase). After V.8, on a leased line and on a clear channel,
  it goes directly to LAPM as before.
- The Courier ignores an XID command that has our zstd set ("ZST") after
  the V.42bis parameters in its private parameter group. It sends no
  answer, and the link continued without V.42. When our first XID gets
  no answer, the next XID offers no zstd (`link/lapm.cpp`).

In HST, the calls found that a Courier does not run V.42 after the
change from V.22bis. HST has its own link layer, which turns the
channels around when the side of the back channel has data (a Courier
keeps its data until then). The open modem now has that link
(`link/hst_link.cpp`), and its pump turns the channels when the link
tells it to. Refer to [HST's technical details](dsp/hst-details.md).
The two calls send data in the two directions (`STREAM=5`) while the
channels turn. A 30 kB transfer in each direction, past the wrap of
N(S), is also successful.

Not yet:

- **V.FC and V.34.** Between two Couriers, the two start (the overlay
  loaded, line probing at 10266.67 Hz), but training does not finish,
  and they retrain again and again: the emulation does not have some
  item there.

## The open modem on an HD-audio modem codec

`half_hda` is not the modem of a different vendor. It is the open modem,
on the modem core as `dmodemd -a` runs it, over an emulation of the
hardware of an HD-audio soft modem (the laptops of approximately 2005 to
2010). No closed driver in the suite operates HD audio. Thus, the driver
is our own, written to the interface that the `snd-hda-intel` and
`snd-hda-codec-si3054` drivers of Linux use
(`sound/pci/hda/patch_si3054.c`). The calls test that path: the verbs of
the codec, its registers and switches, the DMA of the streams, and the
modem on the clock of the codec.

- `hw/hda.hpp`: the reset and STATESTS of the controller, the immediate
  command interface (ICOI, IRII, ICIS), and one input and one output
  stream (SDnCTL, SDnSTS, LPIB, CBL, LVI, FMT, BDPL/BDPU) that move
  through their buffer descriptor lists in the memory of the process.
- `hw/si3054_hda.hpp`: the root node of the Si3054 (vendor ID 163c:3055;
  `HDA_VENDOR` sets a different ID) and its modem function group, with
  the registers as nodes 2 to 21 (verbs 0x9 and 0x1, 16-bit payloads):
  the ready bits of EXTENDED_MID, LINE_RATE, the stream numbers of
  LINE_LEVEL, the hook and line monitor of GPIO_CONTROL, CHIPID, and the
  frame detect of LINE_STATUS. Off hook, its ADC hears the line and its
  DAC drives it. On hook, the ADC hears the line only with the monitor
  on.
- `half_hda.cpp`: the driver.
  - It probes the codec. It refuses a vendor ID that is not in the list
    of the Si3054 driver, because no driver binds it.
  - It runs the init of the Si3054 driver.
  - It prepares the two streams: four periods of 10 ms, IOC (interrupt
    on completion) on each, and the stream format of the rate (48 kHz
    divided by 5 for 9600 Hz).
  - It sets the switches as the ALSA controls "Off-hook Switch" and
    "Caller ID Switch" do.
  - Each period captured is the next 10 ms of the modem. It writes the
    answer of the modem two periods ahead into the playback buffer, as
    `AlsaLine` does.

`HDA_CODEC_RATE` sets the rate of the codec (9600 by default, or 8000).

`ctest -L hda` runs its calls with the open modem:

- all modulations from V.34 to V.21, in the two directions;
- V.34 with the codec at 8000 Hz;
- V.90 (56000 down, the two laws) and V.92 (quick connect, PCM upstream
  at 37333), with it as the analogue modem;
- class 1 fax in the two directions at each modulation.

The data calls have the codec at 9600 Hz on the 8000 Hz line, through
the resampler of `CodecLine`. The PCM calls have the line at 9600 Hz
(`HDA_RATE=9600`), as the calls of the Intel modem have. The reason: the
gain of the resampler is a little different at each of its five phases.
The DIL (digital impairment learning) measures this as a different level
at each frame position, and then V.90 falls back to V.34 (no real PCM
line has such a thing). The fax calls are at 8000 Hz, with `half_open8`,
because the fax relay runs at one rate. `hw_test` checks the controller
and the codec alone.

## The line rung

With `NET_RING=<country>`, netsim connects the call in the same way as
the exchange of that country. Without it, netsim only carries the
samples.

- **The hooks.** The modems tell netsim their hook state: the
  `PipeModem` of `half_open`, and `half_hda`, write `1` off hook and `0`
  on hook to `MODEM_HOOK_FD`. `interop_call` makes that their fd 5, and
  the `NET_HOOK_A` and `NET_HOOK_B` of netsim.
- **Dial tone and digits.** A, off hook, hears the dial tone of the
  country (steady, or in its cadence: the dial tone of Italy is 0.2 s
  on, 0.2 off, 0.6 on, 1 off) until its first digit. Netsim hears the
  digits: DTMF on the line of A (`DtmfDetect`), or dial pulses. A pulse
  is a break by A of less than 0.5 s. A digit is the count of pulses
  when 0.3 s go by without a pulse. Netsim logs the number dialled.
- **Ringing.** `NET_RING_DELAY` seconds (2) after the last digit (or
  `NET_DIAL_TIMEOUT` seconds, 4, of dial tone if A dials no digit), the
  line of B, on hook, gets the ring signal, in the cadence of the
  ringing tone of the country. The ring signal is 25 Hz. It is 20 Hz
  with the North American ringing tone (440 + 480 Hz), 16 Hz in Japan,
  and 50 Hz in the networks of France. It is 24000 at its peak. During
  this time, A hears the ringing tone itself (its one or two
  frequencies, or one modulated).
- **Answer.** When B goes off hook, it answers, and after that the line
  is connected. Until then, each side hears only the signals above. A
  on hook for 0.5 s (longer than a dial pulse) ends the call.
- **Busy and congestion.** If B is already off hook, or with
  `NET_BUSY=1`, A hears the busy tone of the country and not the
  ringing (and B does not ring). With `NET_CONGESTION=1`, A hears the
  congestion tone (the lines or switches of the network are busy). The
  two tones continue until A hangs up.

### The countries

The countries are those of the ITU list of the tones in national
networks (Annex to ITU Operational Bulletin No. 955, position on 1 May
2010, after Recommendation E.180): 192 of them, in `dsp/tones.cpp`.
`dsp/tones_gen.py` makes that file from the Word file of the list. It
also tells how it selects a tone where the list gives more than one.
Where the list has gaps:

- The countries with no dial tone in the list get a steady dial tone at
  the frequency of their ringing tone.
- A "Re-order tone" counts as a congestion tone.
- A country with no busy tone uses its congestion tone (or the 0.5 s on
  and 0.5 s off of E.180). A country with no congestion tone uses its
  busy tone.

The names are the names in the list, in capitals
(`UNITED_KINGDOM_OF_GREAT_BRITAIN_AND_NORTHERN_IRELAND`), or an alias:
the country names of the core (`UK`, `USA`, `SOUTHKOREA`, ...) and some
short names.

### The ring tests

`ctest -L ring` makes a call between two open modems in each country. A,
set to that country, waits for the dial tone (`ATX4`) and dials 123 in
DTMF. B answers at its first RING (V.22bis, one second of data). Some
more calls:

- no dial tone wait and no digits (`ATX3`), with B that answers at the
  second ring (`ring_uk_second`);
- two calls (`ring_usa_calls`);
- V.34 (`ring_germany_34`);
- `half_hda` as the answerer (`ring_hda_answers`);
- `half_hda` with pulse dialling after its dial tone
  (`ring_hda_pulse_dials`).

Then there is a busy call and a congestion call in each country. A, set
to the country (`COUNTRY_A`, the `--country` of dmodemd, which accepts
all countries of the list), waits for the dial tone, dials 123, and must
end with BUSY (`EXPECT_FAIL`). On a steady tone (the congestion tone of
Ascension), it must end with NO ANSWER after S7. All 384 calls do this.

The modem accepts the busy and congestion tones of the country as busy
(`dsp/dp_callprog.cpp`, from `dsp/tones.h`): four cycles of the cadence
of one of the two, in 15% or 80 ms on each side. It does not accept the
ringing tone of the country as busy. Thus, the callers of the countries
with a ringing tone of 0.4 s on and 0.2 s off without end can also dial
with `ATX4`. `ring_tones_test` checks the same without the calls: the
busy and congestion tones of each country are BUSY to a modem in that
country, and its ringing tone is not.

The dial tone check of the modem (the 350, 425 and 440 Hz filters of the
call progress pump, which also pass 400 and 450 Hz and the pairs) needs
1 s of dial tone. A dial tone in a cadence also counts: the modem adds
its tone across gaps of up to 1 s. While it hears one, the modem waits
2 s longer than S6 for it.

`ring_tones_test` also checks the ring signal of each country without
the calls. Through `RingDetect` to the core, it must give a RING for
each of its rings, and auto answer at S0=2. For that, the ring check of
the core (`core/modem.cpp`) does these things:

- It accepts a burst of four pulses or more (0.3 s of a 16 Hz signal)
  as a ring.
- It accepts a ring that continues when it is checked (2.5 s after its
  first pulse) as a ring.
- It accepts ringing with no gap of 1.5 s (2 s on and 1 off in Egypt)
  as a ring each 6 s.
- It keeps a ring in S1 for 8 s. Thus, a cadence of up to 10 s (1 s on
  and 9 off in Serbia) rings in time.

The 12.5 s cadence of Tuvalu is longer. There, each ring counts as the
first, and only S0=1 answers.

## Adaptive answer

`ctest -L adaptive` runs calls into the open modem, which answers with
`AT+FAA=1`. That is adaptive answer: a data answer, in which the modem
finds if the call is a fax or a data call
([at_commands.pdf](at_commands.pdf)).

- **Fax senders.** The class 1 fax sender of each modem calls it, with
  it as the answerer in fax mode (and the sender of the open modem also
  with it in data mode). The answerer must show FAX before its CONNECT
  (`ANSWER_B`). Then the test checks the page.
  - The open modem, the library of Smart Link, and the Broadcom, Intel
    and HD-audio modems are recognised by their CNG (calling tone).
  - The Agere and Conexant modems stop their CNG immediately when they
    hear our ANSam. The short burst also identifies them.
  - The sender of the open modem without CNG (`DSP_FAX_CNG=0`), silent
    until the DIS, into fax and into data mode: FAX at the end of the
    automode chain (after the 3 s of Bell 103).
- **A V.8 fax call.** The V.8 of spandsp with the call function of a fax
  machine (`SPANDSP_V8_CALL=4`), into data mode: FAX, and no data call.
- **Data callers.** The Agere modem at V.34 and V.22bis, the Smart Link
  modem at V.34, the spandsp modem at V.22bis and V.21, the SM56 and
  Conexant modems at V.90, and the open modem at V.92, in data mode and
  in fax mode: DATA, then the data call.
- **Voice mode.** The open, Agere and Conexant fax senders call the open
  modem in voice mode, on the line with `+FAA=1` (`ANSWER_VOICE`):
  `<DLE>c`, FAX, the class 1 answer on the call, then the page. (The
  library of Smart Link sends no CNG on a silent line. Thus, it is not
  one of them.)
- **Slow legacy callers,** silent until they hear their own answer
  signal (the Agere modem forced to Bell 103, V.21 or V.22, without
  V.8), into fax mode with the full automode chain: each one answers its
  stage, and the result is DATA.

## MNP

The Agere, SM56, PCTel, HSF, Broadcom, Intel and ESS modems have MNP 2
to 4 and MNP 5 (the library of Smart Link and spandsp have none). With
their defaults, they all select V.42 with us.

When forced to MNP, each one connects with MNP 4 and MNP 5 in the two
directions (`ctest -R mnp`, 20000 octets in each direction):

- When the peer calls with MNP alone (`+ES=4,4,6`), our default
  answerer accepts the LR of the peer in the detection phase.
- When the peer answers with MNP alone (`+ES=3,0,6`), the peer accepts
  our LR (we call with `\N5`).

Special behaviours:

- The PCTel modem accepts no LR that offers N401 256. Our LR offers 64,
  with the 256-octet optimization, as their LRs do.
- The SM56 modem sends its LR 1.1 s after the connection. Our answerer
  with MNP waits three times T400 for it.
- The HSF modem sends LTs without information with MNP 5.
- None of them has MNP 10 or 10EC.

Refer to [link-details.md](link-details.md#mnp) for MNP, and for what
MNP 10EC would need.

## V.80

With the `V80=1` of `interop_call` (`interop_call.h`), the data of the
call is V.80 frames:

- The two DTEs ask for Synchronous Access Mode (`+ES=6` to call,
  `+ES=,,8` to answer).
- After CONNECT, each DTE goes to the framed sub-mode (`<EM><flag>`). It
  sends a warm-up frame that does not have to arrive, then 20 frames
  (`V80_FRAMES`) of 1 to 250 octets, which contain all octet values.
- The other DTE must get each frame complete, ended by `<EM><flag>`.

The calls are V.34, with our transmitter at a maximum of 28800 bit/s:
V.80 has no error correction, and a renegotiation in the middle (the
renegotiation of the SM56 modem, from 33600) causes lost frames.

The tests (`ctest -R _v80_`), and what each modem does:

| Modem | `+ES=?` | FCS (`+ESA`'s `<crc_type>`) | Calls | What its DCE does |
|---|---|---|---|---|
| Agere | `(0,1,2,3,4,5-6),...,(0-3,4,5,6,7-8)` | none, FCS-16 | both ways, each FCS | It drops the FCS it checked, but counting the octets of its shielded output: an FCS octet 19h or 99h, shielded, leaves a stray EM before `<EM><flag>` (the frame itself arrives whole). With FCS-32 its DCE sends nothing and gives its DTE nothing |
| Broadcom | the same | none, FCS-16 | both ways, each FCS | The same V.80 code as Agere's: the same answers and the same stray EM |
| SM56 | `(0-4,6),(0-4),(0-6,8)` | none, FCS-16 (`+ESA:(0),(0),(0-1),(),(0-1),...`) | it calling with FCS-16; answering with none | It gives its DTE the received frames with their FCS (two octets more). Answering, after any `+ESA` command it no longer answers. Its receiver starts hunting only with its own first frame |
| Intel | `(1-4,6),(0,2-4),(1,2,4-6,8)` | none only | both ways, its reception only | It receives as it should. Its transmitter sends 16 1s in place of each frame's last two octets (an FCS field with no FCS), which a receiver takes for an abort |
| HSF, PCTel, ESS | no 6 or 8 | | none | No Synchronous Access Mode (ESS accepts `+ES=6` and makes a plain call) |

All receivers accepted the frames of our transmitter complete, with
each FCS that it offers (our FCS-16 is the same as theirs). Our receiver
accepted their frames complete, but not the frames of Intel.

You can make the checks less strict for each side:

- `V80_STRAY_A=1` (`_B`) lets the DTE of that side get the unwanted EM.
- `V80_EXTRA_A=2` (`_B`) lets it get the FCS after each frame.
- `V80_ONLY=A` (`B`) checks only the frames of one side.
- `V80_PAD_A=<octets>` (`_B`) puts octets after the frames of that side.
  This is for the two lost last octets of Intel: with `XY` after each
  frame, the frame itself arrives complete.

While online, `half_intel` gives the Intel controller the characters of
the DTE when they come, as the interrupt of its serial port did. When
only its background task gave them, each 15 ms, its V.80 transmitter
had no data in the middle of frames.

### The transparent sub-mode's SYN hunt

`V80=2` keeps the two DTEs in the transparent sub-mode, with the
`<trans_idle>` of `+ESA` set to 1 (the 8-bit SYN, 16h) or 2 (16h then
96h). Each DCE sends the SYN when idle. The receiver of the other DCE
must find it and give its DTE octets aligned on it, with the SYN first,
from the CONNECT.

Then each DTE sends 3000 octets. The other DTE must get them complete,
with only the SYN idle before them. Then, `V80_HUNT=1` makes the DTE of
A send `<EM><hunt>`, and the DTE of B send 1000 more octets. Without the
hunt (`<trans_idle>` 0), our DTE gets `8a 76 b3 ff ...` before the SYN,
and the check fails.

The tests (`ctest -R _v80_syn`):

| Modem | `+ESA=?` | Calls | What its DCE does |
|---|---|---|---|
| Agere, Broadcom | `(0-2),(0,1),(0,1),(0,1),(0-2),(0,1),(0-255),(0-255)` | both ways, both SYNs | It finds our SYN, and we find theirs. Its DTE gets DC1 and DC3 shielded, and it loses the first of two octets in a row that it shields: `13 13` and `99 13` arrive as `<EM><t4>` alone (`V80_PAIR_LOST`: the octets sent to it have no such pair). The Agere modem calling, with the 16-bit SYN, gives its DTE the idle from a `<syn2>` on (`V80_IDLE_FIRST`); with octets of ours that start with the SYN, it gives the SYN first |
| SM56, Intel | `<trans_idle>` 0 only | none | |

Two open modems run the same SYN hunt calls (`open_v80_syn1`,
`open_v80_syn2`).

## The call's statistics in the middle of a call

`EXPECT_MIDCALL_A=<regex>` (`_B`) checks the answers of a modem to its
`MIDCALL` commands, as `EXPECT_A` checks its log.

`open_v90a_stats` escapes the two D-Modems in V.90 data, on a network
with a 3 dB pad, a robbed bit and the codec of the analogue modem
100 ppm fast, for `ATI11`, `AT&V1` and `AT#UD`. It checks:

- the symbol rate and carrier of each direction (8000 and PCM
  downstream, 3429 baud and 1959 Hz upstream);
- the trellis code of the upstream, on each side;
- the clock 100 ppm off as each modem sees it (-100 for the analogue
  modem, because the network is slower; +100 for the digital modem);
- the pad and the robbed bit as the DIL of the analogue modem found
  them;
- the same in the keys of Unimodem (the `DIAG` lines of `AT#UD`), keys
  20 and 21 for each direction: the `C` of V.34 upstream and the `E` of
  PCM downstream;
- the Rockwell lines of `AT&V1`: the termination `NONE` while up, the
  last receive rate, the robbed bit as an `RBS Pattern` mask and the
  `Digital Pad` of 3dB.

## Retrains and renegotiations on request

`MIDCALL_ATO_A=<cmd>` (`_B`) makes the mid-call return to online of
`interop_call` use that command, not `ATO`: `ATO1` retrains, and `ATO3`
renegotiates the rate. `ctest -R dte_` runs them with `STREAM=25` and an
escape after 6 s:

- V.34 (`AT+MS=V34`) retrains, in the two directions, against Agere,
  Broadcom, SM56, PCTel, HSF, Intel and ESS
  (`<peer>_34_dte_retrain_open_calls`,
  `<peer>_34_dte_retrain_<peer>_calls`). Also renegotiations with us as
  the caller to Agere and Broadcom (`<peer>_34_dte_reneg_open_calls`).
  Each peer answers, and all the data arrives.
- Between two of our modems: V.90 as the analogue modem
  (`open_v90a_dte_retrain`, `open_v90a_dte_reneg`) and as the digital
  modem (`open_v90_dte_reneg`); and V.92 with PCM upstream, as the two
  (`open_v92_dte_reneg`, `open_v92a_dte_reneg`, `open_v92_dte_retrain`).

The Agere, Broadcom and HSF modems answer the tone B of our retrain with
tone A plus an 1800 Hz tone of the same strength. Until the tone
detector accepted this ([dsp/v34-details.md](dsp/v34-details.md)), our
retrains with them failed with no tone A.

## The calls' options

**`ANSWER_AFTER=<s>`** gives the answerer its ATA that time after the
ATD of the caller, because a real line rings only after the number is
dialled. The quick connect tests dial (`DIAL`), and the Agere modem
waits 2 s off hook before its digits. If we answer immediately, our
ANSam ends before it listens.

**`MODEM_REC=<prefix>`** makes `half_open` and `half_smartlink` record
their line input and output (`<prefix>.<name>.in` and
`<prefix>.<name>.out`, where the name is the name in `MODEM_NAME`:
`modemA` and `modemB` in an interop call).

**`DBG=<level>`** sets the debug level of the modems in the interop
calls (3 by default).

**`WAV=<file>`** makes `interop_call` save the call as audio: a stereo
16-bit WAV at 8000 Hz of the line where it goes through the simulated
network (after G.711; A, the caller, to B on the left, and B to A on the
right), from the first dial to the end of the data (or of the call that
failed). It needs `NET` (netsim writes the file, `NET_WAV`). With
`CALLS`, the calls between them are also in it. For example:

    NETSIM=build/dsp/test/netsim NET=9600:8000:ulaw SMARTLINK_V92FULL=1 \
      WAV=call.wav build/dsp/test/interop_call \
      build/dsp/test/half_smartlink build/dsp/test/half_open8 "ATX3;AT+MS=92"

**The sync settings** make a call repeat exactly from run to run:

| Variable | Harness |
|---|---|
| `PIPE_SYNC=<samples>` | `half_open`, `half_smartlink` |
| `AGERE_SYNC=<samples>` | `half_agere` |
| `SM56_SYNC=<samples>` | `half_sm56` |
| `SPANDSP_SYNC=<samples>` | `half_spandsp` |
| `PCTEL_SYNC=<samples>` | `half_pctel` |
| `HSF_SYNC=<samples>` | `half_hsf` |
| `BROADCOM_SYNC=<samples>` | `half_broadcom` |
| `INTEL_SYNC=<samples>` | `half_intel` |
| `ESS_SYNC=<samples>` | `half_ess` |
| `HDA_SYNC=<samples>` | `half_hda` |

With them, the modems accept the commands of the DTE only each that
number of line samples (data, after the connection, goes in
immediately). They run their timers between half frames, and keep time
by the line. Without them, the scheduler decides where the commands
meet the line.

A data call sets `PIPE_SYNC=4800` if it is not already set. It also
sets the `<NAME>_SYNC` of each peer to the value of `PIPE_SYNC` if that
is not set (`interop_call.cpp`, `data_call()`). A fax call sets
`PIPE_SYNC=192` (refer to [Smart Link's fax](#against-the-smart-link-soft-modem)).
`0` sets the sync to off.

With `PIPE_SYNC`, `half_open` and `half_smartlink` also run in real time
while on hook, with their line in operation from the start (the lines of
the other harnesses always operate in that way). Thus, the commands
meet the samples that they would meet in real time, also when the call
itself runs without pacing (refer to [testing-details.md](testing-details.md)).

`AGERE_SYNC_PHASE` (the `<NAME>_SYNC_PHASE` of each peer) moves that
side in the period, to try the alignments one at a time.
