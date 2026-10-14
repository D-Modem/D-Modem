/*
 *    interop_windows.c - a Windows kernel of our own for the WDM driver
 *    of a soft modem (interop_windows.h): what the driver imports
 *    (ntoskrnl, HAL, WMILIB, ks: stdcall and fastcall as there), the PnP
 *    manager and the PCI bus driver under the driver, modem.sys over it.
 *
 *    The driver is a PE image: loaded at its base (0x10000), its imports
 *    bound to ours by name (one we do not have stops us with its name,
 *    if it is called).  Everything runs on one thread and the line's
 *    clock: the interrupt time and the system time are the line's
 *    samples (each read on by 1 us while the line stands still, as busy
 *    waits need), timers fire and their DPCs run between samples, the ISR
 *    when the device's interrupt comes up, and the driver's system
 *    threads are coroutines that run when what they wait for comes.  The
 *    registry is the driver's INF (its model's AddReg sections, as the
 *    modem class installer writes them) in memory.  fs is a KPCR (the
 *    driver checks its stack against the TIB's limit).
 *
 *    The device is the harness's (struct win_dev).  Its registers are a
 *    window of ours that is not accessible: each access faults and is
 *    single-stepped with the device's values (see mm_fault).  in, out,
 *    cli, sti, mov to and from CRn and rdtsc trap (no I/O permission,
 *    PR_TSC_SIGSEGV) and are carried out here; PCI's configuration ports
 *    are the device's configuration space.
 *
 *    We are the rest of the device stack: the PCI bus driver under the
 *    driver (its PDO: the PnP IRPs sent down, the device's resources),
 *    the PnP manager (DriverEntry, AddDevice, START_DEVICE) and, over it,
 *    Windows' modem.sys: the driver is a serial port, opened
 *    (IRP_MJ_CREATE), set up by serial IOCTLs and read and written with
 *    IRPs.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "half_stack.h"
#include "interop_windows.h"

typedef int NTSTATUS;
typedef unsigned short wchar;   /* Windows' WCHAR */

#define NTAPI __attribute__((stdcall))
#define FASTCALL __attribute__((fastcall))

int win_debug;
#define LOG(fmt, ...) do { if (win_debug) fprintf(stderr, "win: " fmt, ##__VA_ARGS__); } while (0)

/* the device's IRQL (its interrupt's, as the HAL translates it) */
#define DIRQL 0x16

#define STATUS_SUCCESS                  0
#define STATUS_TIMEOUT                  0x00000102
#define STATUS_PENDING                  0x00000103
#define STATUS_BUFFER_OVERFLOW          ((NTSTATUS)0x80000005)
#define STATUS_NO_MORE_ENTRIES          ((NTSTATUS)0x8000001a)
#define STATUS_UNSUCCESSFUL             ((NTSTATUS)0xc0000001)
#define STATUS_NOT_IMPLEMENTED          ((NTSTATUS)0xc0000002)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xc000000d)
#define STATUS_INVALID_DEVICE_REQUEST   ((NTSTATUS)0xc0000010)
#define STATUS_MORE_PROCESSING_REQUIRED ((NTSTATUS)0xc0000016)
#define STATUS_NO_MEMORY                ((NTSTATUS)0xc0000017)
#define STATUS_ACCESS_DENIED            ((NTSTATUS)0xc0000022)
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xc0000023)
#define STATUS_OBJECT_NAME_NOT_FOUND    ((NTSTATUS)0xc0000034)
#define STATUS_OBJECT_PATH_NOT_FOUND    ((NTSTATUS)0xc000003a)
#define STATUS_NOT_SUPPORTED            ((NTSTATUS)0xc00000bb)
#define STATUS_CANCELLED                ((NTSTATUS)0xc0000120)
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)

/*
 *    the kernel's structures, as the 32 bit DDK has them
 */

struct list_entry { struct list_entry *flink, *blink; };
struct ustr { word len, max; wchar *buf; };     /* UNICODE_STRING */
struct astr { word len, max; char *buf; };      /* ANSI_STRING */

/* DISPATCHER_HEADER: the objects one can wait on */
struct dispatcher {
	byte type, absolute, size, inserted;
	int signal;
	struct list_entry wait;
};
#define OBJ_NOTIFICATION_EVENT 0
#define OBJ_SYNCHRONIZATION_EVENT 1
#define OBJ_THREAD 6
#define OBJ_TIMER_NOTIFICATION 8
#define OBJ_TIMER_SYNCHRONIZATION 9
struct kevent { struct dispatcher h; };
struct kdpc {
	short type;
	byte number, importance;
	struct list_entry entry;
	void *routine, *ctx, *a1, *a2;
	void *lock;             /* (queued: ours, non-NULL) */
};
struct ktimer {
	struct dispatcher h;
	u64 due;
	struct list_entry entry;
	struct kdpc *dpc;
	int period;
};
struct iosb { NTSTATUS status; u32 info; };

struct drvobj;
struct irp;
struct devobj {
	short type;
	word size;
	int refcount;
	struct drvobj *driver;
	struct devobj *next, *attached;
	struct irp *curirp;
	void *timer;
	u32 flags, characteristics;
	void *vpb, *ext;
	u32 devtype;
	char stacksize;
	byte pad[3];
	byte queue[40];
	u32 align;
	byte devqueue[20];
	struct kdpc dpc;
	u32 activethreads;
	void *secdesc;
	struct kevent lock;
	word sectorsize, spare1;
	void *devobjext, *reserved;
};
#define DO_BUFFERED_IO          0x00000004
#define DO_DIRECT_IO            0x00000010
#define DO_DEVICE_INITIALIZING  0x00000080
#define DO_BUS_ENUMERATED_DEVICE 0x00001000
#define DO_POWER_PAGABLE        0x00002000

struct drvext {
	struct drvobj *driver;
	void *adddevice;
	u32 count;
	struct ustr service;
};
struct drvobj {
	short type, size;
	struct devobj *devobj;
	u32 flags;
	void *start;
	u32 dsize;
	void *section;
	struct drvext *ext;
	struct ustr name;
	struct ustr *hwdb;
	void *fastio, *init, *startio, *unload;
	void *major[28];
};

struct iosl {
	byte major, minor, flags, control;
	u32 p[4];               /* Parameters */
	struct devobj *dev;
	void *fileobj;
	void *completion, *ctx;
};
#define SL_PENDING_RETURNED  0x01
#define SL_INVOKE_ON_CANCEL  0x20
#define SL_INVOKE_ON_SUCCESS 0x40
#define SL_INVOKE_ON_ERROR   0x80

struct irp {
	short type;
	word size;
	void *mdl;
	u32 flags;
	void *sysbuf;
	struct list_entry thread_list;
	struct iosb iosb;
	char reqmode;
	byte pending;
	char stackcount, curloc;
	byte cancel, cancelirql;
	char apcenv;
	byte allocflags;
	struct iosb *useriosb;
	struct kevent *userevent;
	u64 overlay;
	void *cancelroutine, *userbuf;
	void *drvctx[4];
	void *thread, *auxbuf;
	struct list_entry listentry;
	struct iosl *csl;
	void *fileobj;
	byte tail_pad[8];
};
#define IRP_BUFFERED_IO         0x00000010
#define IRP_DEALLOCATE_BUFFER   0x00000020
#define IRP_INPUT_OPERATION     0x00000040

struct mdl {
	struct mdl *next;
	short size, flags;
	void *process, *mapped, *startva;
	u32 count, offset;
};

struct dma_adapter { word version, size; void **ops; };

struct objattr {
	u32 length;
	void *root;
	struct ustr *name;
	u32 attributes;
	void *secdesc, *secqos;
};

struct rtlq {                   /* RTL_QUERY_REGISTRY_TABLE */
	void *routine;
	u32 flags;
	wchar *name;
	void *entryctx;
	u32 deftype;
	void *defdata;
	u32 deflen;
};

#define IRP_MJ_CREATE 0x00
#define IRP_MJ_CLOSE 0x02
#define IRP_MJ_READ 0x03
#define IRP_MJ_WRITE 0x04
#define IRP_MJ_DEVICE_CONTROL 0x0e
#define IRP_MJ_INTERNAL_DEVICE_CONTROL 0x0f
#define IRP_MJ_CLEANUP 0x12
#define IRP_MJ_POWER 0x16
#define IRP_MJ_SYSTEM_CONTROL 0x17
#define IRP_MJ_PNP 0x1b

#define IRP_MN_START_DEVICE 0x00
#define IRP_MN_QUERY_INTERFACE 0x08
#define IRP_MN_QUERY_CAPABILITIES 0x09
#define IRP_MN_QUERY_RESOURCES 0x0a
#define IRP_MN_QUERY_RESOURCE_REQUIREMENTS 0x0b
#define IRP_MN_QUERY_DEVICE_RELATIONS 0x07
#define IRP_MN_QUERY_ID 0x13
#define IRP_MN_QUERY_PNP_DEVICE_STATE 0x14
#define IRP_MN_SET_POWER 0x02
#define IRP_MN_QUERY_POWER 0x03

/*
 *    the image: a PE file mapped at its base, its relocations applied if
 *    it cannot be, its imports bound to ours
 */

static byte *img;               /* where it is */
static u32 img_size;
static void *img_entry;         /* its DriverEntry */

static u32 rd32(const byte *p) { u32 v; memcpy(&v, p, 4); return v; }
static word rd16(const byte *p) { word v; memcpy(&v, p, 2); return v; }

/* an import of ours: its name, our function (or the variable it is,
   for a data import) */
struct export { const char *name; void *fn; };
static const struct export *export_find(const char *name);
static const char *import_names[1024];
static int nimports;
static byte *thunks;            /* the traps and traces made for imports */
static int thunk_n;

/* an import we do not have: what it was, and the end */
static void __attribute__((used, noreturn)) import_missing(int i)
{
	fprintf(stderr, "win: the driver called %s, which we do not have\n",
		import_names[i]);
	exit(1);
}
/* the trace of a call of import i (win_debug 3) */
static void __attribute__((used)) import_trace(int i)
{
	fprintf(stderr, "win: > %s\n", import_names[i]);
}

/* code at thunks for import i: a jump to fn after its trace, or (fn
   NULL) the trap import_missing */
static void *thunk_make(int i, void *fn)
{
	byte *p = thunks + thunk_n;
	u32 v;
	if (!fn) {
		p[0] = 0x68;                    /* push i */
		v = (u32)i; memcpy(p + 1, &v, 4);
		p[5] = 0xe8;                    /* call import_missing */
		v = (u32)((uintptr_t)import_missing - (uintptr_t)(p + 10));
		memcpy(p + 6, &v, 4);
		thunk_n += 16;
		return p;
	}
	/* pushad, pushfd, push i, call import_trace, add esp 4, popfd,
	   popad, jmp fn */
	p[0] = 0x60; p[1] = 0x9c; p[2] = 0x68;
	v = (u32)i; memcpy(p + 3, &v, 4);
	p[7] = 0xe8;
	v = (u32)((uintptr_t)import_trace - (uintptr_t)(p + 12));
	memcpy(p + 8, &v, 4);
	p[12] = 0x83; p[13] = 0xc4; p[14] = 0x04;
	p[15] = 0x9d; p[16] = 0x61;
	p[17] = 0xe9;
	v = (u32)((uintptr_t)fn - (uintptr_t)(p + 22));
	memcpy(p + 18, &v, 4);
	thunk_n += 32;
	return p;
}

