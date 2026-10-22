/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    driver.cpp - dmodemvm.dll, D-Modem's virtual modem driver: a UMDF 2
 *                 driver whose device is a modem (dmodem.inf: the Modem
 *                 class, Unimodem's modem.sys above it) without hardware.
 *                 Its COM port (port.h) is what modem.sys, TAPI's apps
 *                 (dial-up networking, fax) and programs that open the
 *                 COM port by name use; dmodemd (--driver) is the modem
 *                 on the port's other side, through the pipe the driver
 *                 makes (link.h).
 *
 *    The device:
 *      - its COM port's name (PortName, which the modem class installer
 *        gives it) as \DosDevices\Global\<name> and in
 *        HARDWARE\DEVICEMAP\SERIALCOMM, for programs that list COM ports;
 *      - GUID_DEVINTERFACE_MODEM's interface;
 *      - one handle at a time (a serial port's rule; a second is
 *        refused): its reads, writes and serial IOCTLs to the port, the
 *        ones that wait cancelable;
 *      - the pipe \\.\pipe\dmodem-vm-<name>, until the device goes.
 *
 *    Only this file is the WDF's; the port and the pipe are tested
 *    without it (port_test, vmodem_test).
 *
 */

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <wdf.h>

#include <stdarg.h>
#include <stdio.h>

#include <atomic>
#include <mutex>
#include <string>

#include "link.h"
#include "port.h"

namespace {

/* GUID_DEVINTERFACE_MODEM (ntddmodm.h) */
const GUID MODEM_INTERFACE = { 0x2c7089aa, 0x2e0e, 0x11d1,
			       { 0xb1, 0x14, 0x00, 0xc0, 0x4f, 0xc2, 0xaa, 0xe4 } };

const wchar_t LINK_PREFIX[] = L"\\DosDevices\\Global\\";
const wchar_t SERIALCOMM[] = L"SERIALCOMM";
const wchar_t PORT_NAME[] = L"PortName";
/* a port without a name of its own (no PortName): its pipe's */
const char NO_NAME[] = "modem";

/* a message for the debugger (DebugView) */
void dbg(const char *fmt, ...)
{
	char buf[300];
	va_list ap;
	int n = snprintf(buf, sizeof(buf), "dmodemvm: ");
	va_start(ap, fmt);
	vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
	va_end(ap);
	strcat(buf, "\n");
	OutputDebugStringA(buf);
}

/* (the link's messages) */
void link_log(const char *msg)
{
	dbg("%s", msg);
}

/* a UNICODE_STRING on s (not copied) */
UNICODE_STRING ustr(const wchar_t *s)
{
	UNICODE_STRING u;
	u.Buffer = (PWCH)s;
	u.Length = (USHORT)(wcslen(s) * sizeof(wchar_t));
	u.MaximumLength = u.Length + sizeof(wchar_t);
	return u;
}

/* the device's own: the pipe with the port, the names it made */
struct DeviceCtx {
	vmodem::Link *link;
	std::atomic<int> *opens;        /* the port's handles (one at most) */
	wchar_t port[64];               /* PortName, "" if none */
	wchar_t pdo[128];               /* SERIALCOMM's value name, "" if
					   none written */
};
/* (device_ctx(dev): a device's) */
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceCtx, device_ctx)

class WdfReq;

/* every request's: the port's hold on it while it waits */
struct ReqCtx {
	WdfReq *held;
};
/* (req_ctx(req): a request's) */
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(ReqCtx, req_ctx)

/* a read, a write or an IOCTL_SERIAL_WAIT_ON_MASK the port holds,
   cancelable while it does.  Two references: the port's (until it
   completes it, or gives it up to a cancel) and the request's (until
   the request is completed, by complete() or by the cancel); the cancel
   and complete() meet under mu_, so that a request is completed once
   and never unmarked once it is */
class WdfReq final : public vmodem::Request {
public:
	/* (buf, len: the request's buffer, from the framework) */
	WdfReq(WDFREQUEST req, vmodem::Port &port, void *buf, size_t len)
		: req_(req), port_(port), buf_((uint8_t *)buf), len_(len) {}

	uint8_t *data() override { return buf_; }
	size_t size() const override { return len_; }

