# Using dmodemd

dmodemd is the modem.  It began as Smart Link's slmodemd (Debian’s sl-modem-daemon package) and is now a C++ program on the modem core (`core/`), with the open DSP and a SIP user agent (on libre) built in; its AT commands are D-Modem's own, a superset of the original's.  Each call's audio comes from SIP, and the modem appears as a pty.  With `-a` the line is instead a softmodem's codec through ALSA, as described in [Softmodem hardware](#softmodem-hardware).

Configure the SIP account in the SIP_LOGIN environment variable (`SIP_PORT` sets the local SIP port, 5060 by default): 

    # export SIP_LOGIN=username:password@sip.example.com
Next, run dmodemd.  Use -d<level> for debug logging. 

    # ./build/dmodemd/dmodemd -d9
    D-Modem: version 1.0.0 Oct 15 2026 07:44:51
    symbolic link `/dev/ttyDM0' -> `/dev/pts/3' created. 
    modem `dmodem0' created. TTY is `/dev/pts/3' 
    Use `/dev/ttyDM0' as modem device, Ctrl+C for termination.

In another terminal, connect to the newly created serial device at 115200 bps: 

    # screen /dev/ttyDM0 115200

You can now interact with this terminal (almost) as you would with a normal modem using standard AT commands; [docs/at_commands.pdf](at_commands.pdf) lists them all. 

Because there isn’t any dial tone on our SIP connection, you’ll need to disable dial tone detection: 

    atx3 
    OK

By default the modem negotiates the best common modulation (V.8, then V.34 at up to 33.6kbps, falling back to V.32bis and below).  dmodemd keeps the call's audio free of sample slips (a fixed jitter buffer, the modem at the call's own 8000 Hz), which V.34 needs; over a clean G.711 path (the tests in `frontend/asterisk/test`) V.34 connects at 33600.  A real PSTN gateway adds its own impairments, so you may still want to select a modulation and data rate manually.  For example, the following command selects V.32bis with a data rate of 4800 – 9600 bps.  Refer to the manual for further details. 

    at+ms=132,0,4800,9600 
    OK

## Fax
dmodemd is a class 1 fax modem (`AT+FCLASS=1`), and a class 2 and 2.0
one (`AT+FCLASS=2`, `AT+FCLASS=2.0`, T.32: the modem runs T.30, the
software sends, takes and polls the pages), for fax software on its
pty such as efax (`efax -d /dev/ttyDM0 -o1 -iX3 -t <number> page.tif`;
`-iS0=1 -w -r <prefix>` waits for a call and receives; efax reads a TIFF
page up to its RTC, as efix writes them).  A fax call goes
over to T.38 when the other end asks for it, or a second after dmodemd
answered one: the same commands and results for the software, the call's
UDPTL instead of its audio.  `SIP_T38=no` keeps faxes on the audio.

## Media: V.152 and SRTP
dmodemd marks the G.711 formats in its offers as voice band data
(V.152, `a=gpmd:0 vbd=yes`), so a gateway that honours it turns off
its echo canceller, comfort noise and silence suppression for the call;
an offer with a format so marked gets that format in the answer.
`SIP_V152=no` leaves the marks out.

`SIP_V150=yes` adds V.150.1's modem relay: on a data call with a gateway
that has it, the gateway terminates its modem's modulation and the data
goes as bytes over SPRT, retransmitted when lost, with no modem sound on
dmodemd's side ([docs/v150.md](v150.md)).  It is offered beside the
audio, so a far end without it gets an ordinary call.

A call whose offer is SRTP (`RTP/SAVP`, RFC 3711, its keys in
RFC 4568's `a=crypto`: AES_CM_128 and AES_256_CM, with 80 or 32 bit
tags) is answered in SRTP; dmodemd's own offers are plain RTP.
`SIP_SRTP=require` offers SRTP and declines a plain RTP line;
`SIP_SRTP=no` declines SRTP. The keys go in the SDP, and dmodemd's SIP
is plain UDP: SRTP keeps the media from those who see only the RTP, not
from those who see the SIP. SRTP needs libre's
AES (OpenSSL, or Apple's on macOS): builds without it (Windows,
Android) have it off.

## Voice
In voice mode (`AT+FCLASS=8`), `AT+VLS=1` answers a ringing call,
`ATD` dials one (OK when the far end answers with its 200 OK; BUSY on
486, 600 or 603, NO ANSWER on 408 or 480 or after S7, ERROR on anything
else, as V.253 has no NO CARRIER in voice mode).  The modem listens to
the call's early media (183 Session Progress) and to the line: an
answer tone is the answer and a busy tone BUSY, and on a line that does
not signal the answer, such as a PBX module's, the answer is also the
ringing tone gone for `AT+VRA` after its last ring, or no ring in
`AT+VRN`; rings, busy and answer tones are reported as `<DLE>r`,
`<DLE>b`, `<DLE>a` (2100 Hz) and `<DLE>f` (2225 Hz).  `ATH` hangs up and
goes back to data mode, and a call the far end ends is reported as
`<DLE>l`, ending a `+VRX` stream (`<DLE><ETX>`, OK) or a `+VTX` or `+VTR`
one when the voice software ends it.  `AT+VTS`
plays tones and DTMF digits in the audio.  Its hook flash (`!`)
goes to the far end as an RFC 4733 telephone-event (event 16), which
dmodemd offers and takes on every call; `AT+VTS` with `!` is an ERROR on
a call whose far end has no telephone-events.  DTMF digits the far end
sends as telephone-events reach the modem as their tones, as they would
on an analogue line, and the modem reports the digits it hears to the
voice software as V.253's `<DLE>` codes (`<DLE>5`, then `<DLE>~` when
the digit ends).  So do RFC 4734's modem and fax tones, which some
gateways relay as events in place of the audio: the answer tones (ANS,
ANSam, with their phase reversals every 450 ms), CNG, CT, the V.21 tones
and V.8bis's segments, each at the level the event gives.  Events for
modulated signals (CI, V.21 preamble, the modulations in use) and the far
end's hook flash are logged and not played: the audio carries the
former, and no signal of a line stands for the latter.

## Receiving calls
With `-L` (listen), dmodemd registers with the SIP server, so that calls to the account reach the modem: it reports `RING`, and answers on `ATA` or by itself after `ATS0=n` rings.  With `AT+VCID=1` (or `AT#CID=1`) the caller's number and name follow the first `RING` as caller ID does on a phone line (`DATE`, `TIME`, `NMBR`, `NAME`), taken from the INVITE's P-Asserted-Identity or From header; a withheld caller (the Privacy header, an anonymous URI) is `P`.  Dialing works as before in this mode.

    # ./build/dmodemd/dmodemd -L

