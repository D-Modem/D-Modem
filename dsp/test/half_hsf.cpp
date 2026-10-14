/*
 *    half_hsf.cpp - Conexant's HSF soft modem (hsfmodem-7.80.02.06oem:
 *    hsfengine, its controller and data pumps, V.92 with PCM upstream;
 *    hsfmc97ich, the Intel ICH back end; hsfsoar, the SmartDAA library;
 *    i386 objects) as a test peer (modem_half.hpp): line audio from fd 3
 *    and to fd 4 (16 bit, HSF_RATE, 8000 by default), AT commands on a
 *    pty whose name is printed ("created hsf: /dev/pts/N"); HSF_SYNC as
 *    the others'.
 *
 *    The objects import nothing of a kernel, only Conexant's OS layer
 *    (OsAllocate, timers, events, port I/O, PCI configuration, NVM_Read
 *    and so on, cnxthsf_7800206oem_*), which is here: one thread, its
 *    time the line's samples.  Timers run when their time comes, sleeps
 *    and waits let the time run.  The modem's serial side is the
 *    engine's ComCtrl interface (as Linuxant's serial_cnxt.c uses it).
 *
 *    Its hardware: an Intel ICH AC'97 modem controller (8086:2416,
 *    half::Ich) with a Conexant modem codec, as half_sm56's, on the port
 *    I/O the module asks the OS layer for.  The codec runs at the rate the
 *    module sets: a windowed sinc resampler between it and the line
 *    (half::CodecLine).
 *
 *    NVM (the driver's static configuration, Linuxant's nvm/ tree) is
 *    read from the package's hsfmc97ich.inf and hsf.cty, as its cvtinf.pl
 *    converts them; dynamic values (country, stored profile) are the
 *    defaults and kept in memory.
 *
 *    The objects, the .inf and the .cty are test/hsf/ as Conexant and
 *    Linuxant shipped them; -DDMODEM_HSF_DIR=<dir> for another copy, or
 *    HSF_NVM=<dir> for the .inf and .cty at run time.  HSF_DEBUG=1 prints
 *    the modem's own messages and the port accesses we do not know, 2
 *    every access.
 */

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <ucontext.h>

#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "modem_half.hpp"

using namespace half;

#define OS(name) cnxthsf_7800206oem_##name

/*
 *    the objects' interface
 */

extern "C" {
int HsfEngineInit(void);
void *GetHwFuncs(void);
void *OS(ComCtrl_Create)(void);
int OS(ComCtrl_Open)(void *h);
int OS(ComCtrl_Configure)(void *h, int code, void *cfg);
int OS(ComCtrl_Control)(void *h, int code, void *ctl);
int OS(ComCtrl_Monitor)(void *h, int code, void *mon);
u32 OS(ComCtrl_Read)(void *h, void *buf, u32 n);
u32 OS(ComCtrl_Write)(void *h, void *buf, u32 n);
}

#define COMCTRL_CONFIG_DEVICE_ID        0
#define COMCTRL_CONFIG_EVENT_HANDLER    1
#define COMCTRL_MONITOR_TXFREE          4
#define COMCTRL_CONTROL_SETRTS          0
#define COMCTRL_CONTROL_SETDTR          2
#define COMCTRL_CONTROL_PORTCONFIG      12

#define HW_TYPE_ICH 2

/* osresour_ex.h's OS_DEVNODE */
struct devnode {
	void *hwDev, *hwDevLink, *hwModule;
	int hwInstNum;
	char hwInstName[50], hwProfile[50], hwRevision[75];
	int hwType;
	void *hwIf;
	int hwInUse, hwSuspendInProgress;
	unsigned long osPageOffset;
	void *pmDev, *pmControl, *hcomctrl;
	u32 *pci_state;
	void *isr, *devid;
};
static_assert(sizeof(devnode) == 236, "OS_DEVNODE");

/* the controller: its I/O BARs, its interrupt */
#define NAMBAR  0xe000
#define NABMBAR 0xe400
#define IRQ     5

/*
 *    NVM: the .inf files, as cvtinf.pl makes files of them and osnvm.c
 *    reads those
 */
class Nvm {
public:
	/* the HKR lines of an .inf or .cty file as cvtinf.pl takes them: OEM
	   values (by their configuration names), Country, Region and Profile
	   ones kept for cty(); continuation lines joined; -1 if file cannot
	   be read */
	int load(const char *file);
	/* the Region and Profile files, as cvtinf.pl writes them (all-zero
	   values left out: the reader goes to the profile then) */
	void cty();
	/* value code into buf, as NVM_Read; value code = size bytes of buf,
	   as NVM_Write */
	int read(int code, void *buf, u32 *size) const;
	int write(int code, const void *buf, u32 size);

private:
	/* a Region or Profile entry: its index, its T35 code, its type, its
	   values (key and text, in the order read) */
	struct CtyEnt {
		bool region;
		std::string idx, t35, type;
		std::vector<std::pair<std::string, std::string>> vals;
	};
	/* the values' text, by their path (as the nvm tree's, under
	   hsfmc97ich/) */
	std::map<std::string, std::string> vals_;
	std::vector<CtyEnt> ents_;      /* in the order found */
	/* the dynamic values (written at run time): in memory */
	std::map<int, std::vector<byte>> dyn_;

	CtyEnt &cty_get(bool region, const std::string &idx);
	bool read_path(const std::string &path, void *buf, u32 *size) const;
	bool cty_parm(int t35, byte ref, const char *name, void *buf,
		      u32 size) const;
	void country(int t35, byte *c) const;
};

/*
 *    the modem: the codec, the controller, the time, the interrupt, the
 *    NVM, the ComCtrl
 */

class Hsf : public HalfModem {
public:
	Hsf();

	Ich ich;
	PciConfig pci;
	X86Traps traps;
	Nvm nvm;

	/* timers: one-shot (kernel timers, called with their data) and
	   periodic (run in the modem's thread, called with their
	   reference); both when the line's time comes to them */
	struct Timer {
		Timer *next;
		bool periodic, armed, busy, dead;
		u32 ms;
		unsigned long long due;
		void (*fn)(void *);
		void *ref;
	};
	Timer *timers = nullptr;

	/* the module's interrupt handler, as Linux calls it, and its
	   device */
	int (*isr)(int irq, void *dev, void *regs) = nullptr;
	void *isr_dev = nullptr;

	/* the time stamp counter, which the engine and the ICH back end read
	   (rdtsc) to follow the codec's clock against the CPU's */
	Tsc tsc;

	Timer *timer_new();
	void timer_free(Timer *t);
	void run_timers();
	void run_time(unsigned ms);
	u32 io_in(unsigned a, int size);
	void io_out(unsigned a, u32 v, int size);

protected:
	int start() override;
	short line_sample(short in) override;
	void dte_poll() override;
	void after_read() override;

private:
	/* the Conexant codec's registers: secondary, 0x80 to 0xff */
	word codec_regs[0x80] = {};
	unsigned codec_rate = 8000;
	/* the SmartDAA's line side registers, behind codec register 0x7a:
	   a write of 0x8000 | reg << 8 asks for one (the answer: reg << 8 |
	   the value), reg << 8 | value writes one */
	byte daa[0x80] = {};
	/* and the line side chip's, behind DAA registers 1 (data) and 3
	   (bit 7 read, the register below it) */
	byte lsd[0x80] = {};
	/* HSF_KEEP8: see interrupt() */
	bool keep8 = false;
	/* the device node of the PCI probe, and the handle that stands for
	   its pci_dev */
	devnode node = {};
	byte pcidev[64] = {};
	void *hcom = nullptr;

