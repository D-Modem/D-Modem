/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    driver7.cpp - dmodemvm7.dll, D-Modem's virtual modem driver for
 *                  Windows 7 and 8: driver.cpp's device on UMDF 1.9
 *                  (Windows 7's own UMDF; UMDF 2 begins with Windows
 *                  8.1), a COM DLL in UMDF's host (dmodem7.inf).  The same
 *                  COM port (port.h) and pipe to dmodemd (link.h), the
 *                  same device:
 *      - its COM port's name (PortName, from the device's key) as
 *        \DosDevices\Global\<name> and in HARDWARE\DEVICEMAP\SERIALCOMM;
 *      - GUID_DEVINTERFACE_MODEM's interface;
 *      - one handle at a time: its reads, writes and serial IOCTLs to the
 *        port, the ones that wait cancelable;
 *      - the pipe \\.\pipe\dmodem-vm-<name> while the device runs.
 *
 *    UMDF 1's objects are COM's: the framework calls the driver's
 *    callback objects (IDriverEntry, the device's and the queue's), and
 *    the driver its objects (IWDFDevice, IWDFIoRequest...).  A request the
 *    port holds keeps a buffer of its own (Held), copied into the
 *    request's memory by whichever completes it, the port or a cancel:
 *    a request cancelled before the port has it never has its memory
 *    touched after.  Statuses go back as HRESULT_FROM_NT, which the
 *    reflector turns into the NTSTATUS a serial port's are.
 *
 *    Built where UMDF 1.9's headers are (DMODEM_UMDF1_INCLUDE: the WDK's
 *    Include/wdf/umdf/1.9); umdf1_test runs it in a host of the test's
 *    own (test/umdf1_host.cpp).
 *
 */

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <wudfddi.h>

#include "link.h"
#include "port.h"

/* the driver's class, dmodem7.inf's DriverCLSID */
extern "C" const CLSID CLSID_DModemVM7 = { 0x66416716, 0xac5e, 0x46d3,
	{ 0x8c, 0xbc, 0xdd, 0x42, 0x55, 0xb9, 0xee, 0xc4 } };