	/* see port.h: the request completed unless its cancel has it */
	void complete(uint32_t status, size_t info) override
	{
		bool mine = false;
		{
			std::lock_guard<std::mutex> lk(mu_);
			if (!cancelled_ && WdfRequestUnmarkCancelable(req_) !=
			    STATUS_CANCELLED)
				mine = true;
		}
		if (mine) {
			req_ctx(req_)->held = nullptr;
			WdfRequestCompleteWithInformation(req_,
				(NTSTATUS)status, info);
			drop();
		}
		drop();
	}

	/* the request cancelled (its EvtRequestCancel): completed as such,
	   with what it had moved */
	void cancel()
	{
		bool held = port_.cancel(this);
		{
			std::lock_guard<std::mutex> lk(mu_);
			cancelled_ = true;
			req_ctx(req_)->held = nullptr;
			WdfRequestCompleteWithInformation(req_,
				STATUS_CANCELLED, vmodem::Port::done(this));
		}
		drop();
		if (held)
			drop();
	}

	/* one reference let go: the last deletes */
	void drop()
	{
		if (--refs_ == 0)
			delete this;
	}

private:
	WDFREQUEST req_;
	vmodem::Port &port_;
	uint8_t *buf_;
	size_t len_;
	std::mutex mu_;
	bool cancelled_ = false;
	std::atomic<int> refs_{2};
};

/* EvtRequestCancel: a request the port holds, cancelled */
VOID on_cancel(WDFREQUEST req)
{
	WdfReq *r = req_ctx(req)->held;
	if (r)
		r->cancel();
}

/* the port of the device a queue is on */
vmodem::Port &port_of(WDFQUEUE q)
{
	return device_ctx(WdfIoQueueGetDevice(q))->link->port();
}

/* req given to the port to hold (go: its read(), write() or the wait),
   cancelable first; or completed as cancelled if it already is */
template <class Go>
void hold(WDFREQUEST req, vmodem::Port &port, void *buf, size_t len, Go go)
{
	WdfReq *r = new WdfReq(req, port, buf, len);
	req_ctx(req)->held = r;
	NTSTATUS st = WdfRequestMarkCancelableEx(req, on_cancel);
	if (!NT_SUCCESS(st)) {
		req_ctx(req)->held = nullptr;
		delete r;
		WdfRequestComplete(req, st);
		return;
	}
	go(r);
}

/* EvtIoRead */
VOID on_read(WDFQUEUE q, WDFREQUEST req, size_t length)
{
	void *buf = nullptr;
	size_t len = 0;
	if (length == 0) {
		WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, 0);
		return;
	}
	NTSTATUS st = WdfRequestRetrieveOutputBuffer(req, 1, &buf, &len);
	if (!NT_SUCCESS(st)) {
		WdfRequestComplete(req, st);
		return;
	}
	vmodem::Port &port = port_of(q);
	hold(req, port, buf, len, [&port](WdfReq *r) { port.read(r); });
}

/* EvtIoWrite */
VOID on_write(WDFQUEUE q, WDFREQUEST req, size_t length)
{
	void *buf = nullptr;
	size_t len = 0;
	if (length == 0) {
		WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, 0);
		return;
	}
	NTSTATUS st = WdfRequestRetrieveInputBuffer(req, 1, &buf, &len);
	if (!NT_SUCCESS(st)) {
		WdfRequestComplete(req, st);
		return;
	}
	vmodem::Port &port = port_of(q);
	hold(req, port, buf, len, [&port](WdfReq *r) { port.write(r); });
}

/* EvtIoDeviceControl: the serial IOCTLs, IOCTL_SERIAL_WAIT_ON_MASK held
   until its event */
