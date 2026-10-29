/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    umdf1_host.cpp - umdf1_test: dmodemvm7.dll, the virtual modem driver
 *                     for Windows 7 and 8 (driver7.cpp), in a UMDF 1 host
 *                     of this test's own (Wine has no UMDF), with dmodemd
 *                     started beside it on the driver's pipe (--driver):
 *      - the DLL's class factory, the driver's IDriverEntry, a device
 *        added: its callbacks, its modem interface, its default queue
 *        (creates dispatched to it too);
 *      - the hardware prepared: the COM port's symbolic link and its
 *        SERIALCOMM value; released: the value taken back;
 *      - one handle at a time (a second create refused);
 *      - AT commands through the driver's writes, reads and serial IOCTLs
 *        (DSR and CTS once dmodemd is there);
 *      - a read cancelled while the port holds it, and one whose cancel
 *        comes as it is marked cancelable (the port never has it: what
 *        comes next isn't lost to it);
 *      - IOCTL_SERIAL_WAIT_ON_MASK held until EV_RXCHAR;
 *      - every object of the DLL let go at the end (DllCanUnloadNow).
 *
 *    The framework's objects here do what the driver asks of them and
 *    record it; methods it doesn't use answer E_NOTIMPL.  Built with
 *    UMDF 1.9's headers (DMODEM_UMDF1_INCLUDE), as the driver.
 *
 */

#include <windows.h>
#include <wudfddi.h>

#include <stdio.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "port.h"

using namespace vmodem;

namespace {

const CLSID CLSID_DModemVM7 = { 0x66416716, 0xac5e, 0x46d3,
	{ 0x8c, 0xbc, 0xdd, 0x42, 0x55, 0xb9, 0xee, 0xc4 } };
const GUID MODEM_INTERFACE = { 0x2c7089aa, 0x2e0e, 0x11d1,
			       { 0xb1, 0x14, 0x00, 0xc0, 0x4f, 0xc2, 0xaa, 0xe4 } };
const wchar_t DEVICE_NAME[] = L"\\Device\\UMDF7Test";

/* the program's directory, with name */
std::string beside(const char *name)
{
	char me[MAX_PATH];
	DWORD n = GetModuleFileNameA(nullptr, me, sizeof(me));
	std::string s(me, n);
	return s.substr(0, s.find_last_of("\\/") + 1) + name;
}

/* a port name of our own: parallel runs don't meet */
std::wstring port_name()
{
	return L"TEST7COM" + std::to_wstring(GetCurrentProcessId());
}

/* IUnknown counted, and QueryInterface for I (and IUnknown, IWDFObject
   where I is one) */
template <class I> class Unk : public I {
public:
	/* (what it holds let go) */
	virtual ~Unk() {}
	/* IUnknown's: one more reference */
	ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++refs; }
	/* IUnknown's: one reference less (the last deletes) */
	ULONG STDMETHODCALLTYPE Release() override
	{
		long n = --refs;
		if (n == 0)
			delete this;
		return (ULONG)n;
	}
	/* IUnknown's: the interfaces it has */
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
	{
		if (IsEqualIID(iid, __uuidof(I)) || IsEqualIID(iid, IID_IUnknown) ||
		    (std::is_base_of<IWDFObject, I>::value &&
		     IsEqualIID(iid, __uuidof(IWDFObject)))) {
			*out = static_cast<I *>(this);
			AddRef();
			return S_OK;
		}
		return more(iid, out);
	}
	/* the other interfaces an object has */
	virtual HRESULT more(REFIID, void **out)
	{
		*out = nullptr;
		return E_NOINTERFACE;
	}
	std::atomic<long> refs{1};
};

/* IWDFObject's methods, none used */
template <class I> class Obj : public Unk<I> {
public:
	/* (the framework's DeleteWdfObject: not used by the driver) */
	HRESULT STDMETHODCALLTYPE DeleteWdfObject() override { return E_NOTIMPL; }
	/* (the framework's AssignContext: not used by the driver) */
	HRESULT STDMETHODCALLTYPE AssignContext(IObjectCleanup *, void *) override
	{
		return E_NOTIMPL;
	}
	/* (the framework's RetrieveContext: not used by the driver) */
	HRESULT STDMETHODCALLTYPE RetrieveContext(void **) override
	{
		return E_NOTIMPL;
	}
	/* (the framework's AcquireLock: not used by the driver) */
	void STDMETHODCALLTYPE AcquireLock() override {}
	/* (the framework's ReleaseLock: not used by the driver) */
	void STDMETHODCALLTYPE ReleaseLock() override {}
};

/* a request's buffer */
class Memory final : public Obj<IWDFMemory> {
public:
	/* (see the class) */
	explicit Memory(size_t n) : buf(n) {}
	/* (the framework's CopyFromMemory: not used by the driver) */
	HRESULT STDMETHODCALLTYPE CopyFromMemory(IWDFMemory *,
		PWDFMEMORY_OFFSET) override { return E_NOTIMPL; }
	/* (the framework's CopyToBuffer: not used by the driver) */
	HRESULT STDMETHODCALLTYPE CopyToBuffer(ULONG_PTR off, void *to,
					       SIZE_T n) override
	{
		if (off + n > buf.size())
			return E_INVALIDARG;
		memcpy(to, buf.data() + off, n);
		return S_OK;
	}
	/* (the framework's CopyFromBuffer, as the driver uses it) */
	HRESULT STDMETHODCALLTYPE CopyFromBuffer(ULONG_PTR off, void *from,
						 SIZE_T n) override
	{
		if (off + n > buf.size())
			return E_INVALIDARG;
		memcpy(buf.data() + off, from, n);
		return S_OK;
	}
	/* (the framework's GetSize: not used by the driver) */
	SIZE_T STDMETHODCALLTYPE GetSize() override { return buf.size(); }
	/* (the framework's GetDataBuffer, as the driver uses it) */
	void *STDMETHODCALLTYPE GetDataBuffer(SIZE_T *n) override
	{
		if (n)
			*n = buf.size();
		return buf.empty() ? nullptr : buf.data();
	}
	/* (the framework's SetBuffer: not used by the driver) */
	void STDMETHODCALLTYPE SetBuffer(void *, SIZE_T) override {}

	std::vector<uint8_t> buf;
};

/* a file object (the port's handle) */
class File final : public Obj<IWDFFile> {
public:
	/* (the framework's RetrieveFileName: not used by the driver) */
	HRESULT STDMETHODCALLTYPE RetrieveFileName(PWSTR, DWORD *) override
	{
		return E_NOTIMPL;
	}
	/* (the framework's GetDevice: not used by the driver) */
	void STDMETHODCALLTYPE GetDevice(IWDFDevice **d) override { *d = nullptr; }
};

/* a request: its type and buffers, its cancel (cancel()), its
   completion recorded and waited for */
class Request final : public Obj<IWDFIoRequest> {
public:
	/* (see the class) */
	Request(WDF_REQUEST_TYPE t, size_t in, size_t out, ULONG code = 0)
		: type(t), code(code),
		  input(in ? new Memory(in) : nullptr),
		  output(out ? new Memory(out) : nullptr) {}
	/* (what it holds let go) */
	~Request() override
	{
		if (input)
			input->Release();
		if (output)
			output->Release();
	}

	/* the framework's cancel: the callback called if it is marked */
	void cancel()
	{
		IRequestCallbackCancel *cb = nullptr;
		{
			std::lock_guard<std::mutex> lk(mu);
			cancelled = true;
			cb = cancel_cb;
			cancel_cb = nullptr;
		}
		if (cb) {
			cb->OnCancel(this);
			cb->Release();
		}
	}
	/* completed within ms */
	bool wait(DWORD ms)
	{
		std::unique_lock<std::mutex> lk(mu);
		return cv.wait_for(lk, std::chrono::milliseconds(ms),
				   [this] { return done; });
	}
	/* what the output memory got */
	std::string got()
	{
		return std::string(output->buf.begin(),
				   output->buf.begin() + info);
	}

	/* (the framework's CompleteWithInformation, as the driver uses it) */
	void STDMETHODCALLTYPE CompleteWithInformation(HRESULT hr,
						       SIZE_T n) override
	{
		std::lock_guard<std::mutex> lk(mu);
		completions++;
		status = hr;
		info = n;
		done = true;
		cv.notify_all();
	}
	/* (the framework's SetInformation: not used by the driver) */
	void STDMETHODCALLTYPE SetInformation(ULONG_PTR n) override { info = n; }
	/* (the framework's Complete, as the driver uses it) */
	void STDMETHODCALLTYPE Complete(HRESULT hr) override
	{
		CompleteWithInformation(hr, info);
	}
	/* (the framework's SetCompletionCallback: not used by the driver) */
	void STDMETHODCALLTYPE SetCompletionCallback(
		IRequestCallbackRequestCompletion *, void *) override {}
	/* (the framework's GetType: not used by the driver) */
	WDF_REQUEST_TYPE STDMETHODCALLTYPE GetType() override { return type; }
	/* (the framework's GetCreateParameters: not used by the driver) */
	void STDMETHODCALLTYPE GetCreateParameters(ULONG *, USHORT *,
						   USHORT *) override {}
	/* (the framework's GetReadParameters: not used by the driver) */
	void STDMETHODCALLTYPE GetReadParameters(SIZE_T *n, LONGLONG *,
						 ULONG *) override
	{
		if (n)
			*n = output ? output->buf.size() : 0;
	}
	/* (the framework's GetWriteParameters: not used by the driver) */
	void STDMETHODCALLTYPE GetWriteParameters(SIZE_T *n, LONGLONG *,
						  ULONG *) override
	{
		if (n)
			*n = input ? input->buf.size() : 0;
	}
	/* (the framework's GetDeviceIoControlParameters: not used by the driver) */
	void STDMETHODCALLTYPE GetDeviceIoControlParameters(ULONG *c,
		SIZE_T *in, SIZE_T *out) override
	{
		if (c)
			*c = code;
		if (in)
			*in = input ? input->buf.size() : 0;
		if (out)
			*out = output ? output->buf.size() : 0;
	}
	/* (the framework's GetOutputMemory, as the driver uses it) */
	void STDMETHODCALLTYPE GetOutputMemory(IWDFMemory **m) override
	{
		*m = output;
		if (output)
			output->AddRef();
	}
	/* (the framework's GetInputMemory, as the driver uses it) */
	void STDMETHODCALLTYPE GetInputMemory(IWDFMemory **m) override
	{
		*m = input;
		if (input)
			input->AddRef();
	}
	/* (the framework's MarkCancelable, as the driver uses it) */
	void STDMETHODCALLTYPE MarkCancelable(IRequestCallbackCancel *cb) override
	{
		marks++;
		cb->AddRef();
		bool now;
		{
			std::lock_guard<std::mutex> lk(mu);
			cancel_cb = cb;
			now = cancelled || cancel_on_mark;
			cancelled = now;
		}
		/* (cancelled already: the callback at once, as UMDF does) */
		if (now) {
			{
				std::lock_guard<std::mutex> lk(mu);
				cancel_cb = nullptr;
			}
			cb->OnCancel(this);
			cb->Release();
		}
	}
	/* (the framework's UnmarkCancelable, as the driver uses it) */
	HRESULT STDMETHODCALLTYPE UnmarkCancelable() override
	{
		IRequestCallbackCancel *cb;
		{
			std::lock_guard<std::mutex> lk(mu);
			if (!cancel_cb)
				return HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
			cb = cancel_cb;
			cancel_cb = nullptr;
		}
		cb->Release();
		return S_OK;
	}
	/* (the framework's CancelSentRequest: not used by the driver) */
	BOOL STDMETHODCALLTYPE CancelSentRequest() override { return FALSE; }
	/* (the framework's ForwardToIoQueue: not used by the driver) */
	HRESULT STDMETHODCALLTYPE ForwardToIoQueue(IWDFIoQueue *) override
	{
		return E_NOTIMPL;
	}
	/* (the framework's Send: not used by the driver) */
	HRESULT STDMETHODCALLTYPE Send(IWDFIoTarget *, ULONG, LONGLONG) override
	{
		return E_NOTIMPL;
	}
	/* (the framework's GetFileObject: not used by the driver) */
	void STDMETHODCALLTYPE GetFileObject(IWDFFile **f) override { *f = nullptr; }
	/* (the framework's FormatUsingCurrentType: not used by the driver) */
	void STDMETHODCALLTYPE FormatUsingCurrentType() override {}
	/* (the framework's GetRequestorProcessId: not used by the driver) */
	ULONG STDMETHODCALLTYPE GetRequestorProcessId() override { return 0; }
	/* (the framework's GetIoQueue: not used by the driver) */
	void STDMETHODCALLTYPE GetIoQueue(IWDFIoQueue **q) override { *q = nullptr; }
	/* (the framework's Impersonate: not used by the driver) */
	HRESULT STDMETHODCALLTYPE Impersonate(SECURITY_IMPERSONATION_LEVEL,
		IImpersonateCallback *, void *) override { return E_NOTIMPL; }
	/* (the framework's IsFrom32BitProcess: not used by the driver) */
	BOOL STDMETHODCALLTYPE IsFrom32BitProcess() override { return FALSE; }
	/* (the framework's GetCompletionParams: not used by the driver) */
	void STDMETHODCALLTYPE GetCompletionParams(
		IWDFRequestCompletionParams **p) override { *p = nullptr; }

	WDF_REQUEST_TYPE type;
	ULONG code;
	Memory *input, *output;
	std::mutex mu;
	std::condition_variable cv;
	IRequestCallbackCancel *cancel_cb = nullptr;
	bool cancelled = false, cancel_on_mark = false, done = false;
	int completions = 0, marks = 0;
	HRESULT status = E_FAIL;
	size_t info = 0;
};

/* the device's default queue (the driver's callbacks are the device's) */
class Queue final : public Obj<IWDFIoQueue> {
public:
	/* (the framework's GetDevice: not used by the driver) */
	void STDMETHODCALLTYPE GetDevice(IWDFDevice **d) override { *d = nullptr; }
	/* (the framework's ConfigureRequestDispatching: not used by the driver) */
	HRESULT STDMETHODCALLTYPE ConfigureRequestDispatching(WDF_REQUEST_TYPE,
							      BOOL) override
	{
		return E_NOTIMPL;
	}
	/* (the framework's GetState: not used by the driver) */
	WDF_IO_QUEUE_STATE STDMETHODCALLTYPE GetState(ULONG *, ULONG *) override
	{
		return (WDF_IO_QUEUE_STATE)0;
	}
	/* (the framework's RetrieveNextRequest: not used by the driver) */
	HRESULT STDMETHODCALLTYPE RetrieveNextRequest(IWDFIoRequest **) override
	{
		return E_NOTIMPL;
	}
	/* (the framework's RetrieveNextRequestByFileObject: not used by the driver) */
	HRESULT STDMETHODCALLTYPE RetrieveNextRequestByFileObject(IWDFFile *,
		IWDFIoRequest **) override { return E_NOTIMPL; }
	/* (the framework's Start: not used by the driver) */
	void STDMETHODCALLTYPE Start() override {}
	/* (the framework's Stop: not used by the driver) */
	void STDMETHODCALLTYPE Stop(IQueueCallbackStateChange *) override {}
	/* (the framework's StopSynchronously: not used by the driver) */
	void STDMETHODCALLTYPE StopSynchronously() override {}
	/* (the framework's Drain: not used by the driver) */
	void STDMETHODCALLTYPE Drain(IQueueCallbackStateChange *) override {}
	/* (the framework's DrainSynchronously: not used by the driver) */
	void STDMETHODCALLTYPE DrainSynchronously() override {}
	/* (the framework's Purge: not used by the driver) */
	void STDMETHODCALLTYPE Purge(IQueueCallbackStateChange *) override {}
	/* (the framework's PurgeSynchronously: not used by the driver) */
	void STDMETHODCALLTYPE PurgeSynchronously() override {}
};

/* a registry key: the device's (PortName) or SERIALCOMM's */
class Store final : public Unk<IWDFNamedPropertyStore2> {
public:
	/* (the framework's more: not used by the driver) */
	HRESULT more(REFIID iid, void **out) override
	{
		if (IsEqualIID(iid, __uuidof(IWDFNamedPropertyStore))) {
			*out = static_cast<IWDFNamedPropertyStore *>(this);
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	/* (the framework's GetNamedValue, as the driver uses it) */
	HRESULT STDMETHODCALLTYPE GetNamedValue(LPCWSTR name, PROPVARIANT *v) override
	{
		std::lock_guard<std::mutex> lk(mu);
		auto i = find(name);
		if (i == values.end())
			return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
		size_t n = (i->second.size() + 1) * sizeof(wchar_t);
		v->vt = VT_LPWSTR;
		v->pwszVal = (LPWSTR)CoTaskMemAlloc(n);
		memcpy(v->pwszVal, i->second.c_str(), n);
		return S_OK;
	}
	/* (the framework's SetNamedValue, as the driver uses it) */
	HRESULT STDMETHODCALLTYPE SetNamedValue(LPCWSTR name,
						const PROPVARIANT *v) override
	{
		if (v->vt != VT_LPWSTR)
			return E_INVALIDARG;
		std::lock_guard<std::mutex> lk(mu);
		auto i = find(name);
		if (i != values.end())
			i->second = v->pwszVal;
		else
			values.push_back({ name, v->pwszVal });
		return S_OK;
	}
	/* (the framework's GetNameCount: not used by the driver) */
	HRESULT STDMETHODCALLTYPE GetNameCount(DWORD *n) override
	{
		*n = (DWORD)values.size();
		return S_OK;
	}
	/* (the framework's GetNameAt: not used by the driver) */
	HRESULT STDMETHODCALLTYPE GetNameAt(DWORD, PWSTR *) override
	{
		return E_NOTIMPL;
	}
	/* (the framework's DeleteNamedValue, as the driver uses it) */
	HRESULT STDMETHODCALLTYPE DeleteNamedValue(LPCWSTR name) override
	{
		std::lock_guard<std::mutex> lk(mu);
		auto i = find(name);
		if (i == values.end())
			return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
		values.erase(i);
		return S_OK;
	}
	/* the value of name, "" if none */
	std::wstring get(const wchar_t *name)
	{
		std::lock_guard<std::mutex> lk(mu);
		auto i = find(name);
		return i == values.end() ? L"" : i->second;
	}

	std::mutex mu;
	std::vector<std::pair<std::wstring, std::wstring>> values;

private:
	/* name's value, or the end */
	std::vector<std::pair<std::wstring, std::wstring>>::iterator
	find(LPCWSTR name)
	{
		for (auto i = values.begin(); i != values.end(); ++i)
			if (i->first == name)
				return i;
		return values.end();
	}
};

/* the device: what the driver made of it recorded; its property stores */
class Device final : public Obj<IWDFDevice>, public IWDFPropertyStoreFactory {
public:
	/* (see the class) */
	Device()
	{
		hw = new Store();
		serialcomm = new Store();
		queue = new Queue();
	}
	/* (what it holds let go) */
	~Device() override
	{
		hw->Release();
		serialcomm->Release();
		queue->Release();
		if (queue_cb)
			queue_cb->Release();
	}

	/* (the framework's more: not used by the driver) */
	HRESULT more(REFIID iid, void **out) override
	{
		if (IsEqualIID(iid, __uuidof(IWDFPropertyStoreFactory))) {
			*out = static_cast<IWDFPropertyStoreFactory *>(this);
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	/* (IWDFPropertyStoreFactory's IUnknown: the device's) */
	ULONG STDMETHODCALLTYPE AddRef() override { return Obj::AddRef(); }
	/* IUnknown's: one reference less (the last deletes) */
	ULONG STDMETHODCALLTYPE Release() override { return Obj::Release(); }
	/* IUnknown's: the interfaces it has */
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
	{
		return Obj::QueryInterface(iid, out);
	}

	/* (the framework's RetrieveDevicePropertyStore, as the driver uses it) */
	HRESULT STDMETHODCALLTYPE RetrieveDevicePropertyStore(
		PWDF_PROPERTY_STORE_ROOT root, WDF_PROPERTY_STORE_RETRIEVE_FLAGS,
		REGSAM, PCWSTR, IWDFNamedPropertyStore2 **s,
		WDF_PROPERTY_STORE_DISPOSITION *) override
	{
		Store *st = nullptr;
		if (root->RootClass == WdfPropertyStoreRootClassHardwareKey &&
		    root->Qualifier.HardwareKey.ServiceName &&
		    !wcscmp(root->Qualifier.HardwareKey.ServiceName, L"\\"))
			st = hw;
		else if (root->RootClass ==
			 WdfPropertyStoreRootClassLegacyHardwareKey &&
			 !wcscmp(root->Qualifier.LegacyHardwareKey.LegacyMapName,
				 L"SERIALCOMM"))
			st = serialcomm;
		if (!st)
			return E_INVALIDARG;
		st->AddRef();
		*s = st;
		return S_OK;
	}

	/* (the framework's RetrieveDevicePropertyStore: not used by the driver) */
	HRESULT STDMETHODCALLTYPE RetrieveDevicePropertyStore(PCWSTR,
		WDF_PROPERTY_STORE_RETRIEVE_FLAGS, IWDFNamedPropertyStore **,
		WDF_PROPERTY_STORE_DISPOSITION *) override { return E_NOTIMPL; }
	/* (the framework's GetDriver: not used by the driver) */
	void STDMETHODCALLTYPE GetDriver(IWDFDriver **d) override { *d = nullptr; }
	/* (the framework's RetrieveDeviceInstanceId: not used by the driver) */
	HRESULT STDMETHODCALLTYPE RetrieveDeviceInstanceId(PWSTR, DWORD *) override
	{
		return E_NOTIMPL;
	}
	/* (the framework's GetDefaultIoTarget: not used by the driver) */
	void STDMETHODCALLTYPE GetDefaultIoTarget(IWDFIoTarget **t) override
	{
		*t = nullptr;
	}
	/* (the framework's CreateWdfFile: not used by the driver) */
	HRESULT STDMETHODCALLTYPE CreateWdfFile(LPCWSTR,
		IWDFDriverCreatedFile **) override { return E_NOTIMPL; }
	/* (the framework's GetDefaultIoQueue: not used by the driver) */
	void STDMETHODCALLTYPE GetDefaultIoQueue(IWDFIoQueue **q) override
	{
		*q = queue;
		queue->AddRef();
	}
	/* (the framework's CreateIoQueue, as the driver uses it) */
	HRESULT STDMETHODCALLTYPE CreateIoQueue(IUnknown *cb, BOOL dflt,
		WDF_IO_QUEUE_DISPATCH_TYPE type, BOOL power, BOOL zero,
		IWDFIoQueue **q) override
	{
		queues++;
		queue_default = dflt;
		queue_type = type;
		queue_power = power;
		queue_zero = zero;
		cb->AddRef();
		queue_cb = cb;
		queue->AddRef();
		*q = queue;
		return S_OK;
	}
	/* (the framework's CreateDeviceInterface, as the driver uses it) */
	HRESULT STDMETHODCALLTYPE CreateDeviceInterface(LPCGUID g, PCWSTR) override
	{
		interface_guid = *g;
		return S_OK;
	}
	/* (the framework's AssignDeviceInterfaceState: not used by the driver) */
	HRESULT STDMETHODCALLTYPE AssignDeviceInterfaceState(LPCGUID, PCWSTR,
							     BOOL) override
	{
		return S_OK;
	}
	/* (the framework's RetrieveDeviceName, as the driver uses it) */
	HRESULT STDMETHODCALLTYPE RetrieveDeviceName(PWSTR name, DWORD *n) override
	{
		DWORD need = (DWORD)wcslen(DEVICE_NAME) + 1;
		if (*n < need) {
			*n = need;
			return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
		}
		wcscpy(name, DEVICE_NAME);
		*n = need;
		return S_OK;
	}
	/* (the framework's PostEvent: not used by the driver) */
	HRESULT STDMETHODCALLTYPE PostEvent(REFGUID, WDF_EVENT_TYPE, BYTE *,
					    DWORD) override { return E_NOTIMPL; }
	/* (the framework's ConfigureRequestDispatching, as the driver uses it) */
	HRESULT STDMETHODCALLTYPE ConfigureRequestDispatching(IWDFIoQueue *q,
		WDF_REQUEST_TYPE t, BOOL fwd) override
	{
		if (q == queue && t == WdfRequestCreate && fwd)
			creates_to_queue = true;
		return S_OK;
	}
	/* (the framework's SetPnpState: not used by the driver) */
	void STDMETHODCALLTYPE SetPnpState(WDF_PNP_STATE, WDF_TRI_STATE) override {}
	/* (the framework's GetPnpState: not used by the driver) */
	WDF_TRI_STATE STDMETHODCALLTYPE GetPnpState(WDF_PNP_STATE) override
	{
		return WdfUseDefault;
	}
	/* (the framework's CommitPnpState: not used by the driver) */
	void STDMETHODCALLTYPE CommitPnpState() override {}
	/* (the framework's CreateRequest: not used by the driver) */
	HRESULT STDMETHODCALLTYPE CreateRequest(IUnknown *, IWDFObject *,
						IWDFIoRequest **) override
	{
		return E_NOTIMPL;
	}
	/* (the framework's CreateSymbolicLink, as the driver uses it) */
	HRESULT STDMETHODCALLTYPE CreateSymbolicLink(PCWSTR link) override
	{
		symlink = link;
		return S_OK;
	}

	Store *hw, *serialcomm;
	Queue *queue;
	IUnknown *queue_cb = nullptr;
	IUnknown *callback = nullptr;   /* the device's (CreateDevice's) */
	int queues = 0;
	BOOL queue_default = FALSE, queue_power = TRUE, queue_zero = TRUE;
	WDF_IO_QUEUE_DISPATCH_TYPE queue_type = WdfIoQueueDispatchManual;
	bool creates_to_queue = false;
	GUID interface_guid = {};
	std::wstring symlink;
};

/* the device's initialization: what the driver asked of it */
class DeviceInit final : public Unk<IWDFDeviceInitialize> {
public:
	/* (the framework's SetFilter: not used by the driver) */
	void STDMETHODCALLTYPE SetFilter() override { filter = true; }
	/* (the framework's SetLockingConstraint, as the driver uses it) */
	void STDMETHODCALLTYPE SetLockingConstraint(WDF_CALLBACK_CONSTRAINT c) override
	{
		locking = c;
	}
	/* (the framework's RetrieveDevicePropertyStore: not used by the driver) */
	HRESULT STDMETHODCALLTYPE RetrieveDevicePropertyStore(PCWSTR,
		WDF_PROPERTY_STORE_RETRIEVE_FLAGS, IWDFNamedPropertyStore **,
		WDF_PROPERTY_STORE_DISPOSITION *) override { return E_NOTIMPL; }
	/* (the framework's SetPowerPolicyOwnership, as the driver uses it) */
	void STDMETHODCALLTYPE SetPowerPolicyOwnership(BOOL b) override
	{
		power_owner = b;
	}
	/* (the framework's AutoForwardCreateCleanupClose: not used by the driver) */
	void STDMETHODCALLTYPE AutoForwardCreateCleanupClose(WDF_TRI_STATE) override {}
	/* (the framework's RetrieveDeviceInstanceId: not used by the driver) */
	HRESULT STDMETHODCALLTYPE RetrieveDeviceInstanceId(PWSTR, DWORD *) override
	{
		return E_NOTIMPL;
	}
	/* (the framework's SetPnpCapability: not used by the driver) */
	void STDMETHODCALLTYPE SetPnpCapability(WDF_PNP_CAPABILITY,
						WDF_TRI_STATE) override {}
	/* (the framework's GetPnpCapability: not used by the driver) */
	WDF_TRI_STATE STDMETHODCALLTYPE GetPnpCapability(WDF_PNP_CAPABILITY) override
	{
		return WdfUseDefault;
	}

	bool filter = false;
	BOOL power_owner = FALSE;
	WDF_CALLBACK_CONSTRAINT locking = WdfDeviceLevel;
};

/* the driver object: its device made */
class Driver final : public Obj<IWDFDriver> {
public:
	/* (the framework's CreateDevice, as the driver uses it) */
	HRESULT STDMETHODCALLTYPE CreateDevice(IWDFDeviceInitialize *,
		IUnknown *cb, IWDFDevice **d) override
	{
		device = new Device();
		cb->AddRef();
		device->callback = cb;
		device->AddRef();
		*d = device;
		return S_OK;
	}
	/* (the framework's CreateWdfObject: not used by the driver) */
	HRESULT STDMETHODCALLTYPE CreateWdfObject(IUnknown *, IWDFObject *,
						  IWDFObject **) override
	{
		return E_NOTIMPL;
	}
	/* (the framework's CreatePreallocatedWdfMemory: not used by the driver) */
	HRESULT STDMETHODCALLTYPE CreatePreallocatedWdfMemory(BYTE *, SIZE_T,
		IUnknown *, IWDFObject *, IWDFMemory **) override
	{
		return E_NOTIMPL;
	}
	/* (the framework's CreateWdfMemory: not used by the driver) */
	HRESULT STDMETHODCALLTYPE CreateWdfMemory(SIZE_T, IUnknown *,
		IWDFObject *, IWDFMemory **) override { return E_NOTIMPL; }
	/* (the framework's IsVersionAvailable: not used by the driver) */
	BOOL STDMETHODCALLTYPE IsVersionAvailable(UMDF_VERSION_DATA *) override
	{
		return TRUE;
	}
	/* (the framework's RetrieveVersionString: not used by the driver) */
	HRESULT STDMETHODCALLTYPE RetrieveVersionString(PWSTR, DWORD *) override
	{
		return E_NOTIMPL;
	}

	Device *device = nullptr;
};

/* dmodemd with args, until the end of the test */
class Daemon {
public:
	/* dmodemd started with args (-n, an account nobody answers) */
	explicit Daemon(const std::string &args)
	{
		std::string cmd = "\"" + beside("dmodemd.exe") + "\" -n -d1 " +
			args;
		STARTUPINFOA si = {};
		si.cb = sizeof(si);
		SetEnvironmentVariableA("SIP_LOGIN", "test:test@127.0.0.1:9");
		SetEnvironmentVariableA("SIP_PORT", "0");
		ok_ = CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE,
				     0, nullptr, nullptr, &si, &pi_);
	}
	/* dmodemd killed, waited for */
	~Daemon()
	{
		if (!ok_)
			return;
		TerminateProcess(pi_.hProcess, 0);
		WaitForSingleObject(pi_.hProcess, 10000);
		CloseHandle(pi_.hProcess);
		CloseHandle(pi_.hThread);
	}
	/* it was started */
	bool ok() const { return ok_; }

private:
	PROCESS_INFORMATION pi_ = {};
	bool ok_ = false;
};

/* until ok() or ms gone: whether ok() */
template <class F> bool within(DWORD ms, F ok)
{
	DWORD t0 = GetTickCount();
	while (!ok()) {
		if (GetTickCount() - t0 > ms)
			return false;
		Sleep(50);
	}
	return true;
}

/* the DLL loaded, the driver's IDriverEntry and its device added; the
   app's side through the device's queue callbacks */
class Host {
public:
	/* (see the class) */
	Host()
	{
		dll = LoadLibraryA(beside("dmodemvm7.dll").c_str());
		if (!dll)
			return;
		get_class = (decltype(get_class))(void *)GetProcAddress(dll,
			"DllGetClassObject");
		can_unload = (decltype(can_unload))(void *)GetProcAddress(dll,
			"DllCanUnloadNow");
	}
	/* (what it holds let go) */
	~Host()
	{
		if (entry)
			entry->Release();
		if (wdf)
			wdf->Release();
		if (dll)
			FreeLibrary(dll);
	}

	/* the driver's entry and its device: S_OK */
	HRESULT add()
	{
		IClassFactory *f = nullptr;
		HRESULT hr = get_class(CLSID_DModemVM7, IID_IClassFactory,
				       (void **)&f);
		if (FAILED(hr))
			return hr;
		hr = f->CreateInstance(nullptr, __uuidof(IDriverEntry),
				       (void **)&entry);
		f->Release();
		if (FAILED(hr))
			return hr;
		wdf = new Driver();
		if (FAILED(hr = entry->OnInitialize(wdf)))
			return hr;
		DeviceInit *init = new DeviceInit();
		hr = entry->OnDeviceAdd(wdf, init);
		locking = init->locking;
		power_owner = init->power_owner;
		init->Release();
		dev = wdf->device;
		return hr;
	}

	/* the device's callback object's interface I (null if it hasn't) */
	template <class I> I *device_cb()
	{
		I *p = nullptr;
		if (dev && dev->callback)
			dev->callback->QueryInterface(__uuidof(I), (void **)&p);
		return p;
	}
	/* the queue's callback object's interface I */
	template <class I> I *queue_cb()
	{
		I *p = nullptr;
		if (dev && dev->queue_cb)
			dev->queue_cb->QueryInterface(__uuidof(I), (void **)&p);
		return p;
	}

	/* a create: its status */
	HRESULT create(File *f)
	{
		auto *cb = queue_cb<IQueueCallbackCreate>();
		Request *r = new Request(WdfRequestCreate, 0, 0);
		cb->OnCreateFile(dev->queue, r, f);
		HRESULT hr = r->wait(5000) ? r->status : E_ABORT;
		r->Release();
		cb->Release();
		return hr;
	}
	/* the handle's cleanup */
	void cleanup(File *f)
	{
		auto *cb = device_cb<IFileCallbackCleanup>();
		cb->OnCleanupFile(f);
		cb->Release();
	}
	/* a read of n (the caller waits, and releases it) */
	Request *read(size_t n, bool cancel_on_mark = false)
	{
		auto *cb = queue_cb<IQueueCallbackRead>();
		Request *r = new Request(WdfRequestRead, 0, n);
		r->cancel_on_mark = cancel_on_mark;
		cb->OnRead(dev->queue, r, n);
		cb->Release();
		return r;
	}
	/* s written: its status and count */
	HRESULT write(const std::string &s, size_t *n)
	{
		auto *cb = queue_cb<IQueueCallbackWrite>();
		Request *r = new Request(WdfRequestWrite, s.size(), 0);
		memcpy(r->input->buf.data(), s.data(), s.size());
		cb->OnWrite(dev->queue, r, s.size());
		cb->Release();
		HRESULT hr = r->wait(5000) ? r->status : E_ABORT;
		*n = r->info;
		r->Release();
		return hr;
	}
	/* an IOCTL (the caller waits, and releases it) */
	Request *ioctl(ULONG code, const void *in, size_t il, size_t ol)
	{
		auto *cb = queue_cb<IQueueCallbackDeviceIoControl>();
		Request *r = new Request(WdfRequestDeviceIoControl, il, ol,
					 code);
		if (il)
			memcpy(r->input->buf.data(), in, il);
		cb->OnDeviceIoControl(dev->queue, r, code, il, ol);
		cb->Release();
		return r;
	}
	/* an IOCTL done at once: its status */
	HRESULT ioctl_now(ULONG code, const void *in, size_t il,
			  void *out = nullptr, size_t ol = 0)
	{
		Request *r = ioctl(code, in, il, ol);
		HRESULT hr = r->wait(5000) ? r->status : E_ABORT;
		if (out && ol)
			memcpy(out, r->output->buf.data(), ol);
		r->Release();
		return hr;
	}
	/* the modem status register */
	uint32_t status()
	{
		uint32_t s = 0;
		ioctl_now(IO_GET_MODEMSTATUS, nullptr, 0, &s, 4);
		return s;
	}
	/* what came within a read's time ("" if nothing) */
	std::string read_some()
	{
		Request *r = read(1024);
		std::string s;
		if (r->wait(5000) && SUCCEEDED(r->status))
			s = r->got();
		else
			r->cancel();
		r->wait(5000);
		r->Release();
		return s;
	}
	/* cmd to the modem, and what it says until want (or ERROR) */
	std::string command(const std::string &cmd, const char *want)
	{
		size_t n = 0;
		if (FAILED(write(cmd + "\r", &n)) || n != cmd.size() + 1)
			return "(write failed)";
		std::string said;
		DWORD t0 = GetTickCount();
		while (GetTickCount() - t0 < 10000) {
			said += read_some();
			if (said.find(want) != std::string::npos ||
			    said.find("ERROR") != std::string::npos)
				break;
		}
		return said;
	}
	/* the hardware's start or release: its HRESULT */
	HRESULT prepare(bool on)
	{
		auto *cb = device_cb<IPnpCallbackHardware>();
		HRESULT hr = on ? cb->OnPrepareHardware(dev) :
			cb->OnReleaseHardware(dev);
		cb->Release();
		return hr;
	}
	/* the framework's references to the driver's objects let go */
	void unload()
	{
		if (dev) {
			if (dev->callback) {
				dev->callback->Release();
				dev->callback = nullptr;
			}
			if (dev->queue_cb) {
				dev->queue_cb->Release();
				dev->queue_cb = nullptr;
			}
		}
		if (entry) {
			entry->OnDeinitialize(wdf);
			entry->Release();
			entry = nullptr;
		}
	}

	HMODULE dll = nullptr;
	HRESULT(STDAPICALLTYPE *get_class)(REFCLSID, REFIID, void **) = nullptr;
	HRESULT(STDAPICALLTYPE *can_unload)() = nullptr;
	IDriverEntry *entry = nullptr;
	Driver *wdf = nullptr;
	Device *dev = nullptr;
	WDF_CALLBACK_CONSTRAINT locking = WdfDeviceLevel;
	BOOL power_owner = FALSE;
};

/* the driver's class factory, its device, the port's names made and
   taken back; nothing of the DLL left */
TEST(Umdf1Driver, Device)
{
	Host h;
	ASSERT_TRUE(h.dll) << "dmodemvm7.dll beside the test";
	ASSERT_TRUE(h.get_class && h.can_unload);
	EXPECT_TRUE(GetProcAddress(h.dll, "Microsoft_WDF_UMDF_Version"));
	ASSERT_EQ(h.add(), S_OK);
	EXPECT_EQ(h.locking, None);
	EXPECT_TRUE(h.power_owner);
	ASSERT_TRUE(h.dev);
	EXPECT_TRUE(IsEqualGUID(h.dev->interface_guid, MODEM_INTERFACE));
	EXPECT_EQ(h.dev->queues, 1);
	EXPECT_TRUE(h.dev->queue_default);
	EXPECT_EQ(h.dev->queue_type, WdfIoQueueDispatchParallel);
	EXPECT_FALSE(h.dev->queue_power);
	EXPECT_TRUE(h.dev->creates_to_queue);
	for (auto *p : { (IUnknown *)h.queue_cb<IQueueCallbackCreate>(),
			 (IUnknown *)h.queue_cb<IQueueCallbackRead>(),
			 (IUnknown *)h.queue_cb<IQueueCallbackWrite>(),
			 (IUnknown *)h.queue_cb<IQueueCallbackDeviceIoControl>(),
			 (IUnknown *)h.device_cb<IPnpCallbackHardware>(),
			 (IUnknown *)h.device_cb<IFileCallbackCleanup>() }) {
		EXPECT_TRUE(p);
		if (p)
			p->Release();
	}

	/* the COM port's names, and their going */
	std::wstring port = port_name();
	h.dev->hw->values.push_back({ L"PortName", port });
	ASSERT_EQ(h.prepare(true), S_OK);
	EXPECT_EQ(h.dev->symlink, L"\\DosDevices\\Global\\" + port);
	EXPECT_EQ(h.dev->serialcomm->get(DEVICE_NAME), port);
	ASSERT_EQ(h.prepare(false), S_OK);
	EXPECT_EQ(h.dev->serialcomm->get(DEVICE_NAME), L"");

	Device *dev = h.dev;
	dev->AddRef();
	h.unload();
	EXPECT_EQ(h.can_unload(), S_OK) << "objects of the DLL left";
	dev->Release();
	h.wdf->device = nullptr;
}

/* AT commands, cancels and WaitCommEvent through the driver, with
   dmodemd on its pipe */
TEST(Umdf1Driver, AtCommands)
{
	Host h;
	ASSERT_TRUE(h.dll);
	ASSERT_EQ(h.add(), S_OK);
	std::wstring port = port_name();
	h.dev->hw->values.push_back({ L"PortName", port });
	ASSERT_EQ(h.prepare(true), S_OK);
	char name[64];
	WideCharToMultiByte(CP_UTF8, 0, port.c_str(), -1, name, sizeof(name),
			    nullptr, nullptr);

	File *f = new File(), *g = new File();
	ASSERT_EQ(h.create(f), S_OK);
	EXPECT_EQ(h.create(g), HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
	{
		Daemon d(std::string("--driver ") + name);
		ASSERT_TRUE(d.ok());
		/* (reads that give what came within 200 ms) */
		SerialTimeouts t = { 0xffffffff, 0xffffffff, 200, 0, 1000 };
		ASSERT_EQ(h.ioctl_now(IO_SET_TIMEOUTS, &t, sizeof(t)), S_OK);
		ASSERT_TRUE(within(60000, [&] {
			return (h.status() & (PMSR_DSR | PMSR_CTS)) ==
				(PMSR_DSR | PMSR_CTS);
		}));
		std::string r = h.command("ATZ", "OK\r\n");
		EXPECT_NE(r.find("OK\r\n"), std::string::npos) << r;
		r = h.command("ATI", "OK\r\n");
		EXPECT_NE(r.find("D-Modem"), std::string::npos) << r;

		/* a read the port holds (no time limit), cancelled */
		SerialTimeouts forever = { 0, 0, 0, 0, 0 };
		ASSERT_EQ(h.ioctl_now(IO_SET_TIMEOUTS, &forever,
				      sizeof(forever)), S_OK);
		Request *rd = h.read(64);
		EXPECT_FALSE(rd->wait(300));
		rd->cancel();
		ASSERT_TRUE(rd->wait(5000));
		EXPECT_EQ(rd->status, HRESULT_FROM_NT(ST_CANCELLED));
		EXPECT_EQ(rd->completions, 1);
		rd->Release();

		/* a read cancelled as it is marked: the port never has it,
		   so the next read has the modem's answer (reads that wait
		   for the first character as long as it takes: one the port
		   had would take the answer) */
		SerialTimeouts gap = { 50, 0, 0, 0, 0 };
		ASSERT_EQ(h.ioctl_now(IO_SET_TIMEOUTS, &gap, sizeof(gap)), S_OK);
		rd = h.read(64, true);
		ASSERT_TRUE(rd->wait(5000));
		EXPECT_EQ(rd->status, HRESULT_FROM_NT(ST_CANCELLED));
		EXPECT_EQ(rd->info, 0u);
		rd->Release();
		r = h.command("AT", "OK\r\n");
		EXPECT_NE(r.find("OK\r\n"), std::string::npos) << r;
		ASSERT_EQ(h.ioctl_now(IO_SET_TIMEOUTS, &t, sizeof(t)), S_OK);

		/* IOCTL_SERIAL_WAIT_ON_MASK: until a character comes */
		uint32_t mask = PEV_RXCHAR;
		ASSERT_EQ(h.ioctl_now(IO_SET_WAIT_MASK, &mask, 4), S_OK);
		Request *w = h.ioctl(IO_WAIT_ON_MASK, nullptr, 0, 4);
		EXPECT_FALSE(w->wait(300));
		size_t n = 0;
		ASSERT_EQ(h.write("AT\r", &n), S_OK);
		ASSERT_TRUE(w->wait(10000));
		EXPECT_EQ(w->status, S_OK);
		ASSERT_EQ(w->info, 4u);
		uint32_t ev;
		memcpy(&ev, w->output->buf.data(), 4);
		EXPECT_TRUE(ev & PEV_RXCHAR);
		w->Release();
		h.read_some();

		/* a wait whose buffer is too small: refused */
		w = h.ioctl(IO_WAIT_ON_MASK, nullptr, 0, 2);
		ASSERT_TRUE(w->wait(5000));
		EXPECT_EQ(w->status, HRESULT_FROM_NT(ST_BUFFER_TOO_SMALL));
		w->Release();
	}
	h.cleanup(f);
	/* (the handle closed: another can open the port) */
	EXPECT_EQ(h.create(g), S_OK);
	h.cleanup(g);
	f->Release();
	g->Release();
	ASSERT_EQ(h.prepare(false), S_OK);

	Device *dev = h.dev;
	dev->AddRef();
	h.unload();
	EXPECT_EQ(h.can_unload(), S_OK) << "objects of the DLL left";
	dev->Release();
	h.wdf->device = nullptr;
}

} // namespace

/* gtest's */
int main(int argc, char **argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
