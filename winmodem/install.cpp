/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    install.cpp - dmodem-driver.exe: the virtual modem's device (a root
 *                  device, Root\DModemVM) made and its driver installed,
 *                  or removed, as the WDK's devcon would; and WinUSB for
 *                  the USB modems.  As an administrator.
 *
 *      dmodem-driver install <dmodem.inf>   a device, its driver from
 *                                           the INF (another device for
 *                                           each install: another port;
 *                                           dmodem7.inf on Windows 7 and
 *                                           8)
 *      dmodem-driver update <dmodem.inf>    the devices there are to the
 *                                           INF's driver
 *      dmodem-driver remove                 every device of it
 *      dmodem-driver list                   each device, its COM port and
 *                                           the pipe dmodemd takes
 *      dmodem-driver usb <dmodem-usb.inf>   WinUSB for the USB modems
 *                                           (SmartUSB56, pico-serial
 *                                           Line), into the driver store
 *                                           and onto those plugged in, so
 *                                           that dmodemd --usb drives them
 *
 */

#include <windows.h>
#include <newdev.h>
#include <setupapi.h>

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

namespace {

/* the device's hardware ID (dmodem.inf's), as a REG_MULTI_SZ */
const char HWID[] = "Root\\DModemVM";
const char HWID_MULTI[] = "Root\\DModemVM\0";

/* a message for the user, with Windows' words for the last error */
void fail(const char *what)
{
	DWORD e = GetLastError();
	char msg[256] = "";
	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		       nullptr, e, 0, msg, sizeof(msg), nullptr);
	size_t n = strlen(msg);
	while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r'))
		msg[--n] = 0;
	fprintf(stderr, "dmodem-driver: %s: error %lu (%s)\n", what,
		(unsigned long)e, msg);
}

/* path made absolute (the installer wants it so) */
std::string full_path(const char *path)
{
	char buf[MAX_PATH];
	DWORD n = GetFullPathNameA(path, sizeof(buf), buf, nullptr);
	return n && n < sizeof(buf) ? std::string(buf, n) : std::string(path);
}

/* the devices with our hardware ID in set, each by f(set, data) */
template <class F> int each_device(F f)
{
	HDEVINFO set = SetupDiGetClassDevsA(nullptr, nullptr, nullptr,
					    DIGCF_ALLCLASSES);
	int found = 0;
	if (set == INVALID_HANDLE_VALUE) {
		fail("the devices can't be listed");
		return -1;
	}
	SP_DEVINFO_DATA d = {};
	d.cbSize = sizeof(d);
	for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); i++) {
		char ids[1024] = "";
		if (!SetupDiGetDeviceRegistryPropertyA(set, &d,
				SPDRP_HARDWAREID, nullptr, (BYTE *)ids,
				sizeof(ids) - 2, nullptr))
			continue;
		for (const char *p = ids; *p; p += strlen(p) + 1) {
			if (!_stricmp(p, HWID)) {
				f(set, &d);
				found++;
				break;
			}
		}
	}
	SetupDiDestroyDeviceInfoList(set);
	return found;
}

/* install: a root device with our hardware ID, then the INF's driver
   for it (and for any there already were) */
int install(const char *inf_arg)
{
	std::string inf = full_path(inf_arg);
	GUID cls;
	char cls_name[256];          /* (MAX_CLASS_NAME_LEN is 32) */
	BOOL reboot = FALSE;

	if (!SetupDiGetINFClassA(inf.c_str(), &cls, cls_name,
				 sizeof(cls_name), nullptr)) {
		fail(inf.c_str());
		return 1;
	}
	HDEVINFO set = SetupDiCreateDeviceInfoList(&cls, nullptr);
	if (set == INVALID_HANDLE_VALUE) {
		fail("no device list");
		return 1;
	}
	SP_DEVINFO_DATA d = {};
	d.cbSize = sizeof(d);
	if (!SetupDiCreateDeviceInfoA(set, cls_name, &cls, nullptr, nullptr,
				      DICD_GENERATE_ID, &d) ||
	    !SetupDiSetDeviceRegistryPropertyA(set, &d, SPDRP_HARDWAREID,
			(const BYTE *)HWID_MULTI, sizeof(HWID_MULTI)) ||
	    !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &d)) {
		fail("the device can't be made");
		SetupDiDestroyDeviceInfoList(set);
		return 1;
	}
	SetupDiDestroyDeviceInfoList(set);
	if (!UpdateDriverForPlugAndPlayDevicesA(nullptr, HWID, inf.c_str(),
						INSTALLFLAG_FORCE, &reboot)) {
		fail("the driver can't be installed (is the package signed?)");
		return 1;
	}
	printf("dmodem-driver: installed%s; dmodem-driver list gives its "
	       "COM port\n", reboot ? " (Windows must restart first)" : "");
	return 0;
}

