# The Apple USB Modem: technical details

This page gives the technical details for [The Apple USB Modem](apple-usb-modem.md),
the plain-English overview. Read the overview first. This page keeps the full
details: the interfaces of the device, its requests and notifications, and
where we found each item.

The Apple USB Modem (A1101, MA034Z/A) is USB `05ac:1401`. The driver of Apple
also accepts `05ac:8220`. Thus, dmodemd also accepts it. Inside the device is the SM56
USB soft modem of Motorola: a USB controller with a Silicon Labs Si3056 DAA (data
access arrangement, the part on the line) behind it. dmodemd operates it
in `frontend/dmodemd/usb_apple.cpp`, as the `UsbLine` kind `Apple`
(`usb_line.h`).

## Sources

We read all of this information from the Mac OS X 10.4.11 drivers of Apple (the
PowerPC combo update). The symbols of these drivers give the names of their functions:

- **AppleSM56KUSBModemFamily** 1.0.0b20: the line. `setHookState`,
  `write_DAA_Register`, `OR_DAA_Register`, `AND_DAA_Register`,
  `XOR_DAA_Register`, `get_DAA_Register`, the notifications of the interrupt pipe.
- **AppleSM56KUSBAudio** 1.9.9 (with **SM56KUSBAudioFamily**): the
  samples. `AppleSM56KUSBAudioEngine::SetSampleRate`, the streaming
  interfaces that it takes.
- **MotorolaSM56KUSB** 1.5.9 (in the plug-ins of IOSerialFamily): the modem
  itself, from Motorola. `si3056_HW_UpdateHookCtl`, `si3056_HW_ReadRegister`,
  `si3056_HW_WriteRegister`, its country table.

The code is the code of D-Modem. Only the protocol comes from these drivers.

## Interfaces

| # | Class | What it is |
|---|---|---|
| 0 | CDC (2), Direct Line Control (3) | the hook; an interrupt IN endpoint of notifications |
| 1 | vendor (0xff), subclass 2 | the commands of the DAA |
| 2 | audio (1), control (1) | audio control |
| 3 | audio, streaming (2) | samples IN (endpoint 0x82, 24 octets), in alternate setting 1 |
| 4 | audio, streaming | samples OUT (endpoint 0x03, 24 octets), in alternate setting 1 |
| 5 | application (0xfe), DFU (1) | firmware update; not used |

dmodemd finds each interface by its class (if this fails, it uses these numbers). It
claims all interfaces except 5. On Linux, `snd-usb-audio` binds interfaces 2 to 4.
While dmodemd runs, the auto-detach of libusb takes them.

## Samples

The samples use standard USB audio (class 1). Alternate setting 1 of each streaming interface
has an isochronous endpoint and a format type I descriptor that gives
its rates. dmodemd uses an alternate setting of 16-bit mono
samples (2 octets each) that has the `-r` rate. If there is no such setting, dmodemd refuses,
and tells what the device has. dmodemd sets the rate on each endpoint with the
`SET_CUR` of the audio class (`bmRequestType` 0x22, `bRequest` 1, `wValue`
0x0100 `SAMPLING_FREQ_CONTROL`, `wIndex` the endpoint, 3 octets).

The streams run in the two directions from the start, as on the pico-serial Line
stick. An OUT transfer goes when an IN transfer is complete, with the lengths of its packets.
We do not know the buffering of the device. dmodemd permits 2 ms
in each direction.

## The line (interface 0)

- **Hook:** `SET_HOOK_STATE` (`bmRequestType` 0x21, `bRequest` 0x11,
  `wValue` 1 off hook, 0 on hook), as `si3056_HW_UpdateHookCtl` sends it.
  Pulse dialling and the hook flash break and make it. If the device
  stalls it, dmodemd uses the bit of the Si3056 itself for the hook from that time:
  the OH (0x01) of register 5. The `testInterface` of Apple uses the two methods:
  `setHookState` and then `write_DAA_Register(5, 1)` and `(5, 0)`.
- **Notifications** on the interrupt endpoint, with the 8-octet header of CDC
  (`wValue` little-endian at octets 2 and 3):
  - 0x02, `wValue` bit 0: the response of a DAA command is ready.
  - 0x09: the ring. A real device sends it each 40 ms while the line
    rings (2 s on, 4 s off on a US line), with `wValue` 0. It also sends it one time at
    start-up. The `intReadComplete` of Apple sends it to its client as
    message 9. dmodemd accepts it as a ring at the second notification in 150 ms
    (thus, not the single notification at start-up). It accepts the ring as stopped when no notification
    came for 150 ms. It changes a ring into the ring pulses of the core: one at each
    change and one each 50 ms while it rings (the core counts 4 pulses
    in 150 ms as a ring). If a ring does not stop after 4 s, dmodemd
    ignores it.
  - 0x03 `RING_DETECT`: the level of the ring detector. A real device sends it
    each millisecond on hook, with `wValue` bit 15 set and a small level in
    the low octet (`0x8007` to `0x800b`, sometimes up to `0x801e`).
    It sends it also when the line does not ring. The `intReadComplete` of Apple
    keeps its low octet separate by bit 15. dmodemd traces one report in 1000 and does not use it.
  - 0x29: the phone cord, `wValue` 4 disconnected, 5 connected. dmodemd
    logs it.
  - 0x08, 0x09, 0x28: call and line states, and a vendor state. dmodemd traces them at
    `-d3`, and does not use them.

