/*
 *    half_sm56.c - Motorola's SM56 soft modem (sm56-06.09.05, sm56.ko of
 *    Fedora Core 3's kernel 2.6.9: its controller and data pumps, i386
 *    object code, V.92 with quick connect and modem-on-hold) as a test
 *    peer, as half_agere does for Agere's: line audio from fd 3 and to
 *    fd 4 (16 bit, SM56_RATE, 8000 by default), AT commands on a pty
 *    whose name is printed ("created sm56: /dev/pts/N"); SM56_SYNC as
 *    half_agere's AGERE_SYNC.
 *
 *    The module is linked in as it is, its imports renamed k_<name>
 *    (objcopy) and given here: a kernel of its own for it, regparm(3) as
 *    Fedora's was built, with the structures it was compiled against
 *    (FC3's: spinlocks with debugging, the 4G/4G split's PAGE_OFFSET of
 *    0x02000000, so that a bus address is the virtual one less that).
 *
 *    Its hardware: an Intel ICH AC'97 modem controller (8086:2416, its
 *    "is_" back end) with a Motorola modem codec ("MOTf") as the
 *    secondary codec, on port I/O it does with in and out itself: those
 *    trap (no I/O permission) and SIGSEGV's handler carries them out on
 *    the emulated devices, PCI's configuration space at 0xcf8 too.  The
 *    codec runs at whatever rate the module sets it to (7200 to 12000 Hz,
 *    by modulation): a windowed sinc resampler between it and the line.
 *    Time is the samples': jiffies (HZ 1000), its timers and delays.
 *
 *    Its serial side is the tty driver of the module (mot_open,
 *    mot_write, and its receive path into the tty's flip buffer, whose
 *    work we run): a tty_struct of 2.6.9 with only what it uses.
 *
 *    The module is not in this tree (it is Motorola's); build with
 *    -DDMODEM_SM56_KO=<path to sm56.ko> (sm56_obj.cmake renames its
 *    imports).  It must be that build (sha256 a757fd85...): the places
 *    patched for "current" are checked.  SM56_DEBUG=1 prints its printk
 *    and the port accesses we do not know, 2 every port access.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <ucontext.h>
#include <unistd.h>

typedef unsigned char byte;
typedef unsigned short word;
typedef unsigned int u32;

#define KAPI __attribute__((regparm(3)))

static int dbg;
#define LOG(fmt, ...) do { if (dbg) fprintf(stderr, "sm56: " fmt, ##__VA_ARGS__); } while (0)

/*
 *    the module
 */

extern int init_module(void);
extern KAPI int mot_open(void *tty, void *filp);
extern KAPI int mot_write(void *tty, int from_user, const void *buf, int count);
extern KAPI int mot_writeroom(void *tty);

#define PAGE_OFFSET 0x02000000u
static void *bus_to_virt(u32 a) { return (void *)(uintptr_t)(a + PAGE_OFFSET); }

/*
 *    time: the line's samples
 */

static int line_rate = 8000;
static unsigned long long line_n;       /* line samples so far */
unsigned long k_jiffies;
static void line_idle(unsigned n);

/*
 *    the Motorola codec: secondary, registers 0x80 to 0xff
 */

static word codec[0x80];
static unsigned codec_rate = 8000;

static void codec_init(void)
{
	memset(codec, 0, sizeof(codec));
	codec[0x3c] = 0x0001;           /* extended modem ID: line 1 */
	codec[0x40] = 8000;
	codec[0x7c] = 0x4d4f;           /* "MO" */
	codec[0x7e] = 0x5466;           /* "Tf" */
}

static word codec_read(int r)
{
	r &= 0x7e;
	switch (r) {
	case 0x3e:
		/* ADC/DAC of both lines ready (bits 0..3), what was
		   written above */
		return (codec[r] & 0xff00) | 0x000f;
	default:
		return codec[r];
	}
}

