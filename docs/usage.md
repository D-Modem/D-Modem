# Using dmodemd

dmodemd is the modem. It started as the slmodemd of Smart Link (the sl-modem-daemon package of Debian). Now it is a C++ program on the modem core (`core/`), with the open DSP and a SIP user agent (on libre) built in. Its AT commands are the commands of D-Modem, a superset of the commands of the original. The audio of each call comes from SIP, and the modem shows as a pty. With `-a`, the line is the codec of a softmodem through ALSA instead, as [Softmodem hardware](#softmodem-hardware) tells.

Configure the SIP account in the SIP_LOGIN environment variable (`SIP_PORT` sets the local SIP port, 5060 by default):

    # export SIP_LOGIN=username:password@sip.example.com

If you have no account, `SIP_LOGIN=anonymous` runs an anonymous user agent. In this mode:

- The user agent does not register.
- Calls go out from `"Anonymous" <sip:anonymous@anonymous.invalid>` (RFC 3323) to a SIP URI, which the user agent dials as it is (refer to [Dialing](#dialing)).
- With `-L`, the user agent answers the calls that come directly to the address of the machine and `SIP_PORT`.

If a far end asks for credentials (401, 407), it gets none, and the call stops.

`SIP_LOG_LEVEL` sets the quantity of log data from the SIP user agent (a number, 4 by
default; a higher number gives more data). `DMODEM_REC=<base>` records the line audio of each call
as it goes through the modem. The received audio goes in `<base>.rx`, and the
sent audio goes in `<base>.tx`. The [environment variables](reference/environment.md)
page gives all the variables, and [dmodemd's command line](reference/dmodemd.md) gives
all the options.

Next, run dmodemd. For debug logging, use -d<level>.

    # ./build/dmodemd/dmodemd -d9
    D-Modem: version 1.0.0 Oct 15 2026 07:44:51
    symbolic link `/dev/ttyDM0' -> `/dev/pts/3' created. 
    modem `dmodem0' created. TTY is `/dev/pts/3' 
    Use `/dev/ttyDM0' as modem device, Ctrl+C for termination.

In a different terminal, connect to the new serial device at 115200 bps:

    # screen /dev/ttyDM0 115200

Now you can use this terminal (almost) as you use a normal modem, with standard AT commands. [docs/at_commands.pdf](at_commands.pdf) gives all of them.

Our SIP connection has no dial tone. Thus, you must disable dial tone detection:

    atx3 
    OK

By default, the modem negotiates the best common modulation (V.8, then V.34 at up to 33.6kbps, and if necessary V.32bis and lower). dmodemd keeps the audio of the call free of sample slips (a fixed jitter buffer, and the modem at the 8000 Hz of the call), because V.34 must have this. Over a clean G.711 path (the tests in `frontend/asterisk/test`), V.34 connects at 33600. A real PSTN gateway adds its own impairments. Thus, it can be necessary to select a modulation and data rate manually. For example, the subsequent command selects V.32bis with a data rate of 4800 – 9600 bps. For more details, refer to the manual.

    at+ms=132,0,4800,9600 
    OK

## Fax
dmodemd is a class 1 fax modem (`AT+FCLASS=1`). It is also a class 2 and 2.0
fax modem (`AT+FCLASS=2`, `AT+FCLASS=2.0`, T.32). In class 2 and 2.0, the modem runs T.30, and the
software sends, receives and polls the pages. Fax software can use these classes on the pty of dmodemd, for example
efax:

- `efax -d /dev/ttyDM0 -o1 -iX3 -t <number> page.tif` sends a page.
- `-iS0=1 -w -r <prefix>` waits for a call and receives.

efax reads a TIFF page up to its RTC, as efix writes them. A fax call changes
to T.38 when the other end asks for it, or one second after dmodemd
answered the call. The software uses the same commands and gets the same results, but the call uses its
UDPTL instead of its audio. `SIP_T38=no` keeps faxes on the audio.

## Media: V.152 and SRTP
In its offers, dmodemd marks the G.711 formats as voice band data
(V.152, `a=gpmd:0 vbd=yes`). A gateway that obeys this mark disables
its echo canceller, comfort noise and silence suppression for the call.
If an offer has a format with this mark, the answer has that format.
The offer also tells, in the `a=maxmptime` of V.152, that dmodemd sends and
receives 20 ms packets. `SIP_V152=no` removes the marks.

On a trunk that loses packets, the lost packets of the far end can go to
the modem as gaps of silence. Then the modem must retrain or
send again. To prevent this, dmodemd offers RFC 2198 redundancy (`red/8000`, the assured
mode of V.152) on all calls. If the far end accepts it, each packet also carries the
two frames before it. Then the modem can make a lost packet from the far end again from the
subsequent packets. These settings change the redundancy:

- `SIP_RED=1` to `3` sets the number of frames that go again
  (each one adds 64 kbit/s).
- `SIP_RED=no` disables redundancy.

A far end that does not accept the offer gets ordinary G.711. At the end of a
call that lost packets or used redundancy, the log tells how much audio
was lost and how many packets were made again.

The jitter buffer has a fixed delay of 60 ms, as V.152 tells (a modem
cannot accept a change in delay during a call). On a trunk where
packets frequently come later than that, `SIP_JITTER=<ms>` (20 to 1000)
makes the delay longer. If a packet does not come, the modem plays silence for it.
If the modem made artificial audio instead, the modem would receive incorrect data with no indication of
the error. With silence, V.42 sees the error and sends the data again.

`SIP_V150=yes` adds the modem relay of V.150.1. On a data call with a gateway
that has it, the gateway terminates the modulation of its modem. Then the data
goes as bytes over SPRT, and SPRT sends lost data again. dmodemd
has no modem sound ([docs/v150.md](v150.md)). dmodemd offers modem relay together with the
audio. Thus, a far end without modem relay gets an ordinary call.

dmodemd answers in SRTP if the offer of a call is SRTP. This is `RTP/SAVP` (RFC 3711), with its keys in
the `a=crypto` of RFC 4568: AES_CM_128 and AES_256_CM, with 80 or 32 bit
tags. The offers of dmodemd itself are plain RTP. These settings change the behavior:

- `SIP_SRTP=require` offers SRTP and refuses a plain RTP line.
- `SIP_SRTP=no` refuses SRTP.

The keys go in the SDP, and the SIP of dmodemd
is plain UDP. Thus, SRTP keeps the media secret from persons who see only the RTP, but not
from persons who see the SIP. SRTP operates in all builds. The AES of libre
is one of these items:

- The AES of Mbed TLS (the patch of D-Modem, `cmake/libre-mbedtls-aes.patch`).
- The AES of OpenSSL, with `-DDMODEM_CRYPTO=openssl`.
- The AES of Apple, on the systems of Apple.

## Voice
In voice mode (`AT+FCLASS=8`), `AT+VLS=1` answers a ringing call.
`ATD` dials a call, with these results:

- OK when the far end answers with its 200 OK.
- BUSY on 486, 600 or 603.
- NO ANSWER on 408 or 480, or after S7.
- ERROR on all other responses, because V.253 has no NO CARRIER in voice mode.

The modem listens to the early media of the call (183 Session Progress) and to the line. An
answer tone is the answer, and a busy tone gives BUSY. Some lines do
not signal the answer, for example the line of a PBX module. On such a line, the modem also
finds the answer in these conditions:

- The ringing tone stops for `AT+VRA` after its last ring.
- No ring comes in `AT+VRN`.

The modem reports rings, busy and answer tones as `<DLE>r`,
`<DLE>b`, `<DLE>a` (2100 Hz) and `<DLE>f` (2225 Hz). `ATH` hangs up and
goes back to data mode. When the far end stops a call, the modem reports
`<DLE>l`. This stops a `+VRX` stream (`<DLE><ETX>`, OK). It also stops a `+VTX` or `+VTR`
stream when the voice software stops it.

`AT+VTS` plays tones and DTMF digits in the audio. Its hook flash (`!`)
goes to the far end as an RFC 4733 telephone-event (event 16). dmodemd
offers and accepts telephone-events on all calls. `AT+VTS` with `!` gives an ERROR on
a call where the far end has no telephone-events.

The far end can send DTMF digits as telephone-events. These digits go to the modem as their tones, as they
do on an analogue line. The modem reports the digits that it hears to the
voice software as the `<DLE>` codes of V.253 (`<DLE>5`, then `<DLE>~` when
the digit stops). The modem also does this for the modem and fax tones of RFC 4734. Some
gateways relay these tones as events instead of the audio. These tones are:

- The answer tones (ANS,
  ANSam, with their phase reversals each 450 ms).
- CNG, CT and the V.21 tones.
- The segments of V.8bis.

The modem plays each tone at the level that the event gives. The modem logs these events
but does not play them:

- Events for modulated signals (CI, V.21 preamble, the modulations in use). The audio carries
  these signals.
- The hook flash of the far end. No signal of a line represents it.

Software for the voice modems of Rockwell or US Robotics (Unimodem/V,
the `Rockwell` and `US_Robotics` drivers of vgetty) can use their older
commands instead:

- `AT#CLS=8`, `#VLS`.
- `#VBS`/`#VSR` (8-bit or 16-bit linear).
- `#VSM=129` (the ADPCM of US Robotics, G.726 at 32 kbit/s, also
  `AT+VSM=133`).
- `#VTX`, `#VRX` and `#VTS`.
- `VCON` when the modem takes the line.

The 2-, 3- and 4-bit ADPCM of Rockwell and the GSM of US Robotics are not
available. Give vgetty `rec_compression 8`.

## Receiving calls
With `-L` (listen), dmodemd registers with the SIP server. Then calls to the account go to the modem. The modem reports `RING`. It answers on `ATA`, or automatically after `ATS0=n` rings. With `AT+VCID=1` (or `AT#CID=1`), the number and name of the caller follow the first `RING`, as caller ID does on a phone line (`DATE`, `TIME`, `NMBR`, `NAME`). The modem takes them from the P-Asserted-Identity or From header of the INVITE. A withheld caller (the Privacy header, an anonymous URI) shows as `P`. In this mode, dialing operates as before.

    # ./build/dmodemd/dmodemd -L

## Dialing
Finally, dial the number of the target system. The call goes to `sip:<number>@<domain>` of `SIP_LOGIN`. The number has only its digits (and `*`, `#`, `+`). The modem removes `ATDT`, `ATDP`, pauses (`,`, `W`) and the dashes and brackets of a formatted number. The modem uses all other text as the name of a SIP user (`ATDalice`).

The modem dials a SIP URI as it is, with or without an account. Examples are `ATDsip:bbs@example.com`, `ATDsips:bbs@example.com:5061`, or `user@host` (`ATDbbs@203.0.113.7`, `ATD5551000@pbx.example.com`). The text after the last `@` is the name or address of a host. (The `@` of V.250, wait for quiet answer, has digits after it.)

The credentials of an account go only to the host of the account. If a URI at a different host asks for them, it gets none. A number with no account (`SIP_LOGIN=anonymous`) has no domain, thus the call stops immediately with `NO CARRIER`. A `;` at the end (`ATDsip:bbs@example.com;`) makes a voice call, as for a number. (`ATDL` dials the last number again. Thus, to dial a user whose name starts with an L, use its `sip:`, `ATDsip:lisa@example.com`.) This example shows a connection to the NIST atomic clock:

    atd303-494-4774 
    CONNECT 9600 
    National Institute of Standards and Technology 
    Telephone Time Service, Generator 1b 
    Enter the question mark character for HELP 
                            D  L 
     MJD  YR MO DA HH MM SS ST S UT1 msADV         <OTM> 
    59515 21-10-28 21:40:18 11 0 -.1 045.0 UTC(NIST) * 
    59515 21-10-28 21:40:19 11 0 -.1 045.0 UTC(NIST) * 
    59515 21-10-28 21:40:20 11 0 -.1 045.0 UTC(NIST) * 
    59515 21-10-28 21:40:21 11 0 -.1 045.0 UTC(NIST) * 
    59515 21-10-28 21:40:22 11 0 -.1 045.0 UTC(NIST) * 
    59515 21-10-28 21:40:23 11 0 -.1 045.0 UTC(NIST) *
 
## Softmodem hardware
dmodemd can be the modem of an AC'97 or HD-audio softmodem (the Si3054 type) that Linux still operates through ALSA. It replaces the closed, i386-only slmodemd of Smart Link, on any architecture:

    # ./build/dmodemd/dmodemd -a hw:1

`aplay -l` shows the "Modem" device of the card (an HD-audio modem is a device of the audio card, for example `hw:0,6`). The modem dials in DTMF after the dial tone (`ATX4`). It hears rings and caller ID on hook. It connects at V.34 and lower, and it also does fax and voice.

Some HD-audio modem codecs are Si3054-compatible, but they are not in the list of IDs of the kernel driver. For such a codec, `hda-modem-setup write` gives it the driver (the early patching of snd-hda-intel). A Si3054 gets the line settings of the country through the HD-audio hwdep device of the kernel. [docs/hardware.md](hardware.md) gives the hardware and the limits.

These USB modems do not need a kernel driver:

- The USB modem of Smart Link (SmartUSB56, ST7554). `dmodemd --usb=auto` operates it through libusb. It has caller ID and the ST75951 silicon DAA version, as the Windows driver of Smart Link does them ([docs/smartusb56-windows.md](smartusb56-windows.md)).
- The pico-serial Line, a USB-C phone-line stick made for D-Modem (a Si3056 DAA at 9600 Hz, with caller ID and the line settings of each country).
- The USB 2.0 soft modem of Agere (USB `047e:2892`), on the isochronous stream of its ROM ([docs/agere-usb.md](agere-usb.md)).

We tested this on a simulated line (`ctest -L alsa`). We did not test it on a real codec yet.
