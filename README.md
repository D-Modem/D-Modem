# D-Modem
Connect to dialup modems over VoIP using SIP, no modem hardware required.

## Building
You'll need Linux, CMake (3.16 or later), a C++17 compiler (and a C compiler for the Asterisk module), git and pjproject's dependencies (OpenSSL, uuid).

    cmake -B build
    cmake --build build

This builds `build/slmodemd/slmodemd`, `build/slmodemd/libdmodem.a` and, when Asterisk's headers are installed, `build/asterisk/app_dmodem.so`.  `cmake --install build` installs them.

pjproject (PJSIP) is not part of this repository.  CMake uses an installed one (`pkg-config libpjproject`); without one it clones the pinned release (2.17) once and builds it in the build directory.  `-DDMODEM_PJPROJECT=system` requires an installed pjproject, `-DDMODEM_PJPROJECT=fetch` always builds the pinned one, and `-DDMODEM_PJPROJECT_TAG=...` picks another release.

Tests: `ctest --test-dir build -L quick` runs the fast ones (a few seconds); `-L spandsp` (with libspandsp-dev) and `-L interop` are longer.  The interop tests run the modem against Smart Link's original binary-only i386 DSP library (`dsp/test/dsplibs.o`), kept only as a test peer; they need a 32-bit build environment (`g++ -m32`; Debian-based systems can install `g++-multilib`) and are skipped without one.  See [dsp/README.md](dsp/README.md).

### DSP
The modem's signal processing is open source, in `dsp/` (C++, behind the C interface of Smart Link's original binary-only library that slmodemd was written for, `slmodemd/dsplibs.h`); it builds on any architecture.  It implements V.8 and V.34 (up to 33600 bps), V.32bis (14400 bps), V.32, V.22bis, V.22, Bell 212A, V.23, V.21 and Bell 103, class 1 fax (V.17, V.29, V.27ter and V.21 HDLC), call progress, caller ID and voice.  It implements V.90 only as the digital modem (on an 8000 Hz G.711 line), not V.92, and V.91 between two digital modems (up to 62667 bps both ways).  See [dsp/README.md](dsp/README.md) for details and tests.

### Asterisk module
`asterisk/` has `app_dmodem`, a dialplan application that answers a call on an Asterisk channel with the open modem and hands the connection to a BBS (TCP) or a program on a pty such as pppd, per profile in `dmodem.conf`.  It is built on `libdmodem` (`slmodemd/dmodem.h`, and `slmodemd/dmodem.hpp` with C++ classes for it), the modem core and open DSP as a library.  See [asterisk/README.md](asterisk/README.md).

## How it Works
Traditional “controller-based” modems generally used a microcontroller and a DSP to handle all aspects of modem communication on the device itself.  Later, so-called “Winmodems” were introduced that allowed for field-programmable DSPs and moved the controller and other functionality into software running on the host PC.  This was followed by “pure software” modems that moved DSP functionality to the host as well.  The physical hardware of these softmodems was only used to connect to the phone network, and all processing was done in software. 

D-Modem replaces a softmodem’s physical hardware with a SIP stack.  Instead of passing audio to and from the software DSP over an analog phone line, audio travels via the RTP (or SRTP) media streams of a SIP VoIP call.   

## Usage
slmodemd is the modem: a patched version of Debian’s sl-modem-daemon package, with the open DSP and a SIP user agent (pjsua) built in.  Its kernel driver code is gone: each call's audio comes from SIP, and the modem appears as a pty.

Configure the SIP account in the SIP_LOGIN environment variable (`SIP_PORT` sets the local SIP port, 5060 by default): 

    # export SIP_LOGIN=username:password@sip.example.com
Next, run slmodemd.  Use -d<level> for debug logging. 

    # ./build/slmodemd/slmodemd -d9
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

By default the modem negotiates the best common modulation (V.8, then V.34 at up to 33.6kbps, falling back to V.32bis and below).  slmodemd keeps the call's audio free of sample slips (a fixed jitter buffer, the modem at the call's own 8000 Hz), which V.34 needs; over a clean G.711 path (the tests in `asterisk/test`) V.34 connects at 33600.  A real PSTN gateway adds its own impairments, so you may still want to select a modulation and data rate manually.  For example, the following command selects V.32bis with a data rate of 4800 – 9600 bps.  Refer to the manual for further details. 

    at+ms=132,0,4800,9600 
    OK

### Receiving calls
With `-L` (listen), slmodemd registers with the SIP server, so that calls to the account reach the modem: it reports `RING`, and answers on `ATA` or by itself after `ATS0=n` rings.  Dialing works as before in this mode.

    # ./build/slmodemd/slmodemd -L

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
- For a server answering many calls (a BBS, PPP dial-in or fax), the Asterisk module in `asterisk/` is the better fit than `slmodemd -L`: it answers with the open modem at V.91 (another digital modem calling, up to 62667 bps both ways), V.90 (as the digital modem, up to 56000 bps downstream), V.34, V.32bis, V.22bis, V.22 or V.21.
- Tested over SIP against a local Asterisk only, not yet through real PSTN gateways.
- V.34 rate renegotiation during a call (the modem retrains instead, after a line hit or when its error rate climbs), and V.92, are not implemented.
- V.90 needs an analogue modem at the other end: slmodemd is always on the digital side of the network. Two digital modems (D-Modem calling the Asterisk module, or another D-Modem) connect at V.91 instead: up to 62667 bps both ways on a clean G.711 call (about 58700 bps of data through V.42), less where the network robs bits or adds a pad; `AT+MS=91` asks for V.91 (or V.34), the default `AT+MS=92` offers it with V.90.
- When a calling modem tries V.34 and V.32bis before V.22bis (a V.8 caller against `AT+MS=122`), slmodemd may answer at 1200 instead of 2400.
- The serial interface could be replaced with stdio or a socket, and common AT configuration options could be exposed as command line options 


## Acknowledgements
D-Modem was created by Aon's Cyber Labs in 2021, who published it with the article [Introducing D-Modem: A Software SIP Modem](https://www.aon.com/cyber-solutions/aon_cyber_labs/introducing-d-modem-a-software-sip-modem/).  This project builds on their code (slmodemd's socket interface, and their d-modem program, whose SIP user agent is now `slmodemd/sip.cpp`); their copyright notices remain in the source files they wrote.  The modem daemon itself is Smart Link's `slmodemd` (from Debian's sl-modem-daemon package), and SIP is handled by [pjproject](https://github.com/pjsip/pjproject).
