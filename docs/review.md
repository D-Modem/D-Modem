# Code review (October 2026)

In October 2026, we reviewed all of the code of D-Modem. During the review, we did these tasks:

- We gave each function a comment that tells what it does.
- We made small fixes that were clearly correct immediately. Each fix has a test, where a test can find the bug.
- We wrote down each larger problem, to correct it later.

**Result: we corrected all the problems that the review found.** No problem
stays open.

The [technical details](review-details.md) give each fix: the file, the
problem, and the test that now checks it.

## What kinds of problems were fixed

- **Error correction and compression.**
  - Bad or too large data frames from the other modem could cause a buffer overflow.
  - Some timers and retry counts were different from the V.42 standard.
  - The two modems negotiated one combination of settings incorrectly.
  - A modem that called a modem without error correction could wait for ever.
- **The AT commands.**
  - Some commands answered with the result of the previous command.
  - The modem refused `AT+MS=56` (K56flex).
  - Data that stayed at the end of a call could later run as AT commands.
- **Fax.**
  - In some conditions, the modem lost a fax page or frame, or sent it two times.
  - Some bad input from a fax file could cause the program to crash or to loop for ever.
- **The signal processing of the modem.**
  - Unusual or bad messages from the other modem during start-up could cause the modem to crash.
  - On noisy lines, V.34 could select a speed that could not operate.
  - When some start-up messages did not arrive, the modem did not send them again.
  - Sound that stayed at the end of a block could go to the line.
  - The answer tone was one second late.
  - We made the code that calls share, when they run at the same time, safe for this use.
- **The daemon and the phone-system modules.**
  - When you dialled `ATDT555...`, the modem sent the `T` as part of the number.
  - The modem read a SIP password that contains `@` incorrectly.
  - Memory and file handles leaked from one call to the next.
  - The modem opened files that must have root access too late.
  - A slow server could cause the call to drop while the modem waited for the server.
- **The tests themselves.**
  - Some tests passed when they must fail.
  - Some tests failed at random times for reasons that were not related to
    D-Modem (timing, memory that stayed in the test harness).

  We corrected these tests. Now, a failure shows a real problem.

## Checked and left alone

We examined one item that looked incorrect, and we kept it. In the quick start of V.92,
D-Modem ignores one flag that tells which carrier frequency the other modem
uses. The real modems that we tested send on the same carrier, and when D-Modem
obeyed the flag, one of them retrained.