static void codec_write(int r, word v)
{
	r &= 0x7e;
	if (r == 0x7c || r == 0x7e || r == 0x3c)
		return;
	if (codec[r] != v)
		LOG("codec %02x = %04x\n", r, v);
	if (r == 0x40 && v != codec_rate) {
		codec_rate = v;
		LOG("codec rate %u Hz\n", v);
	}
	codec[r] = v;
}

/*
 *    the ICH AC'97 modem controller (8086:2416): I/O at NAMBAR (the
 *    codecs: 0x00 primary, 0x80 secondary) and NABMBAR (the bus master)
 */

#define NAMBAR  0xe000
#define NABMBAR 0xe400
#define IRQ     5

struct bm {
	u32 bdbar;
	byte civ, lvi, cr;
	word sr, picb;
	int pos;
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

static u32 *bd(struct bm *b, int i)
{
	return (u32 *)bus_to_virt(b->bdbar + 8 * (i & 31));
}

static void bm_load(struct bm *b)
{
	b->picb = (word)(bd(b, b->civ)[1] & 0xffff);
	b->pos = 0;
}

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
		return;
	}
	if ((v & CR_RPBM) && !(b->cr & CR_RPBM)) {
		b->sr &= ~(SR_DCH | SR_CELV);
		if (b->bdbar)
			bm_load(b);
	}
	if (!(v & CR_RPBM))
		b->sr |= SR_DCH;
	b->cr = v & 0x1f;
}

static int bm_irq(const struct bm *b)
{
	return ((b->sr & SR_BCIS) && (b->cr & CR_IOCE)) ||
		((b->sr & SR_LVBCI) && (b->cr & CR_LVBIE)) ||
		((b->sr & SR_FIFOE) && (b->cr & CR_FEIE));
}

static u32 glob_status(void)
{
	return (glob_sta & ~6u) | (bm_irq(&pi) ? 2 : 0) | (bm_irq(&po) ? 4 : 0);
}

/* a codec sample through a running bus master */
static short bm_sample(struct bm *b, short in)
{
	short out = 0;
	u32 *d;
	short *buf;

	if (!(b->cr & CR_RPBM) || (b->sr & SR_DCH) || !b->bdbar)
		return 0;
	d = bd(b, b->civ);
	buf = (short *)bus_to_virt(d[0]);
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
			b->sr |= SR_LVBCI | SR_CELV | SR_DCH;
		} else {
			b->civ = (b->civ + 1) & 31;
			bm_load(b);
		}
	}
	return out;
}

/*
 *    PCI configuration space (0xcf8, 0xcfc): the controller at 00:1f.6
 */

#define DEVFN ((0x1f << 3) | 6)
static u32 cf8;
static u32 cfg[64];

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

static int cfg_ours(void)
{
	return (cf8 & 0x80000000) && ((cf8 >> 16) & 0xff) == 0 &&
		((cf8 >> 8) & 0xff) == DEVFN;
}

/* function 0 of the device: the ICH's LPC bridge, multi-function (the
   module looks before it reads function 6) */
static int cfg_fn0(void)
{
	return (cf8 & 0x80000000) && ((cf8 >> 16) & 0xff) == 0 &&
		((cf8 >> 8) & 0xff) == (DEVFN & ~7);
}

static u32 cfg_read(unsigned off, int size)
{
	u32 v;
	unsigned r = (cf8 & 0xfc) + off;
	if (cfg_fn0()) {
		v = r < 4 ? 0x24108086 : (r & ~3u) == 0x0c ? 0x00800000 : 0;
		v >>= 8 * (r & 3);
		return size == 4 ? v : size == 2 ? v & 0xffff : v & 0xff;
	}
	if (!cfg_ours())
		return 0xffffffff;
	v = cfg[(r >> 2) & 63] >> (8 * (r & 3));
	return size == 4 ? v : size == 2 ? v & 0xffff : v & 0xff;
}

