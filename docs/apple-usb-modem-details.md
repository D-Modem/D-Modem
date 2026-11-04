# The Apple USB Modem: technical details

This is the technical companion to [The Apple USB Modem](apple-usb-modem.md),
the plain-English overview. Start there; this page keeps the full
detail: the device's interfaces, its requests and notifications, and
where each was found.

The Apple USB Modem (A1101, MA034Z/A) is USB `05ac:1401`. Apple's driver
also takes `05ac:8220`, so dmodemd does too. Inside is Motorola's SM56
USB soft modem: a USB controller with a Silicon Labs Si3056 DAA (data
access arrangement, the part on the line) behind it. dmodemd drives it
in `frontend/dmodemd/usb_apple.cpp`, as the `UsbLine` kind `Apple`
(`usb_line.h`).

## Sources

Everything here was read from Apple's Mac OS X 10.4.11 drivers (the
PowerPC combo update), whose symbols name their functions:

- **AppleSM56KUSBModemFamily** 1.0.0b20: the line. `setHookState`,
  `write_DAA_Register`, `OR_DAA_Register`, `AND_DAA_Register`,
  `XOR_DAA_Register`, `get_DAA_Register`, the interrupt pipe's
  notifications.
- **AppleSM56KUSBAudio** 1.9.9 (with **SM56KUSBAudioFamily**): the
  samples. `AppleSM56KUSBAudioEngine::SetSampleRate`, the streaming
  interfaces it takes.
- **MotorolaSM56KUSB** 1.5.9 (in IOSerialFamily's plug-ins): the modem
  itself, Motorola's. `si3056_HW_UpdateHookCtl`, `si3056_HW_ReadRegister`,
  `si3056_HW_WriteRegister`, its country table.

The code is D-Modem's own; only the protocol comes from them.

## Interfaces

| # | Class | What it is |
|---|---|---|
| 0 | CDC (2), Direct Line Control (3) | the hook; an interrupt IN endpoint of notifications |
| 1 | vendor (0xff), subclass 2 | the DAA's commands |
| 2 | audio (1), control (1) | audio control |
| 3 | audio, streaming (2) | samples IN (endpoint 0x82, 24 octets), in alternate setting 1 |
| 4 | audio, streaming | samples OUT (endpoint 0x03, 24 octets), in alternate setting 1 |
| 5 | application (0xfe), DFU (1) | firmware update; not used |

dmodemd finds each by its class (falling back on these numbers), and
claims all but 5. On Linux, `snd-usb-audio` binds interfaces 2 to 4;
libusb's auto-detach takes them while dmodemd runs.

## Samples

Standard USB audio (class 1). Each streaming interface's alternate
setting 1 has an isochronous endpoint and a format type I descriptor
listing its rates. dmodemd takes an alternate setting of 16-bit mono
samples (2 octets each) that has the `-r` rate, and refuses otherwise,
saying what the device has. The rate is set on each endpoint with the
audio class's `SET_CUR` (`bmRequestType` 0x22, `bRequest` 1, `wValue`
0x0100 `SAMPLING_FREQ_CONTROL`, `wIndex` the endpoint, 3 octets).

The streams run both ways from the start, as the pico-serial Line
stick's: an OUT transfer goes when an IN one is done, with its packets'
lengths. The device's own buffering is not known; dmodemd allows 2 ms
each way.

## The line (interface 0)

- **Hook:** `SET_HOOK_STATE` (`bmRequestType` 0x21, `bRequest` 0x11,
  `wValue` 1 off hook, 0 on hook), as `si3056_HW_UpdateHookCtl` sends it.
  Pulse dialling and the hook flash break and make it. If the device
  stalls it, dmodemd hooks with the Si3056's own bit instead, register
  5's OH (0x01), from then on: Apple's `testInterface` hooks both ways,
  `setHookState` and then `write_DAA_Register(5, 1)` and `(5, 0)`.
