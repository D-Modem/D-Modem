#!/usr/bin/env python3
#
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
#
#    make_inf.py - dmodem.inf, the virtual modem driver's INF: the UMDF
#                  driver's install and Unimodem's settings for D-Modem,
#                  its result codes in Unimodem's Responses with each
#                  CONNECT rate the core has (core/modem.cpp's
#                  connect_codes)
#
#    usage: make_inf.py SOURCE_DIR           write winmodem/dmodem.inf
#           make_inf.py SOURCE_DIR --check   exit 1 if it is not what
#                                            this would write
#
#    A Responses value is Unimodem's MODEMRESPONSE: the state (0 OK,
#    1 negotiation progress, 2 CONNECT, 3 ERROR, 4 NO CARRIER, 5 NO
#    DIALTONE, 6 BUSY, 7 NO ANSWER, 8 RING), the negotiated options (1
#    compression, 2 error control), the DCE rate and the DTE rate (each 4
#    bytes, little-endian; 0 for none).
#

import os
import re
import sys

HEAD = r"""; SPDX-License-Identifier: AGPL-3.0-only
; Copyright (C) 2026 Neo-Desktop
;
; dmodem.inf - D-Modem's virtual modem: a modem without hardware whose
; COM port (dmodemvm.dll, a UMDF 2 driver under Unimodem's modem.sys) is
; served by dmodemd on its pipe (dmodemd --driver auto).
;
; Made by winmodem/make_inf.py from the core's result codes: change that,
; not this.  Install with dmodem-driver.exe install dmodem.inf (a root
; device, Root\DModemVM), on Windows 10 1803 or later (the driver store's
; DIRID 13); the package must be signed (docs/windows.md).

[Version]
Signature   = "$WINDOWS NT$"
Class       = Modem
ClassGuid   = {4D36E96D-E325-11CE-BFC1-08002BE10318}
Provider    = %Provider%
CatalogFile = dmodemvm.cat
DriverVer   = 10/22/2026,1.0.0.0
PnpLockdown = 1

[DestinationDirs]
DefaultDestDir = 13

[SourceDisksNames]
1 = %Disk%

[SourceDisksFiles]
dmodemvm.dll = 1

[Manufacturer]
%Mfg% = DModem, NTamd64.10.0...17134

[DModem.NTamd64.10.0...17134]
%ModemName% = DModem_Install, Root\DModemVM

; ---- the driver: WUDFRd (UMDF's reflector) below modem.sys, our DLL in
; its host; modem.sys, a kernel driver, its client

[DModem_Install.NT]
CopyFiles = DModem_Files
AddReg    = DModem_All, DModem_Settings, DModem_Responses

[DModem_Files]
dmodemvm.dll

[DModem_Install.NT.HW]
AddReg = DModem_HW

[DModem_HW]
HKR,,"LowerFilters",0x00010000,"WUDFRd"

[DModem_Install.NT.Services]
AddService = WUDFRd,0x00000000,WUDFRd_Service

[WUDFRd_Service]
DisplayName   = %WudfRd%
ServiceType   = 1
StartType     = 3
ErrorControl  = 1
ServiceBinary = %12%\WUDFRd.sys

[DModem_Install.NT.Wdf]
UmdfService                = dmodemvm, dmodemvm_Install
UmdfServiceOrder           = dmodemvm
UmdfKernelModeClientPolicy = AllowKernelModeClients
UmdfFsContextUsePolicy     = CannotUseFsContexts

[dmodemvm_Install]
UmdfLibraryVersion = 2.15.0
ServiceBinary      = %13%\dmodemvm.dll

; ---- Unimodem's settings for D-Modem's commands (docs/at)
;
; Properties: dial options (none), call setup timer (S7, up to 255 s),
; inactivity timer (none), speaker volume and modes (none: a SIP line
; has no speaker), modem options (compression, error control, forced
; error control, hardware flow control, tone and blind dialling), the
; highest DTE rate (115200) and DCE rate (56000).

[DModem_All]
HKR,,ConfigDialog,,modemui.dll
HKR,,EnumPropPages,,"modemui.dll,EnumPropPages"
HKR,,PortSubClass,1,02
HKR,,DeviceType,1,02
HKR,,InactivityScale,1,0a,00,00,00
HKR,,Properties,1,00,00,00,00, ff,00,00,00, 00,00,00,00, 00,00,00,00, 00,00,00,00, 17,03,00,00, 00,c2,01,00, c0,da,00,00
HKR,,DCB,1,1c,00,00,00, 00,c2,01,00, 15,20,00,00, 00,00, 0a,00, 0a,00, 08, 00, 00, 11, 13, 00, 00, 00
HKR,,Reset,,"ATZ<cr>"
HKR,Init,1,,"AT<cr>"
HKR,Init,2,,"AT&FE0V1&C1&D2S0=0<cr>"
HKR,Monitor,1,,"ATS0=0<cr>"
HKR,Monitor,2,,"None"
HKR,Hangup,1,,"ATH<cr>"
HKR,Answer,1,,"ATA<cr>"

[DModem_Settings]
HKR,Settings,Prefix,,"AT"
HKR,Settings,Terminator,,"<cr>"
HKR,Settings,DialPrefix,,"D"
HKR,Settings,DialSuffix,,";"
HKR,Settings,Tone,,"T"
HKR,Settings,Pulse,,"P"
HKR,Settings,Blind_Off,,"X4"
HKR,Settings,Blind_On,,"X3"
HKR,Settings,CallSetupFailTimer,,"S7=<#>"
HKR,Settings,FlowControl_Off,,"&H0"
HKR,Settings,FlowControl_Hard,,"&H1"
HKR,Settings,FlowControl_Soft,,"&H2"
HKR,Settings,ErrorControl_On,,"\N3"
HKR,Settings,ErrorControl_Off,,"\N0"
HKR,Settings,ErrorControl_Forced,,"\N4"
HKR,Settings,Compression_On,,"%%C3"
HKR,Settings,Compression_Off,,"%%C0"

[DModem_Responses]
"""

