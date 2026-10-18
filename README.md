# D-Modem
Connect to dialup modems over VoIP using SIP, no modem hardware required.
Or, on an old laptop's softmodem, use it as the modem's open driver on a
telephone line ([docs/hardware.md](docs/hardware.md)).

## Building
You'll need Linux (glibc or musl: Alpine builds and passes the same tests), CMake (3.16 or later), a C++17 compiler (and a C compiler for the Asterisk and FreeSWITCH modules), git and OpenSSL (libssl-dev: libre's digests for SIP's authentication), or Mbed TLS in its place (below).

    cmake -B build
    cmake --build build

With libasound2-dev (ALSA), dmodemd can also drive a softmodem's codec (`-a`, [docs/hardware.md](docs/hardware.md)).

This builds `build/dmodemd/dmodemd`, `build/dmodemd/libdmodem.a`, `build/dmodemd/libdmodem_sip.a` and, when Asterisk's headers are installed, `build/asterisk/app_dmodem.so`, and when FreeSWITCH is installed (its `freeswitch.pc`), `build/freeswitch/mod_dmodem.so`.  `cmake --install build` installs them.

dmodemd's SIP is [libre](https://github.com/baresip/re) (baresip's re, BSD-3-Clause), which is not part of this repository.  CMake uses an installed one (`pkg-config libre`, 4.12 or later); without one it clones the pinned release (v4.12.0) once and builds it in the build directory, a static library.  `-DDMODEM_LIBRE=system` requires an installed libre, `-DDMODEM_LIBRE=fetch` always builds the pinned one, and `-DDMODEM_LIBRE_TAG=...` picks another release.  The SDP, RTP (G.711) and UDPTL are dmodemd's own.  `-DDMODEM_CRYPTO=mbedtls` builds that libre on [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) (its pinned release, v3.6.7, fetched and built the same way) instead of the system's OpenSSL.

`libdmodem_sip.a` is the modem on a SIP line as a library (`dmodemd/sip_modem.h`): the modem core with its SIP user agent, its DTE any descriptor (a pty, or one end of a socket pair), run in a thread of the program's own and stopped from another; dmodemd is a pty in front of it.  It cross compiles: with a CMake toolchain file (the Android NDK's, say: `-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26`) libre and its crypto are fetched and built with the same toolchain, Mbed TLS by default for Android, which has no OpenSSL of its own; a system without `/etc/resolv.conf` (Android) gives the DNS servers in `sip::Config`.  The tree also builds as another CMake project's subdirectory.

### Windows
dmodemd also builds for Windows as a single static `dmodemd.exe`: with MSVC on Windows (`cmake -G Ninja` in a Developer Command Prompt), or from Linux with MinGW-w64 (`-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake`) or clang-cl against MSVC's libraries (`cmake/clang-cl.cmake`).  Its DTE is a COM port (`--com COM5`, one end of a [com0com](https://sourceforge.net/projects/com0com/) pair), a named pipe (`--pipe`, `--pipe-connect`: a virtual machine's COM port, so a guest can dial out through it) or a TCP connection (`--tcp`, by default 127.0.0.1:2323).  The PBX modules and the ALSA line are Linux-only.  See [docs/windows.md](docs/windows.md).

