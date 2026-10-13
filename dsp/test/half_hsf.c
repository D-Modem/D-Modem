/*
 *    half_hsf.c - Conexant's HSF soft modem (hsfmodem-7.80.02.06oem:
 *    hsfengine, its controller and data pumps, V.92 with PCM upstream;
 *    hsfmc97ich, the Intel ICH back end; hsfsoar, the SmartDAA library;
 *    i386 objects) as a test peer, as half_sm56 does for Motorola's: line
 *    audio from fd 3 and to fd 4 (16 bit, HSF_RATE, 8000 by default), AT
 *    commands on a pty whose name is printed ("created hsf: /dev/pts/N");
 *    HSF_SYNC as half_sm56's SM56_SYNC.
 *
 *    The objects import nothing of a kernel, only Conexant's OS layer
 *    (OsAllocate, timers, events, port I/O, PCI configuration, NVM_Read
 *    and so on, cnxthsf_7800206oem_*), which is here: one thread, its
 *    time the line's samples.  Timers run when their time comes, sleeps
 *    and waits let the time run.  The modem's serial side is the
 *    engine's ComCtrl interface (as Linuxant's serial_cnxt.c uses it).
 *
 *    Its hardware: an Intel ICH AC'97 modem controller (8086:2416) with a
 *    Conexant modem codec, as half_sm56's, on the port I/O the module asks
 *    the OS layer for.  The codec runs at the rate the module sets: a
 *    windowed sinc resampler between it and the line.
 *
 *    NVM (the driver's static configuration, Linuxant's nvm/ tree) is
 *    read from the package's hsfmc97ich.inf and hsf.cty, as its cvtinf.pl
 *    converts them; dynamic values (country, stored profile) are the
 *    defaults and kept in memory.
 *
 *    The objects, the .inf and the .cty are test/hsf/ as Conexant and
 *    Linuxant shipped them; -DDMODEM_HSF_DIR=<dir> for another copy.
 *    HSF_DEBUG=1 prints the modem's own messages and the port accesses we
 *    do not know, 2 every access.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <termios.h>
#include <ucontext.h>
#include <unistd.h>

typedef unsigned char byte;
typedef unsigned short word;
typedef unsigned int u32;

static int dbg;
#define LOG(fmt, ...) do { if (dbg) fprintf(stderr, "hsf: " fmt, ##__VA_ARGS__); } while (0)

#define OS(name) cnxthsf_7800206oem_##name

/*
 *    the objects' interface
 */

extern int HsfEngineInit(void);
extern void *GetHwFuncs(void);
extern void *OS(ComCtrl_Create)(void);
extern int OS(ComCtrl_Open)(void *h);
extern int OS(ComCtrl_Configure)(void *h, int code, void *cfg);
extern int OS(ComCtrl_Control)(void *h, int code, void *ctl);
extern int OS(ComCtrl_Monitor)(void *h, int code, void *mon);
extern u32 OS(ComCtrl_Read)(void *h, void *buf, u32 n);
extern u32 OS(ComCtrl_Write)(void *h, void *buf, u32 n);

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
_Static_assert(sizeof(struct devnode) == 236, "OS_DEVNODE");

static struct devnode node;
static byte pcidev[64];         /* the handle that stands for the pci_dev */

/*
 *    time: the line's samples
 */

static int line_rate = 8000;
static unsigned long long line_n;       /* line samples so far */
/* ms of line time */
static unsigned long long now_ms(void) { return line_n * 1000 / line_rate; }
static void run_time(unsigned ms);

/*
 *    the time stamp counter: the engine and the ICH back end read it
 *    (rdtsc) to follow the codec's clock against the CPU's; theirs is the
 *    line's time, at 1 GHz (rdtsc faults, PR_TSC_SIGSEGV, and is done
 *    here).  Monotonic, and on by a little each read
 */

static unsigned long long tsc_last;

static void on_segv(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = ctx;
	greg_t *g = uc->uc_mcontext.gregs;
	byte *ip = (byte *)(uintptr_t)g[REG_EIP];
	unsigned long long t;

	if (ip[0] == 0x0f && ip[1] == 0x31) {
		/* (and on by 0.1 us a read: busy waits end) */
		t = line_n * (1000000000ull / line_rate);
		if (t <= tsc_last)
			t = tsc_last + 100;
		tsc_last = t;
		g[REG_EAX] = (greg_t)(u32)t;
		g[REG_EDX] = (greg_t)(u32)(t >> 32);
		g[REG_EIP] += 2;
		return;
	}
	(void)si;
	signal(sig, SIG_DFL);
}