VOID on_ioctl(WDFQUEUE q, WDFREQUEST req, size_t out_len, size_t in_len,
	      ULONG code)
{
	vmodem::Port &port = port_of(q);
	void *in = nullptr, *out = nullptr;
	size_t il = 0, ol = 0, info = 0;

	if (in_len && !NT_SUCCESS(WdfRequestRetrieveInputBuffer(req, 1, &in,
								 &il)))
		in = nullptr, il = 0;
	if (out_len && !NT_SUCCESS(WdfRequestRetrieveOutputBuffer(req, 1,
								   &out, &ol)))
		out = nullptr, ol = 0;
	if (code == vmodem::IO_WAIT_ON_MASK) {
		if (!out || ol < 4) {
			WdfRequestComplete(req, STATUS_BUFFER_TOO_SMALL);
			return;
		}
		hold(req, port, out, ol, [&](WdfReq *r) {
			uint32_t st = port.ioctl(code, nullptr, 0, nullptr, 0,
						 &info, r);
			/* (refused: completed as any would be) */
			if (st != vmodem::ST_PENDING)
				r->complete(st, 0);
		});
		return;
	}
	uint32_t st = port.ioctl(code, in, il, out, ol, &info, nullptr);
	WdfRequestCompleteWithInformation(req, (NTSTATUS)st, info);
}

/* EvtDeviceFileCreate: the port opened, by one handle at a time (a
   serial port's rule: UMDF has no exclusive devices of its own) */
VOID on_create(WDFDEVICE dev, WDFREQUEST req, WDFFILEOBJECT)
{
	DeviceCtx *ctx = device_ctx(dev);
	if (ctx->opens->fetch_add(1) != 0) {
		ctx->opens->fetch_sub(1);
		WdfRequestComplete(req, STATUS_ACCESS_DENIED);
		return;
	}
	ctx->link->port().open();
	WdfRequestComplete(req, STATUS_SUCCESS);
}

/* EvtFileCleanup: the port's handle closed: what it had waiting
   cancelled, DTR dropped */
VOID on_cleanup(WDFFILEOBJECT file)
{
	DeviceCtx *ctx = device_ctx(WdfFileObjectGetDevice(file));
	ctx->link->port().close();
	ctx->opens->fetch_sub(1);
}

/* EvtCleanupCallback of the device: the pipe closed, SERIALCOMM's value
   taken back */
VOID on_device_cleanup(WDFOBJECT obj)
{
	DeviceCtx *ctx = device_ctx((WDFDEVICE)obj);
	if (ctx->link) {
		ctx->link->stop();
		delete ctx->link;
		ctx->link = nullptr;
	}
	delete ctx->opens;
	ctx->opens = nullptr;
	if (ctx->pdo[0]) {
		WDFKEY key;
		UNICODE_STRING sub = ustr(SERIALCOMM), name = ustr(ctx->pdo);
		if (NT_SUCCESS(WdfDeviceOpenDevicemapKey((WDFDEVICE)obj, &sub,
				KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES,
				&key))) {
			WdfRegistryRemoveValue(key, &name);
			WdfRegistryClose(key);
		}
	}
}

/* PortName from the device's key into ctx->port ("" if it has none) */
void read_port_name(WDFDEVICE dev, DeviceCtx *ctx)
{
	WDFKEY key;
	UNICODE_STRING name = ustr(PORT_NAME), value;

	ctx->port[0] = 0;
	value.Buffer = ctx->port;
	value.Length = 0;
	value.MaximumLength = sizeof(ctx->port) - sizeof(wchar_t);
	if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(dev, PLUGPLAY_REGKEY_DEVICE,
			KEY_QUERY_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key)))
		return;
	if (NT_SUCCESS(WdfRegistryQueryUnicodeString(key, &name, nullptr,
						     &value)))
		ctx->port[value.Length / sizeof(wchar_t)] = 0;
	else
		ctx->port[0] = 0;
	WdfRegistryClose(key);
}

/* the COM port's name for programs: \DosDevices\Global\<name>, and
   SERIALCOMM's value (the PDO's name: the COM port's) */
