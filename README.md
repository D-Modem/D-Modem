# D-Modem
Connect to dialup modems over VoIP using SIP, no modem hardware required.

https://www.aon.com/cyber-solutions/aon_cyber_labs/introducing-d-modem-a-software-sip-modem/

## Building
You'll need Linux and a working 32-bit development environment (gcc -m32 needs to work, Debian-based systems can install libc6-dev-i386), along with PJSIP's dependencies (OpenSSL).  Then run 'make' from the top-level directory.

### Open source DSP (optional)
By default slmodemd links Smart Link's binary-only i386 DSP library (`slmodemd/dsplibs.o`).  `slmodemd/dsp/` contains a C reimplementation of its interface that builds on any architecture and needs no 32-bit environment:

    make DSPLIBS=open

It implements V.8 and V.34 (up to 33600 bps), V.32bis (14400 bps), V.32, V.22bis, V.22, Bell 212A, V.23, V.21 and Bell 103, class 1 fax (V.17, V.29, V.27ter and V.21 HDLC), call progress, caller ID and voice.  It implements V.90 only as the digital modem (on an 8000 Hz G.711 line), not V.92.  See [slmodemd/dsp/README.md](slmodemd/dsp/README.md) for details and tests.

### Asterisk module
`asterisk/` has `app_dmodem`, a dialplan application that answers a call on an Asterisk channel with the open modem and hands the connection to a BBS (TCP) or a program on a pty such as pppd, per profile in `dmodem.conf`.  It is built on `libdmodem` (`slmodemd/dmodem.h`), the modem core and open DSP as a library.  See [asterisk/README.md](asterisk/README.md).

## How it Works
Traditional “controller-based” modems generally used a microcontroller and a DSP to handle all aspects of modem communication on the device itself.  Later, so-called “Winmodems” were introduced that allowed for field-programmable DSPs and moved the controller and other functionality into software running on the host PC.  This was followed by “pure software” modems that moved DSP functionality to the host as well.  The physical hardware of these softmodems was only used to connect to the phone network, and all processing was done in software. 

D-Modem replaces a softmodem’s physical hardware with a SIP stack.  Instead of passing audio to and from the software DSP over an analog phone line, audio travels via the RTP (or SRTP) media streams of a SIP VoIP call.   

## Usage
The repository contains two applications: 

slmodemd – A stripped down and patched version of Debian’s sl-modem-daemon package.  All kernel driver code has been replaced with socket-based communication, allowing external applications to manage audio streams. 

d-modem – External application that interfaces with slmodemd to manage SIP calls and their associated audio streams. 

After they have been built, you will need to configure SIP account information in the SIP_LOGIN environment variable: 

    # export SIP_LOGIN=username:password@sip.example.com
Next, run slmodemd, passing the path to d-modem in the -e option.  Use -d<level> for debug logging. 

    # ./slmodemd/slmodemd -d9 -e ./d-modem
    SmartLink Soft Modem: version 2.9.11 Oct 28 2021 16:51:30 
    symbolic link `/dev/ttySL0' -> `/dev/pts/3' created. 
    modem `slamr0' created. TTY is `/dev/pts/3' 
    Use `/dev/ttySL0' as modem device, Ctrl+C for termination.

In another terminal, connect to the newly created serial device at 115200 bps: 

    # screen /dev/ttySL0 115200

You can now interact with this terminal (almost) as you would with a normal modem using standard AT commands.  A similar modem’s manual provides a more complete list. 

Because there isn’t any dial tone on our SIP connection, you’ll need to disable dial tone detection: 

    atx3 
    OK

By default the modem negotiates the best common modulation (V.8, then V.34 at up to 33.6kbps, falling back to V.32bis and below).  d-modem keeps the call's audio free of sample slips (a fixed jitter buffer, its own 8000/9600 Hz conversion), which V.34 needs; over a clean G.711 path (the tests in `asterisk/test`) V.34 connects at 33600.  A real PSTN gateway adds its own impairments, so you may still want to select a modulation and data rate manually.  For example, the following command selects V.32bis with a data rate of 4800 – 9600 bps.  Refer to the manual for further details. 

    at+ms=132,0,4800,9600 
    OK

### Receiving calls
With `-L` (listen), slmodemd keeps d-modem running, registered with the SIP server, so that calls to the account reach the modem: it reports `RING`, and answers on `ATA` or by itself after `ATS0=n` rings.  Dialing works as before in this mode.

    # ./slmodemd/slmodemd -L -e ./d-modem

### Dialing
Finally, dial the number of the target system.  Below shows a connection to the NIST atomic clock: 

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
 
## Known Issues / Future Work
- For a server answering many calls (a BBS, PPP dial-in or fax), the Asterisk module in `asterisk/` is the better fit than `slmodemd -L`: it answers with the open modem at V.90 (as the digital modem, up to 56000 bps downstream), V.34, V.32bis, V.22bis, V.22 or V.21.
- Tested over SIP against a local Asterisk only, not yet through real PSTN gateways.
- V.34 retrains and rate renegotiation during a call, and V.92, are not implemented in the open DSP.
- The serial interface could be replaced with stdio or a socket, and common AT configuration options could be exposed as command line options 


Copyright 2021 Aon plc
