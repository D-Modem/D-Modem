# Interop tests against other soft modems: technical details

This is the technical companion to [Interop against other soft modems](interop.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

## The harnesses

The interop tests make calls between the open modem and other soft
modems:

- Smart Link's soft modem, running on our modem core;
- the Agere, SM56 (Motorola), PCTel, HSF (Conexant) and Intel soft
  modems, each in user space on an emulation of its hardware;
- Broadcom's and ESS's Windows drivers, on a Windows kernel of our own
  and an emulation of their chips;
- spandsp's modems;
- the open modem itself, on an emulated HD-audio modem codec.

[Building and testing](testing-details.md) describes the call driver
(`interop_call`), the network simulator (`netsim`) and how the calls
keep time.

Each modem has a harness program, `half_<name>`. A harness reads the
line's audio on file descriptor 3, writes its own audio on file
descriptor 4, and acts as the modem's DTE (the data terminal, the
computer side) on a pty. Each modem also has its own ctest label, so
`ctest -L <label> -j16` runs its calls.

### The shared program: `modem_half.hpp`

The harnesses of the closed modems and spandsp's are C++ classes built
on `dsp/test/modem_half.hpp`. `half::HalfModem` is the program itself.
It:

- reads its settings from the environment: `<NAME>_DEBUG`, `<NAME>_RATE`
  (the line's sample rate, 8000 by default), `<NAME>_SYNC` and
  `<NAME>_SYNC_PHASE` (see [the calls' options](#the-calls-options));
- opens the pty;
- runs the line one sample at a time, and serves the DTE every 48
  samples.

A harness derives from it and supplies three things: the modem's start
(`start()`), its handling of a line sample (`line_sample()` or
`line_block()`), and its DTE service (`dte_poll()`).

### The emulated hardware: `dsp/test/hw/`

The hardware a harness emulates is in `dsp/test/hw/`, one header for
each kind. Each is a mixin: a class template that the harness also
derives from, with the harness as its template parameter. That way,
whatever the hardware needs from the board it sits on is provided by the
harness itself. For example, the ICH controller reads and writes its
codec's registers through the harness's `codec_read()` and
`codec_write()`, and the x86 traps carry out their `in` and `out`
instructions through its `io_in()` and `io_out()`. The SM56 harness is
declared as:

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
| `si3054_hda.hpp` | a Silicon Labs Si3054 HD-audio modem codec: its root node and modem function group, its registers as nodes 2 to 21, the hook and the line monitor (`Si3054Hda`) | HDA |

Each mixin's members carry its name (`ich_sample()`, `pci_cfg`,
`si3034_rate`, ...), so a harness can derive from any combination of
them without name clashes.

`CodecLine` puts a windowed sinc resampler in each direction and adds
noise to the codec's input, because a real line is never digitally
silent. The noise is uniform, the same on every run, and its rms is 5 by
default (about -76 dB of full scale); `<NAME>_NOISE` sets another.

## The peers' binaries

The vendor modems are their own binaries, as their Linux drivers shipped
them, in `dsp/test/<peer>/`. They are i386 objects, which the harnesses
link in (built `-m32`). Broadcom's and ESS's are their Windows drivers,
which `half_broadcom` and `half_ess` load at run time.

Each file is the file of a known release, compared byte for byte with
that release wherever it can still be obtained. The files belong to
their owners and are licensed by them, not under this repository's terms
([dsp/test/NOTICE.md](../dsp/test/NOTICE.md)).

| Peer | Files | Release | Built | Byte-identical to |
|---|---|---|---|---|
| Smart Link | `smartlink/dsplibs.o` | slmodem-2.9.11-20110321 | 22 Sep 2005, GCC 3.4.2 | `modem/dsplibs.o` of Debian's `sl-modem_2.9.11~20110321.orig.tar.gz` |
| Agere | `agere/agrsm_core.o` | Agere SoftModem 2.1.40, from agrsm-20070804 | 16 Nov 2005, GCC 3.3.5 (Debian) | none to be had (below) |
| SM56 | `sm56/sm56.ko` | Motorola sm56-06.09.05 for Fedora Core 3 (2.6.9-1.667) | 17 May 2005, GCC 3.4.2 (Fedora Core 3) | `usr/tmp/sm56.ko` of `sm56-06.09.05-1-fc3-2.6.9-1.667.i686.rpm` |
| PCTel | `pctel/dsp.o`, `pctel/control.o` | pctel-0.9.7-9 (Linux driver K2.4.7.V92.001) | pumps 21 Jun 2001, GCC 2.95.3; controller dated 26 Mar 2003, GCC 2.96 (Red Hat 7.1) | `src/lib/amr/dsp.a`, `src/lib/control.a` of pctel-0.9.7-9-rht-6 |
| HSF | `hsf/hsfengine.o`, `hsfmc97ich.o`, `hsfsoar.o`, `hsfmc97ich.inf`, `hsf.cty` | Linuxant hsfmodem-7.80.02.06oem | 10 May 2010, GCC 3.4.6 (Red Hat) | `modules/imported/<name>-i386.O`, `nvm/inf/hsfmc97ich.inf`, `nvm/inf/hsf.cty` of `hsfmodem-7.80.02.06oem.tar.gz` |
| Intel | `intel/537core.lib`, `intel/wwh_dflt.c` | Intel537EP 2.37.50.1 for Fedora Core 1 SMP (core "Release 4.65 - Jan 17 2004") | 17 Jan 2004, GCC 3.2 (its IPP parts Intel C++ 7.0 Beta) | `coredrv/537core.lib`, `coredrv/wwh_dflt.c` of `intel-537ep_secure-2.37.50.1-fc1-smp.tgz` |
| Broadcom | `broadcom/BCMSM.sys`, `broadcom/BCMSMwxp.inf` | BCM V.92 56K Modem 3.4.19, Dell's "V.92 Soft Modem (Data Fax) Driver" A05 | 26 Jul 2002, Microsoft linker 7.0 | the hashes in the package's catalog, signed by Microsoft (WHQL, 31 Jul 2002) |
| ESS | `ess/es56hpi.sys`, `ess/mdmzn.inf` | ESS ES56H-PI modem driver 5.43.070 (`ess54370.exe`, for Windows 98 to XP) | 24 Mar 2003, Microsoft linker 5.12 | the hashes in the package's catalog, signed by Microsoft (WHQL, 2 Apr 2003) |
| spandsp | (the system's libspandsp) | spandsp 0.0.6 (release 20110122) | | |

The files' SHA-256 hashes, and what each was compared with:

- **Smart Link.** `dsplibs.o`:
  `1f3e56d0dfae1a6aaf4eb6fcc4875a4524905e010d5758114cde288b3cf0b379`.
  The tarball
  (`cea496e9c34a16cf347124b27ed673f3a6e87089f83e15ab5e0b0365a56fd253`)
  is at `https://deb.debian.org/debian/pool/non-free/s/sl-modem/`. The
  library came with slmodemd 2.9.11 in D-Modem's first commit.
- **Agere.** `agrsm_core.o`:
  `a00a7fe7a21c2b20a353bf373042c3e30599ff4a9238a061acb494bb559e171c`.
  - It says "Agere SoftModem Version 2.1.40" and "Built on 11/16/2005
    16:43:35", and of its PCM module "AGERE PCMLL V2.1.37.105, seed =
    1220, 2/1/2004".
  - agrsm-20070804 is a dated package of the linmodems "sv92" series
    (from linmodems.technion.ac.il, now gone), which carries Agere's
    2.1.40. Its successor, agrsm-20080203, answers `ATI` with the same
    version.
  - Agere's own builds of 2.1.40 for Mandrake, Red Hat and SuSE
    (December 2005, `agrmodemlib.o` on the Kaiomy Pinball-C driver CD at
    archive.org) have the same 3132 global symbols. But they were built
    with other compilers, and their `LXHardwareInfoCreate` and
    `LXHardwareInfoDestroy` are global.
  - In ours those two symbols are weak, because the harness provides its
    own (the package's Makefile weakens them). The object was therefore
    rewritten after Agere linked it, and no file of Agere's matches it
    byte for byte.
- **SM56.** `sm56.ko`:
  `a757fd85bc27cc4bd816073efa9d03591307973de5c5aabcb37e0bb225ba516a`,
  with `vermagic=2.6.9-1.667 686 REGPARM 4KSTACKS gcc-3.4`, "SM56 Rel.
  6.09 Build 05" and "Copyright 1998-2004, Motorola Inc.". The RPM
  (`1f24ba64dae3901de3fe497ec47f6490b7756558cd9ec26f568cc475b0da13d0`;
  vendor Motorola, built 2005-05-17, its checksums good, unsigned) can no
  longer be obtained: Motorola gave it only to distributors and OEMs, and
  its copies at linux-xp-brasil.com.br and linmodems.technion.ac.il are
  gone.
- **PCTel.** `dsp.o`:
  `15d8e5ab1ac4019a029fd9e562e0e74b3f233aa3ab42621334506721e66f82b1`;
  `control.o`:
  `38d46cabd67f675f8c011afd4725a41a1a813075bae4b516c94cda1b4dfec69d`.
  - These are upstream's `.a` files renamed (they are plain objects).
  - The pumps' `timeStamp` is "Jun 21 2001,14:27:03". The controller says
    "Linux Driver K2.4.7.V92.001 (03/26/2003)", and of the DSP "LCE-336V
    version .2.15.4.".
  - The copy compared
    (`f1304bbcefd1b88dc1733421f7850598334eaf62c5b132c8d129af3405d1ddfb`)
    is
    `https://archive.org/download/pctel-0.9.7-9-rht-6_for_Ubuntu-2.6.15-23-386/`:
    Robert Thornburrow's 0.9.7-9-rht-6 of 2006, as a third party
    uploaded it. Its ChangeLog changes no binaries after 0.9.7-8, so
    0.9.7-9 and its rht releases carry the same two files.
- **HSF.** `hsfengine.o`:
  `6d570356f37e62153374f2f50b5d3a3e4b1246285199cbba2aa7d631a5f894fc`;
  `hsfmc97ich.o`:
  `b6c18aab4585498700b401b0948161f25ee18e6863af48b2b2f35b0536b60c25`;
  `hsfsoar.o`:
  `3be23c8e6d6ee8f6bc92b89913590ad4828cf2013ba4cf9abc3be854c22ecde9`;
  `hsfmc97ich.inf`:
  `0d7401ddcefd2c362adcdc19fb0b828312983a8e3970c2389596e2ad271a970c`;
  `hsf.cty`:
  `bf205eda7dc753a416c347179e2058cdb1a15f053afbc609fe25995ca00eba34`.
  - The tarball
    (`7388bf4338f434aa65363f2097a9f9c2c87f523ac1a549764de831b68f37846c`,
    released 9 May 2010) is at
    `https://www.linuxant.com/drivers/hsf/oem/downloads.php`.
  - The objects' symbols are `cnxthsf_7800206oem_<name>`. The "full"
    build's symbols differ (`cnxthsf_7800206full_`), and Linuxant no
    longer offers that build.
  - The `.inf` is Conexant's Linux_Intel_SmartMC.inf, "Version:
    5.03.27"; the country file says "Version: V7.72.00".
- **Broadcom.** `BCMSM.sys`:
  `033c6716ef63b1984a8f7a7b36c8efce1c7d9ec622dd97132d54eb5b8a5c0662`;
  `BCMSMwxp.inf`:
  `34f4059d27803bb1b4947ff6dc1f4b872230eaf738551f8c267ba6f50e363ca8`.
  - They come from Dell's package "Communications:V.92 Soft Modem (Data
    Fax) Driver", version A05 (OEM version 3.4.19.1.1, for the Dimension
    2200 to 8300, created 27 Aug 2002; Windows 2000 and XP).
  - The driver answers `ATI3` with "BCM V.92 56K Modem Version 3.4.19".
    Its debug path is
    `C:\releases\3_4_19_1_1\softmodem\build\Win32\opt-dell-wdm-pentium-mmx\bin\BCMSM.pdb`,
    and the INF's `DriverVer` is 05/21/2002,3.4.19.
  - The package's catalog, `BCMSMwxp.cat` (signed by Microsoft Windows
    Hardware Compatibility Publisher, 31 Jul 2002), lists the driver's
    Authenticode SHA-1 (`3261275f43309bd1155178d2549b1496ac9552f2`) and
    the INF's SHA-1 (`74a33b6de90b0d12e3b688a29c94cc1dd399ea51`): both
    files are the ones Microsoft signed. Dell's download of the package
    was not compared.
- **Intel.** `537core.lib`:
  `e81d02cbe185191e1bf9ed46672117edb02ccfb7f3d493800c1aa428f9d6f49f`;
  `wwh_dflt.c`:
  `4d308a3988ae17c9348341feab0f05432e3d6d2483f512667ff09d03a7ccf1e8`;
  and the archive's `license.txt` with them
  (`87e01e6b9bf0b40ecf0d665d033348c61f964a1793124e0516908afd4b1518fa`).
  - The archive
    (`bfc42b0917e1f15cf974de0ddeff7dbf2617316e017888eec1c25574cd5b25c0`,
    "Intel(R) 537 V9x DF modem Linux driver", for 2.4.22-1.2115.nptlsmp)
    was given to us; it was not compared with a download of Intel's.
  - Its licence is a BSD one (redistribution in binary form with the
    notice).
  - The core answers `ATI3` with "537EP".
- **ESS.** `es56hpi.sys`:
  `dc0f3b3614dc3cb168759526d47a306977616318e1bc15102987dd1a415d576f`;
  `mdmzn.inf`:
  `18c4114cfc3c78581791e6d719e1c7e4a41fdde52beca95695ed96c55b4db8ba`.
  - They are from the `WinXP` directory of ESS's installer
    `ess54370.exe`
    (`d9d87c53d3a3dd48c11990b8823ab51755861004fbb4fd565e998551726385e7`);
    its `Win2k` and `Win98` directories carry the same driver.
  - The driver is "ES56H-PI Modem Card", "ESS Telephony Driver", file
    version V5.43.070. It answers `ATI3` with "V5.43.070-H-PI" and `ATI1`
    with "ES2838".
  - The package's catalog, `slm.cat` (signed by Microsoft Windows
    Hardware Compatibility Publisher, 2 Apr 2003), lists the driver's
    Authenticode SHA-1 (`7ccbff2b0b602f8fddec4557ab6505dde6dc0899`) and
    the INF's SHA-1 (`5dc97a690730326335a15cc2ba6e8392f2935796`): both
    files are the ones Microsoft signed.
  - The installer was given to us; it was not compared with a download
    of ESS's.
  - An older build of the driver, 5.43.064 of March 2002
    (`ES2838_2839_XP_583211.zip`, whose own catalog matches it too), is
    not in the tree.