Tests: `ctest --test-dir build -L quick` runs the fast ones (a few seconds; GoogleTest programs, with the system's libgtest-dev or a copy fetched while configuring); `-L spandsp` (with libspandsp-dev) and `-L smartlink` are longer.  The interop tests run the modem against other soft modems ([docs/interop.md](docs/interop.md)); `-L smartlink` against Smart Link's original binary-only i386 DSP library (`dsp/test/smartlink/dsplibs.o`), kept only as a test peer; they need a 32-bit build environment (`g++ -m32`; Debian-based systems can install `g++-multilib`) and are skipped without one.  See [docs/testing.md](docs/testing.md).

### DSP
The modem's signal processing is open source, in `dsp/` (C++, behind the C interface of Smart Link's original binary-only library that Smart Link's slmodemd was written for, `dsp/dsplibs.h`); it builds on any architecture.  It implements V.8 and V.34 (up to 33600 bps), V.32bis (14400 bps), V.32, V.22bis, V.22, Bell 212A, V.23, V.21 and Bell 103, class 1 fax (V.17, V.29, V.27ter and V.21 HDLC), call progress, caller ID and voice.  It implements V.90 only as the digital modem (on an 8000 Hz G.711 line), with V.92's quick connect, PCM upstream (up to 36000 bps upstream in the tests) and its rate renegotiation, and V.91 between two digital modems (up to 62667 bps both ways).  See [docs/dsp/](docs/dsp/README.md) for details and [docs/testing.md](docs/testing.md) for the tests.  Between the data pumps and the DTE, `link/` is the data link layer: V.42 error correction (LAPM, with its detection phase), MNP 2 to 4 (V.42's alternative procedure) for modems without LAPM, V.44, V.42bis and MNP 5 compression and plain asynchronous characters ([docs/link.md](docs/link.md)).  `core/` is the modem core in C++ (the AT commands, S-registers, result codes, the call's state machine and its timers), with the behaviour of Smart Link's slmodemd core; slmodemd, libdmodem and modem_test run on it ([docs/core.md](docs/core.md)).

### Asterisk module
`asterisk/` has `app_dmodem`, a dialplan application that answers a call on an Asterisk channel with the open modem and hands the connection to a server (TCP or a unix socket) or a program on a pty such as pppd, per profile in `dmodem.conf`, and `DModemSendFax` and `DModemReceiveFax`, a group 3 fax machine on the audio or over T.38 (PJSIP).  It is built on `libdmodem` (`dmodemd/dmodem.h`, and `dmodemd/dmodem.hpp` with C++ classes for it), the modem core and open DSP as a library.  See [docs/asterisk.md](docs/asterisk.md).

`ari/` has `dmodem-ari`, the same as a program of its own beside Asterisk, an application of the Asterisk REST Interface (ARI): the dialplan's `Stasis(dmodem,<profile>)` hands it a call, and `Stasis(dmodem,sendfax,<file>)` or `Stasis(dmodem,receivefax,<file>)` a fax, whose audio it takes over RTP from an external media channel, or from a SIP leg of its own that can go over to T.38 (`t38 = yes`).  Nothing of D-Modem is loaded into Asterisk.  It is a prototype; see [docs/ari.md](docs/ari.md).

### FreeSWITCH module
`freeswitch/` has `mod_dmodem`, the same for FreeSWITCH: the `dmodem` application (a server over TCP or a unix socket, or a program on a pty, per profile in `dmodem.conf.xml`) and the fax applications `dmodem_send_fax` and `dmodem_receive_fax`, on the audio or over T.38 (sofia).  See [docs/freeswitch.md](docs/freeswitch.md).

The two modules share `pbx/dmodem_bridge.c` on libdmodem's C API: the profile's options, the modem with its DTE on a socketpair, and the data between it and the profile's server (TCP or a unix socket, optionally with the call's details first) or program on a pty.  Each module keeps its PBX's side (the channel's frames, its variables and log, the fax applications).

### Login menu (optional)
[modemmenu](https://github.com/Neo-Desktop/modemmenu) is a login and menu for callers, in Go: it checks a user name and password against a text file, LDAP, an SQL database or PAM, then offers a BBS, PPP or a shell, started with the caller's credentials, the dialed number, the connect rate and the time of the call.  Either module runs it as a `mode = exec` profile, or connects calls to one `modemmenu -serve` by a `mode = unix` profile with `header = yes`.

## Documentation
The technical documentation is in [docs/](docs/README.md): how the DSP, the data link layer and the modem core work, the Asterisk and FreeSWITCH modules, the tests and the interop results against other soft modems.  [docs/specs/](docs/specs/README.md) lists the ITU-T Recommendations implemented, with a script that downloads them.

## How it Works
Traditional “controller-based” modems generally used a microcontroller and a DSP to handle all aspects of modem communication on the device itself.  Later, so-called “Winmodems” were introduced that allowed for field-programmable DSPs and moved the controller and other functionality into software running on the host PC.  This was followed by “pure software” modems that moved DSP functionality to the host as well.  The physical hardware of these softmodems was only used to connect to the phone network, and all processing was done in software. 

D-Modem replaces a softmodem’s physical hardware with a SIP stack.  Instead of passing audio to and from the software DSP over an analog phone line, audio travels via the RTP (or SRTP) media streams of a SIP VoIP call.   

## Usage
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

You can now interact with this terminal (almost) as you would with a normal modem using standard AT commands; [docs/at_commands.pdf](docs/at_commands.pdf) lists them all. 

Because there isn’t any dial tone on our SIP connection, you’ll need to disable dial tone detection: 

    atx3 
    OK

By default the modem negotiates the best common modulation (V.8, then V.34 at up to 33.6kbps, falling back to V.32bis and below).  dmodemd keeps the call's audio free of sample slips (a fixed jitter buffer, the modem at the call's own 8000 Hz), which V.34 needs; over a clean G.711 path (the tests in `asterisk/test`) V.34 connects at 33600.  A real PSTN gateway adds its own impairments, so you may still want to select a modulation and data rate manually.  For example, the following command selects V.32bis with a data rate of 4800 – 9600 bps.  Refer to the manual for further details. 

    at+ms=132,0,4800,9600 
    OK