	word codec_read(int r);
	void codec_write(int r, word v);
	void dte_out();
	void dte_in();
	void interrupt();
};

static Hsf hsf;
#define LOG(...) do { if (hsf.dbg) hsf.log(__VA_ARGS__); } while (0)

Hsf::Hsf() :
	HalfModem("hsf", "HSF"),
	ich(*this, NAMBAR, NABMBAR, true, 0, true),
	pci(*this)
{
	ich.codec_read = [this](int r) { return codec_read(r); };
	ich.codec_write = [this](int r, word v) { codec_write(r, v); };
	traps.in = [this](unsigned a, int size) { return io_in(a, size); };
	traps.out = [this](unsigned a, u32 v, int size) { io_out(a, v, size); };
	traps.tsc = [this]() { return tsc.read(line_n, line_rate); };
}

/*
 *    the time stamp counter: rdtsc faults (PR_TSC_SIGSEGV) and is done
 *    here, the line's time (half::Tsc)
 */

/* rdtsc: carried out and stepped over */
static void on_segv(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = (ucontext_t *)ctx;
	byte *ip = (byte *)(uintptr_t)uc->uc_mcontext.gregs[REG_EIP];

	if (hsf.traps.step(uc))
		return;
	fprintf(stderr, "hsf: fault at %p (%02x %02x %02x), address %p\n",
		(void *)ip, ip[0], ip[1], ip[2], si->si_addr);
	signal(sig, SIG_DFL);
}

extern "C" {

/* the ICH module's busy wait of us microseconds (hsfmc97ich41_, made
   weak by hsf_obj.cmake): it reads the counter and then reads it again
   until (now - start) / OsGetProcessorFreq() comes to us, the
   microseconds it returns.  Every read traps, and with the line still
   (no samples go by in a wait) each is 0.1 us on: ten traps a
   microsecond, most of the HSF's processor time.  The counter steps
   over the wait at once instead, to where those reads leave it (one at
   least), and the result is the same */
unsigned long long hsfmc97ich41_(u32 us)
{
	unsigned long long n = us ? 10ull * us : 1;  /* the loop's reads */
	hsf.tsc.read(hsf.line_n, hsf.line_rate);
	hsf.tsc.last += 100 * n;
	return 100 * n / 1000;
}

/*
 *    memory, strings, atomics, locks: one thread
 */

void *OS(OsAllocate)(unsigned n) { return calloc(1, n ? n : 1); }
void OS(OsFree)(void *p) { free(p); }
void *OS(OsMemCpy)(void *d, void *s, size_t n) { return memcpy(d, s, n); }
void *OS(OsMemMove)(void *d, void *s, size_t n) { return memmove(d, s, n); }
void *OS(OsMemSet)(void *d, int c, size_t n) { return memset(d, c, n); }
int OS(OsStrCmp)(char *a, char *b) { return strcmp(a, b); }
void *OS(OsStrCpy)(char *d, char *s) { return strcpy(d, s); }
char *OS(OsStrnCpy)(char *d, char *s, size_t n) { return strncpy(d, s, n); }
size_t OS(OsStrLen)(char *s) { return strlen(s); }
int OS(OsToupper)(int c) { return toupper(c); }
int OS(OsIsDigit)(int c) { return isdigit(c); }
/* vsprintf */
int OS(OsSprintf)(char *b, char *f, ...)
{
	va_list ap;
	int n;
	va_start(ap, f);
	n = vsprintf(b, f, ap);
	va_end(ap);
	return n;
}
/* the modem's messages: with HSF_DEBUG */
void OS(OsDebugPrintf)(const char *f, ...)
{
	va_list ap;
	if (!hsf.dbg)
		return;
	va_start(ap, f);
	vfprintf(stderr, f, ap);
	va_end(ap);
}
void OS(OsErrorPrintf)(const char *f, ...)
{
	va_list ap;
	if (!hsf.dbg)
		return;
	fprintf(stderr, "hsf error: ");
	va_start(ap, f);
	vfprintf(stderr, f, ap);
	va_end(ap);
}
void OS(OsDebugBreakpoint)(const char *m) { LOG("breakpoint: %s\n", m); }

/* atomics: plain; the new value */
int OS(OsAtomicAdd)(int a, int *p) { return *p += a; }
int OS(OsAtomicIncrement)(int *p) { return ++*p; }
int OS(OsAtomicDecrement)(int *p) { return --*p; }
/* *p = nw if it is old (pointers of 4 bytes only); whether it was */
int OS(OsAtomicCompareAndSwapEx)(void *old, void *nw, void **p, int size)
{
	if (size == 4 || size == (int)sizeof(void *)) {
		if (*p != old)
			return 0;
		*p = nw;
		return 1;
	}
	LOG("compare and swap size %d\n", size);
	return 0;
}

static int dummy_handle;
/* critical sections and locks: one handle, never contended */
void *OS(OsCriticalSectionCreate)(void) { return &dummy_handle; }
void OS(OsCriticalSectionDestroy)(void *h) { (void)h; }
void OS(OsCriticalSectionAcquire)(void *h) { (void)h; }
void OS(OsCriticalSectionRelease)(void *h) { (void)h; }
void *OS(OsLockCreate)(void) { return &dummy_handle; }
void OS(OsLockDestroy)(void *h) { (void)h; }
void OS(OsLockLock)(void *h) { (void)h; }
void OS(OsLockUnlock)(void *h) { (void)h; }
int OS(OsLockTry)(void *h) { (void)h; return 1; }
void OS(OsLockTryUnlock)(void *h) { (void)h; }
/* one thread; the TSC at 1000 MHz; floating point free to use; the
   module's calls without regparm */
void *OS(OsGetCurrentThread)(void) { return (void *)1; }
u32 OS(OsGetProcessorFreq)(void) { return 1000; }      /* MHz: the TSC's */
int OS(OsFloatPrefix)(void) { return 0; }
int OS(OsFloatSuffix)(int id) { (void)id; return 1; }
int OS(OsKernelUsesRegParm)(void) { return 0; }

/* the data call progress monitor (a daemon playing the line) and the
   diagnostics: none */
void *OS(DcpCreate)(void *dev) { (void)dev; return NULL; }
void OS(DcpDestroy)(void *h) { (void)h; }
void OS(DcpCallback)(void *h, void *p, u32 n) { (void)h; (void)p; (void)n; }
void OS(DcpSetVolume)(void *h, int v) { (void)h; (void)v; }
void *OS(OsDiagMgrOpen)(void *sys, ...) { (void)sys; return NULL; }
int OS(OsDiagMgrClose)(void *h) { (void)h; return 0; }
int OS(OsDiagMgrNotify)(void *h, ...) { (void)h; return 0; }

}

/*
 *    timers
 */

/* a timer, zeroed, on the list */
Hsf::Timer *Hsf::timer_new()
{
	Timer *t = new Timer();
	t->next = timers;
	timers = t;
	return t;
}

/* t off the list, freed */
void Hsf::timer_free(Timer *t)
{
	for (Timer **pp = &timers; *pp; pp = &(*pp)->next)
		if (*pp == t) {
			*pp = t->next;
			break;
		}
	delete t;
}

