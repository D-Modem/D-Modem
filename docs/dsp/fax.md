# Fax

D-Modem can operate as a fax modem for fax programs on your computer. It can
also operate as a complete fax machine for the phone-system modules and
libdmodem. Faxes can go over the audio of an ordinary phone call, or over the
internet with T.38.

For each command, frame and timing, refer to
[the technical details](fax-details.md).

## Fax modem for fax programs

Fax programs (HylaFAX, efax, Windows Fax and Scan and other programs) communicate with a
fax modem with one of three command sets. D-Modem has all of them:

| Command set | Turn on with | Who does the fax protocol |
|---|---|---|
| **Class 1** | `AT+FCLASS=1` | the fax program; the modem only sends and receives the signals |
| **Class 1.0** | `AT+FCLASS=1.0` | as class 1, with V.34 fax too |
| **Class 2** | `AT+FCLASS=2` | the modem |
| **Class 2.0** | `AT+FCLASS=2.0` | the modem (the standardised version of class 2) |

Class 1 has the widest support and is the most flexible. Classes 2
and 2.0 are applicable to simpler programs.

The fax speeds are the usual speeds: V.27ter (2400 and 4800 bit/s), V.29
(7200 and 9600) and V.17 (up to 14400). In class 2 and 2.0, **V.34 fax**
goes up to 33600 bit/s when the other machine also has it. To use it, set the
highest speed to 13 with error correction enabled (for example
`AT+FIS=1,13,0,2,0,2,0,0`). A machine without V.34 gets the
usual speeds. To poll (get a document) on V.34, send `AT+FSP=1`
before you dial.

In class 1.0 (`AT+FCLASS=1.0`), fax programs that know V.34 fax (for example
HylaFAX) get it in the same method. The modem sends `+F34:` with the speed
before its `CONNECT`. Then the program sends and receives the pages at
up to 33600 bit/s. `AT+F34=` sets the highest and lowest speed (in
steps of 2400 bit/s). `AT+F34=0` disables V.34. With a machine that has
no V.34, the call continues as ordinary class 1.

## Built-in fax machine

D-Modem also has its own complete fax machine (the T.30 protocol). These
items use it: the fax applications of [Asterisk](../asterisk.md), [FreeSWITCH](../freeswitch.md) and
[dmodem-ari](../ari.md), and libdmodem. It sends and
receives TIFF fax files. It uses error correction mode when the other
machine supports it. Thus, pages arrive without damaged lines.

When error correction is enabled, the pages also go in **T.85** (JBIG) coding
if the other machine accepts it. This compression is better than the usual
MH coding. Thus, a page of text takes about half the time on the line.
The fax machine saves the pages that come in T.85 as ordinary fax TIFF files.

It also sends and receives **colour and grey-scale** pages (T.42, JPEG
coding), also with error correction enabled. A colour or grey page in the
TIFF file to send (8-bit RGB or grey, uncompressed) goes in one of these forms:

- In colour, if the other machine accepts colour.
- In grey, if it accepts only grey.
- As a black-and-white dithered page, if it accepts neither.

The fax machine saves a received colour page in colour in the received TIFF file.
Programs that accept only black and white (class 2 fax software) get a dithered copy.

Pages with **few colours** (for example charts, diagrams and coloured text)
go **without any loss** (T.43, JBIG coding of the colours) if the
other machine accepts that. The colours come out as they went in, and
the page is much smaller than in JPEG (a bar chart of five colours: 199
octets instead of 25809). Photographs and other pages of many colours
go in JPEG. The fax machine saves the pages that come in T.43 as it saves JPEG pages.

Pages with **text over a picture** go in **T.44** (mixed raster
content) if the other machine accepts it. The shapes of the text stay
sharp in black and white, and the picture goes in JPEG at half the
resolution below the text. A page of text over a photo is much smaller than
in JPEG (the page of the test takes about a quarter of the time), and its
text is sharper. The fax machine saves the pages that come in T.44 as it saves JPEG
pages.

### Binary files

When error correction is enabled, the fax machine can also send and receive
**binary files** (T.434) in the same call as the pages. Any file,
with its name and type, arrives exactly as it was sent. A program
that uses libdmodem enables this with the `bft` field of `t30_config`. A machine that
does not accept files gets the pages, and the files do not go.

## Fax over the internet (T.38)

On many internet phone calls, fax tones become damaged. **T.38** sends
the fax as data packets instead of sound. The built-in fax machine
and the class 1 fax modem can both use T.38. A call can change to it
during the call.

## Testing

The fax tests do these checks:

- They send pages through each fax speed on a simulated line.
- They run two fax machines against each other (on audio and over T.38, with lost
  packets).
- They check D-Modem against the fax code of spandsp and against the fax modems of other
  companies.

Refer to [Building and testing](../testing.md).
