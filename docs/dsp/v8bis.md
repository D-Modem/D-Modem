# V.8bis: a mode agreement before the modems start

When two modems connect, they usually start V.8 immediately. The
answering modem sends its answer tone, and the two modems agree on a
modulation.
**V.8bis** (ITU-T V.8 bis) is an optional step that can occur before V.8.
With some short tones and messages, the two ends tell each other which
*kinds* of call they can make (plain data, voice and data together,
video telephony, fax, text telephone). Then they select one, before any
modem sound starts. Its design lets it operate also during a telephone
conversation. It is sufficiently quiet that it does not disturb the
persons who talk.

D-Modem must have V.8bis for **V.70** (voice and data at the same time on
one call). With V.8bis, the two ends agree to change to that mode.
D-Modem does a V.8bis exchange at the start of a data call. Then it
continues with V.8 and the usual modulations. These settings change the
mode:

- With `AT+DSVD=1` at the two ends, it selects V.70
  ([voice and data together](../v70.md)).
- With `AT+ASVD=1` at the two ends, it selects V.61 ([ASVD](v61.md)).
  Then the start-up of V.61 replaces V.8.

For the full technical description, refer to
[the technical details](v8bis-details.md).

## How to enable it

V.8bis is disabled until you enable it with the command of V.251:

    AT+A8E=1,1,,1

The last number enables V.8bis (`1`) or disables it (`0`, the default).
For the exchange to occur, enable it at the two ends.

- **When it answers**: the modem sends the opening signal of V.8bis
  before its answer tone. Then it waits up to 3 seconds for a reply. If
  the caller does not reply, the modem continues with the normal answer
  tone. Thus, a caller without V.8bis also connects (about 3.5 seconds
  later than usual).
- **When it calls**: the modem listens for that opening signal and for
  the answer tone, and it replies to the opening signal. If a modem
  answers without V.8bis, the call continues as usual.

`AT+A8E?` shows the settings, and `AT+A8E=?` shows the values that the
modem accepts.

## What the user sees

The AT commands do not change. `ATD` and `ATA` operate as usual, and
`CONNECT` reports the speed. When V.8bis is enabled at the two ends, the
call takes about two seconds more to start. This is the time of the
exchange.

## Limits

- D-Modem selects only the data mode and voice and data (V.70 with
  `AT+DSVD=1`, V.61 with `AT+ASVD=1`). It does not select the other modes
  of V.8bis (video telephony, fax, text telephones).
- D-Modem uses the exchanges in which the answering modem stays the
  answering modem of the subsequent start-up. Some callers ask for the
  opposite (V.8bis transaction 7, or an answerer that sends its selection
  first). Such a caller gets the normal start-up without V.8bis.
- D-Modem does not have the commands of V.251 that let a terminal do
  V.8bis itself (`+A8M`, the `+A8x` reports).
- We tested it between two D-Modems, and against the SM56 modem of
  Motorola as a K56flex client.
- Conexant modems do not always answer V.8bis. The HSF engine of the
  Linux driver (also for the HDA modems of Conexant) has no V.8bis. One
  HCF modem with K56flex did not answer V.8bis (issue #119), and we do
  not know the cause. With these modems, the call continues with the
  normal answer tone.
  Refer to [the technical details](v8bis-details.md#against-other-modems).