- **Notifications** on the interrupt endpoint, CDC's 8-octet header
  (`wValue` little-endian at octets 2 and 3):
  - 0x02, `wValue` bit 0: a DAA command's response is ready.
  - 0x03 `RING_DETECT`: the ring detector's report. A real one sends
    one every millisecond on hook: `wValue` bit 15 is the ring signal's
    polarity and the low octet a level. Apple's `intReadComplete` splits
    it that way, keeping the low octet of each polarity apart and
    telling the modem nub which one came. A line that isn't ringing
    gives one polarity only (`0x8007` to `0x800a` on the first device).
    dmodemd takes it as ringing once both polarities have come within
    100 ms, as a ring signal's do (a 20 Hz ring changes every 25 ms),
    and as ended when one hasn't for 100 ms. It turns a ring into the
    core's ring pulses: one at each change and one every 50 ms while it
    rings (the core counts 4 pulses in 150 ms as a ring). A ring that
    has not ended after 4 s is dropped.
  - 0x29: the phone cord, `wValue` 4 unplugged, 5 plugged in. dmodemd
    logs it.
  - 0x08, 0x09, 0x28: call and line states, and a vendor one. Traced at
    `-d3`, not used.

Apple's driver also has `SEND_PULSE` (0x13), `SET_PULSE_TIME` (0x14) and
the country as `SET_COMM_FEATURE` (`wValue` 2, `wIndex` 1, 2 octets).
dmodemd does not use them: it dials pulses with the hook, and writes the
country's registers itself.

## The DAA (interface 1)

The Si3056's registers are reached with CDC's
`SEND_ENCAPSULATED_COMMAND` (`bmRequestType` 0x21, `bRequest` 0, `wIndex`
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

The device's own controller runs the Si3056's serial link (its PLL and
rate). dmodemd only does the line side's part, as Motorola's driver
does:

1. Register 11 (revision): the line side's ID in its top bits, 1 a
   Si3018, 3 a Si3019 (whose AC termination is register 30).
2. Register 6 to 0: the line side powered up.
3. Register 12 polled for FDT (0x40): the isolation link is up. If it
   never comes, dmodemd says so and goes on.
4. On hook, and the on-hook line monitor on (register 5's ONHM, 0x08),
   so caller ID comes in the samples. Off hook it is turned off.
5. The country's registers (16, 26, 30 on a Si3019, 31, 59) read, then
   written with the country's values (`si3056.cpp`, the datasheet's
   Table 15), at start and on `AT+GCI`.

On close: on hook, the monitor off, alternate settings 0, the
interfaces released.

## A real one

The first real device tried (November 2026, on Android) listed 16-bit
mono at 7200, 8000, 8229, 8400, 9000, 9600 and 10286 Hz on both
streams, and a Si3056 revision 4 with a Si3018 line side. Its first
register read worked; the second's notification overflowed a 16-octet
read, and SET_HOOK_STATE then stalled. dmodemd now reads the
interrupt endpoint a whole packet at a time, as Apple's driver does
(`intReadComplete` takes the pipe's maximum packet size), and fetches a
read's response even when its notification was lost, since a response
left unread may hold up the device's other requests.

With that, the register reads all worked (its interrupt endpoint's
packets are 11 octets), and SET_HOOK_STATE (on hook, at start) still
stalled. So dmodemd hooks with register 5's OH when it does
(`FAKE_APPLE_HOOK_STALL` has the fake stall it too). Whether the
device takes the request off hook, or never, is not known yet.

The next try started, but the modem gave phantom calls: its ring
detector's report comes every millisecond, always with bit 15 set, and
dmodemd had read bit 15 as "ringing". It now needs both polarities (see
the notifications above). Whether a real ring gives both is not known
yet.

## Not yet known

- What a real ring looks like in the ring detector's reports (both
  polarities, as dmodemd expects, or only a higher level).
- The LEDs' bits.

## Testing without hardware

`frontend/dmodemd/test/apple_test.py` (ctest's `dmodemd_apple`, label
`alsa`) runs two dmodemd on fake Apple USB Modems on the simulated
exchange. `frontend/dmodemd/test/fake_apple.cpp` is one (05ac:1401): its six
interfaces and their descriptors, the DAA's commands on registers (ONHM
the exchange's monitor switch), `SET_HOOK_STATE` the exchange's hook,
`SET_CUR` the rate, the ring detector's notifications and the phone
cord's. The test checks:

- a line side that does not answer (said, and dmodemd goes on), the
  interfaces found, the phone cord plugged in;
- a rate the modem has not (refused, its rates said);
- the country's registers written: `-c UK` and `AT+GCI=09`;
- no ring while nobody calls (the ring detector's steady reports);
- a data call (rings, caller ID, the answer at the second ring, CONNECT
  at both ends, text each way), a busy line, a call dialled in pulses
  and a voice call;
- that both exit once the exchange has gone.
