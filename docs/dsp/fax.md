# Fax

D-Modem can work as a fax modem for fax programs on your computer, and
as a complete fax machine of its own for the phone-system modules and
libdmodem. Faxes can go over an ordinary phone call's audio or over the
internet using T.38.

For every command, frame and timing, see
[the technical details](fax-details.md).

## Fax modem for fax programs

Fax programs (HylaFAX, efax, Windows Fax and Scan and so on) talk to a
fax modem with one of three command sets. D-Modem has all of them:

| Command set | Turn on with | Who does the fax protocol |
|---|---|---|
| **Class 1** | `AT+FCLASS=1` | the fax program; the modem only sends and receives the signals |
| **Class 1.0** | `AT+FCLASS=1.0` | as class 1, with V.34 fax too |
| **Class 2** | `AT+FCLASS=2` | the modem |
| **Class 2.0** | `AT+FCLASS=2.0` | the modem (the standardised version of class 2) |

Class 1 is the most widely supported and the most flexible. Classes 2
and 2.0 suit simpler programs.

The fax speeds are the usual ones: V.27ter (2400 and 4800 bit/s), V.29
(7200 and 9600) and V.17 (up to 14400). In class 2 and 2.0, **V.34 fax**
goes up to 33600 bit/s when the other machine has it too: set the
highest speed to 13 with error correction on (for example
`AT+FIS=1,13,0,2,0,2,0,0`). A machine without V.34 still gets the
usual speeds. To poll (fetch a document) on V.34, send `AT+FSP=1`
before dialling.

In class 1.0 (`AT+FCLASS=1.0`), fax programs that know V.34 fax, such as
HylaFAX, get it the same way: the modem says `+F34:` with the speed
before its `CONNECT`, and the program sends and receives the pages at
up to 33600 bit/s. `AT+F34=` sets the highest and lowest speed (in
steps of 2400 bit/s; `AT+F34=0` turns V.34 off). With a machine that has
no V.34, the call carries on as ordinary class 1.

## Built-in fax machine

D-Modem also has its own complete fax machine (the T.30 protocol), used
by the [Asterisk](../asterisk.md), [FreeSWITCH](../freeswitch.md) and
[dmodem-ari](../ari.md) fax applications and by libdmodem. It sends and
receives TIFF fax files, and uses error correction mode when the other
machine supports it, so pages arrive without damaged lines.

With error correction on, the pages also go in **T.85** (JBIG) coding
when the other machine takes it: a better compression than the usual
MH coding, so a page of text takes about half the time on the line.
Pages that come in T.85 are saved as ordinary fax TIFF files.

It also sends and receives **colour and grey-scale** pages (T.42, JPEG
coding), again with error correction on. A colour or grey page in the
TIFF file to send (8-bit RGB or grey, uncompressed) goes in colour when
the other machine takes it, in grey when it takes only grey, and as a
black-and-white dithered page otherwise. A colour page that comes in is
saved in colour in the received TIFF file; programs that take only
black and white (class 2 fax software) get a dithered copy.

## Fax over the internet (T.38)

Fax tones do not survive many internet phone calls well. **T.38** sends
the fax as data packets instead of sound. Both the built-in fax machine
and the class 1 fax modem can use T.38, and a call can switch to it in
the middle.

## Testing

The fax tests send pages through each fax speed on a simulated line, run
two fax machines against each other (on audio and over T.38, with lost
packets), and check D-Modem against spandsp's fax code and against other
companies' fax modems. See [Building and testing](../testing.md).
