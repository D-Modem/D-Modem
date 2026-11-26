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
- On a digital line, it is also the modem of the customer when it calls
  a K56flex server (refer to
  [The 56k client on a SIP line](sip-client.md)).

We got our data about K56flex from the one K56flex modem
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
   This keeps the mean power low. The remaining bits select a value in each ring
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
simulated telephone network. They connect at 56000 bit/s down and 31200
bit/s up. They send 200000 bytes in each direction without an error. The same call
also operates in these conditions:

- On an A-law network.
- With the codec clock of the customer 100 parts per million different from the clock of the network.
- With an echo on the line of the customer.
- Against a server that has only V.90 (then the call uses V.90).

The network can make the signal weaker by 3 or 6 dB (a "digital pad").
On some lines, the network also uses the last bit of each sixth value for its own
signals ("robbed bits"). The modem of the customer measures the two items during the
training and tells the server. Then the server uses only the values that
the modem of the customer can still identify. The tests send data with these conditions:

- A 3 dB pad (54000 bit/s).
- Robbed bits (50000 bit/s or more).
- A 6 dB pad with robbed bits (46000 bit/s).

The SM56 modem also receives our data correctly when there are robbed bits on its
line.

The modems can also change speed during a call. If the line becomes
worse or better, the modem of the customer asks for a new speed, as the
SM56 modem does. The server can also start a speed change. If the
line becomes much worse, the modems retrain: they measure the line
again and train again, without a new CONNECT. Each of the two modems can start
a retrain. The tests change the speed and retrain in the two directions, and
also when noise starts during a call. The data stays correct.

## What is missing

- We tried the modem of the customer only against the server of D-Modem.
  We built that server from the behavior of one K56flex client. The K56flex server
  of a real provider can be different.
- The SM56 modem cannot train on a line with a digital pad. This is a
  fault in SM56 itself, not in D-Modem.
- A-law with a real K56flex modem: the code is written, but we did not try it.
- After a speed change, the SM56 modem does not receive our data
  correctly. Then it retrains, and after the retrain the data is
  correct. In a retrain, the training of the SM56 modem fails about half
  of the time. Then the modems retrain again. The probable cause is
  in the SM56 modem: it keeps values that it changed in data mode.

The modem driver of Conexant is the other driver in the test suite. Its K56flex
support is disabled, thus it cannot help.

We also read the firmware of two K56flex modems (Rockwell and Zoom/Lucent).
The firmware contains only the controller code, not the signal
processing. Thus, it does not show how the protocol operates. It shows the
data that these modems report about a K56flex call, for example the digital pad
and the robbed bits.