/* the armed timers due (not those running already) until none is: a
   one-shot disarmed first, a periodic one due again ms after it ran */
void Hsf::run_timers()
{
	bool again = true;

	while (again) {
		again = false;
		for (Timer *t = timers; t; t = t->next) {
			if (!t->armed || t->busy || t->due > ms())
				continue;
			if (!t->periodic)
				t->armed = false;
			t->busy = true;
			stack_clear();
			t->fn(t->ref);
			t->busy = false;
			if (t->dead) {
				timer_free(t);
				again = true;
				break;
			}
			if (t->periodic && t->armed)
				t->due = ms() + (t->ms ? t->ms : 1);
		}
	}
}

/* time with no line: a sleep or a wait of the modem's */
void Hsf::run_time(unsigned ms)
{
	unsigned long long n = (unsigned long long)ms * line_rate / 1000;
	while (n--)
		line_sample(0);
}

extern "C" {

/* a one-shot timer of ms, fn(data) when due; not armed */
void *OS(OsCreateTimer)(u32 ms, void *fn, void *data)
{
	Hsf::Timer *t = hsf.timer_new();
	t->ms = ms;
	t->fn = (void (*)(void *))fn;
	t->ref = data;
	return t;
}
/* armed: due ms from now */
void OS(OsSetTimer)(void *h)
{
	Hsf::Timer *t = (Hsf::Timer *)h;
	t->armed = true;
	t->due = hsf.ms() + t->ms;
}
/* disarmed; its time for the next OsSetTimer */
void OS(OsCancelTimer)(void *h) { ((Hsf::Timer *)h)->armed = false; }
void OS(OsChangeTimerTimeOut)(void *h, u32 ms) { ((Hsf::Timer *)h)->ms = ms; }
/* freed, or once its function returns if it is running */
void OS(OsDestroyTimer)(void *h)
{
	Hsf::Timer *t = (Hsf::Timer *)h;
	if (t->busy)
		t->dead = true;
	else
		hsf.timer_free(t);
}

/* a periodic timer of ms (0: stopped), fn(ref) every ms from now on;
   *thread a dummy */
void *OS(OsCreatePeriodicTimer)(u32 ms, void *fn, void *alloc, void *free_fn,
				void *ref, void **thread)
{
	Hsf::Timer *t = hsf.timer_new();
	(void)alloc; (void)free_fn;
	t->periodic = true;
	t->ms = ms;
	t->fn = (void (*)(void *))fn;
	t->ref = ref;
	t->armed = ms != 0;
	t->due = hsf.ms();
	LOG("periodic timer %p: %u ms\n", (void *)t, ms);
	if (thread)
		*thread = (void *)1;
	return t;
}
/* its period ms (0 stops it), the next ms from now (unless the same
   period); whether it runs */
int OS(OsSetPeriodicTimer)(void *h, u32 ms)
{
	Hsf::Timer *t = (Hsf::Timer *)h;
	if (ms == t->ms)
		return 1;
	LOG("periodic timer %p: %u ms\n", h, ms);
	t->ms = ms;
	t->armed = ms != 0;
	t->due = hsf.ms() + ms;
	return ms != 0;
}
/* due now */
void OS(OsImmediateTimeOut)(void *h)
{
	Hsf::Timer *t = (Hsf::Timer *)h;
	t->armed = true;
	t->due = hsf.ms();
}
/* as OsDestroyTimer */
void OS(OsDestroyPeriodicTimer)(void *h) { OS(OsDestroyTimer)(h); }

/* ms of line time */
u32 OS(OsGetSystemTime)(void) { return (u32)hsf.ms(); }

/* ms of line time run */
void OS(OsSleep)(u32 ms) { hsf.run_time(ms); }

/* events: set by another context, which here is the time that runs while
   one waits */
struct Event { bool flag; };
/* an event: a flag */
void *OS(OsEventCreate)(const char *name) { (void)name; return new Event(); }
void OS(OsEventDestroy)(void *h) { delete (Event *)h; }
long OS(OsEventSet)(void *h) { ((Event *)h)->flag = true; return 0; }
long OS(OsEventClear)(void *h) { ((Event *)h)->flag = false; return 0; }
/* the event, waiting for it a ms of line time at a time, ms at most;
   0 set (and cleared), 1 timeout */
int OS(OsEventWaitTime)(void *h, u32 ms)
{
	Event *e = (Event *)h;
	for (u32 i = 0; !e->flag && i < ms; i++)
		hsf.run_time(1);
	if (!e->flag)
		return 1;       /* OSEVENT_WAIT_TIMEOUT */
	e->flag = false;
	return 0;
}

/*
 *    DMA memory: aligned to 64 kB, its bus address the address itself
 *    (a 32 bit process)
 */

/* 4 kB << order, 64 kB aligned and zeroed; 1, 0 if none */
int OS(OsMemDMAAllocate)(u32 order, void **phys, void **handle, void **buf)
{
	void *p = NULL;
	if (posix_memalign(&p, 65536, 4096u << order))
		return 0;
	memset(p, 0, 4096u << order);
	*buf = p;
	*phys = p;
	if (handle)
		*handle = p;
	return 1;
}
/* and freed */
int OS(OsMemDMAFree)(void *h, u32 order) { (void)order; free(h); return 1; }

}

/*
 *    the codec: a Conexant modem codec, secondary (registers 0x80 to
 *    0xff of the mixer space)
 */

/* codec register r */
word Hsf::codec_read(int r)
{
	r &= 0x7e;
	switch (r) {
	case 0x3e:
		/* ADC/DAC of line 1, MREF, GPIO ready, as written above */
		return (codec_regs[r] & 0xff00) | 0x000f;
	default:
		return codec_regs[r];
	}
}

/* codec register r = v (the IDs read only): 0x7a reads or writes a
   DAA register (DAA register 3 the line side chip's through 1), 0x40
   is the codec's rate */
void Hsf::codec_write(int r, word v)
{
	r &= 0x7e;
	if (r == 0x7c || r == 0x7e || r == 0x3c)
		return;
	if (codec_regs[r] != v && r != 0x7a)
		LOG("codec %02x = %04x\n", r, v);
	if (r == 0x7a) {
		int reg = (v >> 8) & 0x7f;
		if (v & 0x8000) {
			if (dbg > 2)
				LOG("daa read %02x: %02x\n", reg, daa[reg]);
			v = (word)(reg << 8 | daa[reg]);
		} else {
			if (daa[reg] != (byte)v)
				LOG("daa %02x = %02x\n", reg, v & 0xff);
			daa[reg] = (byte)v;
			if (reg == 0x03) {
				int lr = v & 0x7f;
				if (v & 0x80)
					daa[0x01] = lsd[lr];
				else {
					if (lsd[lr] != daa[0x01])
						LOG("line side %02x = %02x\n", lr, daa[0x01]);
					lsd[lr] = daa[0x01];
				}
			}
		}
		codec_regs[r] = v;
		return;
	}
	if (r == 0x40 && v != codec_rate && v) {
		codec_rate = v;
		LOG("codec rate %u Hz\n", v);
	}
	codec_regs[r] = v;
}

/*
 *    the controller: PCI's configuration space, port I/O, the interrupt
 */