static void cfg_write(unsigned off, u32 v, int size)
{
	unsigned r = (cf8 & 0xfc) + off;
	u32 mask = size == 4 ? 0xffffffff : size == 2 ? 0xffff : 0xff;
	if (!cfg_ours())
		return;
	LOG("pci config %02x = %x\n", r, v);
	if (r == 0x10 || r == 0x14) {
		/* (the sizes of the BARs) */
		if (v == 0xffffffff)
			v = r == 0x10 ? 0xffffff01 : 0xffffff81;
		else
			v = (v & ~3u) | 1;
	}
	cfg[(r >> 2) & 63] = (cfg[(r >> 2) & 63] & ~(mask << 8 * (r & 3))) |
		(v & mask) << 8 * (r & 3);
}

/*
 *    port I/O
 */

static u32 io_in(unsigned a, int size)
{
	u32 v = 0xffffffff;
	if (a == 0xcf8)
		v = cf8;
	else if (a >= 0xcfc && a < 0xd00)
		v = cfg_read(a - 0xcfc, size);
	else if (a >= NAMBAR && a < NAMBAR + 0x100) {
		int r = a - NAMBAR;
		v = r >= 0x80 ? codec_read(r - 0x80) : 0xffff;
	} else if (a >= NABMBAR && a < NABMBAR + 0x80) {
		int o = a - NABMBAR;
		struct bm *b = o < 0x10 ? &pi : o < 0x20 ? &po : NULL;
		v = 0xffffffff;
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
		fprintf(stderr, "sm56: in%d %04x = %x\n", size, a, v);
	return v;
}

static void io_out(unsigned a, u32 v, int size)
{
	if (dbg > 1)
		fprintf(stderr, "sm56: out%d %04x = %x\n", size, a, v);
	if (a == 0xcf8 && size == 4) {
		cf8 = v;
		return;
	}
	if (a >= 0xcfc && a < 0xd00) {
		cfg_write(a - 0xcfc, v, size);
		return;
	}
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

/* the module's in and out: a general protection fault without I/O
   permission; carried out here and stepped over (ec to ef, e4 to e7,
   with 0x66 for 16 bits); cli and sti likewise */
static void on_segv(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = (ucontext_t *)ctx;
	greg_t *g = uc->uc_mcontext.gregs;
	byte *ip = (byte *)(uintptr_t)g[REG_EIP];
	int size = 4, n = 1;
	unsigned port = (unsigned)g[REG_EDX] & 0xffff;

	(void)si;
	if (ip[0] == 0x66) {
		size = 2;
		ip++;
		n++;
	}
	if (ip[0] == 0x0f && (ip[1] == 0x20 || ip[1] == 0x22) && ip[2] == 0xc1) {
		/* mov cr0, ecx and back (its FPU use in the interrupt) */
		static u32 cr0 = 0x80050033;
		if (ip[1] == 0x20)
			g[REG_ECX] = cr0;
		else
			cr0 = (u32)g[REG_ECX];
		g[REG_EIP] += 3;
		return;
	}
	switch (ip[0]) {
	case 0xfa:      /* cli, sti: one thread, nothing to keep out */
	case 0xfb:
		break;
	case 0xe4:      /* in al, imm8; in eax, imm8; out imm8, al/eax */
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
		fprintf(stderr, "sm56: fault at %p (%02x %02x %02x), address %p\n",
			(void *)ip, ip[0], ip[1], ip[2], si->si_addr);
		signal(sig, SIG_DFL);
		return;
	}
	g[REG_EIP] += n;
}

/*
 *    current: the module takes it from its stack pointer (thread_info at
 *    the 4 KB stack's bottom), at a few places: there and %esp,%eax
 *    becomes int3, nop, and SIGTRAP's handler gives eax our thread_info
 *    (not in an interrupt; a task with no controlling tty)
 */

/* where the module's .text has its and %esp,%eax (from init_module's
   offset in it) */
static const u32 current_at[] = {
	0xb3876, 0xb3c8b, 0xb3ce2, 0xb3d28, 0xb3d84, 0xb3dd7, 0xb3e24,
	0xb3e51, 0xb4029, 0xb40c7, 0xb411c, 0xb4fd7, 0xb504f,
};
#define INIT_MODULE_AT 0xb3147
static u32 thread_info[16];
static u32 task[0x800];
static u32 task_signal[0x40];

static void on_trap(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = (ucontext_t *)ctx;
	greg_t *g = uc->uc_mcontext.gregs;
	(void)sig; (void)si;
	g[REG_EAX] = (greg_t)(uintptr_t)thread_info;
	g[REG_EIP] += 1;        /* (past the nop) */
}

/* its read-only data, writable as in the kernel: its interrupt saves
   the FPU at .rodata.cst4 + 0x640 (past that section: a stray
   relocation, harmless in a kernel where a module's memory is all
   writable) */
extern char etext[], __data_start[];
static int rodata_writable(void)
{
	uintptr_t lo = (uintptr_t)etext & ~4095u, hi = (uintptr_t)__data_start;
	return mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC);
}

static int current_patch(void)
{
	uintptr_t text = (uintptr_t)init_module - INIT_MODULE_AT;
	unsigned i;

	thread_info[0] = (u32)(uintptr_t)task;
	task[0x4fc / 4] = (u32)(uintptr_t)task_signal;
	for (i = 0; i < sizeof(current_at) / sizeof(current_at[0]); i++) {
		byte *p = (byte *)(text + current_at[i]);
		uintptr_t pg = (uintptr_t)p & ~4095u;
		if (p[0] != 0x21 || p[1] != 0xe0) {
			fprintf(stderr, "sm56: not the module expected (%x)\n",
				current_at[i]);
			return -1;
		}
		if (mprotect((void *)pg, 8192, PROT_READ | PROT_WRITE | PROT_EXEC))
			return -1;
		p[0] = 0xcc;
		p[1] = 0x90;
	}
	return 0;
}

/*
 *    the kernel
 */

int k_printk(const char *fmt, ...)
{
	va_list ap;
	int n = 0;
	if (dbg) {
		va_start(ap, fmt);
		/* (the level's <n> left as it is) */
		n = vfprintf(stderr, fmt, ap);
		va_end(ap);
	}
	return n;
}
KAPI void *k_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
KAPI void *k_memset(void *d, int c, size_t n) { return memset(d, c, n); }
KAPI void k_dump_stack(void) { LOG("dump_stack\n"); }
KAPI int k_capable(int cap) { (void)cap; return 1; }
KAPI void k___might_sleep(const char *file, int line) { (void)file; (void)line; }
KAPI int k___cond_resched(void) { return 0; }
u32 k_kernel_flag[8];
unsigned char k__ctype[256];

static void ctype_init(void)
{
	int c;
	for (c = 0; c < 256; c++)
		k__ctype[c] = (byte)((isupper(c) ? 0x01 : 0) | (islower(c) ? 0x02 : 0) |
			(isdigit(c) ? 0x04 : 0) | (iscntrl(c) ? 0x08 : 0) |
			(ispunct(c) ? 0x10 : 0) | (isspace(c) ? 0x20 : 0) |
			(isxdigit(c) ? 0x40 : 0) | (c == ' ' ? 0x80 : 0));
}

/* memory: kmalloc's caches (malloc_sizes: size, cache, DMA cache) */
struct kcache { unsigned size; };
static struct kcache kcache = { 1 << 17 };
struct cache_sizes { unsigned size; void *cachep, *dmacachep; };
struct cache_sizes k_malloc_sizes[32];
static void malloc_sizes_init(void)
{
	int i;
	for (i = 0; i < 32; i++) {
		k_malloc_sizes[i].size = 32u << (i < 12 ? i : 12);
		k_malloc_sizes[i].cachep = &kcache;
		k_malloc_sizes[i].dmacachep = &kcache;
	}
}
KAPI void *k_kmem_cache_alloc(void *cachep, unsigned flags)
{
	(void)flags;
	return calloc(1, ((struct kcache *)cachep)->size);
}
KAPI void *k___kmalloc(size_t n, unsigned flags) { (void)flags; return calloc(1, n + 64); }
KAPI void k_kfree(const void *p) { free((void *)p); }
KAPI void *k_vmalloc(unsigned long n) { return calloc(1, n + 64); }
KAPI void k_vfree(void *p) { free(p); }
KAPI void *k___ioremap(unsigned long a, unsigned long n, unsigned long flags)
{
	(void)flags;
	LOG("ioremap %lx %lu\n", a, n);
	return NULL;
}
KAPI void k_iounmap(void *p) { (void)p; }
/* copy_from_user and copy_to_user of FC3: one address space here */
KAPI unsigned long k_get_user_size(unsigned n, void *to, const void *from)
{
	memcpy(to, from, n);
	return 0;
}
KAPI unsigned long k_put_user_size(unsigned n, const void *from, void *to)
{
	memcpy(to, from, n);
	return 0;
}

/* delays: the line goes on (silent), interrupts and all */
KAPI void k___udelay(unsigned long us) { line_idle((unsigned)(us * line_rate / 1000000) + 1); }
KAPI void k___const_udelay(unsigned long xloops) { k___udelay(xloops / 0x10c7); }

/* semaphores: never contended here (one thread) */
__asm__(".globl k___down_failed\n k___down_failed: ret\n"
	".globl k___up_wakeup\n k___up_wakeup: ret\n");
KAPI void k___wake_up(void *q, unsigned mode, int nr, void *key)
{
	(void)q; (void)mode; (void)nr; (void)key;
}

/* timers (FC3's struct timer_list: expires at 8, function at 0x28, data
   at 0x2c) */
#define NTIMERS 16
static byte *timers[NTIMERS];
typedef KAPI void (*kfn)(unsigned long);
#define T_EXPIRES(t) (*(unsigned long *)((t) + 0x08))
#define T_FN(t) (*(kfn *)((t) + 0x28))
#define T_DATA(t) (*(unsigned long *)((t) + 0x2c))
KAPI int k___mod_timer(void *t, unsigned long expires)
{
	int i, free_i = -1;
	T_EXPIRES((byte *)t) = expires;
	for (i = 0; i < NTIMERS; i++) {
		if (timers[i] == t)
			return 1;
		if (!timers[i] && free_i < 0)
			free_i = i;
	}
	if (free_i >= 0)
		timers[free_i] = (byte *)t;
	else
		fprintf(stderr, "sm56: too many timers\n");
	return 0;
}
KAPI int k_del_timer(void *t)
{
	int i;
	for (i = 0; i < NTIMERS; i++)
		if (timers[i] == t) {
			timers[i] = NULL;
			return 1;
		}
	return 0;
}
static void run_timers(void)
{
	int i;
	for (i = 0; i < NTIMERS; i++) {
		byte *t = timers[i];
		if (t && (long)(k_jiffies - T_EXPIRES(t)) >= 0) {
			timers[i] = NULL;
			T_FN(t)(T_DATA(t));
		}
	}
}

/* tasklets: next, state (bit 0 scheduled), count, func, data */
#define NTASKLETS 8
static byte *tasklets[NTASKLETS];
static int ntasklets;
KAPI void k___tasklet_schedule(void *t)
{
	if (ntasklets < NTASKLETS)
		tasklets[ntasklets++] = (byte *)t;
}
static void run_tasklets(void)
{
	while (ntasklets) {
		byte *t = tasklets[0];
		memmove(tasklets, tasklets + 1, --ntasklets * sizeof(tasklets[0]));
		*(u32 *)(t + 4) &= ~1u;
		if (*(int *)(t + 8) == 0)
			(*(kfn *)(t + 12))(*(unsigned long *)(t + 16));
	}
}

/* the interrupt */
typedef KAPI int (*kisr)(int irq, void *dev, void *regs);
static kisr isr;
static void *isr_dev;
KAPI int k_request_irq(unsigned irq, void *handler, unsigned long flags,
		       const char *name, void *dev)
{
	(void)flags;
	LOG("irq %u for %s\n", irq, name ? name : "?");
	isr = (kisr)handler;
	isr_dev = dev;
	return 0;
}
KAPI void k_free_irq(unsigned irq, void *dev) { (void)irq; (void)dev; isr = NULL; }

/* PCI: the controller, for pci_find_device; its configuration space at
   0xcf8 (the module reads it there) */
static byte pci_bus[0x100];
static byte pci_dev[0x400];
KAPI void *k_pci_find_device(unsigned vendor, unsigned device, void *from)
{
	LOG("pci_find_device %04x:%04x%s\n", vendor, device, from ? " (next)" : "");
	if (from || vendor != 0x8086 || device != 0x2416)
		return NULL;
	/* (2.6.9's struct pci_dev: bus, devfn, irq, resource[0] and [1]
	   starts, which the module takes for the BARs and the line) */
	*(void **)(pci_dev + 0x10) = pci_bus;
	pci_bus[0x3c] = 0;
	*(u32 *)(pci_dev + 0x20) = DEVFN;
	*(u32 *)(pci_dev + 0x118) = IRQ;
	*(u32 *)(pci_dev + 0x120) = NAMBAR;
	*(u32 *)(pci_dev + 0x124) = NAMBAR + 0xff;
	*(u32 *)(pci_dev + 0x13c) = NABMBAR;
	*(u32 *)(pci_dev + 0x140) = NABMBAR + 0x7f;
	return pci_dev;
}
KAPI void k_pci_set_master(void *dev) { (void)dev; }

/* the character devices and the tty driver */
static void *tty_driver;
KAPI int k_register_chrdev(unsigned major, const char *name, void *fops)
{
	(void)fops;
	LOG("register_chrdev %u %s\n", major, name);
	return 0;
}
KAPI int k_unregister_chrdev(unsigned major, const char *name)
{
	(void)major; (void)name;
	return 0;
}
KAPI int k_tty_register_driver(void *d) { tty_driver = d; return 0; }
KAPI int k_tty_unregister_driver(void *d) { (void)d; return 0; }

/* the tty (2.6.9): the flip buffer at 0xec (its work), char_buf_ptr
   0x160, flag_buf_ptr 0x164, count 0x168, char_buf 0x170 */
#define TTY_FLIPBUF 512
static byte tty[0x2000];
static int pty;
/* its termios (at 0x80): 115200 bit/s, 8N1, CREAD, CLOCAL, raw */
static u32 termios[16] = { 0, 0, 010000 | 2 | 060 | 0200 | 04000, 0 };
static void flip_reset(void)
{
	*(byte **)(tty + 0x160) = tty + 0x170;
	*(byte **)(tty + 0x164) = tty + 0x170 + 2 * TTY_FLIPBUF;
	*(int *)(tty + 0x168) = 0;
}
static void tty_init(void)
{
	memset(tty, 0, sizeof(tty));
	*(u32 **)(tty + 0x80) = termios;
	flip_reset();
}
/* SM56_SYNC=<samples>: the DTE's input only every that many samples of
   the line, so that a test's commands meet the line at the same samples
   every time (as half_agere's AGERE_SYNC) */
static long sync_n;

/* online between its CONNECT and NO CARRIER or OK */
static int online;
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

static void flip_push(void)
{
	int n = *(int *)(tty + 0x168), k;
	if (n > 0 && write(pty, tty + 0x170, n) < 0)
		LOG("pty write: %s\n", strerror(errno));
	for (k = 0; k < n; k++)
		result_scan(tty[0x170 + k]);
	flip_reset();
}
KAPI int k_schedule_delayed_work(void *work, unsigned long delay)
{
	(void)delay;
	if ((byte *)work == tty + 0xec)
		flip_push();
	else
		LOG("schedule_delayed_work %p\n", work);
	return 1;
}

/*
 *    the codec's samples: a windowed sinc resampler each way between the
 *    line's rate and the codec's
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
static void rs_push(struct rs *r, float x)
{
	memmove(r->hist, r->hist + 1, (RS_TAPS - 1) * sizeof(float));
	r->hist[RS_TAPS - 1] = x;
}

static struct rs up, down;      /* line to codec, codec to line */
static double up_ph;            /* the next codec sample's time after the
				   line's sample before the newest, in line
				   samples */

static short sat(double x)
{
	return (short)(x > 32767 ? 32767 : x < -32768 ? -32768 : lrint(x));
}

static void codec_sample(short adc)
{
	short dac = bm_sample(&po, 0);
	bm_sample(&pi, adc);
	rs_push(&down, dac);
}

/* one line sample each way */
static int irq_was;
static short line_sample(short in)
{
	double r = (double)line_rate / codec_rate;
	double out;
	int irq_now;

	/* the codec's samples due within this line sample */
	rs_push(&up, in);
	while (up_ph < 1) {
		codec_sample(sat(rs_at(up.hist, up_ph, r > 1 ? 1 / r : 1)));
		up_ph += r;
	}
	up_ph -= 1;
	/* (the newest codec sample was at up_ph - r) */
	out = rs_at(down.hist, 1 - up_ph / r, r < 1 ? r : 1);
	line_n++;
	if (line_n * 1000 / line_rate != (line_n - 1) * 1000 / line_rate) {
		k_jiffies++;
		run_timers();
	}
	irq_now = bm_irq(&pi) || bm_irq(&po);
	if (irq_now && !irq_was && isr) {
		int guard = 0;
		while ((bm_irq(&pi) || bm_irq(&po)) && guard++ < 4) {
			isr(IRQ, isr_dev, NULL);
			run_tasklets();
		}
		irq_now = bm_irq(&pi) || bm_irq(&po);
	}
	irq_was = irq_now;
	return sat(out);
}

static void line_idle(unsigned n)
{
	while (n--)
		line_sample(0);
}

/*
 *    main loop
 */

/* the DTE's input, as much as the modem takes; only commands need the
   fixed timing, so data (online) goes in at once */
static void dte_poll(void)
{
	unsigned char buf[256];
	int n, room;

	if (sync_n && line_n % sync_n && !online)
		return;
	room = mot_writeroom(tty);
	if (room <= 0)
		return;
	n = (int)read(pty, buf, room < (int)sizeof(buf) ? room : sizeof(buf));
	if (n > 0 && mot_write(tty, 0, buf, n) != n)
		LOG("mot_write: short\n");
}

int main(int argc, char **argv)
{
	short in[480], out[480];
	struct sigaction sa;
	char *name;
	int i, n, r;

	dbg = getenv("SM56_DEBUG") ? atoi(getenv("SM56_DEBUG")) : 0;
	(void)argc; (void)argv;
	/* SM56_RATE: the line's sample rate (8000) */
	if (getenv("SM56_RATE"))
		line_rate = atoi(getenv("SM56_RATE"));
	sync_n = getenv("SM56_SYNC") ? (atol(getenv("SM56_SYNC")) + 47) / 48 * 48 : 0;
	setvbuf(stdout, NULL, _IONBF, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_segv;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sa.sa_sigaction = on_trap;
	sigaction(SIGTRAP, &sa, NULL);
	if (current_patch() || rodata_writable()) {
		perror("sm56: mprotect");
		return 1;
	}
	ctype_init();
	malloc_sizes_init();
	codec_init();
	cfg_init();
	pi.sr = po.sr = SR_DCH;

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

	r = init_module();
	if (r) {
		fprintf(stderr, "sm56: init_module: %d\n", r);
		return 1;
	}
	tty_init();
	r = mot_open(tty, NULL);
	if (r) {
		fprintf(stderr, "sm56: mot_open: %d\n", r);
		return 1;
	}
	printf("created sm56: %s\n", name);

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
			if (line_n % 48 == 0)
				dte_poll();
		}
		if (write(4, out, n * 2) != n * 2)
			break;
		if (odd)
			((char *)in)[0] = ((char *)in)[n * 2];
		if (!sync_n)
			dte_poll();
	}
	return 0;
}
