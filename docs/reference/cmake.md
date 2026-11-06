<!-- Made by tools/site_reference.py from the source: change the code (or the script), not this page. -->

# CMake options

What `cmake -B build -D<NAME>=<value>` takes, from the `option()`s and cache settings in the build files. [Building](../building.md) says what each system needs.

## Switches

| option | default | what it does | set in |
|---|---|---|---|
| `DMODEM_PREFER_CLANG` | ON | Clang where it is installed, not GCC | `CMakeLists.txt` |
| `DMODEM_DMODEMD` | ON | Build dmodemd (SIP, needs libre) | `CMakeLists.txt` |
| `DMODEM_ASTERISK` | ON, but OFF on Windows | Build codec_clearmode when Asterisk's headers are found | `CMakeLists.txt` |
| `DMODEM_APP_DMODEM` | OFF | Also build app_dmodem (local use only, not to be distributed: LICENSING.md) | `CMakeLists.txt` |
| `DMODEM_FREESWITCH` | ON, but OFF on Windows | Build mod_dmodem when FreeSWITCH is found | `CMakeLists.txt` |
| `DMODEM_ARI` | ON, but OFF on Windows | Build dmodem-ari (D-Modem as an Asterisk ARI application) | `CMakeLists.txt` |
| `DMODEM_TESTS` | ON | Build the test programs (ctest) | `CMakeLists.txt` |
| `DMODEM_USB` | ON | dmodemd's --usb on Windows and macOS (libusb, fetched) | `CMakeLists.txt` |
| `DMODEM_WERROR` | OFF | Treat compiler warnings as errors (CI) | `CMakeLists.txt` |
| `DMODEM_TEST_BUILTIN_FAKES` | OFF | Linux: also test dmodemd with the fake USB modems linked in | `frontend/dmodemd/CMakeLists.txt` |

## Settings

| setting | default | type | what it is | set in |
|---|---|---|---|---|
| `DMODEM_LIBRE` | `auto` | string | libre: auto, system or fetch | `CMakeLists.txt` |
| `DMODEM_CRYPTO` | `mbedtls` | string | libre's crypto: mbedtls or openssl | `CMakeLists.txt` |
| `DMODEM_WINNT` | `0x0601` | string | _WIN32_WINNT: the oldest Windows to run on (0x0601: Windows 7) | `CMakeLists.txt` |
| `DMODEM_LIBRE_REPO` | `https://github.com/baresip/re.git` | string | libre repository to fetch | `cmake/libre.cmake` |
| `DMODEM_LIBRE_TAG` | `v4.12.0` | string | libre release to fetch | `cmake/libre.cmake` |
| `DMODEM_MBEDTLS_REPO` | `https://github.com/Mbed-TLS/mbedtls.git` | string | Mbed TLS repository to fetch | `cmake/libre.cmake` |
| `DMODEM_MBEDTLS_TAG` | `v3.6.7` | string | Mbed TLS release to fetch | `cmake/libre.cmake` |
| `DMODEM_LIBUSB_VERSION` | `1.0.27` | string | libusb release to fetch | `cmake/libusb.cmake` |
| `DMODEM_LIBUSB_URL` | `https://github.com/libusb/libusb/releases/download/v${DMODEM_LIBUSB_VERSION}/libusb-${DMODEM_LIBUSB_VERSION}.tar.bz2` | string | libusb release tarball | `cmake/libusb.cmake` |
| `DMODEM_LIBUSB_SHA256` | `ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575` | string | the tarball's SHA-256 | `cmake/libusb.cmake` |
| `DMODEM_LIBUSB_DIR` | `` | path | a libusb source tree (else fetched) | `cmake/libusb.cmake` |
| `DMODEM_TEST_LOG_ROOT` | `(depends on the system)` | path | where each ctest run's test logs go | `cmake/test_logs.cmake` |
| `DMODEM_TEST_LOG_KEEP` | `20` | string | how many ctest runs' test logs are kept | `cmake/test_logs.cmake` |
| `DMODEM_AGERE_LIB` | `(depends on the system)` | filepath | agrsm_core.o of the Agere soft modem (interop tests) | `dsp/test/CMakeLists.txt` |
| `DMODEM_SM56_KO` | `(depends on the system)` | filepath | sm56.ko of the Motorola SM56 soft modem (interop tests) | `dsp/test/CMakeLists.txt` |
| `DMODEM_PCTEL_DIR` | `(depends on the system)` | path | dsp.o and control.o of the PCTel HSP soft modem (interop tests) | `dsp/test/CMakeLists.txt` |
| `DMODEM_HSF_DIR` | `(depends on the system)` | path | objects and NVM files of the HSF soft modem (interop tests) | `dsp/test/CMakeLists.txt` |
| `DMODEM_BROADCOM_DIR` | `(depends on the system)` | path | BCMSM.sys and BCMSMwxp.inf of the Broadcom soft modem (interop tests) | `dsp/test/CMakeLists.txt` |
| `DMODEM_ESS_DIR` | `(depends on the system)` | path | es56hpi.sys and mdmzn.inf of the ESS soft modem (interop tests) | `dsp/test/CMakeLists.txt` |
| `DMODEM_USR_DIR` | `(depends on the system)` | path | IDSDL302.ROM, c51-mask.rom and courier-nvram.bin of the Courier (interop tests) | `dsp/test/CMakeLists.txt` |
| `DMODEM_INTEL_DIR` | `(depends on the system)` | path | 537core.lib and wwh_dflt.c of the Intel 537EP soft modem (interop tests) | `dsp/test/CMakeLists.txt` |
| `DMODEM_HOST_LINE_EXCHANGE` | `(depends on the system)` | filepath | a Linux build's line_exchange (Windows USB tests) | `frontend/dmodemd/CMakeLists.txt` |
| `DMODEM_UMDF1_INCLUDE` | `(depends on the system)` | path | UMDF 1.9's headers (wudfddi.h's directory) | `platforms/windows/winmodem/CMakeLists.txt` |
| `DMODEM_WDF_INCLUDE` | `(depends on the system)` | path | UMDF 2.15's headers (wdf.h's directory) | `platforms/windows/winmodem/CMakeLists.txt` |
| `DMODEM_WDF_STUB` | `(depends on the system)` | filepath | the WDK's WdfDriverStubUm.lib (UMDF 2.15, x64) | `platforms/windows/winmodem/CMakeLists.txt` |
