# The x2 upgrade host: technical details

This document gives the protocol between a USRobotics Courier and the
x2 upgrade host, the source of each fact, and the tests. For the
configuration, refer to [the x2 upgrade host](x2-upgrade.md).

## 1. Sources

- "x2: Understanding the Issues / Troubleshooting Problems", version
  1.0, 19 June 1997 (`x2shoot4.pdf` in the `docs/x2/` folder of
  [courier-emu](https://github.com/D-Modem/courier-emu)).
  Appendix B gives the procedure for a Courier V.Everything: load the
  x2 code, "call U.S. Robotics host to enable x2", send `AT&F1S0=0&W`,
  and set the power switch of the modem to OFF, then to ON.
- The Courier firmware. U.S. Robotics did not publish the host side.
  The modem side is in the supervisor code of the Courier. We examined
  these images:

| Image | Supervisor | Use |
| --- | --- | --- |
| `courier-714-221-flash-20261007` | 7.1.4, 02/25/97 (DSP 2.2.1) | Read; handler run offline |
| `courier-board-21210-capture-403` | 7.4.16, 04/30/98 (ID_SDL 4.03d) | Read; full calls in courier-emu |
| Five other captures and `IDSDL302.ROM` | 7.3.x and others | The same code is present |

The host side in this document is new work. It sends the messages that
the Courier code accepts, and it reads the answers that the Courier code
sends.

## 2. Frame format

All messages are LAPM UI frames (V.42, 8.2), in the data phase.

| Octet | Content |
| --- | --- |
| 1 | Address. The Courier accepts the command address and the response address. |
| 2 | Control: `03` (UI, P = 0). The Courier ignores a frame with P = 1. |
| 3 | Type: `0a`. |
| 4 | Message number (table below). |
| 5 and on | The data of the message. |

D-Modem sends each message as a command, with its own command address.
The answerer's command address is `01`. Thus the x2 set from an
answering D-Modem is `01 03 0a 02 25`.

The Courier sends its answers as UI commands with its own command
address. A calling Courier uses `03`.

## 3. Messages

| Number | Direction | Data | Function |
| --- | --- | --- | --- |
| 0 | Courier to host | Six octets | The six records, from record 6 to record 1. |
| 1 | Host to Courier | None | Ask for the records. The Courier sends message 0. |
| 2 | Host to Courier | One selector | Set one bit in one record. |
| 3 | Host to Courier | None | Ask for the serial number. The Courier sends message 4. |
| 4 | Courier to host | A count, then that number of octets | The serial number. |
| 5 | Host to Courier | One selector | Clear one bit in one record. |

The Courier does not answer messages 2 and 5.

### 3.1 The selector

The high nibble of the selector is the record number (1 to 6). The low
nibble is the bit number (1 to 8). Bit n has the value 1 << (n - 1).
Thus `25` is record 2, bit 5 (the value 16).

### 3.2 The records

The six records are the six fields of `ATY14`. `ATY14` shows them from
record 6 to record 1.

| Record | Meaning |
| --- | --- |
| 1 | Country (`ATC10`). 0 is USA and Canada. |
| 2 | Features (`ATC8`). |
| 3 | Board type 2. |
| 4 | Board type 1. |
| 5, 6 | Not used. |

The feature bits are:

| Bit | Value | Feature |
| --- | --- | --- |
| 1 | 1 | HST |
| 2 | 2 | Fax |
| 3 | 4 | Terbo |
| 4 | 8 | V.FC and V.34 |
| 5 | 16 | x2. In builds that have V.90, this bit also enables V.90. |

A Courier with all features except x2 shows 015. With x2, it shows 031.

Each record is stored as three octets in the settings EEPROM. Each
octet has a different encoding. The decoder is at `d929` in 7.1.4. For
example, 15 is `28 0f 12` and 31 is `68 17 02`.

### 3.3 The serial number

Message 4 has one of two forms:

- Count 12 (`0c`), then 12 octets: 9 from RAM `073a` and 3 from RAM
  `0887`. This form is used when RAM `0887` is not `ff`.
- Count 16 (`10`), then 16 ASCII digits made from the octets at RAM
  `073a`.

D-Modem keeps the printable octets and changes other octets to `?`.

## 4. The firmware code (7.1.4)

Addresses are offsets in the 512 KiB image. The CPU address is the
offset plus `80000`.

| Offset | Function |
| --- | --- |
| `cd32` | The control frame queue. `cc00` reads the address and control. |
| `cd6b` | U frames. The table at `cb86` selects `c515` for UI. |
| `c515` | UI. P = 1 stops the frame (`c290`). Type `08` has a different function, which we did not examine. |
| `c53e` | Type `0a`. The message number selects an entry of the table at `c559`. |
| `c565` | Message 0: send the records, with commas, to the local output routines (probably to the DTE). |
| `c593` | Message 1: set the flag at RAM `0b7e`. |
| `c5a9` | Message 2: set the bit. `c5b9` decodes the selector. |
| `c5d8` | Message 3: set the flag at RAM `0b7f`. |
| `c5df` | Message 4: show "Remote Serial Number" and the octets. |
| `c59a` | Message 5: clear the bit. |
| `c5b2` | Encode the record (`d99e`, `d9c3`) and write the EEPROM (`da13`). |
| `c009` | The flag table at RAM `0b7b` (19 flags), with handlers at `bfdb`. |
| `ba72` | Flag `0b7e`: send message 0, the records 6 to 1. |
| `bad2` | Flag `0b7f`: send message 4, the serial number. |

The code at `ba8e`, `ba9d`, `bab0` and `bac3` sends messages 1, 5, 2
and 3. No code calls these routines. Thus the Courier contains the
sender of the host side, but no command starts it.

7.1.4 has no permission gate. Supervisor 7.4.16 has the same code at
`ce69` and on. Before the write it tests RAM `04ac` bit 0 (S30 bit 0,
"Enable feature upgrades"). If the bit is 0, the Courier does not keep
the change.

A change in the RAM copy and the EEPROM has no effect on the call. At
power on, the Courier reads record 2 (7.1.4: `0908`). Bit 5 then sets
the x2 capability (`0933`).

Supervisor 7.4.16 and 7.6.7 set the x2 capability at `0519` with no
condition, after they read the records. Thus, in these builds, bit 5 has
no effect. Supervisor 7.1.4 and 7.3.14 do not have this step. The audit
is in courier-emu's `docs/feature-bits-audit.md`.

## 5. The host sequence

`dlink::UsrFeature` (`link/usr_feature.h`) does these steps on the
modem that has `AT+X2UP=1` or `AT+X2UP=2`:

1. Wait 1 s after LAPM connects.
2. Send message 3. Wait 3 s for message 4. Send again, up to 3 times.
   If no answer comes, continue without the serial number.
3. Send message 1. Wait 3 s for message 0. Send again, up to 3 times.
   If no answer comes, the result is `NO REPLY`.
4. With mode 1, the result is `QUERIED`.
5. Find the bits of `<enable>` that record 2 does not have, and the
   bits of `<disable>` that it has. If there are none, the result is
   `ALREADY`.
6. For each bit to enable, send message 2. For each bit to disable,
   send message 5. The selectors are `23` (Terbo), `24` (V.FC) and `25`
   (x2). Wait 1.5 s.
7. Send message 1 again, as in step 3. If the bits of `<enable>` and
   `<disable>` in record 2 are now as asked, the result is `ENABLED`
   (only enables), `DISABLED` (only disables) or `CHANGED` (both). If
   not, or if no answer comes, the result is `REFUSED`.

The timers count the bits that D-Modem sends. The result goes to the DTE
as one `+X2UP:` line (`core/modem.cpp`, `LinkOwner::usr_feature()`).

`AT+X2UP` starts the exchange one time, when LAPM connects. A program can
start it again later in the same call, with new values:

- `Link::usr_start()` starts the steps above at step 1. It refuses when
  LAPM is not up, when the link is V.76 (DSVD), or when an exchange is
  in progress.
- `Modem::usr_feature_now()` makes sure that the values are correct, as
  `AT+X2UP` does. Then it calls `Link::usr_start()`.
- `dmodem_x2up()` in libdmodem calls `Modem::usr_feature_now()`.
- The PBX bridge (`frontend/pbx/dmodem_bridge.c`) gives an exec program
  a control channel: one end of a socket pair, on the file descriptor 3
  of the program (`DMODEM_CONTROL=3`). The bridge reads command lines on
  it. `X2UP=<mode>,<enable>,<disable>` calls `dmodem_x2up()` when the
  call is online. The bridge sends `OK` or `ERROR` back.

The caller screens (`tools/x2upgrade-host.py` and
`tools/x2upgrade-host/`) use this. The profile reads the Courier at the
start (`AT+X2UP=1`). The caller selects the features. Then the screen
sends `X2UP=2,<enable>,<disable>` on the control channel.

## 6. Tests

### 6.1 Unit test

`link_test`'s `Link.UsrFeature` connects two LAPM links. After LAPM
connects, a model of the Courier code answers the host. The test
examines five calls: `ENABLED` (015 to 031, and the frame
`01 03 0a 02 25`), `ALREADY`, `REFUSED` (the model does not keep the
change), `NO REPLY` (six requests, then the end), and `QUERIED`.

`Link.UsrFeatureSwitches` examines the switches: `DISABLED` (031 to 019,
with the frames `01 03 0a 05 23` and `01 03 0a 05 24`), `CHANGED` (019
to 007), `ALREADY` for a feature that is already disabled, no frame for
HST and fax, and `REFUSED`.

`Link.UsrFeatureLater` reads the records when LAPM connects (mode 1).
Then it calls `Link::usr_start()` with mode 2 (enable x2, disable
V.FC): `CHANGED`, 015 to 023. A second exchange during the first one is
refused, and a link with no LAPM refuses the start.

`dmodem_bridge_test`'s `Bridge.ExecControlChannel` makes a V.22 call
with V.42 to an exec profile. The program sends three lines on its
descriptor 3: `X2UP=1` gets `OK`, and `X2UP=2,4,4` (a feature in the two
values) and a line that is not a command get `ERROR`.

`tools/test_x2upgrade_host.py` and the Go tests of
`tools/x2upgrade-host/` examine the caller screens: the menu, the
control line for each choice, `Q`, no key, an `ERROR` from the control
channel, `REFUSED`, and a host with no control channel.

### 6.2 Calls to emulated Couriers

courier-emu runs the original 7.4.16 supervisor and DSP firmware. Its
`tools/probe_v90modem_closed_loop.py` connects the Courier to
D-Modem's `courier_peer` on a digital line. The Courier dials. D-Modem
answers with `AT+MS=X2+X2UP=2`.

| Courier EEPROM | Courier settings | Result |
| --- | --- | --- |
| The board's own (features 031) | `S30=0` | `ALREADY`, 031 |
| Features changed to 015 | `S30=1` | `ENABLED`, 015 to 031 |
| Features changed to 015 | `S30=0` | `REFUSED`, 015 |

All calls connected with x2 and LAPM. The serial number in all calls
was `0009540034268322`.

The image in these calls (`courier-board-21210-capture-403`) is ID_SDL
4.03d, a third-party build of 7.4.16. Like the stock 7.4.16, it enables
x2 at each start, whatever the value of the record. Thus these calls show
that the host changes the record. They do not show the effect of the
change on the modem.

Supervisor 7.3.14 (`courier-board-21210-capture-01`) shows the effect.
With record 2 set to 31, the Courier connected with
`CONNECT 49333/ARQ/x2/LAPM/V42BIS`, and `ATI7` showed `x2,V90`. With
record 2 set to 15, the Courier sent no x2 marker, `ATI7` did not show
x2, and the Courier gave `ERROR` for the x2 speed `&N39`.

A call to 7.3.14 with record 2 set to 19 (HST, fax, x2) and
`AT+MS=X2+X2UP=2,12,16` gave `+X2UP: CHANGED,"0009540034268322",019,015`.
D-Modem sent the selectors `23` and `24` in message 2, and `25` in
message 5. After a new start from the changed EEPROM, `ATI7` showed
`HST,V32bis,Terbo,VFC,V34+`, and `ATY14` showed
`000,000,030,007,015,000`.

After the `ENABLED` call, the EEPROM image held `68 17 02` (31) in
place of `28 0f 12` (15). A new start of the emulated Courier from that
image showed `000,000,030,007,031,000` for `ATY14`. Before the call,
it showed `000,000,030,007,015,000`.

In the `REFUSED` call, the first two requests after the set got no answer,
and the third got one. The retries are thus necessary. The cause of the
lost answers is not known. Errors in the upstream of the emulated
Courier are a possible cause.

### 6.3 Supervisor 7.1.4

A full call with 7.1.4 in courier-emu did not connect. The 7.1.4
Courier did not send the x2 marker, and the V.34 start-up between it
and D-Modem failed. Thus, for 7.1.4, we ran the handler offline. The
real 80186 code ran from `cd32` with the frame in RAM and a record of
15 at RAM `0748`. The EEPROM writer at `da13` was stopped before the
hardware access.

| Frame | Result |
| --- | --- |
| `01 03 0a 02 25` (D-Modem's) | Record 31. The writer is reached. |
| `03 03 0a 02 25` | Record 31. The writer is reached. |
| `01 13 0a 02 25` (P = 1) | No change. |

The script is courier-emu's
`artifacts/courier-714-221-flash-20261007/remote-analysis/check_handler.py`.

### 6.4 How to repeat the calls

Build D-Modem's `courier_peer`. Then, in a courier-emu checkout:

1. Make an EEPROM image with the features 015:

       PYTHONPATH=. .venv/bin/python tools/feature_nvram.py /tmp/015.sav --features 15

2. Start the call:

       COURIER_NVRAM=/tmp/015.sav \
       COURIER_AT='ATS30=1X1S54=0S58=48&A3&B1Q0&U26&N39DT5551234' \
       COURIER_PEER_AT='AT+MS=X2+X2UP=2' \
       .venv/bin/python tools/probe_v90modem_closed_loop.py analog \
           --output /tmp/x2up --engine <D-Modem build>/dsp/test/courier_peer \
           --fast --instructions 200000000

3. Find the `+X2UP:` line in `/tmp/x2up/engine.log`.

The run writes the changed EEPROM back to `/tmp/015.sav`. The emulated
calls are sensitive to the time of the dial. A change in the length of
`COURIER_AT` can stop the training. If the call does not connect, keep
the length of the command line that is given above.

### 6.5 Not tested

- A physical Courier. The earlier attempt in courier-emu's
  `artifacts/courier-x2-service/` sent `01 02 0a 02 25` as raw octets.
  On the line, a control octet of `02` is an I frame, not a UI frame.
  D-Modem sends `03`.
- A host call with V.34 in place of x2.
- The Courier's own `AT&F1S0=0&W` step after the upgrade.