/* a port read of size bytes: the secondary codec at NAMBAR + 0x80, the
   bus masters', GLOB_CNT and GLOB_STA at NABMBAR; all ones elsewhere */
u32 Hsf::io_in(unsigned a, int size)
{
	u32 v;
	if (!ich.io_in(a, size, &v)) {
		LOG("in%d %04x\n", size, a);
		v = size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;
	}
	if (dbg > 1)
		fprintf(stderr, "hsf: in%d %04x = %x\n", size, a, v);
	return v;
}

/* a port write of size bytes, as io_in has them */
void Hsf::io_out(unsigned a, u32 v, int size)
{
	if (dbg > 1)
		fprintf(stderr, "hsf: out%d %04x = %x\n", size, a, v);
	if (!ich.io_out(a, v, size))
		LOG("out%d %04x = %x\n", size, a, v);
}

extern "C" {

/* the configuration space of the one device */
void OS(OsPciReadConfigb)(void *d, byte o, byte *v) { (void)d; *v = (byte)hsf.pci.read(o, 1); }
void OS(OsPciReadConfigw)(void *d, byte o, word *v) { (void)d; *v = (word)hsf.pci.read(o, 2); }
void OS(OsPciReadConfigdw)(void *d, byte o, u32 *v) { (void)d; *v = hsf.pci.read(o, 4); }
void OS(OsPciWriteConfigb)(void *d, byte o, byte v) { (void)d; hsf.pci.write(o, v, 1); }
void OS(OsPciWriteConfigw)(void *d, byte o, word v) { (void)d; hsf.pci.write(o, v, 2); }
void OS(OsPciWriteConfigdw)(void *d, byte o, u32 v) { (void)d; hsf.pci.write(o, v, 4); }

struct pci_res { u32 addr, len; };
/* its resources: no memory, NAMBAR and NABMBAR; 1 */
int OS(OsGetPCIDeviceResources)(void *dev, pci_res *mem, pci_res *io1,
				pci_res *io2, int bm)
{
	(void)dev; (void)bm;
	if (mem)
		mem->addr = mem->len = 0;
	if (io1) {
		io1->addr = NAMBAR;
		io1->len = 0x100;
	}
	if (io2) {
		io2->addr = NABMBAR;
		io2->len = 0x80;
	}
	return 1;
}

/* its interrupt handler, for line_sample to call with devid; 1 */
int OS(OsHookInterrupt)(void *dev, void *fn, void *devid)
{
	(void)dev;
	hsf.isr = (int (*)(int, void *, void *))fn;
	hsf.isr_dev = devid;
	return 1;
}
/* and no more */
void OS(OsUnhookInterrupt)(void *dev, void *devid) { (void)dev; (void)devid; hsf.isr = NULL; }

/* the module's port I/O */
byte OS(OsIoPortReadb)(unsigned p) { return (byte)hsf.io_in(p, 1); }
word OS(OsIoPortReadw)(unsigned p) { return (word)hsf.io_in(p, 2); }
u32 OS(OsIoPortReadl)(unsigned p) { return hsf.io_in(p, 4); }
void OS(OsIoPortWriteb)(unsigned p, byte v) { hsf.io_out(p, v, 1); }
void OS(OsIoPortWritew)(unsigned p, word v) { hsf.io_out(p, v, 2); }
void OS(OsIoPortWritel)(unsigned p, u32 v) { hsf.io_out(p, v, 4); }

}

/*
 *    NVM
 */

