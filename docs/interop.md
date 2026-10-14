# Interop tests against other soft modems

Calls between the open modem and other soft modems: Smart Link's on
slmodemd's old core, the Agere, SM56, PCTel and HSF ones each in user
space on an emulation of its hardware, Broadcom's Windows driver on a
Windows kernel of our own and an emulation of its chip, and spandsp's (see
[testing.md](testing.md) for the driver, netsim and how the calls keep
time). Each has a harness, `half_<name>`, which takes the line's audio
on fd 3, gives its own on fd 4 and is the modem's DTE on a pty, and a
ctest label of its own; `ctest -L <label> -j16` runs its calls.

## The peers' binaries

The vendor modems are their own binaries, as their Linux drivers shipped
them, in `dsp/test/<peer>/` (i386 objects; the harnesses link them in,
`-m32`); Broadcom's is its Windows driver, which `half_win` loads. Each
is the file of a known release, compared byte for byte where the release
can still be had:

| Peer | Files | Release | Built | Byte-identical to |
|---|---|---|---|---|
| Smart Link | `smartlink/dsplibs.o` | slmodem-2.9.11-20110321 | 22 Sep 2005, GCC 3.4.2 | `modem/dsplibs.o` of Debian's `sl-modem_2.9.11~20110321.orig.tar.gz` |
| Agere | `agere/agrsm_core.o` | Agere SoftModem 2.1.40, from agrsm-20070804 | 16 Nov 2005, GCC 3.3.5 (Debian) | none to be had (below) |
| SM56 | `sm56/sm56.ko` | Motorola sm56-06.09.05 for Fedora Core 3 (2.6.9-1.667) | 17 May 2005, GCC 3.4.2 (Fedora Core 3) | `usr/tmp/sm56.ko` of `sm56-06.09.05-1-fc3-2.6.9-1.667.i686.rpm` |
| PCTel | `pctel/dsp.o`, `pctel/control.o` | pctel-0.9.7-9 (Linux driver K2.4.7.V92.001) | pumps 21 Jun 2001, GCC 2.95.3; controller dated 26 Mar 2003, GCC 2.96 (Red Hat 7.1) | `src/lib/amr/dsp.a`, `src/lib/control.a` of pctel-0.9.7-9-rht-6 |
| HSF | `hsf/hsfengine.o`, `hsfmc97ich.o`, `hsfsoar.o`, `hsfmc97ich.inf`, `hsf.cty` | Linuxant hsfmodem-7.80.02.06oem | 10 May 2010, GCC 3.4.6 (Red Hat) | `modules/imported/<name>-i386.O`, `nvm/inf/hsfmc97ich.inf`, `nvm/inf/hsf.cty` of `hsfmodem-7.80.02.06oem.tar.gz` |
| Broadcom | `broadcom/BCMSM.sys`, `broadcom/BCMSMwxp.inf` | BCM V.92 56K Modem 3.4.19, Dell's "V.92 Soft Modem (Data Fax) Driver" A05 | 26 Jul 2002, Microsoft linker 7.0 | the hashes in the package's catalog, signed by Microsoft (WHQL, 31 Jul 2002) |
| spandsp | (the system's libspandsp) | spandsp 0.0.6 (release 20110122) | | |

The files' sha256, and what was compared:

- Smart Link: `dsplibs.o`
  `1f3e56d0dfae1a6aaf4eb6fcc4875a4524905e010d5758114cde288b3cf0b379`.
  The tarball (`cea496e9c34a16cf347124b27ed673f3a6e87089f83e15ab5e0b0365a56fd253`)
  is at `https://deb.debian.org/debian/pool/non-free/s/sl-modem/`. The
  library came with slmodemd 2.9.11 in D-Modem's first commit.
- Agere: `agrsm_core.o`
  `a00a7fe7a21c2b20a353bf373042c3e30599ff4a9238a061acb494bb559e171c`.
  It says "Agere SoftModem Version 2.1.40", "Built on 11/16/2005
  16:43:35", and of its PCM module "AGERE PCMLL V2.1.37.105, seed =
  1220, 2/1/2004". agrsm-20070804 is a dated package of linmodems'
  "sv92" series (linmodems.technion.ac.il, gone), which carries Agere's
  2.1.40 (its successor, agrsm-20080203, answers `ATI` with the same).
  Agere's own builds of 2.1.40 for Mandrake, Red Hat and SuSE (December
  2005, `agrmodemlib.o` on the Kaiomy Pinball-C driver CD at archive.org)
  have the same 3132 global symbols, but other compilers, and
  `LXHardwareInfoCreate` and `LXHardwareInfoDestroy` global. Ours has
  those two weak (the harness has its own): the object was rewritten
  after Agere's link, so no file of Agere's matches it byte for byte.
- SM56: `sm56.ko`
  `a757fd85bc27cc4bd816073efa9d03591307973de5c5aabcb37e0bb225ba516a`,
  with `vermagic=2.6.9-1.667 686 REGPARM 4KSTACKS gcc-3.4`, "SM56 Rel.
  6.09 Build 05" and "Copyright 1998-2004, Motorola Inc.". The RPM
  (`1f24ba64dae3901de3fe497ec47f6490b7756558cd9ec26f568cc475b0da13d0`,
  Vendor Motorola, built 2005-05-17, its checksums good, unsigned) is
  not to be had any more: Motorola gave it to distributors and OEMs
  only, and its copies at linux-xp-brasil.com.br and
  linmodems.technion.ac.il are gone.