- **spandsp.** Not in the tree: the build uses the system's library
  (pkg-config `spandsp`), which is 0.0.6 in Debian and Ubuntu.

## The closed modules' stack

The Agere, SM56, PCTel and HSF modules read some of their stack before
they write it. Valgrind's memcheck shows where. (It was run on the
harnesses linked `-static`: on the dynamic ones it wants i386 glibc's
debug symbols. Under Valgrind, SM56's `cli`, `in` and `out` reach the
harness as SIGILL rather than SIGSEGV.)

- **Agere.** `ATProcessorStatusHandler`, called from
  `LinkLayerStatusHandler`, tests a value of the status that
  `V32DataPumpReportStatus` and `V22DataPumpReportStatus` build on their
  stack and leave unset. `CodecStatusHandler`'s text (`StatusParser`,
  through its `sprintf`) takes unset bytes of `ReportConnectionStatus`'s.
- **SM56.** `CM_ConfigReqEvent` tests a value that `ACU_DialResponse`
  leaves unset on its stack (in four of the five calls run under
  Valgrind).
- **HSF.** Two of the engine's routines (`hsfengine564_`, under its
  timer, and `hsfengine2004_`) test locals they have not set.
- **PCTel.** Its `+MS` command ([below](#against-the-pctel-hsp-soft-modem)).

What they read is whatever the calls before them left there, including
the harness's own calls. A read or write of the pty, whose timing
depends on the driver, could leave other bytes there, so the same line
samples could give a different call.

So each harness clears 8 KB of the stack below its frame before each
call into its module (`half::stack_clear()`), and a module finds the
same there every time. `HALF_STACK_FILL=<byte>` fills that stack with the
given byte instead of 0. With 0, 0xa5 and 0xff, the Agere, SM56 and HSF
tests (and PCTel's, with their top rate given) all pass, connect at the
same rates and retrain the same way. So what those modules read unset
does not decide their calls here. For PCTel's `+MS` it did.

## Against the Smart Link soft modem

`half_smartlink` runs Smart Link's soft modem: its DSP library
(`test/smartlink/dsplibs.o`, binary-only, i386). This is the library that
slmodemd was written for, and the open DSP keeps its interface
(`dsp/dsplibs.h`). It runs on our modem core in place of the open pumps:
`half_smartlink` is `half_open` built with the library and
`test/smartlink_glue.cpp` instead of `dsp/`.

It needs no hardware. The core gives it the line's samples at 9600 Hz,
as it gives the open pumps theirs; the library does not work at 8000 Hz.

The glue (`smartlink_glue.cpp`) has the little the library needs beyond
the core's interface (its binary log records), and the tests' handles on
it, `SMARTLINK_V92FULL` and `SMARTLINK_FPE_AT`
([dsp/v92-details.md](dsp/v92-details.md)). These work through the
linker's `--wrap` of `dp_runtime_create` and `modem_dp_register`.

The library also asks for the line's delay (`MDMPRM_IODELAY`: PipeModem's
lead silence), and may ask for it to change (`MDMPRM_UPDATE_DELAY`:
input dropped or silence added). The core passes both to its host
(`Host::io_delay`, `Host::update_delay`).

Until October 2026 the library ran on slmodemd's old C core, which was
kept in the tree for it. That core is gone. The 60 calls of the time
came out the same on our core, within the run-to-run variation of the
old harness.

`ctest -L smartlink -j16` runs the tests. `smartlink_log.py` decodes the
library's own debug lines ([dsp/v92-details.md](dsp/v92-details.md)).

The library is slmodem-2.9.11-20110321's (see the table above). Its parts
report their own versions:

- the V.90 modem 2.98 (25-Mar-04, "Modified Quick Connect without Memory
  + Train Time + Constel Power");
- the V.92 modem 1.1 (9-Apr-01, "Memory cleanups + dil descriptor crash
  fix");
- a "V.92 Lite" build.

slmodem's changes after 2.9.11 are to its drivers only.

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

V.34 tests beyond plain connects:

- **A line hit.** 300 ms of silence both ways in the middle of the data
  (`smartlink_34_hit_*`): both modems retrain, and V.42 carries on.
- **Rate renegotiation.** It answers our V.34 rate renegotiation
  (`smartlink_34_reneg_*`).

**V.90.** As the analogue modem, it calls ours (the digital modem, at
8000 Hz) through netsim's G.711 network and connects with V.90, µ-law and
A-law (`smartlink_v90_*`). On the µ-law tests' line (with netsim's noise
floor, `NET_FLOOR=3`) that is 54666/31200
([dsp/v90-details.md](dsp/v90-details.md)). The rate renegotiations both
ways are tested too.

**V.92.** Its V.92 is a "V.92 Lite" build, which retrains when it is
offered PCM upstream. `SMARTLINK_V92FULL=1` sets its full V.92 bit, and
it then does PCM upstream with us:

- 56000/38666 (µ-law) and 40000 upstream (A-law) with our prefilter;
- quick connect;
- rate renegotiations and fast parameter exchanges both ways
  (`SMARTLINK_FPE_AT=<s>` starts its own exchange that many seconds into
  the data);
- modem-on-hold;
- the exchange for its timing slip.

These are the `smartlink_v92_*` tests ([dsp/v92-details.md](dsp/v92-details.md)).

**Class 1 fax** both ways at 9600 Hz (`smartlink_fax_*`):

| Modulation | Smart Link sender → open receiver | open sender → Smart Link receiver |
|---|---|---|
| V.27ter 2400, 4800 | pass | pass |
| V.29 7200, 9600 | pass | pass |
| V.17 7200, 9600, 12000, 14400 (long train for TCF, short for the page) | pass | pass |

Notes on its fax:

- It makes a few bit errors with some of its own signals; hence the
  driver allows 1 bit error in 1000 on a page.
- Its V.27ter training contains stray 180 degree phase flips, which the
  open receiver follows.
- Its V.17 receiver takes a short train or not depending on how its 20
  ms frames fall on it. So the fax calls give the DTE's commands to the
  modems only every 192 samples (`PIPE_SYNC=192`, see
  [testing-details.md](testing-details.md)).

## Against the Agere soft modem

`half_agere` runs the Agere Systems soft modem library
(`test/agere/agrsm_core.o`, Agere SoftModem 2.1.40 from agrsm-20070804;
see above) in user space, on an emulated Intel ICH modem controller and a
Silicon Labs Si3038 codec. Its serial side is a virtual 16550 UART
between the library and the pty. `AGERE_DEBUG=1` prints its log and the
port accesses the harness does not know.

`ctest -L agere -j8` calls it from and to the open modem. Results at the
time of writing:

| Agere `AT+MS=` / ours | Agere caller → open answerer | open caller → Agere answerer |
|---|---|---|
| V34 / 34 | CONNECT 33600 (our transmitter; we receive at 24000) | CONNECT 33600 / 24000 |
| V32B / 132, V32 / 32 | CONNECT 14400, 9600 | CONNECT 14400, 9600 |
| V22B / 122, V22 / 22, B212 / 212 | CONNECT 2400, 1200, 1200 | the same |
| V23C / 23, V21 / 21, B103 / 103 | CONNECT 1200/75, 300, 300 | the same |
| V90 / 90 (ours as the digital modem), µ-law and A-law | CONNECT 46666 | - |

The other Agere tests:

- V.44 both ways, 60000 octets each way (`agere_v44_*`);
- our V.34 rate renegotiation both ways, and a much noisier line 10 s
  after CONNECT, which forces a retrain (`agere_34_reneg_*`,
  `agere_34_noise`);
- phase 2's recovery when our INFO0a is lost (`agere_34_lose_info0`);
- V.92 quick connect with short phase 2 on the second call
  (`agere_v92_qc`);
- V.92 modem-on-hold: its request granted (`agere_v92_moh_grant`) and
  denied (`agere_v92_moh_deny`), and our request, which it takes for a
  retrain (`agere_v92_moh_request`);
- V.90 at the alignment `AGERE_SYNC_PHASE=96` (`agere_v90_phase96`), and
  our V.90 rate renegotiation (`agere_v90_reneg_open`);
- MNP ([below](#mnp)), V.80 ([below](#v80)) and the DTE's retrains and
  renegotiations ([below](#retrains-and-renegotiations-on-request)).

**Class 1 fax** at 8000 Hz (`agere_fax_*`): V.27ter, V.29 and V.17 (7200
to 14400, short-train pages) pass both ways.

Agere's library, in its default V.92 mode, does V.8 with automode off,
and connects to the open modem at V.34.

## Against spandsp's data modems

`half_spandsp` is a modem built around spandsp's pumps (the system's
libspandsp 0.0.6, the version Asterisk and FreeSWITCH use). It has:

- V.21, V.23, Bell 103 and V.22/V.22bis;
- asynchronous 8N1 characters;
- a few AT commands;
- V.8 by default, or the modulation forced with `AT+MS=`.

spandsp has no modem of its own. The `data_modems` of its later versions
is a sketch: its V.32bis (V.17's pumps with V.32's scramblers, no
handshake) and its V.34 (no data receiver yet) cannot connect.

`ctest -L spanpeer -j8` calls it from and to the open modem at each
modulation it has, with `AT+MS=` forced on both sides. (This label is not
part of CI's `spandsp` label, which holds only quick tests.)

| `AT+MS=` | spandsp caller → open answerer | open caller → spandsp answerer |
|---|---|---|
| 122 (V.22bis) | CONNECT 2400 | CONNECT 2400 (after our LAPM attempt, about 20 s) |
| 22 (V.22) | CONNECT 1200 | CONNECT 1200 (the same) |
| 23 (V.23) | CONNECT 1200/75 | CONNECT 1200/75 |
| 21 (V.21), 103 (Bell 103) | CONNECT 300 | CONNECT 300 |

spandsp's modem has no error correction, which found two faults in ours:

- Our V.42 detection phase counted T400 (750 ms) in bits at a nominal
  28800 bit/s: that made it 9 s at 2400 bit/s, 18 s at 1200 and 72 s at
  300. It now counts at the pump's rate (`link/hdlc.cpp`).
- Our caller goes to LAPM without the detection phase (V.42 allows
  this). When the answerer had no V.42, it sent XID N400 times and then
  waited for nothing. It now goes on in asynchronous mode (the
  `link_test` test `LapmCallerAsyncRemote`).

V.8 between the two is not tested. When this was written it did not
connect: our CM offered only V.34 and V.32bis, and our answerer went to
V.32bis automode on a CM without V.34 instead of sending a JM for the
modulation both had. Both have changed since: our CM and JM now carry
V.34, V.32bis, V.22bis, V.23 and V.21 (`dsp/dp_v34.cpp`, `v8_msg`), and
our answerer chooses the best modulation both have (`v8_choice()`; see
[the HSF modem](#against-the-conexant-hsf-soft-modem)).

spandsp, for its part:

- sends a V.92 octet before its CM (a QC1a with all fields 0, which ours
  answers), even with V.92 off;
- in 0.0.6, sends the CM's modulations back in its JM unless the
  application trims them (`half_spandsp` does, in `v8_cb()`).

`SPANDSP_V8_CALL=<n>` sets the call function its V.8 sends (Table 3/V.8;
6, data, by default; 4 is a fax machine that sends), which the adaptive
answer tests use ([below](#adaptive-answer)). `SPANDSP_DEBUG=1` prints
spandsp's logs and the harness's.

## Against the Motorola SM56 soft modem

`half_sm56` runs Motorola's SM56 soft modem: `sm56.ko` of sm56-06.09.05,
built for Fedora Core 3's kernel 2.6.9, with its controller and data
pumps and V.92. It runs in user space:

- The module is linked in as it is. At build time `sm56_obj.cmake`
  renames its kernel imports to `k_<name>` (into `sm56k.o`), and the
  harness provides them.
- The harness emulates an Intel ICH modem controller with a Motorola
  "MOTf" codec, on port I/O that traps and that the harness carries out.
- It patches the module's few reads of `current`.
- It resamples between the line's 8000 Hz and the codec's rate, which
  the module sets by modulation, from 7200 to 12000 Hz (7200 Hz idle,
  9600 Hz for V.90).

The module must be `test/sm56/sm56.ko` exactly as Motorola shipped it
(SHA-256 `a757fd85...`): the harness checks the places it patches.
`ctest -L sm56 -j8` runs the tests. `SM56_DEBUG=1` prints its `printk`s
and the port accesses the harness does not know; `SM56_DEBUG=2` prints
every port access.

**The FPU save.** The module's interrupt and deferred routines save the
FPU (`fnsave`) to `.rodata.cst4` + 0x640 and + 0x644. That runs past the
end of the section into whatever the linker puts after it. With the
module linked first, that is its own constants, and its V.90
interpolation then reads far outside its buffers. The harness points
those four instructions (`fnsave` and `frstor`) at buffers of its own.

| SM56 `AT+MS=` / ours | SM56 caller → open answerer | open caller → SM56 answerer |
|---|---|---|
| V34 / 34 | CONNECT 33600/LAPM (we receive at 24000) | CONNECT 33600 / 24000 |
| V32B,0 / 132 | CONNECT 14400 | CONNECT 14400 |
| V92 (its automode) / 122; V22B,0 / 122 | CONNECT 2400/LAPM | CONNECT 2400 |
| V22,0 / 22, V21,0 / 21 | CONNECT 1200, 300 | the same |
| V90 / 90 (ours as the digital modem), µ-law and A-law | CONNECT 50666/31200 | - |

**V.22bis.** Below V.34, its `+MS` wants automode off: `AT+MS=V22` alone
is an error. Forced to V.22bis that way (`AT+MS=V22B,0`), it does not
connect as the caller to a V.22bis answerer that sends the answer tone
(ours, or the Agere modem's):

- Its unscrambled ones detector starts during the 2100 Hz tone and takes
  the tone for them.
- It sends S1 456 ms after the tone ends. With V.22bis's 75 ms of
  silence, that is while it is already training on the answerer's
  unscrambled ones, so it misses the answerer's S1.
- It goes on at 1200 bit/s, sending S1 again every 2 s.

With 600 ms of silence, or none (the original library answers without
the tone), it connects at 2400. In its automode, which is its default, it
calls our V.22bis answerer at 2400, so that is what the test does.

**The codec's noise.** The harness adds noise to the codec's input
(`SM56_NOISE`, rms 5, about -76 dB of full scale). On exact zeros, the
module's frequency estimate stays at its initial 1800 Hz, which its
answering automode takes for a V.32 caller's AA tone. It then leaves
ANSam within 40 ms for V.32's AC, and a V.34 caller never hears an
answer tone. Rms 3 is not enough; rms 10 costs V.90 a step (45333).

**Its V.92.** It sends QC1a in V.8 on every call, and on the second call
(`CALLS=2`) it does quick connect with short phase 2 (`sm56_v92_qc`). Its
upstream stays V.34:

- The module has PCM upstream (`UAPCMP3_*`), but chooses it only in quick
  connect's INFO1a (`SC_QC_Phase2_Set_Info_1A`), from INFO1d's PCM
  upstream bit.
- Its full phase 2 (`SC_Phase2_Create_Tone_A_Set_1A`) clears that bit
  after reading it.
- Forced on, that path crashes the module (in `SC_Modulate`).

Its short phase 2's INFO1a (3200 baud) says high carrier (bit 33, Table
19/V.92), but it sends on the low one. Its upstream's spectrum is centred
at the same 1850 Hz as after a full phase 2, where our INFO1d asked for
the low carrier (1829 Hz; the high one is 1920 Hz). A receiver on the
high carrier never finds its S, and it retrains. `sm56_v92_qc` fails if
there is a retrain after short phase 2.

**Modem-on-hold.**

- We grant its own request (`AT+PMHR`, `sm56_v92_moh_grant`). It goes on
  hold and comes back with quick connect.
- It takes ours for a retrain, as the Agere modem does
  (`sm56_v92_moh_request`).
- It answers MHreq only where its phase 2 begins, in place of INFO0d
  (`SC_Phase2_Detect_Info_0` looks for both).
- An MHreq sent at its retrain reversal does get an answer: MHnack with
  `+PMHT=0` (its default), and MHcda then clears the call down. With
  `+PMHT` set, it waits for an accept from its platform (the handset,
  `MO_HoldStatePlatformInput`).
- After MHfrr it does not reconnect with V.8, and expects something else
  after the silence.

Up to the point where it answers MHreq, it behaves like a remote that
knows nothing of modem-on-hold. And an MHreq sent at the reversal stalls
the original library and the Agere modem (they take it for our
reversal). So we keep following the retrain.

## Against the PCTel HSP soft modem

`half_pctel` runs PCTel's HSP soft modem in user space: pctel-0.9.7-9,
"HSP56 MicroModem ... K2.4.7.V92.001 (03/26/2003)". It uses the Intel
AMR build of the data pumps (`lib/amr/dsp.a`) and the controller
(`lib/control.a`).

Unlike the Agere and SM56 modems, these need nothing of a kernel. The two
objects (`test/pctel/dsp.o` and `control.o`, PCTel's files renamed)
import only their hardware layer and the virtual UART's registers and
queues. The harness provides these in C, after the driver's `i8xxhal.s`
and `vuart.s`.

- Its interrupt runs `dspMain()` and `modem_main()` once per block of
  codec samples.
- The pumps take the block with `RdASIC` and give theirs with `WrASIC`,
  at the rate and block size they set (7200 Hz and 24 samples when idle).
- `GlobalTimer` is the blocks' time in ms.

`ctest -L pctel -j8` runs the tests. `PCTEL_DEBUG=1` prints what the HAL
(hardware layer) is asked, and the pump the controller asks for
(`sharedRam` + 0x1c, + 0x20) against the pump running (`curModem`).

**`+MS` read its stack.** The controller reads some of its stack before
writing it. `+MS` (`plus_ms`) copies the carrier's name (`ext_ms` +
0x1b) to its stack without the terminating 0, and compares the copy with
"V21" ... "V90", "V92", "K56FLEX".

- The name matched only when the byte after the copy happened to be 0.
  Otherwise `+MS` left the carrier as it was.
- The harness clears the stack below its frame before each call into the
  objects (they use about 1 KB of it; it clears 8 KB), so the copy ends
  in 0, as it was meant to.
- What was there before came from the harness's own calls: a print added
  to the harness, or the stack filled with 0, turned `pctel_v90_calls`
  from 50666 to 45333 every time.

With the name matched, `+MS=V90` and `+MS=V92` take the top rate from the
command (`ext_ms` + 0x86):

- Without one (0, which is under 28000), they set S37 to 0 (automode) and
  keep the downstream to drn 14, 45333.
- `+MS=V90,1,300,56000` sets S37 to 14 (V.90), and the call trains at
  50666. The tests give the top rate.

**A missed INFO0d.** At some starts of a call it misses our INFO0d and
repeats its INFO0a, unacknowledged, every 82 ms. In a sweep of the
call's start over nine periods of the commands, one start did this,
with the harness's codec noise at its 5 rms; at 0 or 2 rms the same start
trains. We had no recovery for that outside quick connect, and the call
waited in phase 2 until the driver gave up. `pctel_v90_calls` failed
this way about once in six full runs, where the machine's load moved the
call's start (the driver's setup commands land a period later when it
answers late). Now our INFO0d goes again until the remote takes one
([dsp/v34-details.md](dsp/v34-details.md)). `pctel_v90_lose_info0` loses
our first one on purpose.

| PCTel `AT+MS=` / ours | PCTel caller → open answerer | open caller → PCTel answerer |
|---|---|---|
| V34 / 34 | CONNECT (we receive at 24000) | CONNECT 24000 |
| V32B,0 / 132 | CONNECT 14400 | CONNECT 14400 |
| V90,1,300,56000 / 90 (V.90, the open modem digital) | CONNECT 50666/33600 | - |
| (its automode) / 122 | CONNECT 2400 | CONNECT 2400 |
| (its automode) / 22 | CONNECT 1200 | CONNECT 1200 |
| (its automode) / 23 | CONNECT 1200/75 | CONNECT 1200/75 |
| (its automode) / 21 | CONNECT 300 | CONNECT 300 |

Its CONNECT reports the DTE's rate (115200). Against itself and the SM56
modem it connects at V.34 both ways (33600 with SM56).

**Low speeds.** Its low-speed modulations work in its automode (no
`+MS`), both ways, against our forced V.21, V.22, V.22bis and V.23
modems. Answering, it sends V.22bis's unscrambled ones after its answer
tone. Forced to one of them (`+MS=V21,0` and so on) they do not work:
answering, it is silent after its answer tone. (These were the open
failures while the tests forced it. Our V.22bis answerer, which once
took 10 s over its V.22bis caller, connects in 8 s now.) With V.8 it
always offers V.34, whatever `+MS` says.

**V.34 with it answering** failed at first: it retrained phase 2 instead
of answering our INFO1c with INFO1a, every time. Its INFO decoder (state
in `g34`) never found the frame's start. We went from L2 straight into
INFO1c, so the frame's first bit was a phase change from our last
control channel symbol before L1. We now send two symbols of B first, as
a phase reference. (The Agere and SM56 modems and the original library
did not mind either way.)

**V.90** with it as the analogue modem connects at 50666/33600, µ-law and
A-law. It used to retrain in phase 4, 200 ms into our TRN2d, and the
call went on at V.34:

- Its receiver measured TRN2d at 20 dB (it wants 32,
  `VpcmARxHandshake`), and later found no usable constellation at all
  (`VpcmSelectCon`). Its CPt already asked for little (drn 6).
- The cause was the harness. During V.90 the modem runs its codec at
  FreqTable's "10287 Hz", which is V.34's 3429 baud times 3, that is
  72000 / 7 Hz. The harness ran it at exactly 10287 Hz, 125 ppm off the
  line's 8000 Hz.
- A V.90 analogue modem's sampling has to follow the network's clock,
  and this one does not track such an offset. At 72000 / 7 Hz its CPt
  asks for drn 14 and the call trains.

**No V.92 to test.** Asked for V.92 (`AT+MS=V92`), it connects with V.90,
with no quick connect or PCM upstream offer, on the first call and on a
second one too. This build has V.92 in its controller only:

- `+MS=V92` is accepted but leaves S37 (the modulation, `act[0x25]`) at
  0, automode, so V.8 picks V.90.
- The controller's V.92 path (`go_v8_v92`: its V.8 for V.92, then
  `set_dsp_interface_v92`) is taken with S37=15 and S19=2. `+PQC` and
  `+PIG` default to on (`V92QuickConnectFlag`, `V92UpStreamFlag`).
- But with `ATS37=15 S19=2` it asks the DSP for modem 15, and the DSP
  (`dsp.o`, "LCE-336V 2.15.4") knows only modems 2 to 14 (`initSys`'s
  switch; 14 is V.90). It sends a CM (with V.23 duplex added) and no
  INFO0a after it, and the call ends in phase 2.
- Nor has the DSP any of V.92's signals (QC1a, QTS, ANSpcm, INFO0a's V.92
  bits) or a PCM upstream transmitter (Ru, TRN1u, the precoder and
  prefilter): only V.90's.

**Hanging up and calling again.** It answers ATH after +++ with NO
CARRIER, which `interop_call` takes as the hang-up, as it does OK. On
hook for the driver's usual 2 s, its second call stops in phase 2: its
DSP never leaves the state after INFO0, and never switches its codec to
9600 Hz for it. With 3 s or more on hook it trains (in real time, as the
tests run it; `HANGUP_WAIT`). `pctel_v90_calls` makes two calls with 5 s
on hook between them: 50666/33600 both times.

## Against the Conexant HSF soft modem

`half_hsf` runs Conexant's HSF soft modem (Linuxant's hsfmodem
7.80.02.06oem) in user space. It has three of the driver's i386 objects,
as they are in `test/hsf/`:

- its engine (`hsfengine`: the controller and the data pumps);
- its AC'97 ICH codec module (`hsfmc97ich`);
- its soft DAA module (`hsfsoar`);

plus the modem's `.inf` and country file, for its NVM (its stored
configuration).

**Linking.** CMake renames each object's `.ctors` and `.gnu.linkonce.*`
sections for that object (`hsf_obj.cmake`): each object has the same C++
names, and the linker would otherwise keep only one. It also makes the
ICH module's microsecond busy wait (`hsfmc97ich41_`) and its print
function (`hsfmc97ich57_`) weak symbols, so the harness can provide its
own (below).

**What the harness provides.**

- The engine's OS layer (`cnxthsf_7800206oem_Os*`): memory, timers on the
  line's clock, I/O ports, the interrupt.
- Its NVM: the `HKR` lines of the `.inf` and the country's lines of
  `hsf.cty` (`HSF_NVM=<dir>` reads them from another directory).
- Models of what the modules drive:
  - an ICH's AC'97 bus master (PCM in and out, pausing and resuming at
    the position it was at);
  - a secondary MC'97 codec (Conexant, 0x4358/0x5429) at 15999 Hz;
  - the SmartDAA behind the codec's register 0x7a, its line side
    answering commands as the DAA library drives them (its kind 0x12;
    see [hardware-details.md](hardware-details.md#conexant-ac97-codecs)).
    So the library powers the DAA up, sets the line up and takes it off
    hook.
- The modem's `ComCtrl` interface is the DTE, on a pty.

**The TSC.** The engine reads the TSC (the processor's time stamp
counter). The harness traps `rdtsc` (`PR_SET_TSC`) and gives the line's
time at 1000 MHz, moving it on by 0.1 µs a read while the line stands
still (otherwise a busy wait would never end). The ICH module waits about
37 µs that way about 80 times a second of the line, 370 trapped reads
each, and most of the modem's processor time went into those signals.
The harness's `hsfmc97ich41_` steps the counter over the wait at once, to
where those reads would have left it, with the same result (a call
trains the same, sample for sample). The HSF calls take about a quarter
of the time they did.

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

| HSF `AT+MS=` / ours | HSF caller → open answerer | open caller → HSF answerer |
|---|---|---|
| V34 / 34 | CONNECT | CONNECT |
| V32B,0 / 132 | CONNECT | CONNECT |
| V22B / 122 | CONNECT | CONNECT |
| V21,0 / 21 | CONNECT | - |
| V21, V23C, V22B, V32B (automode) / 34 (V.8) | CONNECT: V.21, V.23, V.22bis, V.32bis | CONNECT: V.21, V.23, V.22bis, V.32bis |
| V90 / 90 (V.90, the open modem digital) | CONNECT 52000/33600 | - |
| V92 / 92 (V.92, PCM upstream) | CONNECT 52000/41333 | - |

Other HSF tests: our forced V.21, V.23, V.22 and V.22bis callers against
its automode (`hsf_low_*`); V.90 with our INFO0d lost
(`hsf_v90_lose_info0`); our echo canceller against the echo of our signal
from its line's hybrid, in V.90 and V.92 (`hsf_v90_echo`,
`hsf_v92_echo`); and our fast parameter exchange, which it does not have
(no RM to our Rf), so we renegotiate instead (`hsf_v92_fpe`).

**V.90's CJ.** V.90 with it as the analogue modem connects at 52000/33600
over µ-law and A-law. It failed at first: we waited for its CJ while it
went on with INFO0a. It cuts its CM short for CJ, and drops its carrier
right after the last data bit of CJ's third zero octet. So that octet has
no stop bit, and our V.8 receiver counted only two. Now two zero octets,
plus the start bit and eight zero bits of a third, count as CJ (CM and JM
never have two zero octets in a row).

**The ring buffer.** The ICH module ran off its ring buffer every 3.27 s
at first. A word of its device (offset 0x280, set to 2 once the engine
runs) makes it add 8 samples to each position it reads past sample 700,
as for a codec that runs fast. The harness clears that word before each
interrupt (`HSF_KEEP8=1` keeps it).

**Its V.21 and V.23 answerers work only after V.8.** Forced
(`+MS=V21,0`, `V23C,0`), it stays silent after ATA: no answer tone and
no mark for a minute. That happens:

- against our caller, the Agere, SM56, spandsp and PCTel modems' callers,
  and its own (all of which wait for the answerer, as V.25 has them do);
- against a 980 Hz V.21 originate mark put on the line from the start
  (by netsim, as a test);
- with another country (`+GCI`), or the rates given (`V21,0,300,300`);
  and `ATB` is not there.

The harness is not the cause: forced, its V.22, V.22bis and V.32bis
answerers work, its V.21 caller works (the same FSK transmitter), and so
do the V.21 and V.23 answerers of its automode, through V.8.

In automode it sends ANSam and, when no CM comes, its 1650 Hz mark, but
it drops the mark 2 s after the caller's mark (again also against
itself, calling with `+MS=V21,0`). So our V.21 caller (`AT+MS=21`) did not
connect to it until our caller, too, used V.8 on its ANSam. That is for
V.21 alone; our V.22, V.22bis and V.23 callers connect to it without V.8.

**The V.8 changes this led to.**

- Our V.8 caller's CM now offers V.22bis, V.23 and V.21 as well as V.34
  and V.32bis. A JM with one of these (the HSF modem's, answering with
  `+MS=V21` and so on) gets CJ and that modulation, the FSK caller's
  carrier first (as the HSF modem's own caller does).
- Answering, a CM that goes no higher than one of these gets our JM of it
  and, after CJ, that modulation.
- A fault found on the way: the PCTel modem takes our CJ only when V.8's
  75 ms without signal follows it (`dsp/dp_v34.cpp`, `V8C_GAP`). Our V.8
  caller used to start the agreed modulation's carrier right after CJ,
  and the PCTel modem went on with JM until it gave up. About half the
  calls got through anyway, by chance: the V.8 pump did not clear the
  rest of the block it ended in, and what the host's buffer held from
  before went out, now and then a moment of V.21 space, which the PCTel
  modem took for the end of CJ. Our caller now keeps the 75 ms gap.

**Its V.32bis caller.** Its V.32bis (and V.32) caller went silent after
our answerer's AC/CA exchange and never answered our R1. Its own
answerer, after the reversal that answers the caller's CC, keeps on with
AC for 256 symbols before the silence and S. Ours stopped after 64, and
the HSF modem missed our S. With 128 or more it trains; we now send 256,
as it does (the other modems do not mind).

**V.92 with PCM upstream** connects at 52000/41333, µ-law and A-law, with
quick connect (it sends QC1a on the first call already) and without it
(`+PQC=3`, `hsf_v92_pcmup_noqc`). It failed at first in phase 3: we
waited for its Su to reverse while it waited for our Jp.

- Its Su, Su-bar and Su come as one short burst: Su for 144 symbols,
  Su-bar for only 24, then Su again until Jp.
- Its Su has Sd's shape, period 6, but its energy is mostly at 4000 Hz
  (with quick connect, all of it; without, the Su it holds is a sine).
- Our receiver took only a sine of period 6 for Su, and looked for
  reversals between two 12-symbol windows 6 apart: too far apart for a
  Su-bar of 24 symbols.

Now, while we send Jd, a signal of period 6 with some of its energy at
1333 Hz counts as Su, and Su's reversals are each period's phase against
Su's.

## Against the Broadcom soft modem (Windows)

`half_broadcom` runs Broadcom's V.92 soft modem for Windows, `BCMSM.sys`
3.4.19. This is the WDM (Windows Driver Model) driver of the "BCM V.92
56K Modem", with its controller, data pumps, V.42, V.44 and fax all in
that one driver, as Dell shipped it for its Dimension desktops in 2002
(see above). It runs on `interop_windows.cpp`, a Windows kernel of our
own.

### Why not Wine

Wine could not run it. Wine's kernel (`ntoskrnl.exe`, master of October
2026) has 726 of its 1716 exports as stubs, and its HAL 71 of 92:

- Its `KeInsertQueueDpc` queues nothing.
- `IoConnectInterrupt`, `IoGetDmaAdapter` and `KeSynchronizeExecution`
  are stubs.
- `MmMapIoSpace` returns NULL, and port reads return 0.
- START_DEVICE comes without resources.
- Timers run in real time on a thread pool.

A soft modem needs all of these, and here it has to run on the line's
clock.

### Our Windows kernel: `interop_windows.cpp`

`interop_windows.cpp` is a Windows kernel for the WDM driver of a soft
modem on a PCI device. The Broadcom driver was the first it ran.

A harness describes its device in `struct win_dev` (`interop_windows.h`):

- the driver's files and the INF's model;
- the PCI configuration space (the harness's `PciDevice`'s);
- the registers (BAR 0's memory or port I/O, PCI's configuration ports
  among them), as calls;
- the interrupt line;
- the line's rate, and how to run the line on while the kernel waits.

Its interface:

- `win_start()` loads and starts the driver and opens its port;
- `win_tick()`, called after each line sample, runs the kernel's time;
- `win_serial_read()` and `win_serial_write()` are the port's reads and
  writes.

Everything else the driver needs comes from the INF: the registry, the
service, and the device's description, maker and class. The device's IDs
come from its configuration space.

The kernel is part of a 32-bit program. It loads the PE image at its
base (0x10000), relocates it and binds its imports by name. The Broadcom
driver has 114 imports: 102 from `ntoskrnl.exe` (one of them data), 7
from `HAL.dll`, 3 from `WMILIB.SYS` and 2 from `ks.sys`, stdcall and
fastcall as in Windows. An import the kernel does not have stops the
program with its name.

Everything runs on one thread, on the line's clock:

- The interrupt time and the system time are the line's samples. While
  the line stands still, each read moves them on by 1 µs, because the
  driver busy-waits on them a few µs at a time.
- Timers and their DPCs (deferred procedure calls) run between samples.
  The ISR runs when the device's interrupt comes up (for the BCM4212, a
  status bit it enables).
- The driver's system threads are coroutines (`ucontext`). Each runs
  when the event or timer it waits for is signalled.
- IRQL is respected: no interrupt at or above the device's IRQL, no DPC
  at DISPATCH_LEVEL and above, and threads at PASSIVE_LEVEL only.
- The registry is held in memory: the INF's `AddReg` sections, as the
  modem class installer writes them, and `PortName`.
- fs points at a KPCR (a TLS entry of the GDT, set with
  `set_thread_area`). The driver checks the stack it has left against its
  NT_TIB's limit (fs:8), which the kernel sets for each coroutine.
- `in`, `out`, `cli`, `sti`, `rdtsc` (`PR_SET_TSC`) and moves to and
  from control registers (the driver reads CR0 and CR4) fault, and the
  kernel carries them out.

We are the rest of the device stack:

- Under the driver we are the PCI bus driver: its PDO (physical device
  object), with the configuration space, the PnP IRPs the driver sends
  down, and its resources.
- We are also the PnP manager: DriverEntry, AddDevice, and START_DEVICE
  with the device's memory BAR and interrupt.
- Over the driver we are Windows' `modem.sys`, whose lower filter the INF
  makes the driver. To us the driver is a serial port. We open it
  (IRP_MJ_CREATE), set it up with the serial IOCTLs (baud rate, line
  control, handflow, timeouts, DTR, RTS), and read and write it with IRPs
  that carry the pty's bytes (`half_broadcom`'s DTE).

### The BCM4212 chip

The driver's hardware is the BCM4212, Broadcom's PCI modem chip
(14e4:4212, Dell's subsystem 1028:0001). This build (`opt-dell-wdm`) has
codec code only for CodecType 0x15, 0x19 and 0x1c, the BCM4212's; the INF
gives 0x15. The driver also looks on the PCI bus for AC'97 controllers
(the ICHs that the other harnesses emulate). But for the codec types
those use, its codec operations are a stub that fails, and opening the
port then fails with STATUS_INSUFFICIENT_RESOURCES. So `half_broadcom`
emulates the BCM4212.

There is no datasheet to be had, so the registers below are what the
driver does with them:

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
`MmMapIoSpace`. The kernel gives it a window it cannot access, so each
access faults. The kernel then decodes the instruction, opens the window
with the register's value in place (from `half_broadcom`), and
single-steps the instruction (with the trap flag, TF). At the trap after
it, the kernel gives the device what was written and closes the window
again.

The DMA engine is Broadcom's own, the BCM4401 Ethernet's (Linux's `b44`
driver knows it). Its rings hold 8-octet descriptors: a control word
(the length in bits 0 to 12, 0x10000000 for the end of the ring,
0x20000000 for an interrupt when done) and an address. The samples are
16 bit.

- TX: 64 descriptors of 96 octets (6 ms), with an interrupt on the last.
  The engine stops at the descriptor the driver posted last.
- RX: 128 descriptors of 4 and 92 octets in turn, end to end, with an
  interrupt for each pair.

The line side is a Silicon Labs Si3034 DAA behind the chip's serial
port:

- Its PLL sets the sample rate (registers 7 to 9: 4 MHz · M1 M2 / (N1 N2)
  / 5120). It stays at 8000 Hz in the calls here, V.90's too.
- Register 5's bit 0 is the hook.
- Register 12 is the line's state: frame detect, and the loop current in
  its low four bits. 1111 there is an overload, and the driver hangs up at
  once (it went off hook and straight back on). The harness reports 1000,
  half the scale.

### How the driver starts the chip

This comes from `BCMSM.sys`'s code, read with REA's disassembly, and
from its register trace under `BROADCOM_DEBUG=2`, which prints each
access with the address of the instruction.

1. It maps the BAR and reads its settings from the registry: `SiSetup`
   (20 bytes of DAA setup; without it, it works out the PLL itself for a
   4 MHz clock), `UseGPIOHook`, `DaisyChainMode` and
   `OffHookWatchdogTimeout`.
2. Reset (CodecType 0x15): control (0x00) = 0x1001, a short wait, then
   0x2000; 0xb0 and 0xb4 = 0x80; 0xc8 and 0xca = 0x3f; both DMA
   channels (0x200, 0x210) and the interrupt mask (0x24) to 0. Then
   0xb0's bit 2 is cleared, control = 0x1000 for ten reads of it, and
   control = 0x2000. The driver tries this for up to 100 ms until the
   DAA answers.
3. The DAA's setup, with each write read back and retried up to 25
   times:
   - register 1 = 0x80 (soft reset);
   - the PLL (7 = 0x18, 8 = 0xff, 10 = 0, 9 = 0: 8000 Hz);
   - 6 = 0xe0; 5 = 0;
   - it waits for frame detect (register 12, bit 6), then reads register
     13. Bit 6 set means an Si3034 ("International"): the driver sets
     register 18's bit 1 and writes the country's registers 16, 17 and
     18 (for the US: 0x08, 0x00, 0x02). Bit 6 clear means an Si3035
     ("USA Only"), which keeps to the US alone (`AT+GCI=?` lists only
     B5).

   The model is an Si3035 by default; `BROADCOM_DAA=3034` makes it an
   Si3034. `broadcom_si3034_broadcom_calls` tests that: off hook, register
   18 is then 0x03, not 0x01. Even so, `AT+GCI=?` lists only B5: the
   other countries' values come from the registry
   (`SOFTWARE\BCMSM\Modem\Country\`), which this INF does not fill.
4. DMA: both channels off, then on (0x200, 0x210 = 1), the rings'
   addresses (0x204, 0x214), the interrupt mask (0x24 = 0x101c000), the
   last descriptors posted (0x208, 0x218 = 0xff8), and control |= 0x60.

A DAA write waits for 0x3c's bit 15 to clear, then writes `reg << 8 |
value` to 0x38. A read writes `0x2000 | reg << 8`, waits again and takes
0x3c's low byte. The driver polls register 12 (the line's state) about
500 times a second.

Off hook, the driver sets register 18's bit 0, then 5 = 0x01 (the hook),
17 = 0x20, 20 = 0x60 and then 0, and 6 = 0xe3. On hook it undoes them:
18's bit 0 clear, 5 = 0, 17 = 0, 6 = 0xe0.

CodecType 0x19 and 0x1c are other chips: a Sonics backplane like the
BCM4401's, whose V.90 core (core ID 0x807 at 0xffc) the driver resets
through 0xf98 (0x30001, then 0x30000), with its interrupt routed by 0xf94
and a serial clock divider in control's bits 16 and up (from
`BackplaneFreq` and `MasterClockDiv`). The BCM4212 has none of this.

`half_broadcom` resamples between the codec's rate and the line's
(`BROADCOM_RATE`, 8000 Hz) and adds the codec's noise (`BROADCOM_NOISE`,
rms 5).

Two of the driver's own habits:

- It reads each setting with Windows XP's version appended first
  (`CodecFlags__5_1`), then without it.
- It reads `DisableTxRxDpc` with a handle that is not a key (an address
  in its device extension). Windows answers STATUS_INVALID_HANDLE, and so
  does our kernel.

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
µ-law line (the same as with the Agere and SM56 modems); it receives ours
at 33600. Its LAPM calls agree on V.44, and `broadcom_v44_*` sends 60000
octets each way through it.

**Class 1 fax** at 8000 Hz (`broadcom_fax_*`): V.27ter, V.29 and V.17
(7200 to 14400) pass both ways.

**Its V.92** sends QC1a on every call, and its upstream stays V.34. By
default it leaves PCM upstream out (`+PIG:1`). With `AT+PIG=0` it
chooses PCM upstream in INFO1a (`broadcom_v92_pcmup`):

- Phase 3 failed at first. Its Su lasts 96T before Su-bar, where V.92
  has 144T. Our receiver accepted Su only after 96 coherent samples,
  which came inside its Su-bar, so it never saw the reversal. The modem
  then waited 4 s for our Jp and gave up. Su now counts after 48 samples,
  and phases 3 and 4 get through.
- After E2u it sends no B1u (48 frames of scrambled ones). With our usual
  CPd (a prefilter of 64 taps), its output turns periodic (48 symbols) a
  few samples after E2u. With no prefilter (`DSP_V92_PREFILTER=0`) it
  falls silent: exact zeros, which no V.92 precoder can produce, since
  the constellations exclude zero. Neither decodes as ones. We retrain,
  and the call connects at V.90 (50666/33600).

PCM upstream may well be unfinished in this build, as it is off by
default.

## Against the ESS soft modem (Windows)

`half_ess` runs ESS's V.92 soft modem for Windows, `es56hpi.sys`
5.43.070. This is the WDM driver of the "ESS ES56H-PI Data Fax Modem",
with its controller, data pumps (fixed point and MMX), V.42, V.44, MNP
and fax in that one driver, as ESS shipped it in 2003 (see above).

It runs on `interop_windows.cpp`, as the Broadcom driver does, and uses
some parts of it that the Broadcom driver did not need:

- Its BAR 0 is ports, not memory. START_DEVICE gives it a port resource
  (`win_dev::io_size`), and its port accesses go to the harness. They are
  16 bit: the port is the one in dx, whatever the driver passes above
  it.
- 20 more imports: C library functions (`strstr`, `strncmp`, `_strnicmp`,
  `atoi`, `wcsstr`, ...), the compiler's 64-bit helpers (`_allmul`,
  `_alldiv`, and `_allshl` and `_allshr`, which take edx:eax and cl),
  `DbgPrint`, `RtlInitString`, the fast mutexes, and `KeTickCount`, a
  data import (a KSYSTEM_TIME that the kernel moves on every 10 ms of the
  line).
- AddReg lines of the harness's own after the INF's (`win_dev::addreg`):
  with `ESS_DEBUG=2` they set the driver's `DebugLevel`, and its
  `DbgPrint`s print.

### The ES2838 chip

Its hardware is ESS's ES2838 (PCI 125d:2838, ESS's own subsystem; the
INF's model `ModemPV`), a host signal processing modem: the chip has a
codec, a sample FIFO and an interrupt, and the driver does everything
else. There is no datasheet to be had, so the ports below
(`hw/es2838.hpp`) are what the driver does with them:

| Port | Register |
|---|---|
| 0x00, 0x02 | an index and its register's data: the chip's registers below 0x100, its codec's above (0x14c the GPIO configuration, 0x154 the GPIO status, 0x15a the codec's rate in bits 12 to 15) |
| 0x04 | the FIFO: the driver writes four DAC samples, then reads four ADC samples, and again |
| 0x06 | the number of ADC samples in the FIFO (more than 0x1ff is an overrun) |
| 0x08, 0x0a | a DAA's registers behind a serial port (0x08 the command, bit 7 a read, bit 0 busy; 0x0a the data); this codec type leaves them alone |
| 0x0c | control: bit 0 the FIFO runs, bit 1 its interrupt |
| 0x0e | status: bit 1 the FIFO's interrupt, bit 0 the line's |

- The interrupt comes with 16 samples in the FIFO. The driver takes 12
  at a time (16 at its V.34 rates), a block every 1/600 s.
- The codec's rate is in its register 0x5a (index 0x15a), set from a
  table in the driver of the rates its pumps run at: 6 for 7200 Hz (V.8,
  V.21 to V.32bis, fax); 5, 4, 3, 2 and 1 for 8229, 8400, 9000, 9600 and
  10286 Hz (V.34's symbol rates at three samples a symbol, and 2400 baud
  at four); and 11 for 8000 Hz (call progress and V.90).
- Bit 9 of the GPIO status is the loop current. Off hook without it, the
  driver counts on and says OVERLOAD (its over-current protection).

`half_ess` resamples between the codec's rate and the line's (`ESS_RATE`,
8000 Hz) and adds the codec's noise (`ESS_NOISE`, rms 5).

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

These pass too: V.8 in its automode (V.34, with it calling), V.44 both
ways (60000 octets), MNP 5 both ways, and, as with the other peers:

- V.8 agreeing on V.21, V.23, V.22bis and V.32bis both ways
  (`ess_v8_*`), and our forced V.21, V.23, V.22 and V.22bis callers
  against its automode (`ess_low_*`);
- our V.34 rate renegotiations both ways, the line 10 dB noisier 10 s in
  (a retrain), and phase 2's recovery from our lost INFO0a;
- V.90 with our INFO0d lost; our renegotiation (after its own to 26400
  upstream, which it starts at every V.90 connect); and the echo of our
  PCM signal 20 dB down (our echo canceller takes about 31 dB away);
- V.92 with the echo 10 dB down, PCM upstream on A-law lines, and our
  modem-on-hold request (it takes our tone B for a retrain's and
  retrains, as the Agere modem does);
- MNP 4 alone, with it calling.

**Class 1 fax** at 8000 Hz with it receiving (`ess_fax_*_open_sends`)
passes at all ten modulations. It says CONNECT a little before it
receives: at 300 bit/s its first octet or more are lost, so its FSK calls
send their data a second after CONNECT (`DATA_WAIT=1`). Its V.29 at 9600
now and then gets the page's first three octets wrong, which the fax
check accepts (it finds the page by its next 128 bits).

**Bell 103.** As the Bell 103 caller it ignores an answer tone (2225 Hz)
that is already on the line as it dials, and sends nothing. So
`ess_103_ess_calls` answers 2 s after it dials (`DIAL`, `ANSWER_AFTER`),
as after rings, and it then calls at 300 bit/s.

### What it does not do here

- Once it has left a call's data state for command state, its parser
  takes no command. After `+++` it says OK; then `ATI` gets no answer,
  and `ATO` goes out on the line as data. As the fax sender (the caller)
  it takes the answerer's frames and says OK, but takes no `AT+FTH=3`
  after them, nor `AT` (with our answerer, Agere's and Broadcom's alike),
  until the call is over. So there are no fax calls with it sending, and
  no modem-on-hold of its own (`AT+PMHR` needs the escape). Keeping a
  WAIT_ON_MASK in flight, as modem.sys does, makes no difference.
- No Bell 212A: `AT+MS=B212` is ERROR (and `ATB1` with V.22 is no 212A).
- Its V.92 INFO0a has short phase 2 off: no quick connect, with or
  without `+PQC=0`.
- Answering with MNP (`+ES=3,0,6`), it often takes our direction no
  higher than 14400 bit/s (7200 at some answer timings), and retrains up
  a second or two into the call, as an MNP 10 modem would. MNP 5 goes on
  through the retrain. With MNP 4 alone (our `%C2`) it gives its DTE no
  CONNECT after the retrain, so that test runs only with it calling.
- Plain V.34 answers take 33600 at most timings, but 14400 or 7200 at
  some: about one call in ten with `ESS_SYNC=0`, where the ATA lands
  wherever the scheduler puts it. These were the intermittent caps of
  full-suite runs. The tests' `ESS_SYNC` (the default, `PIPE_SYNC`'s 4800
  samples) keeps them at a timing that takes 33600.

### Its PCM upstream

With `+PIG=0` its INFO1a asks for PCM upstream. Phases 3 and 4 get
through: Ru, TRN1u, Ja, TRN2u, SUVu, its CPu (6 constellations), SUVu'
and E2u. Three things in ours had to change.

**The CPd's filter sections.** Its INFO1a has filter sections 0 (p1 and
z2 alone, Table 18/V.92). Our CPd had 4 taps of z1 and p2 in it (for the
Smart Link library, whose filters have all four). It never took that CPd,
and retrained after half a second. Our CPd now has z1 and p2 only where
the INFO1a has them.

**Its points.** Its points then differ from V.92's in two ways, both now
handled in our receiver (`v92.cpp`, `data_symbol()`):

- Without precoding, its negative points are a grid step (16 G) further
  out than the mirror of its positive ones: they land on the even grid
  units. The first 24 symbols' negative points show it, and from then on
  they go a step in.
- Its precoder rounds its feedback down to the grid unit below: it sends
  x = w - ceil(fb), where fb is the sum of p1 over its previous x. So we
  receive the point less a fraction of a unit; the decisions sit at the
  edge of their regions, and B1u had about 5 % of its bits wrong. We
  rebuild its x from our decisions, and so fb (near a unit, on either
  side, the one nearer a point: its sum is not quite ours). The first 48
  symbols' grid mse with this and without it tells which applies.

**The trellis frame's 4th symbol.** With our prefilter (in z2, the
precoder in p1) its points come on our levels. But on the trellis frame's
4th symbol, about half of the time, it sent 0. There the classes are of
2M (eta = 2K + 2zM + parity), and its precoder goes past the M (1 + sum
|b|) points our prefilter's CPd had (42 for M 36); there it sends 0
rather than the last point. (The HSF and Smart Link modems keep within
them.) The CPd now has at least 2M points, as the design without the
prefilter had; the HSF and Smart Link rates are the same with them.

ESS's PCM upstream now connects at 41333 (µ-law and A-law), with
modem-on-hold, and with the echo 10 dB down (33333) too.

**The fallback.** Should a PCM upstream with the prefilter fail on a
modem with p1 and z2 alone, we retrain and try once more without the
prefilter or trellis decoding: precoding alone, with the corrections
above. ESS gets 32000 or 33333 that way. Its points, off G.711's levels,
carry its quantization noise (which TRN2u's mse misses), so the margin
renegotiates down, each time below the last rate (`drn_cap`), to about
30666.

### What it showed in ours

Our V.32 and V.32bis caller sent S for 256 symbols after R1, then S-bar.
V.32 (5.4.1) has S first for NT, the round trip delay the caller measured
from its CC, and then the 256 symbols of the conditioning signal. The ESS
answerer stopped R1 at our S and never sent its S, TRN and R3.
(Broadcom's caller, with NT, got through.) Our caller now measures NT
(from its CC reversal to the answerer's, less the answerer's 64 symbols)
and sends S for NT + 256 symbols.

## Against the Intel 537EP soft modem

`half_intel` runs the core of Intel's 537EP driver for Linux
(`537core.lib`, "Release 4.65", of the Intel537EP 2.37.50.1 driver for
Fedora Core 1). The core has its controller (AT commands, V.42, V.42bis,
V.44, MNP, fax) and its data pumps (Intel's own, on IPP, Intel's
Integrated Performance Primitives) up to V.92. The core runs on the host;
its "DSP" is a software comm RAM.

It needs no kernel: only what the driver's open glue (BSD licensed) gave
it, which the harness provides again in C++ (89 symbols):

- the virtual UART's queues and lines (`uart.c`), with the DTE on the
  pty;
- the real-time service (`rts.c`). Every 10 ms of the line it runs the
  controller's timers (`linux_sys_isr_timer_ticker`, `acu_timer`), and
  when its time comes, the RTS task `clm_bg()`, on a stack of its own (a
  `ucontext`). The tasker's `RSATaskYield()` sleeps the task until the
  next time. In the kernel, the task's time slice ran out while the timer
  ticked; here no time passes inside the task, so it sleeps at each yield
  (`fax1_execute_command` waits that way for `AT+FTS`);
- the precise dialer (pulse dialling, hook flash), a task on its own
  stack that waits with `linux_schedule()`, resumed every ms;
- the locks (none: there is one thread), but the core's resources
  (`linux_RSAGetExclusive`, try-locks in `locks.c`) as they were: the RTS
  task can yield while holding one, and `cp_dsp` gets none from a stub;
- the DMA rings' memory (`AllocateDmaResources`), the country tables
  (`wwh_dflt.c`, Intel's data as it is), `gettimeofday` and `rdtsc` on the
  line's clock, and none of the persistent settings (the hamregistry's
  requests time out at once).

### The chip

The chip (8086:1080, `hw/i537ep.hpp`) is an Si3052 DAA with its own DMA.
The core reaches its registers through four accessors of its own on a
base pointer (the driver's `iobase`), so here they are plain memory, and
the chip does its work between the driver's calls:

- two rings of 32-bit words (a sample in the low half of each), 32
  periods of 96 words: the ADC's at 0x08 to 0x14 and the DAC's at 0x18 to
  0x24 (base, end, interrupt point, current word);
- the interrupt status at 0x04 (written back to clear: the handler
  acknowledges what it read);
- the codec's rate at 0x37 (7200 to 16000 Hz; 9600 by default, and
  72000 / 7 for V.34);
- the codec's clock lock and the loop current at 0x3c;
- the hook at 0x35.

Each period's interrupt runs the chip's handler (`AfeIRQ`), the comm
RAM's, and then the DSP tasklet `DspTask()`, with a clean FPU as the
kernel gave it (`fnsave`).

### IPP's processor

IPP picks its pumps' code by `cpuid` (px, m6, a6, w7). The harness makes
it a Pentium 4 (`ippCoreGetCpuType` returns 6, w7) whatever the host;
`INTEL_CPU=<type>` picks another. IPP's cycle counter
(`ipp_get_pentium_counter`, a bare `rdtsc`) runs on the line's clock.
Both symbols are weakened in the object, and its own `exit` (empty),
`index` and `pow` are renamed (`CMakeLists.txt`). With the other code
paths (`INTEL_CPU=0`, 3, 5: px, a6, m6), its V.34, V.32bis, V.90 and V.17
fax calls pass as well.

`INTEL_DEBUG=1` prints the chip's DMA and the codec's rate;
`INTEL_DEBUG=2` also prints the core's own messages.

### The results

Its line runs at 9600 Hz (`INTEL_RATE=9600`): V.90 connects at 53333
that way, but at 45333 through the resampling to 8000 Hz. Against the
open modem (`ctest -L intel`):

- V.34 (33600), V.32bis (14400), V.22bis, V.22, V.21, V.23 and Bell 103,
  each forced, both ways;
- V.8 with both modems in their automodes (V.34, with it calling); V.44
  both ways (60000 octets); MNP 5 both ways;
- V.90 as the analogue modem, both laws (53333/31200);
- class 1 fax at all ten modulations, both ways.

**Its V.92** (`+MS=V92`, the default): its INFO0a says V.92, but its
INFO1a asks for V.90 (no PCM upstream), and the call is V.90
(`intel_v92`). With `+PIG=0` it asks for PCM upstream and gets through
phase 3 (Ru, Ja, CPt, E1u), TRN2u and SUVu, and acknowledges our CPd
(SUVu'). But it sends no CPu: 0.4 s later it retrains (tone A), with or
without our prefilter, with a 16- or 64-state trellis, and the call is
V.90 after the retrain (`intel_v92_pcmup`).

**What it showed in ours.** Our V.32bis answerer reversed its CA to AC as
soon as it saw the caller's CC; 6.2/V.32bis has it 64 symbols after. The
caller times the round trip from that reversal, and the Intel modem,
calling, then missed our training at most alignments of the line (as it
does the Agere answerer's at 6 alignments of 8). Our answerer now
reverses 38 symbols after its detector's detection (which is 26 symbols
late), as our caller does its CC. It also sends AC until the caller's
tone drops (16 symbols of silence, then S), not for 256 symbols.

## The open modem on an HD-audio modem codec

`half_hda` is not another vendor's modem. It is the open modem, on the
modem core as `dmodemd -a` runs it, over an emulation of the hardware of
an HD-audio soft modem (the laptops of about 2005 to 2010). No closed
driver in the suite drives HD audio, so the driver is our own, written to
the interface that Linux's `snd-hda-intel` and `snd-hda-codec-si3054`
use (`sound/pci/hda/patch_si3054.c`). What the calls test is that path:
the codec's verbs, its registers and switches, the streams' DMA, and the
modem running on the codec's clock.

- `hw/hda.hpp`: the controller's reset and STATESTS, the immediate
  command interface (ICOI, IRII, ICIS), and one input and one output
  stream (SDnCTL, SDnSTS, LPIB, CBL, LVI, FMT, BDPL/BDPU) walking their
  buffer descriptor lists in the process's memory.
- `hw/si3054_hda.hpp`: the Si3054's root node (vendor ID 163c:3055;
  `HDA_VENDOR` sets another) and its modem function group, with the
  registers as nodes 2 to 21 (verbs 0x9 and 0x1, 16-bit payloads):
  EXTENDED_MID's ready bits, LINE_RATE, LINE_LEVEL's stream numbers,
  GPIO_CONTROL's hook and line monitor, CHIPID, and LINE_STATUS's frame
  detect. Off hook its ADC hears the line and its DAC drives it; on hook
  the ADC hears the line only with the monitor on.
- `half_hda.cpp`: the driver.
  - It probes the codec. A vendor ID outside the Si3054 driver's list is
    refused, as no driver would bind it.
  - It runs the Si3054 driver's init.
  - It prepares both streams: four periods of 10 ms, IOC (interrupt on
    completion) on each, and the stream format of the rate (48 kHz over
    5 for 9600 Hz).
  - It sets the switches as ALSA's "Off-hook Switch" and "Caller ID
    Switch" controls do.
  - Each period captured is the modem's next 10 ms, and the modem's
    answer is written two periods ahead into the playback buffer, as
    `AlsaLine` does.

`HDA_CODEC_RATE` sets the codec's rate (9600 by default, or 8000).

`ctest -L hda` runs its calls with the open modem:

- every modulation from V.34 to V.21, both ways;
- V.34 with the codec at 8000 Hz;
- V.90 (56000 down, both laws) and V.92 (quick connect, PCM upstream at
  37333), with it as the analogue modem;
- class 1 fax both ways at each modulation.

The data calls have the codec at 9600 Hz on the 8000 Hz line, through
`CodecLine`'s resampler. The PCM calls have the line at 9600 Hz
(`HDA_RATE=9600`), as the Intel modem's has. The reason: the resampler's
gain differs a little at each of its five phases, which the DIL (digital
impairment learning) measures as a level of its own at each frame
position, and V.90 then falls back to V.34 (no real PCM line has such a
thing). The fax calls are at 8000 Hz, with `half_open8`, because the fax
relay runs at one rate. `hw_test` checks the controller and the codec on
their own.

## The line rung

With `NET_RING=<country>`, netsim puts the call through the way that
country's exchange does. Without it, netsim only carries the samples.

- **The hooks.** The modems tell netsim their hook state: `half_open`'s
  `PipeModem` and `half_hda` write `1` off hook and `0` on hook to
  `MODEM_HOOK_FD`. `interop_call` makes that their fd 5, and netsim's
  `NET_HOOK_A` and `NET_HOOK_B`.
- **Dial tone and digits.** A, off hook, hears the country's dial tone
  (steady, or in its cadence: Italy's is 0.2 s on, 0.2 off, 0.6 on, 1
  off) until its first digit. Netsim hears the digits: DTMF on A's line
  (`DtmfDetect`), or dial pulses. A pulse is a break by A of under 0.5 s,
  and a digit is the count of pulses once 0.3 s go by without one.
  Netsim logs the number dialled.
- **Ringing.** `NET_RING_DELAY` seconds (2) after the last digit (or
  `NET_DIAL_TIMEOUT` seconds, 4, of dial tone if A dials none), B's line,
  on hook, gets the ring signal, in the cadence of the country's ringing
  tone. The ring signal is 25 Hz; 20 Hz with the North American ringing
  tone (440 + 480 Hz), 16 Hz in Japan, and 50 Hz in France's networks;
  24000 at its peak. Meanwhile A hears the ringing tone itself (its one
  or two frequencies, or one modulated).
- **Answer.** B going off hook answers, and from then on the line is
  through. Until then each side hears only the above. A on hook for 0.5 s
  (longer than a dial pulse) ends the call.
- **Busy and congestion.** With B off hook already, or `NET_BUSY=1`, A
  hears the country's busy tone instead of the ringing (and B is not
  rung). With `NET_CONGESTION=1`, it hears the congestion tone (the
  network's lines or switches busy). Either tone goes on until A hangs
  up.

### The countries

The countries are those of the ITU's list of the tones used in national
networks (Annex to ITU Operational Bulletin No. 955, position on 1 May
2010, after Recommendation E.180): 192 of them, in `dsp/tones.cpp`.
`dsp/tones_gen.py` generates that file from the list's Word file, and
says how it picks a tone where the list gives several. Where the list
has gaps:

- the countries with no dial tone in it get a steady one at their
  ringing tone's frequency;
- a "Re-order tone" counts as a congestion tone;
- a country with no busy tone takes its congestion tone (or E.180's 0.5
  s on and 0.5 s off), and one with no congestion tone takes its busy
  tone.

Names are the list's, in capitals
(`UNITED_KINGDOM_OF_GREAT_BRITAIN_AND_NORTHERN_IRELAND`), or an alias:
the core's country names (`UK`, `USA`, `SOUTHKOREA`, ...) and a few short
ones.

### The ring tests

`ctest -L ring` makes a call between two open modems in each country. A,
set to that country, waits for the dial tone (`ATX4`) and dials 123 in
DTMF; B answers at its first RING (V.22bis, a second of data). A few more
calls:

- no dial tone wait and no digits (`ATX3`), with B answering at the
  second ring (`ring_uk_second`);
- two calls (`ring_usa_calls`);
- V.34 (`ring_germany_34`);
- `half_hda` answering (`ring_hda_answers`);
- `half_hda` pulse dialling after its dial tone (`ring_hda_pulse_dials`).

Then a busy call and a congestion call in each country. A, set to the
country (`COUNTRY_A`, dmodemd's `--country`, which accepts every country
of the list), waits for the dial tone, dials 123, and must end with BUSY
(`EXPECT_FAIL`). On a steady tone (Ascension's congestion tone) it must
end with NO ANSWER after S7. All 384 calls do.

The modem takes the country's own busy and congestion tones for busy
(`dsp/dp_callprog.cpp`, from `dsp/tones.h`): four cycles of either's
cadence, within 15% or 80 ms either side. It does not take the country's
ringing tone for busy, so the callers of the countries whose ringing tone
is 0.4 s on and 0.2 s off for ever can dial with `ATX4` too.
`ring_tones_test` checks the same without the calls: every country's
busy and congestion tones are BUSY to a modem in that country, and its
ringing tone is not.

The modem's dial tone check (the call progress pump's 350, 425 and 440
Hz filters, which also pass 400 and 450 Hz and the pairs) wants 1 s of
dial tone. A dial tone in a cadence counts too: its tone is added up
across gaps of up to 1 s, and while one is heard the modem waits 2 s
longer than S6 for it.

`ring_tones_test` also checks every country's ring signal without the
calls: through `RingDetect` to the core, it must give a RING for each of
its rings, and auto answer at S0=2. For that the core's ring check
(`core/modem.cpp`):

- takes a burst of four pulses or more (a 16 Hz signal's 0.3 s) as a
  ring;
- takes a ring still going when it is checked (2.5 s after its first
  pulse) as a ring;
- takes ringing with no gap of 1.5 s (Egypt's 2 s on and 1 off) as a ring
  every 6 s;
- keeps a ring in S1 for 8 s, so that a cadence of up to 10 s (Serbia's 1
  s on and 9 off) rings in time.

Tuvalu's 12.5 s cadence is longer: there each ring counts as the first,
and only S0=1 answers.

## Adaptive answer

`ctest -L adaptive` runs calls into the open modem answering with
`AT+FAA=1`. That is adaptive answer: a data answer, in which the modem
finds out whether the call is a fax or a data call
([at_commands.pdf](at_commands.pdf)).

- **Fax senders.** Each modem's class 1 fax sender calls it, answering in
  fax mode (and the open modem's sender also with it in data mode). The
  answerer must say FAX before its CONNECT (`ANSWER_B`), and then the page
  is checked.
  - The open modem, Smart Link's library, and the Broadcom, Intel and
    HD-audio modems are recognised by their CNG (calling tone).
  - The Agere and Conexant modems stop their CNG as soon as they hear our
    ANSam. The burst cut short also tells.
  - The open modem's sender without CNG (`DSP_FAX_CNG=0`), silent until
    the DIS, into fax and into data mode: FAX at the end of the automode
    chain (after Bell 103's 3 s).
- **A V.8 fax call.** spandsp's V.8 with the call function of a fax
  machine (`SPANDSP_V8_CALL=4`), into data mode: FAX, and no data call.
- **Data callers.** The Agere modem at V.34 and V.22bis, Smart Link's at
  V.34, spandsp's at V.22bis and V.21, the SM56 and Conexant modems at
  V.90, and the open modem at V.92, in data mode and in fax mode: DATA,
  then the data call.
- **Voice mode.** The open, Agere and Conexant fax senders call the open
  modem in voice mode, on the line with `+FAA=1` (`ANSWER_VOICE`):
  `<DLE>c`, FAX, the class 1 answer on the call, then the page. (Smart
  Link's library sends no CNG on a silent line, so it is not among them.)
- **Slow legacy callers,** silent until they hear their own answer signal
  (the Agere modem forced to Bell 103, V.21 or V.22, without V.8), into
  fax mode with the whole automode chain: each answers its stage, and the
  result is DATA.

## MNP

The Agere, SM56, PCTel, HSF, Broadcom, Intel and ESS modems have MNP 2 to
4 and MNP 5 (Smart Link's library and spandsp have none). With their
defaults they all choose V.42 with us.

Forced to MNP, each connects with MNP 4 and MNP 5 both ways (`ctest -R
mnp`, 20000 octets each way):

- calling with MNP alone (`+ES=4,4,6`), the peer's LR is taken by our
  default answerer in the detection phase;
- answering with MNP alone (`+ES=3,0,6`), the peer takes our LR (we call
  with `\N5`).

Peculiarities:

- The PCTel modem accepts no LR offering N401 256. Ours offers 64, with
  the 256-octet optimization, as theirs do.
- The SM56 modem sends its LR 1.1 s after the connection. Our answerer
  with MNP waits three times T400 for it.
- The HSF modem sends LTs without information with MNP 5.
- None of them has MNP 10 or 10EC.

See [link-details.md](link-details.md#mnp) for MNP, and for what MNP
10EC would take.

## V.80

With `interop_call`'s `V80=1` (`interop_call.h`), the call's data is
V.80 frames:

- Both DTEs ask for Synchronous Access Mode (`+ES=6` calling, `+ES=,,8`
  answering).
- After CONNECT each goes over to the framed sub-mode (`<EM><flag>`),
  sends a warm-up frame that need not arrive, then 20 frames
  (`V80_FRAMES`) of 1 to 250 octets, with every octet value among them.
- The other DTE must get each frame whole, ended by `<EM><flag>`.

The calls are V.34, with our transmitter at 28800 bit/s at most: V.80 has
no error correction, and a renegotiation in the middle (the SM56
modem's, from 33600) costs frames.

The tests (`ctest -R _v80_`), and what each modem does:

| Modem | `+ES=?` | FCS (`+ESA`'s `<crc_type>`) | Calls | What its DCE does |
|---|---|---|---|---|
| Agere | `(0,1,2,3,4,5-6),...,(0-3,4,5,6,7-8)` | none, FCS-16 | both ways, each FCS | It drops the FCS it checked, but counting the octets of its shielded output: an FCS octet 19h or 99h, shielded, leaves a stray EM before `<EM><flag>` (the frame itself arrives whole). With FCS-32 its DCE sends nothing and gives its DTE nothing |
| Broadcom | the same | none, FCS-16 | both ways, each FCS | The same V.80 code as Agere's: the same answers and the same stray EM |
| SM56 | `(0-4,6),(0-4),(0-6,8)` | none, FCS-16 (`+ESA:(0),(0),(0-1),(),(0-1),...`) | it calling with FCS-16; answering with none | It gives its DTE the received frames with their FCS (two octets more). Answering, after any `+ESA` command it no longer answers. Its receiver starts hunting only with its own first frame |
| Intel | `(1-4,6),(0,2-4),(1,2,4-6,8)` | none only | both ways, its reception only | It receives as it should. Its transmitter sends 16 1s in place of each frame's last two octets (an FCS field with no FCS), which a receiver takes for an abort |
| HSF, PCTel, ESS | no 6 or 8 | | none | No Synchronous Access Mode (ESS accepts `+ES=6` and makes a plain call) |

Every receiver took our transmitter's frames whole, with each FCS it
offers (our FCS-16 is theirs). Ours took theirs whole, except Intel's.

The checks can be relaxed per side:

- `V80_STRAY_A=1` (`_B`) lets that side's DTE get the stray EM;
- `V80_EXTRA_A=2` (`_B`) lets it get the FCS after each frame;
- `V80_ONLY=A` (`B`) checks one side's frames alone;
- `V80_PAD_A=<octets>` (`_B`) puts octets after that side's frames. For
  Intel's two lost last octets: with `XY` after each frame, the frame
  itself arrives whole.

`half_intel` gives the Intel controller the DTE's characters as they come
while online, as its serial port's interrupt did. When they were given
from its background task alone, every 15 ms, its V.80 transmitter ran
out in the middle of frames.

### The transparent sub-mode's SYN hunt

`V80=2` keeps both DTEs in the transparent sub-mode, with `+ESA`'s
`<trans_idle>` set to 1 (the 8-bit SYN, 16h) or 2 (16h then 96h). Each
DCE idles with the SYN, and the other's receiver must find it and give
its DTE octets aligned on it, starting with the SYN, from the CONNECT on.

Then each DTE sends 3000 octets, which the other's must get whole, with
only the SYN idle before them. `V80_HUNT=1` then has A's DTE send
`<EM><hunt>`, and B's 1000 octets more. Without the hunt (`<trans_idle>`
0) our DTE gets `8a 76 b3 ff ...` before the SYN, and the check fails.

The tests (`ctest -R _v80_syn`):

| Modem | `+ESA=?` | Calls | What its DCE does |
|---|---|---|---|
| Agere, Broadcom | `(0-2),(0,1),(0,1),(0,1),(0-2),(0,1),(0-255),(0-255)` | both ways, both SYNs | It finds our SYN, and we find theirs. Its DTE gets DC1 and DC3 shielded, and it loses the first of two octets in a row that it shields: `13 13` and `99 13` arrive as `<EM><t4>` alone (`V80_PAIR_LOST`: the octets sent to it have no such pair). The Agere modem calling, with the 16-bit SYN, gives its DTE the idle from a `<syn2>` on (`V80_IDLE_FIRST`); with octets of ours that start with the SYN, it gives the SYN first |
| SM56, Intel | `<trans_idle>` 0 only | none | |

Two open modems run the same SYN hunt calls (`open_v80_syn1`,
`open_v80_syn2`).

## The call's statistics in the middle of a call

`EXPECT_MIDCALL_A=<regex>` (`_B`) checks what a modem answered to its
`MIDCALL` commands, as `EXPECT_A` checks its log.

`open_v90a_stats` escapes both D-Modems in V.90 data, on a network with
a 3 dB pad, a robbed bit and the analogue modem's codec 100 ppm fast, for
`AT&V1` or `ATI11`, and `AT#UD`. It checks:

- each direction's symbol rate and carrier (8000 and PCM downstream, 3429
  baud and 1959 Hz upstream);
- the upstream's trellis code, on each side;
- the clock 100 ppm off as each modem sees it (the analogue modem's
  -100, the network being slower; the digital modem's +100);
- the pad and the robbed bit as the analogue modem's DIL found them;
- the same in Unimodem's keys (`AT#UD`'s `DIAG` lines), keys 20 and 21
  per direction: V.34's `C` upstream and PCM's `E` downstream.

## Retrains and renegotiations on request

`MIDCALL_ATO_A=<cmd>` (`_B`) makes `interop_call`'s mid-call return to
online use that command instead of `ATO`: `ATO1` retrains, and `ATO3`
renegotiates the rate. `ctest -R dte_` runs them with `STREAM=25` and an
escape 6 s in:

- V.34 (`AT+MS=V34`) retrains, both ways, against Agere, Broadcom, SM56,
  PCTel, HSF, Intel and ESS (`<peer>_34_dte_retrain_open_calls`,
  `<peer>_34_dte_retrain_<peer>_calls`); and renegotiations with us
  calling Agere and Broadcom (`<peer>_34_dte_reneg_open_calls`). Each
  peer answers, and the data carries on whole.
- Between two of ours: V.90 as the analogue modem
  (`open_v90a_dte_retrain`, `open_v90a_dte_reneg`) and as the digital
  modem (`open_v90_dte_reneg`); and V.92 with PCM upstream, as either
  (`open_v92_dte_reneg`, `open_v92a_dte_reneg`, `open_v92_dte_retrain`).

The Agere, Broadcom and HSF modems answer our retrain's tone B with tone
A plus an 1800 Hz tone of the same strength. Until the tone detector
allowed for it ([dsp/v34-details.md](dsp/v34-details.md)), our retrains
with them failed with no tone A.

## The calls' options

**`ANSWER_AFTER=<s>`** gives the answerer its ATA that long after the
caller's ATD, as a real line rings only once the number is dialled. The
quick connect tests dial (`DIAL`), and the Agere modem waits 2 s off hook
before its digits; answering at once, our ANSam would be over before it
listens.

**`MODEM_REC=<prefix>`** makes `half_open` and `half_smartlink` record
their line input and output (`<prefix>.<name>.in` and
`<prefix>.<name>.out`, where the name is `MODEM_NAME`'s: `modemA` and
`modemB` in an interop call).

**`DBG=<level>`** sets the debug level the interop calls run the modems
with (3 by default).

**`WAV=<file>`** makes `interop_call` save the call as audio: a stereo
16-bit WAV at 8000 Hz of the line where it crosses the simulated network
(after G.711; A, the caller, to B on the left, and B to A on the right),
from the first dial to the end of the data (or of the call that failed).
It needs `NET` (netsim writes the file, `NET_WAV`); with `CALLS`, the
calls in between are in it as well. For example:

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

With them, the modems take the DTE's commands only every that many line
samples (data, once connected, goes in at once), run their timers between
half frames, and keep time by the line. Without them, where the commands
meet the line depends on the scheduler.

A data call sets `PIPE_SYNC=4800` unless it is already set, and each
peer's `<NAME>_SYNC` to `PIPE_SYNC`'s value unless that is set
(`interop_call.cpp`, `data_call()`); a fax call sets `PIPE_SYNC=192` (see
[Smart Link's fax](#against-the-smart-link-soft-modem)). `0` turns the
sync off.

With `PIPE_SYNC`, `half_open` and `half_smartlink` also run in real time
while on hook, their line running from the start (the other harnesses'
lines do anyway). That way the commands meet the samples they would meet
in real time, even when the call itself runs unpaced (see
[testing-details.md](testing-details.md)).

`AGERE_SYNC_PHASE` (each peer's `<NAME>_SYNC_PHASE`) moves that side
within the period, to try the alignments one by one.
