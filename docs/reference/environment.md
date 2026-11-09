<!-- Made by tools/site_reference.py from the source: change the code (or the script), not this page. -->

# Environment variables

The variables D-Modem's programs read, and those its PBX modules set for the programs they run, as the source has them. Each links the pages that describe it.

| variable | | described in | in the code |
|---|---|---|---|
| `DMODEM_AGERE_DUMP` | read | [The Agere USB 2.0 soft modem: technical details](../agere-usb-details.md), [The Agere USB 2.0 soft modem](../agere-usb.md), [Softmodem hardware: dmodemd on ALSA and USB: technical details](../hardware-details.md) | `frontend/dmodemd/usb_agere.cpp` |
| `DMODEM_AGERE_LED_PROBE` | read | [The Agere USB 2.0 soft modem: technical details](../agere-usb-details.md), [Softmodem hardware: dmodemd on ALSA and USB: technical details](../hardware-details.md) | `frontend/dmodemd/usb_agere.cpp` |
| `DMODEM_AGERE_ROM` | read | [The Agere USB 2.0 soft modem: technical details](../agere-usb-details.md) | `frontend/dmodemd/usb_agere.cpp` |
| `DMODEM_AGERE_TRANSFERS` | read | [The Agere USB 2.0 soft modem: technical details](../agere-usb-details.md), [Softmodem hardware: dmodemd on ALSA and USB: technical details](../hardware-details.md) | `frontend/dmodemd/usb_agere.cpp` |
| `DMODEM_CALLERID` | set for a program | [dmodem-ari: D-Modem as an Asterisk ARI application: technical details](../ari-details.md), [app_dmodem: a data modem on an Asterisk channel: technical details](../asterisk-details.md), [mod_dmodem: a data modem on a FreeSWITCH channel: technical details](../freeswitch-details.md) | `frontend/ari/dmodem_ari.cpp`, `frontend/asterisk/app_dmodem.c`, `frontend/freeswitch/mod_dmodem.c` ... |
| `DMODEM_CHANNEL` | set for a program | [dmodem-ari: D-Modem as an Asterisk ARI application: technical details](../ari-details.md), [app_dmodem: a data modem on an Asterisk channel: technical details](../asterisk-details.md), [mod_dmodem: a data modem on a FreeSWITCH channel: technical details](../freeswitch-details.md) | `frontend/ari/dmodem_ari.cpp`, `frontend/asterisk/app_dmodem.c`, `frontend/freeswitch/mod_dmodem.c` |
| `DMODEM_CONNECTED` | set for a program | [dmodem-ari: D-Modem as an Asterisk ARI application: technical details](../ari-details.md), [app_dmodem: a data modem on an Asterisk channel: technical details](../asterisk-details.md), [mod_dmodem: a data modem on a FreeSWITCH channel: technical details](../freeswitch-details.md) | `frontend/ari/dmodem_ari.cpp`, `frontend/asterisk/app_dmodem.c`, `frontend/freeswitch/mod_dmodem.c` |
| `DMODEM_DEBUG` | read | [app_dmodem: a data modem on an Asterisk channel: technical details](../asterisk-details.md), [mod_dmodem: a data modem on a FreeSWITCH channel: technical details](../freeswitch-details.md), [Building and testing the DSP: technical details](../testing-details.md) | `dsp/test/dmodem_fax_test.cpp`, `dsp/test/dmodem_test.cpp`, `dsp/test/pos_test.cpp` ... |
| `DMODEM_DIALED` | set for a program | [dmodem-ari: D-Modem as an Asterisk ARI application: technical details](../ari-details.md), [app_dmodem: a data modem on an Asterisk channel: technical details](../asterisk-details.md), [mod_dmodem: a data modem on a FreeSWITCH channel: technical details](../freeswitch-details.md) | `frontend/ari/dmodem_ari.cpp`, `frontend/asterisk/app_dmodem.c`, `frontend/freeswitch/mod_dmodem.c` |
| `DMODEM_G729_VECTORS` | read | tests only | `link/test/g729a_bcg729_test.cpp`, `link/test/g729a_test.cpp` |
| `DMODEM_PROFILE` | set for a program | [dmodem-ari: D-Modem as an Asterisk ARI application: technical details](../ari-details.md), [app_dmodem: a data modem on an Asterisk channel: technical details](../asterisk-details.md), [mod_dmodem: a data modem on a FreeSWITCH channel: technical details](../freeswitch-details.md) | `frontend/ari/dmodem_ari.cpp`, `frontend/asterisk/app_dmodem.c`, `frontend/freeswitch/mod_dmodem.c` |
| `DMODEM_RATE` | set for a program | [dmodem-ari: D-Modem as an Asterisk ARI application: technical details](../ari-details.md), [app_dmodem: a data modem on an Asterisk channel: technical details](../asterisk-details.md), [mod_dmodem: a data modem on a FreeSWITCH channel: technical details](../freeswitch-details.md) | `frontend/ari/dmodem_ari.cpp`, `frontend/asterisk/app_dmodem.c`, `frontend/freeswitch/mod_dmodem.c` ... |
| `DMODEM_REC` | read | [Using dmodemd](../usage.md) | `frontend/dmodemd/sip.cpp` |
| `DMODEM_TEST_DIR` | read | [Building and testing the DSP: technical details](../testing-details.md) | `dsp/test/interop_test.cpp` |
| `DMODEM_USBDK` | read | [D-Modem on Windows: technical details](../windows-details.md) | `frontend/dmodemd/usb_line.cpp` |
| `DMODEM_USB_REC` | read | [The Agere USB 2.0 soft modem: technical details](../agere-usb-details.md), [The Agere USB 2.0 soft modem](../agere-usb.md), [The Apple USB Modem](../apple-usb-modem.md), [Softmodem hardware: dmodemd on ALSA and USB: technical details](../hardware-details.md) | `frontend/dmodemd/usb_line.cpp` |
| `DMODEM_USB_TX_GAIN` | read | [The Agere USB 2.0 soft modem: technical details](../agere-usb-details.md), [The Agere USB 2.0 soft modem](../agere-usb.md), [Softmodem hardware: dmodemd on ALSA and USB: technical details](../hardware-details.md) | `frontend/dmodemd/usb_line.cpp` |
| `DMODEM_UUID` | set for a program | [mod_dmodem: a data modem on a FreeSWITCH channel: technical details](../freeswitch-details.md) | `frontend/freeswitch/mod_dmodem.c` |
| `SIP_CLEARMODE` | read | [ISDN: the clear 64 kbit/s channel: technical details](../isdn-details.md), [ISDN: the clear 64 kbit/s channel](../isdn.md) | `frontend/dmodemd/sip.cpp` |
| `SIP_JITTER` | read | [Using dmodemd](../usage.md) | `frontend/dmodemd/sip.cpp` |
| `SIP_LOGIN` | read | [Building](../building.md), [Softmodem hardware: dmodemd on ALSA and USB: technical details](../hardware-details.md), [Code review (October 2026): technical details](../review-details.md), [Using dmodemd](../usage.md), [D-Modem on Windows: technical details](../windows-details.md), [D-Modem on Windows](../windows.md) | `frontend/dmodemd/sip.cpp` |
| `SIP_LOG_LEVEL` | read | [Using dmodemd](../usage.md) | `frontend/dmodemd/sip.cpp` |
| `SIP_PORT` | read | [Using dmodemd](../usage.md) | `frontend/dmodemd/sip.cpp` |
| `SIP_RED` | read | [Using dmodemd](../usage.md) | `frontend/dmodemd/sip.cpp` |
| `SIP_SRTP` | read | [ISDN: the clear 64 kbit/s channel: technical details](../isdn-details.md), [Using dmodemd](../usage.md) | `frontend/dmodemd/sip.cpp` |
| `SIP_T38` | read | [Using dmodemd](../usage.md) | `frontend/dmodemd/sip.cpp` |
| `SIP_V150` | read | [Building and testing the DSP: technical details](../testing-details.md), [Using dmodemd](../usage.md), [Modem relay (V.150.1): technical details](../v150-details.md), [Modem relay (V.150.1)](../v150.md) | `frontend/dmodemd/sip.cpp` |
| `SIP_V152` | read | [Using dmodemd](../usage.md) | `frontend/dmodemd/sip.cpp` |
