# V.8bis: agreeing on a mode before the modems start

When two modems connect, they normally go straight into V.8: the
answering modem plays its answer tone and the two agree on a modulation.
**V.8bis** (ITU-T V.8 bis) is an extra step that can come before that.
In a few short tones and messages, the two ends tell each other which
*kinds* of call they can make (plain data, voice and data together,
video telephony, fax, text telephone) and pick one, before any modem
sound starts. It was designed to work even in the middle of a telephone
conversation, quietly enough not to disturb the people talking.

D-Modem needs it for **V.70**, voice and data at the same time on one
call, where V.8bis is how the two ends agree to switch to that mode.
D-Modem runs a V.8bis exchange at the start of a data call, then
carries on with V.8 and the usual modulations; with `AT+DSVD=1` at both
ends it chooses V.70 ([voice and data together](../v70.md)), and with
`AT+ASVD=1` at both V.61 ([ASVD](v61.md)), whose own start-up then takes
the place of V.8.

For the full technical description, see
[the technical details](v8bis-details.md).

## Turning it on

V.8bis is off unless asked for, with V.251's command:

    AT+A8E=1,1,,1

The last number turns V.8bis on (`1`) or off (`0`, the default). It
must be on at both ends for the exchange to happen.

- **Answering**, the modem sends V.8bis's opening signal before its
  answer tone and waits up to 3 seconds for a reply. If the caller does
  not reply, it goes on with the normal answer tone, so a caller without
  V.8bis still connects (about 3.5 seconds later than usual).
- **Calling**, the modem listens for that opening signal as well as the
  answer tone, and replies to it. A modem that answers without V.8bis is
  called as usual.

`AT+A8E?` shows the settings and `AT+A8E=?` what is accepted.

## What the user sees

Nothing changes on the AT command side: `ATD` and `ATA` work as usual
and `CONNECT` reports the speed. With both ends on, the call takes about
two seconds longer to start, the time of the exchange.

## Limits

- Only the data mode and voice and data (V.70 with `AT+DSVD=1`, V.61
  with `AT+ASVD=1`) are chosen; the other modes V.8bis knows (video telephony, fax, text
  telephones) are not.
- D-Modem uses the exchanges where the answering modem stays the
  answering modem of the start-up that follows. A caller that asks the
  other way round (V.8bis transaction 7, or an answerer that sends its
  choice first) gets the normal start-up without V.8bis instead.
- The commands V.251 has for a terminal to run V.8bis itself (`+A8M`,
  the `+A8x` reports) are not there.
- It has been tested between two D-Modems, not yet against another
  maker's V.8bis modem.
