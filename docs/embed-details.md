# dmodemd as a library: technical details

This page gives the technical details of `libdmodem_embed`. For the
summary, refer to [embed.md](embed.md).

## Files

| File | Contents |
| --- | --- |
| `frontend/embed/dmodem_embed.h` | The C API |
| `frontend/embed/dmodem_embed.cpp` | The thread of the modem, and the loop of the USB, ALSA and mISDN lines |
| `frontend/embed/CMakeLists.txt` | The target `dmodem_embed`, and the test |
| `frontend/embed/test/embed_cat.c` | A test program in C: the DTE on stdin and stdout |
| `frontend/embed/test/embed_test.py` | The test `dmodem_embed` (label `quick`) |

## The thread of the modem

`dmodem_embed_start()` does these steps:

1. It copies the settings. The strings of the program must stay valid only
   during the call.
2. It makes two socket pairs (`dmd::socketpair_cloexec()`): one for the
   DTE, and one to wake the loop of a USB, ALSA or mISDN line.
3. It starts the thread. The thread gets one end of the DTE pair. The
   program gets the other end. That end is non-blocking.

The thread does these steps:

1. It sets `dcore::log_level`, the log hook (`dcore::log_set_hook()`) and
   calls `dcore::log_open()` and `prop_dp_init()`.
2. It makes the modem on the line:
   - SIP: `dmd::SipModem` on `dmd::FdDte`. This is the same as the
     Android app (D-Modem/app, `engine.cpp`).
   - USB: `dmd::UsbLine`. ALSA: `dmd::AlsaLine`. mISDN: `dmd::MisdnLine`.
     The modem is `dcore::Modem` on the line. The loop is the same as
     `run_line()` of `frontend/dmodemd/main.cpp`. It also watches the wake
     socket, and it writes the DTE output that waits (`FdDte::flush()`).
3. It loads the data file (`dmd::DataFile`) before the modem starts, and
   saves it after the modem stops, if `data_file` is set.
4. At the end, it calls `prop_dp_exit()` and `dcore::log_close()`, removes
   the log hook, closes its end of the DTE, and calls the exit callback.

## How the modem stops

| Cause | Status of the exit callback |
| --- | --- |
| `dmodem_embed_stop()`: `SipModem::stop()`, or a byte to the wake socket | 0 |
| The program closes its descriptor: `FdDte::serve()` returns 1 | 0 |
| The line fails to open, or fails during a call | -1 |

`dmodem_embed_stop()` waits until the thread ends. Thus, the callbacks
must not call `dmodem_embed_stop()` or `dmodem_embed_free()`.

## Changes to dmodemd

`dmd::AlsaLine` and `dmd::MisdnLine` now take a `dmd::Dte`, not a
`dmd::Pty`. `dmodemd` gives them its `dmd::PtyDte`. The behaviour of
`dmodemd` does not change.

## Global state

The modem core has one log (`dcore::log_*`) and one set of pump tables
(`prop_dp_init()`) for each process. Thus, `dmodem_embed_start()` refuses
a second modem while one runs in the process.

## Test

`embed_test.py` starts `embed_cat` two times, each on an anonymous SIP
line on 127.0.0.1:

1. Each modem answers `AT` and `ATI3`.
2. Modem A has `ATS0=1` and `-L`. Modem B dials
   `ATX3DTsip:a@127.0.0.1:<port>`. The two modems give `CONNECT`
   (V.91, `CONNECT 62666`).
3. Text from B comes out of A.
4. Program A closes its descriptor. Program B calls `dmodem_embed_stop()`.
   Each modem stops with status 0.
