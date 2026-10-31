# K56flex

K56flex was Rockwell's and Lucent's 56k modem standard from 1997, before
V.90 replaced it. Many modems of that time had it. Its specification was
never published.

**D-Modem does not support K56flex yet.** `AT+MS=56` runs V.90 instead.

This page records what has been worked out about K56flex so far, as the
start of a K56flex server in D-Modem. It was learned from the one
K56flex modem available, Motorola's SM56 soft modem, by reading its code
and watching its calls with D-Modem. For the details, see
[the technical details](k56flex-details.md).

## What is known

Like V.90, K56flex has a digital end (the server) and an analogue end
(the customer's modem). Its start-up is known from the first tone up to
the point where data starts, and so is its data mode:

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

D-Modem has the data mode's encoder and decoder. The encoder's output
was fed into the SM56 modem's own decoder, and the data came back intact
at every speed from 32000 to 56000 bit/s. Two Rockwell patents describe
the same methods.

## What is missing

The start-up is not built into a working modem yet, so D-Modem cannot
make a K56flex call. A few smaller points (some timings, one bit's
polarity, how A-law modems behave) are also still open.

Conexant's modem driver, the other one in the test suite, turned out to
have its K56flex support switched off, so it cannot help.