static const struct { int code; const char *name; } cfg_names[] = {
	{ 0, "MS_PARAMS" },
	{ 1, "EC_PARAMS" },
	{ 2, "BRK_PARAMS" },
	{ 3, "ITF_PARAMS" },
	{ 4, "AUTODETECT" },
	{ 5, "COMPRESSION_PARAMS" },
	{ 6, "V44_PARAMS" },
	{ 7, "RETRAIN_ENABLED" },
	{ 8, "CADENCE_PARAMS" },
	{ 9, "FORCE_RENEG_UP" },
	{ 10, "FORCE_RENEG_DOWN" },
	{ 11, "SREG" },
	{ 12, "RING_COUNTER" },
	{ 13, "ESCAPE_CHAR" },
	{ 14, "CR_CHAR" },
	{ 15, "LF_CHAR" },
	{ 16, "BS_CHAR" },
	{ 17, "BLIND_DIAL_WAIT_TIME" },
	{ 18, "CARRIER_WAIT_TIME" },
	{ 19, "PAUSE_DELAY_TIME" },
	{ 20, "CARDET_RESPONSE_TIME" },
	{ 21, "HANGUP_DELAY_TIME" },
	{ 22, "DTMF_DURATION" },
	{ 23, "ESCAPE_PROMPT_DELAY" },
	{ 29, "TEST_TIMER" },
	{ 40, "FLASH_DELAY_TIME" },
	{ 41, "INACTIVITY_TIME" },
	{ 57, "DATA_COMPRESSION" },
	{ 106, "EXTENDED_RESULT_CODES" },
	{ 266, "SREG_LAST" },
	{ 267, "DIALSTRING" },
	{ 268, "PULSE_DIAL_CONFIG" },
	{ 269, "TONE_DIAL_CONFIG" },
	{ 270, "DIALTONE_WAIT_TIME" },
	{ 271, "DIAL_MODE" },
	{ 272, "USE_S7_WHEN_W" },
	{ 273, "INVERT_CALLING_TONE" },
	{ 274, "CALL_PROGRESS_TONE_DEBOUNCE" },
	{ 275, "CALL_MODE" },
	{ 276, "CALL_PROGRESS_TIMING" },
	{ 277, "CALL_PROGRESS_FLAGS" },
	{ 278, "V8BIS_CONTROL" },
	{ 279, "V8BIS_RESULT" },
	{ 280, "PREFERRED_FLEX_VERSION" },
	{ 281, "V90_ENABLED" },
	{ 282, "V70_ALLOWED" },
	{ 283, "ATMESSAGE_I0" },
	{ 284, "ATMESSAGE_I1" },
	{ 285, "ATMESSAGE_I2" },
	{ 286, "ATMESSAGE_I3" },
	{ 287, "ATMESSAGE_I4" },
	{ 288, "ATMESSAGE_I5" },
	{ 289, "ATMESSAGE_I6" },
	{ 290, "ATMESSAGE_I7" },
	{ 291, "ATMESSAGE_I8" },
	{ 292, "ATMESSAGE_I9" },
	{ 293, "ATMESSAGE_I10" },
	{ 294, "ATMESSAGE_I11" },
	{ 295, "ATMESSAGE_I12" },
	{ 296, "DTE_ECHO" },
	{ 297, "DTE_CONFIG" },
	{ 298, "SYNC_MODE" },
	{ 299, "RLSD_BEHAVIOR" },
	{ 300, "DTR_BEHAVIOR" },
	{ 301, "SPEAKER_VOLUME" },
	{ 302, "SPEAKER_CONTROL" },
	{ 303, "PULSE_MAKE_BREAK" },
	{ 304, "IMMEDIATE_RING_DETECTED" },
	{ 305, "ANSWER_TONE_DETECTOR" },
	{ 306, "BELL_TONE_DETECTOR" },
	{ 307, "DTMF_GENERATOR" },
	{ 308, "MODULATION_REPORT" },
	{ 309, "MODULATION_INFO" },
	{ 310, "BUSY_TONE_CADENCE" },
	{ 311, "RINGBACK_TONE_CADENCE" },
	{ 312, "RING_CADENCE" },
	{ 313, "REORDER_TONE_CADENCE" },
	{ 314, "SDIAL_TONE_CADENCE" },
	{ 315, "DATA_CALL_TONE_CADENCE" },
	{ 316, "FAX_CALL_TONE_CADENCE" },
	{ 317, "FAX_AUTO_ANSWER" },
	{ 318, "OEM_FLAGS" },
	{ 319, "OEM_FLAGS2" },
	{ 320, "OEM_FILTERS" },
	{ 321, "OEM_SPKR_MUTE_DELAY" },
	{ 322, "OEM_READ_MASK" },
	{ 323, "OEM_THRESHOLD" },
	{ 324, "COUNTRY_CODE" },
	{ 325, "PREVIOUS_COUNTRY_CODE" },
	{ 326, "COUNTRY_NAME" },
	{ 327, "COUNTRY_CODE_LIST" },
	{ 328, "COUNTRY_STRUCT" },
	{ 329, "COUNTRY_BLIST" },
	{ 330, "FILTERS" },
	{ 331, "DTMF" },
	{ 332, "RING_PARAMS" },
	{ 333, "RLSDOFFSET" },
	{ 334, "THRESHOLD" },
	{ 335, "TXLEVELS" },
	{ 336, "RELAYS" },
	{ 337, "SPEED_ADJUST" },
	{ 338, "SREG_LIMITS" },
	{ 339, "PR_IGNORE_TIME" },
	{ 340, "USER_SYMRATE" },
	{ 341, "COUNTRY_CALLERID1" },
	{ 342, "COUNTRY_CALLERID2" },
	{ 343, "COUNTRY_CALL_WAITING" },
	{ 344, "VCID_FORMAT" },
	{ 345, "VRID_FORMAT" },
	{ 346, "CALLER_ID_INFO" },
	{ 347, "VRID_CRC" },
	{ 348, "VRID_TYPE" },
	{ 349, "VRID_LENGTH" },
	{ 350, "VRID_DATA" },
	{ 351, "VDR_PARAMS" },
	{ 352, "CALL_WAITING" },
	{ 353, "MOH" },
	{ 354, "PCMUPSTREAM" },
	{ 355, "QC" },
	{ 356, "QC_PROFILE" },
	{ 357, "V92_CONTROL" },
	{ 358, "PROFILE_STORED" },
	{ 359, "PROFILE_FACTORY" },
	{ 360, "POUND_UD" },
	{ 361, "ATPARSER_ONLINE" },
	{ 362, "BLACK_LIST" },
	{ 363, "TONE_PRIMARY_PARAMS" },
	{ 364, "TONE_ALTERNATE_PARAMS" },
	{ 365, "TONE_ATBEL_PARAMS" },
	{ 366, "TONE_ATV25_PARAMS" },
	{ 367, "TONE_1100_PARAMS" },
	{ 368, "TONE_2130_PARAMS" },
	{ 369, "TONE_2312_PARAMS" },
	{ 370, "TONE_2750_PARAMS" },
	{ 371, "TONE_2912_PARAMS" },
	{ 372, "VOICETONEACALLPROG_PARAMS" },
	{ 373, "VOICETONEB_1100_PARAMS" },
	{ 374, "VOICETONEC_1300_PARAMS" },
	{ 375, "VOICETONEC_2100_PARAMS" },
	{ 376, "VOICETONEB_2225_PARAMS" },
	{ 377, "V8BTONE_2100_PARAMS" },
	{ 378, "V8BTONE_2225_PARAMS" },
	{ 381, "CI_CENTER_TONEB_PARAMS" },
	{ 382, "CI_SIDE_TONEC_PARAMS" },
	{ 383, "VOICEVIEW_TONEC_2912_PARAMS" },
	{ 384, "VOICEVIEW_TONEB_2312_PARAMS" },
	{ 385, "VOICEVIEW_TONEB_2750_PARAMS" },
	{ 386, "VOICEVIEW_TONEA_2130_PARAMS" },
	{ 387, "VOICEVIEW_TONEA_NOT_ST_PARAMS" },
	{ 388, "TONEC_1650_PARAMS" },
	{ 390, "TONE_1270_PARAMS" },
	{ 393, "TONE_AS" },
	{ 395, "TONE_GEN_1650" },
	{ 396, "TONE_GEN_V25CALL" },
	{ 397, "TONE_GEN_CNG" },
	{ 398, "TONE_GEN_CED" },
	{ 399, "TONE_GEN_CI" },
	{ 400, "BUFFERS_DELAY" },
	{ 401, "PCI_DEVICE_ID" },
	{ 402, "PCI_VENDOR_ID" },
	{ 403, "OEM_HWDATA" },
	{ 404, "OEM_DAATYPE" },
	{ 405, "OEM_SMARTDAA_PARAMS" },
	{ 406, "OEM_SMARTDAA_PARAMSK3" },
	{ 407, "OEM_SMARTDAA_PARAMSHAL" },
	{ 408, "OEM_SOFT_RING_PARAMS" },
	{ 409, "HW_ADAPTER_TYPE" },
	{ 410, "YUKON_FIRMWARE_DATA" },
	{ 411, "YUKON_FIRMWARE_DELETE" },
	{ 412, "CTRY_TONE" },
	{ 413, "CTRY_CADENCE" },
	{ 414, "CTRY_CALLERID" },
	{ 415, "HSFI_TYPE" },
	{ 416, "CENTERAL_SITE" },
	{ 417, "RXSAMPLES_OUT_FILENAME" },
	{ 418, "TXSAMPLES_OUT_FILENAME" },
	{ 419, "RXLEN_OUT_FILENAME" },
	{ 420, "RXSAMPLES_IN_FILENAME" },
	{ 421, "RXLEN_IN_FILENAME" },
	{ 422, "PRESOURCES" },
	{ 423, "HW_BASE_ADDRESS" },
	{ 424, "DSL_HWIO_PARAMS" },
	{ 425, "SETTINGS_ATUR_DESCRIPTION" },
	{ 426, "SETTINGS_HSTUR_DESCRIPTION" },
	{ 427, "SETTINGS_HSTUC_DESCRIPTION" },
	{ 429, "SETTINGS_ATUR_PARAMS" },
	{ 430, "SETTINGS_HSTUR_PARAMS" },
	{ 431, "SETTINGS_HSTUC_PARAMS" },
	{ 432, "SETTINGS_NILECTRL_PARAMS" },
	{ 433, "SETTINGS_ATUR_DATA" },
	{ 434, "SETTINGS_ATUC_DATA" },
	{ 435, "SETTINGS_HSTUR_DATA" },
	{ 436, "SETTINGS_HSTUC_DATA" },
	{ 437, "SETTINGS_NILECTRL_DATA" },
	{ 438, "G9922ATU_C_CONFIGURATION" },
	{ 439, "G9922ATU_R_CONFIGURATION" },
	{ 440, "HSTU_C_CONFIGURATION" },
	{ 441, "HSTU_R_CONFIGURATION" },
	{ 442, "NILE_CTRL_CONFIGURATION" },
	{ 443, "HS_MODULATION_DB" },
	{ 444, "CO_HS_MODULATION_DB" },
	{ 445, "RT_HS_MODULATION_DB" },
	{ 446, "LOWMIPS" },
	{ 447, "DSL_HWIO_SAMPLE_RATE" },
	{ 448, "HARDWARE_PROFILE" },
	{ 449, "HARDWARE_ID" },
	{ 450, "LICENSE_OWNER" },
	{ 451, "LICENSE_KEY" },
	{ 452, "LICENSE_STATUS" },
	{ 453, "LAST" },
};