- PCTel: `dsp.o`
  `15d8e5ab1ac4019a029fd9e562e0e74b3f233aa3ab42621334506721e66f82b1`,
  `control.o`
  `38d46cabd67f675f8c011afd4725a41a1a813075bae4b516c94cda1b4dfec69d`:
  upstream's `.a` files renamed (they are plain objects). The pumps'
  `timeStamp` is "Jun 21 2001,14:27:03"; the controller says "Linux
  Driver K2.4.7.V92.001 (03/26/2003)" and of the DSP "LCE-336V version
  .2.15.4.". The copy compared
  (`f1304bbcefd1b88dc1733421f7850598334eaf62c5b132c8d129af3405d1ddfb`)
  is
  `https://archive.org/download/pctel-0.9.7-9-rht-6_for_Ubuntu-2.6.15-23-386/`,
  Robert Thornburrow's 0.9.7-9-rht-6 of 2006 as a third party uploaded
  it; its ChangeLog changes no binaries after 0.9.7-8, so 0.9.7-9 and
  its rht releases carry the same two.
- HSF: `hsfengine.o`
  `6d570356f37e62153374f2f50b5d3a3e4b1246285199cbba2aa7d631a5f894fc`,
  `hsfmc97ich.o`
  `b6c18aab4585498700b401b0948161f25ee18e6863af48b2b2f35b0536b60c25`,
  `hsfsoar.o`
  `3be23c8e6d6ee8f6bc92b89913590ad4828cf2013ba4cf9abc3be854c22ecde9`,
  `hsfmc97ich.inf`
  `0d7401ddcefd2c362adcdc19fb0b828312983a8e3970c2389596e2ad271a970c`,
  `hsf.cty`
  `bf205eda7dc753a416c347179e2058cdb1a15f053afbc609fe25995ca00eba34`.
  The tarball
  (`7388bf4338f434aa65363f2097a9f9c2c87f523ac1a549764de831b68f37846c`,
  released 9 May 2010) is at
  `https://www.linuxant.com/drivers/hsf/oem/downloads.php`. The objects'
  symbols are `cnxthsf_7800206oem_<name>`: the "full" build's differ
  (`cnxthsf_7800206full_`), and Linuxant no longer offers it. The `.inf`
  is Conexant's Linux_Intel_SmartMC.inf, "Version: 5.03.27"; the country
  file says "Version: V7.72.00".
- Broadcom: `BCMSM.sys`
  `033c6716ef63b1984a8f7a7b36c8efce1c7d9ec622dd97132d54eb5b8a5c0662`,
  `BCMSMwxp.inf`
  `34f4059d27803bb1b4947ff6dc1f4b872230eaf738551f8c267ba6f50e363ca8`,
  from Dell's package "Communications:V.92 Soft Modem (Data Fax)
  Driver", version A05 (OEM version 3.4.19.1.1, for the Dimension 2200
  to 8300, created 27 Aug 2002; Windows 2000 and XP). The driver answers
  `ATI3` with "BCM V.92 56K Modem Version 3.4.19"; its debug path is
  `C:\releases\3_4_19_1_1\softmodem\build\Win32\opt-dell-wdm-pentium-mmx\bin\BCMSM.pdb`,
  and the INF's `DriverVer` is 05/21/2002,3.4.19. The package's catalog,
  `BCMSMwxp.cat` (signed by Microsoft Windows Hardware Compatibility
  Publisher, 31 Jul 2002), lists the driver's Authenticode SHA-1
  (`3261275f43309bd1155178d2549b1496ac9552f2`) and the INF's SHA-1
  (`74a33b6de90b0d12e3b688a29c94cc1dd399ea51`): both files are those
  Microsoft signed. Dell's download of the package was not compared.
- spandsp: not in the tree; the build takes the system's (pkg-config
  `spandsp`), 0.0.6 in Debian and Ubuntu.

## The closed modules' stack

The Agere, SM56, PCTel and HSF modules read some of their stack before
writing it. Valgrind's memcheck on the harnesses (linked `-static`: on
the dynamic ones it wants i386 glibc's debug symbols; and SM56's `cli`,
`in` and `out` come to the harness as SIGILL under it, not SIGSEGV)
shows where:

- Agere: `ATProcessorStatusHandler`, from `LinkLayerStatusHandler`,
  tests a value of the status `V32DataPumpReportStatus` and
  `V22DataPumpReportStatus` build on their stack that they leave unset;
  `CodecStatusHandler`'s text (`StatusParser`, its `sprintf`) takes
  unset bytes of `ReportConnectionStatus`'s.
- SM56: `CM_ConfigReqEvent` tests a value `ACU_DialResponse` leaves
  unset on its stack (in four of the five calls run under it).
- HSF: two of the engine's routines (`hsfengine564_`, under its timer,
  and `hsfengine2004_`) test locals they have not set.
- PCTel: `+MS` (below).

What they read is what the calls before left there, the harness's own
among them: a read or write of the pty, which the driver's timing
decides, could leave other bytes there, and the same line samples could
give another call. Each harness clears 8 KB of the stack below its frame
before each call into its module (`dsp/test/half_stack.h`), so a module
finds the same there every time. `HALF_STACK_FILL=<byte>` fills it with
that instead of 0: the Agere, SM56 and HSF tests (and PCTel's, with
their top rate given) pass, connect at the same rates and retrain the
same with 0, 0xa5 and 0xff, so what those modules read unset does not
decide their calls here. For
PCTel's `+MS` it did.

## Against the Smart Link soft modem

`half_smartlink` runs Smart Link's soft modem: its DSP library
(`test/smartlink/dsplibs.o`, binary-only, i386: the library slmodemd was
written for, whose interface the open DSP keeps, `dsp/dsplibs.h`) on
slmodemd's old C core (`test/legacy/`), which it calls into. It needs no
hardware: the core gives it the line's samples at 9600 Hz as ours gives
the open pumps theirs (`half_smartlink8`, at 8000 Hz, is built as well,
but the library does not work at that rate). `ctest -L smartlink -j16`
runs the tests; `smartlink_log.py` decodes the library's own debug lines
(`docs/dsp/v92.md`).

