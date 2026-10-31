# K56flex

K56flex was Rockwell's and Lucent's 56k modem standard from 1997, before
V.90 replaced it. Many modems of that time had it. Its specification was
never published.

**D-Modem can be either end of a K56flex call.** With `AT+MS=56`:

- On a digital line it is the **server**, the role an internet
  provider's modems had. It answers a K56flex modem with K56flex, and
  any other modem with V.90.
- On an analogue line (dmodemd `-a`, at 9600 Hz) it is the **customer's
  modem**. It calls with K56flex when the server answers with K56flex's
  first signal, and with V.90 when the server answers like a V.90
  server.

What D-Modem knows about K56flex was learned from the one K56flex modem
available, Motorola's SM56 soft modem, by reading its code and watching
its calls with D-Modem. For the details, see
[the technical details](k56flex-details.md).

## What is known

Like V.90, K56flex has a digital end (the server) and an analogue end
(the customer's modem). Its start-up and its data mode are both known:

1. The modems announce K56flex with V.8bis, then agree on V.34 with V.8.
2. They measure the line as V.34 does, with a K56flex marker in one of
   the messages.
3. The server sends a training sequence, and the customer's modem
   trains on it.
4. They exchange their settings.
5. In data mode, the server sends the data in frames of 8 digital values.
   Most of each frame's bits choose 8 "rings" of values at once, which
   keeps the average power low; the rest choose a value within each ring
   and its sign. Every few values, one sign is set to keep the signal
   balanced around zero instead of carrying data.

The encoder's output was fed into the SM56 modem's own decoder, and the
data came back intact at every speed from 32000 to 56000 bit/s. Two
Rockwell patents describe the same methods.

## How it was tested

In the test suite, the SM56 modem calls D-Modem over a simulated
digital line. They connect at 44000 bit/s down and 31200 bit/s up, with
V.42 error correction, and pass data both ways.

D-Modem's own customer's modem calls D-Modem's own server over a
simulated phone network. They connect at 56000 bit/s down and 31200
bit/s up and pass 200000 bytes each way without an error. The same call
also works on an A-law network, with the customer's codec clock 100
parts per million off the network's, with an echo on the customer's
line, and against a server that only has V.90 (it then runs V.90).

## What is missing

- The customer's modem has only been tried against D-Modem's own
  server, which was itself built from what one K56flex client does. A
  real provider's K56flex server may differ.
- The customer's modem does not measure the network's digital loss or
  robbed bits, and asks for neither; it assumes a clean line.
- A-law with a real K56flex modem: written, but not tried.
- Retrains and speed changes during a call.

Conexant's modem driver, the other one in the test suite, turned out to
have its K56flex support switched off, so it cannot help.