#define CFG_COUNTRY_CODE    324
#define CFG_COUNTRY_STRUCT  328
#define CFG_PROFILE_STORED  358
#define CFG_PROFILE_FACTORY 359
#define CFG_OEM_DAATYPE     404

/* the name of NVM value code (configtypes.h's CFGMGR_*), NULL if none */
static const char *cfg_name(int code)
{
	for (const auto &c : cfg_names)
		if (c.code == code)
			return c.name;
	return NULL;
}

/* osnvm.c's NVM_ReadFile on the text: quoted strings as they are, hex
   numbers a byte each; the number of bytes */
static int nvm_bytes(const char *t, byte *out, int max)
{
	int n = 0;
	bool str = false;
	for (const char *p = t; *p && n < max; p++) {
		if (*p == '"') {
			str = !str;
			continue;
		}
		if (str) {
			out[n++] = (byte)*p;
			continue;
		}
		if (*p == ';') {        /* a comment to the end of the line */
			while (p[1] && p[1] != '\n')
				p++;
			continue;       /* (its last character is no byte) */
		}
		if (!isxdigit((byte)*p))
			continue;
		out[n++] = (byte)strtoul(p, (char **)&p, 16);
		p--;
	}
	return n;
}

/* the value's bytes all zero */
static bool nvm_zero(const std::string &t)
{
	std::vector<byte> b(t.size() + 1);
	int n = nvm_bytes(t.c_str(), b.data(), (int)b.size());
	for (int i = 0; i < n; i++)
		if (b[i])
			return false;
	return true;
}

static const char *const oem_keys[][2] = {
	{ "AT+MS", "MS_PARAMS" }, { "SREGS", "SREG" }, { "FLAGS", "OEM_FLAGS" },
	{ "FLAGS2", "OEM_FLAGS2" }, { "FILTER", "OEM_FILTERS" },
	{ "SPKR_MUTE_DELAY", "OEM_SPKR_MUTE_DELAY" }, { "RING", "RING_PARAMS" },
	{ "DEFAULT_CW", "CALL_WAITING" }, { "DEFAULT_MOH", "MOH" },
	{ "DEFAULT_PIG", "PCMUPSTREAM" }, { "BuffersDelay", "BUFFERS_DELAY" },
	{ "HwData", "OEM_HWDATA" }, { "DAAType", "OEM_DAATYPE" },
	{ "SmartDAAParams", "OEM_SMARTDAA_PARAMS" },
	{ "SmartDAAParamsK3", "OEM_SMARTDAA_PARAMSK3" },
	{ "SmartDAAParamsHal", "OEM_SMARTDAA_PARAMSHAL" },
	{ "HwAdapterType", "HW_ADAPTER_TYPE" },
	{ "SOFT_RING_PARAMS", "OEM_SOFT_RING_PARAMS" },
	{ "MISC_VOICE_PARMS", "VOICE_CACHED_MISC_PARAMS" },
};

/* each from in s replaced by to */
static void subst(std::string &s, const char *from, const char *to)
{
	size_t p;
	while ((p = s.find(from)) != std::string::npos)
		s.replace(p, strlen(from), to);
}

/* l ends in a backslash (spaces after it aside): continued */
static bool line_cont(const std::string &l)
{
	size_t n = l.size();
	while (n && isspace((byte)l[n - 1]))
		n--;
	return n && l[n - 1] == '\\';
}

/* the Region (region) or Profile entry idx, made if new */
Nvm::CtyEnt &Nvm::cty_get(bool region, const std::string &idx)
{
	for (auto &e : ents_)
		if (e.region == region && e.idx == idx)
			return e;
	ents_.push_back(CtyEnt());
	ents_.back().region = region;
	ents_.back().idx = idx;
	return ents_.back();
}

int Nvm::load(const char *file)
{
	std::ifstream f(file);
	std::string line;

	if (!f) {
		fprintf(stderr, "hsf: %s: %s\n", file, strerror(errno));
		return -1;
	}
	while (std::getline(f, line)) {
		char sub[128], k[128];
		bool entry = false, region = false;
		std::string key, idx, text;
		size_t at;

		line.resize(strcspn(line.c_str(), "\r\n"));
		if (line.compare(0, 4, "HKR,"))
			continue;
		/* HKR,<subkey>,<key>,<type>,<data> */
		if (sscanf(line.c_str() + 4, "%127[^,],%127[^,],", sub, k) < 2)
			continue;
		key = k;
		while (!key.empty() && isspace((byte)key.back()))
			key.pop_back();
		/* the data: after the third comma past HKR, */
		at = 4;
		for (int n = 0; n < 3 && at != std::string::npos; n++) {
			at = line.find(',', at);
			if (at != std::string::npos)
				at++;
		}
		if (at == std::string::npos)
			at = line.size();
		while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
			at++;
		if (at < line.size() && line[at] == '\\')
			at++;
		text = line.substr(at) + "\n";
		/* continuation lines: while the line ends in a backslash */
		for (bool cont = line_cont(line); cont && std::getline(f, line); ) {
			line.resize(strcspn(line.c_str(), "\r\n"));
			cont = line_cont(line);
			text += line + "\n";
		}
		subst(text, "@CNXTTARGET@", "hsf");
		subst(text, "@CNXTLINUXVERSION@", "7.80.02.06oem");
		subst(text, "@CNXTDRVDSC@", "Conexant HSF softmodem");

		if (!strncmp(sub, "Country\\", 8) || !strncmp(sub, "Region\\", 7) ||
		    !strncmp(sub, "Profile\\", 8)) {
			entry = true;
			region = sub[0] != 'P';
			idx = strchr(sub, '\\') + 1;
		} else if (!strcmp(sub, "OEM")) {
			for (const auto &o : oem_keys)
				if (key == o[0]) {
					key = o[1];
					break;
				}
		} else if (!strncmp(sub, "OEM\\", 4)) {
			int d;
			key = std::string(sub + 4) + "_" + key;
			if (sscanf(key.c_str(), "ATI_%d", &d) == 1)
				key = "ATMESSAGE_I" + std::to_string(d);
		} else
			continue;

		if (!entry) {
			vals_[key] = text;
			continue;
		}
		CtyEnt &e = cty_get(region, idx);
		char t[8];
		if (key == "T35CODE") {
			byte b[2] = { 0, 0 };
			nvm_bytes(text.c_str(), b, 2);
			snprintf(t, sizeof(t), "%02X%02X", b[1], b[0]);
			e.t35 = t;
			continue;
		}
		if (key == "TYPE") {
			byte b = 0;
			nvm_bytes(text.c_str(), &b, 1);
			snprintf(t, sizeof(t), "%02X", b);
			e.type = t;
			continue;
		}
		if (key == "CALLWAITING_PARMS")
			key = "CALL_WAITING";
		e.vals.emplace_back(key, text);
	}
	return 0;
}