The library is slmodem-2.9.11-20110321's (above). Its parts give their
own versions: the V.90 modem 2.98 (25-Mar-04, "Modified Quick Connect
without Memory + Train Time + Constel Power"), the V.92 modem 1.1
(9-Apr-01, "Memory cleanups + dil descriptor crash fix"), a "V.92 Lite"
build. slmodem's changes after 2.9.11 are to its drivers only. Results
at the time of writing:

| `AT+MS=` | Smart Link caller → open answerer | open caller → Smart Link answerer |
|---|---|---|
| 34 (V.34) | CONNECT 33600 | CONNECT 33600 |
| 132 (V.32bis) | CONNECT 14400 | CONNECT 14400 |
| 122 (V.22bis) | CONNECT 2400 | CONNECT 2400 |
| 22 (V.22) | CONNECT 1200 | CONNECT 1200 |
| 212 (Bell 212A) | CONNECT 1200 | CONNECT 1200 |
| 103 (Bell 103) | CONNECT 300 | CONNECT 300 |
| 23 (V.23) | CONNECT 1200 | CONNECT 1200 |
| 21 (V.21) | CONNECT 300 | fails: its V.21 answerer only sends 2225 Hz and does not connect to itself either (not in the tests) |

V.34 through a line hit (300 ms of silence both ways in the data:
`smartlink_34_hit_*`) retrains both modems and V.42 carries on; it
answers our V.34 rate renegotiation (`smartlink_34_reneg_*`).

As the analogue modem, calling ours (the digital modem, 8000 Hz) through
netsim's G.711 network, it connects with V.90 at 56000/31200, µ-law and
A-law (`smartlink_v90_*`, [dsp/v90.md](dsp/v90.md), with the rate
renegotiations both ways). Its V.92 is a "V.92 Lite" build, which
retrains when offered PCM upstream; `SMARTLINK_V92FULL=1` sets its full
V.92 bit, and it then does PCM upstream with us: 56000/38666 (µ-law) and
40000 (A-law) with our prefilter, quick connect, rate renegotiations and
fast parameter exchanges both ways (`SMARTLINK_FPE_AT=<s>` starts its
own that far into the data), modem-on-hold, and the exchange for its
timing slip (`smartlink_v92_*`, [dsp/v92.md](dsp/v92.md)).

Class 1 fax both ways at 9600 Hz (`smartlink_fax_*`):

| Modulation | Smart Link sender → open receiver | open sender → Smart Link receiver |
|---|---|---|
| V.27ter 2400, 4800 | pass | pass |
| V.29 7200, 9600 | pass | pass |
| V.17 7200, 9600, 12000, 14400 (long train for TCF, short for the page) | pass | pass |

It makes a few bit errors with some of its own signals (hence the
driver's 1e-3 allowance on a page), and its V.27ter training contains
stray 180 degree phase flips, which the open receiver follows. Its V.17
receiver takes a short train or not depending on how its 20 ms frames
fall on it, so the fax calls give the DTE's commands every 192 samples
(`PIPE_SYNC=192`, `docs/testing.md`).

## Against the Agere soft modem

`half_agere` runs the Agere Systems soft modem library
(`test/agere/agrsm_core.o`, Agere SoftModem 2.1.40 from agrsm-20070804,
above) in user space on an emulated Intel ICH modem controller and
Si3038 codec; `ctest -L agere -j8` calls it from and to the
open modem. Results at the time of writing:

| Agere `AT+MS=` / ours | Agere caller → open answerer | open caller → Agere answerer |
|---|---|---|
| V34 / 34 | CONNECT 33600 (our transmitter; we receive at 24000) | CONNECT 33600 / 24000 |
| V32B / 132, V32 / 32 | CONNECT 14400, 9600 | CONNECT 14400, 9600 |
| V22B / 122, V22 / 22, B212 / 212 | CONNECT 2400, 1200, 1200 | the same |
| V23C / 23, V21 / 21, B103 / 103 | CONNECT 1200/75, 300, 300 | the same |
| V90 / 90 (ours as the digital modem), mu-law and A-law | CONNECT 46666 | - |

Class 1 fax at 8000 Hz (`agere_fax_*`): V.27ter, V.29 and V.17 (7200 to
14400, short train pages) pass both ways.

The original library in its default V.92 mode does V.8 with automode off,
and connects to this implementation at V.34.

## Against spandsp's data modems

`half_spandsp` is a modem around spandsp's pumps (the system's
libspandsp, 0.0.6, the one Asterisk and FreeSWITCH use): V.21, V.23, Bell 103 and V.22/V.22bis,
asynchronous 8N1, a few AT commands, and V.8 by default. spandsp has no
modem of its own: the `data_modems` of its later versions is a sketch,
and their V.32bis (V.17's pumps with V.32's scramblers, no handshake) and
V.34 (no data receiver yet) cannot connect. `ctest -L spanpeer -j8`
(not in CI's `spandsp` label) calls it from and to the open modem at
each modulation it has, with `AT+MS=` forced on both sides:

| `AT+MS=` | spandsp caller → open answerer | open caller → spandsp answerer |
|---|---|---|
| 122 (V.22bis) | CONNECT 2400 | CONNECT 2400 (after our LAPM attempt, about 20 s) |
| 22 (V.22) | CONNECT 1200 | CONNECT 1200 (the same) |
| 23 (V.23) | CONNECT 1200/75 | CONNECT 1200/75 |
| 21 (V.21), 103 (Bell 103) | CONNECT 300 | CONNECT 300 |

It has no error correction, which found two faults in ours:

- Our V.42 detection phase counted T400 (750 ms) in bits at a nominal
  28800 bit/s: 9 s at 2400 bit/s, 18 s at 1200, 72 s at 300. It is at
  the pump's rate now.
- Our caller goes to LAPM without the detection phase (V.42 allows it),
  and when the answerer has no V.42 it sent XID N400 times and then
  waited for nothing. It now goes on in async (`LapmCallerAsyncRemote`).

V.8 between the two does not connect yet: ours offers only V.34 and
V.32bis in its CM, and as the answerer it goes to V.32bis automode on a
CM without V.34 instead of sending a JM for the modulation both have.
spandsp, for its part, sends a V.92 octet before its CM (a QC1a with all
fields 0, which ours answers) even with V.92 off, and 0.0.6 sends the
CM's modulations back in its JM unless the application trims them
(`half_spandsp` does).

## Against the Motorola SM56 soft modem

`half_sm56` runs Motorola's SM56 soft modem (`sm56.ko` of sm56-06.09.05,
built for Fedora Core 3's kernel 2.6.9: controller and data pumps, V.92)
in user space. The module is linked in as it is, its kernel imports
renamed `k_<name>` (`sm56_obj.cmake`) and provided by the harness, which
emulates an Intel ICH modem controller with a Motorola "MOTf" codec on
trapped port I/O, patches the module's few `current` reads, and resamples
between the line's 8000 Hz and the codec's rate (7200 Hz idle, 9600 Hz
for V.90). The module is `test/sm56/sm56.ko` as Motorola shipped it (sha256
`a757fd85...`; that build only, the patched places are checked); the
build renames its imports into `sm56k.o`. `ctest -L sm56 -j8` runs the
tests. `SM56_DEBUG=1` prints its `printk`s.

Its interrupt and deferred routine save the FPU (`fnsave`) to
`.rodata.cst4` + 0x640 and + 0x644, which runs past that section into
whatever the linker puts after it. With the module linked first, that is
its own constants, and its V.90 interpolation then reads far outside its
buffers. The harness points those four instructions at buffers of its own.

| SM56 `AT+MS=` / ours | SM56 caller → open answerer | open caller → SM56 answerer |
|---|---|---|
| V34 / 34 | CONNECT 33600/LAPM (we receive at 24000) | CONNECT 33600 / 24000 |
| V32B,0 / 132 | CONNECT 14400 | CONNECT 14400 |
| V92 (its automode) / 122; V22B,0 / 122 | CONNECT 2400/LAPM | CONNECT 2400 |
| V22,0 / 22, V21,0 / 21 | CONNECT 1200, 300 | the same |
| V90 / 90 (ours as the digital modem), mu-law and A-law | CONNECT 50666/31200 | - |

Below V.34 its `+MS` wants automode off (`AT+MS=V22` alone is an
error). Forced to V.22bis that way (`AT+MS=V22B,0`), it does not connect
as the caller to a V.22bis answerer that sends the answer tone (ours, the
Agere modem's): its unscrambled ones detector starts during the 2100 Hz
tone and takes it for them, and it sends S1 456 ms after the tone ends.
With the 75 ms silence of V.22bis that is while it already trains on the
answerer's unscrambled ones, so it misses the answerer's S1 and goes on at
1200 bit/s, sending S1 again every 2 s. With 600 ms of silence, or none
(the original library answers without the tone), it connects at 2400. In
its automode, its default, it calls our V.22bis answerer at 2400, so that
is the test.

The harness adds noise to the codec's input (`SM56_NOISE`, rms 5, about
-76 dB of full scale): on exact zeros the module's frequency estimate
stays at its initial 1800 Hz, which its answering automode takes for a
V.32 caller's AA. It then leaves ANSam within 40 ms for V.32's AC, and
a V.34 caller never hears an answer tone. Rms 3 is not enough; rms 10
costs V.90 a step (45333).

Its V.92: QC1a in V.8 on every call, and on the second call (`CALLS=2`)
quick connect with short phase 2. Its upstream stays V.34: the module has
PCM upstream (`UAPCMP3_*`), but chooses it only in quick connect's INFO1a
(`SC_QC_Phase2_Set_Info_1A`), from INFO1d's PCM upstream bit, which its
full phase 2 (`SC_Phase2_Create_Tone_A_Set_1A`) clears after reading it.
Forced on, that path crashes the module (`SC_Modulate`). Its short phase
2's INFO1a (3200 baud) says high carrier (bit 33, Table 19/V.92), but it
sends on the low one: its upstream's spectrum is centred at the same
1850 Hz as after a full phase 2, where our INFO1d asked for the low
carrier (1829 Hz; the high one is 1920). A receiver on the high carrier
never finds its S, and it retrains (`sm56_v92_qc` fails on a retrain
after short phase 2).

Modem-on-hold: its own request (`AT+PMHR`) we grant; it goes on hold
and comes back with quick connect. Ours it takes for a retrain, as the
Agere modem does. It answers MHreq only where its phase 2 begins, in place
of INFO0d (`SC_Phase2_Detect_Info_0` looks for both). An MHreq sent at its
retrain reversal does get an answer: MHnack with `+PMHT=0` (its default),
and MHcda then clears the call down. With `+PMHT` set, it waits for an
accept from its platform (the handset, `MO_HoldStatePlatformInput`). After
MHfrr it does not reconnect with V.8, and expects something else after
silence. Up to that point a remote that knows nothing of MH behaves the
same way, and an MHreq at the reversal stalls the original library and the
Agere modem (they take it for our reversal), so we keep the
retrain-follow.

## Against the PCTel HSP soft modem

`half_pctel` runs PCTel's HSP soft modem (pctel-0.9.7-9, "HSP56
MicroModem ... K2.4.7.V92.001 (03/26/2003)": the Intel AMR build of its
data pumps, `lib/amr/dsp.a`, and its controller, `lib/control.a`) in user
space. Unlike the Agere and SM56 modems they need nothing of a kernel: the
two objects (`test/pctel/dsp.o` and `control.o`, PCTel's files renamed)
import only their hardware layer and the virtual UART's registers and
queues, which the harness provides in C after the driver's `i8xxhal.s`
and `vuart.s`. Its interrupt runs `dspMain()` and `modem_main()` once per
block of codec samples: the pumps take the block with `RdASIC` and give
theirs with `WrASIC`, at the rate and block size they set (7200 Hz and 24
samples idle). `GlobalTimer` is the blocks' time in ms. `ctest -L pctel
-j8` runs the tests. `PCTEL_DEBUG=1` prints what the HAL is asked and the
controller's choice of pump (`sharedRam` + 0x1c, + 0x20) against the
pump running (`curModem`).

The controller reads some of its stack before writing it. `+MS`
(`plus_ms`) copies the carrier's name (`ext_ms` + 0x1b) to its stack
without the 0 after it, and compares the copy with "V21" ... "V90",
"V92", "K56FLEX": the name matched only when the byte after the copy
happened to be 0, and otherwise `+MS` left the carrier as it was. The
harness clears the stack below its frame before each call into the
objects (they use some 1 KB of it; it clears 8), so the copy ends in 0,
as it was meant to. What was there before came from the harness's own
calls: a print added to the harness, or the stack filled with 0, turned
`pctel_v90_calls` from 50666 to 45333 every time. With the name matched,
`+MS=V90` and `+MS=V92` take the top rate from the command (`ext_ms` +
0x86): without one (0, under 28000) they set S37 to 0, automode, and
keep the downstream to drn 14, 45333; `+MS=V90,1,300,56000` sets S37 to
14, V.90, and the call trains at 50666. The tests give the top rate.

At some starts of a call it misses our INFO0d and repeats its INFO0a,
unacknowledged, every 82 ms (in a sweep of the call's start over nine
periods of the commands, one start, with the harness's codec noise at
its 5 rms; at 0 or 2 rms the same start trains). We had no recovery for
that outside quick connect, and the call waited in phase 2 until the
driver gave up: `pctel_v90_calls` failed so about once in six full runs,
where the machine's load moved the call's start (the driver's setup
commands land a period later when it answers late). Now our INFO0d goes
again until it takes one (dsp/v34.md). `pctel_v90_lose_info0` loses our
first one on purpose.

| PCTel `AT+MS=` / ours | PCTel caller → open answerer | open caller → PCTel answerer |
|---|---|---|
| V34 / 34 | CONNECT (we receive at 24000) | CONNECT 24000 |
| V32B,0 / 132 | CONNECT 14400 | CONNECT 14400 |
| V90,1,300,56000 / 90 (V.90, the open modem digital) | CONNECT 50666/33600 | - |
| (its automode) / 122 | CONNECT 2400 | CONNECT 2400 |
| (its automode) / 22 | CONNECT 1200 | CONNECT 1200 |
| (its automode) / 23 | CONNECT 1200/75 | CONNECT 1200/75 |
| (its automode) / 21 | CONNECT 300 | CONNECT 300 |

It reports the DTE's rate (115200) in its CONNECT. Against itself and
the SM56 modem it connects at V.34 both ways (33600 with SM56).

Its low speed modulations work in its automode (no `+MS`), both ways
against our forced V.21, V.22, V.22bis and V.23 modems (answering, it
sends V.22bis's unscrambled ones after its answer tone). Forced to one of
them (`+MS=V21,0` and so on) it does not: answering, it is silent after
its answer tone. (These were open while the tests forced it; our V.22bis
answerer, which once took 10 s over its V.22bis caller, connects in 8 s
now.) With V.8 it always offers V.34, whatever `+MS` says.

V.34 with it as the answerer failed at first: it retrained phase 2 instead
of answering our INFO1c with INFO1a, every time. Its INFO decoder (state
in `g34`) never found the frame's start: we went from L2 straight into
INFO1c, so the frame's first bit was a phase change from our last
control channel symbol before L1. We now send two symbols of B first, a
phase reference (the Agere and SM56 modems and the original library did
not mind either way).

V.90 with it as the analogue modem connects (50666/33600, µ-law and
A-law). It used to retrain in phase 4, 200 ms into our TRN2d, and the
call went on at V.34: its receiver measured TRN2d at 20 dB (it wants 32,
VpcmARxHandshake), and later found no usable constellation at all
(VpcmSelectCon). Its CPt already asked for little (drn 6). The cause was
the harness: during V.90 it runs its codec at FreqTable's "10287 Hz",
which is V.34's 3429 baud times 3, 72000 / 7 Hz, and the harness ran it
at 10287 Hz exactly, 125 ppm off the line's 8000 Hz. A V.90 analogue
modem's sampling has to follow the network's clock; this one does not
track such an offset. At 72000 / 7 Hz its CPt asks for drn 14 and the
call trains.

Asked for V.92 (`AT+MS=V92`) it connects with V.90, with no quick connect
or PCM upstream offer, on the first call and on a second one too: this
build has V.92 in its controller only. `+MS=V92` is taken but leaves
S37 (the modulation, `act[0x25]`) at 0, automode, so V.8 picks V.90. The
controller's V.92 path (`go_v8_v92`: its V.8 for V.92, then
`set_dsp_interface_v92`) is taken with S37=15 and S19=2; `+PQC` and `+PIG`
default to on (`V92QuickConnectFlag`, `V92UpStreamFlag`). But with
`ATS37=15 S19=2` it asks the DSP for modem 15, and the DSP (`dsp.o`,
"LCE-336V 2.15.4") knows modems 2 to 14 only (`initSys`'s switch; 14 is
V.90): it sends a CM (with V.23 duplex added) and no INFO0a after it,
and the call ends in phase 2. Nor has the DSP any of V.92's
signals (QC1a, QTS, ANSpcm, INFO0a's V.92 bits) or a PCM upstream
transmitter (Ru, TRN1u, the precoder and prefilter): only V.90's.
So there is no V.92 to test against it. It
answers ATH after +++ with NO CARRIER, which interop_call takes for the
hang up as it does OK. On hook for the driver's usual 2 s its second
call stops in phase 2 (its DSP never leaves the state after INFO0, and
never switches its codec to 9600 Hz for it); with 3 s or more it trains
(in real time, as the tests run; `HANGUP_WAIT`). `pctel_v90_calls` makes
two calls on hook for 5 s between: 50666/33600 both.

## Against the Conexant HSF soft modem

`half_hsf` runs Conexant's HSF soft modem (Linuxant's hsfmodem
7.80.02.06oem) in user space: its engine (`hsfengine`, the controller and
the data pumps), its AC'97 ICH codec module (`hsfmc97ich`) and its soft
DAA module (`hsfsoar`), the driver's i386 objects as they are in
`test/hsf/`, with the modem's `.inf` and country file for its NVM. CMake
renames each object's `.ctors` and `.gnu.linkonce.*` sections for the
object (`hsf_obj.cmake`): each has the same C++ names and the linker
would keep only one. It also makes the ICH module's microsecond busy wait
(`hsfmc97ich41_`) a weak symbol, for the harness's own (below). The
harness provides the engine's OS layer
(`cnxthsf_7800206oem_Os*`: memory, timers on the line's clock, I/O ports,
the interrupt) and its NVM (the `HKR` lines of the `.inf` and the
country's of `hsf.cty`), and models what the modules drive: an ICH's
AC'97 bus master (PCM in and out, pausing and resuming at the position it
was at), a secondary MC'97 codec (Conexant, 0x4358/0x5429) at 15999 Hz
and the SmartDAA behind its register 0x7a. The modem's `ComCtrl` is the
DTE, on a pty. The engine reads the TSC: the harness traps `rdtsc`
(`PR_SET_TSC`) and gives the line's time at 1000 MHz, on by 0.1 µs a
read while the line stands still (a busy wait would never end). The ICH
module waits some 37 µs that way about 80 times a second of the line,
370 trapped reads each: most of the modem's processor time went into
those signals. The harness's `hsfmc97ich41_` steps the
counter over the wait at once, to where those reads left it, with the
same result (a call trains the same, sample for sample); the HSF calls
take about a quarter of the time they did. `ctest -L hsf -j8` runs the
tests; `HSF_DEBUG=1` prints the modem's own messages and
the port accesses the harness does not know, `HSF_DEBUG=2` every access.

| HSF `AT+MS=` / ours | HSF caller → open answerer | open caller → HSF answerer |
|---|---|---|
| V34 / 34 | CONNECT | CONNECT |
| V32B,0 / 132 | CONNECT | CONNECT |
| V22B / 122 | CONNECT | CONNECT |
| V21,0 / 21 | CONNECT | - |
| V21, V23C, V22B, V32B (automode) / 34 (V.8) | CONNECT: V.21, V.23, V.22bis, V.32bis | CONNECT: V.21, V.23, V.22bis, V.32bis |
| V90 / 90 (V.90, the open modem digital) | CONNECT 52000/33600 | - |
| V92 / 92 (V.92, PCM upstream) | CONNECT 52000/41333 | - |

V.90 with it as the analogue modem connects at 52000/33600 over µ-law
and A-law. It failed at first: we waited for its CJ while it went on with
INFO0a. It cuts its CM short for CJ and drops its carrier right after the
last data bit of CJ's third zero octet, so the octet has no stop bit and
our V.8 receiver counted only two. Two zero octets and the start bit and
eight zero bits of a third now count as CJ (CM and JM never have two zero
octets in a row).

The ICH module ran off its ring buffer every 3.27 s at first: a word of
its device (offset 0x280, 2 once the engine runs) makes it add 8 samples
to each position it reads past 700, as for a codec that runs fast; the
harness clears it before each interrupt (`HSF_KEEP8=1` keeps it).

Its V.21 and V.23 answerers work only after V.8. Forced (`+MS=V21,0`,
`V23C,0`) it stays silent after ATA: no answer tone and no mark for a
minute, against our caller, the Agere, SM56, spandsp and PCTel modems'
and its own (all of which wait for the answerer, as V.25 has them do),
and against a 980 Hz V.21 originate mark put on the line from the start
(netsim, as a test); also with another country (`+GCI`) or the rates
given (`V21,0,300,300`), and `ATB` is not there. The harness is not the
cause: forced, its V.22, V.22bis and V.32bis answerers work, its V.21
caller does (the same FSK transmitter), and so do the V.21 and V.23
answerers of its automode, through V.8. In automode it
sends ANSam and, when no CM comes, its 1650 Hz mark, but drops it 2 s
after the caller's mark (again also against itself, calling with
`+MS=V21,0`). So our V.21 caller (`AT+MS=21`) did not connect to it
until it, too, used V.8 on its ANSam (V.21 alone; our V.22, V.22bis
and V.23 callers connect to it without). It takes our CJ only with
V.8's 75 ms without signal after it: our V.8 caller started the agreed
modulation's carrier right after CJ, and the PCTel modem went on with
JM until it gave up. About half the calls got through anyway, by
chance: the V.8 pump did not clear the rest of the block it ended in,
and what the host's buffer held from before went out, a moment of V.21
space now and then, which the PCTel modem took for the end of CJ.
Our V.8 caller does: its CM now offers V.22bis, V.23 and V.21 as
well, and a JM with one of these (the HSF modem's, answering with
`+MS=V21` and so on) gets CJ and that modulation, the FSK caller's
carrier first (as the HSF modem's own caller does). Answering, its CM up
to one of these gets our JM of it and, after CJ, that modulation.

Its V.32bis (and V.32) caller went silent after our answerer's AC/CA
exchange and never answered our R1. Its own answerer, after the reversal
that answers the caller's CC, keeps on with AC for 256 symbols before the
silence and S; ours stopped after 64, and the HSF modem missed our S.
With 128 or more it trains; we now send 256, as it does (the other
modems do not mind).

V.92 with PCM upstream connects (52000/41333, µ-law and A-law), with quick
connect (it sends QC1a on the first call already) and without (`+PQC=3`).
It failed at first in phase 3: we waited for its Su to reverse while it
waited for our Jp. Its Su, Su-bar and Su come as one short burst (Su for
144 symbols, Su-bar for only 24, then Su again until Jp), and its Su is
of Sd's shape, period 6 but its energy mostly at 4000 Hz (with quick
connect all of it; without, the Su it holds is a sine). Our receiver took
only a sine of period 6 for Su and found reversals between two 12 symbol
windows 6 apart, too far apart for a Su-bar of 24 symbols. While we send
Jd, a signal of period 6 with some of its energy at 1333 Hz is Su now,
and Su's reversals are each period's phase against Su's.

## Against the Broadcom soft modem (Windows)

`half_win` runs Broadcom's V.92 soft modem for Windows, `BCMSM.sys`
3.4.19: the WDM driver of the "BCM V.92 56K Modem", with its controller,
data pumps, V.42, V.44 and fax all in that one driver, as Dell shipped it
for its Dimension desktops in 2002 (above). Wine could not run it. Wine's kernel (`ntoskrnl.exe`,
master of October 2026) has 726 of its 1716 exports as stubs, and its
HAL 71 of 92. Its `KeInsertQueueDpc` queues nothing. `IoConnectInterrupt`,
`IoGetDmaAdapter` and `KeSynchronizeExecution` are stubs, `MmMapIoSpace`
gives NULL and the port reads give 0. START_DEVICE comes without
resources, and timers run in real time on a thread pool. A soft modem
needs all of these, and here it runs on the line's clock.

So the harness is a Windows kernel of its own, made for this driver. It
is a 32 bit program that loads the PE image at its base (0x10000),
relocates it and binds its 114 imports by name (102 from `ntoskrnl.exe`,
one of them data, 7 from `HAL.dll`, 3 from `WMILIB.SYS` and 2 from
`ks.sys`), stdcall and fastcall as there. An import the harness does not have stops the harness
with its name. Everything runs on one thread and on the line's clock:

- The interrupt time and the system time are the line's samples. While
  the line stands still each read moves them on by 1 µs, because the
  driver waits on them busily a few µs at a time.
- Timers and their DPCs run between samples, and the interrupt runs when
  the device's status and mask raise it.
- The driver's system threads are coroutines (`ucontext`). Each runs when
  the event or timer it waits for is signalled.
- IRQL is kept: no interrupt at or above the device's IRQL, no DPC at
  DISPATCH_LEVEL and above, threads at PASSIVE_LEVEL only.
- The registry is held in memory: the INF's `AddReg` sections as the
  modem class installer writes them, and `PortName`.
- fs is a KPCR (a TLS entry of the GDT, `set_thread_area`). The driver
  checks the stack it has left against its NT_TIB's limit (fs:8), which
  the harness sets for each coroutine.
- `in`, `out`, `cli`, `sti`, `rdtsc` (`PR_SET_TSC`) and moves to and from
  control registers (it reads CR0 and CR4) fault, and the harness carries
  them out.

We are the rest of the device stack. Under the driver we are the PCI bus
driver: its PDO, with the configuration space, the PnP IRPs the driver
sends down, and its resources. We are also the PnP manager (DriverEntry,
AddDevice, and START_DEVICE with the device's memory BAR and interrupt).
Over the driver we are Windows' `modem.sys`, whose lower filter the INF
makes it. To us the driver is a serial port: we open it (IRP_MJ_CREATE),
set it up with the serial IOCTLs (baud rate, line control, handflow,
timeouts, DTR, RTS), and read and write it with IRPs that carry the pty's
bytes.

Its hardware is the BCM4212, Broadcom's PCI modem chip (14e4:4212,
Dell's subsystem 1028:0001). This build (`opt-dell-wdm`) has codec code
only for CodecType 0x15, 0x19 and 0x1c, the BCM4212's; the INF gives
0x15. The driver also looks on the PCI bus for AC'97 controllers, the
ICHs that the other harnesses emulate. But for the codec types those
use, its codec operations are a stub that fails, and opening the port
then fails (STATUS_INSUFFICIENT_RESOURCES). So the harness emulates the
BCM4212. There is no datasheet to be had, so the registers below are
what the driver does with them:

| Offset | Register |
|---|---|
| 0x00 | control: 0x1000 reset, 0x60 DMA on |
| 0x20 | interrupt status, a 1 written clears it: 0x1000000 TX and 0x10000 RX (a descriptor done), 0x4000 and 0x8000 FIFO errors, 0x40 DAA |
| 0x24 | interrupt mask (the driver sets 0x101c000) |
| 0x38 | the DAA's serial port: the DAA register in bits 8 to 12, 0x2000 for a read, the data in bits 0 to 7 |
| 0x3c | the DAA read's result (bit 15: busy) |
| 0x200, 0x204, 0x208, 0x20c | TX DMA: control, the descriptor ring's address, the last descriptor posted, status (0x1000 active or 0x2000 idle, with the current descriptor's offset) |
| 0x210 to 0x21c | the same for RX |

The registers are a 16 KB memory BAR, which the driver maps with
`MmMapIoSpace`. The harness gives it a window it cannot access, so each
access faults. The harness then decodes the instruction, opens the
window with the register's value in place, and single-steps the
instruction (TF). At the trap after it, the harness takes what was
written and closes the window again.

The DMA engine is Broadcom's own, the BCM4401 Ethernet's (Linux's `b44`
driver knows it). Its rings hold 8 octet descriptors: a control word
(the length in bits 0 to 12, 0x10000000 for the end of the ring,
0x20000000 for an interrupt when done) and an address. The samples are
16 bit.

- TX: 64 descriptors of 96 octets (6 ms), with an interrupt on the last.
  The engine stops at the descriptor the driver posted last.
- RX: 128 descriptors of 4 and 92 octets in turn, end to end, with an
  interrupt for each pair.

The line side is a Silicon Labs Si3034 DAA behind the chip's serial port:

- Its PLL gives the rate (registers 7 to 9, 4 MHz · M1 M2 / (N1 N2) /
  5120). It stays at 8000 Hz in the calls here, V.90's too.
- Register 5's bit 0 is the hook.
- Register 12 is the line's state: frame detect, and the loop current in
  its low four bits. 1111 there is an overload, and the driver hangs up
  at once (it went off hook and straight back on). The harness reports
  1000, half the scale.

The harness resamples between the codec's rate and the line's
(`WIN_RATE`, 8000 Hz) and adds the codec's noise (`WIN_NOISE`, rms 5).

Two of the driver's own ways:

- It reads each setting with Windows XP's version appended first
  (`CodecFlags__5_1`), then without it.
- It reads `DisableTxRxDpc` with a handle that is not a key (an address
  in its device extension). Windows answers STATUS_INVALID_HANDLE, and so
  does the harness.

`ctest -L broadcom -j16` runs the tests. `WIN_DEBUG=1` prints the
driver's calls of note, the hook, the rate and the line's levels each
second. `WIN_DEBUG=2` also prints every register, DAA and port access,
and `WIN_DEBUG=3` every call the driver makes into the kernel.

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
octets each way through it. Class 1 fax at 8000 Hz (`broadcom_fax_*`):
V.27ter, V.29 and V.17 (7200 to 14400) pass both ways.

Its V.92 sends QC1a on every call, and its upstream stays V.34. By
default it leaves PCM upstream out (`+PIG:1`). With `AT+PIG=0` it
chooses PCM upstream in INFO1a (`broadcom_v92_pcmup`):

- Phase 3 failed at first. Its Su lasts 96T before Su-bar, where V.92
  has 144T. Our receiver took Su only after 96 coherent samples, which
  came inside its Su-bar, so it never saw the reversal. It then waited 4
  s for our Jp and gave up. Su now counts after 48 samples, and phases 3
  and 4 get through.
- After E2u it sends no B1u (48 frames of scrambled ones). With our
  usual CPd (a prefilter of 64 taps) its output turns periodic (48
  symbols) a few samples after E2u. With no prefilter (`DSP_V92_PREFILTER=0`)
  it falls silent: exact zeros, which no V.92 precoder can give, since
  the constellations exclude zero. Neither decodes as ones. We retrain,
  and the call connects at V.90 (50666/33600).

PCM upstream may well be unfinished in this build, as it is off by
default.

## The calls' options

`ANSWER_AFTER=<s>` gives the answerer its ATA that long after the
caller's ATD, as a real line rings only once the number is dialed:
the quick connect tests dial (`DIAL`), and the Agere modem waits 2 s off
hook before its digits; answering at once, our ANSam would be over
before it listens.

`MODEM_REC=<prefix>` makes `modem_half` record its line input and output
(`<prefix>.in`, `<prefix>.out`), and `DBG=<level>` sets the debug level the
interop calls run the modems with. `WAV=<file>` makes `interop_call` save
the call as audio: a stereo 16 bit WAV at 8000 Hz of the line where it
crosses the simulated network (after G.711; A, the caller, to B left, B to
A right), from the first dial to the end of the data (or of the call that
failed). It needs `NET` (netsim writes it, `NET_WAV`); with `CALLS` the
calls in between are in it as well. For example:

    NETSIM=build/dsp/test/netsim NET=9600:8000:ulaw SMARTLINK_V92FULL=1 \
      WAV=call.wav build/dsp/test/interop_call \
      build/dsp/test/half_smartlink build/dsp/test/half_open8 "ATX3;AT+MS=92"

`PIPE_SYNC=<samples>` (`half_open`, `half_smartlink`),
`AGERE_SYNC=<samples>` (`half_agere`), `SM56_SYNC=<samples>`
(`half_sm56`), `SPANDSP_SYNC=<samples>` (`half_spandsp`),
`PCTEL_SYNC=<samples>` (`half_pctel`), `HSF_SYNC=<samples>`
(`half_hsf`) and `WIN_SYNC=<samples>` (`half_win`) make a call repeat exactly from run to run: the modems take
the DTE's commands only every that many line samples (data, once
connected, at once), run their timers between half frames and keep time
by the line; without them, where the commands meet the line depends on
the scheduler. With `PIPE_SYNC`, `half_open` and `half_smartlink` also
run in real time while on hook, their line running from the start (the
others' does anyway), so that the commands meet the samples they would
in real time even when the call itself runs unpaced (see
`docs/testing.md`); `half_smartlink` then runs the old core's timers on
the line's samples as well. `AGERE_SYNC_PHASE` and
`SMARTLINK_SYNC_PHASE` move one side within the period, to try the
alignments one by one.
