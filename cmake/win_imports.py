#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
#
# win_imports.py - checks that Windows programs and DLLs (PE files) load
# on the oldest Windows D-Modem supports (Windows 7, NT 6.1):
#
#   python3 cmake/win_imports.py [--list] <file.exe|file.dll>...
#
# - the subsystem version in the PE header (Windows refuses a program
#   whose version is newer than its own);
# - the imported DLLs: none of Windows 8's and later, and no Universal
#   CRT or Visual C++ runtime DLLs (the builds link the C runtime
#   statically: Windows 7 has the Universal CRT only with an update);
# - the imported functions, the delay-loaded ones too: none of those
#   below, which Windows 8 and later brought.  The list can't be all of
#   them: it holds the ones runtimes and libraries are known to reach for.
#   A new import is checked against Microsoft's documentation ("Minimum
#   supported client") when it appears: --list prints them all.
#
# Exit status 1 if a file fails, with what failed.
import struct
import sys

WINNT = (6, 1)

# DLLs (lower case) Windows 7 lacks, or has only with an update
BAD_DLL_PREFIXES = ("api-ms-win-", "ext-ms-", "ucrtbase", "vcruntime",
                    "msvcp1", "concrt1", "vccorlib")

# functions of Windows 8 and later (kernel32/kernelbase, ws2_32, advapi32,
# user32, setupapi, cfgmgr32, winusb)
NEWER = set("""
GetSystemTimePreciseAsFileTime CreateFile2 CopyFile2 GetOverlappedResultEx
WaitOnAddress WakeByAddressSingle WakeByAddressAll
InitializeSynchronizationBarrier EnterSynchronizationBarrier
DeleteSynchronizationBarrier GetCurrentThreadStackLimits
PrefetchVirtualMemory SetThreadInformation GetThreadInformation
SetProcessInformation GetProcessInformation SetThreadDescription
GetThreadDescription GetProcessMitigationPolicy SetProcessMitigationPolicy
GetFirmwareType GetFirmwareEnvironmentVariableExA
GetFirmwareEnvironmentVariableExW SetFirmwareEnvironmentVariableExA
SetFirmwareEnvironmentVariableExW CreateFileMappingFromApp
MapViewOfFileFromApp VirtualAlloc2 VirtualAlloc2FromApp MapViewOfFile3
MapViewOfFile3FromApp CreateFileMapping2 UnmapViewOfFile2
GetSystemCpuSetInformation SetThreadSelectedCpuSets
GetThreadSelectedCpuSets SetProcessDefaultCpuSets GetProcessDefaultCpuSets
GetPackageFamilyName GetPackageFullName GetCurrentPackageId
GetCurrentPackageFullName GetCurrentPackageFamilyName GetCurrentPackagePath
GetCurrentPackageInfo AppPolicyGetProcessTerminationMethod
AppPolicyGetThreadInitializationType AppPolicyGetWindowingModel
AppPolicyGetShowDeveloperDiagnostic QueryUnbiasedInterruptTimePrecise
QueryInterruptTime QueryInterruptTimePrecise GetOsSafeBootMode
IsWow64Process2 GetMachineTypeAttributes GetTempPath2A GetTempPath2W
CompareObjectHandles DiscardVirtualMemory OfferVirtualMemory
ReclaimVirtualMemory LoadPackagedLibrary GetSystemTimes2
SetDefaultDllDirectories AddDllDirectory RemoveDllDirectory
GetIntegratedDisplaySize CreateThreadpoolIo2 SetCoalescableTimer
GetFileInformationByName CreatePseudoConsole ResizePseudoConsole
ClosePseudoConsole GetThreadEnabledXStateFeatures
GetAddrInfoExCancel GetAddrInfoExOverlappedResult ProcessSocketNotifications
WinUsb_ReadIsochPipe WinUsb_ReadIsochPipeAsap WinUsb_WriteIsochPipe
WinUsb_WriteIsochPipeAsap WinUsb_RegisterIsochBuffer
WinUsb_UnregisterIsochBuffer WinUsb_GetCurrentFrameNumber
WinUsb_GetAdjustedFrameNumber WinUsb_StartTrackingForTimeSync
WinUsb_StopTrackingForTimeSync WinUsb_GetCurrentFrameNumberAndQpc
CM_Register_Notification CM_Unregister_Notification
SetupDiGetDevicePropertyKeys
GetDpiForWindow GetDpiForSystem SetThreadDpiAwarenessContext
GetSystemMetricsForDpi AdjustWindowRectExForDpi
""".split())
# (SetDefaultDllDirectories and its kin: Windows 7 only with KB2533623)


def u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