void Nvm::cty()
{
	/* (the last found first: of two values for one path, the first
	   found stays) */
	for (auto e = ents_.rbegin(); e != ents_.rend(); ++e) {
		std::string dir = e->region ? "Region/" + e->t35 :
			"Profile/" + e->idx;
		for (auto v = e->vals.rbegin(); v != e->vals.rend(); ++v) {
			if (v->first == "NAME") {
				std::string t = v->second;
				size_t q = t.rfind('"');
				if (e->type == "01" && q != std::string::npos)
					t.insert(q, "*");
				vals_[dir + "_NAME"] = t;
				continue;
			}
			if (v->first != "REFERENCE" && nvm_zero(v->second))
				continue;
			vals_[dir + "/" + v->first] = v->second;
		}
	}
}

/* osnvm.c's NVM_ReadFile on a value: its bytes into buf (*size, the
   rest zeros; *size what there was); false if there is no such value */
bool Nvm::read_path(const std::string &path, void *buf, u32 *size) const
{
	auto v = vals_.find(path);
	if (v == vals_.end())
		return false;
	std::vector<byte> b(v->second.size() + 1);
	u32 n = (u32)nvm_bytes(v->second.c_str(), b.data(), (int)b.size());
	if (n > *size)
		n = *size;
	memcpy(buf, b.data(), n);
	memset((byte *)buf + n, 0, *size - n);
	*size = n;
	return true;
}

static const byte factory_profile[25] = {
	1, 1, 1, 0, 0, 1, 3, 0, 1, 2,
	0, 0, 43, 13, 10, 8, 2, 50, 2, 14, 95, 50, 0, 0, 70
};

/* code is a dynamic value (written at run time), kept in memory */
static bool nvm_dynamic(int code)
{
	switch (code) {
	case CFG_PROFILE_STORED: case 360: case 362: case CFG_COUNTRY_CODE:
	case 325: case 345: case 347: case 348: case 349: case 350: case 356:
	case 402: case 401:
		return true;
	}
	const char *n = cfg_name(code);
	return n && (!strcmp(n, "HARDWARE_PROFILE") ||
		     !strcmp(n, "HARDWARE_ID") ||
		     !strncmp(n, "LICENSE_", 8));
}

/* country t35's value name (size bytes into buf), else its profile
   ref's (0xff: none); whether there was one */
bool Nvm::cty_parm(int t35, byte ref, const char *name, void *buf,
		   u32 size) const
{
	char path[64];
	u32 s = size;
	snprintf(path, sizeof(path), "Region/%04X%s", t35, name);
	if (read_path(path, buf, &s))
		return true;
	if (ref == 0xff)
		return false;
	snprintf(path, sizeof(path), "Profile/%04X%s", ref, name);
	s = size;
	return read_path(path, buf, &s);
}

/* osnvm.c's NVM_ReadCountry: configtypes.h's CtryPrmsStruct (929 bytes) */
void Nvm::country(int t35, byte *c) const
{
	static const struct { const char *name; int off, size; } f[] = {
		{ "_NAME", 2, 80 }, { "/INTCODE", 82, 80 },
		{ "/SMART_TXLEVEL", 162, 13 }, { "/SMART_RELAYS", 175, 48 },
		{ "/PULSE", 223, 13 }, { "/RING", 236, 17 },
		{ "/SREG", 253, 40 }, { "/SMART_DTMF", 293, 4 },
		{ "/FILTER", 297, 162 }, { "/SMART_THRESHOLD", 459, 20 },
		{ "/RLSD", 479, 4 }, { "/TONE", 483, 11 },
		{ "/TIMING", 494, 12 }, { "/CADENCE", 506, 145 },
		{ "/BLACKLISTING", 651, 19 }, { "/CALLERID", 670, 6 },
		{ "/CALLERID2", 676, 53 }, { "/FLAGS", 729, 4 },
		{ "/SPEEDADJUST", 733, 3 }, { "/CALL_WAITING", 736, 125 },
		{ "/V92_CONTROL", 861, 4 }, { "/V92_OGC", 865, 64 },
	};
	byte ref = 0xff;

	c[0] = (byte)t35;
	c[1] = (byte)(t35 >> 8);
	cty_parm(t35, 0xff, "/REFERENCE", &ref, 1);
	for (const auto &p : f)
		if (!cty_parm(t35, ref, p.name, c + p.off, p.size))
			LOG("nvm: country %04x: no %s\n", t35, p.name);
	LOG("nvm: country %04x, profile %02x\n", t35, ref);
}

/* value code into buf (*size bytes, the rest zeros; *size what there
   was): the country structure (the T35 code in size), the dynamic
   values (the USA and the factory profile by default), the .inf's;
   0, 7 if none */
int Nvm::read(int code, void *buf, u32 *size) const
{
	const char *name;
	if (code == CFG_COUNTRY_STRUCT) {
		/* (the country's T35 code in place of the size) */
		memset(buf, 0, 929);
		country((int)(uintptr_t)size, (byte *)buf);
		return 0;
	}
	if (nvm_dynamic(code)) {
		auto d = dyn_.find(code);
		if (d != dyn_.end()) {
			u32 n = d->second.size() < *size ? (u32)d->second.size() : *size;
			memcpy(buf, d->second.data(), n);
			memset((byte *)buf + n, 0, *size - n);
			*size = n;
			return 0;
		}
		if (code == CFG_COUNTRY_CODE && *size == 2) {
			((byte *)buf)[0] = 0xb5;        /* USA */
			((byte *)buf)[1] = 0;
			return 0;
		}
		if (code == CFG_PROFILE_STORED && *size == sizeof(factory_profile)) {
			memcpy(buf, factory_profile, sizeof(factory_profile));
			return 0;
		}
		LOG("nvm: no %s (%d)\n", cfg_name(code) ? cfg_name(code) : "?", code);
		return 7;       /* COM_STATUS_VALUE_NOT_FOUND */
	}
	name = cfg_name(code);
	if (name && read_path(name, buf, size))
		return 0;
	if (code == CFG_PROFILE_FACTORY && *size == sizeof(factory_profile)) {
		memcpy(buf, factory_profile, sizeof(factory_profile));
		return 0;
	}
	if (hsf.dbg > 1)
		LOG("nvm: no %s (%d)\n", name ? name : "?", code);
	memset(buf, 0, *size);
	*size = 0;
	return 7;
}

int Nvm::write(int code, const void *buf, u32 size)
{
	dyn_[code].assign((const byte *)buf, (const byte *)buf + size);
	return 0;
}

extern "C" {

/* the NVM: no handle of its own */
void *OS(NVM_Open)(unsigned long dev) { return (void *)dev; }
int OS(NVM_Close)(void *h) { (void)h; return 0; }
/* value code into buf, *size bytes (Nvm::read) */
int OS(NVM_Read)(void *h, int code, void *buf, u32 *size)
{
	(void)h;
	return hsf.nvm.read(code, buf, size);
}
/* value code = *size bytes of buf, in memory; 0 */
int OS(NVM_Write)(void *h, int code, void *buf, u32 *size)
{
	(void)h;
	return hsf.nvm.write(code, buf, *size);
}

}

/*
 *    the DTE: the engine's ComCtrl
 */

/* the modem's output (ComCtrl_Read) to the pty, all there is */
void Hsf::dte_out()
{
	byte buf[1024];
	if (!hcom)
		return;
	pty.flush();
	for (;;) {
		stack_clear();
		u32 n = OS(ComCtrl_Read)(hcom, buf, sizeof(buf));
		if (n == 0 || n > sizeof(buf))
			break;
		results.scan(buf, n);
		pty.write(buf, n);
	}
}

