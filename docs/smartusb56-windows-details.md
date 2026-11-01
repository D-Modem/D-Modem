# The SmartUSB56 under Windows: Creative's Modem Blaster V.92 USB: technical details

This is the technical companion to [The SmartUSB56 under Windows](smartusb56-windows.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

This is what Smart Link's Windows drivers for the ST7554 USB modem do, as
shipped by Creative for the **Modem Blaster V.92 USB (DE5671)**: driver
3.00.04 (October/November 2002) for Windows 98, ME, 2000 and XP. It was
read from the binaries with REA and objdump (see
[Method](#method)); nothing in the package was run. The hardware is
the one `dmodemd --usb` drives ([hardware.md](hardware-details.md#smart-link-smartusb56)),
from Smart Link's Linux driver (`st7554.c` of slmodem 2.9.11). This page
is mostly about what Windows does that Linux does not.

Marks: **[seen]** means seen in a file or in the code; **[inferred]**
means worked out from names, use or context, and not proven.

Terms used on this page:
- **ST7554**: ST's USB bridge chip in the SmartUSB56. It carries the
  codec's samples over USB and has the GPIOs.
- **GPIO**: a general-purpose input/output pin of the ST7554. The modem's
  hook relay, ring detector, LEDs and line controls hang on them.
- **DAA** (data access arrangement): the circuit that connects the modem
  to the telephone line. A *silicon DAA* puts it in a chip.
- **SSI1, SSI2**: the ST7554's two serial ports to codecs: SSI1 for the
  modem's codec, SSI2 for an optional second (voice) codec.
- **FIFO**: the ST7554's sample buffers, one per codec.
- **HAL**: Smart Link's hardware abstraction layer, between the soft
  modem and the USB driver.
- **INF**: a Windows driver package's setup file.

## Contents

- [The device](#the-device)
- [The software stack](#the-software-stack)
- [USB](#usb)
- [The ST7554's registers](#the-st7554s-registers)
- [The GPIOs](#the-gpios)
- [The codecs: STLC7550 and the ST75951 silicon DAA](#the-codecs-stlc7550-and-the-st75951-silicon-daa)
- [Start-up](#start-up)
- [The line: hook, ring, caller ID, pulse dialling](#the-line-hook-ring-caller-id-pulse-dialling)
- [The interface to the soft modem](#the-interface-to-the-soft-modem)
- [The soft modem](#the-soft-modem)
- [Countries](#countries)
- [Installation and the user-mode tools](#installation-and-the-user-mode-tools)
- [Windows 98 and ME](#windows-98-and-me)
- [What this means for dmodemd](#what-this-means-for-dmodemd)
- [Open questions](#open-questions)
- [Method](#method)

## The device

| | |
|---|---|
| USB ID | `0483:7554` (ST's), the same as every SmartUSB56 [seen] |
| Name | "Creative Modem Blaster V.92 DE5671", hardware version DE5671 [seen] |
| Chips | ST7554 USB bridge; an STLC7550 codec with a discrete DAA, or an ST75951/2 silicon DAA [seen] |
| Line rate | 9600 Hz (`HALDriver\RateIn`, `RateOut` = 0x2580 on every OS) [seen] |
| Features (INF) | `SLDevice="MV (CID\SPK)"`: voice modem with caller ID and speakerphone [inferred from the INF's own token list] |

The XP INF also matches `UNIMODEM92F4CB06`, a Unimodem legacy-detection
ID [inferred]. On ME the USB device gets a synthetic child
`USB\VID_0483&PID_FF10` that the modem INF binds to. On 98 the VxD
creates a virtual devnode `SLUSB&VID0483&PID7554`.

## The software stack

On 2000 and XP (ME runs the same `.sys` files) [seen]:

```
 COM port (Unimodem)
   mtlmnt5.sys    virtual serial port and AT command processor ("clModem")
   mtlstrm.sys    the soft modem: data pumps, V.42/V.44, voice, caller ID
   ntmtlfax.sys   fax (class 1)
   slnthal.sys    HAL: sample blocks, rate converters, the period timer
   slnt7554.sys   the USB driver: ST7554 registers, iso streams, GPIOs
   slwdmsup.sys   loads the others on demand (its API export is a stub)
```

- mtlmnt5 loads mtlstrm, and mtlstrm loads slnthal and ntmtlfax. Each
  gets the next one's function table through one private request
  (0x1B2004).
- slnthal finds the USB driver at `\Device\SLinkUsbMon<n>`. The two
  swap C++ objects through internal IOCTL 0x222040, and from then on all
  calls are direct (below).
- The INF installs slnt7554 and mtlmnt5 as lower filters under
  Unimodem. Six kernel services in all.

## USB

**Configuration** (`BasicUsbDevice::ConfigureDevice`, 0x138b4) [seen]:
- Configuration 0, interface 0, alternate setting from the registry
  value `Alt` (the INF sets 1):
  - Alt 1: three pipes: interrupt IN, isochronous OUT, isochronous IN.
  - Alt 2: the same pipes, for the voice codec.
  - Alt 3: five pipes, modem and voice codec.
- An interrupt endpoint with wMaxPacketSize 3 is treated as 4
  (`FixEndpointAttributes`).
- SET_FEATURE(DEVICE_REMOTE_WAKEUP) at configuration time.
- No bulk transfers. The bulk methods have no code behind them.

**Isochronous streams** (`KIsoBidiChannel`) [seen]:
- 2 OUT and 2 IN transfers of 40 packets (1 ms each) are in flight.
  Linux's driver, and dmodemd, use 3 transfers of 5 packets
  (`ISO_TRANSFERS`, `ISO_PACKETS` in `frontend/dmodemd/usb_line.h`). Property 0x5A
  changes the count.
- First a 2-byte OUT "start" packet at the current frame + 10. IN
  transfers then go at explicit frames from + 13, OUT ones ASAP.
- OUT packet sizes come from a fixed pattern, not from the IN lengths.
  At 9600 Hz it is 20, 20, 20, 18, 18 bytes per 5 ms (`tPacketer`,
  0x1c303). Linux and dmodemd instead send as many bytes as came in.
- Drift between the codec and the USB clock is corrected from the
  codec FIFO's level:
  - below the low threshold, `EnlargePacket` drops one packet and
    moves its bytes into the first;
  - above the high threshold, `ReducePacket` adds a 2-byte packet;
  - a 3-step cool-down follows each correction.
- Lost OUT bytes are sent again (unless `Debug.NoRestore`). A frame
  overrun resynchronises at the current frame + 4.
- `TakeFrameControl`, `GetFrameLength` and `ChangeFrameLength` can trim
  the host controller's SOF period. `Calibrate` measures the codec
  clock against it in ppm and stores `MasterClock`, but only with
  `Calibration=1`. The package ships patched Microsoft UHCI drivers for
  XP (usbuhci.sys QFE) and 98 (uhcd.sys QFE) [seen]; that they exist for
  this is [inferred].

**Interrupt endpoint**: one 4-byte transfer always pending
(`TranslateInterruptData` 0x1d6ae, `OnHardwareInterrupt` 0x1589c) [seen].

| Bits | Meaning |
|---|---|
| 0–15 | FIFO status. SSI1: 0x80 period tick, 0x40 underrun, 0x20 overrun, 0x10 and 0x1000 low/high level [inferred]. SSI2: 0x08, 0x04, 0x02, 0x01, 0x100 |
| 16–31 | GPIO events: bit 0 ring, bit 2 handset |

## The ST7554's registers

The vendor requests are the Linux driver's [seen]:
- Write: VENDOR | DEVICE request, `bRequest` = register, `wValue` =
  value, `wIndex` = 0, no data.
- Read: `bRequest` = register | 0x80, 2 bytes back.

The driver issues one request at a time and keeps a shadow copy of
every register. It sends a write only when the value changes (or when
forced), and reads most writes back. It refuses registers other than
these (`RequestCodeFromReg`, 0x17863) [seen]:

| Reg | Windows name | Linux name | Notes |
|---|---|---|---|
| 0x00 | REVISION | – | read-only; below 0x200 RHOM is forced off |
| 0x01 | AFE_POWER | SSI1_CONTROL | codec power |
| 0x02 | SSI_CTRL | SSI2 control | voice codec power: 0x21 or 0 |
| 0x03 | FIFO_MASK | FIFO_MASK | 0x00E0 while streaming, which adds the 0x80 period tick (Linux and dmodemd: 0x0060) |
| 0x04 | AFE_COUNTER | SSI1_COUNTER | the period: rate × ms / 1000 − 1, e.g. 47 for 5 ms at 9600 Hz (Linux and dmodemd: 96) |
| 0x06 | SSI_COUNTER | SSI2_COUNTER | the voice codec's period |
| 0x08 | GPIO_DIR | GPIO_DIR | |
| 0x0A | GPIO_OUT | GPIO_OUT | |
| 0x0C | GPIO_MASK | GPIO_MASK | interrupt enables |
| 0x0E | GPIO_INV | GPIO_INV | always 0 |
| 0x15 | GPIO_STATUS | GPIO_STATUS | read-only |
| 0x17 | codec control word | SSI1_CWORD | |
| 0x19 | FIFO_CTRL_LOW | FIFO_CONTROL1 | bits 8–14 SSI1 low threshold, 0–6 SSI2's; bit 15 byte swap (clear with `Debug.DoNotSwap`) |
| 0x1A | FIFO_CTRL_HI | FIFO_CONTROL2 | bits 8–14 SSI1 high threshold, bit 15 SSI1 "RHOM"; bits 0–6 and 7 the same for SSI2 |

The FIFO thresholds (low/high) while streaming are 0x20/0x3E at 8000 Hz,
0x20/0x3A at 9600 Hz and 0x0E/0x2E at 16000 Hz. Linux writes 0x28/0x28,
and so does dmodemd [seen].

RHOM (`ConfigureRHOM` 0x163e8 → `EnableRHOM` 0x1d872) only sets
FIFO_CTRL2's bit 15, and bit 7 for the voice codec when `Alt` ≥ 2. It
is on when `Options.RHOM` is set (the INF does not set it, and
`ConfigureDevice` clears it below revision 0x200) and the soft modem
has the stream enabled (`VModemLine::Enable`, `Disable`, which both
call it). What it stands for is not known [seen].

## The GPIOs

From `InitializeRegisters`, `SetHookOn`, `EnableCID`, `UpdateLEDs`,
`CalculateGPIOBits` and the Australian timer. The names are st7554.c's
[seen; electrical meanings inferred]:

| Bit | Name | Dir | Use |
|---|---|---|---|
| 0x0001 | RING1 | in | ring detector; its interrupt is on while rings are wanted |
| 0x0002 | – | in | read at codec start-up; reported upward as "HW_Restriction_LOCK" (property 0x7B) |
| 0x0004 | HANDSET | in | its interrupt is on from start-up, but the event goes nowhere |
| 0x0008 | HOOK1 | out | hook relay, 1 = off hook |
| 0x0020 | CID1 | out | **caller ID**: on-hook line monitor |
| 0x0040 | LED_CARRIER | out | lit on connect (`Options.ExtraLeds`) |
| 0x0080 | LED_HOOK | out | switched with HOOK1 |
| 0x0100 | RFC | out | International mode: pulse dialling and the "Australian timer" |
| 0x0400 | DP | out | set at start-up and kept; with International mode, on hook clears it and off hook sets it |
| 0x0800 | BUZEN | out | speaker on/off (`Options.Buzzer`) |
| 0x1000 | DC | out | DC termination, International mode only |
| 0x2000 | LED_RW | out | data activity LED (`Options.ExtraLeds`) |
| 0x4000 | MONOLITHIC | in | 1 = ST75951/2 silicon DAA |
| 0x8000 | – | out | an output; nothing writes it |

GPIO_DIR is 0xBFF8 (Linux 0x3FF8). The DE5671's INF sets
`Options.ExtraLeds=0x100`, `Options.Buzzer=0` and
`Options.International=0`. So on this modem the extra LEDs work, while
the buzzer and the International-mode bits (DP switching, RFC, DC) stay
off.

## The codecs: STLC7550 and the ST75951 silicon DAA

`K7550CodecInterface::Initialize` (0x1dabe) reads GPIO_STATUS [seen]:
- Bit 0x4000 set: an ST75951 ("Codec 75951 detected"); clear: an
  STLC7550.
- `Debug.ForceCodecDetection` overrides it: 1 forces the 7550, any
  other non-zero value the 75951.
- The result goes to the registry as `CodecModel` ("ST7550" or
  "ST75951"), with `Version.HW`.

The **ST75951 is supported** [seen]. The Linux driver refuses it;
dmodemd now starts it up as Windows does ([below](#what-this-means-for-dmodemd)).
Its control word (register 0x17) addresses four 12-bit registers:

| | STLC7550 | ST75951 |
|---|---|---|
| Write | the control word as is | `reg << 12 \| (value & 0x4FFF)`, reg 0–3 |
| Read | – | write `0x8000 \| reg << 12`, read register 0x17, & 0x4FFF |
| Start-up | AFE_POWER = 1; control word 0x3C0 | AFE_POWER = 3; reg 3 = 0x050; reg 0 = 0x3C0; AFE_POWER = 2 |

Sample-rate control words (reg 0 on the 75951) [seen]:

| Rate | Control word |
|---|---|
| 8000 Hz | 0x3C8 |
| 9600 Hz | 0x3C0 |
| 16000 Hz | 0x3F0 |

There is no 24000 Hz entry (Linux has 0x3E8). `Debug.Attenuation` clears
the word's bit 0x40 and `Debug.GainIn` sets 0x04. Samples are 16 bit.

Hook, ring and caller ID use the same GPIOs on both chips; only the DC
bit's sense is inverted on the 75951. The driver writes no country or
line registers into the 75951. Its line settings stay at their reset
values [seen].

`K7554VoiceCodecInterface` is a second codec on SSI2 [seen]: 8 kHz only,
8-bit samples, powered through SSI_CTRL = 0x21, period in register 6. It
is used only with `Alt` ≥ 2; the INF sets 1. Inference: a handset or
speakerphone codec on other boards. More [seen]:
- `Format()` is 3 (the STLC7550's 1), `BytesPerSample()` 1; nothing
  calls `Format()`, so which 8-bit format it is (G.711, or linear) is
  not known.
- `SetSampleRate` takes 8000 only; `SetPTCPeriod` writes register 6 =
  rate × ms / 1000 − 1; `Initialize` writes SSI_CTRL = 0; `Power(1)`
  writes 0x21.
- `UpdateCodecPowerMode` powers it with the STLC7550 when `Alt` ≥ 2.
- Its FIFO bits are SSI1's counterparts: underrun 0x04, overrun 0x02 in
  FIFO_MASK and on the interrupt; thresholds in bits 0–6 of registers
  0x19 and 0x1A.
- `ConfigureDevice` gives the channels their pipes: Alt 1 the codec's
  pipes 1 and 2, Alt 2 the voice codec's 1 and 2, Alt 3 the codec's 1
  and 2 and the voice codec's 3 and 4.
- Only the driver's test interface (`IoControl`, codes 0x10A, 0x10D and
  0x10E with the voice flag; trace names `IsoModem`, `IsoVoice`,
  `TransferTest`) streams the voice channel; the soft modem's
  `VModemLine` interface has the codec's channel only, and
  `CaptureHandset` is a stub. Device property 4 changes `Alt`.

## Start-up

`BasicUsbDevice::InitializeRegisters` (0x1648a) [seen]:

| Step | Register | Windows | Linux |
|---|---|---|---|
| 1 | GPIO_DIR | 0xBFF8 | 0x3FF8 |
| 2 | GPIO_OUT | 0x0480 | 0 |
| 3 | GPIO_MASK | 0x0004 (handset) | 0x0001 (ring) |
| 4 | FIFO_CTRL1 (0x19) | 0x8000 | 0xA828 |
| 5 | FIFO_CTRL2 (0x1A) | 0x2828 | |
| 6 | FIFO_MASK | 0 | |
| 7 | AFE_COUNTER | 0 | |

8. Read back every register.
9. Codec start-up (above), GPIO_INV = 0, voice codec off.
10. On hook, and rings enabled as before.

With International mode off, on hook leaves GPIO_OUT at **0x0400**: DP
is held high.

When the stream starts, the driver:
- sets the FIFO thresholds and FIFO_MASK = 0x00E0;
- writes the period counter once the start packet is done;
- puts 96 samples of silence (`Debug.Delay`) into the input.

The codec is powered only while the port is open. Stopping writes no
registers; Linux clears the masks, GPIO_OUT and the power. Resume
configures everything from scratch. A background thread runs every
500 ms to:
- apply registry changes;
- run `Calibrate` (with `Calibration=1` only);
- re-arm the interrupt pipe.

Registry values the driver reads (`CheckForRegistrySettings` 0x15e72)
[seen]:
- **Options.\***: RHOM, International, ExtraLeds, Buzzer,
  RejectPowerQuery, SurpriseRemovalOK.
- **Other settings**: `Offset`, `Alt`, `PowerScheme`, `InstallOptions`,
  `Calibration`, `MasterClock`.
- **Debug.\***: DoNotSwap, Delay, AddDelay, RedDelay, DC (an ADC offset
  subtracted from every input sample, `RemoveDC`), Attenuation, GainIn,
  NoRestore, NoRateCorr, SmallBuffers, ForceCodecDetection,
  RFCPerDigit, SoftDSP, NeverTurnOff, Send/NoWaitWake, and the trace
  levels.

## The line: hook, ring, caller ID, pulse dialling

**Hook** (`SetHookOn`, 0x17634; property 2) [seen]:
1. If caller ID is on, it is turned off first ("Force CID off due to
   hook access").
2. HOOK1 and LED_HOOK (0x0088) are set off hook and cleared on hook,
   as Linux does.
3. Only in International mode:
   - DP is set off hook; DP and RFC are cleared on hook (except while
     pulse dialling).
   - DC is set off hook when the country asks for it (inverted on the
     75951).
   - The Australian timer starts off hook and is cancelled on hook.
4. On hook, the carrier and activity LEDs go off.

Hook flash (property 0x38) is a stub. The modem above flashes by going
on and off hook [inferred].

**Ring** [seen]:
- Property 1 only sets or clears GPIO_MASK bit 0.
- Each interrupt report with GPIO bit 0 set is passed up as one ring
  event. The cadence is judged above, in mtlstrm, from the country's
  ring parameters. This is what dmodemd does.
- With wake armed and rings enabled, suspend keeps the ring interrupt
  and remote wakeup on, so **a ring wakes the PC**.

**Caller ID** (`EnableCID`, 0x175a6; property 3) [seen]:
- It sets or clears GPIO_OUT bit 0x0020 (CID1), and that is all. Any
  hook change turns it off first.
- Inference: CID1 connects the codec to the line on hook (a line
  monitor path), so the soft modem hears the FSK between rings in the
  ordinary sample stream.
- The soft modem decodes caller ID: Bellcore/V.23-style FSK type I, and
  call-waiting caller ID during modem-on-hold (`AT#CID=1`) [seen in
  mtlstrm/mtlmnt5].
- Linux's st7554.c defines GPIO_CID1 but never uses it. **So the
  SmartUSB56 hardware can do caller ID**, which D-Modem's hardware notes
  used to say it could not.

**Pulse dialling** [seen]:
- The USB driver has no make/break timing. The soft modem pulses the
  hook through property 2, with the country's make and break times
  [inferred].
- Property 0x22 marks each digit (`OnDigitPulseDialing`), and 0x6E
  ends dialling.
- Only in International mode: RFC is held high while dialling. It drops
  at the end, or after each digit with `Debug.RFCPerDigit`.

**The Australian timer** (`SpawnAustralianTimerIfNeeded` 0x23d3c,
`OnAustralianTimer` 0x23cb8) [seen]:
- In International mode, when the country asks for it, RFC is held high
  for **350 ms** after each on→off hook change, then cleared.
- It is not started while pulse dialling and is cancelled on hook.
- Inference: RFC switches the DAA's DC characteristic for the
  off-hook transient Australian approval wants.

**Voice and speaker** [seen]:
- The modem has no speaker. `SetSpeakerVolume` (levels 0–3) only
  switches BUZEN, and only with `Options.Buzzer`.
- Audio routing and volume (`SetAudioSource`, `SetAudioIn/OutVolume`)
  are stubs.
- Input gain and output attenuation accept only 0.
- The speaker, and the speakerphone and handset, are done by the
  host: slextspk.dll plays the line through the PC's sound card (below).

**Not in this hardware** [seen, from the property handlers]: no loop
current or line voltage measurement, no line-in-use detection, no
country DAA registers (AC impedance, ring threshold, current limit).
Properties 0x4A, 0x4B, 0x55, 0x64, 0x65 (line settings), 0x77 (line
current), 0x53 (serial number) and 0x54 ("Protocol Factor") reach the
driver's default case. The soft modem has them for other Smart Link
hardware. Inference: GPIO 2 (HANDSET) could tell a parallel phone off
hook.

## The interface to the soft modem

slnthal and slnt7554 exchange objects (internal IOCTL 0x222040). The
HAL gets a `VVirtualModioCard` with SetProp, GetProp, Start, Stop,
ModemClose and HandsetClose. It must report interface version 0x106.
The USB driver calls the HAL back with:
- 1: ring;
- 2: period tick;
- 4: failure;
- otherwise FIFO flags: 0x2 data ready, 0x10 overrun, 0x20 underrun.

Each period tick runs one cycle of the data pump. The period is 48 or
192 samples at 9600 Hz (5 or 20 ms), or 40 or 160 at 8000 Hz [seen].

**Start** takes the in and out rates (equal, and 8000, 9600 or 16000)
and the period in samples.

SetProp numbers the USB driver handles (`BasicUsbDevice::SetProp`
0x16a78, `VModemLine::SetProp` 0x1caeb) [seen]:

| Prop | Effect |
|---|---|
| 0x01 | rings on/off |
| 0x02 | hook |
| 0x03 | caller ID monitor |
| 0x04 | period (samples) |
| 0x06 | speaker (buzzer) |
| 0x08, 0x0A | attenuation, gain (0 only) |
| 0x1E | port open/close (codec power) |
| 0x22 | pulse-dialling digit |
| 0x2C | delay reduction |
| 0x57 | country hardware bits: bit 0 DC on off hook, bit 1 Australian timer |
| 0x5A | packets per transfer |
| 0x5B | "disbalance" |
| 0x5C | registers again |
| 0x63 | prepare to stop |
| 0x69 | connected: carrier LED |
| 0x6A | data activity LED |
| 0x6E | pulse dialling done |

GetProp:
- 0 gives capability bits: 0x1E277, or 0x5E277 with `Debug.SoftDSP`.
- 0x0B gives the delay: `Debug.Delay` + `Debug.AddDelay` = 126 samples.

Diagnostics: DeviceControl 0x222000 (`BasicUsbDevice::IoControl`
0x20166). The `SLUSBMON` control device forwards the same codes. They
give:
- raw register and control-word access, and raw requests;
- pipe reset/abort and iso read/write;
- host-controller frame-length changes;
- transfer tests and interrupt logs.

Internal IOCTLs 0x1B0004 and 0x1B0008 arm and disarm wake on ring.

## The soft modem

mtlstrm and mtlmnt5 are Smart Link's modem, the same design as the
slmodem library D-Modem descends from [seen]:
- **Versions**: V90Modem 2.93 (12 September 2002), V92Modem 1.1
  (9 April 2001), V8 of 3 May 2002, with floating-point/MMX pumps.
  slmodem 2.9.x is about 1.5 years newer (V90 2.98, fixed point).
- **Modulations**: V.21, V.22/V.22bis, V.23, V.32/V.32bis, V.34,
  K56flex, V.90, V.92 (quick connect, modem-on-hold, PCM upstream),
  Bell 103/212.
- **Error control and compression**: V.42, MNP 2–5, V.42bis, V.44.
- **Fax**: class 1, to V.17 14400.
- **Voice**: `#VSR` 7200/8000/11025 Hz, 4-bit Rockwell ADPCM or 8-bit
  PCM (`#VBS`), about 30 `#` commands (`#CLS`, `#VLS`, `#SPK`, `#VTX`,
  `#VRX`, `#VTS`, `#CID`, …).
- **Other commands**: `#UD` diagnostics, `*B` blacklist, `ATI0`–`7`;
  `ATI2000` answers `SLHAMR56`.
- **Defaults**: PCM upstream off (`PcmUpstreamEnabled=0`); quick
  connect, modem-on-hold and V.44 on.
- The HAL has 26 rate converters (8000/9600 ↔ 10–48 kHz, MMX or float).
  `ESSM` lets the HAL change its period without stopping the stream
  [inferred].

## Countries

The INF installs only the USA. `Setup.scp`'s `[Homologation]` section,
written to the registry by slgen.dll at install, has **72 country
records of 193 bytes** [seen]. Each is a
`HostBasedModemData\Parameters\DPDriver\Country\HomologationParameters\<NAME>`
value with `VersionID=2710`. The modem reads `CountryName` and that
record and nothing else. Without a record it falls back to the USA.

**Choosing the country.** `CountryName=AUTO` is resolved by the control
panel applet (slcpappl.cpl), run after install by smcfg.exe [seen]:
- 2000: the dialling code `iCountry` is looked up in its string table.
- XP: the GEOID `Geo\Nation` is looked up, while `AutoXP` = 1, which is
  then reset.
- Fallback: `IfLocaleNotFound` (USA).

**The record.** The fields of D-Modem's `homolog_params`
(core/modem_homolog.h) are all in the record, at other offsets. Their
values agree with core/homolog_data.cpp in every country but one:
Singapore's hook-flash time is 50 there and 9 in D-Modem. The table
below gives those fields' offsets and the extra fields the code reads
by name. Offsets are mtlstrm's getters [seen]:

| Offset | Field |
|---|---|
| 0, 1, 2 | pulse make time, break time (ms), digit pattern |
| 3, 55 | DTMF high-tone level; high/low difference |
| 4, 5, 91 | S11 (DTMF duration) min, max, default |
| 10, 11 | S0 (auto-answer rings) min, max (USA 255, most of Europe 4) |
| 12–15, 73–81, 126, 127 | ring detection (cadence, tolerances, cycles) |
| 16–20 | busy cadence: min on, max on, cycles, min off, max off |
| 37 | S24 hook flash (10 ms) |
| 38, 39, 86 | S6 (dial tone wait) min, max, default |
| 40, 41, 92 | S7 (carrier wait) min, max, default |
| 42 | S8 comma pause |
| 47, 60, 61 | dial modifier validation; A–D allowed; comma limit |
| 53, 58, 59 | dial tone: validation time, filter, threshold |
| 69, 70, 71, 72, 84 | hardware properties 0x4A, 0x4B, 0x64, 0x65, 0x55 (line settings; not used by this modem's driver) |
| 83 | busy tone filter |
| 85 | gap between pulse digits (S148) |
| **90** | **hardware property 0x57** |
| 100–111 | ringback detector (all 0) |
| 112, 113 | call progress buffer length (6, 66 → 666) |
| 114 | noise filter |
| **130** | **T.35 country code** (S252, `+GCI`) |
| 132–134, 142 | modem-on-hold timing |

The rest map to S-register defaults and limits: S10, S16, S53, S72,
S92, S144, S148, S149, S200–S204, S241.

Byte 90 is the one this modem's hardware takes [seen]:
- bit 0 (DC on off hook) is set for France, Algeria and Tunisia;
- bit 1 (the Australian timer) only for Australia (0x62).

Its other bits vary by country (UK 0x10, Germany 0xE0, Japan 0x20).
Inference: they are an impedance or termination code for Smart Link's
other DAAs. With the DE5671's INF (`Options.International=0`), even
bits 0 and 1 are ignored.

Byte 130 has Windows-side codes for a few countries: Baltic states
0xB8, CTR21 and Slovenia 0xFF, Slovakia 0x2E, Taiwan 0x26, South Korea
0x61. Groups of identical records:
- USA, Argentina, Bahrain, Chile, Peru, Saudi Arabia, South Korea,
  Uruguay;
- Mexico, Philippines, Vietnam;
- Greece, Iceland, Luxembourg;
- CTR21 Europe, Estonia, Turkey.

There is a combined CZECH_REPUBLIC/SLOVAKIA record. Colombia,
Venezuela, Bolivia, Ecuador and Paraguay are named in the applet but
have no record.

## Installation and the user-mode tools

**2000/XP** (Mdmntstm.inf, WHQL-signed, 3.00.04.638 of 24 October
2002) [seen]:
- Installs the six services. Its co-installer (coinst.dll) runs
  `rundll32 SLGen.dll,SLAddExtra`, which carries out Setup.scp: it
  unpacks setup.cab, writes the country table and makes backups.
- It also creates the auto-start service `SLService` (slserv.exe). This
  asks the HAL for the modem's registry path, then keeps `slrundll.exe
  SLGen.dll,SLRunServ` running. That loads every enabled
  `HostBasedModemData\Services\<dll>@<entry>`: the software speaker and
  the lights.

The tools talk to the drivers through `DeviceIoControl(0x222000)` with a
32-byte header `{cmd, len, 0, status, p1..p4}` [seen]:

| Tool | What it does |
|---|---|
| slextspk.dll | "Software Speaker": reads the speaker samples from the HAL and plays them with waveOut (8 kHz, 8 bit; 16 optional; 800-byte blocks); can also record the microphone for the speakerphone |
| slcpappl.cpl | control panel: country, speaker (soft speaker, +6 dB), modem-on-hold, V.92 (quick connect, V.44, PCM upstream); hidden HAL and DSP pages (sample rates, ESSM, MMX, any S-register) |
| sllights.dll | tray lights, connection info, modem-on-hold and call-waiting pop-ups with caller ID, an "excessive line current" warning |
| SLMH.exe | "Modem Helper" diagnostics over AT commands: loopback (`ATS32=253`, `ATS253=6`), dial tone, line current (`AT[P118?`/`[P119?`), clock accuracy (`AT#UD` field 128, ppm × 10) |
| smcfg.exe | after install: runs the country selection when the country is AUTO |
| MiniRec.exe | log recorder |
| winddx.sys | Smart Link's trace sink (`\\.\WINDDX0`), used by all the drivers |
| amr_cpl.dll | AC'97 modem pages, not hooked up here |

The help file (SLCPAPPL.chm) only covers the Modem Helper.

## Windows 98 and ME

- **ME** runs the 2000/XP drivers. `slwdmhlp.sys` (a VxD) loads them and
  stands in for slwdmsup.sys, with Microsoft's ccport.sys and
  wdmmdmld.vxd as the port. Its USB node sets `Debug.DoNotSwap=1` and
  `Debug.SoftDSP=1`.
- **98** loads the same WDM `slnt7554.sys` under NTKERN, so the
  hardware code is the same everywhere. The rest are VxDs:
  - slv7554.vxd: devnodes and the COM port;
  - mtlhal.vxd: the HAL;
  - mtlstrm.vxd and mtlfax.vxd: the modem and fax;
  - mtlport.vxd: the VCOMM port;
  - mtlmdm.vxd: state, lights, line-current test values, restrictive
    dialling;
  - slld.vxd: the loader.

  98's INF is 3.00.04Y2 (24 September 2002), unsigned.

mtlhal.vxd names the HAL's hardware properties: `RingEn`, `HookOn`,
`CallerID`, `CIDData`, `SampleRate`, `AttenOut`, `GainIn`, `NearTrip`,
`MakeTime`, `BreakTime`, `Dial`, `LoopRLoop`, `HSOpen`, `VoiceEn`,
`TxOffset`, `RxOffset`, … [seen]. `CIDData` suggests some hardware
decodes caller ID itself; this one does not [inferred].

## What this means for dmodemd

`frontend/dmodemd/usb_line.cpp` follows Linux, with more taken from Windows
(items 1, 2, 6, 7, 8, 9, 10, 11 and 12). Against Windows:

1. **Caller ID: done.** Caller ID is off by default (`AT#CID=0`). With
   `AT#CID=1`, from the first ring dmodemd sets GPIO 0x0020 (CID1)
   while the core listens on hook (`MODEM_HOOK_SNOOPING`), and clears it
   before the hook changes (`UsbLine::set_hook`). Tested on the fake
   device, not yet on hardware.
2. **The ST75951 silicon DAA: done.** dmodemd detects it from
   GPIO_STATUS bit 0x4000 and starts it up as Windows does
   (`UsbLine::start_st75951`): power 3, codec register 3 = 0x050,
   register 0 = 0x3C0, power 2, then power 3 and the rate's control
   word. It powers it down to 2. Hook, ring and caller ID use the same
   GPIOs. The DC bit, inverted on the ST75951, is used only with
   `--usb-international` (item 9).
3. **The idle GPIO state differs**: Windows holds DP (0x0400) high.
   The Linux values work, but DP's role is not known.
4. **Clock drift**: Windows adds or drops whole packets from the FIFO
   level. dmodemd's IN-paced OUT lengths do the same job.
5. **Countries**: the 72 records fit D-Modem's table; only Singapore's
   hook flash differs. The DE5671 applies no DAA settings per country.
   Australia's 350 ms RFC and France/Algeria/Tunisia's DC bit apply only
   in International mode, which this modem's INF leaves off.
6. **LEDs: done, behind `--usb-leds`.** The carrier LED (0x0040)
   while the call is online and the activity LED (0x2000) once data
   has passed, both off on hook. Windows drives them only when the INF
   says the board has them (`Options.ExtraLeds`), so dmodemd only with
   the option.
7. **Wake on ring: done, behind `--usb-wake`.** The remote wakeup is
   armed at the start (Windows arms it when the PC sleeps), and dmodemd
   survives the suspend: it sets the device up again after a resume, as
   Windows does.
8. **The handset detector: done, behind `--usb-handset`.** Windows
   enables GPIO 2's interrupt but drops its events. dmodemd reads the
   GPIO on each change and reports the phone on the modem's jack off or
   on hook (V.253's `<DLE>H` and `<DLE>h` in voice mode). That set means
   off hook is inferred.
9. **International line controls: done, behind `--usb-international`.**
   DP, DC (per country, inverted on the ST75951), the Australian timer
   and RFC through pulse dialling, as Windows' `SetHookOn`,
   `CalculateGPIOBits`, `SpawnAustralianTimerIfNeeded` and
   `OnDigitPulseDialing` do them. The countries are those of the table's
   byte 90 (France, Algeria, Tunisia; Australia).
10. **The voice codec: done, behind `--usb-voice-codec`.** Alternate
   setting 3, SSI2 powered up and its period set as Windows does; the
   handset is V.253's `+VLS=14` and `15`. Windows itself never streams
   it outside its tests, and the sample format is a guess the option
   names.
11. **RHOM: done, behind `--usb-rhom`.** As `ConfigureRHOM`: FIFO_CTRL2
   bit 15 (bit 7 for the voice codec when it streams) while the soft
   modem has the stream enabled (`VModemLine::Enable` to `Disable`;
   dmodemd: the core's `start()` to `stop()`), never below revision
   0x200. What it does is still unknown.
12. **The buzzer: done, behind `--usb-buzzer`.** As `SetSpeakerVolume`
   (0x16d46) with `Options.Buzzer`: BUZEN (0x0800) set for any level but
   0, cleared at 0; dmodemd's levels are the core's ATM/ATL speaker.
13. **Rates**: dmodemd has Windows' three control words, for 8000,
   9600 and 16000 Hz (`cword()` in `frontend/dmodemd/usb_line.cpp`), and not
   Linux's 24000 Hz word (0x3E8). Its `-r` option takes only 8000 or
   9600 for this modem (`line_rate_ok()` in `frontend/dmodemd/options.h`), so
   16000 Hz is not reachable from the command line.

## Open questions

- What DP (0x0400), RFC (0x0100), DC (0x1000) and GPIO 0x0002 do
  electrically.
- Which boards have the SSI2 voice codec, and whether its octets are
  mu-law, A-law or linear.
- What "RHOM" stands for, and what FIFO_CTRL2 bit 15 does.
- How the soft modem produces the caller ID labels (DATE/NMBR/NAME):
  they are not plain strings in any driver.
- About 40 country-record bytes that no named code reads.
- How Windows uses the frame-length trimming in normal operation, if it
  does.

## Method

- The package's files were expanded with `msexpand` and `cabextract`.
  Nothing was run, not even under Wine.
- slnt7554.sys and mtlmnt5.sys still carry their COFF symbol tables, so
  most functions above are named.
- REA opened the binaries, but its decompiler back ends (Ghidra, IDA,
  Hopper) are not available for PE files here. So the code was read with
  REA's objdump wrapper, local objdump/ndisasm and llvm-undname, and
  the PE resources with pefile.
- Addresses are virtual addresses at the files' preferred base
  (0x10000).
- This is static analysis only: there is no harness for these drivers
  yet, unlike Broadcom's BCMSM.sys (half_broadcom).
