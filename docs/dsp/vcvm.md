# V.cvm: data over mobile phone calls

Usual modems do not operate through mobile telephone calls. Mobile networks
(also VoLTE and 5G calls) compress speech with codecs, for example AMR,
AMR-WB and EVS. These codecs keep the natural sound of a voice. They do
not keep the tones of a modem. V.34 and V.32 do not go through.

**V.cvm** is the proposal of D-Modem for a modem that goes through. It has
the form of an ITU-T Recommendation ([v_cvm.pdf](../specs/v_cvm.pdf), with its
LaTeX source in [cellular/](../cellular/v_cvm.tex)). The Recommendation
gives the reasons and the measurements for it. This page is about the
implementation in D-Modem. To start it, use `AT+MS=VCVM`.

For the steps of the start-up, the files, the receivers and the full list
of tests, refer to [the technical details](vcvm-details.md).

## How it operates

1. **V.8** at the start, as usual. If the two modems have V.cvm, they use
   it. If not, they use V.22 or V.21 as the fallback.
2. **Probe signals.** The modems send test signals to learn which signals
   this codec lets through. The start-up takes 15 to 21 seconds.
3. **Data.** The modems send data on the tones that the codec does not
   change. Some codec settings let no usual signal through. For these
   settings, a special "parametric" mode sends the data with the model
   of voice pitch in the codec.

The speeds are from 175 to 3500 bit/s. The speed changes with the codec.

## Compression for short messages

At these low speeds, the data of a call is frequently a small number of
short messages. Examples are a value from a sensor or a short email.
Usual compression becomes good only after it has received much data. Thus,
V.cvm can start the compression with a built-in dictionary of common
text. This dictionary has web, email, JSON, XML, GPS sentences and plain
English. It decreases the size of short messages by about half. Between
two D-Modems, the modems use zstd compression with the same dictionary.

## Status and limits

- We tested it between two D-Modems through simulated AMR and AMR-WB
  codecs. The tests do not have EVS.
- The code has only the narrowband (8000 Hz) interface.
- If the start-up fails, the call stops. It does not use V.22 as a
  fallback.
- There are two known issues:
  - After long tests through AMR 7.4, a data error that the modem does
    not find occurs rarely.
  - Through AMR-WB at its highest rate, the start-ups are slow.