void publish_port(WDFDEVICE dev, DeviceCtx *ctx)
{
	wchar_t link[128];
	WDFMEMORY mem;
	WDF_OBJECT_ATTRIBUTES attrs;

	_snwprintf(link, 127, L"%s%s", LINK_PREFIX, ctx->port);
	link[127] = 0;
	UNICODE_STRING ul = ustr(link);
	NTSTATUS st = WdfDeviceCreateSymbolicLink(dev, &ul);
	if (!NT_SUCCESS(st))
		dbg("no symbolic link for %ls (0x%08lx)", ctx->port,
		    (unsigned long)st);

	WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
	attrs.ParentObject = dev;
	if (!NT_SUCCESS(WdfDeviceAllocAndQueryProperty(dev,
			DevicePropertyPhysicalDeviceObjectName, NonPagedPoolNx,
			&attrs, &mem)))
		return;
	size_t n = 0;
	const wchar_t *pdo = (const wchar_t *)WdfMemoryGetBuffer(mem, &n);
	n /= sizeof(wchar_t);
	if (n >= sizeof(ctx->pdo) / sizeof(wchar_t))
		n = sizeof(ctx->pdo) / sizeof(wchar_t) - 1;
	wcsncpy(ctx->pdo, pdo, n);
	ctx->pdo[n] = 0;

	WDFKEY key;
	UNICODE_STRING sub = ustr(SERIALCOMM), name = ustr(ctx->pdo),
		value = ustr(ctx->port);
	if (NT_SUCCESS(WdfDeviceOpenDevicemapKey(dev, &sub, KEY_SET_VALUE,
			WDF_NO_OBJECT_ATTRIBUTES, &key))) {
		if (!NT_SUCCESS(WdfRegistryAssignUnicodeString(key, &name,
							       &value)))
			ctx->pdo[0] = 0;
		WdfRegistryClose(key);
	} else {
		ctx->pdo[0] = 0;
	}
}

/* EvtDriverDeviceAdd: the device, its port and its pipe */
NTSTATUS on_device_add(WDFDRIVER, PWDFDEVICE_INIT init)
{
	WDF_FILEOBJECT_CONFIG fcfg;
	WDF_OBJECT_ATTRIBUTES attrs, rattrs;
	WDF_IO_QUEUE_CONFIG qcfg;
	WDFDEVICE dev;
	NTSTATUS st;

	WDF_FILEOBJECT_CONFIG_INIT(&fcfg, on_create, WDF_NO_EVENT_CALLBACK,
				   on_cleanup);
	WdfDeviceInitSetFileObjectConfig(init, &fcfg,
					 WDF_NO_OBJECT_ATTRIBUTES);
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&rattrs, ReqCtx);
	WdfDeviceInitSetRequestAttributes(init, &rattrs);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DeviceCtx);
	attrs.EvtCleanupCallback = on_device_cleanup;
	st = WdfDeviceCreate(&init, &attrs, &dev);
	if (!NT_SUCCESS(st)) {
		dbg("WdfDeviceCreate: 0x%08lx", (unsigned long)st);
		return st;
	}
	DeviceCtx *ctx = device_ctx(dev);
	ctx->link = new vmodem::Link(link_log);
	ctx->opens = new std::atomic<int>(0);
	ctx->pdo[0] = 0;

	st = WdfDeviceCreateDeviceInterface(dev, &MODEM_INTERFACE, nullptr);
	if (!NT_SUCCESS(st)) {
		dbg("no modem interface: 0x%08lx", (unsigned long)st);
		return st;
	}
	read_port_name(dev, ctx);
	if (ctx->port[0])
		publish_port(dev, ctx);

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg,
					       WdfIoQueueDispatchParallel);
	qcfg.EvtIoRead = on_read;
	qcfg.EvtIoWrite = on_write;
	qcfg.EvtIoDeviceControl = on_ioctl;
	st = WdfIoQueueCreate(dev, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, nullptr);
	if (!NT_SUCCESS(st)) {
		dbg("WdfIoQueueCreate: 0x%08lx", (unsigned long)st);
		return st;
	}

	char name[64];
	std::string why;
	if (!ctx->port[0] || !WideCharToMultiByte(CP_UTF8, 0, ctx->port, -1,
						  name, sizeof(name), nullptr,
						  nullptr))
		snprintf(name, sizeof(name), "%s", NO_NAME);
	if (!ctx->link->start(name, &why)) {
		/* (the port works without dmodemd: as a modem switched
		   off) */
		dbg("%s", why.c_str());
	} else {
		dbg("%s: waiting for dmodemd on %s", name,
		    ctx->link->pipe_name().c_str());
	}
	return STATUS_SUCCESS;
}

} // namespace

/* the driver's entry: its device callback */
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	WDF_DRIVER_CONFIG cfg;
	WDF_DRIVER_CONFIG_INIT(&cfg, on_device_add);
	return WdfDriverCreate(drv, reg, WDF_NO_OBJECT_ATTRIBUTES, &cfg,
			       WDF_NO_HANDLE);
}