### Fax
dmodemd is a class 1 fax modem (`AT+FCLASS=1`) for fax software on its
pty such as efax (`efax -d /dev/ttyDM0 -o1 -iX3 -t <number> page.tif`;
`-iS0=1 -w -r <prefix>` waits for a call and receives; efax reads a TIFF
page up to its RTC, as efix writes them).  A fax call goes
over to T.38 when the other end asks for it, or a second after dmodemd
answered one: the same commands and results for the software, the call's
UDPTL instead of its audio.  `SIP_T38=no` keeps faxes on the audio.

### Voice
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

### Receiving calls
With `-L` (listen), dmodemd registers with the SIP server, so that calls to the account reach the modem: it reports `RING`, and answers on `ATA` or by itself after `ATS0=n` rings.  Dialing works as before in this mode.

    # ./build/dmodemd/dmodemd -L

### Dialing
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
 
### Softmodem hardware
dmodemd can be the modem of an AC'97 or HD-audio softmodem (the Si3054 kind) that Linux still drives through ALSA.  It replaces Smart Link's closed, i386-only slmodemd, on any architecture:

    # ./build/dmodemd/dmodemd -a hw:1

`aplay -l` lists the card's "Modem" device (an HD-audio modem is a device of the audio card, e.g. `hw:0,6`).  The modem dials in DTMF after the dial tone (`ATX4`), hears rings and caller ID on hook, and connects at V.34 and below, with fax and voice too.  An HD-audio modem codec that is Si3054-compatible but outside the kernel driver's list of IDs gets the driver with `hda-modem-setup write` (snd-hda-intel's early patching).  [docs/hardware.md](docs/hardware.md) lists the hardware and the limits.  It has been tested on a simulated line (`ctest -L alsa`), not yet on a real codec.

## Known Issues / Future Work
- For a server answering many calls (a BBS, PPP dial-in or fax), the Asterisk module in `asterisk/` or the FreeSWITCH module in `freeswitch/` is the better fit than `dmodemd -L`: it answers with the open modem at V.91 (another digital modem calling, up to 62667 bps both ways), V.90 (as the digital modem, up to 56000 bps downstream), V.34, V.32bis, V.22bis, V.22 or V.21.
- Tested over SIP against a local Asterisk and a local FreeSWITCH only, not yet through real PSTN gateways.
- Of V.92, the digital modem has quick connect (short phase 1) and short phase 2 for V.92 analogue modems calling, PCM upstream (36000 bps upstream in the tests, against 31200 for V.34; `DSP_V92_PCMUP=0` leaves it out) fast parameter exchange and modem-on-hold (it holds the call for an analogue modem taking a waiting call, and picks it up again; `AT+PMH`, `AT+PMHT`; and `AT+PMHR` asks the analogue modem to go on hold, though the modems we test with retrain instead). V.34, V.90 and V.92 calls renegotiate their rates when the line gets a little worse or better (both ways: ours, or the analogue modem's), and retrain after a line hit or a large change.
- V.90 needs an analogue modem at the other end: dmodemd is always on the digital side of the network. Two digital modems (D-Modem calling the Asterisk module, or another D-Modem) connect at V.91 instead: up to 62667 bps both ways on a clean G.711 call (about 58700 bps of data through V.42), less where the network robs bits or adds a pad; `AT+MS=91` asks for V.91 (or V.34), the default `AT+MS=92` offers it with V.90.
- The serial interface could be replaced with stdio or a socket, and common AT configuration options could be exposed as command line options 


## Acknowledgements
D-Modem was created by Aon's Cyber Labs in 2021, who published it with the article [Introducing D-Modem: A Software SIP Modem](https://www.aon.com/cyber-solutions/aon_cyber_labs/introducing-d-modem-a-software-sip-modem/).  This project grew from their idea and their code: slmodemd's audio on a socket, and their d-modem program, a SIP user agent beside it.  Their code has since been replaced; the SIP user agent, `dmodemd/sip.cpp`, was written anew from a specification of what it does.  The modem daemon began as Smart Link's `slmodemd` (from Debian's sl-modem-daemon package); its code has been replaced too, written anew from specifications of what it does.  SIP is handled by [libre](https://github.com/baresip/re).

## License

D-Modem is copyright Neo-Desktop, under the GNU Affero General Public License version 3 only ([COPYING](COPYING)), or under a commercial licence from Neo-Desktop.  See [LICENSING.md](LICENSING.md) for the parts under other licences: the Asterisk module (GPL-2.0) and the interop test binaries in `dsp/test/`, which belong to their owners and are **not** subject to this repository's terms ([dsp/test/NOTICE.md](dsp/test/NOTICE.md)).
