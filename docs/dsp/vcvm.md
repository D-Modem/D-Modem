# V.cvm: data over mobile phone calls

Ordinary modems do not work through mobile phone calls. Mobile networks
(including VoLTE and 5G calls) squeeze speech with codecs such as AMR,
AMR-WB and EVS, which are built to keep a voice sounding natural, not to
keep a modem's tones intact. V.34 and V.32 simply do not get through.

**V.cvm** is D-Modem's proposal for a modem that does. It is written in
the form of an ITU-T Recommendation ([v_cvm.pdf](../v_cvm.pdf), with its
LaTeX source in [cellular/](../cellular/v_cvm.tex)), which explains the
reasons and the measurements behind it. This page is about D-Modem's
implementation, which you start with `AT+MS=VCVM`.

For the start-up's steps, the files, the receivers and the full test
list, see [the technical details](vcvm-details.md).

## How it works

1. **V.8** at the start, as usual. If both modems have V.cvm, they use
   it; otherwise they fall back to V.22 or V.21.
2. **Probing.** The modems send test signals to learn what this
   particular codec lets through. This takes 15 to 21 seconds.
3. **Data.** They send data on the tones that survive the codec. For
   codec settings that let no ordinary signal through, a special
   "parametric" mode uses the codec's own model of a voice's pitch to
   carry the data instead.

Speeds range from 175 to 3500 bit/s, depending on the codec.

## Compression for short messages

At these low speeds a call's data is often a few short messages, such as
a sensor reading or a short email. Normal compression only gets good
after it has seen a lot of data. So V.cvm can start the compression with
a built-in dictionary of common text (web, email, JSON, XML, GPS
sentences and plain English), which roughly halves short messages.
Between two D-Modems, zstd compression is used with the same dictionary.

## Status and limits

- Tested between two D-Modems through simulated AMR and AMR-WB codecs.
  EVS is not in the tests yet.
- Only the narrowband (8000 Hz) interface is implemented.
- After a failed start-up the call ends instead of falling back to V.22.
- Two known issues: a rare undetected data error after long runs through
  AMR 7.4, and slow start-ups through AMR-WB at its top rate.