/* update: the devices there are to the INF's driver */
int update(const char *inf_arg)
{
	std::string inf = full_path(inf_arg);
	BOOL reboot = FALSE;
	if (!UpdateDriverForPlugAndPlayDevicesA(nullptr, HWID, inf.c_str(),
						INSTALLFLAG_FORCE, &reboot)) {
		fail("the driver can't be updated");
		return 1;
	}
	printf("dmodem-driver: updated%s\n",
	       reboot ? " (Windows must restart first)" : "");
	return 0;
}

/* remove: every device of ours (the driver stays in the store) */
int remove_all()
{
	int bad = 0;
	int n = each_device([&bad](HDEVINFO set, SP_DEVINFO_DATA *d) {
		SP_REMOVEDEVICE_PARAMS rp = {};
		rp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
		rp.ClassInstallHeader.InstallFunction = DIF_REMOVE;
		rp.Scope = DI_REMOVEDEVICE_GLOBAL;
		if (!SetupDiSetClassInstallParamsA(set, d,
				&rp.ClassInstallHeader, sizeof(rp)) ||
		    !SetupDiCallClassInstaller(DIF_REMOVE, set, d)) {
			fail("a device can't be removed");
			bad++;
		}
	});
	if (n < 0)
		return 1;
	printf("dmodem-driver: %d device%s removed\n", n - bad,
	       n - bad == 1 ? "" : "s");
	return bad ? 1 : 0;
}

/* list: each device, its COM port (PortName) and its pipe */
int list()
{
	int n = each_device([](HDEVINFO set, SP_DEVINFO_DATA *d) {
		char id[256] = "", port[64] = "";
		SetupDiGetDeviceInstanceIdA(set, d, id, sizeof(id), nullptr);
		HKEY k = SetupDiOpenDevRegKey(set, d, DICS_FLAG_GLOBAL, 0,
					      DIREG_DEV, KEY_QUERY_VALUE);
		if (k != INVALID_HANDLE_VALUE) {
			DWORD type, len = sizeof(port) - 1;
			if (RegQueryValueExA(k, "PortName", nullptr, &type,
					     (BYTE *)port, &len) != ERROR_SUCCESS ||
			    type != REG_SZ)
				port[0] = 0;
			RegCloseKey(k);
		}
		printf("%s: %s, \\\\.\\pipe\\dmodem-vm-%s\n", id,
		       port[0] ? port : "(no COM port)",
		       port[0] ? port : "modem");
	});
	if (n < 0)
		return 1;
	if (n == 0)
		printf("dmodem-driver: no virtual modem installed\n");
	return 0;
}

/* WinUSB streams (isochronous transfers) from Windows 8.1 on: its
   winusb.dll has WinUsb_ReadIsochPipeAsap then */
bool winusb_streams()
{
	HMODULE m = LoadLibraryA("winusb.dll");
	bool yes = m && GetProcAddress(m, "WinUsb_ReadIsochPipeAsap") != nullptr;
	if (m)
		FreeLibrary(m);
	return yes;
}

/* usb: dmodem-usb.inf's package (WinUSB for the USB modems) into the
   driver store, and onto the modems there are now */
int usb(const char *inf_arg)
{
	std::string inf = full_path(inf_arg);
	BOOL reboot = FALSE;
	/* (from newdev.dll itself: MinGW's import library hasn't it) */
	typedef BOOL(WINAPI * DiInstall)(HWND, PCSTR, DWORD, PBOOL);
	HMODULE nd = LoadLibraryA("newdev.dll");
	DiInstall install = nd ? (DiInstall)(void *)GetProcAddress(nd,
		"DiInstallDriverA") : nullptr;

	if (!install) {
		fail("newdev.dll has no DiInstallDriverA");
		return 1;
	}
	if (!install(nullptr, inf.c_str(), DIIRFLAG_FORCE_INF, &reboot)) {
		fail("WinUSB can't be installed for them (is the package "
		     "signed?)");
		return 1;
	}
	printf("dmodem-driver: WinUSB for the USB modems%s; dmodemd --usb "
	       "list shows them\n",
	       reboot ? " (Windows must restart first)" : "");
	if (!winusb_streams())
		printf("dmodem-driver: this Windows' WinUSB can't stream "
		       "(Windows 8.1 and later can): give the modems libusbK "
		       "with Zadig instead, or install UsbDk\n");
	return 0;
}

/* how to use it */
int usage()
{
	fprintf(stderr,
		"usage: dmodem-driver install <dmodem.inf>\n"
		"       dmodem-driver update <dmodem.inf>\n"
		"       dmodem-driver remove\n"
		"       dmodem-driver list\n"
		"       dmodem-driver usb <dmodem-usb.inf>\n");
	return 2;
}

} // namespace

/* see the top */
int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "install"))
		return install(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "update"))
		return update(argv[2]);
	if (argc == 2 && !strcmp(argv[1], "remove"))
		return remove_all();
	if (argc == 2 && !strcmp(argv[1], "list"))
		return list();
	if (argc == 3 && !strcmp(argv[1], "usb"))
		return usb(argv[2]);
	return usage();
}