## Dialing
Finally, dial the number of the target system.  The call goes to `sip:<number>@<domain>` of `SIP_LOGIN`, the number's digits (and `*`, `#`, `+`): `ATDT`, `ATDP`, pauses (`,`, `W`) and the dashes and brackets of a formatted number are left out, and anything else is taken as a SIP user's name (`ATDalice`).  Below shows a connection to the NIST atomic clock: 

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
dmodemd can be the modem of an AC'97 or HD-audio softmodem (the Si3054 kind) that Linux still drives through ALSA.  It replaces Smart Link's closed, i386-only slmodemd, on any architecture:

    # ./build/dmodemd/dmodemd -a hw:1

`aplay -l` lists the card's "Modem" device (an HD-audio modem is a device of the audio card, e.g. `hw:0,6`).  The modem dials in DTMF after the dial tone (`ATX4`), hears rings and caller ID on hook, and connects at V.34 and below, with fax and voice too.  An HD-audio modem codec that is Si3054-compatible but outside the kernel driver's list of IDs gets the driver with `hda-modem-setup write` (snd-hda-intel's early patching); a Si3054 gets the country's line settings through the kernel's HD-audio hwdep device.  [docs/hardware.md](hardware.md) lists the hardware and the limits.  Smart Link's USB modem (SmartUSB56, ST7554) needs no kernel driver: `dmodemd --usb=auto` drives it through libusb (with caller ID, and the ST75951 silicon DAA version, as Smart Link's Windows driver does them: [docs/smartusb56-windows.md](smartusb56-windows.md)).  So does the pico-serial Line, a USB-C phone-line stick made for D-Modem (a Si3056 DAA at 9600 Hz, with caller ID and each country's line settings).  So does Agere's USB 2.0 soft modem (USB `047e:2892`), on its ROM's own isochronous stream ([docs/agere-usb.md](agere-usb.md)).  It has been tested on a simulated line (`ctest -L alsa`), not yet on a real codec.
