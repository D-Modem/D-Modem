<!-- Made by tools/site_reference.py from the source: change the code (or the script), not this page. -->

# dmodemd's command line

Every option `dmodemd` takes, from its option table (`frontend/dmodemd/options.cpp`); `dmodemd --help` prints the same. The SIP account and the rest of the line's settings are [environment variables](environment.md); [Usage](../usage.md) shows them in use.

    dmodemd [options] [name]

`name` is the modem's: its pty is `/dev/tty<name>` (`/dev/ttyDM0` for the default, `dmodem0`... as the usage says), and its logs are named after it.

| option | argument | default | systems | what it does |
|---|---|---|---|---|
| `-h`, `--help` |  |  |  | Print the usage, and exit. |
| `-u`, `--usage` |  |  |  | Print the usage, and exit. |
| `-v`, `--version` |  |  |  | The version, and exit. |
| `-c`, `--country` | text | `USA` |  | The country's rules (--countrylist) |
| `--countrylist` |  |  |  | The countries -c takes, and exit. |
| `-g`, `--group` | text | `dialout` | not Windows | The pty's group. |
| `-p`, `--perm` | number | `0660` | not Windows | The pty's mode, in octal. |
| `-n`, `--nortpriority` |  |  |  | No realtime scheduling (on Windows: normal priority, not high) |
| `-d`, `--debug` | number | `0` |  | Debug messages up to level. |
| `-l`, `--log` | number | `5` |  | Binary log dmodem.log.&lt;name&gt; to level. |
| `-L`, `--listen` |  |  |  | Register, and take calls in. |
| `-a`, `--alsa` | text | `none` | not Windows | The line a modem codec's ALSA PCM (hw:1, hw:0,6), not SIP. |
| `--alsa-ctl` | text | `-a's` | not Windows | Its controls (hw:&lt;card&gt; of -a's) |
| `--usb` | text | `none` |  | The line a Smart Link SmartUSB56, a pico-serial Line or WB, an Agere USB modem, an Apple USB Modem or a Conexant USB HSF modem (auto, or &lt;bus&gt;:&lt;address&gt;), not SIP; list: those there are. |
| `--usb-leds` |  |  |  | A SmartUSB56's carrier and data LEDs (GPIO 6 and 13) lit, on a board that has them (Creative's Modem Blaster) |
| `--usb-wake` |  |  |  | A ring on a SmartUSB56's line wakes the computer (its USB remote wakeup armed) |
| `--usb-handset` |  |  |  | A SmartUSB56's handset detector (GPIO 2): the phone on its jack off or on hook, &lt;DLE&gt;H and &lt;DLE&gt;h in voice mode. |
| `--usb-international` |  |  |  | A SmartUSB56's international line controls (GPIO 8, 10, 12: the country's DC bit, Australia's 350 ms after off hook, pulse dialling's) |
| `--usb-voice-codec` | text |  |  | A SmartUSB56's second codec (SSI2, alternate setting 3) as a handset, +VLS=14 and 15 in voice mode; its octets ulaw, alaw or linear. |
| `--usb-rhom` |  |  |  | A SmartUSB56's RHOM bit (FIFO_CONTROL2 bit 15) while the modem runs, as Windows' Options.RHOM. |
| `--usb-buzzer` |  |  |  | A SmartUSB56's buzzer (GPIO 11) as the speaker (ATM, ATL) |
| `--usb-wb-tx-gain` | text | `0` |  | A pico-serial WB's transmit level, dB (its DAC's volume: -63.5 to 24, in 0.5 dB steps) |
| `--usb-wb-rx-gain` | text | `0` |  | A pico-serial WB's hybrid input gain, dB (its MicPGA: 0 to 47.5, in 0.5 dB steps) |
| `--misdn` | text | `none` | not Windows | The line an ISDN basic rate interface through Linux's mISDN (the device's number or name), not SIP. |
| `--misdn-msn` | text | `any` | not Windows | --misdn: take calls for this MSN alone (and send it as ours) |
| `--misdn-noflip` |  |  | not Windows | --misdn: the B-channel's octets as the card gives them, not bit reversed. |
| `-r`, `--rate` | number | `9600` |  | The codec's rate with -a or --usb (8000 or 9600; 48000, 96000 or 192000 for the wideband mode, AT+MS=VWB: a pico-serial WB's 96000 unless given) |
| `--dsvd-audio` | text | `none` | not Windows | DSVD's voice (AT+DSVD=1) through this command while its voice channel is open: the far end's speech on its stdin, ours from its stdout (raw 16 bit little-endian, 8000 Hz, mono); a SmartUSB56's handset (--usb-voice-codec) is used before it. |
| `--com` | text | `none` | Windows | The DTE a COM port (COM5: one end of a com0com pair) |
| `--pipe` | text | `none` | Windows | The DTE a named pipe dmodemd makes (\\.\pipe\&lt;name&gt;, or &lt;name&gt;) |
| `--pipe-connect` | text | `none` | Windows | The DTE a named pipe another program makes (a VM's COM port) |
| `--tcp` | text | `127.0.0.1:2323` | Windows | The DTE a TCP connection on [address:]port. |
| `--driver` | text | `none` | Windows | The DTE D-Modem's virtual modem driver: its COM port's name (COM7), or auto. |