The driver of Apple also has `SEND_PULSE` (0x13), `SET_PULSE_TIME` (0x14) and
the country as `SET_COMM_FEATURE` (`wValue` 2, `wIndex` 1, 2 octets).
dmodemd does not use them. It dials pulses with the hook, and writes the
registers of the country itself.

## The DAA (interface 1)

dmodemd gets access to the registers of the Si3056 with the
`SEND_ENCAPSULATED_COMMAND` of CDC (`bmRequestType` 0x21, `bRequest` 0, `wIndex`
1) of 3 octets: command, register, value.

| Command | What it does |
|---|---|
| 0x00 | write the register |
| 0x01, 0x02, 0x03 | OR, AND, XOR the register with the value |
| 0x80 | read: the response comes with `GET_ENCAPSULATED_RESPONSE` (0xa1, 1, `wIndex` 1, 2 octets, the value first), once notification 0x02 says it is ready |
| 0x10 to 0x13, 0x90 | the LEDs: set, OR, AND, XOR, read (not used: which bit is which is not known) |
| 0x20, 0x30, 0xb0 | wait, flush, free buffer space |
| 0x40 | reset the DAA |
| 0xd0 | firmware version |

The controller of the device operates the serial link of the Si3056 (its PLL and
rate). dmodemd does only the part of the line side, as the driver of Motorola
does:

1. Register 11 (revision): the ID of the line side in its top bits. 1 is a
   Si3018, 3 is a Si3019 (whose AC termination is register 30).
2. Register 6 to 0: the line side gets power.
3. dmodemd polls register 12 for FDT (0x40): the isolation link is up. If it
   does not come, dmodemd tells this and continues.
4. On hook, and the on-hook line monitor enabled (the ONHM of register 5, 0x08).
   Thus, caller ID comes in the samples. Off hook, the monitor is disabled.
5. dmodemd reads the registers of the country (16, 26, 30 on a Si3019, 31, 59), then
   writes them with the values of the country (`si3056.cpp`, Table 15 of the
   datasheet), at the start and on `AT+GCI`.

On close: on hook, the monitor disabled, alternate settings 0, the
interfaces released.

## A real one

We tried the first real device in November 2026, on Android. It gave 16-bit
mono at 7200, 8000, 8229, 8400, 9000, 9600 and 10286 Hz on the two
streams, and a Si3056 revision 4 with a Si3018 line side. Its first
register read operated. The notification of the second register read caused an overflow of a 16-octet
read, and then SET_HOOK_STATE stalled. Now, dmodemd reads the
interrupt endpoint one full packet at a time, as the driver of Apple does
(`intReadComplete` uses the maximum packet size of the pipe). It also gets the response of a
read when the notification was lost, because a response
that is not read can stop the other requests of the device.

With that change, all the register reads operated (the packets of its interrupt endpoint
are 11 octets), but SET_HOOK_STATE (on hook, at the start) still
stalled. Thus, when it stalls, dmodemd uses the OH of register 5 for the hook
(`FAKE_APPLE_HOOK_STALL` makes the fake device stall it too). We do not know yet if the
device accepts the request off hook, or never.

The next try started, but the modem gave phantom calls. dmodemd had
read bit 15 of `RING_DETECT` as "ringing", but this bit is always set.
Then a log of a real incoming call showed the ring itself:
notification 0x09 each 40 ms during each 2 s ring, nothing between rings, and
no change in `RING_DETECT`. We answered that call with `ATA`. It
connected at V.34 33600 bit/s in the two directions. Off hook, the OH of register 5
operates.

## Not yet known

- If caller ID comes through (the log of the first call had no
  number; possibly it must have the timing of the line monitor after the first ring).
- The bits of the LEDs.

## Testing without hardware

`frontend/dmodemd/test/apple_test.py` (the `dmodemd_apple` test of ctest, label
`alsa`) runs two dmodemd on fake Apple USB Modems on the simulated
exchange. `frontend/dmodemd/test/fake_apple.cpp` is one fake device (05ac:1401). It has
these items: its six interfaces and their descriptors, the commands of the DAA on registers (ONHM
is the monitor switch of the exchange), `SET_HOOK_STATE` as the hook of the exchange,
`SET_CUR` as the rate, and the notifications of the ring, the ring detector and the phone
cord. The test checks these items:

- a line side that does not answer (dmodemd tells this, and continues), the
  interfaces found, the phone cord connected;
- a rate that the modem does not have (refused, with the rates that it has);
- the registers of the country written: `-c UK` and `AT+GCI=09`;
- no ring while nobody calls (the steady reports of the ring detector, the
  single ring notification at the start);
- a data call (rings, caller ID, the answer at the second ring, CONNECT
  at the two ends, text in each direction), a busy line, a call dialled in pulses
  and a voice call;
- that the two dmodemd stop after the exchange has stopped.