/* the file at path mapped as the image; 0, or -1 with a message */
static int pe_load(const char *path)
{
	FILE *f = fopen(path, "rb");
	byte *file, *opt, *sec;
	long n;
	u32 pe, base, nsec, hdrs, i, delta;
	int missing = 0;

	if (!f) {
		perror(path);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	file = malloc((size_t)n);
	if (fread(file, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		return -1;
	}
	fclose(f);
	pe = rd32(file + 0x3c);
	if (memcmp(file + pe, "PE\0\0", 4) || rd16(file + pe + 4) != 0x14c) {
		fprintf(stderr, "win: %s: not an i386 PE image\n", path);
		return -1;
	}
	nsec = rd16(file + pe + 6);
	opt = file + pe + 24;
	sec = opt + rd16(file + pe + 20);
	base = rd32(opt + 28);
	img_size = rd32(opt + 56);
	hdrs = rd32(opt + 60);
	img = mmap((void *)(uintptr_t)base, img_size,
		   PROT_READ | PROT_WRITE | PROT_EXEC,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (img == MAP_FAILED || img != (byte *)(uintptr_t)base) {
		if (img != MAP_FAILED)
			munmap(img, img_size);
		img = mmap(NULL, img_size, PROT_READ | PROT_WRITE | PROT_EXEC,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (img == MAP_FAILED) {
			perror("win: mmap");
			return -1;
		}
	}
	memcpy(img, file, hdrs);
	for (i = 0; i < nsec; i++) {
		byte *s = sec + 40 * i;
		u32 va = rd32(s + 12), raw = rd32(s + 16), off = rd32(s + 20);
		if (raw)
			memcpy(img + va, file + off, raw);
	}
	/* relocations (IMAGE_REL_BASED_HIGHLOW), if not at its base */
	delta = (u32)(uintptr_t)img - base;
	if (delta) {
		u32 rva = rd32(opt + 96 + 5 * 8), size = rd32(opt + 96 + 5 * 8 + 4);
		byte *r = img + rva, *end = r + size;
		LOG("relocated by %x\n", delta);
		while (r < end && rd32(r + 4)) {
			u32 page = rd32(r), bsize = rd32(r + 4), k;
			for (k = 8; k < bsize; k += 2) {
				word e = rd16(r + k);
				if (e >> 12 == 3) {
					byte *at = img + page + (e & 0xfff);
					u32 v = rd32(at) + delta;
					memcpy(at, &v, 4);
				}
			}
			r += bsize;
		}
	}
	/* imports, by name */
	thunks = mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE | PROT_EXEC,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
	{
		u32 rva = rd32(opt + 96 + 1 * 8);
		byte *d;
		for (d = img + rva; rd32(d + 12); d += 20) {
			const char *dll = (const char *)img + rd32(d + 12);
			byte *names = img + (rd32(d) ? rd32(d) : rd32(d + 16));
			byte *iat = img + rd32(d + 16);
			for (; rd32(names); names += 4, iat += 4) {
				u32 t = rd32(names);
				const char *name;
				const struct export *e;
				void *fn;
				if (t & 0x80000000) {
					fprintf(stderr, "win: %s: import by ordinal %u\n",
						dll, t & 0xffff);
					return -1;
				}
				name = (const char *)img + t + 2;
				e = export_find(name);
				if (!e) {
					LOG("import %s!%s: none\n", dll, name);
					missing++;
				}
				import_names[nimports] = name;
				fn = e ? e->fn : NULL;
				if (!e || (win_debug > 2 && !strstr(name, "Interlocked") &&
					   e->name[0] != '*'))
					fn = thunk_make(nimports, fn);
				nimports++;
				memcpy(iat, &fn, 4);
			}
		}
	}
	if (missing)
		LOG("%d imports we do not have (they stop us if called)\n", missing);
	img_entry = img + rd32(opt + 16);
	free(file);
	return 0;
}

/*
 *    UTF-16 and the like
 */

static int wlen(const wchar *w)
{
	int n = 0;
	while (w && w[n])
		n++;
	return n;
}
/* n wide characters (or to the 0, n < 0) as a C string (malloc) */
static char *w2a(const wchar *w, int n)
{
	char *s;
	int i;
	if (n < 0)
		n = wlen(w);
	s = malloc((size_t)n + 1);
	for (i = 0; i < n; i++)
		s[i] = w[i] < 0x80 ? (char)w[i] : '?';
	s[n] = 0;
	return s;
}
static char *us2a(const struct ustr *u)
{
	return u && u->buf ? w2a(u->buf, u->len / 2) : strdup("");
}
/* s as a wide string (malloc), 0 ended */
static wchar *a2w(const char *s)
{
	size_t n = strlen(s), i;
	wchar *w = malloc((n + 1) * 2);
	for (i = 0; i <= n; i++)
		w[i] = (byte)s[i];
	return w;
}

/*
 *    time: the line's samples
 */

static struct win_dev *wdev;            /* the harness's device */
unsigned long long win_line_n;          /* line samples so far */

/* the interrupt time (100 ns since boot): the line's, from 10 s; and
   on by the driver's busy waits (itime_last) while the line stands */
static u64 itime_last;
static u64 itime(void)
{
	u64 t = 100000000ull + win_line_n * 10000000ull / (u64)wdev->rate;
	return t > itime_last ? t : itime_last;
}
/* the system time (100 ns since 1601): 1 August 2002 then */
#define SYSTIME_BOOT ((1028160000ull + 11644473600ull) * 10000000ull)

/* the time stamp counter (rdtsc traps, PR_TSC_SIGSEGV): the line's
   time at 1 GHz, monotonic, on by 0.1 us each read (busy waits end) */
static unsigned long long tsc_last;
static unsigned long long tsc_read(void)
{
	unsigned long long t = win_line_n * (1000000000ull / (u64)wdev->rate);
	if (t <= tsc_last)
		t = tsc_last + 100;
	return tsc_last = t;
}

/* the line run on a ms (silent) while the kernel waits */
static void line_ms(void)
{
	wdev->idle((unsigned)(wdev->rate / 1000));
}

/*
 *    the scheduler: the IRQL, DPCs, timers, threads (coroutines)
 */

static byte irql;               /* 0 passive, 2 dispatch, more: device */

#define NDPC 64
static struct kdpc *dpcq[NDPC];
static int ndpc;

#define NTIMER 64
static struct ktimer *timers[NTIMER];   /* the set ones */
static u64 timer_due[NTIMER];           /* their due (interrupt time) */

struct kthread {
	struct dispatcher h;    /* (first: one can wait on a thread) */
	ucontext_t uc;
	void *stack;
	void *start, *arg;
	int dead;
	/* what it waits for: objects (all or any), until (0: no end) */
	struct dispatcher *wobj[8];
	int nwobj, wall;
	u64 wuntil;
	NTSTATUS wresult;
	int waiting;
	long prio;
};
#define NTHREAD 16
static struct kthread *threads[NTHREAD];
static int nthreads;
static struct kthread *cur_thread;      /* NULL: the main one */
static ucontext_t main_uc;

/* the KPCR's stack limits and thread, for each thread run (below) */
static void kpcr_stack(u32 base, u32 size, void *thread);
static u32 main_stack_base;
static byte kthread_main[0x400];

/* what DPC routines and the ISR are given */
typedef void NTAPI (*dpc_fn)(struct kdpc *dpc, void *ctx, void *a1, void *a2);

/* the DPCs queued, in order, at DISPATCH_LEVEL */
static void run_dpcs(void)
{
	if (irql >= 2)
		return;
	while (ndpc) {
		struct kdpc *d = dpcq[0];
		byte old = irql;
		memmove(dpcq, dpcq + 1, (size_t)--ndpc * sizeof(dpcq[0]));
		d->lock = NULL;
		irql = 2;
		if (!cur_thread)
			stack_clear();
		((dpc_fn)d->routine)(d, d->ctx, d->a1, d->a2);
		irql = old;
	}
}

/* object o signalled for a waiter; a synchronization event and timer
   (and the like) taken: reset */
static int obj_take(struct dispatcher *o, int take)
{
	if (o->type == OBJ_THREAD)
		return ((struct kthread *)o)->dead;
	if (o->signal <= 0)
		return 0;
	if (take && (o->type == OBJ_SYNCHRONIZATION_EVENT ||
		     o->type == OBJ_TIMER_SYNCHRONIZATION))
		o->signal = 0;
	return 1;
}

/* t's wait over? its result then */
static int wait_done(struct kthread *t)
{
	int i;
	if (t->wall) {
		for (i = 0; i < t->nwobj; i++)
			if (!obj_take(t->wobj[i], 0))
				break;
		if (i == t->nwobj) {
			for (i = 0; i < t->nwobj; i++)
				obj_take(t->wobj[i], 1);
			t->wresult = STATUS_SUCCESS;
			return 1;
		}
	} else {
		for (i = 0; i < t->nwobj; i++)
			if (obj_take(t->wobj[i], 1)) {
				t->wresult = STATUS_SUCCESS + i;
				return 1;
			}
	}
	if (t->wuntil && itime() >= t->wuntil) {
		t->wresult = STATUS_TIMEOUT;
		return 1;
	}
	return 0;
}

/* the threads that can go, each until it waits or ends, until none can */
static void run_threads(void)
{
	int i, more = 1;
	if (cur_thread || irql)
		return;
	while (more) {
		more = 0;
		for (i = 0; i < nthreads; i++) {
			struct kthread *t = threads[i];
			if (t->dead || (t->waiting && !wait_done(t)))
				continue;
			t->waiting = 0;
			cur_thread = t;
			kpcr_stack((u32)(uintptr_t)t->stack + (1 << 18), 1 << 18, t);
			swapcontext(&main_uc, &t->uc);
			cur_thread = NULL;
			kpcr_stack(main_stack_base, 1 << 20, kthread_main);
			run_dpcs();
			more = 1;
		}
	}
}

/* the timers due: off (or on again, periodic), signalled, their DPCs
   queued */
static int KeInsertQueueDpc_(struct kdpc *d, void *a1, void *a2);
static void run_timers(void)
{
	u64 now = itime();
	int i;
	for (i = 0; i < NTIMER; i++) {
		struct ktimer *t = timers[i];
		if (!t || timer_due[i] > now)
			continue;
		if (t->period)
			timer_due[i] += (u64)t->period * 10000;
		else {
			timers[i] = NULL;
			t->h.inserted = 0;
		}
		t->h.signal = 1;
		if (t->dpc)
			KeInsertQueueDpc_(t->dpc, (void *)(uintptr_t)(u32)(now + SYSTIME_BOOT),
					  (void *)(uintptr_t)(u32)((now + SYSTIME_BOOT) >> 32));
	}
}

/* all that can run: DPCs, then threads */
static void kernel_run(void)
{
	run_dpcs();
	run_threads();
}

/* a wait of the caller for n objects (all or any), until (interrupt
   time, 0 none); a thread's yields, the main one's runs the line on
   (silent) until it is over */
static NTSTATUS wait_for(struct dispatcher **o, int n, int all, u64 until)
{
	struct kthread self, *t = cur_thread ? cur_thread : &self;
	int i;
	memset(&self, 0, sizeof(self));
	for (i = 0; i < n && i < 8; i++)
		t->wobj[i] = o[i];
	t->nwobj = n;
	t->wall = all;
	t->wuntil = until;
	if (wait_done(t))
		return t->wresult;
	if (until && until <= itime())
		return STATUS_TIMEOUT;
	if (cur_thread) {
		t->waiting = 1;
		swapcontext(&t->uc, &main_uc);
		return t->wresult;
	}
	/* the main thread: the line on a ms at a time (60 s at most) */
	{
		u64 start = itime();
		for (;;) {
			kernel_run();
			if (wait_done(t))
				return t->wresult;
			if (itime() - start > 600000000ull) {
				fprintf(stderr, "win: a wait of 60 s in the main thread\n");
				return STATUS_TIMEOUT;
			}
			line_ms();
		}
	}
}

/* a timeout (100 ns, < 0 relative, > 0 absolute system time; NULL
   none) as the interrupt time it ends at (0 none; now for 0) */
static u64 timeout_end(const long long *to)
{
	if (!to)
		return 0;
	if (*to < 0)
		return itime() + (u64)(-*to);
	if (*to == 0)
		return itime();
	return (u64)*to > SYSTIME_BOOT ? (u64)*to - SYSTIME_BOOT : itime();
}

/*
 *    the KPCR at fs: its NT_TIB (exception list, stack base and limit:
 *    the driver checks the stack it has left), self, PRCB, the current
 *    thread; fs set to it through a TLS entry of the GDT (glibc's TLS is
 *    gs here)
 */
static byte kpcr[0x1000] __attribute__((aligned(4096)));

static void kpcr_stack(u32 base, u32 size, void *thread)
{
	u32 limit = base - size;
	memcpy(kpcr + 4, &base, 4);
	memcpy(kpcr + 8, &limit, 4);
	memcpy(kpcr + 0x124, &thread, 4);
}

static int kpcr_init(void)
{
	struct user_desc {
		unsigned entry_number, base_addr, limit;
		unsigned seg_32bit:1, contents:2, read_exec_only:1, limit_in_pages:1,
			seg_not_present:1, useable:1;
	} d;
	u32 v;
	memset(&d, 0, sizeof(d));
	d.entry_number = (unsigned)-1;
	d.base_addr = (u32)(uintptr_t)kpcr;
	d.limit = 0xfff;
	d.seg_32bit = 1;
	d.useable = 1;
	if (syscall(243 /* set_thread_area */, &d)) {
		perror("win: set_thread_area");
		return -1;
	}
	v = 0xffffffff;                 /* (no exception handler) */
	memcpy(kpcr, &v, 4);
	v = (u32)(uintptr_t)kpcr;
	memcpy(kpcr + 0x1c, &v, 4);     /* SelfPcr */
	v += 0x120;
	memcpy(kpcr + 0x20, &v, 4);     /* Prcb */
	main_stack_base = (u32)(uintptr_t)__builtin_frame_address(0) + 0x10000;
	kpcr_stack(main_stack_base, 1 << 20, kthread_main);
	__asm__ volatile("movw %w0, %%fs" : : "q"((d.entry_number << 3) | 3));
	return 0;
}

/* a thread's start: its routine, then its end */
static void thread_main(void)
{
	struct kthread *t = cur_thread;
	((void NTAPI (*)(void *))t->start)(t->arg);
	t->dead = 1;
	swapcontext(&t->uc, &main_uc);
}

/*
 *    the registry: keys by their full path, values in them; from the INF
 */

struct rval {
	struct rval *next;
	char *name;
	u32 type;
	byte *data;
	u32 len;
};
struct rkey {
	struct rkey *next;
	char *path;
	struct rval *vals;
};
static struct rkey *rkeys;
#define REG_NONE 0
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define REG_BINARY 3
#define REG_DWORD 4
#define REG_MULTI_SZ 7

/* the driver's keys: its service's, its driver key (its class's, the
   first instance) and its device's (win_start() names them) */
static char key_service[256], key_driver[256], key_device[512];

/* the key at path (made, if create); NULL if none */
static struct rkey *reg_key(const char *path, int create)
{
	struct rkey *k;
	size_t n = strlen(path);
	while (n && path[n - 1] == '\\')
		n--;
	for (k = rkeys; k; k = k->next)
		if (strlen(k->path) == n && !strncasecmp(k->path, path, n))
			return k;
	if (!create)
		return NULL;
	k = calloc(1, sizeof(*k));
	k->path = strndup(path, n);
	k->next = rkeys;
	rkeys = k;
	/* (and its parents, for enumeration) */
	{
		const char *b = memrchr(path, '\\', n);
		if (b && b > path) {
			char *p = strndup(path, (size_t)(b - path));
			reg_key(p, 1);
			free(p);
		}
	}
	return k;
}
/* the value name of k (made, if create) */
static struct rval *reg_val(struct rkey *k, const char *name, int create)
{
	struct rval *v, **at = &k->vals;
	for (v = k->vals; v; v = v->next) {
		if (!strcasecmp(v->name, name))
			return v;
		at = &v->next;
	}
	if (!create)
		return NULL;
	v = calloc(1, sizeof(*v));
	v->name = strdup(name);
	*at = v;
	return v;
}
static void reg_set(const char *path, const char *name, u32 type,
		    const void *data, u32 len)
{
	struct rval *v = reg_val(reg_key(path, 1), name, 1);
	free(v->data);
	v->type = type;
	v->data = malloc(len + 2);
	memcpy(v->data, data, len);
	v->data[len] = v->data[len + 1] = 0;
	v->len = len;
}
/* a string value (REG_SZ, as UTF-16) */
static void reg_set_sz(const char *path, const char *name, u32 type,
		       const char *s)
{
	wchar *w = a2w(s);
	reg_set(path, name, type, w, (u32)(strlen(s) + 1) * 2);
	free(w);
}

/* the INF: its lines by section, [Strings] for %name% */
static char *inf;

/* the lines of section name, one at a time: *at the place to go on
   from (NULL to start); NULL after the last */
static char *inf_line(const char *name, char **at)
{
	char *p = *at;
	if (!p) {
		size_t n = strlen(name);
		for (p = inf; p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
			if (*p == '[' && !strncasecmp(p + 1, name, n) && p[n + 1] == ']')
				break;
		}
		if (!p)
			return NULL;
		p = strchr(p, '\n');
		if (!p)
			return NULL;
		p++;
	}
	for (;;) {
		char *e;
		if (!*p || *p == '[')
			return NULL;
		e = strchr(p, '\n');
		*at = e ? e + 1 : p + strlen(p);
		if (*p != ';' && *p != '\n' && *p != '\r') {
			static char line[1024];
			size_t n = e ? (size_t)(e - p) : strlen(p);
			if (n >= sizeof(line))
				n = sizeof(line) - 1;
			memcpy(line, p, n);
			line[n] = 0;
			while (n && (line[n - 1] == '\r' || isspace((byte)line[n - 1])))
				line[--n] = 0;
			if (n)
				return line;
		}
		p = *at;
	}
}

/* section's value of key (key = value, quotes off), or NULL */
static const char *inf_value(const char *section, const char *key)
{
	static char val[512];
	char *at = NULL, *l;
	size_t n = strlen(key);
	while ((l = inf_line(section, &at))) {
		char *eq = strchr(l, '=');
		char *k = l, *v;
		if (!eq)
			continue;
		while (eq > k && isspace((byte)eq[-1]))
			eq--;
		if ((size_t)(eq - k) != n || strncasecmp(k, key, n))
			continue;
		v = strchr(l, '=') + 1;
		while (isspace((byte)*v))
			v++;
		if (*v == '"') {
			char *q = strrchr(v + 1, '"');
			v++;
			if (q)
				*q = 0;
		}
		snprintf(val, sizeof(val), "%s", v);
		return val;
	}
	return NULL;
}

/* [Strings]' value of key */
static const char *inf_string(const char *key)
{
	return inf_value("Strings", key);
}

/* s with %name% as [Strings] has it, %% as % */
static void inf_subst(const char *s, char *out, size_t size)
{
	size_t o = 0;
	while (*s && o + 1 < size) {
		if (*s == '%') {
			const char *e = strchr(s + 1, '%');
			if (e == s + 1) {
				out[o++] = '%';
				s += 2;
				continue;
			}
			if (e) {
				char key[128];
				const char *v;
				snprintf(key, sizeof(key), "%.*s", (int)(e - s - 1), s + 1);
				v = inf_string(key);
				if (v) {
					while (*v && o + 1 < size)
						out[o++] = *v++;
					s = e + 1;
					continue;
				}
			}
		}
		out[o++] = *s++;
	}
	out[o] = 0;
}

/* the line's comma separated fields (quotes off, blanks trimmed), up
   to max; how many */
static int inf_fields(char *l, char **f, int max)
{
	int n = 0;
	char *p = l;
	while (n < max) {
		char *o;
		int q = 0;
		while (*p == ' ' || *p == '\t')
			p++;
		f[n++] = o = p;
		while (*p && (q || *p != ',')) {
			if (*p == '"') {
				if (q && p[1] == '"') {
					*o++ = '"';
					p += 2;
					continue;
				}
				q = !q;
				p++;
				continue;
			}
			*o++ = *p++;
		}
		{
			char *end = o;
			int last = *p == 0;
			if (*p)
				p++;
			*end = 0;
			while (end > f[n - 1] && (end[-1] == ' ' || end[-1] == '\t'))
				*--end = 0;
			if (last)
				break;
		}
	}
	return n;
}

/* section's AddReg lines into the registry, HKR as hkr */
static void inf_addreg(const char *section, const char *hkr)
{
	char *at = NULL, *l;
	while ((l = inf_line(section, &at))) {
		char raw[1024], line[1024], *f[64], path[1024];
		int n, i;
		u32 flags;
		/* (a copy: inf_string's lines are in the same buffer) */
		snprintf(raw, sizeof(raw), "%s", l);
		inf_subst(raw, line, sizeof(line));
		n = inf_fields(line, f, 64);
		if (n < 3)
			continue;
		if (!strcasecmp(f[0], "HKR"))
			snprintf(path, sizeof(path), "%s%s%s", hkr, *f[1] ? "\\" : "", f[1]);
		else if (!strcasecmp(f[0], "HKLM"))
			snprintf(path, sizeof(path), "\\Registry\\Machine\\%s", f[1]);
		else
			continue;
		flags = n > 3 && *f[3] ? (u32)strtoul(f[3], NULL, 0) : 0;
		if ((flags & 0xffff0001) == 0x00000001) {      /* binary */
			byte b[64];
			int k = 0;
			for (i = 4; i < n && k < 64; i++)
				b[k++] = (byte)strtoul(f[i], NULL, 16);
			reg_set(path, f[2], REG_BINARY, b, (u32)k);
		} else if ((flags & 0xffff0001) == 0x00010001) {       /* DWORD */
			u32 v = n > 4 ? (u32)strtoul(f[4], NULL, 0) : 0;
			reg_set(path, f[2], REG_DWORD, &v, 4);
		} else if (flags & 0x00010000) {        /* multi-sz */
			wchar w[1024];
			int k = 0;
			for (i = 4; i < n; i++) {
				const char *s = f[i];
				while (*s && k < 1000)
					w[k++] = (byte)*s++;
				w[k++] = 0;
			}
			w[k++] = 0;
			reg_set(path, f[2], REG_MULTI_SZ, w, (u32)k * 2);
		} else
			reg_set_sz(path, f[2], flags & 0x00020000 ? REG_EXPAND_SZ : REG_SZ,
				   n > 4 ? f[4] : "");
	}
}

/* the INF at path read; -1 if it cannot be */
static int inf_load(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;
	if (!f) {
		perror(path);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	inf = calloc(1, (size_t)n + 1);
	if (fread(inf, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

/* s with its blanks at either end off */
static char *trim(char *s)
{
	size_t n;
	while (isspace((byte)*s))
		s++;
	n = strlen(s);
	while (n && isspace((byte)s[n - 1]))
		s[--n] = 0;
	return s;
}

/* model's description and maker, as the INF's [Manufacturer] and its
   models' sections (description = model, IDs) have them; -1 if none
   has the model */
static int inf_model(const char *model, char *desc, size_t dn, char *mfg, size_t mn)
{
	char *at = NULL, *l;
	while ((l = inf_line("Manufacturer", &at))) {
		/* (copies: inf_line()'s line is overwritten by the next) */
		char maker[512], *f[8], *eq, sect[128], *at2 = NULL, *l2;
		snprintf(maker, sizeof(maker), "%s", l);
		eq = strchr(maker, '=');
		if (!eq)
			continue;
		*eq = 0;
		inf_fields(eq + 1, f, 8);       /* models[, decorations] */
		snprintf(sect, sizeof(sect), "%s", f[0]);
		while ((l2 = inf_line(sect, &at2))) {
			char line[1024], *g[4], *e;
			snprintf(line, sizeof(line), "%s", l2);
			e = strchr(line, '=');
			if (!e)
				continue;
			*e = 0;
			inf_fields(e + 1, g, 4);        /* model, IDs */
			if (strcasecmp(g[0], model))
				continue;
			inf_subst(trim(line), desc, dn);
			inf_subst(trim(maker), mfg, mn);
			return 0;
		}
	}
	return -1;
}

/* model's AddReg sections into the registry (HKR the driver's key) */
static void inf_install(const char *model)
{
	char sect[128], *at = NULL, *l;
	snprintf(sect, sizeof(sect), "%s.NT", model);
	while ((l = inf_line(sect, &at))) {
		char line[1024], *fl[16];
		int k, nf;
		if (strncasecmp(l, "AddReg", 6))
			continue;
		snprintf(line, sizeof(line), "%s", strchr(l, '=') ? strchr(l, '=') + 1 : "");
		nf = inf_fields(line, fl, 16);
		for (k = 0; k < nf; k++) {
			char save[128];
			snprintf(save, sizeof(save), "%s", fl[k]);
			inf_addreg(save, key_driver);
		}
	}
}

/*
 *    PCI's configuration space (0xcf8, 0xcfc): the device's, on bus 0 at
 *    its devfn; the other ports are the device's
 */
static u32 cf8;
static u32 mem_phys;                    /* BAR 0, as the device starts */

static int cfg_ours(void)
{
	return (cf8 & 0x80000000) && ((cf8 >> 16) & 0xff) == 0 &&
		((cf8 >> 8) & 0xff) == wdev->devfn;
}

static u32 cfg_read(unsigned off, int size)
{
	u32 v;
	unsigned r = (cf8 & 0xfc) + off;
	if (!cfg_ours())
		return 0xffffffff;
	v = wdev->cfg[(r >> 2) & 63] >> (8 * (r & 3));
	return size == 4 ? v : size == 2 ? v & 0xffff : v & 0xff;
}

static void cfg_write(unsigned off, u32 v, int size)
{
	unsigned r = (cf8 & 0xfc) + off;
	u32 mask = size == 4 ? 0xffffffff : size == 2 ? 0xffff : 0xff;
	if (!cfg_ours())
		return;
	LOG("pci config %02x = %x\n", r, v);
	if (r == 0x10)          /* (BAR 0: its size in the bits that stay 0) */
		v = v == 0xffffffff ? ~(wdev->mem_size - 1) : (v & ~(wdev->mem_size - 1));
	wdev->cfg[(r >> 2) & 63] = (wdev->cfg[(r >> 2) & 63] & ~(mask << 8 * (r & 3))) |
		(v & mask) << 8 * (r & 3);
}

/* port I/O: PCI's configuration ports, the rest the device's */
static u32 io_in(unsigned a, int size)
{
	u32 v = 0xffffffff;
	if (a == 0xcf8)
		v = cf8;
	else if (a >= 0xcfc && a < 0xd00)
		v = cfg_read(a - 0xcfc, size);
	else if (wdev->io_in)
		v = wdev->io_in(a, size);
	else
		LOG("in%d %04x\n", size, a);
	if (size == 1)
		v &= 0xff;
	else if (size == 2)
		v &= 0xffff;
	if (win_debug > 1)
		fprintf(stderr, "win: in%d %04x = %x\n", size, a, v);
	return v;
}

static void io_out(unsigned a, u32 v, int size)
{
	if (win_debug > 1)
		fprintf(stderr, "win: out%d %04x = %x\n", size, a, v);
	if (a == 0xcf8 && size == 4)
		cf8 = v;
	else if (a >= 0xcfc && a < 0xd00)
		cfg_write(a - 0xcfc, v, size);
	else if (wdev->io_out)
		wdev->io_out(a, v, size);
	else
		LOG("out%d %04x = %x\n", size, a, v);
}

/*
 *    the register window's traps: an access faults (the window is not
 *    accessible), the instruction is decoded for what it does to memory
 *    (read, write, or both) and its size, the window opened with the
 *    register's value in it, and the instruction single-stepped (TF);
 *    its trap takes what it wrote to the device and closes the window
 */

#define MM_R 1
#define MM_W 2
static byte *mmio;                      /* the window: the driver's view */
static struct { int on, kind, size; u32 off; byte *ip; } mm;

/* what the instruction at p does to its memory operand: MM_R, MM_W or
   both; its size in *size; 0 if it is not one we know */
static int mm_decode(const byte *p, int *size)
{
	int opsz = 4, reg;
	for (;; p++) {
		if (*p == 0x66)
			opsz = 2;
		else if (*p != 0xf0 && *p != 0xf2 && *p != 0xf3 && *p != 0x2e &&
			 *p != 0x36 && *p != 0x3e && *p != 0x26 && *p != 0x64 && *p != 0x65)
			break;
	}
	reg = (p[1] >> 3) & 7;
	*size = opsz;
	switch (p[0]) {
	case 0x88: *size = 1; return MM_W;
	case 0x89: return MM_W;
	case 0x8a: *size = 1; return MM_R;
	case 0x8b: return MM_R;
	case 0xc6: *size = 1; return MM_W;
	case 0xc7: return MM_W;
	case 0xa0: *size = 1; return MM_R;
	case 0xa1: return MM_R;
	case 0xa2: *size = 1; return MM_W;
	case 0xa3: return MM_W;
	case 0x84: *size = 1; return MM_R;
	case 0x85: return MM_R;
	case 0x86: *size = 1; return MM_R | MM_W;
	case 0x87: return MM_R | MM_W;
	case 0x80: *size = 1; return reg == 7 ? MM_R : MM_R | MM_W;
	case 0x81: case 0x83: return reg == 7 ? MM_R : MM_R | MM_W;
	case 0xf6: *size = 1; return reg == 2 || reg == 3 ? MM_R | MM_W : MM_R;
	case 0xf7: return reg == 2 || reg == 3 ? MM_R | MM_W : MM_R;
	case 0xfe: *size = 1; return MM_R | MM_W;
	case 0xff: return reg <= 1 ? MM_R | MM_W : MM_R;
	case 0xd0: case 0xd2: case 0xc0: *size = 1; return MM_R | MM_W;
	case 0xd1: case 0xd3: case 0xc1: return MM_R | MM_W;
	case 0x0f:
		switch (p[1]) {
		case 0xb6: case 0xbe: *size = 1; return MM_R;
		case 0xb7: case 0xbf: *size = 2; return MM_R;
		case 0xa3: return MM_R;
		case 0xab: case 0xb3: case 0xbb: return MM_R | MM_W;
		case 0xba: return ((p[2] >> 3) & 7) == 4 ? MM_R : MM_R | MM_W;
		}
		return 0;
	}
	if (p[0] < 0x40 && (p[0] & 7) <= 3) {         /* the ALU's m,r and r,m */
		if (!(p[0] & 1))
			*size = 1;
		if ((p[0] & 0x38) == 0x38 || (p[0] & 2))
			return MM_R;                    /* cmp, or into a register */
		return MM_R | MM_W;
	}
	return 0;
}

/* an access of the window at a: the register's value there (read),
   the window open and the instruction stepped; 0, or -1 if a is not in
   the window */
static int mm_fault(ucontext_t *uc, byte *a)
{
	greg_t *g = uc->uc_mcontext.gregs;
	byte *ip = (byte *)(uintptr_t)g[REG_EIP];
	u32 v;
	if (!mmio || a < mmio || a >= mmio + wdev->mem_size)
		return -1;
	mm.kind = mm_decode(ip, &mm.size);
	if (!mm.kind) {
		fprintf(stderr, "win: register access by %02x %02x %02x at %p: not known\n",
			ip[0], ip[1], ip[2], (void *)ip);
		mm.kind = MM_R | MM_W;
		mm.size = 4;
	}
	mm.off = (u32)(a - mmio);
	mm.ip = ip;
	mprotect(mmio, wdev->mem_size, PROT_READ | PROT_WRITE);
	if (mm.kind & MM_R) {
		v = wdev->mem_read(mm.off, mm.size);
		memcpy(mmio + mm.off, &v, (size_t)mm.size);
		if (win_debug > 1)
			fprintf(stderr, "win: reg %04x r%d = %x (%p)\n", mm.off, mm.size, v,
				(void *)ip);
	}
	mm.on = 1;
	g[REG_EFL] |= 0x100;
	return 0;
}

/* the step done: what it wrote to the device, the window closed */
static void on_trap(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = ctx;
	(void)sig; (void)si;
	uc->uc_mcontext.gregs[REG_EFL] &= ~0x100;
	if (!mm.on)
		return;
	mm.on = 0;
	if (mm.kind & MM_W) {
		u32 v = 0;
		memcpy(&v, mmio + mm.off, (size_t)mm.size);
		if (win_debug > 1)
			fprintf(stderr, "win: reg %04x w%d = %x (%p)\n", mm.off, mm.size, v,
				(void *)mm.ip);
		wdev->mem_write(mm.off, mm.size, v);
	}
	mprotect(mmio, wdev->mem_size, PROT_NONE);
}

/* the driver's in and out (a general protection fault without I/O
   permission), cli and sti, rdtsc (PR_TSC_SIGSEGV) and its register
   window: carried out here and stepped over */
static void on_segv(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = (ucontext_t *)ctx;
	greg_t *g = uc->uc_mcontext.gregs;
	byte *ip = (byte *)(uintptr_t)g[REG_EIP];
	int size = 4, n = 1;
	unsigned port = (unsigned)g[REG_EDX] & 0xffff;

	if (si->si_code == SEGV_ACCERR && mm_fault(uc, si->si_addr) == 0)
		return;
	if ((byte *)si->si_addr == ip || (uintptr_t)ip < 0x10000) {
		fprintf(stderr, "win: a jump to %p (from around %p)\n", (void *)ip,
			*(void **)(uintptr_t)g[REG_ESP]);
		signal(sig, SIG_DFL);
		return;
	}
	if (ip[0] == 0x0f && (ip[1] == 0x20 || ip[1] == 0x22) && (ip[2] & 0xc0) == 0xc0) {
		/* mov from and to a control register: CR0 with TS clear
		   (the FPU ours to use), CR4 with OSFXSR */
		static u32 cr[8] = { 0x80050033, 0, 0, 0, 0x000006f0 };
		static const int greg[8] = { REG_EAX, REG_ECX, REG_EDX, REG_EBX,
					     REG_ESP, REG_EBP, REG_ESI, REG_EDI };
		int c = (ip[2] >> 3) & 7, r = greg[ip[2] & 7];
		if (ip[1] == 0x20)
			g[r] = (greg_t)cr[c];
		else
			cr[c] = (u32)g[r];
		g[REG_EIP] += 3;
		return;
	}
	if (ip[0] == 0x0f && ip[1] == 0x31) {
		unsigned long long t = tsc_read();
		g[REG_EAX] = (greg_t)(u32)t;
		g[REG_EDX] = (greg_t)(u32)(t >> 32);
		g[REG_EIP] += 2;
		return;
	}
	if (ip[0] == 0x66) {
		size = 2;
		ip++;
		n++;
	}
	switch (ip[0]) {
	case 0xfa:      /* cli, sti: one thread, nothing to keep out */
	case 0xfb:
		break;
	case 0xe4:
		g[REG_EAX] = (g[REG_EAX] & ~0xff) | io_in(ip[1], 1);
		n++;
		break;
	case 0xe5:
		g[REG_EAX] = size == 2 ? (g[REG_EAX] & ~0xffff) | io_in(ip[1], 2) :
			io_in(ip[1], 4);
		n++;
		break;
	case 0xe6:
		io_out(ip[1], g[REG_EAX] & 0xff, 1);
		n++;
		break;
	case 0xe7:
		io_out(ip[1], size == 2 ? g[REG_EAX] & 0xffff : (u32)g[REG_EAX],
		       size);
		n++;
		break;
	case 0xec:
		g[REG_EAX] = (g[REG_EAX] & ~0xff) | io_in(port, 1);
		break;
	case 0xed:
		if (size == 2)
			g[REG_EAX] = (g[REG_EAX] & ~0xffff) | io_in(port, 2);
		else
			g[REG_EAX] = io_in(port, 4);
		break;
	case 0xee:
		io_out(port, g[REG_EAX] & 0xff, 1);
		break;
	case 0xef:
		io_out(port, size == 2 ? g[REG_EAX] & 0xffff : (u32)g[REG_EAX],
		       size);
		break;
	default:
		fprintf(stderr, "win: fault at %p (%02x %02x %02x), address %p\n",
			(void *)ip, ip[0], ip[1], ip[2], si->si_addr);
		fprintf(stderr, "eax %x ebx %x ecx %x edx %x esi %x edi %x ebp %x esp %x\n",
			(u32)g[REG_EAX], (u32)g[REG_EBX], (u32)g[REG_ECX], (u32)g[REG_EDX],
			(u32)g[REG_ESI], (u32)g[REG_EDI], (u32)g[REG_EBP], (u32)g[REG_ESP]);
		signal(sig, SIG_DFL);
		return;
	}
	g[REG_EIP] += n;
}

/*
 *    the kernel: what the driver imports
 */

/* memory: the C library's, zeroed */
static void *NTAPI ExAllocatePoolWithTag(int type, u32 n, u32 tag)
{
	(void)type; (void)tag;
	return calloc(1, n + 16);
}
static void NTAPI ExFreePool(void *p) { free(p); }
static void *NTAPI MmLockPagableDataSection(void *a) { return a; }
static void NTAPI MmUnlockPagableImageSection(void *h) { (void)h; }
static int NTAPI MmQuerySystemSize(void) { return 2; }   /* large */
static void *NTAPI MmMapIoSpace(u64 pa, u32 n, int cache)
{
	(void)cache;
	if (pa >= mem_phys && pa + n <= mem_phys + wdev->mem_size) {
		if (!mmio) {
			mmio = mmap(NULL, wdev->mem_size, PROT_NONE,
				    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		}
		LOG("MmMapIoSpace %llx %u: the registers, %p\n", pa, n, (void *)mmio);
		return mmio + (pa - mem_phys);
	}
	LOG("MmMapIoSpace %llx %u: none\n", pa, n);
	return NULL;
}
static void NTAPI MmUnmapIoSpace(void *va, u32 n) { (void)va; (void)n; }
static void NTAPI MmUnlockPages(struct mdl *m) { (void)m; }
static void NTAPI IoFreeMdl(struct mdl *m) { free(m); }

/* HAL's port I/O: as the driver's own in and out */
static u32 NTAPI READ_PORT_ULONG(u32 *port) { return io_in((u32)(uintptr_t)port, 4); }
static void NTAPI WRITE_PORT_ULONG(u32 *port, u32 v) { io_out((u32)(uintptr_t)port, v, 4); }
static word NTAPI READ_PORT_USHORT(word *port) { return (word)io_in((u32)(uintptr_t)port, 2); }
static void NTAPI WRITE_PORT_USHORT(word *port, word v) { io_out((u32)(uintptr_t)port, v, 2); }
static byte NTAPI READ_PORT_UCHAR(byte *port) { return (byte)io_in((u32)(uintptr_t)port, 1); }
static void NTAPI WRITE_PORT_UCHAR(byte *port, byte v) { io_out((u32)(uintptr_t)port, v, 1); }

/* the C library of the kernel */
static void *__attribute__((cdecl)) k_memmove(void *d, const void *s, size_t n)
{
	return memmove(d, s, n);
}
static wchar *__attribute__((cdecl)) k_wcschr(const wchar *s, wchar c)
{
	for (;; s++) {
		if (*s == c)
			return (wchar *)s;
		if (!*s)
			return NULL;
	}
}

/* sprintf as Microsoft's C library has it: %S and %ws wide strings, %Z
   and %wZ counted ones, I64 */
static int ms_vsnprintf(char *out, size_t size, const char *fmt, va_list ap)
{
	size_t o = 0;
	char spec[32];
#define PUT(...) do { int _k = snprintf(out + o, o < size ? size - o : 0, __VA_ARGS__); \
		if (_k > 0) o += (size_t)_k; } while (0)
	while (*fmt) {
		const char *s = fmt;
		int k = 0, wide = 0, i64 = 0, h = 0;
		if (*fmt != '%') {
			if (o + 1 < size)
				out[o] = *fmt;
			o++;
			fmt++;
			continue;
		}
		fmt++;
		if (*fmt == '%') {
			if (o + 1 < size)
				out[o] = '%';
			o++;
			fmt++;
			continue;
		}
		spec[k++] = '%';
		while (strchr("-+ #0", *fmt) && k < 20)
			spec[k++] = *fmt++;
		if (*fmt == '*') {
			k += snprintf(spec + k, sizeof(spec) - (size_t)k, "%d", va_arg(ap, int));
			fmt++;
		} else
			while (isdigit((byte)*fmt) && k < 24)
				spec[k++] = *fmt++;
		if (*fmt == '.') {
			spec[k++] = *fmt++;
			if (*fmt == '*') {
				k += snprintf(spec + k, sizeof(spec) - (size_t)k, "%d", va_arg(ap, int));
				fmt++;
			} else
				while (isdigit((byte)*fmt) && k < 28)
					spec[k++] = *fmt++;
		}
		for (;;) {
			if (!strncmp(fmt, "I64", 3)) { i64 = 1; fmt += 3; }
			else if (!strncmp(fmt, "I32", 3)) fmt += 3;
			else if (!strncmp(fmt, "ll", 2)) { i64 = 1; fmt += 2; }
			else if (*fmt == 'l' || *fmt == 'L') fmt++;
			else if (*fmt == 'h') { h = 1; fmt++; }
			else if (*fmt == 'w') { wide = 1; fmt++; }
			else break;
		}
		switch (*fmt) {
		case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
			if (i64) {
				spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = *fmt; spec[k] = 0;
				PUT(spec, va_arg(ap, long long));
			} else {
				spec[k++] = *fmt; spec[k] = 0;
				PUT(spec, h ? (int)(short)va_arg(ap, int) : va_arg(ap, int));
			}
			break;
		case 'c': case 'C':
			spec[k++] = 'c'; spec[k] = 0;
			PUT(spec, va_arg(ap, int));
			break;
		case 's': case 'S': {
			int w = *fmt == 'S' ? !h : wide;
			spec[k++] = 's'; spec[k] = 0;
			if (w) {
				wchar *ws = va_arg(ap, wchar *);
				char *a = ws ? w2a(ws, -1) : strdup("(null)");
				PUT(spec, a);
				free(a);
			} else {
				const char *a = va_arg(ap, const char *);
				PUT(spec, a ? a : "(null)");
			}
			break;
		}
		case 'Z': {
			spec[k++] = 's'; spec[k] = 0;
			if (wide) {
				char *a = us2a(va_arg(ap, struct ustr *));
				PUT(spec, a);
				free(a);
			} else {
				struct astr *as = va_arg(ap, struct astr *);
				char *a = as && as->buf ? strndup(as->buf, as->len) : strdup("");
				PUT(spec, a);
				free(a);
			}
			break;
		}
		case 'p':
			PUT("%08X", va_arg(ap, u32));
			break;
		case 'e': case 'E': case 'f': case 'g': case 'G':
			spec[k++] = *fmt; spec[k] = 0;
			PUT(spec, va_arg(ap, double));
			break;
		case 'n':
			(void)va_arg(ap, int *);
			break;
		default:
			/* (not a conversion: as it was) */
			while (s <= fmt && *s) {
				if (o + 1 < size)
					out[o] = *s;
				o++;
				s++;
			}
			if (!*fmt)
				goto end;
			fmt++;
			continue;
		}
		if (*fmt)
			fmt++;
	}
end:
	if (size)
		out[o < size ? o : size - 1] = 0;
#undef PUT
	return (int)o;
}
static int __attribute__((cdecl)) k_sprintf(char *out, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = ms_vsnprintf(out, 4096, fmt, ap);
	va_end(ap);
	if (win_debug > 2)
		LOG("sprintf: %s\n", out);
	return n;
}

/* interlocked operations (one thread): fastcall */
static long FASTCALL InterlockedIncrement(long *p) { return ++*p; }
static long FASTCALL InterlockedDecrement(long *p) { return --*p; }
static long FASTCALL InterlockedExchange(long *p, long v) { long o = *p; *p = v; return o; }
static long FASTCALL InterlockedCompareExchange(long *p, long v, long cmp)
{
	long o = *p;
	if (o == cmp)
		*p = v;
	return o;
}

/* the IRQL and spin locks */
static byte NTAPI KeGetCurrentIrql(void) { return irql; }
static byte FASTCALL KfRaiseIrql(byte n) { byte o = irql; irql = n; return o; }
static void FASTCALL KfLowerIrql(byte n) { irql = n; }
static byte FASTCALL KfAcquireSpinLock(u32 *l) { byte o = irql; (void)l; irql = 2; return o; }
static void FASTCALL KfReleaseSpinLock(u32 *l, byte n) { (void)l; irql = n; }
static void NTAPI KeInitializeSpinLock(u32 *l) { *l = 0; }
static void NTAPI IoAcquireCancelSpinLock(byte *o) { *o = irql; irql = 2; }
static void NTAPI IoReleaseCancelSpinLock(byte n) { irql = n; }

/* strings */
static void NTAPI RtlInitUnicodeString(struct ustr *u, const wchar *s)
{
	u->buf = (wchar *)s;
	u->len = s ? (word)(wlen(s) * 2) : 0;
	u->max = s ? (word)(u->len + 2) : 0;
}
static void NTAPI RtlInitAnsiString(struct astr *a, const char *s)
{
	a->buf = (char *)s;
	a->len = s ? (word)strlen(s) : 0;
	a->max = s ? (word)(a->len + 1) : 0;
}
static NTSTATUS NTAPI RtlAppendUnicodeStringToString(struct ustr *d, const struct ustr *s)
{
	if (d->len + s->len > d->max)
		return STATUS_BUFFER_TOO_SMALL;
	memcpy((byte *)d->buf + d->len, s->buf, s->len);
	d->len += s->len;
	if (d->len + 2 <= d->max)
		d->buf[d->len / 2] = 0;
	return STATUS_SUCCESS;
}
static NTSTATUS NTAPI RtlAppendUnicodeToString(struct ustr *d, const wchar *s)
{
	struct ustr u;
	RtlInitUnicodeString(&u, s);
	return RtlAppendUnicodeStringToString(d, &u);
}
static NTSTATUS NTAPI RtlIntegerToUnicodeString(u32 v, u32 base, struct ustr *u)
{
	char b[40];
	int i, n;
	if (base == 16)
		n = snprintf(b, sizeof(b), "%x", v);
	else if (base == 8)
		n = snprintf(b, sizeof(b), "%o", v);
	else if (base == 2) {
		n = 0;
		for (i = 31; i >= 0; i--)
			if (n || (v >> i & 1) || i == 0)
				b[n++] = (char)('0' + (v >> i & 1));
		b[n] = 0;
	} else
		n = snprintf(b, sizeof(b), "%u", v);
	if ((n + 1) * 2 > u->max)
		return STATUS_BUFFER_OVERFLOW;
	for (i = 0; i <= n; i++)
		u->buf[i] = (byte)b[i];
	u->len = (word)(n * 2);
	return STATUS_SUCCESS;
}
static NTSTATUS NTAPI RtlUnicodeStringToInteger(const struct ustr *u, u32 base, u32 *v)
{
	char *a = us2a(u), *p = a;
	int neg = 0;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '-' || *p == '+')
		neg = *p++ == '-';
	if (!base) {
		base = 10;
		if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
		else if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) { base = 8; p += 2; }
		else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) { base = 2; p += 2; }
	}
	*v = (u32)strtoul(p, NULL, (int)base);
	if (neg)
		*v = -*v;
	free(a);
	return STATUS_SUCCESS;
}
static byte NTAPI RtlEqualUnicodeString(const struct ustr *a, const struct ustr *b, byte ci)
{
	int i;
	if (a->len != b->len)
		return 0;
	for (i = 0; i < a->len / 2; i++) {
		wchar x = a->buf[i], y = b->buf[i];
		if (ci && x < 0x80 && y < 0x80) {
			x = (wchar)toupper(x);
			y = (wchar)toupper(y);
		}
		if (x != y)
			return 0;
	}
	return 1;
}
static void NTAPI RtlFreeUnicodeString(struct ustr *u)
{
	free(u->buf);
	u->buf = NULL;
	u->len = u->max = 0;
}
static void NTAPI RtlFreeAnsiString(struct astr *a)
{
	free(a->buf);
	a->buf = NULL;
	a->len = a->max = 0;
}
static NTSTATUS NTAPI RtlAnsiStringToUnicodeString(struct ustr *d, const struct astr *s, byte alloc)
{
	int i;
	if (alloc) {
		d->max = (word)(s->len * 2 + 2);
		d->buf = malloc(d->max);
	} else if (s->len * 2 > d->max)
		return STATUS_BUFFER_OVERFLOW;
	for (i = 0; i < s->len; i++)
		d->buf[i] = (byte)s->buf[i];
	d->len = (word)(s->len * 2);
	if (d->len + 2 <= d->max)
		d->buf[i] = 0;
	return STATUS_SUCCESS;
}
static NTSTATUS NTAPI RtlUnicodeStringToAnsiString(struct astr *d, const struct ustr *s, byte alloc)
{
	int i, n = s->len / 2;
	if (alloc) {
		d->max = (word)(n + 1);
		d->buf = malloc(d->max);
	} else if (n > d->max)
		return STATUS_BUFFER_OVERFLOW;
	for (i = 0; i < n; i++)
		d->buf[i] = s->buf[i] < 0x80 ? (char)s->buf[i] : '?';
	d->len = (word)n;
	if (n + 1 <= d->max)
		d->buf[n] = 0;
	return STATUS_SUCCESS;
}
static u32 NTAPI RtlxAnsiStringToUnicodeSize(const struct astr *a) { return (u32)a->len * 2 + 2; }
static byte NlsMbCodePageTag;   /* (a data import: single byte code page) */

/* time */
/* (each read on by 1 us: a busy wait on the time ends, the line not
   going on in it) */
static u64 NTAPI KeQueryInterruptTime(void)
{
	u64 t = itime();
	itime_last = t + 10;
	return t;
}
static void NTAPI KeQuerySystemTime(u64 *t)
{
	*t = SYSTIME_BOOT + itime();
	itime_last = itime() + 10;
}
struct time_fields { short year, month, day, hour, minute, second, ms, weekday; };
static void NTAPI RtlTimeToTimeFields(const u64 *t, struct time_fields *f)
{
	u64 s = *t / 10000000ull;
	long days = (long)(s / 86400);
	long rem = (long)(s % 86400);
	long y = 1601;
	static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	int m = 0;
	f->ms = (short)((*t / 10000) % 1000);
	f->weekday = (short)((days + 1) % 7);   /* 1 Jan 1601: a Monday */
	f->hour = (short)(rem / 3600);
	f->minute = (short)(rem / 60 % 60);
	f->second = (short)(rem % 60);
	for (;;) {
		int leap = (y % 4 == 0 && y % 100) || y % 400 == 0;
		if (days < 365 + leap)
			break;
		days -= 365 + leap;
		y++;
	}
	for (m = 0; m < 12; m++) {
		int leap = m == 1 && ((y % 4 == 0 && y % 100) || y % 400 == 0);
		if (days < mdays[m] + leap)
			break;
		days -= mdays[m] + leap;
	}
	f->year = (short)y;
	f->month = (short)(m + 1);
	f->day = (short)(days + 1);
}

/* events */
static void NTAPI KeInitializeEvent(struct kevent *e, int type, byte state)
{
	e->h.type = (byte)type;
	e->h.size = sizeof(*e) / 4;
	e->h.signal = state;
	e->h.wait.flink = e->h.wait.blink = &e->h.wait;
}
static long NTAPI KeSetEvent(struct kevent *e, long incr, byte wait)
{
	long o = e->h.signal;
	(void)incr; (void)wait;
	e->h.signal = 1;
	return o;
}
static void NTAPI KeClearEvent(struct kevent *e) { e->h.signal = 0; }

/* DPCs */
static void NTAPI KeInitializeDpc(struct kdpc *d, void *routine, void *ctx)
{
	d->type = 0x13;
	d->number = d->importance = 0;
	d->routine = routine;
	d->ctx = ctx;
	d->lock = NULL;
}
static int KeInsertQueueDpc_(struct kdpc *d, void *a1, void *a2)
{
	if (d->lock)
		return 0;
	if (ndpc >= NDPC) {
		fprintf(stderr, "win: too many DPCs\n");
		return 0;
	}
	d->a1 = a1;
	d->a2 = a2;
	d->lock = d;
	dpcq[ndpc++] = d;
	return 1;
}
static byte NTAPI KeInsertQueueDpc(struct kdpc *d, void *a1, void *a2)
{
	return (byte)KeInsertQueueDpc_(d, a1, a2);
}
static byte NTAPI KeRemoveQueueDpc(struct kdpc *d)
{
	int i;
	for (i = 0; i < ndpc; i++)
		if (dpcq[i] == d) {
			memmove(dpcq + i, dpcq + i + 1, (size_t)(--ndpc - i) * sizeof(dpcq[0]));
			d->lock = NULL;
			return 1;
		}
	return 0;
}

/* timers */
static void NTAPI KeInitializeTimerEx(struct ktimer *t, int type)
{
	memset(t, 0, sizeof(*t));
	t->h.type = type ? OBJ_TIMER_SYNCHRONIZATION : OBJ_TIMER_NOTIFICATION;
	t->h.size = sizeof(*t) / 4;
	t->h.wait.flink = t->h.wait.blink = &t->h.wait;
}
static void NTAPI KeInitializeTimer(struct ktimer *t) { KeInitializeTimerEx(t, 0); }
static byte NTAPI KeCancelTimer(struct ktimer *t)
{
	int i;
	for (i = 0; i < NTIMER; i++)
		if (timers[i] == t) {
			timers[i] = NULL;
			t->h.inserted = 0;
			return 1;
		}
	return 0;
}
static byte NTAPI KeSetTimerEx(struct ktimer *t, long long due, long period, struct kdpc *dpc)
{
	byte was = KeCancelTimer(t);
	int i;
	for (i = 0; i < NTIMER && timers[i]; i++)
		;
	if (i == NTIMER) {
		fprintf(stderr, "win: too many timers\n");
		return was;
	}
	timers[i] = t;
	timer_due[i] = timeout_end(&due);
	t->h.signal = 0;
	t->h.inserted = 1;
	t->period = (int)period;
	t->dpc = dpc;
	return was;
}
static byte NTAPI KeSetTimer(struct ktimer *t, long long due, struct kdpc *dpc)
{
	return KeSetTimerEx(t, due, 0, dpc);
}

/* waits and threads */
static NTSTATUS NTAPI KeWaitForSingleObject(void *o, int reason, int mode, byte alertable,
					    long long *timeout)
{
	struct dispatcher *d = o;
	(void)reason; (void)mode; (void)alertable;
	return wait_for(&d, 1, 0, timeout_end(timeout));
}
static NTSTATUS NTAPI KeWaitForMultipleObjects(u32 n, void **o, int type, int reason,
					       int mode, byte alertable, long long *timeout,
					       void *blocks)
{
	(void)reason; (void)mode; (void)alertable; (void)blocks;
	return wait_for((struct dispatcher **)o, (int)n, type == 0, timeout_end(timeout));
}
static NTSTATUS NTAPI KeDelayExecutionThread(int mode, byte alertable, long long *interval)
{
	(void)mode; (void)alertable;
	return wait_for(NULL, 0, 0, timeout_end(interval)) == STATUS_TIMEOUT ?
		STATUS_SUCCESS : STATUS_SUCCESS;
}
static NTSTATUS NTAPI PsCreateSystemThread(void **handle, u32 access, void *oa, void *process,
					   void *client, void *start, void *arg)
{
	struct kthread *t;
	(void)access; (void)oa; (void)process; (void)client;
	if (nthreads == NTHREAD)
		return STATUS_NO_MEMORY;
	t = calloc(1, sizeof(*t));
	t->h.type = OBJ_THREAD;
	t->start = start;
	t->arg = arg;
	t->stack = mmap(NULL, 1 << 18, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	getcontext(&t->uc);
	t->uc.uc_stack.ss_sp = t->stack;
	t->uc.uc_stack.ss_size = 1 << 18;
	t->uc.uc_link = NULL;
	makecontext(&t->uc, thread_main, 0);
	threads[nthreads++] = t;
	*handle = t;
	LOG("PsCreateSystemThread %p (%p)\n", start, arg);
	return STATUS_SUCCESS;
}
static NTSTATUS NTAPI PsTerminateSystemThread(NTSTATUS s)
{
	struct kthread *t = cur_thread;
	(void)s;
	if (!t) {
		fprintf(stderr, "win: PsTerminateSystemThread not in a thread\n");
		exit(1);
	}
	t->dead = 1;
	swapcontext(&t->uc, &main_uc);
	return STATUS_SUCCESS;
}
static long NTAPI KeSetPriorityThread(void *t, long p)
{
	long o = 0;
	if (t) {
		o = ((struct kthread *)t)->prio;
		((struct kthread *)t)->prio = p;
	}
	return o;
}
static NTSTATUS NTAPI ObReferenceObjectByHandle(void *h, u32 access, void *type, int mode,
						void **o, void *info)
{
	(void)access; (void)type; (void)mode; (void)info;
	*o = h;
	return STATUS_SUCCESS;
}
static void FASTCALL ObfDereferenceObject(void *o) { (void)o; }
static NTSTATUS NTAPI ZwClose(void *h) { (void)h; return STATUS_SUCCESS; }
static void *NTAPI PsGetCurrentProcessId(void) { return (void *)4; }
static byte NTAPI PsGetVersion(u32 *major, u32 *minor, u32 *build, struct ustr *csd)
{
	if (major) *major = 5;
	if (minor) *minor = 1;
	if (build) *build = 2600;
	if (csd) { csd->len = 0; }
	return 0;
}
static byte NTAPI IoIsWdmVersionAvailable(byte major, byte minor)
{
	return major < 1 || (major == 1 && minor <= 0x20);
}

/*
 *    the I/O manager: devices, IRPs
 */

static struct devobj *top_of(struct devobj *d)
{
	while (d->attached)
		d = d->attached;
	return d;
}

/* what each device is attached to, its name */
#define NDEV 16
static struct { struct devobj *dev, *lower; char *name; } devs[NDEV];
static int ndevs;

static NTSTATUS NTAPI IoCreateDevice(struct drvobj *drv, u32 extsize, struct ustr *name,
				     u32 type, u32 chars, byte excl, struct devobj **out)
{
	struct devobj *d;
	(void)excl;
	if (ndevs == NDEV)
		return STATUS_NO_MEMORY;
	d = calloc(1, sizeof(*d) + extsize + 64);
	d->type = 3;
	d->size = (word)(sizeof(*d) + extsize);
	d->driver = drv;
	d->ext = extsize ? (byte *)(d + 1) : NULL;
	d->devtype = type;
	d->characteristics = chars;
	d->stacksize = 1;
	d->flags = DO_DEVICE_INITIALIZING;
	d->devobjext = calloc(1, 64);
	d->next = drv->devobj;
	drv->devobj = d;
	devs[ndevs].dev = d;
	devs[ndevs].name = name ? us2a(name) : NULL;
	ndevs++;
	LOG("IoCreateDevice %s type %x ext %u: %p\n", name ? devs[ndevs - 1].name : "-",
	    type, extsize, (void *)d);
	*out = d;
	return STATUS_SUCCESS;
}
static void NTAPI IoDeleteDevice(struct devobj *d)
{
	LOG("IoDeleteDevice %p\n", (void *)d);
}
static struct devobj *NTAPI IoAttachDeviceToDeviceStack(struct devobj *src, struct devobj *target)
{
	struct devobj *t = top_of(target);
	int i;
	t->attached = src;
	src->stacksize = (char)(t->stacksize + 1);
	for (i = 0; i < ndevs; i++)
		if (devs[i].dev == src)
			devs[i].lower = t;
	LOG("IoAttachDeviceToDeviceStack %p on %p\n", (void *)src, (void *)t);
	return t;
}
static void NTAPI IoDetachDevice(struct devobj *lower) { lower->attached = NULL; }
static NTSTATUS NTAPI IoCreateSymbolicLink(struct ustr *link, struct ustr *dev)
{
	if (win_debug) {
		char *a = us2a(link), *b = us2a(dev);
		LOG("symbolic link %s -> %s\n", a, b);
		free(a);
		free(b);
	}
	return STATUS_SUCCESS;
}
static NTSTATUS NTAPI IoDeleteSymbolicLink(struct ustr *link) { (void)link; return STATUS_SUCCESS; }

/* an IRP of n stack locations, zeroed */
static struct irp *NTAPI IoAllocateIrp(char n, byte quota)
{
	struct irp *irp = calloc(1, sizeof(struct irp) + (size_t)n * sizeof(struct iosl));
	(void)quota;
	irp->type = 6;
	irp->size = (word)(sizeof(struct irp) + (size_t)n * sizeof(struct iosl));
	irp->stackcount = n;
	irp->curloc = (char)(n + 1);
	irp->csl = (struct iosl *)(irp + 1) + n;
	return irp;
}
static void NTAPI IoFreeIrp(struct irp *irp) { free(irp); }
static struct iosl *next_sl(struct irp *irp) { return irp->csl - 1; }

typedef NTSTATUS NTAPI (*dispatch_fn)(struct devobj *d, struct irp *irp);
typedef NTSTATUS NTAPI (*completion_fn)(struct devobj *d, struct irp *irp, void *ctx);

static NTSTATUS FASTCALL IofCallDriver(struct devobj *d, struct irp *irp)
{
	struct iosl *sl;
	irp->curloc--;
	irp->csl--;
	if (irp->curloc <= 0) {
		fprintf(stderr, "win: IRP with no stack location left\n");
		exit(1);
	}
	sl = irp->csl;
	sl->dev = d;
	return ((dispatch_fn)d->driver->major[sl->major])(d, irp);
}
static NTSTATUS NTAPI PoCallDriver(struct devobj *d, struct irp *irp) { return IofCallDriver(d, irp); }

/* the IRPs the I/O manager built (IoBuild...): its marker */
static int threaded_irp;

/* an IRP done: its completion routines, from the current location up;
   then (built by IoBuild...) its buffer back, its status block, its
   event, and it freed */
static void FASTCALL IofCompleteRequest(struct irp *irp, char boost)
{
	(void)boost;
	while (irp->curloc <= irp->stackcount) {
		struct iosl *sl = irp->csl;
		NTSTATUS s = irp->iosb.status;
		void *routine = sl->completion, *ctx = sl->ctx;
		byte control = sl->control;
		struct devobj *d;
		irp->pending = control & SL_PENDING_RETURNED;
		irp->curloc++;
		irp->csl++;
		d = irp->curloc == irp->stackcount + 1 ? NULL : irp->csl->dev;
		memset(sl, 0, sizeof(*sl));
		if (routine && ((NT_SUCCESS(s) && (control & SL_INVOKE_ON_SUCCESS)) ||
				(!NT_SUCCESS(s) && (control & SL_INVOKE_ON_ERROR)) ||
				(irp->cancel && (control & SL_INVOKE_ON_CANCEL)))) {
			if (((completion_fn)routine)(d, irp, ctx) == STATUS_MORE_PROCESSING_REQUIRED)
				return;
		} else if (irp->pending && irp->curloc <= irp->stackcount)
			irp->csl->control |= SL_PENDING_RETURNED;
	}
	if (irp->thread == &threaded_irp) {
		if ((irp->flags & IRP_BUFFERED_IO) && (irp->flags & IRP_INPUT_OPERATION) &&
		    irp->userbuf && irp->sysbuf && NT_SUCCESS(irp->iosb.status))
			memcpy(irp->userbuf, irp->sysbuf, irp->iosb.info);
		if (irp->useriosb)
			*irp->useriosb = irp->iosb;
		if (irp->userevent)
			KeSetEvent(irp->userevent, 0, 0);
		if (irp->flags & IRP_DEALLOCATE_BUFFER)
			free(irp->sysbuf);
		free(irp->mdl);
		free(irp);
	}
}

static struct irp *NTAPI IoBuildDeviceIoControlRequest(u32 code, struct devobj *d, void *in,
							u32 inlen, void *out, u32 outlen,
							byte internal, struct kevent *ev,
							struct iosb *iosb)
{
	struct irp *irp = IoAllocateIrp(d->stacksize, 0);
	struct iosl *sl = next_sl(irp);
	sl->major = internal ? IRP_MJ_INTERNAL_DEVICE_CONTROL : IRP_MJ_DEVICE_CONTROL;
	sl->p[0] = outlen;
	sl->p[1] = inlen;
	sl->p[2] = code;
	switch (code & 3) {
	case 0:         /* buffered */
		irp->sysbuf = calloc(1, (inlen > outlen ? inlen : outlen) + 16);
		if (in && inlen)
			memcpy(irp->sysbuf, in, inlen);
		irp->flags = IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER |
			(outlen ? IRP_INPUT_OPERATION : 0);
		irp->userbuf = out;
		break;
	case 3:         /* neither */
		sl->p[3] = (u32)(uintptr_t)in;
		irp->userbuf = out;
		break;
	default:        /* direct: the input buffered, the output an MDL */
		if (in && inlen) {
			irp->sysbuf = calloc(1, inlen + 16);
			memcpy(irp->sysbuf, in, inlen);
			irp->flags = IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER;
		}
		if (out && outlen) {
			struct mdl *m = calloc(1, sizeof(*m) + 64);
			m->startva = (void *)((uintptr_t)out & ~4095u);
			m->offset = (u32)(uintptr_t)out & 4095;
			m->count = outlen;
			m->mapped = out;
			m->flags = 0x0001 | 0x0002;     /* mapped, pages locked */
			irp->mdl = m;
		}
		irp->userbuf = out;
	}
	irp->userevent = ev;
	irp->useriosb = iosb;
	irp->thread = &threaded_irp;
	irp->iosb.status = STATUS_NOT_SUPPORTED;
	LOG("IoBuildDeviceIoControlRequest %x to %p\n", code, (void *)d);
	return irp;
}
static struct irp *NTAPI IoBuildSynchronousFsdRequest(u32 major, struct devobj *d, void *buf,
						       u32 len, long long *offset,
						       struct kevent *ev, struct iosb *iosb)
{
	struct irp *irp = IoAllocateIrp(d->stacksize, 0);
	struct iosl *sl = next_sl(irp);
	sl->major = (byte)major;
	if (major == IRP_MJ_READ || major == IRP_MJ_WRITE) {
		sl->p[0] = len;
		if (offset)
			memcpy(&sl->p[2], offset, 8);
		if (d->flags & DO_BUFFERED_IO) {
			irp->sysbuf = calloc(1, len + 16);
			if (major == IRP_MJ_WRITE)
				memcpy(irp->sysbuf, buf, len);
			irp->flags = IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER |
				(major == IRP_MJ_READ ? IRP_INPUT_OPERATION : 0);
		}
		irp->userbuf = buf;
	}
	irp->userevent = ev;
	irp->useriosb = iosb;
	irp->thread = &threaded_irp;
	irp->iosb.status = major == IRP_MJ_PNP ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
	LOG("IoBuildSynchronousFsdRequest %x to %p\n", major, (void *)d);
	return irp;
}
typedef void NTAPI (*cancel_fn)(struct devobj *d, struct irp *irp);
static byte NTAPI IoCancelIrp(struct irp *irp)
{
	cancel_fn c = irp->cancelroutine;
	irp->cancel = 1;
	if (!c)
		return 0;
	irp->cancelroutine = NULL;
	IoAcquireCancelSpinLock(&irp->cancelirql);
	c(irp->csl->dev, irp);
	return 1;
}
static struct devobj *NTAPI IoGetRelatedDeviceObject(void *fileobj)
{
	return *(struct devobj **)((byte *)fileobj + 4);
}
static NTSTATUS NTAPI IoGetDeviceObjectPointer(struct ustr *name, u32 access, void **fo,
					       struct devobj **d)
{
	char *a = us2a(name);
	(void)access; (void)fo; (void)d;
	LOG("IoGetDeviceObjectPointer %s: none\n", a);
	free(a);
	return STATUS_OBJECT_NAME_NOT_FOUND;
}

/* interrupts */
typedef byte NTAPI (*isr_fn)(void *intr, void *ctx);
static isr_fn isr;
static void *isr_ctx;
static byte intr_obj[64];
static NTSTATUS NTAPI IoConnectInterrupt(void **out, void *routine, void *ctx, u32 *lock,
					 u32 vector, byte irq_l, byte sync_l, int mode,
					 byte share, u32 affinity, byte fsave)
{
	(void)lock; (void)share; (void)affinity; (void)fsave;
	LOG("IoConnectInterrupt vector %u irql %u/%u mode %d\n", vector, irq_l, sync_l, mode);
	isr = (isr_fn)routine;
	isr_ctx = ctx;
	*out = intr_obj;
	return STATUS_SUCCESS;
}
static void NTAPI IoDisconnectInterrupt(void *i) { (void)i; isr = NULL; }
static byte NTAPI KeSynchronizeExecution(void *i, void *routine, void *ctx)
{
	byte o = irql, r;
	(void)i;
	irql = 0x1c;
	r = ((byte NTAPI (*)(void *))routine)(ctx);
	irql = o;
	return r;
}

/* DMA: common buffers, their bus address the virtual one */
static void NTAPI dma_put(struct dma_adapter *a) { (void)a; }
static void *NTAPI dma_alloc(struct dma_adapter *a, u32 n, u64 *bus, byte cache)
{
	void *p = NULL;
	(void)a; (void)cache;
	if (posix_memalign(&p, 4096, n ? n : 1))
		return NULL;
	memset(p, 0, n);
	*bus = (u32)(uintptr_t)p;
	LOG("AllocateCommonBuffer %u: %p\n", n, p);
	return p;
}
static void NTAPI dma_free(struct dma_adapter *a, u32 n, u64 bus, void *va, byte cache)
{
	(void)a; (void)n; (void)bus; (void)cache;
	free(va);
}
static void NTAPI dma_other(void) { fprintf(stderr, "win: a DMA operation we do not have\n"); exit(1); }
static void *dma_ops[16] = {
	(void *)(uintptr_t)64, dma_put, dma_alloc, dma_free,
};
static struct dma_adapter dma = { 1, 8, dma_ops };
static struct dma_adapter *NTAPI IoGetDmaAdapter(struct devobj *pdo, void *desc, u32 *nmap)
{
	int i;
	(void)pdo; (void)desc;
	for (i = 4; i < 16; i++)
		dma_ops[i] = dma_other;
	*nmap = 16;
	LOG("IoGetDmaAdapter\n");
	return &dma;
}

/* PnP, power, WMI, interfaces: what the PnP manager knows of the
   device (win_start()): its hardware and compatible IDs (from its
   configuration space, '\0' between them and two at the end), its
   instance, and from the INF its description, maker, class and class
   GUID */
static char hw_ids[256], compat_ids[256], instance[32];
static char dev_desc[256], dev_mfg[256], dev_class[64], class_guid[64], drv_key[80];

static NTSTATUS NTAPI IoGetDeviceProperty(struct devobj *d, int prop, u32 n, void *buf, u32 *res)
{
	byte tmp[512];
	u32 len = 0;
	const char *s = NULL;
	int multi = 0;
	(void)d;
	switch (prop) {
	case 0: s = dev_desc; break;                    /* description */
	case 1: s = hw_ids; multi = 1; break;
	case 2: s = compat_ids; multi = 1; break;
	case 5: s = dev_desc; break;                    /* friendly name */
	case 7: s = dev_class; break;                   /* class name */
	case 8: s = class_guid; break;
	case 9: s = dev_mfg; break;                     /* manufacturer */
	case 10: s = "\\Device\\NTPNP_PCI0007"; break;   /* PDO name */
	case 11: s = drv_key; break;
	case 13: len = 4; memcpy(tmp, &(u32){ 5 }, 4); break;           /* PCIBus */
	case 14: len = 4; memcpy(tmp, &(u32){ 0 }, 4); break;           /* bus 0 */
	case 15: len = 4; memcpy(tmp, &(u32){ (wdev->devfn >> 3) << 16 | (wdev->devfn & 7) }, 4); break;
	case 22: s = "PCI"; break;                        /* enumerator */
	default:
		LOG("IoGetDeviceProperty %d: none\n", prop);
		return STATUS_INVALID_PARAMETER;
	}
	if (s) {
		size_t k = 0;
		const char *p = s;
		for (;;) {
			size_t l = strlen(p);
			size_t i;
			for (i = 0; i <= l && k + 2 <= sizeof(tmp); i++, k += 2) {
				tmp[k] = (byte)p[i];
				tmp[k + 1] = 0;
			}
			if (!multi)
				break;
			p += l + 1;
			if (!*p) {
				tmp[k] = tmp[k + 1] = 0;
				k += 2;
				break;
			}
		}
		len = (u32)k;
	}
	if (res)
		*res = len;
	LOG("IoGetDeviceProperty %d (%u bytes)\n", prop, len);
	if (n < len)
		return STATUS_BUFFER_TOO_SMALL;
	memcpy(buf, tmp, len);
	return STATUS_SUCCESS;
}
static NTSTATUS NTAPI IoRegisterDeviceInterface(struct devobj *pdo, void *guid, struct ustr *ref,
						struct ustr *link)
{
	const byte *g = guid;
	char s[256];
	(void)pdo; (void)ref;
	snprintf(s, sizeof(s), "\\??\\PCI#VEN_%04X&DEV_%04X#%s#"
		 "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
		 wdev->cfg[0] & 0xffff, wdev->cfg[0] >> 16, instance, rd32(g),
		 rd16(g + 4), rd16(g + 6), g[8], g[9], g[10], g[11], g[12], g[13],
		 g[14], g[15]);
	RtlInitUnicodeString(link, a2w(s));
	return STATUS_SUCCESS;
}
static NTSTATUS NTAPI IoSetDeviceInterfaceState(struct ustr *link, byte on)
{
	(void)link;
	LOG("IoSetDeviceInterfaceState %d\n", on);
	return STATUS_SUCCESS;
}
static NTSTATUS NTAPI IoGetDeviceInterfaces(void *guid, struct devobj *pdo, u32 flags, wchar **list)
{
	(void)guid; (void)pdo; (void)flags;
	*list = calloc(1, 4);
	return STATUS_SUCCESS;
}
static NTSTATUS NTAPI IoRegisterPlugPlayNotification(int category, u32 flags, void *data,
						     struct drvobj *drv, void *cb, void *ctx,
						     void **entry)
{
	(void)data; (void)drv; (void)cb; (void)ctx;
	LOG("IoRegisterPlugPlayNotification %d %x\n", category, flags);
	*entry = calloc(1, 16);
	return STATUS_SUCCESS;
}
static NTSTATUS NTAPI IoUnregisterPlugPlayNotification(void *entry) { (void)entry; return STATUS_SUCCESS; }
static NTSTATUS NTAPI IoWMIRegistrationControl(struct devobj *d, u32 action) { (void)d; (void)action; return STATUS_SUCCESS; }
static NTSTATUS NTAPI WmiSystemControl(void *info, struct devobj *d, struct irp *irp, int *disp)
{
	(void)info; (void)d; (void)irp;
	*disp = 2;      /* IrpForward */
	return STATUS_SUCCESS;
}
static NTSTATUS NTAPI WmiCompleteRequest(struct devobj *d, struct irp *irp, NTSTATUS s, u32 n, char boost)
{
	(void)d; (void)n;
	irp->iosb.status = s;
	IofCompleteRequest(irp, boost);
	return s;
}
static NTSTATUS NTAPI WmiFireEvent(struct devobj *d, void *guid, u32 inst, u32 n, void *data)
{
	(void)d; (void)guid; (void)inst; (void)n;
	free(data);
	return STATUS_SUCCESS;
}
static void *NTAPI PoRegisterSystemState(void *h, u32 flags) { (void)flags; return h ? h : calloc(1, 4); }
static void NTAPI PoUnregisterSystemState(void *h) { (void)h; }
static u32 NTAPI PoSetPowerState(struct devobj *d, int type, u32 state) { (void)d; (void)type; (void)state; return 1; }
static void NTAPI PoStartNextPowerIrp(struct irp *irp) { (void)irp; }
typedef void NTAPI (*power_done_fn)(struct devobj *d, byte minor, u32 state, void *ctx,
				     struct iosb *iosb);
static NTSTATUS NTAPI PoRequestPowerIrp(struct devobj *pdo, byte minor, u32 state, void *cb,
					void *ctx, struct irp **out)
{
	struct devobj *top = top_of(pdo);
	struct irp *irp = IoAllocateIrp(top->stacksize, 0);
	struct iosl *sl = next_sl(irp);
	struct iosb iosb;
	LOG("PoRequestPowerIrp %u D%u\n", minor, state - 1);
	sl->major = IRP_MJ_POWER;
	sl->minor = minor;
	sl->p[1] = 1;                   /* DevicePowerState */
	sl->p[2] = state;
	irp->iosb.status = STATUS_SUCCESS;
	if (out)
		*out = irp;
	IofCallDriver(top, irp);
	iosb = irp->iosb;
	if (cb)
		((power_done_fn)cb)(pdo, minor, state, ctx, &iosb);
	free(irp);
	return STATUS_PENDING;
}

/* kernel streaming (a speakerphone's): none */
static NTSTATUS NTAPI KsCreatePin(void *filter, void *connect, u32 access, void **pin)
{
	(void)filter; (void)connect; (void)access; (void)pin;
	return STATUS_NOT_SUPPORTED;
}
static NTSTATUS NTAPI KsSynchronousIoControlDevice(void *fo, int mode, u32 code, void *in,
						   u32 inlen, void *out, u32 outlen, u32 *ret)
{
	(void)fo; (void)mode; (void)code; (void)in; (void)inlen; (void)out; (void)outlen;
	(void)ret;
	return STATUS_NOT_SUPPORTED;
}

/* files (a driver's log): none */
static NTSTATUS NTAPI ZwCreateFile(void **h, u32 access, struct objattr *oa, struct iosb *iosb,
				   void *size, u32 attr, u32 share, u32 disp, u32 opts,
				   void *ea, u32 ealen)
{
	char *a = oa && oa->name ? us2a(oa->name) : strdup("?");
	(void)h; (void)access; (void)iosb; (void)size; (void)attr; (void)share; (void)disp;
	(void)opts; (void)ea; (void)ealen;
	LOG("ZwCreateFile %s: denied\n", a);
	free(a);
	return STATUS_ACCESS_DENIED;
}
static NTSTATUS NTAPI ZwWriteFile(void *h, void *ev, void *apc, void *ctx, struct iosb *iosb,
				  void *buf, u32 n, long long *off, u32 *key)
{
	(void)h; (void)ev; (void)apc; (void)ctx; (void)buf; (void)off; (void)key;
	if (iosb) {
		iosb->status = STATUS_SUCCESS;
		iosb->info = n;
	}
	return STATUS_SUCCESS;
}

/*
 *    the registry's calls
 */

/* h one of our keys? */
static int key_ok(void *h)
{
	struct rkey *k;
	for (k = rkeys; k; k = k->next)
		if (k == h)
			return 1;
	LOG("registry handle %p: not a key\n", h);
	return 0;
}
#define STATUS_INVALID_HANDLE ((NTSTATUS)0xc0000008)

/* the key oa names (relative to its root, if it has one) */
static char *oa_path(struct objattr *oa)
{
	char *name = us2a(oa->name), *p;
	if (oa->root && key_ok(oa->root)) {
		struct rkey *k = oa->root;
		size_t n = strlen(k->path) + strlen(name) + 2;
		p = malloc(n);
		snprintf(p, n, "%s%s%s", k->path, *name ? "\\" : "", name);
		free(name);
		return p;
	}
	return name;
}
static NTSTATUS NTAPI ZwOpenKey(void **h, u32 access, struct objattr *oa)
{
	char *p = oa_path(oa);
	struct rkey *k = reg_key(p, 0);
	(void)access;
	LOG("ZwOpenKey %s%s\n", p, k ? "" : ": none");
	free(p);
	if (!k)
		return STATUS_OBJECT_NAME_NOT_FOUND;
	*h = k;
	return STATUS_SUCCESS;
}
static NTSTATUS NTAPI ZwCreateKey(void **h, u32 access, struct objattr *oa, u32 title,
				  struct ustr *cls, u32 opts, u32 *disp)
{
	char *p = oa_path(oa);
	int was = reg_key(p, 0) != NULL;
	(void)access; (void)title; (void)cls; (void)opts;
	*h = reg_key(p, 1);
	if (disp)
		*disp = was ? 2 : 1;
	LOG("ZwCreateKey %s\n", p);
	free(p);
	return STATUS_SUCCESS;
}
static NTSTATUS NTAPI IoOpenDeviceRegistryKey(struct devobj *d, u32 which, u32 access, void **h)
{
	(void)d; (void)access;
	*h = reg_key(which & 2 ? key_driver : key_device, 1);
	LOG("IoOpenDeviceRegistryKey %s\n", which & 2 ? "driver" : "device");
	return STATUS_SUCCESS;
}

/* value v as KEY_VALUE_*_INFORMATION class c into buf (n bytes) */
static NTSTATUS value_info(struct rval *v, int c, byte *buf, u32 n, u32 *res)
{
	u32 namelen = (u32)strlen(v->name) * 2, need, i;
	switch (c) {
	case 0:         /* basic: title, type, name length, name */
		need = 12 + namelen;
		break;
	case 1:         /* full: title, type, data offset, data length, name
			   length, name, data */
		need = 20 + namelen + v->len;
		break;
	case 2:         /* partial: title, type, data length, data */
		need = 12 + v->len;
		break;
	default:
		return STATUS_INVALID_PARAMETER;
	}
	if (res)
		*res = need;
	if (n < need)
		return n < 12 ? STATUS_BUFFER_TOO_SMALL : STATUS_BUFFER_OVERFLOW;
	memset(buf, 0, need);
	memcpy(buf + 4, &v->type, 4);
	if (c == 0) {
		memcpy(buf + 8, &namelen, 4);
		for (i = 0; i < namelen / 2; i++)
			buf[12 + 2 * i] = (byte)v->name[i];
	} else if (c == 1) {
		u32 off = 20 + namelen;
		memcpy(buf + 8, &off, 4);
		memcpy(buf + 12, &v->len, 4);
		memcpy(buf + 16, &namelen, 4);
		for (i = 0; i < namelen / 2; i++)
			buf[20 + 2 * i] = (byte)v->name[i];
		memcpy(buf + off, v->data, v->len);
	} else {
		memcpy(buf + 8, &v->len, 4);
		memcpy(buf + 12, v->data, v->len);
	}
	return STATUS_SUCCESS;
}
static NTSTATUS NTAPI ZwQueryValueKey(void *h, struct ustr *name, int c, void *buf, u32 n, u32 *res)
{
	char *a = us2a(name);
	struct rval *v;
	if (!key_ok(h)) {
		LOG("ZwQueryValueKey %s: bad handle (from %p)\n", a, __builtin_return_address(0));
		free(a);
		return STATUS_INVALID_HANDLE;
	}
	v = reg_val(h, a, 0);
	LOG("ZwQueryValueKey %s\\%s%s\n", ((struct rkey *)h)->path, a, v ? "" : ": none");
	free(a);
	if (!v)
		return STATUS_OBJECT_NAME_NOT_FOUND;
	return value_info(v, c, buf, n, res);
}
static NTSTATUS NTAPI ZwEnumerateValueKey(void *h, u32 i, int c, void *buf, u32 n, u32 *res)
{
	struct rval *v;
	if (!key_ok(h))
		return STATUS_INVALID_HANDLE;
	v = ((struct rkey *)h)->vals;
	while (v && i--)
		v = v->next;
	if (!v)
		return STATUS_NO_MORE_ENTRIES;
	return value_info(v, c, buf, n, res);
}
static NTSTATUS NTAPI ZwSetValueKey(void *h, struct ustr *name, u32 title, u32 type, void *data,
				    u32 n)
{
	char *a = us2a(name);
	struct rval *v;
	(void)title;
	if (!key_ok(h)) {
		free(a);
		return STATUS_INVALID_HANDLE;
	}
	v = reg_val(h, a, 1);
	LOG("ZwSetValueKey %s\\%s type %u (%u bytes)\n", ((struct rkey *)h)->path, a, type, n);
	free(a);
	free(v->data);
	v->type = type;
	v->data = malloc(n + 2);
	memcpy(v->data, data, n);
	v->data[n] = v->data[n + 1] = 0;
	v->len = n;
	return STATUS_SUCCESS;
}
/* the i-th subkey of k (by creation), or NULL */
static struct rkey *subkey(struct rkey *k, u32 i)
{
	struct rkey *s;
	size_t n = strlen(k->path);
	u32 at = 0;
	for (s = rkeys; s; s = s->next)
		if (strlen(s->path) > n + 1 && !strncasecmp(s->path, k->path, n) &&
		    s->path[n] == '\\' && !strchr(s->path + n + 1, '\\')) {
			if (at++ == i)
				return s;
		}
	return NULL;
}
static NTSTATUS key_info(struct rkey *k, int c, byte *buf, u32 n, u32 *res, int full)
{
	const char *name = strrchr(k->path, '\\') ? strrchr(k->path, '\\') + 1 : k->path;
	u32 namelen = (u32)strlen(name) * 2, need, i;
	(void)full;
	switch (c) {
	case 0:         /* basic: time, title, name length, name */
		need = 16 + namelen;
		break;
	case 1:         /* node: time, title, class offset and length, name
			   length, name */
		need = 24 + namelen;
		break;
	case 2: {       /* full: time, title, class offset, class length,
			   subkeys, max name, max class, values, max value
			   name, max value data */
		u32 nsub = 0, nval = 0;
		struct rval *v;
		while (subkey(k, nsub))
			nsub++;
		for (v = k->vals; v; v = v->next)
			nval++;
		need = 44;
		if (res)
			*res = need;
		if (n < need)
			return STATUS_BUFFER_TOO_SMALL;
		memset(buf, 0, need);
		memcpy(buf + 20, &nsub, 4);
		memcpy(buf + 24, &(u32){ 256 }, 4);
		memcpy(buf + 32, &nval, 4);
		memcpy(buf + 36, &(u32){ 256 }, 4);
		memcpy(buf + 40, &(u32){ 4096 }, 4);
		return STATUS_SUCCESS;
	}
	default:
		return STATUS_INVALID_PARAMETER;
	}
	if (res)
		*res = need;
	if (n < need)
		return STATUS_BUFFER_OVERFLOW;
	memset(buf, 0, need);
	if (c == 0) {
		memcpy(buf + 12, &namelen, 4);
		for (i = 0; i < namelen / 2; i++)
			buf[16 + 2 * i] = (byte)name[i];
	} else {
		u32 off = 0xffffffff;
		memcpy(buf + 12, &off, 4);
		memcpy(buf + 20, &namelen, 4);
		for (i = 0; i < namelen / 2; i++)
			buf[24 + 2 * i] = (byte)name[i];
	}
	return STATUS_SUCCESS;
}
static NTSTATUS NTAPI ZwEnumerateKey(void *h, u32 i, int c, void *buf, u32 n, u32 *res)
{
	struct rkey *s;
	if (!key_ok(h))
		return STATUS_INVALID_HANDLE;
	s = subkey(h, i);
	if (!s)
		return STATUS_NO_MORE_ENTRIES;
	return key_info(s, c, buf, n, res, 0);
}
static NTSTATUS NTAPI ZwQueryKey(void *h, int c, void *buf, u32 n, u32 *res)
{
	if (!key_ok(h))
		return STATUS_INVALID_HANDLE;
	return key_info(h, c, buf, n, res, 1);
}

/* RtlQueryRegistryValues's and RtlWriteRegistryValue's paths */
static char *rtl_path(u32 rel, const wchar *path)
{
	static const char *const roots[] = {
		"", "\\Registry\\Machine\\System\\CurrentControlSet\\Services\\",
		"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\",
		"\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion\\",
		"\\Registry\\Machine\\Hardware\\DeviceMap\\", "\\Registry\\User\\.Default\\",
	};
	char *a, *p;
	size_t n;
	if (rel & 0x40000000)           /* RTL_REGISTRY_HANDLE */
		return strdup(((struct rkey *)path)->path);
	a = w2a(path, -1);
	rel &= 0xffff;
	if (rel > 5)
		rel = 0;
	n = strlen(roots[rel]) + strlen(a) + 1;
	p = malloc(n);
	snprintf(p, n, "%s%s", roots[rel], a);
	free(a);
	return p;
}

typedef NTSTATUS NTAPI (*query_fn)(wchar *name, u32 type, void *data, u32 n, void *ctx,
				    void *entryctx);
#define RTL_QUERY_REGISTRY_SUBKEY 0x01
#define RTL_QUERY_REGISTRY_TOPKEY 0x02
#define RTL_QUERY_REGISTRY_REQUIRED 0x04
#define RTL_QUERY_REGISTRY_NOVALUE 0x08
#define RTL_QUERY_REGISTRY_DIRECT 0x20

/* a value for a DIRECT entry: strings into its UNICODE_STRING, up to 4
   bytes as they are, more into a buffer whose first long is minus its
   size */
static void direct_value(void *ctx, u32 type, const byte *data, u32 n)
{
	if (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ) {
		struct ustr *u = ctx;
		if (!u->buf) {
			u->max = (word)(n + 2);
			u->buf = calloc(1, u->max);
		}
		if (n > u->max)
			n = u->max;
		memcpy(u->buf, data, n);
		u->len = (word)(n >= 2 && !data[n - 2] && !data[n - 1] ? n - 2 : n);
	} else if (n <= 4) {
		memset(ctx, 0, 4);
		memcpy(ctx, data, n);
	} else {
		long size = *(long *)ctx;
		if (size < 0 && (u32)-size >= n) {
			memcpy(ctx, data, n);
		} else if (size < 0 && (u32)-size >= n + 8) {
			memcpy((byte *)ctx + 8, data, n);
		}
	}
}

static NTSTATUS NTAPI RtlQueryRegistryValues(u32 rel, const wchar *path, struct rtlq *q,
					     void *ctx, void *env)
{
	char *base = rtl_path(rel, path), *kpath = strdup(base);
	struct rkey *k = reg_key(kpath, 0);
	NTSTATUS s = STATUS_SUCCESS;
	(void)env;
	LOG("RtlQueryRegistryValues %s%s\n", base, k ? "" : ": none");
	if (!k) {
		free(base);
		free(kpath);
		return rel & 0x80000000 ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
	}
	for (; q->routine || q->name; q++) {
		char *name;
		struct rval *v;
		if (q->flags & (RTL_QUERY_REGISTRY_SUBKEY | RTL_QUERY_REGISTRY_TOPKEY)) {
			free(kpath);
			if ((q->flags & RTL_QUERY_REGISTRY_SUBKEY) && q->name) {
				char *sub = w2a(q->name, -1);
				size_t n = strlen(base) + strlen(sub) + 2;
				kpath = malloc(n);
				snprintf(kpath, n, "%s\\%s", base, sub);
				free(sub);
			} else
				kpath = strdup(base);
			k = reg_key(kpath, 0);
			if (!k) {
				s = STATUS_OBJECT_NAME_NOT_FOUND;
				break;
			}
			if (!q->routine && !(q->flags & RTL_QUERY_REGISTRY_DIRECT))
				continue;
		}
		if (!q->name || (q->flags & RTL_QUERY_REGISTRY_NOVALUE)) {
			/* each value to the routine */
			if (q->routine && !(q->flags & RTL_QUERY_REGISTRY_NOVALUE)) {
				for (v = k->vals; v; v = v->next) {
					wchar *w = a2w(v->name);
					s = ((query_fn)q->routine)(w, v->type, v->data, v->len, ctx,
								   q->entryctx);
					free(w);
					if (!NT_SUCCESS(s))
						break;
				}
			} else if (q->routine)
				s = ((query_fn)q->routine)(q->name, REG_NONE, NULL, 0, ctx,
							   q->entryctx);
			if (!NT_SUCCESS(s))
				break;
			continue;
		}
		name = w2a(q->name, -1);
		v = reg_val(k, name, 0);
		LOG("  %s%s\n", name, v ? "" : ": none");
		free(name);
		if (!v) {
			if (q->flags & RTL_QUERY_REGISTRY_REQUIRED) {
				s = STATUS_OBJECT_NAME_NOT_FOUND;
				break;
			}
			if (q->deftype == REG_NONE)
				continue;
			{
				u32 n = q->deflen;
				if (!n && (q->deftype == REG_SZ || q->deftype == REG_EXPAND_SZ))
					n = (u32)(wlen(q->defdata) + 1) * 2;
				if (q->flags & RTL_QUERY_REGISTRY_DIRECT)
					direct_value(q->entryctx, q->deftype, q->defdata, n);
				else if (q->routine)
					s = ((query_fn)q->routine)(q->name, q->deftype, q->defdata, n,
								   ctx, q->entryctx);
			}
		} else if (q->flags & RTL_QUERY_REGISTRY_DIRECT)
			direct_value(q->entryctx, v->type, v->data, v->len);
		else if (q->routine)
			s = ((query_fn)q->routine)(q->name, v->type, v->data, v->len, ctx,
						   q->entryctx);
		if (!NT_SUCCESS(s))
			break;
	}
	free(base);
	free(kpath);
	return s;
}
static NTSTATUS NTAPI RtlWriteRegistryValue(u32 rel, const wchar *path, const wchar *name,
					    u32 type, void *data, u32 n)
{
	char *p = rtl_path(rel, path), *a = w2a(name, -1);
	LOG("RtlWriteRegistryValue %s\\%s\n", p, a);
	reg_set(p, a, type, data, n);
	free(p);
	free(a);
	return STATUS_SUCCESS;
}
static NTSTATUS NTAPI RtlDeleteRegistryValue(u32 rel, const wchar *path, const wchar *name)
{
	char *p = rtl_path(rel, path), *a = w2a(name, -1);
	struct rkey *k = reg_key(p, 0);
	NTSTATUS s = STATUS_OBJECT_NAME_NOT_FOUND;
	if (k) {
		struct rval **v;
		for (v = &k->vals; *v; v = &(*v)->next)
			if (!strcasecmp((*v)->name, a)) {
				*v = (*v)->next;
				s = STATUS_SUCCESS;
				break;
			}
	}
	free(p);
	free(a);
	return s;
}

/* the exports: by name ('*' a data import) */
#define E(f) { #f, (void *)f }
static const struct export exports[] = {
	E(ExAllocatePoolWithTag), E(ExFreePool), E(MmLockPagableDataSection),
	E(MmUnlockPagableImageSection), E(MmQuerySystemSize), E(MmMapIoSpace),
	E(MmUnmapIoSpace), E(MmUnlockPages), E(IoFreeMdl),
	{ "memmove", (void *)k_memmove }, { "wcschr", (void *)k_wcschr },
	{ "sprintf", (void *)k_sprintf },
	E(READ_PORT_ULONG), E(WRITE_PORT_ULONG), E(READ_PORT_USHORT), E(WRITE_PORT_USHORT),
	E(READ_PORT_UCHAR), E(WRITE_PORT_UCHAR),
	E(InterlockedIncrement), E(InterlockedDecrement), E(InterlockedExchange),
	E(InterlockedCompareExchange),
	E(KeGetCurrentIrql), E(KfRaiseIrql), E(KfLowerIrql), E(KfAcquireSpinLock),
	E(KfReleaseSpinLock), E(KeInitializeSpinLock), E(IoAcquireCancelSpinLock),
	E(IoReleaseCancelSpinLock),
	E(RtlInitUnicodeString), E(RtlInitAnsiString), E(RtlAppendUnicodeStringToString),
	E(RtlAppendUnicodeToString), E(RtlIntegerToUnicodeString), E(RtlUnicodeStringToInteger),
	E(RtlEqualUnicodeString), E(RtlFreeUnicodeString), E(RtlFreeAnsiString),
	E(RtlAnsiStringToUnicodeString), E(RtlUnicodeStringToAnsiString),
	E(RtlxAnsiStringToUnicodeSize), { "NlsMbCodePageTag", &NlsMbCodePageTag },
	E(KeQueryInterruptTime), E(KeQuerySystemTime), E(RtlTimeToTimeFields),
	E(KeInitializeEvent), E(KeSetEvent), E(KeClearEvent),
	E(KeInitializeDpc), E(KeInsertQueueDpc), E(KeRemoveQueueDpc),
	E(KeInitializeTimer), E(KeInitializeTimerEx), E(KeSetTimer), E(KeSetTimerEx),
	E(KeCancelTimer),
	E(KeWaitForSingleObject), E(KeWaitForMultipleObjects), E(KeDelayExecutionThread),
	E(PsCreateSystemThread), E(PsTerminateSystemThread), E(KeSetPriorityThread),
	E(ObReferenceObjectByHandle), E(ObfDereferenceObject), E(ZwClose),
	E(PsGetCurrentProcessId), E(PsGetVersion), E(IoIsWdmVersionAvailable),
	E(IoCreateDevice), E(IoDeleteDevice), E(IoAttachDeviceToDeviceStack), E(IoDetachDevice),
	E(IoCreateSymbolicLink), { "IoCreateUnprotectedSymbolicLink", (void *)IoCreateSymbolicLink },
	E(IoDeleteSymbolicLink),
	E(IoAllocateIrp), E(IoFreeIrp), E(IofCallDriver), E(PoCallDriver), E(IofCompleteRequest),
	E(IoBuildDeviceIoControlRequest), E(IoBuildSynchronousFsdRequest), E(IoCancelIrp),
	E(IoGetRelatedDeviceObject), E(IoGetDeviceObjectPointer),
	E(IoConnectInterrupt), E(IoDisconnectInterrupt), E(KeSynchronizeExecution),
	E(IoGetDmaAdapter),
	E(IoGetDeviceProperty), E(IoRegisterDeviceInterface), E(IoSetDeviceInterfaceState),
	E(IoGetDeviceInterfaces), E(IoRegisterPlugPlayNotification),
	E(IoUnregisterPlugPlayNotification), E(IoWMIRegistrationControl),
	E(WmiSystemControl), E(WmiCompleteRequest), E(WmiFireEvent),
	E(PoRegisterSystemState), E(PoUnregisterSystemState), E(PoSetPowerState),
	E(PoStartNextPowerIrp), E(PoRequestPowerIrp),
	E(KsCreatePin), E(KsSynchronousIoControlDevice),
	E(ZwCreateFile), E(ZwWriteFile),
	E(ZwOpenKey), E(ZwCreateKey), E(IoOpenDeviceRegistryKey), E(ZwQueryValueKey),
	E(ZwEnumerateValueKey), E(ZwSetValueKey), E(ZwEnumerateKey), E(ZwQueryKey),
	E(RtlQueryRegistryValues), E(RtlWriteRegistryValue), E(RtlDeleteRegistryValue),
};
static const struct export *export_find(const char *name)
{
	size_t i;
	for (i = 0; i < sizeof(exports) / sizeof(exports[0]); i++)
		if (!strcmp(exports[i].name, name))
			return &exports[i];
	return NULL;
}

/*
 *    the PCI bus driver: the PDO and the IRPs sent down to it
 */

static struct drvobj pci_drv, drv;
static struct drvext drvext;
static struct devobj *pdo;
static char svc_name[64];               /* the driver's service */

/* the device's resources: BAR 0's memory and its interrupt */
static byte res_raw[4 + 8 + 8 + 2 * 16], res_tr[sizeof(res_raw)];
static void res_init(byte *r, int translated)
{
	u32 irq = wdev->cfg[0x3c / 4] & 0xff;
	byte *d;
	memset(r, 0, sizeof(res_raw));
	memcpy(r, &(u32){ 1 }, 4);              /* one full descriptor */
	memcpy(r + 4, &(u32){ 5 }, 4);          /* PCIBus */
	memcpy(r + 12, &(word){ 1 }, 2);        /* version, revision */
	memcpy(r + 14, &(word){ 1 }, 2);
	memcpy(r + 16, &(u32){ 2 }, 4);
	d = r + 20;
	d[0] = 3; d[1] = 1;                     /* memory, exclusive */
	memcpy(d + 2, &(word){ 0 }, 2);         /* (read/write) */
	memcpy(d + 4, &(u32){ mem_phys }, 4);
	memcpy(d + 12, &(u32){ wdev->mem_size }, 4);
	d += 16;
	d[0] = 2; d[1] = 3;                     /* interrupt, shared */
	memcpy(d + 2, &(word){ 0 }, 2);         /* (level) */
	memcpy(d + 4, &(u32){ translated ? DIRQL : irq }, 4);
	memcpy(d + 8, &(u32){ translated ? 0x30 + irq : irq }, 4);
	memcpy(d + 12, &(u32){ 1 }, 4);
}

/* GUID_BUS_INTERFACE_STANDARD: its bus data calls, on the device's
   configuration space */
static const byte guid_bus_std[16] = {
	0x95, 0x6c, 0x6d, 0x49, 0x37, 0x92, 0xd0, 0x11,
	0xa3, 0x45, 0x00, 0xa0, 0xc9, 0x0f, 0xa0, 0x6b };       /* 496b8280-... */
static u32 NTAPI bus_get(void *ctx, u32 type, void *buf, u32 off, u32 n)
{
	u32 i;
	(void)ctx; (void)type;
	for (i = 0; i < n; i++)
		((byte *)buf)[i] = off + i < 256 ? ((byte *)wdev->cfg)[off + i] : 0;
	return n;
}
static u32 NTAPI bus_set(void *ctx, u32 type, void *buf, u32 off, u32 n)
{
	u32 i;
	(void)ctx; (void)type;
	for (i = 0; i < n; i++)
		if (off + i >= 4 && off + i < 256)
			((byte *)wdev->cfg)[off + i] = ((byte *)buf)[i];
	return n;
}
static void NTAPI if_ref(void *ctx) { (void)ctx; }

/* the PDO's PnP and power IRPs (the rest: not supported) */
static NTSTATUS NTAPI pdo_dispatch(struct devobj *d, struct irp *irp)
{
	struct iosl *sl = irp->csl;
	NTSTATUS s = irp->iosb.status;
	(void)d;
	if (sl->major == IRP_MJ_POWER) {
		LOG("pdo: power %u\n", sl->minor);
		s = STATUS_SUCCESS;
	} else if (sl->major == IRP_MJ_PNP) {
		LOG("pdo: pnp %u\n", sl->minor);
		switch (sl->minor) {
		case IRP_MN_START_DEVICE:
		case IRP_MN_QUERY_PNP_DEVICE_STATE:
			s = STATUS_SUCCESS;
			break;
		case IRP_MN_QUERY_CAPABILITIES: {
			byte *c = (byte *)(uintptr_t)sl->p[0];
			u32 bits = 1 << 4 | 1 << 6;     /* LockSupported, Removable no:
							   SurpriseRemovalOK, UniqueID */
			memcpy(c + 4, &bits, 4);
			memcpy(c + 8, &(u32){ 0 }, 4);          /* address */
			memcpy(c + 12, &(u32){ 0xffffffff }, 4);
			memcpy(c + 16, &(u32){ 1 }, 4);         /* S0 -> D0 */
			{
				int k;
				for (k = 1; k < 7; k++)
					memcpy(c + 16 + 4 * k, &(u32){ 4 }, 4);     /* D3 */
			}
			memcpy(c + 44, &(u32){ 1 }, 4);         /* SystemWake */
			memcpy(c + 48, &(u32){ 1 }, 4);         /* DeviceWake */
			s = STATUS_SUCCESS;
			break;
		}
		case IRP_MN_QUERY_INTERFACE: {
			const byte *g = (const byte *)(uintptr_t)sl->p[0];
			if (!memcmp(g, guid_bus_std, 16)) {
				void **i = (void **)(uintptr_t)sl->p[2];
				((word *)i)[0] = 32;
				((word *)i)[1] = 1;
				i[1] = NULL;
				i[2] = if_ref;
				i[3] = if_ref;
				i[4] = NULL;    /* TranslateBusAddress */
				i[5] = NULL;    /* GetDmaAdapter */
				i[6] = bus_set;
				i[7] = bus_get;
				s = STATUS_SUCCESS;
			}
			LOG("pdo: interface %02x%02x%02x%02x: %s\n", g[3], g[2], g[1], g[0],
			    s == STATUS_SUCCESS ? "given" : "none");
			break;
		}
		default:
			break;
		}
	} else
		s = STATUS_NOT_SUPPORTED;
	irp->iosb.status = s;
	IofCompleteRequest(irp, 0);
	return s;
}

/*
 *    modem.sys: IRPs to the top of the stack
 */

static struct devobj *top;
static byte fileobj[128];

struct req { int done; NTSTATUS status; u32 info; };
static NTSTATUS NTAPI req_done(struct devobj *d, struct irp *irp, void *ctx)
{
	struct req *r = ctx;
	(void)d;
	r->done = 1;
	r->status = irp->iosb.status;
	r->info = irp->iosb.info;
	return STATUS_MORE_PROCESSING_REQUIRED;
}

/* an IRP for the top of the stack: its next location and our
   completion routine set */
static struct irp *irp_new(byte major, byte minor, struct req *r)
{
	struct irp *irp = IoAllocateIrp(top->stacksize, 0);
	struct iosl *sl = next_sl(irp);
	sl->major = major;
	sl->minor = minor;
	sl->fileobj = fileobj;
	sl->completion = (void *)req_done;
	sl->ctx = r;
	sl->control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;
	irp->fileobj = fileobj;
	irp->iosb.status = major == IRP_MJ_PNP ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
	memset(r, 0, sizeof(*r));
	return irp;
}

/* irp sent and waited for (the line on while it pends); its status */
static NTSTATUS irp_sync(struct irp *irp, struct req *r)
{
	stack_clear();
	IofCallDriver(top, irp);
	kernel_run();
	{
		u64 start = itime();
		while (!r->done) {
			if (itime() - start > 600000000ull) {
				fprintf(stderr, "win: an IRP not done in 60 s\n");
				exit(1);
			}
			line_ms();
		}
	}
	free(irp);
	return r->status;
}

/* a serial IOCTL (buffered): in, and out back; its status */
static NTSTATUS ioctl(u32 code, const void *in, u32 inlen, void *out, u32 outlen)
{
	struct req r;
	struct irp *irp = irp_new(IRP_MJ_DEVICE_CONTROL, 0, &r);
	struct iosl *sl = next_sl(irp);
	NTSTATUS s;
	byte buf[256];
	memset(buf, 0, sizeof(buf));
	if (in)
		memcpy(buf, in, inlen);
	irp->sysbuf = buf;
	sl->p[0] = outlen;
	sl->p[1] = inlen;
	sl->p[2] = code;
	s = irp_sync(irp, &r);
	if (out && NT_SUCCESS(s))
		memcpy(out, buf, outlen);
	LOG("ioctl %x: %x\n", code, (u32)s);
	return s;
}

#define IOCTL_SERIAL_SET_BAUD_RATE      0x1b0004
#define IOCTL_SERIAL_SET_QUEUE_SIZE     0x1b0008
#define IOCTL_SERIAL_SET_LINE_CONTROL   0x1b000c
#define IOCTL_SERIAL_SET_TIMEOUTS       0x1b001c
#define IOCTL_SERIAL_SET_DTR            0x1b0024
#define IOCTL_SERIAL_SET_RTS            0x1b0030
#define IOCTL_SERIAL_SET_WAIT_MASK      0x1b0044
#define IOCTL_SERIAL_SET_HANDFLOW       0x1b0064
#define IOCTL_SERIAL_SET_CHARS          0x1b0058

/* the PnP manager's part: the driver loaded and started, its port
   opened and set up as modem.sys does; -1 if it fails */
static int driver_start(void)
{
	static struct ustr regpath;
	char name[96];
	NTSTATUS s;
	struct req r;
	struct irp *irp;
	int i;

	/* the PCI bus driver and its PDO */
	pci_drv.type = 4;
	pci_drv.size = sizeof(pci_drv);
	for (i = 0; i < 28; i++)
		pci_drv.major[i] = (void *)pdo_dispatch;
	IoCreateDevice(&pci_drv, 0, NULL, 0x22, 0, 0, &pdo);
	pdo->flags = DO_BUS_ENUMERATED_DEVICE | DO_POWER_PAGABLE;

	/* the driver's object, its entry */
	drv.type = 4;
	drv.size = sizeof(drv);
	drv.ext = &drvext;
	drvext.driver = &drv;
	RtlInitUnicodeString(&drvext.service, a2w(svc_name));
	snprintf(name, sizeof(name), "\\Driver\\%s", svc_name);
	RtlInitUnicodeString(&drv.name, a2w(name));
	drv.start = img;
	drv.dsize = img_size;
	drv.init = img_entry;
	for (i = 0; i < 28; i++)
		drv.major[i] = NULL;
	RtlInitUnicodeString(&regpath, a2w(key_service));
	stack_clear();
	s = ((NTSTATUS NTAPI (*)(struct drvobj *, struct ustr *))img_entry)(&drv, &regpath);
	kernel_run();
	LOG("DriverEntry: %x\n", (u32)s);
	if (!NT_SUCCESS(s))
		return -1;
	for (i = 0; i < 28; i++)
		if (!drv.major[i])
			drv.major[i] = (void *)pdo_dispatch;
	if (!drvext.adddevice) {
		fprintf(stderr, "win: the driver has no AddDevice\n");
		return -1;
	}
	stack_clear();
	s = ((NTSTATUS NTAPI (*)(struct drvobj *, struct devobj *))drvext.adddevice)(&drv, pdo);
	kernel_run();
	LOG("AddDevice: %x\n", (u32)s);
	if (!NT_SUCCESS(s))
		return -1;
	top = top_of(pdo);
	top->flags &= ~DO_DEVICE_INITIALIZING;

	/* START_DEVICE, its resources */
	res_init(res_raw, 0);
	res_init(res_tr, 1);
	irp = irp_new(IRP_MJ_PNP, IRP_MN_START_DEVICE, &r);
	next_sl(irp)->p[0] = (u32)(uintptr_t)res_raw;
	next_sl(irp)->p[1] = (u32)(uintptr_t)res_tr;
	s = irp_sync(irp, &r);
	LOG("START_DEVICE: %x\n", (u32)s);
	if (!NT_SUCCESS(s))
		return -1;
	{
		byte caps[64];
		memset(caps, 0, sizeof(caps));
		memcpy(caps, &(word){ 64 }, 2);
		memcpy(caps + 2, &(word){ 1 }, 2);
		irp = irp_new(IRP_MJ_PNP, IRP_MN_QUERY_CAPABILITIES, &r);
		next_sl(irp)->p[0] = (u32)(uintptr_t)caps;
		s = irp_sync(irp, &r);
		LOG("QUERY_CAPABILITIES: %x\n", (u32)s);
	}

	/* the port opened, as modem.sys sets it up */
	*(struct devobj **)(fileobj + 4) = top;
	irp = irp_new(IRP_MJ_CREATE, 0, &r);
	s = irp_sync(irp, &r);
	LOG("CREATE: %x\n", (u32)s);
	if (!NT_SUCCESS(s))
		return -1;
	{
		u32 baud = 115200;
		byte lc[3] = { 0, 0, 8 };               /* 1 stop bit, none, 8 */
		u32 hf[4] = { 1 | 8, 0x40, 1024, 1024 };   /* DTR control, CTS
							      handshake; RTS
							      control */
		u32 to[5] = { 0xffffffff, 0, 0, 0, 0 }; /* reads: what is there */
		u32 qs[2] = { 4096, 4096 };
		u32 mask = 0;
		ioctl(IOCTL_SERIAL_SET_QUEUE_SIZE, qs, 8, NULL, 0);
		ioctl(IOCTL_SERIAL_SET_BAUD_RATE, &baud, 4, NULL, 0);
		ioctl(IOCTL_SERIAL_SET_LINE_CONTROL, lc, 3, NULL, 0);
		ioctl(IOCTL_SERIAL_SET_HANDFLOW, hf, 16, NULL, 0);
		ioctl(IOCTL_SERIAL_SET_TIMEOUTS, to, 20, NULL, 0);
		ioctl(IOCTL_SERIAL_SET_WAIT_MASK, &mask, 4, NULL, 0);
		ioctl(IOCTL_SERIAL_SET_DTR, NULL, 0, NULL, 0);
		ioctl(IOCTL_SERIAL_SET_RTS, NULL, 0, NULL, 0);
	}
	return 0;
}

/*
 *    the port: the modem's output read, the DTE's written
 */

/* the read and the write in flight (one each), their buffers */
static struct irp *rd_irp, *wr_irp;
static struct req rd_req, wr_req;
static byte rd_buf[WIN_SERIAL_READ / 2], wr_buf[256];

/* a READ or WRITE IRP of n bytes at buf, as the device takes them */
static struct irp *rw_irp(byte major, byte *buf, u32 n, struct req *r)
{
	struct irp *irp = irp_new(major, 0, r);
	struct iosl *sl = next_sl(irp);
	sl->p[0] = n;
	if (top->flags & DO_BUFFERED_IO)
		irp->sysbuf = buf;
	else if (top->flags & DO_DIRECT_IO) {
		struct mdl *m = calloc(1, sizeof(*m) + 64);
		m->startva = (void *)((uintptr_t)buf & ~4095u);
		m->offset = (u32)(uintptr_t)buf & 4095;
		m->count = n;
		m->mapped = buf;
		m->flags = 0x0001 | 0x0002;
		irp->mdl = m;
	}
	irp->userbuf = buf;
	return irp;
}

/* what a READ done brought, into out; how much */
static u32 read_done(byte *out)
{
	u32 n = NT_SUCCESS(rd_req.status) ? rd_req.info : 0;
	free(rd_irp->mdl);
	free(rd_irp);
	rd_irp = NULL;
	if (win_debug && !NT_SUCCESS(rd_req.status))
		LOG("read: %x\n", (u32)rd_req.status);
	if (n > sizeof(rd_buf))
		n = sizeof(rd_buf);
	memcpy(out, rd_buf, n);
	return n;
}

/* the modem's output: a READ for what it has (one in flight at most,
   one a call), and what it brought */
u32 win_serial_read(byte *buf)
{
	u32 n = 0;
	if (rd_irp && rd_req.done)
		n += read_done(buf + n);
	if (!rd_irp) {
		rd_irp = rw_irp(IRP_MJ_READ, rd_buf, sizeof(rd_buf), &rd_req);
		stack_clear();
		IofCallDriver(top, rd_irp);
		kernel_run();
		if (rd_req.done)
			n += read_done(buf + n);
	}
	return n;
}

/* the WRITE in flight done and freed: none in flight? */
int win_serial_writable(void)
{
	if (wr_irp && wr_req.done) {
		free(wr_irp->mdl);
		free(wr_irp);
		wr_irp = NULL;
	}
	return !wr_irp;
}

/* n octets at p to the modem in a WRITE, if none is in flight */
int win_serial_write(const void *p, int n)
{
	if (!win_serial_writable())
		return 0;
	if (n > (int)sizeof(wr_buf))
		n = sizeof(wr_buf);
	memcpy(wr_buf, p, (size_t)n);
	wr_irp = rw_irp(IRP_MJ_WRITE, wr_buf, (u32)n, &wr_req);
	stack_clear();
	IofCallDriver(top, wr_irp);
	kernel_run();
	return n;
}

/*
 *    the line's time: the timers each ms, the device's interrupt
 */

void win_tick(void)
{
	static int irq_was;
	int up;

	win_line_n++;
	if (win_line_n * 1000 / (u64)wdev->rate !=
	    (win_line_n - 1) * 1000 / (u64)wdev->rate) {
		run_timers();
		kernel_run();
	}
	/* the ISR (at the device's IRQL) when the interrupt comes up, and
	   again while it stays up (four times at most), its DPCs and the
	   threads after each */
	up = wdev->irq();
	if (up && !irq_was && isr && irql < DIRQL) {
		int guard = 0;
		while (wdev->irq() && guard++ < 4) {
			byte o = irql;
			irql = DIRQL;
			if (!cur_thread)
				stack_clear();
			isr(intr_obj, isr_ctx);
			irql = o;
			kernel_run();
		}
	}
	irq_was = up;
}

/*
 *    the start: the traps and the KPCR, what the INF says, the image,
 *    the driver
 */

int win_start(struct win_dev *dev)
{
	struct sigaction sa;
	char sect[160];
	const char *v;
	u32 id = dev->cfg[0], cls = dev->cfg[0x08 / 4], sub = dev->cfg[0x2c / 4];
	int k;

	wdev = dev;
	mem_phys = dev->cfg[0x10 / 4] & ~0xfu;
	memset(&sa, 0, sizeof(sa));
	{
		/* (the handlers on a stack of their own: a fault of an
		   overrun stack is told too) */
		stack_t ss;
		ss.ss_sp = malloc(1 << 16);
		ss.ss_size = 1 << 16;
		ss.ss_flags = 0;
		sigaltstack(&ss, NULL);
	}
	sa.sa_sigaction = on_segv;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigaction(SIGSEGV, &sa, NULL);
	sa.sa_sigaction = on_trap;
	sigaction(SIGTRAP, &sa, NULL);
	if (prctl(PR_SET_TSC, PR_TSC_SIGSEGV, 0, 0, 0))
		perror("win: PR_SET_TSC");
	if (kpcr_init())
		return -1;

	/* the device's IDs and instance, from its configuration space */
	k = snprintf(hw_ids, sizeof(hw_ids), "PCI\\VEN_%04X&DEV_%04X&SUBSYS_%04X%04X&REV_%02X",
		     id & 0xffff, id >> 16, sub >> 16, sub & 0xffff, cls & 0xff) + 1;
	k += snprintf(hw_ids + k, sizeof(hw_ids) - (size_t)k, "PCI\\VEN_%04X&DEV_%04X&SUBSYS_%04X%04X",
		      id & 0xffff, id >> 16, sub >> 16, sub & 0xffff) + 1;
	hw_ids[k] = 0;
	k = snprintf(compat_ids, sizeof(compat_ids), "PCI\\VEN_%04X&DEV_%04X&REV_%02X",
		     id & 0xffff, id >> 16, cls & 0xff) + 1;
	k += snprintf(compat_ids + k, sizeof(compat_ids) - (size_t)k, "PCI\\VEN_%04X&DEV_%04X",
		      id & 0xffff, id >> 16) + 1;
	k += snprintf(compat_ids + k, sizeof(compat_ids) - (size_t)k, "PCI\\CC_%04X", cls >> 16) + 1;
	compat_ids[k] = 0;
	snprintf(instance, sizeof(instance), "3&61aaa01&0&%02X", dev->devfn);

	/* what the INF says: the class, the model's description and
	   maker, the driver's service; the keys, the registry */
	if (inf_load(dev->inf))
		return -1;
	v = inf_value("Version", "Class");
	snprintf(dev_class, sizeof(dev_class), "%s", v ? v : "");
	v = inf_value("Version", "ClassGUID");
	snprintf(class_guid, sizeof(class_guid), "%s", v ? v : "");
	snprintf(drv_key, sizeof(drv_key), "%s\\0000", class_guid);
	if (inf_model(dev->model, dev_desc, sizeof(dev_desc), dev_mfg, sizeof(dev_mfg))) {
		fprintf(stderr, "win: %s: no model %s\n", dev->inf, dev->model);
		return -1;
	}
	snprintf(sect, sizeof(sect), "%s.NT.Services", dev->model);
	v = inf_value(sect, "AddService");
	if (!v) {
		fprintf(stderr, "win: %s: no service in [%s]\n", dev->inf, sect);
		return -1;
	}
	snprintf(svc_name, sizeof(svc_name), "%.*s", (int)strcspn(v, ", \t"), v);
	snprintf(key_service, sizeof(key_service),
		 "\\Registry\\Machine\\System\\CurrentControlSet\\Services\\%s", svc_name);
	snprintf(key_driver, sizeof(key_driver),
		 "\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class\\%s", drv_key);
	snprintf(key_device, sizeof(key_device),
		 "\\Registry\\Machine\\System\\CurrentControlSet\\Enum\\PCI\\%s\\%s\\Device Parameters",
		 hw_ids + 4, instance);
	LOG("%s: %s (%s), %s, service %s\n", dev->model, dev_desc, dev_mfg, hw_ids, svc_name);
	inf_install(dev->model);
	reg_key(key_service, 1);
	reg_set_sz(key_device, "PortName", REG_SZ, dev->port);

	if (pe_load(dev->sys))
		return -1;
	return driver_start();
}
