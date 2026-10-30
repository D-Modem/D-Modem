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
the point where data would start:

1. The modems announce K56flex with V.8bis, then agree on V.34 with V.8.
2. They measure the line as V.34 does, with a K56flex marker in one of
   the messages.
3. The server sends a training sequence, and the customer's modem
   trains on it.
4. They exchange their settings.

## What is missing

The way the server turns data into digital values once the call is up
is not known. Without it, a K56flex server can train with a K56flex
modem but cannot carry any data. A few smaller points (some timings,
one bit's polarity, how A-law modems behave) are also still open.

Conexant's modem driver, the other one in the test suite, turned out to
have its K56flex support switched off, so it cannot help.
