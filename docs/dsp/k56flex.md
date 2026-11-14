# K56flex

K56flex was the 56k modem standard of Rockwell and Lucent from 1997. Then
V.90 replaced it. Many modems of that time had it. Nobody published its
specification.

**D-Modem can be the two ends of a K56flex call.** With `AT+MS=56`:

- On a digital line, it is the **server**. This is the role that the modems of an internet
  provider had. It answers a K56flex modem with K56flex, and
  all other modems with V.90.
- On an analogue line (dmodemd `-a`, at 9600 Hz), it is the **modem of the
  customer**. It calls with K56flex when the server answers with the
  first signal of K56flex. It calls with V.90 when the server answers as a V.90
  server.

We learned about K56flex from the one K56flex modem
that was available, the SM56 soft modem of Motorola. We read its code and we monitored
its calls with D-Modem. For the details, refer to
[the technical details](k56flex-details.md).

## What is known

K56flex has a digital end (the server) and an analogue end
(the modem of the customer), as V.90 has. We know its start-up and its data mode:

1. The modems announce K56flex with V.8bis. Then they agree on V.34 with V.8.
2. They measure the line as V.34 does, with a K56flex marker in one of
   the messages.
3. The server sends a training sequence, and the modem of the customer
   trains on it.
4. They exchange their settings.
5. In data mode, the server sends the data in frames of 8 digital values.
   Most of the bits of each frame select 8 "rings" of values together.
   This keeps the average power low. The remaining bits select a value in each ring
   and its sign. At intervals of some values, one sign keeps the signal
   balanced around zero, and does not carry data.

We sent the output of the encoder into the decoder of the SM56 modem. The
data came back without changes at all speeds from 32000 to 56000 bit/s. Two
Rockwell patents describe the same methods.

## How it was tested

In the test suite, the SM56 modem calls D-Modem over a simulated
digital line. They connect at 44000 bit/s down and 31200 bit/s up, with
V.42 error correction. They send data in the two directions.

The modem of the customer of D-Modem calls the server of D-Modem over a
simulated phone network. They connect at 56000 bit/s down and 31200
bit/s up. They send 200000 bytes in each direction without an error. The same call
also operates in these conditions:

- On an A-law network.
- With the codec clock of the customer 100 parts per million different from the clock of the network.
- With an echo on the line of the customer.
- Against a server that has only V.90 (then the call uses V.90).

## What is missing

- We tried the modem of the customer only against the server of D-Modem.
  We built that server from the behavior of one K56flex client. The K56flex server
  of a real provider can be different.
- The modem of the customer does not measure the digital loss or the
  robbed bits of the network, and it does not ask for them. It assumes a clean line.
- A-law with a real K56flex modem: the code is written, but we did not try it.
- Retrains and speed changes during a call.

The modem driver of Conexant is the other driver in the test suite. Its K56flex
support is disabled, thus it cannot help.
