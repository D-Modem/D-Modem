# Known issues and future work

- For a server that answers many calls (a BBS, PPP dial-in or fax), `dmodemd -L` is not the best fit. Use one of these items:
  - dmodem-ari together with Asterisk (`frontend/ari/`).
  - The Asterisk module in `frontend/asterisk/`, built for your own Asterisk.
  - The FreeSWITCH module in `frontend/freeswitch/`.

  These items answer with the open modem at V.91 (when another digital modem calls, up to 62667 bps in the two directions), V.90 (as the digital modem, up to 56000 bps downstream), V.34, V.32bis, V.22bis, V.22 or V.21.
- We tested the modem over SIP only with a local Asterisk and a local FreeSWITCH. We did not test it through real PSTN gateways.
- Of V.92, the digital modem has these features:
  - Quick connect (short phase 1) and short phase 2, for V.92 analogue modems that call.
  - PCM upstream (36000 bps upstream in the tests, compared with 31200 for V.34). `DSP_V92_PCMUP=0` disables it.
  - Fast parameter exchange.
  - Modem-on-hold (`AT+PMH`, `AT+PMHT`). The modem holds the call when an analogue modem accepts a waiting call, and then continues the call. `AT+PMHR` asks the analogue modem to go on hold, but the modems that we use for tests retrain instead.

  V.34, V.90 and V.92 calls negotiate their rates again when the line becomes a little worse or better. This applies in the two directions (our rate, or the rate of the analogue modem). After a line hit or a large change, these calls retrain.
- V.90 must have an analogue modem at the other end, because dmodemd is always on the digital side of the network. Two digital modems (D-Modem that calls the Asterisk module, or another D-Modem) connect at V.91 instead. On a clean G.711 call, V.91 gives up to 62667 bps in the two directions (about 58700 bps of data through V.42). The rate is lower where the network robs bits or adds a pad. `AT+MS=91` asks for V.91 (or V.34). The default, `AT+MS=92`, offers V.91 together with V.90.
- In the future, stdio or a socket can possibly replace the serial interface. Also, command line options can possibly give access to common AT configuration options.
