# Code review (October 2026)

In October 2026 the whole of D-Modem's code was reviewed. Every function
got a comment explaining what it does, small and clearly correct fixes
were made straight away (each with a test where a test could catch the
bug), and anything bigger was written down to be dealt with.

**Result: every problem the review found has now been fixed.** Nothing
is left open.

The [technical details](review-details.md) list each fix: the file, what
was wrong, and the test that now checks it.

## What kinds of problems were fixed

- **Error correction and compression.** Bad or oversized data frames
  from the other modem could overflow a buffer; some timers and retry
  counts did not match the V.42 standard; one setting combination
  between two modems was negotiated wrongly. A modem that called a
  modem without error correction could wait for ever.
- **The AT commands.** Some commands answered with the result of the
  previous command; `AT+MS=56` (K56flex) was refused; data left over at
  the end of a call could later be run as AT commands.
- **Fax.** Several ways a fax page or frame could be lost or sent twice,
  and some bad input from a fax file that could crash the program or loop
  for ever.
- **The modem's signal processing.** Unusual or bad messages from the
  other modem during start-up could crash the modem; on noisy lines V.34
  could choose a speed that could not work; some start-up messages that
  went missing were never sent again; leftover sound could reach the line
  at the end of a block; the answer tone came a second late. Code shared
  between calls running at the same time was made safe for that.
- **The daemon and the phone-system modules.** Dialling `ATDT555...`
  sent the `T` as part of the number; a SIP password containing `@` was
  read wrongly; memory and file handles leaked between calls; files that
  need root were opened too late; and a slow server could make the call
  drop while the modem waited for it.
- **The tests themselves.** Several tests passed when they should have
  failed, or failed now and then for reasons that had nothing to do with
  D-Modem (timing, leftover memory in the test harness). These were
  fixed so a failure now means a real problem.

## Checked and left alone

One thing that looked wrong was checked and kept: in V.92's quick start,
D-Modem ignores one flag about which carrier frequency the other modem
uses, because the real modems tested send on the same carrier anyway, and
following the flag made one of them retrain.