class Pe:
    """a PE file's headers, sections and imports"""
    def __init__(self, data):
        self.d = d = data
        if d[:2] != b"MZ":
            raise ValueError("not a PE file (no MZ)")
        pe = u32(d, 0x3C)
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file (no PE header)")
        coff = pe + 4
        nsect = u16(d, coff + 2)
        optsize = u16(d, coff + 16)
        opt = coff + 20
        magic = u16(d, opt)
        if magic not in (0x10B, 0x20B):
            raise ValueError("unknown optional header 0x%x" % magic)
        self.pe32plus = magic == 0x20B
        self.subsys = (u16(d, opt + 48), u16(d, opt + 50))
        self.osver = (u16(d, opt + 40), u16(d, opt + 42))
        ddir = opt + (112 if self.pe32plus else 96)
        ndir = u32(d, ddir - 4)
        self.dirs = [(u32(d, ddir + 8 * i), u32(d, ddir + 8 * i + 4))
                     for i in range(ndir)]
        sect = opt + optsize
        self.sections = []
        for i in range(nsect):
            s = sect + 40 * i
            vsize, va, rsize, roff = struct.unpack_from("<IIII", d, s + 8)
            self.sections.append((va, max(vsize, rsize), roff, rsize))

    def off(self, rva):
        """the file offset of rva"""
        for va, size, roff, rsize in self.sections:
            if va <= rva < va + size:
                if rva - va >= rsize:
                    raise ValueError("rva 0x%x not in the file" % rva)
                return roff + rva - va
        raise ValueError("rva 0x%x in no section" % rva)

    def cstr(self, rva):
        o = self.off(rva)
        return self.d[o:self.d.index(b"\0", o)].decode("latin-1")

    def thunks(self, rva):
        """the names (or #ordinals) a thunk table at rva imports"""
        size = 8 if self.pe32plus else 4
        flag = 1 << (63 if self.pe32plus else 31)
        o = self.off(rva)
        out = []
        while True:
            v = struct.unpack_from("<Q" if size == 8 else "<I", self.d, o)[0]
            if v == 0:
                return out
            out.append("#%d" % (v & 0xFFFF) if v & flag else
                       self.cstr((v & 0x7FFFFFFF) + 2))
            o += size

    def imports(self):
        """[(dll, function, delay-loaded)]"""
        out = []
        if len(self.dirs) > 1 and self.dirs[1][0]:
            o = self.off(self.dirs[1][0])
            while True:
                oft, _, _, name, ft = struct.unpack_from("<IIIII", self.d, o)
                if name == 0:
                    break
                dll = self.cstr(name)
                for f in self.thunks(oft or ft):
                    out.append((dll, f, False))
                o += 20
        if len(self.dirs) > 13 and self.dirs[13][0]:
            o = self.off(self.dirs[13][0])
            while True:
                attrs, name, _, _, int_rva = struct.unpack_from("<IIIII",
                                                                self.d, o)
                if name == 0:
                    break
                # (version 1 descriptors hold RVAs, version 0 addresses:
                # MSVC's linker makes version 1 alone nowadays)
                if attrs & 1:
                    dll = self.cstr(name)
                    for f in self.thunks(int_rva):
                        out.append((dll, f, True))
                o += 32
        return out


def check(path, listing):
    """the failures of the file at path (a list of strings)"""
    with open(path, "rb") as f:
        pe = Pe(f.read())
    bad = []
    if pe.subsys > WINNT:
        bad.append("subsystem version %d.%d: Windows 7 (%d.%d) refuses it" %
                   (pe.subsys + WINNT))
    for dll, fn, delay in pe.imports():
        if listing:
            print("%s: %s!%s%s" % (path, dll, fn,
                                   " (delay-loaded)" if delay else ""))
        if dll.lower().startswith(BAD_DLL_PREFIXES):
            bad.append("%s!%s: Windows 7 has no %s" % (dll, fn, dll))
        elif fn in NEWER:
            bad.append("%s!%s%s: Windows 8 or later" %
                       (dll, fn, " (delay-loaded)" if delay else ""))
    return sorted(set(bad))


def main(argv):
    listing = "--list" in argv
    files = [a for a in argv if a != "--list"]
    if not files:
        print(__doc__.strip().split("\n\n")[0], file=sys.stderr)
        return 2
    failed = 0
    for path in files:
        try:
            bad = check(path, listing)
        except (OSError, ValueError) as e:
            print("%s: %s" % (path, e))
            failed += 1
            continue
        if bad:
            failed += 1
            for b in bad:
                print("%s: %s" % (path, b))
        else:
            print("%s: loads on Windows 7" % path)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