/* the DTE's input, as much as the modem takes; only commands need the
   fixed timing, so data (online) goes in at once */
void Hsf::dte_in()
{
	u32 room = 0;

	if (!hcom || dte_waits())
		return;
	stack_clear();
	if (OS(ComCtrl_Monitor)(hcom, COMCTRL_MONITOR_TXFREE, &room))
		return;
	pty.read(room > 4096 ? 4096 : (int)room, [this](const void *p, int n) {
		stack_clear();
		return (int)OS(ComCtrl_Write)(hcom, (void *)p, (u32)n);
	});
}

/* every 48 line samples: the modem's output, then the DTE's input */
void Hsf::dte_poll()
{
	dte_out();
	dte_in();
}

/* after each read of the line: the DTE's input (without HSF_SYNC), the
   modem's output */
void Hsf::after_read()
{
	if (!sync_n)
		dte_in();
	dte_out();
}

/* ComCtrl's events (the serial driver's): logged only */
static void event(void *ref, u32 mask)
{
	(void)ref;
	if (hsf.dbg > 1)
		LOG("event %x\n", mask);
}

/*
 *    the line
 */

/* the module's interrupt.  Its flag at 0x280 of its device (the engine
   sets it, 2) makes its playback position 8 samples further on every
   interrupt past sample 700 of the ring: some other controller's, not the
   ICH's, whose position is right, and the engine runs 8 samples a buffer
   ahead of it until it laps the ring (every 3.3 s, a buffer lost); so it
   is cleared (HSF_KEEP8 keeps it) */
void Hsf::interrupt()
{
	if (!keep8)
		*(u32 *)((byte *)isr_dev + 0x280) = 0;
	stack_clear();
	isr(IRQ, isr_dev, NULL);
}

/* one line sample each way through the resamplers (a codec sample each
   way through the bus masters); the ISR while there is an interrupt;
   the timers due each ms */
short Hsf::line_sample(short in)
{
	unsigned long long ms0 = ms();
	short out = codec.sample(in, (double)line_rate / codec_rate,
				 [this](short adc) {
		short dac = ich.sample(ich.po, 0);
		/* DAA register 0x0e, bits 5 and 6: its loopbacks, which
		   the SmartDAA library measures the path's delay by */
		if (daa[0x0e] & 0x60)
			adc = dac;
		ich.sample(ich.pi, adc);
		return dac;
	});

	line_n++;
	/* (level triggered, as PCI's; and with a latency: the module takes
	   a PICB of a whole buffer for one that has not moved yet, and
	   reads it again and again, then guesses) */
	if (isr && ich.irq() && !ich.at_start(ich.pi) && !ich.at_start(ich.po)) {
		int guard = 0;
		while (ich.irq() && guard++ < 4)
			interrupt();
	}
	if (ms() != ms0)
		run_timers();
	return out;
}

/* the rdtsc trap, the NVM, the devices, the engine's init and the
   serial driver's open (115200 8N1, DTR, RTS) */
int Hsf::start()
{
	/* (the engine keeps them) */
	static struct { void *ref; void (*fn)(void *, u32); } evh = { NULL, event };
	static struct {
		u32 valid, speed;
		int parity, bits, xout, xin, err, null, cts, rts;
		char xon, xoff, errc, pealt;
	} pc;
	struct sigaction sa;
	const char *dir = env("NVM") ? env("NVM") : HSF_NVM_DIR;
	char path[512];
	int r;

	keep8 = env("KEEP8") != nullptr;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_segv;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	if (prctl(PR_SET_TSC, PR_TSC_SIGSEGV, 0, 0, 0)) {
		perror("hsf: PR_SET_TSC");
		return -1;
	}

	snprintf(path, sizeof(path), "%s/hsfmc97ich.inf", dir);
	if (nvm.load(path))
		return -1;
	snprintf(path, sizeof(path), "%s/hsf.cty", dir);
	if (nvm.load(path))
		return -1;
	nvm.cty();

	/* the Conexant codec's registers at reset: its IDs, line 1, 8000
	   Hz */
	codec_regs[0x3c] = 0x4001;      /* extended modem ID: line 1, ID 01 */
	codec_regs[0x40] = 8000;
	codec_regs[0x7c] = 0x4358;      /* "CX" */
	codec_regs[0x7e] = 0x5429;      /* "T)" */

	/* the controller's configuration space: its IDs, I/O and bus master
	   on, the modem class, the BARs (256 and 128 octets of I/O), the
	   IRQ */
	pci.cfg[0x00 / 4] = 0x8086 | 0x2416u << 16;
	pci.cfg[0x04 / 4] = 0x02800005;         /* I/O, bus master */
	pci.cfg[0x08 / 4] = 0x07030002;         /* modem */
	pci.cfg[0x10 / 4] = NAMBAR | 1;
	pci.cfg[0x14 / 4] = NABMBAR | 1;
	pci.cfg[0x2c / 4] = 0x8086 | 0x2416u << 16;
	pci.cfg[0x3c / 4] = IRQ | 2 << 8;
	pci.bar_size[0] = 0x100;
	pci.bar_size[1] = 0x80;

	/* the modules' init, the PCI probe's device node, the serial
	   driver's open (serial_cnxt.c) */
	stack_clear();
	r = HsfEngineInit();
	if (r) {
		fprintf(stderr, "hsf: HsfEngineInit: %d\n", r);
		return -1;
	}
	node.hwDev = pcidev;
	node.hwInstNum = 0;
	snprintf(node.hwInstName, sizeof(node.hwInstName),
		 "PCI-8086:2416-8086:2416");
	snprintf(node.hwProfile, sizeof(node.hwProfile), "hsfmc97ich");
	node.hwType = HW_TYPE_ICH;
	node.hwIf = GetHwFuncs();
	stack_clear();
	hcom = OS(ComCtrl_Create)();
	if (!hcom) {
		fprintf(stderr, "hsf: ComCtrl_Create failed\n");
		return -1;
	}
	node.hcomctrl = hcom;
	if ((r = OS(ComCtrl_Configure)(hcom, COMCTRL_CONFIG_DEVICE_ID, &node)) ||
	    (r = OS(ComCtrl_Configure)(hcom, COMCTRL_CONFIG_EVENT_HANDLER, &evh))) {
		fprintf(stderr, "hsf: ComCtrl_Configure: %d\n", r);
		return -1;
	}
	stack_clear();
	r = OS(ComCtrl_Open)(hcom);
	if (r) {
		fprintf(stderr, "hsf: ComCtrl_Open: %d\n", r);
		return -1;
	}
	pc.valid = 0x00000001 | 0x10 | 0x20 | 0x1000 | 0x2000;
	pc.speed = 115200;
	pc.bits = 1;            /* 8 */
	stack_clear();
	OS(ComCtrl_Control)(hcom, COMCTRL_CONTROL_PORTCONFIG, &pc);
	OS(ComCtrl_Control)(hcom, COMCTRL_CONTROL_SETDTR, NULL);
	OS(ComCtrl_Control)(hcom, COMCTRL_CONTROL_SETRTS, NULL);
	return 0;
}

int main()
{
	return hsf.run();
}