TAIL = r"""
[Strings]
Provider  = "D-Modem"
Mfg       = "D-Modem"
ModemName = "D-Modem Virtual Modem"
Disk      = "D-Modem virtual modem driver"
WudfRd    = "Windows Driver Foundation - User-mode Driver Framework Reflector"
"""

# the results with no rate: verbose and numeric (ATV0), as the core
# gives them (core/modem.cpp: result_text, and 88 and 89 for FAX and
# DATA)
RESULTS = [
    ("OK", "0", 0), ("CONNECT", "1", 2), ("RING", "2", 8),
    ("NO CARRIER", "3", 4), ("ERROR", "4", 3), ("NO DIALTONE", "6", 5),
    ("BUSY", "7", 6), ("NO ANSWER", "8", 7), ("FAX", "88", 3),
    ("DATA", "89", 3),
]


def connect_codes(src):
    """(rate, code) of core/modem.cpp's connect_codes, in its order"""
    text = open(os.path.join(src, "core", "modem.cpp")).read()
    m = re.search(r"connect_codes\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        sys.exit("make_inf.py: no connect_codes in core/modem.cpp")
    return [(int(r), int(c))
            for r, c in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*\}", m.group(1))]


def value(state, dce=0):
    """a MODEMRESPONSE as the INF's bytes"""
    b = [state, 0] + list(dce.to_bytes(4, "little")) + [0, 0, 0, 0]
    return "%02x,%02x, %s, %s" % (
        b[0], b[1], ",".join("%02x" % x for x in b[2:6]),
        ",".join("%02x" % x for x in b[6:10]))


def inf(src):
    """the INF's text"""
    out = [HEAD]
    line = "HKR,Responses,\"%s\",1,%s\n"
    for words, number, state in RESULTS:
        out.append(line % ("<cr><lf>%s<cr><lf>" % words, value(state)))
        out.append(line % ("%s<cr>" % number, value(state)))
    for rate, code in sorted(connect_codes(src)):
        out.append(line % ("<cr><lf>CONNECT %d<cr><lf>" % rate,
                           value(2, rate)))
        out.append(line % ("%d<cr>" % code, value(2, rate)))
    out.append(TAIL)
    return "".join(out).replace("\n", "\r\n")


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: make_inf.py SOURCE_DIR [--check]")
    src = sys.argv[1]
    path = os.path.join(src, "winmodem", "dmodem.inf")
    text = inf(src)
    if "--check" in sys.argv[2:]:
        try:
            have = open(path, newline="").read()
        except OSError:
            have = None
        if have != text:
            print("%s is not what make_inf.py makes: run it" % path)
            return 1
        print("dmodem.inf: %d responses" % text.count("HKR,Responses"))
        return 0
    with open(path, "w", newline="") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
