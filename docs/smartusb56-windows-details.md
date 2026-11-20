# The SmartUSB56 under Windows: Creative's Modem Blaster V.92 USB: technical details

This page is the technical companion to [The SmartUSB56 under Windows](smartusb56-windows.md), the plain-English overview. Read that page first. This page gives all the details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

This page tells what the Windows drivers from Smart Link do with the
ST7554 USB modem. Creative supplied these drivers for the **Modem Blaster
V.92 USB (DE5671)**: driver 3.00.04 (October/November 2002) for Windows
98, ME, 2000 and XP. We read the binaries with REA and objdump (refer to
[Method](#method)). We did not run any file in the package. The hardware
is the hardware that `dmodemd --usb` operates ([hardware.md](hardware-details.md#smart-link-smartusb56)),
from the Linux driver of Smart Link (`st7554.c` of slmodem 2.9.11). This
page is almost all about what Windows does and Linux does not do.

Marks: **[seen]** means that we saw it in a file or in the code.
**[inferred]** means that we found it from names, use or context, and
that it is not proved.

Terms on this page:
- **ST7554**: the USB bridge chip from ST in the SmartUSB56. It moves
  the samples of the codec over USB and has the GPIOs.
- **GPIO**: a general-purpose input/output pin of the ST7554. The hook
  relay, ring detector, LEDs and line controls of the modem connect to
  these pins.
- **DAA** (data access arrangement): the circuit that connects the modem
  to the telephone line. A *silicon DAA* puts this circuit in a chip.
- **SSI1, SSI2**: the two serial ports of the ST7554 to codecs. SSI1 is
  for the codec of the modem. SSI2 is for an optional second (voice)
  codec.
- **FIFO**: the sample buffers of the ST7554, one for each codec.
- **HAL**: the hardware abstraction layer of Smart Link, between the
  soft modem and the USB driver.
- **INF**: the setup file of a Windows driver package.

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
| USB ID | `0483:7554` (the ID of ST), the same as all SmartUSB56 modems [seen] |
| Name | "Creative Modem Blaster V.92 DE5671", hardware version DE5671 [seen] |
| Chips | ST7554 USB bridge. An STLC7550 codec with a discrete DAA, or an ST75951/2 silicon DAA [seen] |
| Line rate | 9600 Hz (`HALDriver\RateIn`, `RateOut` = 0x2580 on all OS versions) [seen] |
| Features (INF) | `SLDevice="MV (CID\SPK)"`: voice modem with caller ID and speakerphone [inferred from the token list in the INF] |

The XP INF also matches `UNIMODEM92F4CB06`, a Unimodem legacy-detection
ID [inferred]. On ME, the USB device gets a synthetic child
`USB\VID_0483&PID_FF10`, and the modem INF binds to this child. On 98,
the VxD makes a virtual devnode `SLUSB&VID0483&PID7554`.

## The software stack

On 2000 and XP (ME uses the same `.sys` files) [seen]:

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
  driver gets the function table of the next driver through one private
  request (0x1B2004).
- slnthal finds the USB driver at `\Device\SLinkUsbMon<n>`. The two
  drivers exchange C++ objects through internal IOCTL 0x222040. After
  that, all calls are direct (refer to below).
- The INF installs slnt7554 and mtlmnt5 as lower filters under
  Unimodem. There are six kernel services in total.

## USB

**Configuration** (`BasicUsbDevice::ConfigureDevice`, 0x138b4) [seen]:
- Configuration 0, interface 0, alternate setting from the registry
  value `Alt` (the INF sets 1):
  - Alt 1: three pipes: interrupt IN, isochronous OUT, isochronous IN.
  - Alt 2: the same pipes, for the voice codec.
  - Alt 3: five pipes, modem and voice codec.
- The driver uses 4 as the wMaxPacketSize of an interrupt endpoint
  that has wMaxPacketSize 3 (`FixEndpointAttributes`).
- SET_FEATURE(DEVICE_REMOTE_WAKEUP) at configuration time.
- No bulk transfers. The bulk methods have no code in them.

**Isochronous streams** (`KIsoBidiChannel`) [seen]:
- At one time, 2 OUT and 2 IN transfers of 40 packets (1 ms each) are
  in progress. The Linux driver and dmodemd use 3 transfers of 5 packets
  (`ISO_TRANSFERS`, `ISO_PACKETS` in `frontend/dmodemd/usb_line.h`).
  Property 0x5A changes the count.
- First, the driver sends a 2-byte OUT "start" packet at the current
  frame + 10. Then IN transfers go at explicit frames from + 13, and
  OUT transfers go as soon as possible.
- The OUT packet sizes come from a fixed pattern, not from the IN
  lengths. At 9600 Hz, the pattern is 20, 20, 20, 18, 18 bytes for each
  5 ms (`tPacketer`, 0x1c303). Linux and dmodemd send the same number of
  bytes that came in.
- The driver corrects the drift between the codec clock and the USB
  clock from the level of the codec FIFO:
  - Below the low threshold, `EnlargePacket` removes one packet and
    moves its bytes into the first packet.
  - Above the high threshold, `ReducePacket` adds a 2-byte packet.
  - After each correction, there is a 3-step cool-down.
- The driver sends lost OUT bytes again (but not with
  `Debug.NoRestore`). After a frame overrun, the driver synchronises
  again at the current frame + 4.
- `TakeFrameControl`, `GetFrameLength` and `ChangeFrameLength` can
  adjust the SOF period of the host controller. `Calibrate` measures
  the codec clock against this period in ppm and keeps the result in
  `MasterClock`, but only with `Calibration=1`. The package contains
  patched Microsoft UHCI drivers for XP (usbuhci.sys QFE) and 98
  (uhcd.sys QFE) [seen]. The reason for these drivers is possibly this
  function [inferred].

**Interrupt endpoint**: one 4-byte transfer is always pending
(`TranslateInterruptData` 0x1d6ae, `OnHardwareInterrupt` 0x1589c) [seen].

| Bits | Meaning |
|---|---|
| 0–15 | FIFO status. SSI1: 0x80 period tick, 0x40 underrun, 0x20 overrun, 0x10 and 0x1000 low/high level [inferred]. SSI2: 0x08, 0x04, 0x02, 0x01, 0x100 |
| 16–31 | GPIO events: bit 0 ring, bit 2 handset |

## The ST7554's registers

The vendor requests are the same as in the Linux driver [seen]:
- Write: VENDOR | DEVICE request, `bRequest` = register, `wValue` =
  value, `wIndex` = 0, no data.
- Read: `bRequest` = register | 0x80, 2 bytes back.

The driver sends one request at a time and keeps a shadow copy of
each register. It sends a write only when the value changes (or when it
must). It reads back most writes. It refuses all registers that are
not in this table (`RequestCodeFromReg`, 0x17863) [seen]:

| Reg | Windows name | Linux name | Notes |
|---|---|---|---|
| 0x00 | REVISION | – | read-only; below 0x200 RHOM is forced off |
| 0x01 | AFE_POWER | SSI1_CONTROL | codec power |
| 0x02 | SSI_CTRL | SSI2 control | voice codec power: 0x21 or 0 |
| 0x03 | FIFO_MASK | FIFO_MASK | 0x00E0 while the stream is on, which adds the 0x80 period tick (Linux and dmodemd: 0x0060) |
| 0x04 | AFE_COUNTER | SSI1_COUNTER | the period: rate × ms / 1000 − 1, for example 47 for 5 ms at 9600 Hz (Linux and dmodemd: 96) |
| 0x06 | SSI_COUNTER | SSI2_COUNTER | the period of the voice codec |
| 0x08 | GPIO_DIR | GPIO_DIR | |
| 0x0A | GPIO_OUT | GPIO_OUT | |
| 0x0C | GPIO_MASK | GPIO_MASK | interrupt enables |
| 0x0E | GPIO_INV | GPIO_INV | always 0 |
| 0x15 | GPIO_STATUS | GPIO_STATUS | read-only |
| 0x17 | codec control word | SSI1_CWORD | |
| 0x19 | FIFO_CTRL_LOW | FIFO_CONTROL1 | bits 8–14 SSI1 low threshold, bits 0–6 the SSI2 low threshold; bit 15 byte swap (clear it with `Debug.DoNotSwap`) |
| 0x1A | FIFO_CTRL_HI | FIFO_CONTROL2 | bits 8–14 SSI1 high threshold, bit 15 SSI1 "RHOM"; bits 0–6 and 7 the same for SSI2 |

The FIFO thresholds (low/high) while the stream is on are 0x20/0x3E at
8000 Hz, 0x20/0x3A at 9600 Hz and 0x0E/0x2E at 16000 Hz. Linux writes
0x28/0x28, and dmodemd also writes these values [seen].

RHOM (`ConfigureRHOM` 0x163e8 → `EnableRHOM` 0x1d872) only sets
bit 15 of FIFO_CTRL2, and bit 7 for the voice codec when `Alt` ≥ 2. RHOM
is on when these two conditions are true:
- `Options.RHOM` is set. The INF does not set it, and `ConfigureDevice`
  clears it below revision 0x200.
- The soft modem has the stream enabled (`VModemLine::Enable`,
  `Disable`, which both call it).

We do not know what RHOM means [seen].

## The GPIOs

This data comes from `InitializeRegisters`, `SetHookOn`, `EnableCID`,
`UpdateLEDs`, `CalculateGPIOBits` and the Australian timer. The names
are the names in st7554.c [seen; electrical meanings inferred]:

| Bit | Name | Dir | Use |
|---|---|---|---|
| 0x0001 | RING1 | in | ring detector; its interrupt is on while the modem must detect rings |
| 0x0002 | – | in | read at codec start-up; sent up as "HW_Restriction_LOCK" (property 0x7B) |
| 0x0004 | HANDSET | in | its interrupt is on from start-up, but the driver does not use the event |
| 0x0008 | HOOK1 | out | hook relay, 1 = off hook |
| 0x0020 | CID1 | out | **caller ID**: on-hook line monitor |
| 0x0040 | LED_CARRIER | out | on when the modem connects (`Options.ExtraLeds`) |
| 0x0080 | LED_HOOK | out | changes with HOOK1 |
| 0x0100 | RFC | out | International mode: pulse dialling and the "Australian timer" |
| 0x0400 | DP | out | set at start-up and kept; with International mode, on hook clears it and off hook sets it |
| 0x0800 | BUZEN | out | speaker on/off (`Options.Buzzer`) |
| 0x1000 | DC | out | DC termination, International mode only |
| 0x2000 | LED_RW | out | data activity LED (`Options.ExtraLeds`) |
| 0x4000 | MONOLITHIC | in | 1 = ST75951/2 silicon DAA |
| 0x8000 | – | out | an output; no code writes it |

GPIO_DIR is 0xBFF8 (Linux 0x3FF8). The INF of the DE5671 sets
`Options.ExtraLeds=0x100`, `Options.Buzzer=0` and
`Options.International=0`. Thus, on this modem the extra LEDs operate.
The buzzer and the International-mode bits (DP switching, RFC, DC) stay
off.

## The codecs: STLC7550 and the ST75951 silicon DAA

`K7550CodecInterface::Initialize` (0x1dabe) reads GPIO_STATUS [seen]:
- If bit 0x4000 is set, the codec is an ST75951 ("Codec 75951
  detected"). If the bit is clear, the codec is an STLC7550.
- `Debug.ForceCodecDetection` overrides the bit. The value 1 forces the
  7550. All other non-zero values force the 75951.
- The driver writes the result to the registry as `CodecModel`
  ("ST7550" or "ST75951"), with `Version.HW`.

The **ST75951 is supported** [seen]. The Linux driver refuses it.
dmodemd now starts it up in the same way as Windows ([below](#what-this-means-for-dmodemd)).
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
bit 0x40 of the word and `Debug.GainIn` sets 0x04. Samples are 16 bit.

Hook, ring and caller ID use the same GPIOs on the two chips. Only the
sense of the DC bit is inverted on the 75951. The driver writes no
country or line registers into the 75951. Its line settings stay at
their reset values [seen].

`K7554VoiceCodecInterface` is a second codec on SSI2 [seen]: 8 kHz only,
8-bit samples, power through SSI_CTRL = 0x21, period in register 6. The
driver uses it only with `Alt` ≥ 2, and the INF sets 1. Inference: on
other boards, this is a handset or speakerphone codec. More [seen]:
- `Format()` is 3 (1 for the STLC7550), and `BytesPerSample()` is 1.
  No code calls `Format()`. Thus we do not know which 8-bit format it is
  (G.711, or linear).
- `SetSampleRate` accepts only 8000. `SetPTCPeriod` writes register 6 =
  rate × ms / 1000 − 1. `Initialize` writes SSI_CTRL = 0. `Power(1)`
  writes 0x21.
- `UpdateCodecPowerMode` gives it power with the STLC7550 when
  `Alt` ≥ 2.
- Its FIFO bits are the counterparts of the SSI1 bits: underrun 0x04,
  overrun 0x02 in FIFO_MASK and on the interrupt. The thresholds are in
  bits 0–6 of registers 0x19 and 0x1A.
- `ConfigureDevice` gives the channels their pipes:
  - Alt 1: pipes 1 and 2 of the codec.
  - Alt 2: pipes 1 and 2 of the voice codec.
  - Alt 3: pipes 1 and 2 of the codec, and pipes 3 and 4 of the voice
    codec.
- Only the test interface of the driver streams the voice channel
  (`IoControl`, codes 0x10A, 0x10D and 0x10E with the voice flag; trace
  names `IsoModem`, `IsoVoice`, `TransferTest`). The `VModemLine`
  interface of the soft modem has only the channel of the codec, and
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

8. Read back all registers.
9. Codec start-up (above), GPIO_INV = 0, voice codec off.
10. On hook, and rings enabled as before.

With International mode off, on hook leaves GPIO_OUT at **0x0400**: DP
stays high.

When the stream starts, the driver does these steps:
1. It sets the FIFO thresholds and FIFO_MASK = 0x00E0.
2. It writes the period counter when the start packet is complete.
3. It puts 96 samples of silence (`Debug.Delay`) into the input.

The codec has power only while the port is open. A stop writes no
registers. Linux clears the masks, GPIO_OUT and the power. A resume
configures all items again from the start. A background thread runs
each 500 ms. It does these tasks:
- It applies registry changes.
- It runs `Calibrate` (only with `Calibration=1`).
- It arms the interrupt pipe again.

Registry values that the driver reads (`CheckForRegistrySettings`
0x15e72) [seen]:
- **Options.\***: RHOM, International, ExtraLeds, Buzzer,
  RejectPowerQuery, SurpriseRemovalOK.
- **Other settings**: `Offset`, `Alt`, `PowerScheme`, `InstallOptions`,
  `Calibration`, `MasterClock`.
- **Debug.\***: DoNotSwap, Delay, AddDelay, RedDelay, DC (an ADC offset
  that the driver subtracts from each input sample, `RemoveDC`),
  Attenuation, GainIn, NoRestore, NoRateCorr, SmallBuffers,
  ForceCodecDetection, RFCPerDigit, SoftDSP, NeverTurnOff,
  Send/NoWaitWake, and the trace levels.

## The line: hook, ring, caller ID, pulse dialling

**Hook** (`SetHookOn`, 0x17634; property 2) [seen]:
1. If caller ID is on, the driver first sets it to off ("Force CID off
   due to hook access").
2. The driver sets HOOK1 and LED_HOOK (0x0088) off hook and clears them
   on hook, as Linux does.
3. Only in International mode:
   - The driver sets DP off hook. It clears DP and RFC on hook (but not
     during pulse dialling).
   - The driver sets DC off hook when the country tells it to (inverted
     on the 75951).
   - The Australian timer starts off hook and stops on hook.
4. On hook, the carrier and activity LEDs go off.

Hook flash (property 0x38) is a stub. The modem above does a hook flash
when it goes on hook and then off hook [inferred].

**Ring** [seen]:
- Property 1 only sets or clears bit 0 of GPIO_MASK.
- The driver sends up one ring event for each interrupt report with
  GPIO bit 0 set. mtlstrm, above, examines the cadence with the ring
  parameters of the country. dmodemd does the same.
- When wake is armed and rings are enabled, suspend keeps the ring
  interrupt and remote wakeup on. Thus, **a ring wakes the PC**.

**Caller ID** (`EnableCID`, 0x175a6; property 3) [seen]:
- It sets or clears bit 0x0020 (CID1) of GPIO_OUT, and it does nothing
  more. Each hook change first sets it to off.
- Inference: CID1 connects the codec to the line on hook (a line
  monitor path). Thus, the soft modem gets the FSK between rings in the
  usual sample stream.
- The soft modem decodes caller ID: Bellcore/V.23-style FSK type I, and
  call-waiting caller ID during modem-on-hold (`AT#CID=1`) [seen in
  mtlstrm/mtlmnt5].
- The st7554.c file of Linux defines GPIO_CID1 but does not use it.
  **Thus, the SmartUSB56 hardware can do caller ID**. Before, the
  hardware notes of D-Modem said that it could not.

**Pulse dialling** [seen]:
- The USB driver has no make/break timing. The soft modem pulses the
  hook through property 2, with the make and break times of the country
  [inferred].
- Property 0x22 marks each digit (`OnDigitPulseDialing`), and 0x6E
  ends dialling.
- Only in International mode: RFC stays high during dialling. It goes
  low at the end, or after each digit with `Debug.RFCPerDigit`.

**The Australian timer** (`SpawnAustralianTimerIfNeeded` 0x23d3c,
`OnAustralianTimer` 0x23cb8) [seen]:
- In International mode, when the country tells it to, the driver
  keeps RFC high for **350 ms** after each on→off hook change. Then it
  clears RFC.
- The timer does not start during pulse dialling, and it stops on hook.
- Inference: RFC changes the DC characteristic of the DAA for the
  off-hook transient that Australian approval requires.

**Voice and speaker** [seen]:
- The modem has no speaker. `SetSpeakerVolume` (levels 0–3) only
  sets BUZEN on or off, and only with `Options.Buzzer`.
- Audio routing and volume (`SetAudioSource`, `SetAudioIn/OutVolume`)
  are stubs.
- Input gain and output attenuation accept only 0.
- The host does the speaker, the speakerphone and the handset:
  slextspk.dll plays the line through the sound card of the PC (below).

**Not in this hardware** [seen, from the property handlers]: no loop
current or line voltage measurement, no line-in-use detection, no
country DAA registers (AC impedance, ring threshold, current limit).
Properties 0x4A, 0x4B, 0x55, 0x64, 0x65 (line settings), 0x77 (line
current), 0x53 (serial number) and 0x54 ("Protocol Factor") go to the
default case of the driver. The soft modem has them for other Smart Link
hardware. Inference: GPIO 2 (HANDSET) can possibly show that a parallel
phone is off hook.

## The interface to the soft modem

slnthal and slnt7554 exchange objects (internal IOCTL 0x222040). The
HAL gets a `VVirtualModioCard` with SetProp, GetProp, Start, Stop,
ModemClose and HandsetClose. It must report interface version 0x106.
The USB driver calls the HAL back with:
- 1: ring;
- 2: period tick;
- 4: failure;
- other values: FIFO flags: 0x2 data ready, 0x10 overrun, 0x20 underrun.

Each period tick runs one cycle of the data pump. The period is 48 or
192 samples at 9600 Hz (5 or 20 ms), or 40 or 160 at 8000 Hz [seen].

**Start** takes the in and out rates (equal, and 8000, 9600 or 16000)
and the period in samples.

SetProp numbers that the USB driver accepts (`BasicUsbDevice::SetProp`
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
0x20166). The `SLUSBMON` control device sends on the same codes. They
give:
- raw register and control-word access, and raw requests;
- pipe reset/abort and iso read/write;
- host-controller frame-length changes;
- transfer tests and interrupt logs.

Internal IOCTLs 0x1B0004 and 0x1B0008 arm and disarm wake on ring.

## The soft modem

mtlstrm and mtlmnt5 are the modem of Smart Link. They have the same
design as the slmodem library, which D-Modem comes from [seen]:
- **Versions**: V90Modem 2.93 (12 September 2002), V92Modem 1.1
  (9 April 2001), V8 of 3 May 2002, with floating-point/MMX pumps.
  slmodem 2.9.x is approximately 1.5 years newer (V90 2.98, fixed
  point).
- **Modulations**: V.21, V.22/V.22bis, V.23, V.32/V.32bis, V.34,
  K56flex, V.90, V.92 (quick connect, modem-on-hold, PCM upstream),
  Bell 103/212.
- **Error control and compression**: V.42, MNP 2–5, V.42bis, V.44.
- **Fax**: class 1, to V.17 14400.
- **Voice**: `#VSR` 7200/8000/11025 Hz, 4-bit Rockwell ADPCM or 8-bit
  PCM (`#VBS`), approximately 30 `#` commands (`#CLS`, `#VLS`, `#SPK`,
  `#VTX`, `#VRX`, `#VTS`, `#CID`, …).
- **Other commands**: `#UD` diagnostics, `*B` blacklist, `ATI0`–`7`.
  `ATI2000` gives the answer `SLHAMR56`.
- **Defaults**: PCM upstream off (`PcmUpstreamEnabled=0`). Quick
  connect, modem-on-hold and V.44 on.
- The HAL has 26 rate converters (8000/9600 ↔ 10–48 kHz, MMX or float).
  `ESSM` lets the HAL change its period and not stop the stream
  [inferred].

## Countries

The INF installs only the USA. The `[Homologation]` section of
`Setup.scp` has **72 country records of 193 bytes** [seen]. slgen.dll
writes this section to the registry at installation. Each record is a
`HostBasedModemData\Parameters\DPDriver\Country\HomologationParameters\<NAME>`
value with `VersionID=2710`. The modem reads `CountryName` and that
record, and nothing more. If there is no record, it uses the USA.

**How the country is selected.** The control panel applet
(slcpappl.cpl) changes `CountryName=AUTO` to a country. smcfg.exe runs
the applet after installation [seen]:
- 2000: the applet finds the dialling code `iCountry` in its string
  table.
- XP: the applet finds the GEOID `Geo\Nation` while `AutoXP` = 1. Then
  it sets `AutoXP` to 0 again.
- Fallback: `IfLocaleNotFound` (USA).

**The record.** All fields of the D-Modem `homolog_params`
(core/modem_homolog.h) are in the record, at different offsets. Their
values agree with core/homolog_data.cpp in all countries but one: the
hook-flash time of Singapore is 50 there and 9 in D-Modem. The table
below gives the offsets of those fields, and the extra fields that the
code reads by name. The offsets come from the getters of mtlstrm
[seen]:

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
| 69, 70, 71, 72, 84 | hardware properties 0x4A, 0x4B, 0x64, 0x65, 0x55 (line settings; the driver of this modem does not use them) |
| 83 | busy tone filter |
| 85 | gap between pulse digits (S148) |
| **90** | **hardware property 0x57** |
| 100–111 | ringback detector (all 0) |
| 112, 113 | call progress buffer length (6, 66 → 666) |
| 114 | noise filter |
| **130** | **T.35 country code** (S252, `+GCI`) |
| 132–134, 142 | modem-on-hold timing |

The other fields are S-register defaults and limits: S10, S16, S53,
S72, S92, S144, S148, S149, S200–S204, S241.

Byte 90 is the byte that the hardware of this modem uses [seen]:
- Bit 0 (DC on off hook) is set for France, Algeria and Tunisia.
- Bit 1 (the Australian timer) is set only for Australia (0x62).

Its other bits are different for each country (UK 0x10, Germany 0xE0,
Japan 0x20). Inference: they are an impedance or termination code for
other Smart Link DAAs. With the INF of the DE5671
(`Options.International=0`), the driver ignores bits 0 and 1 too.

Byte 130 has Windows-side codes for some countries: Baltic states
0xB8, CTR21 and Slovenia 0xFF, Slovakia 0x2E, Taiwan 0x26, South Korea
0x61. Groups of records that are the same:
- USA, Argentina, Bahrain, Chile, Peru, Saudi Arabia, South Korea,
  Uruguay;
- Mexico, Philippines, Vietnam;
- Greece, Iceland, Luxembourg;
- CTR21 Europe, Estonia, Turkey.

There is a combined CZECH_REPUBLIC/SLOVAKIA record. The applet names
Colombia, Venezuela, Bolivia, Ecuador and Paraguay, but these countries
have no record.

## Installation and the user-mode tools

**2000/XP** (Mdmntstm.inf, WHQL-signed, 3.00.04.638 of 24 October
2002) [seen]:
- The INF installs the six services. Its co-installer (coinst.dll) runs
  `rundll32 SLGen.dll,SLAddExtra`, which does the steps in Setup.scp:
  it expands setup.cab, writes the country table and makes backups.
- It also makes the auto-start service `SLService` (slserv.exe). This
  service gets the registry path of the modem from the HAL. Then it
  keeps `slrundll.exe SLGen.dll,SLRunServ` in operation. That program
  loads each enabled `HostBasedModemData\Services\<dll>@<entry>`: the
  software speaker and the lights.

The tools send requests to the drivers through `DeviceIoControl(0x222000)`
with a 32-byte header `{cmd, len, 0, status, p1..p4}` [seen]:

| Tool | What it does |
|---|---|
| slextspk.dll | "Software Speaker": reads the speaker samples from the HAL and plays them with waveOut (8 kHz, 8 bit; 16 optional; 800-byte blocks); can also record the microphone for the speakerphone |
| slcpappl.cpl | control panel: country, speaker (soft speaker, +6 dB), modem-on-hold, V.92 (quick connect, V.44, PCM upstream); hidden HAL and DSP pages (sample rates, ESSM, MMX, all S-registers) |
| sllights.dll | tray lights, connection data, modem-on-hold and call-waiting pop-ups with caller ID, an "excessive line current" warning |
| SLMH.exe | "Modem Helper" diagnostics with AT commands: loopback (`ATS32=253`, `ATS253=6`), dial tone, line current (`AT[P118?`/`[P119?`), clock accuracy (`AT#UD` field 128, ppm × 10) |
| smcfg.exe | after installation: runs the country selection when the country is AUTO |
| MiniRec.exe | log recorder |
| winddx.sys | the trace sink of Smart Link (`\\.\WINDDX0`). All the drivers use it |
| amr_cpl.dll | AC'97 modem pages, not connected here |

The help file (SLCPAPPL.chm) is only about the Modem Helper.

## Windows 98 and ME

- **ME** uses the 2000/XP drivers. `slwdmhlp.sys` (a VxD) loads them and
  replaces slwdmsup.sys. The port is ccport.sys and wdmmdmld.vxd from
  Microsoft. Its USB node sets `Debug.DoNotSwap=1` and
  `Debug.SoftDSP=1`.
- **98** loads the same WDM `slnt7554.sys` under NTKERN. Thus, the
  hardware code is the same on all versions. The other drivers are
  VxDs:
  - slv7554.vxd: devnodes and the COM port;
  - mtlhal.vxd: the HAL;
  - mtlstrm.vxd and mtlfax.vxd: the modem and fax;
  - mtlport.vxd: the VCOMM port;
  - mtlmdm.vxd: state, lights, line-current test values, restrictive
    dialling;
  - slld.vxd: the loader.

  The INF for 98 is 3.00.04Y2 (24 September 2002), unsigned.

mtlhal.vxd names the hardware properties of the HAL: `RingEn`, `HookOn`,
`CallerID`, `CIDData`, `SampleRate`, `AttenOut`, `GainIn`, `NearTrip`,
`MakeTime`, `BreakTime`, `Dial`, `LoopRLoop`, `HSOpen`, `VoiceEn`,
`TxOffset`, `RxOffset`, … [seen]. `CIDData` possibly shows that some
hardware decodes caller ID itself. This hardware does not [inferred].

## What this means for dmodemd

`frontend/dmodemd/usb_line.cpp` follows Linux, and also takes more from
Windows (items 1, 2, 6, 7, 8, 9, 10, 11 and 12). The comparison with
Windows:

1. **Caller ID: done.** Caller ID is off by default (`AT#CID=0`). With
   `AT#CID=1`, from the first ring, dmodemd sets GPIO 0x0020 (CID1)
   while the core monitors the line on hook (`MODEM_HOOK_SNOOPING`).
   It clears the GPIO before the hook changes (`UsbLine::set_hook`).
   We tested this on the fake device, but not yet on hardware.
2. **The ST75951 silicon DAA: done.** dmodemd finds it from
   GPIO_STATUS bit 0x4000 and starts it up in the same way as Windows
   (`UsbLine::start_st75951`): power 3, codec register 3 = 0x050,
   register 0 = 0x3C0, power 2, then power 3 and the control word of
   the rate. It sets the power down to 2. Hook, ring and caller ID use
   the same GPIOs. dmodemd uses the DC bit, inverted on the ST75951,
   only with `--usb-international` (item 9).
3. **The idle GPIO state is different**: Windows keeps DP (0x0400)
   high. The Linux values operate correctly, but we do not know the
   function of DP.
4. **Clock drift**: Windows adds or removes full packets from the FIFO
   level. The IN-paced OUT lengths of dmodemd do the same task.
5. **Countries**: the 72 records agree with the D-Modem table. Only the
   hook flash of Singapore is different. The DE5671 applies no DAA
   settings for each country. The 350 ms RFC for Australia and the DC
   bit for France/Algeria/Tunisia apply only in International mode. The
   INF of this modem keeps that mode off.
6. **LEDs: done, behind `--usb-leds`.** The carrier LED (0x0040) is
   on while the call is online. The activity LED (0x2000) comes on
   after data has gone through. The two LEDs go off on hook. Windows
   operates them only when the INF says that the board has them
   (`Options.ExtraLeds`). Thus, dmodemd operates them only with the
   option.
7. **Wake on ring: done, behind `--usb-wake`.** dmodemd arms the remote
   wakeup at the start (Windows arms it when the PC goes to sleep). The
   suspend does not stop dmodemd: it sets up the device again after a
   resume, as Windows does.
8. **The handset detector: done, behind `--usb-handset`.** Windows
   enables the interrupt of GPIO 2 but ignores its events. dmodemd reads
   the GPIO on each change. It reports when the phone on the jack of the
   modem goes off hook or on hook (V.253 `<DLE>H` and `<DLE>h` in voice
   mode). The meaning "set means off hook" is inferred.
9. **International line controls: done, behind `--usb-international`.**
   DP, DC (for each country, inverted on the ST75951), the Australian
   timer and RFC during pulse dialling. dmodemd does these as the
   Windows functions `SetHookOn`, `CalculateGPIOBits`,
   `SpawnAustralianTimerIfNeeded` and `OnDigitPulseDialing` do them.
   The countries are the countries of byte 90 in the table (France,
   Algeria, Tunisia; Australia).
10. **The voice codec: done, behind `--usb-voice-codec`.** Alternate
   setting 3. SSI2 gets power and its period is set as Windows does.
   The handset is V.253 `+VLS=14` and `15`. Windows itself streams it
   only in its tests. The sample format is a guess, and the option
   names it.
11. **RHOM: done, behind `--usb-rhom`.** As `ConfigureRHOM`: FIFO_CTRL2
   bit 15 (bit 7 for the voice codec when it streams) while the soft
   modem has the stream enabled (`VModemLine::Enable` to `Disable`;
   dmodemd: `start()` to `stop()` of the core). Never below revision
   0x200. We still do not know what it does.
12. **The buzzer: done, behind `--usb-buzzer`.** As `SetSpeakerVolume`
   (0x16d46) with `Options.Buzzer`: BUZEN (0x0800) is set for all
   levels but 0, and clear at 0. The levels of dmodemd are the ATM/ATL
   speaker settings of the core.
13. **Rates**: dmodemd has the three control words of Windows, for
   8000, 9600 and 16000 Hz (`cword()` in `frontend/dmodemd/usb_line.cpp`).
   It does not have the 24000 Hz word of Linux (0x3E8). Its `-r` option
   accepts only 8000 or 9600 for this modem (`line_rate_ok()` in
   `frontend/dmodemd/options.h`). Thus, you cannot select 16000 Hz from
   the command line.

## Open questions

- What DP (0x0400), RFC (0x0100), DC (0x1000) and GPIO 0x0002 do
  electrically.
- Which boards have the SSI2 voice codec, and if its octets are
  mu-law, A-law or linear.
- What "RHOM" means, and what bit 15 of FIFO_CTRL2 does.
- How the soft modem makes the caller ID labels (DATE/NMBR/NAME):
  they are not plain strings in a driver.
- Approximately 40 country-record bytes that no named code reads.
- How Windows uses the frame-length adjustment in usual operation, if
  it uses it.

## Method

- We expanded the files of the package with `msexpand` and
  `cabextract`. We did not run any file, not even under Wine.
- slnt7554.sys and mtlmnt5.sys still have their COFF symbol tables.
  Thus, most functions above have names.
- REA opened the binaries, but its decompiler back ends (Ghidra, IDA,
  Hopper) are not available for PE files here. Thus, we read the code
  with the objdump wrapper of REA, local objdump/ndisasm and
  llvm-undname. We read the PE resources with pefile.
- Addresses are virtual addresses at the preferred base of the files
  (0x10000).
- This is static analysis only. There is no harness for these drivers
  yet, but there is one for the BCMSM.sys of Broadcom (half_broadcom).