namespace {

/* GUID_DEVINTERFACE_MODEM (ntddmodm.h) */
const GUID MODEM_INTERFACE = { 0x2c7089aa, 0x2e0e, 0x11d1,
			       { 0xb1, 0x14, 0x00, 0xc0, 0x4f, 0xc2, 0xaa, 0xe4 } };

const wchar_t LINK_PREFIX[] = L"\\DosDevices\\Global\\";
const wchar_t SERIALCOMM[] = L"SERIALCOMM";
const wchar_t PORT_NAME[] = L"PortName";
/* a port without a name of its own (no PortName): its pipe's */
const char NO_NAME[] = "modem";

/* the DLL's live objects (DllCanUnloadNow) */
std::atomic<long> g_objects{0};

/* a message for the debugger (DebugView) */
void dbg(const char *fmt, ...)
{
	char buf[300];
	va_list ap;
	int n = snprintf(buf, sizeof(buf), "dmodemvm7: ");
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

/* a port's status as the HRESULT UMDF completes a request with */
HRESULT hr_of(uint32_t st)
{
	return st == vmodem::ST_SUCCESS ? S_OK : HRESULT_FROM_NT(st);
}

/* IUnknown's counting, for the classes below (each its QueryInterface) */
class Counted {
public:
	/* (see the class) */
	Counted() { g_objects++; }
	/* (what it holds let go) */
	virtual ~Counted() { g_objects--; }

protected:
	/* one more reference */
	ULONG add() { return (ULONG)++refs_; }
	/* one reference less: the last deletes */
	ULONG release()
	{
		long n = --refs_;
		if (n == 0)
			delete this;
		return (ULONG)n;
	}
	std::atomic<long> refs_{1};
};

/* *out = p (counted) if iid is one of p's; E_NOINTERFACE */
template <class I, class T> bool offer(T *p, REFIID iid, void **out)
{
	if (!IsEqualIID(iid, __uuidof(I)))
		return false;
	*out = static_cast<I *>(p);
	static_cast<I *>(p)->AddRef();
	return true;
}

class Device;

/* a read, a write or an IOCTL_SERIAL_WAIT_ON_MASK the port holds,
   cancelable while it does: its own buffer (a write's copied in), the
   request's memory written only by the one that completes it.  Its
   references: the port's (until it completes it), the request's (until
   it is completed, by complete() or the cancel), and the framework's
   while it is the cancel callback */
class Held final : public vmodem::Request, public IRequestCallbackCancel,
		   public Counted {
public:
	/* (out: the request's memory for what the port gives; n: the
	   buffer's size, in: a write's characters) */
	Held(IWDFIoRequest *req, vmodem::Port &port, IWDFMemory *out, size_t n,
	     const void *in)
		: req_(req), port_(port), out_(out), buf_(n)
	{
		refs_ = 2;
		req_->AddRef();
		if (out_)
			out_->AddRef();
		if (in && n)
			memcpy(buf_.data(), in, n);
	}
	/* (what it holds let go) */
	~Held() override
	{
		if (out_)
			out_->Release();
		req_->Release();
	}

	/* (Request's) the buffer of its own */
	uint8_t *data() override { return buf_.data(); }
	/* (Request's) its size */
	size_t size() const override { return buf_.size(); }

	/* see port.h: the request completed unless its cancel has it */
	void complete(uint32_t status, size_t info) override
	{
		bool mine = false;
		{
			std::lock_guard<std::mutex> lk(mu_);
			if (!cancelled_ && SUCCEEDED(req_->UnmarkCancelable())) {
				mine = true;
				done_ = true;
			}
		}
		if (mine) {
			finish(hr_of(status), info);
			Release();
		}
		Release();
	}

	/* IRequestCallbackCancel: completed as cancelled, with what it had
	   moved */
	void STDMETHODCALLTYPE OnCancel(IWDFIoRequest *) override
	{
		{
			std::lock_guard<std::mutex> lk(mu_);
			cancelling_ = true;
		}
		bool held = port_.cancel(this);
		{
			std::lock_guard<std::mutex> lk(mu_);
			if (done_)
				return;
			cancelled_ = true;
			done_ = true;
			finish(hr_of(vmodem::ST_CANCELLED),
			       vmodem::Port::done(this));
		}
		Release();
		if (held)
			Release();
	}

	/* its cancel has come (or is coming) */
	bool cancelling()
	{
		std::lock_guard<std::mutex> lk(mu_);
		return cancelling_;
	}

	/* the request marked cancelable (its cancel may come at once) */
	void mark() { req_->MarkCancelable(this); }

	/* IUnknown's: one more reference */
	ULONG STDMETHODCALLTYPE AddRef() override { return add(); }
	/* IUnknown's: one reference less (the last deletes) */
	ULONG STDMETHODCALLTYPE Release() override { return release(); }
	/* IUnknown's: the interfaces it has */
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
	{
		if (offer<IRequestCallbackCancel>(this, iid, out))
			return S_OK;
		if (IsEqualIID(iid, IID_IUnknown)) {
			*out = static_cast<IRequestCallbackCancel *>(this);
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}

private:
	/* the request's memory given what the port moved, then completed */
	void finish(HRESULT hr, size_t info)
	{
		if (out_ && info) {
			if (info > buf_.size())
				info = buf_.size();
			out_->CopyFromBuffer(0, buf_.data(), info);
		}
		req_->CompleteWithInformation(hr, info);
	}

	IWDFIoRequest *req_;
	vmodem::Port &port_;
	IWDFMemory *out_;
	std::vector<uint8_t> buf_;
	std::mutex mu_;
	bool cancelling_ = false, cancelled_ = false, done_ = false;
};

/* the device: its callbacks (the hardware's start and release, a file's
   cleanup) and its queue's (create, read, write, IOCTLs), the link with
   the port */
class Device final : public IPnpCallbackHardware, public IFileCallbackCleanup,
		     public IQueueCallbackCreate, public IQueueCallbackRead,
		     public IQueueCallbackWrite,
		     public IQueueCallbackDeviceIoControl, public Counted {
public:
	/* (see the class) */
	Device() : link_(link_log) {}

	/* the framework's device made on init, with the default queue */
	HRESULT create(IWDFDriver *drv, IWDFDeviceInitialize *init)
	{
		IWDFDevice *dev = nullptr;
		IWDFIoQueue *q = nullptr;

		/* (the port is thread safe: no framework locking) */
		init->SetLockingConstraint(None);
		init->SetPowerPolicyOwnership(TRUE);
		HRESULT hr = drv->CreateDevice(init,
			static_cast<IPnpCallbackHardware *>(this), &dev);
		if (FAILED(hr)) {
			dbg("CreateDevice: 0x%08lx", (unsigned long)hr);
			return hr;
		}
		hr = dev->CreateDeviceInterface(&MODEM_INTERFACE, nullptr);
		if (FAILED(hr))
			dbg("no modem interface: 0x%08lx", (unsigned long)hr);
		if (SUCCEEDED(hr))
			hr = dev->CreateIoQueue(
				static_cast<IQueueCallbackRead *>(this), TRUE,
				WdfIoQueueDispatchParallel, FALSE, FALSE, &q);
		/* (the creates too, for a port's one handle) */
		if (SUCCEEDED(hr))
			hr = dev->ConfigureRequestDispatching(q, WdfRequestCreate,
							      TRUE);
		if (FAILED(hr))
			dbg("the queue: 0x%08lx", (unsigned long)hr);
		if (q)
			q->Release();
		/* (the framework holds the device) */
		dev->Release();
		return hr;
	}

	/* IPnpCallbackHardware: started: the port's names, the pipe */
	HRESULT STDMETHODCALLTYPE OnPrepareHardware(IWDFDevice *dev) override
	{
		read_port_name(dev);
		if (port_[0])
			publish_port(dev);
		char name[64];
		std::string why;
		if (!port_[0] || !WideCharToMultiByte(CP_UTF8, 0, port_, -1,
						      name, sizeof(name), nullptr,
						      nullptr))
			snprintf(name, sizeof(name), "%s", NO_NAME);
		/* (the port works without dmodemd: as a modem switched off) */
		if (!link_.start(name, &why))
			dbg("%s", why.c_str());
		else
			dbg("%s: waiting for dmodemd on %s", name,
			    link_.pipe_name().c_str());
		return S_OK;
	}

	/* IPnpCallbackHardware: released: the pipe closed, SERIALCOMM's
	   value taken back */
	HRESULT STDMETHODCALLTYPE OnReleaseHardware(IWDFDevice *dev) override
	{
		link_.stop();
		if (pdo_[0]) {
			IWDFNamedPropertyStore2 *s = serialcomm(dev, KEY_SET_VALUE);
			if (s) {
				s->DeleteNamedValue(pdo_);
				s->Release();
			}
			pdo_[0] = 0;
		}
		return S_OK;
	}

	/* IQueueCallbackCreate: the port opened, by one handle at a time */
	void STDMETHODCALLTYPE OnCreateFile(IWDFIoQueue *, IWDFIoRequest *req,
					    IWDFFile *) override
	{
		if (opens_.fetch_add(1) != 0) {
			opens_.fetch_sub(1);
			req->Complete(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
			return;
		}
		link_.port().open();
		req->Complete(S_OK);
	}

	/* IFileCallbackCleanup: the port's handle closed: what it had
	   waiting cancelled, DTR dropped */
	void STDMETHODCALLTYPE OnCleanupFile(IWDFFile *) override
	{
		link_.port().close();
		opens_.fetch_sub(1);
	}

	/* IQueueCallbackRead */
	void STDMETHODCALLTYPE OnRead(IWDFIoQueue *, IWDFIoRequest *req,
				      SIZE_T n) override
	{
		IWDFMemory *mem = nullptr;
		req->GetOutputMemory(&mem);
		if (!mem) {
			req->Complete(E_INVALIDARG);
			return;
		}
		vmodem::Port &port = link_.port();
		hold(req, mem, n, nullptr,
		     [&port](Held *r) { port.read(r); });
		mem->Release();
	}

	/* IQueueCallbackWrite */
	void STDMETHODCALLTYPE OnWrite(IWDFIoQueue *, IWDFIoRequest *req,
				       SIZE_T n) override
	{
		IWDFMemory *mem = nullptr;
		SIZE_T len = 0;
		req->GetInputMemory(&mem);
		const void *in = mem ? mem->GetDataBuffer(&len) : nullptr;
		if (!in) {
			if (mem)
				mem->Release();
			req->Complete(E_INVALIDARG);
			return;
		}
		if (len > n)
			len = n;
		vmodem::Port &port = link_.port();
		hold(req, nullptr, len, in,
		     [&port](Held *r) { port.write(r); });
		mem->Release();
	}

	/* IQueueCallbackDeviceIoControl: the serial IOCTLs,
	   IOCTL_SERIAL_WAIT_ON_MASK held until its event */
	void STDMETHODCALLTYPE OnDeviceIoControl(IWDFIoQueue *,
		IWDFIoRequest *req, ULONG code, SIZE_T in_len,
		SIZE_T out_len) override
	{
		vmodem::Port &port = link_.port();
		IWDFMemory *im = nullptr, *om = nullptr;
		SIZE_T il = 0, ol = 0;
		void *in = nullptr, *out = nullptr;
		size_t info = 0;

		if (in_len) {
			req->GetInputMemory(&im);
			in = im ? im->GetDataBuffer(&il) : nullptr;
		}
		if (out_len) {
			req->GetOutputMemory(&om);
			out = om ? om->GetDataBuffer(&ol) : nullptr;
		}
		if (!in)
			il = 0;
		if (!out)
			ol = 0;
		if (code == vmodem::IO_WAIT_ON_MASK) {
			if (!out || ol < 4) {
				req->Complete(hr_of(vmodem::ST_BUFFER_TOO_SMALL));
			} else {
				hold(req, om, ol, nullptr, [&](Held *r) {
					uint32_t st = port.ioctl(code, nullptr,
						0, nullptr, 0, &info, r);
					/* (refused: completed as any would
					   be) */
					if (st != vmodem::ST_PENDING)
						r->complete(st, 0);
				});
			}
		} else {
			/* (the reply in a buffer of ours, then the
			   request's) */
			std::vector<uint8_t> reply(ol);
			uint32_t st = port.ioctl(code, in, il,
						 ol ? reply.data() : nullptr, ol,
						 &info, nullptr);
			if (om && info) {
				if (info > ol)
					info = ol;
				om->CopyFromBuffer(0, reply.data(), info);
			}
			req->CompleteWithInformation(hr_of(st), info);
		}
		if (im)
			im->Release();
		if (om)
			om->Release();
	}

	/* IUnknown's: one more reference */
	ULONG STDMETHODCALLTYPE AddRef() override { return add(); }
	/* IUnknown's: one reference less (the last deletes) */
	ULONG STDMETHODCALLTYPE Release() override { return release(); }
	/* IUnknown's: the interfaces it has */
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
	{
		if (offer<IPnpCallbackHardware>(this, iid, out) ||
		    offer<IFileCallbackCleanup>(this, iid, out) ||
		    offer<IQueueCallbackCreate>(this, iid, out) ||
		    offer<IQueueCallbackRead>(this, iid, out) ||
		    offer<IQueueCallbackWrite>(this, iid, out) ||
		    offer<IQueueCallbackDeviceIoControl>(this, iid, out))
			return S_OK;
		if (IsEqualIID(iid, IID_IUnknown)) {
			*out = static_cast<IPnpCallbackHardware *>(this);
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}

private:
	/* req given to the port to hold (go: its read(), write() or the
	   wait), cancelable first; not if its cancel came at once, and
	   taken back from the port if the cancel came while it was given
	   (the cancel looked for it there too soon): a cancelled read
	   left in the port would take characters the next one should */
	template <class Go>
	void hold(IWDFIoRequest *req, IWDFMemory *out, size_t n, const void *in,
		  Go go)
	{
		vmodem::Port &port = link_.port();
		Held *r = new Held(req, port, out, n, in);
		r->mark();
		if (r->cancelling()) {
			r->Release();   /* (the port's: it never had it) */
			return;
		}
		r->AddRef();
		go(r);
		if (r->cancelling() && port.cancel(r))
			r->Release();   /* (the port's) */
		r->Release();
	}

	/* a property store of the device's: the device's key, or
	   HARDWARE\DEVICEMAP\SERIALCOMM; null if not */
	IWDFNamedPropertyStore2 *store(IWDFDevice *dev,
				       WDF_PROPERTY_STORE_ROOT_CLASS cls,
				       REGSAM access)
	{
		IWDFPropertyStoreFactory *f = nullptr;
		IWDFNamedPropertyStore2 *s = nullptr;
		WDF_PROPERTY_STORE_ROOT root = {};

		if (FAILED(dev->QueryInterface(__uuidof(IWDFPropertyStoreFactory),
					       (void **)&f)))
			return nullptr;
		root.LengthCb = sizeof(root);
		root.RootClass = cls;
		if (cls == WdfPropertyStoreRootClassHardwareKey)
			root.Qualifier.HardwareKey.ServiceName =
				WDF_PROPERTY_STORE_HARDWARE_KEY_ROOT;
		else
			root.Qualifier.LegacyHardwareKey.LegacyMapName =
				SERIALCOMM;
		if (FAILED(f->RetrieveDevicePropertyStore(&root,
				cls == WdfPropertyStoreRootClassHardwareKey ?
				WdfPropertyStoreNormal :
				WdfPropertyStoreCreateVolatile,
				access, nullptr, &s, nullptr)))
			s = nullptr;
		f->Release();
		return s;
	}

	/* SERIALCOMM's store */
	IWDFNamedPropertyStore2 *serialcomm(IWDFDevice *dev, REGSAM access)
	{
		return store(dev, WdfPropertyStoreRootClassLegacyHardwareKey,
			     access);
	}

	/* PortName from the device's key into port_ ("" if it has none) */
	void read_port_name(IWDFDevice *dev)
	{
		port_[0] = 0;
		IWDFNamedPropertyStore2 *s = store(dev,
			WdfPropertyStoreRootClassHardwareKey, KEY_QUERY_VALUE);
		if (!s)
			return;
		PROPVARIANT v;
		PropVariantInit(&v);
		if (SUCCEEDED(s->GetNamedValue(PORT_NAME, &v)) &&
		    v.vt == VT_LPWSTR && v.pwszVal) {
			wcsncpy(port_, v.pwszVal, std::size(port_) - 1);
			port_[std::size(port_) - 1] = 0;
		}
		PropVariantClear(&v);
		s->Release();
	}

	/* the COM port's name for programs: \DosDevices\Global\<name>, and
	   SERIALCOMM's value (the device's kernel name: the COM port's) */
	void publish_port(IWDFDevice *dev)
	{
		wchar_t link[128];
		_snwprintf(link, 127, L"%s%s", LINK_PREFIX, port_);
		link[127] = 0;
		HRESULT hr = dev->CreateSymbolicLink(link);
		/* (already there after a restart) */
		if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
			dbg("no symbolic link for %ls (0x%08lx)", port_,
			    (unsigned long)hr);

		DWORD n = (DWORD)std::size(pdo_);
		if (FAILED(dev->RetrieveDeviceName(pdo_, &n))) {
			pdo_[0] = 0;
			return;
		}
		IWDFNamedPropertyStore2 *s = serialcomm(dev, KEY_SET_VALUE);
		PROPVARIANT v;
		PropVariantInit(&v);
		v.vt = VT_LPWSTR;
		v.pwszVal = port_;
		if (!s || FAILED(s->SetNamedValue(pdo_, &v)))
			pdo_[0] = 0;
		if (s)
			s->Release();
	}

	vmodem::Link link_;
	std::atomic<int> opens_{0};     /* the port's handles (one at most) */
	wchar_t port_[64] = {};         /* PortName, "" if none */
	wchar_t pdo_[128] = {};         /* SERIALCOMM's value name, "" if
					   none written */
};

/* the driver: a device for each of ours */
class Driver final : public IDriverEntry, public Counted {
public:
	/* IDriverEntry: nothing to set up */
	HRESULT STDMETHODCALLTYPE OnInitialize(IWDFDriver *) override
	{
		return S_OK;
	}
	/* IDriverEntry: the device, its port and its pipe */
	HRESULT STDMETHODCALLTYPE OnDeviceAdd(IWDFDriver *drv,
		IWDFDeviceInitialize *init) override
	{
		Device *d = new Device();
		HRESULT hr = d->create(drv, init);
		/* (the framework holds it, when made) */
		d->Release();
		return hr;
	}
	/* IDriverEntry: nothing to let go */
	void STDMETHODCALLTYPE OnDeinitialize(IWDFDriver *) override {}

	/* IUnknown's: one more reference */
	ULONG STDMETHODCALLTYPE AddRef() override { return add(); }
	/* IUnknown's: one reference less (the last deletes) */
	ULONG STDMETHODCALLTYPE Release() override { return release(); }
	/* IUnknown's: the interfaces it has */
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
	{
		if (offer<IDriverEntry>(this, iid, out))
			return S_OK;
		if (IsEqualIID(iid, IID_IUnknown)) {
			*out = static_cast<IDriverEntry *>(this);
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
};

/* the class factory UMDF's host asks for the driver (DllGetClassObject) */
class Factory final : public IClassFactory {
public:
	/* IClassFactory: a driver object */
	HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID iid,
						 void **out) override
	{
		*out = nullptr;
		if (outer)
			return CLASS_E_NOAGGREGATION;
		Driver *d = new Driver();
		HRESULT hr = d->QueryInterface(iid, out);
		d->Release();
		return hr;
	}
	/* IClassFactory: the DLL held loaded (or let go) */
	HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
	{
		if (lock)
			g_objects++;
		else
			g_objects--;
		return S_OK;
	}
	/* (a static object: no counting) */
	ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
	/* IUnknown's: one reference less (the last deletes) */
	ULONG STDMETHODCALLTYPE Release() override { return 1; }
	/* IUnknown's: the interfaces it has */
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
	{
		if (IsEqualIID(iid, IID_IClassFactory) ||
		    IsEqualIID(iid, IID_IUnknown)) {
			*out = static_cast<IClassFactory *>(this);
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
};

Factory g_factory;

} // namespace

/* the DLL's entry for UMDF's host (dmodemvm7.def exports it): the
   driver's class factory */
extern "C" HRESULT STDAPICALLTYPE DModemGetClassObject(REFCLSID clsid,
						       REFIID iid, void **out)
{
	*out = nullptr;
	if (!IsEqualCLSID(clsid, CLSID_DModemVM7))
		return CLASS_E_CLASSNOTAVAILABLE;
	return g_factory.QueryInterface(iid, out);
}

/* the DLL unloaded when no object of it is left */
extern "C" HRESULT STDAPICALLTYPE DModemCanUnloadNow()
{
	return g_objects == 0 ? S_OK : S_FALSE;
}