/* rdtsc to fault into on_segv (PR_TSC_SIGSEGV); prctl's result */
static int tsc_init(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_segv;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	return prctl(PR_SET_TSC, PR_TSC_SIGSEGV);
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
	if (!dbg)
		return;
	va_start(ap, f);
	vfprintf(stderr, f, ap);
	va_end(ap);
}
void OS(OsErrorPrintf)(const char *f, ...)
{
	va_list ap;
	if (!dbg)
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

/*
 *    timers: one-shot (kernel timers, called with their data) and
 *    periodic (run in the modem's thread, called with their reference);
 *    both when the line's time comes to them
 */

struct timer {
	struct timer *next;
	int periodic, armed, busy, dead;
	u32 ms;
	unsigned long long due;
	void (*fn)(void *);
	void *ref;
	void (*free_fn)(void *, void *);
};
static struct timer *timers;

/* a timer, zeroed, on the list */
static struct timer *timer_new(void)
{
	struct timer *t = calloc(1, sizeof(*t));
	t->next = timers;
	timers = t;
	return t;
}

/* t off the list, freed */
static void timer_free(struct timer *t)
{
	struct timer **pp;
	for (pp = &timers; *pp; pp = &(*pp)->next)
		if (*pp == t) {
			*pp = t->next;
			break;
		}
	free(t);
}

/* a one-shot timer of ms, fn(data) when due; not armed */
void *OS(OsCreateTimer)(u32 ms, void *fn, void *data)
{
	struct timer *t = timer_new();
	t->ms = ms;
	t->fn = (void (*)(void *))fn;
	t->ref = data;
	return t;
}
/* armed: due ms from now */
void OS(OsSetTimer)(void *h)
{
	struct timer *t = h;
	t->armed = 1;
	t->due = now_ms() + t->ms;
}
/* disarmed; its time for the next OsSetTimer */
void OS(OsCancelTimer)(void *h) { ((struct timer *)h)->armed = 0; }
void OS(OsChangeTimerTimeOut)(void *h, u32 ms) { ((struct timer *)h)->ms = ms; }
/* freed, or once its function returns if it is running */
void OS(OsDestroyTimer)(void *h)
{
	struct timer *t = h;
	if (t->busy)
		t->dead = 1;
	else
		timer_free(t);
}

/* a periodic timer of ms (0: stopped), fn(ref) every ms from now on;
   *thread a dummy */
void *OS(OsCreatePeriodicTimer)(u32 ms, void *fn, void *alloc, void *free_fn,
				void *ref, void **thread)
{
	struct timer *t = timer_new();
	(void)alloc;
	t->periodic = 1;
	t->ms = ms;
	t->fn = (void (*)(void *))fn;
	t->ref = ref;
	t->armed = ms != 0;
	t->due = now_ms();
	(void)free_fn;
	LOG("periodic timer %p: %u ms\n", (void *)t, ms);
	if (thread)
		*thread = (void *)1;
	return t;
}
/* its period ms (0 stops it), the next ms from now (unless the same
   period); whether it runs */
int OS(OsSetPeriodicTimer)(void *h, u32 ms)
{
	struct timer *t = h;
	if (ms == t->ms)
		return 1;
	LOG("periodic timer %p: %u ms\n", h, ms);
	t->ms = ms;
	t->armed = ms != 0;
	t->due = now_ms() + ms;
	return ms != 0;
}
/* due now */
void OS(OsImmediateTimeOut)(void *h)
{
	struct timer *t = h;
	t->armed = 1;
	t->due = now_ms();
}
/* as OsDestroyTimer */
void OS(OsDestroyPeriodicTimer)(void *h) { OS(OsDestroyTimer)(h); }

/* ms of line time */
u32 OS(OsGetSystemTime)(void) { return (u32)now_ms(); }

/* the armed timers due (not those running already) until none is: a
   one-shot disarmed first, a periodic one due again ms after it ran */
static void run_timers(void)
{
	struct timer *t;
	int again = 1;

	while (again) {
		again = 0;
		for (t = timers; t; t = t->next) {
			if (!t->armed || t->busy || t->due > now_ms())
				continue;
			if (!t->periodic)
				t->armed = 0;
			t->busy = 1;
			t->fn(t->ref);
			t->busy = 0;
			if (t->dead) {
				timer_free(t);
				again = 1;
				break;
			}
			if (t->periodic && t->armed)
				t->due = now_ms() + (t->ms ? t->ms : 1);
		}
	}
}

/* ms of line time run */
void OS(OsSleep)(u32 ms) { run_time(ms); }

/* events: set by another context, which here is the time that runs while
   one waits */
struct event { int flag; };
/* an event: a flag */
void *OS(OsEventCreate)(const char *name) { (void)name; return calloc(1, sizeof(struct event)); }
void OS(OsEventDestroy)(void *h) { free(h); }
long OS(OsEventSet)(void *h) { ((struct event *)h)->flag = 1; return 0; }
long OS(OsEventClear)(void *h) { ((struct event *)h)->flag = 0; return 0; }
/* the event, waiting for it a ms of line time at a time, ms at most;
   0 set (and cleared), 1 timeout */
int OS(OsEventWaitTime)(void *h, u32 ms)
{
	struct event *e = h;
	u32 i;
	for (i = 0; !e->flag && i < ms; i++)
		run_time(1);
	if (!e->flag)
		return 1;       /* OSEVENT_WAIT_TIMEOUT */
	e->flag = 0;
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

/*
 *    the codec: a Conexant modem codec, secondary (registers 0x80 to
 *    0xff of the mixer space)
 */

static word codec[0x80];
static unsigned codec_rate = 8000;

/* the SmartDAA's line side registers, behind codec register 0x7a: a
   write of 0x8000 | reg << 8 asks for one (the answer: reg << 8 | the
   value), reg << 8 | value writes one */
static byte daa[0x80];

/* and the line side chip's, behind DAA registers 1 (data) and 3 (bit 7
   read, the register below it) */
static byte lsd[0x80];

/* the Conexant codec's registers at reset: its IDs, line 1, 8000 Hz */
static void codec_init(void)
{
	memset(codec, 0, sizeof(codec));
	codec[0x3c] = 0x4001;           /* extended modem ID: line 1, ID 01 */
	codec[0x40] = 8000;
	codec[0x7c] = 0x4358;           /* "CX" */
	codec[0x7e] = 0x5429;           /* "T)" */
}

/* codec register r */
static word codec_read(int r)
{
	r &= 0x7e;
	switch (r) {
	case 0x3e:
		/* ADC/DAC of line 1, MREF, GPIO ready, as written above */
		return (codec[r] & 0xff00) | 0x000f;
	default:
		return codec[r];
	}
}

/* codec register r = v (the IDs read only): 0x7a reads or writes a
   DAA register (DAA register 3 the line side chip's through 1), 0x40
   is the codec's rate */
static void codec_write(int r, word v)
{
	r &= 0x7e;
	if (r == 0x7c || r == 0x7e || r == 0x3c)
		return;
	if (codec[r] != v && r != 0x7a)
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
		codec[r] = v;
		return;
	}
	if (r == 0x40 && v != codec_rate && v) {
		codec_rate = v;
		LOG("codec rate %u Hz\n", v);
	}
	codec[r] = v;
}

/*
 *    the ICH AC'97 modem controller (8086:2416): NAMBAR (the codecs: 0x00
 *    primary, 0x80 secondary) and NABMBAR (the bus master), as half_sm56's
 */

#define NAMBAR  0xe000
#define NABMBAR 0xe400
#define IRQ     5

struct bm {
	u32 bdbar;
	byte civ, lvi, cr;
	word sr, picb;
	int pos;
	int fresh;      /* after a reset: the first buffer to be loaded */
};
static struct bm pi, po;
static u32 glob_cnt, glob_sta = 0x300;  /* primary and secondary ready */

#define SR_DCH   0x01
#define SR_CELV  0x02
#define SR_LVBCI 0x04
#define SR_BCIS  0x08
#define SR_FIFOE 0x10
#define CR_RPBM  0x01
#define CR_RR    0x02
#define CR_LVBIE 0x04
#define CR_FEIE  0x08
#define CR_IOCE  0x10

/* buffer descriptor i of b's list: the buffer's address, its length
   in samples | IOC (bit 31) */
static u32 *bd(struct bm *b, int i)
{
	return (u32 *)(uintptr_t)(b->bdbar + 8 * (i & 31));
}

/* the current descriptor's length into PICB, from its start */
static void bm_load(struct bm *b)
{
	b->picb = (word)(bd(b, b->civ)[1] & 0xffff);
	b->pos = 0;
}

/* a write of the control register: reset (RR), run (RPBM: on where it
   was after a pause, from the current descriptor after a reset),
   stop (DCH set) */
static void bm_cr(struct bm *b, byte v)
{
	if (dbg > 1)
		LOG("%s cr %02x\n", b == &pi ? "pi" : "po", v);
	if (v & CR_RR) {
		b->civ = b->lvi = 0;
		b->picb = 0;
		b->pos = 0;
		b->sr = SR_DCH;
		b->cr = 0;
		b->fresh = 1;
		return;
	}
	/* run after a pause (the module pauses the bus master to move its
	   LVI on): where it was */
	if ((v & CR_RPBM) && !(b->cr & CR_RPBM) && !b->fresh) {
		b->sr &= ~SR_DCH;
		if (b->picb == 0 && b->civ != b->lvi) {
			b->civ = (b->civ + 1) & 31;
			b->sr &= ~SR_CELV;
			bm_load(b);
		}
		b->cr = v & 0x1f;
		return;
	}
	if ((v & CR_RPBM) && !(b->cr & CR_RPBM)) {
		b->fresh = 0;
		if (dbg) {
			int i;
			LOG("%s runs: lvi %u:", b == &pi ? "pcm in" : "pcm out", b->lvi);
			for (i = 0; i < 32 && b->bdbar; i++)
				if (bd(b, i)[1] & 0xffff)
					fprintf(stderr, " %x", bd(b, i)[1]);
			fprintf(stderr, "\n");
		}
		b->sr &= ~(SR_DCH | SR_CELV);
		if (b->bdbar)
			bm_load(b);
	}
	if (!(v & CR_RPBM))
		b->sr |= SR_DCH;
	b->cr = v & 0x1f;
}

/* b's interrupt: a status bit whose enable is set */
static int bm_irq(const struct bm *b)
{
	return ((b->sr & SR_BCIS) && (b->cr & CR_IOCE)) ||
		((b->sr & SR_LVBCI) && (b->cr & CR_LVBIE)) ||
		((b->sr & SR_FIFOE) && (b->cr & CR_FEIE));
}

/* GLOB_STA, its PCM in and out interrupt bits as the bus masters are */
static u32 glob_status(void)
{
	return (glob_sta & ~6u) | (bm_irq(&pi) ? 2 : 0) | (bm_irq(&po) ? 4 : 0);
}

/* a running bus master at the start of a buffer: none of it played */
static int bm_fresh(struct bm *b)
{
	return (b->cr & CR_RPBM) && !(b->sr & SR_DCH) && b->bdbar &&
		b->picb == (bd(b, b->civ)[1] & 0xffff);
}

/* a codec sample through a running bus master */
static short bm_sample(struct bm *b, short in)
{
	short out = 0;
	u32 *d;
	short *buf;

	if ((b->cr & CR_RPBM) && (b->sr & SR_CELV) && b->civ != b->lvi) {
		/* halted at the last valid buffer, and there is another */
		b->civ = (b->civ + 1) & 31;
		b->sr &= ~(SR_CELV | SR_DCH);
		bm_load(b);
	}
	if (!(b->cr & CR_RPBM) || (b->sr & SR_DCH) || !b->bdbar)
		return 0;
	d = bd(b, b->civ);
	buf = (short *)(uintptr_t)d[0];
	if (b->pos < (int)(d[1] & 0xffff) && d[0]) {
		if (b == &pi)
			buf[b->pos] = in;
		else
			out = buf[b->pos];
	}
	b->pos++;
	if (b->picb)
		b->picb--;
	if (b->picb == 0) {
		if (d[1] & 0x80000000)
			b->sr |= SR_BCIS;
		if (b->civ == b->lvi) {
			if (dbg)
				LOG("%s halted at %u ms (civ %u)\n", b == &pi ?
				    "pcm in" : "pcm out", (unsigned)now_ms(), b->civ);
			b->sr |= SR_LVBCI | SR_CELV | SR_DCH;
		} else {
			b->civ = (b->civ + 1) & 31;
			bm_load(b);
		}
	}
	return out;
}

/* PCI configuration space */
static u32 cfg[64];

/* the controller's configuration space: its IDs, I/O and bus master
   on, the modem class, the BARs, the IRQ */
static void cfg_init(void)
{
	cfg[0x00 / 4] = 0x8086 | 0x2416u << 16;
	cfg[0x04 / 4] = 0x02800005;             /* I/O, bus master */
	cfg[0x08 / 4] = 0x07030002;             /* modem */
	cfg[0x10 / 4] = NAMBAR | 1;
	cfg[0x14 / 4] = NABMBAR | 1;
	cfg[0x2c / 4] = 0x8086 | 0x2416u << 16;
	cfg[0x3c / 4] = IRQ | 2 << 8;
}

/* size bytes of configuration register r */
static u32 cfg_read(unsigned r, int size)
{
	u32 v = cfg[(r >> 2) & 63] >> (8 * (r & 3));
	return size == 4 ? v : size == 2 ? v & 0xffff : v & 0xff;
}

/* size bytes of register r = v; the BARs' sizing answered (256 and
   128 bytes of I/O) */
static void cfg_write(unsigned r, u32 v, int size)
{
	u32 mask = size == 4 ? 0xffffffff : size == 2 ? 0xffff : 0xff;
	LOG("pci config %02x = %x\n", r, v);
	if (r == 0x10 || r == 0x14) {
		if (v == 0xffffffff)
			v = r == 0x10 ? 0xffffff01 : 0xffffff81;
		else
			v = (v & ~3u) | 1;
	}
	cfg[(r >> 2) & 63] = (cfg[(r >> 2) & 63] & ~(mask << 8 * (r & 3))) |
		(v & mask) << 8 * (r & 3);
}

/* the configuration space of the one device */
void OS(OsPciReadConfigb)(void *d, byte o, byte *v) { (void)d; *v = (byte)cfg_read(o, 1); }
void OS(OsPciReadConfigw)(void *d, byte o, word *v) { (void)d; *v = (word)cfg_read(o, 2); }
void OS(OsPciReadConfigdw)(void *d, byte o, u32 *v) { (void)d; *v = cfg_read(o, 4); }
void OS(OsPciWriteConfigb)(void *d, byte o, byte v) { (void)d; cfg_write(o, v, 1); }
void OS(OsPciWriteConfigw)(void *d, byte o, word v) { (void)d; cfg_write(o, v, 2); }
void OS(OsPciWriteConfigdw)(void *d, byte o, u32 v) { (void)d; cfg_write(o, v, 4); }

struct pci_res { u32 addr, len; };
/* its resources: no memory, NAMBAR and NABMBAR; 1 */
int OS(OsGetPCIDeviceResources)(void *dev, struct pci_res *mem,
				struct pci_res *io1, struct pci_res *io2, int bm)
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

/* the interrupt: the module's handler, as Linux calls it */
static int (*isr)(int irq, void *dev, void *regs);
static void *isr_dev;
/* its interrupt handler, for line_sample to call with devid; 1 */
int OS(OsHookInterrupt)(void *dev, void *fn, void *devid)
{
	(void)dev;
	isr = (int (*)(int, void *, void *))fn;
	isr_dev = devid;
	return 1;
}
/* and no more */
void OS(OsUnhookInterrupt)(void *dev, void *devid) { (void)dev; (void)devid; isr = NULL; }

/* port I/O.  A read of size bytes: the secondary codec at NAMBAR + 0x80,
   the bus masters', GLOB_CNT and GLOB_STA at NABMBAR; all ones elsewhere */
static u32 io_in(unsigned a, int size)
{
	u32 v = 0xffffffff;
	if (a >= NAMBAR && a < NAMBAR + 0x100) {
		int r = a - NAMBAR;
		v = r >= 0x80 ? codec_read(r - 0x80) : 0xffff;
	} else if (a >= NABMBAR && a < NABMBAR + 0x80) {
		int o = a - NABMBAR;
		struct bm *b = o < 0x10 ? &pi : o < 0x20 ? &po : NULL;
		if (b) {
			switch (o & 0xf) {
			case 0x0: v = b->bdbar; break;
			case 0x4: v = b->civ | b->lvi << 8 | (u32)b->sr << 16; break;
			case 0x5: v = b->lvi; break;
			case 0x6: v = b->sr; break;
			case 0x8: v = b->picb; break;
			case 0xa: v = (b->civ + 1) & 31; break;
			case 0xb: v = b->cr; break;
			default: LOG("in%d bm %02x\n", size, o);
			}
		} else {
			switch (o) {
			case 0x3c: v = glob_cnt; break;
			case 0x40: v = glob_status(); break;
			case 0x44: v = 0; break;        /* the codec is ours */
			default: LOG("in%d nabm %02x\n", size, o);
			}
		}
	} else
		LOG("in%d %04x\n", size, a);
	if (size == 1)
		v &= 0xff;
	else if (size == 2)
		v &= 0xffff;
	if (dbg > 1)
		fprintf(stderr, "hsf: in%d %04x = %x\n", size, a, v);
	return v;
}

/* a port write of size bytes, as io_in has them */
static void io_out(unsigned a, u32 v, int size)
{
	if (dbg > 1)
		fprintf(stderr, "hsf: out%d %04x = %x\n", size, a, v);
	if (a >= NAMBAR && a < NAMBAR + 0x100) {
		int r = a - NAMBAR;
		if (r >= 0x80)
			codec_write(r - 0x80, (word)v);
		return;
	}
	if (a >= NABMBAR && a < NABMBAR + 0x80) {
		int o = a - NABMBAR;
		struct bm *b = o < 0x10 ? &pi : o < 0x20 ? &po : NULL;
		if (b) {
			switch (o & 0xf) {
			case 0x0: b->bdbar = v & ~7u; return;
			case 0x5: b->lvi = v & 31; return;
			case 0x6: b->sr &= ~(v & (SR_LVBCI | SR_BCIS | SR_FIFOE)); return;
			case 0xb: bm_cr(b, (byte)v); return;
			}
			LOG("out%d bm %02x = %x\n", size, o, v);
			return;
		}
		switch (o) {
		case 0x3c: glob_cnt = v; return;
		case 0x40: glob_sta &= ~(v & 0xc01); return;
		case 0x44: return;
		}
		LOG("out%d nabm %02x = %x\n", size, o, v);
		return;
	}
	LOG("out%d %04x = %x\n", size, a, v);
}

/* the module's port I/O */
byte OS(OsIoPortReadb)(unsigned p) { return (byte)io_in(p, 1); }
word OS(OsIoPortReadw)(unsigned p) { return (word)io_in(p, 2); }
u32 OS(OsIoPortReadl)(unsigned p) { return io_in(p, 4); }
void OS(OsIoPortWriteb)(unsigned p, byte v) { io_out(p, v, 1); }
void OS(OsIoPortWritew)(unsigned p, word v) { io_out(p, v, 2); }
void OS(OsIoPortWritel)(unsigned p, u32 v) { io_out(p, v, 4); }

/*
 *    NVM: the .inf files, as cvtinf.pl makes files of them and osnvm.c
 *    reads those
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
	unsigned i;
	for (i = 0; i < sizeof(cfg_names) / sizeof(cfg_names[0]); i++)
		if (cfg_names[i].code == code)
			return cfg_names[i].name;
	return NULL;
}

/* the values: their path (as the nvm tree's, under hsfmc97ich/) and
   their text */
struct nvm_val {
	struct nvm_val *next;
	char *path, *text;
};
static struct nvm_val *nvm_vals;

/* the value at path, NULL if none */
static struct nvm_val *nvm_find(const char *path)
{
	struct nvm_val *v;
	for (v = nvm_vals; v; v = v->next)
		if (!strcmp(v->path, path))
			return v;
	return NULL;
}

/* the value at path made text (a copy) */
static void nvm_set(const char *path, const char *text)
{
	struct nvm_val *v = nvm_find(path);
	if (!v) {
		v = calloc(1, sizeof(*v));
		v->path = strdup(path);
		v->next = nvm_vals;
		nvm_vals = v;
	} else
		free(v->text);
	v->text = strdup(text);
}

/* osnvm.c's NVM_ReadFile on the text: quoted strings as they are, hex
   numbers a byte each; the number of bytes */
static int nvm_bytes(const char *t, byte *out, int max)
{
	int n = 0, str = 0;
	const char *p;
	for (p = t; *p && n < max; p++) {
		if (*p == '"') {
			str = !str;
			continue;
		}
		if (str) {
			out[n++] = (byte)*p;
			continue;
		}
		if (*p == ';')          /* a comment to the end of the line */
			while (p[1] && p[1] != '\n')
				p++;
		if (!isxdigit((byte)*p))
			continue;
		out[n++] = (byte)strtoul(p, (char **)&p, 16);
		p--;
	}
	return n;
}

/* the value's bytes all zero */
static int nvm_zero(const char *t)
{
	byte b[4096];
	int i, n = nvm_bytes(t, b, sizeof(b));
	for (i = 0; i < n; i++)
		if (b[i])
			return 0;
	return 1;
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

/* each from in s replaced by to, in place (4095 characters at most) */
static void subst(char *s, const char *from, const char *to)
{
	char *p, buf[4096];
	while ((p = strstr(s, from))) {
		snprintf(buf, sizeof(buf), "%.*s%s%s", (int)(p - s), s, to,
			 p + strlen(from));
		strcpy(s, buf);
	}
}

/* Region and Profile entries: their index, its T35 code, its values */
struct cty_ent {
	struct cty_ent *next;
	int region;
	char idx[16], t35[8], type[8];
	struct nvm_val *vals;
};
static struct cty_ent *cty_ents;

/* the Region (region 1) or Profile (0) entry idx, made if new */
static struct cty_ent *cty_get(int region, const char *idx)
{
	struct cty_ent *e;
	for (e = cty_ents; e; e = e->next)
		if (e->region == region && !strcmp(e->idx, idx))
			return e;
	e = calloc(1, sizeof(*e));
	e->region = region;
	snprintf(e->idx, sizeof(e->idx), "%s", idx);
	e->next = cty_ents;
	cty_ents = e;
	return e;
}

/* l ends in a backslash (spaces after it aside): continued */
static int line_cont(const char *l)
{
	size_t n = strlen(l);
	while (n && isspace((byte)l[n - 1]))
		n--;
	return n && l[n - 1] == '\\';
}

/* the HKR lines of an .inf or .cty file as cvtinf.pl takes them: OEM
   values (by their configuration names), Country, Region and Profile
   ones into cty_ents; continuation lines joined; exits if file cannot
   be read */
static void nvm_load(const char *file)
{
	FILE *f = fopen(file, "r");
	char line[1024], *text;
	size_t tl;

	if (!f) {
		fprintf(stderr, "hsf: %s: %s\n", file, strerror(errno));
		exit(1);
	}
	text = malloc(65536);
	while (fgets(line, sizeof(line), f)) {
		char sub[128], key[128], type[32], idx[16] = "";
		char *rest, *p;
		int region = -1, n, cont;

		line[strcspn(line, "\r\n")] = 0;
		if (strncmp(line, "HKR,", 4))
			continue;
		/* HKR,<subkey>,<key>,<type>,<data> */
		n = sscanf(line + 4, "%127[^,],%127[^,],%31[^,],", sub, key, type);
		if (n < 2)
			continue;
		if (n < 3)
			type[0] = 0;
		for (p = key; *p; p++)
			;
		while (p > key && isspace((byte)p[-1]))
			*--p = 0;
		rest = line + 4;
		for (n = 0; n < 3 && rest; n++) {
			rest = strchr(rest, ',');
			if (rest)
				rest++;
		}
		if (!rest)
			rest = (char *)"";
		while (*rest == ' ' || *rest == '\t')
			rest++;
		cont = line_cont(line);
		if (*rest == '\\')
			rest++;
		snprintf(text, 65536, "%s\n", rest);
		/* continuation lines: while the line ends in a backslash */
		tl = strlen(text);
		while (cont && fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			cont = line_cont(line);
			tl += snprintf(text + tl, 65536 - tl, "%s\n", line);
		}
		subst(text, "@CNXTTARGET@", "hsf");
		subst(text, "@CNXTLINUXVERSION@", "7.80.02.06oem");
		subst(text, "@CNXTDRVDSC@", "Conexant HSF softmodem");

		if (!strncmp(sub, "Country\\", 8) || !strncmp(sub, "Region\\", 7) ||
		    !strncmp(sub, "Profile\\", 8)) {
			region = sub[0] != 'P';
			snprintf(idx, sizeof(idx), "%s", strchr(sub, '\\') + 1);
		} else if (!strcmp(sub, "OEM")) {
			unsigned i;
			for (i = 0; i < sizeof(oem_keys) / sizeof(oem_keys[0]); i++)
				if (!strcmp(key, oem_keys[i][0]))
					snprintf(key, sizeof(key), "%s", oem_keys[i][1]);
		} else if (!strncmp(sub, "OEM\\", 4)) {
			char k[128];
			int d;
			if (snprintf(k, sizeof(k), "%s_%s", sub + 4, key) >=
			    (int)sizeof(k))
				continue;
			if (sscanf(k, "ATI_%d", &d) == 1)
				snprintf(k, sizeof(k), "ATMESSAGE_I%d", d);
			strcpy(key, k);
		} else
			continue;

		if (region < 0) {
			nvm_set(key, text);
			continue;
		}
		{
			struct cty_ent *e = cty_get(region, idx);
			struct nvm_val *v;
			if (!strcmp(key, "T35CODE")) {
				byte b[2] = { 0, 0 };
				nvm_bytes(text, b, 2);
				snprintf(e->t35, sizeof(e->t35), "%02X%02X", b[1], b[0]);
				continue;
			}
			if (!strcmp(key, "TYPE")) {
				byte b = 0;
				nvm_bytes(text, &b, 1);
				snprintf(e->type, sizeof(e->type), "%02X", b);
				continue;
			}
			if (!strcmp(key, "CALLWAITING_PARMS"))
				snprintf(key, sizeof(key), "CALL_WAITING");
			v = calloc(1, sizeof(*v));
			v->path = strdup(key);
			v->text = strdup(text);
			v->next = e->vals;
			e->vals = v;
		}
	}
	free(text);
	fclose(f);
}

/* the Region and Profile files, as cvtinf.pl writes them (all-zero values
   left out: the reader goes to the profile then) */
static void nvm_cty(void)
{
	struct cty_ent *e;
	for (e = cty_ents; e; e = e->next) {
		struct nvm_val *v;
		char dir[64], path[160];
		if (e->region)
			snprintf(dir, sizeof(dir), "Region/%s", e->t35);
		else
			snprintf(dir, sizeof(dir), "Profile/%s", e->idx);
		for (v = e->vals; v; v = v->next) {
			if (!strcmp(v->path, "NAME")) {
				char t[256];
				snprintf(t, sizeof(t), "%s", v->text);
				if (!strcmp(e->type, "01")) {
					char *q = strrchr(t, '"');
					if (q) {
						memmove(q + 1, q, strlen(q) + 1);
						*q = '*';
					}
				}
				snprintf(path, sizeof(path), "%s_NAME", dir);
				nvm_set(path, t);
				continue;
			}
			if (strcmp(v->path, "REFERENCE") && nvm_zero(v->text))
				continue;
			snprintf(path, sizeof(path), "%s/%s", dir, v->path);
			nvm_set(path, v->text);
		}
	}
}

/* osnvm.c's NVM_ReadFile on a value */
static int nvm_read_path(const char *path, void *buf, u32 *size)
{
	struct nvm_val *v = nvm_find(path);
	byte tmp[8192];
	int n;
	if (!v)
		return 0;
	n = nvm_bytes(v->text, tmp, sizeof(tmp));
	if ((u32)n > *size)
		n = (int)*size;
	memcpy(buf, tmp, n);
	memset((byte *)buf + n, 0, *size - n);
	*size = n;
	return 1;
}

/* the dynamic values (written at run time): in memory */
struct nvm_dyn {
	struct nvm_dyn *next;
	int code;
	u32 size;
	byte data[];
};
static struct nvm_dyn *nvm_dyns;

static const byte factory_profile[25] = {
	1, 1, 1, 0, 0, 1, 3, 0, 1, 2,
	0, 0, 43, 13, 10, 8, 2, 50, 2, 14, 95, 50, 0, 0, 70
};

/* code is a dynamic value (written at run time), kept in memory */
static int nvm_dynamic(int code)
{
	switch (code) {
	case CFG_PROFILE_STORED: case 360: case 362: case CFG_COUNTRY_CODE:
	case 325: case 345: case 347: case 348: case 349: case 350: case 356:
	case 402: case 401:
		return 1;
	}
	{
		const char *n = cfg_name(code);
		return n && (!strcmp(n, "HARDWARE_PROFILE") ||
			     !strcmp(n, "HARDWARE_ID") ||
			     !strncmp(n, "LICENSE_", 8));
	}
}

/* country t35's value name (size bytes into buf), else its profile
   ref's (0xff: none); whether there was one */
static int cty_parm(int t35, byte ref, const char *name, void *buf, u32 size)
{
	char path[64];
	u32 s = size;
	snprintf(path, sizeof(path), "Region/%04X%s", t35, name);
	if (nvm_read_path(path, buf, &s))
		return 1;
	if (ref == 0xff)
		return 0;
	snprintf(path, sizeof(path), "Profile/%04X%s", ref, name);
	s = size;
	return nvm_read_path(path, buf, &s);
}

/* osnvm.c's NVM_ReadCountry: configtypes.h's CtryPrmsStruct (929 bytes) */
static void nvm_country(int t35, byte *c)
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
	unsigned i;

	c[0] = (byte)t35;
	c[1] = (byte)(t35 >> 8);
	cty_parm(t35, 0xff, "/REFERENCE", &ref, 1);
	for (i = 0; i < sizeof(f) / sizeof(f[0]); i++)
		if (!cty_parm(t35, ref, f[i].name, c + f[i].off, f[i].size))
			LOG("nvm: country %04x: no %s\n", t35, f[i].name);
	LOG("nvm: country %04x, profile %02x\n", t35, ref);
}

/* the NVM: no handle of its own */
void *OS(NVM_Open)(unsigned long dev) { return (void *)dev; }
int OS(NVM_Close)(void *h) { (void)h; return 0; }

/* value code into buf (*size bytes, the rest zeros; *size what there
   was): the country structure (the T35 code in size), the dynamic
   values (the USA and the factory profile by default), the .inf's;
   0, 7 if none */
int OS(NVM_Read)(void *h, int code, void *buf, u32 *size)
{
	const char *name;
	(void)h;
	if (code == CFG_COUNTRY_STRUCT) {
		/* (the country's T35 code in place of the size) */
		memset(buf, 0, 929);
		nvm_country((int)(uintptr_t)size, buf);
		return 0;
	}
	if (nvm_dynamic(code)) {
		struct nvm_dyn *d;
		for (d = nvm_dyns; d; d = d->next)
			if (d->code == code) {
				u32 n = d->size < *size ? d->size : *size;
				memcpy(buf, d->data, n);
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
	if (name && nvm_read_path(name, buf, size))
		return 0;
	if (code == CFG_PROFILE_FACTORY && *size == sizeof(factory_profile)) {
		memcpy(buf, factory_profile, sizeof(factory_profile));
		return 0;
	}
	if (dbg > 1)
		LOG("nvm: no %s (%d)\n", name ? name : "?", code);
	memset(buf, 0, *size);
	*size = 0;
	return 7;
}

/* value code = *size bytes of buf, in memory; 0 */
int OS(NVM_Write)(void *h, int code, void *buf, u32 *size)
{
	struct nvm_dyn *d, **pp;
	(void)h;
	for (pp = &nvm_dyns; *pp; pp = &(*pp)->next)
		if ((*pp)->code == code) {
			d = *pp;
			*pp = d->next;
			free(d);
			break;
		}
	d = malloc(sizeof(*d) + *size);
	d->code = code;
	d->size = *size;
	memcpy(d->data, buf, *size);
	d->next = nvm_dyns;
	nvm_dyns = d;
	return 0;
}

/*
 *    the DTE: a pty, the engine's ComCtrl
 */

static int pty;
static void *hcom;

/* HSF_SYNC=<samples>: the DTE's input only every that many samples of
   the line (as half_agere's AGERE_SYNC) */
static long sync_n;

/* online between its CONNECT and NO CARRIER or OK */
static int online;
/* the modem's output to the DTE, c by c: online from CONNECT, offline
   from NO CARRIER or OK */
static void result_scan(unsigned char c)
{
	static const char *const pat[] = { "CONNECT", "NO CARRIER",
					   "\r\nOK\r\n" };
	static unsigned at[3];
	int i;

	for (i = 0; i < 3; i++) {
		if (c != (unsigned char)pat[i][at[i]])
			at[i] = c == (unsigned char)pat[i][0];
		else
			at[i]++;
		if (!pat[i][at[i]]) {
			at[i] = 0;
			online = i == 0;
		}
	}
}

/* the modem's output (ComCtrl_Read) to the pty, all there is */
static void dte_out(void)
{
	byte buf[1024];
	u32 n, k;
	if (!hcom)
		return;
	while ((n = OS(ComCtrl_Read)(hcom, buf, sizeof(buf))) > 0 && n <= sizeof(buf)) {
		for (k = 0; k < n; k++)
			result_scan(buf[k]);
		if (write(pty, buf, n) < 0)
			LOG("pty write: %s\n", strerror(errno));
	}
}

/* the DTE's input, as much as the modem takes */
static void dte_poll(void)
{
	byte buf[256];
	u32 room = 0;
	int n;

	if (!hcom || (sync_n && line_n % sync_n && !online))
		return;
	if (OS(ComCtrl_Monitor)(hcom, COMCTRL_MONITOR_TXFREE, &room) || !room)
		return;
	n = (int)read(pty, buf, room < sizeof(buf) ? room : sizeof(buf));
	if (n > 0 && OS(ComCtrl_Write)(hcom, buf, (u32)n) != (u32)n)
		LOG("ComCtrl_Write: short\n");
}

/* ComCtrl's events (the serial driver's): logged only */
static void event(void *ref, u32 mask)
{
	(void)ref;
	if (dbg > 1)
		LOG("event %x\n", mask);
}

/*
 *    the codec's samples: a windowed sinc resampler each way between the
 *    line's rate and the codec's, as half_sm56's
 */

#define RS_TAPS 32
struct rs {
	float hist[RS_TAPS];    /* the newest last */
};
/* the signal of h at f (0..1 or a little over) after its sample
   RS_TAPS / 2 - 1, lowpass at cut (<= 1) of its Nyquist */
static float rs_at(const float *h, double f, double cut)
{
	double y = 0;
	int k;
	for (k = 0; k < RS_TAPS; k++) {
		double t = k - (RS_TAPS / 2 - 1) - f, x = M_PI * t * cut;
		double w = fabs(t) < RS_TAPS / 2 ?
			0.5 + 0.5 * cos(M_PI * t / (RS_TAPS / 2)) : 0;
		y += h[k] * cut * (fabs(x) < 1e-9 ? 1 : sin(x) / x) * w;
	}
	return (float)y;
}
/* x in as the newest */
static void rs_push(struct rs *r, float x)
{
	memmove(r->hist, r->hist + 1, (RS_TAPS - 1) * sizeof(float));
	r->hist[RS_TAPS - 1] = x;
}

static struct rs up, down;      /* line to codec, codec to line */

/* the codec's input noise (HSF_NOISE, rms; 5 by default) */
static float noise_rms = 5;
static float noise(void)
{
	static u32 x = 1;
	x = x * 1664525u + 1013904223u;
	return noise_rms * 3.4641f * ((float)(x >> 8) / (1 << 24) - 0.5f);
}
static double up_ph;

/* x rounded and clipped to 16 bits */
static short sat(double x)
{
	return (short)(x > 32767 ? 32767 : x < -32768 ? -32768 : lrint(x));
}

/* one codec sample each way through the bus masters (the DAA's
   loopback: the DAC's back to the ADC); the DAC's into the downward
   resampler */
static void codec_sample(short adc)
{
	short dac = bm_sample(&po, 0);
	/* DAA register 0x0e, bits 5 and 6: its loopbacks, which the
	   SmartDAA library measures the path's delay by */
	if (daa[0x0e] & 0x60)
		adc = dac;
	bm_sample(&pi, adc);
	rs_push(&down, dac);
}

/* the module's interrupt.  Its flag at 0x280 of its device (the engine
   sets it, 2) makes its playback position 8 samples further on every
   interrupt past sample 700 of the ring: some other controller's, not the
   ICH's, whose position is right, and the engine runs 8 samples a buffer
   ahead of it until it laps the ring (every 3.3 s, a buffer lost); so it
   is cleared (HSF_KEEP8 keeps it) */
static void interrupt(void)
{
	if (!getenv("HSF_KEEP8"))
		*(u32 *)((byte *)isr_dev + 0x280) = 0;
	isr(IRQ, isr_dev, NULL);
}

/* one line sample each way */
static short line_sample(short in)
{
	double r = (double)line_rate / codec_rate;
	double out;
	unsigned long long ms = now_ms();

	rs_push(&up, in + noise());
	while (up_ph < 1) {
		codec_sample(sat(rs_at(up.hist, up_ph, r > 1 ? 1 / r : 1)));
		up_ph += r;
	}
	up_ph -= 1;
	out = rs_at(down.hist, 1 - up_ph / r, r < 1 ? r : 1);
	line_n++;
	/* (level triggered, as PCI's; and with a latency: the module takes
	   a PICB of a whole buffer for one that has not moved yet, and
	   reads it again and again, then guesses) */
	if (isr && (bm_irq(&pi) || bm_irq(&po)) && !bm_fresh(&pi) &&
	    !bm_fresh(&po)) {
		int guard = 0;
		while ((bm_irq(&pi) || bm_irq(&po)) && guard++ < 4)
			interrupt();
	}
	if (now_ms() != ms)
		run_timers();
	return sat(out);
}

/* time with no line: a sleep or a wait of the modem's */
static void run_time(unsigned ms)
{
	unsigned long long n = (unsigned long long)ms * line_rate / 1000;
	while (n--)
		line_sample(0);
}

/*
 *    main loop
 */

/* the rdtsc trap, the NVM, the devices, the engine's init and the
   serial driver's open (115200 8N1, DTR, RTS); then the line from fd 3
   to fd 4 a sample at a time, the DTE served every 48, until it ends */
int main(int argc, char **argv)
{
	short in[480], out[480];
	struct { void *ref; void (*fn)(void *, u32); } evh = { NULL, event };
	struct {
		u32 valid, speed;
		int parity, bits, xout, xin, err, null, cts, rts;
		char xon, xoff, errc, pealt;
	} pc;
	const char *dir;
	char path[512], *name;
	int i, n, r;

	(void)argc; (void)argv;
	dbg = getenv("HSF_DEBUG") ? atoi(getenv("HSF_DEBUG")) : 0;
	if (getenv("HSF_RATE"))
		line_rate = atoi(getenv("HSF_RATE"));
	if (getenv("HSF_NOISE"))
		noise_rms = (float)atof(getenv("HSF_NOISE"));
	sync_n = getenv("HSF_SYNC") ? (atol(getenv("HSF_SYNC")) + 47) / 48 * 48 : 0;
	dir = getenv("HSF_NVM") ? getenv("HSF_NVM") : HSF_NVM_DIR;
	if (tsc_init()) {
		perror("hsf: PR_SET_TSC");
		return 1;
	}
	setvbuf(stdout, NULL, _IONBF, 0);

	snprintf(path, sizeof(path), "%s/hsfmc97ich.inf", dir);
	nvm_load(path);
	snprintf(path, sizeof(path), "%s/hsf.cty", dir);
	nvm_load(path);
	nvm_cty();

	codec_init();
	cfg_init();
	pi.sr = po.sr = SR_DCH;
	pi.fresh = po.fresh = 1;

	pty = posix_openpt(O_RDWR | O_NOCTTY);
	if (pty < 0 || grantpt(pty) || unlockpt(pty)) {
		perror("pty");
		return 1;
	}
	{
		struct termios t;
		tcgetattr(pty, &t);
		cfmakeraw(&t);
		tcsetattr(pty, TCSANOW, &t);
	}
	fcntl(pty, F_SETFL, O_NONBLOCK);
	name = ptsname(pty);

	/* the modules' init, the PCI probe's device node, the serial
	   driver's open (serial_cnxt.c) */
	r = HsfEngineInit();
	if (r) {
		fprintf(stderr, "hsf: HsfEngineInit: %d\n", r);
		return 1;
	}
	node.hwDev = pcidev;
	node.hwInstNum = 0;
	snprintf(node.hwInstName, sizeof(node.hwInstName),
		 "PCI-8086:2416-8086:2416");
	snprintf(node.hwProfile, sizeof(node.hwProfile), "hsfmc97ich");
	node.hwType = HW_TYPE_ICH;
	node.hwIf = GetHwFuncs();
	hcom = OS(ComCtrl_Create)();
	if (!hcom) {
		fprintf(stderr, "hsf: ComCtrl_Create failed\n");
		return 1;
	}
	node.hcomctrl = hcom;
	if ((r = OS(ComCtrl_Configure)(hcom, COMCTRL_CONFIG_DEVICE_ID, &node)) ||
	    (r = OS(ComCtrl_Configure)(hcom, COMCTRL_CONFIG_EVENT_HANDLER, &evh))) {
		fprintf(stderr, "hsf: ComCtrl_Configure: %d\n", r);
		return 1;
	}
	r = OS(ComCtrl_Open)(hcom);
	if (r) {
		fprintf(stderr, "hsf: ComCtrl_Open: %d\n", r);
		return 1;
	}
	memset(&pc, 0, sizeof(pc));
	pc.valid = 0x00000001 | 0x10 | 0x20 | 0x1000 | 0x2000;
	pc.speed = 115200;
	pc.bits = 1;            /* 8 */
	OS(ComCtrl_Control)(hcom, COMCTRL_CONTROL_PORTCONFIG, &pc);
	OS(ComCtrl_Control)(hcom, COMCTRL_CONTROL_SETDTR, NULL);
	OS(ComCtrl_Control)(hcom, COMCTRL_CONTROL_SETRTS, NULL);
	printf("created hsf: %s\n", name);

	memset(out, 0, sizeof(out));
	if (write(4, out, 48 * 2 * 4) < 0)
		return 1;
	for (;;) {
		static int odd;
		n = (int)read(3, (char *)in + odd, sizeof(in) - odd);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		n += odd;
		odd = n & 1;
		n /= 2;
		for (i = 0; i < n; i++) {
			out[i] = line_sample(in[i]);
			if (line_n % 48 == 0) {
				dte_out();
				dte_poll();
			}
		}
		if (write(4, out, n * 2) != n * 2)
			break;
		if (odd)
			((char *)in)[0] = ((char *)in)[n * 2];
		if (!sync_n)
			dte_poll();
		dte_out();
	}
	return 0;
}
