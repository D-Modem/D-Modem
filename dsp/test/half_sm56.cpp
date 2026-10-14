/*
 *    half_sm56.cpp - Motorola's SM56 soft modem (sm56-06.09.05, sm56.ko
 *    of Fedora Core 3's kernel 2.6.9: its controller and data pumps, i386
 *    object code, V.92 with quick connect and modem-on-hold) as a test
 *    peer (modem_half.hpp): line audio from fd 3 and to fd 4 (16 bit,
 *    SM56_RATE, 8000 by default), AT commands on a pty whose name is
 *    printed ("created sm56: /dev/pts/N"); SM56_SYNC as the others'.
 *
 *    The module is linked in as it is, its imports renamed k_<name>
 *    (objcopy) and given here: a kernel of its own for it, regparm(3) as
 *    Fedora's was built, with the structures it was compiled against
 *    (FC3's: spinlocks with debugging, the 4G/4G split's PAGE_OFFSET of
 *    0x02000000, so that a bus address is the virtual one less that).
 *
 *    Its hardware: an Intel ICH AC'97 modem controller (8086:2416, its
 *    "is_" back end; hw/ich.hpp) with a Motorola modem codec ("MOTf",
 *    hw/ac97_codec.hpp) as the secondary codec, on port I/O it does with
 *    in and out itself: those trap (no I/O permission) and SIGSEGV's
 *    handler carries them out on the emulated devices (hw/x86_traps.hpp),
 *    PCI's configuration space at 0xcf8 too (hw/pci.hpp).  The codec
 *    runs at whatever rate the module sets it to (7200 to 12000 Hz, by
 *    modulation): a windowed sinc resampler between it and the line
 *    (hw/codec_line.hpp).
 *    Time is the samples': jiffies (HZ 1000), its timers and delays.
 *
 *    Its serial side is the tty driver of the module (mot_open,
 *    mot_write, and its receive path into the tty's flip buffer, whose
 *    work we run): a tty_struct of 2.6.9 with only what it uses.
 *
 *    The module is test/sm56/sm56.ko as Motorola shipped it (sha256
 *    a757fd85...; -DDMODEM_SM56_KO=<path> for another copy of it);
 *    sm56_obj.cmake renames its imports at build time.  It must be that
 *    build: the places patched are checked.  SM56_DEBUG=1 prints its
 *    printk and the port accesses we do not know, 2 every port access.
 */

#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "hw/ac97_codec.hpp"
#include "hw/codec_line.hpp"
#include "hw/ich.hpp"
#include "hw/pci.hpp"
#include "hw/x86_traps.hpp"
#include "modem_half.hpp"

using namespace half;

#define KAPI __attribute__((regparm(3)))

/*
 *    the module
 */

extern "C" {
int init_module(void);
KAPI int mot_open(void *tty, void *filp);
KAPI int mot_write(void *tty, int from_user, const void *buf, int count);
KAPI int mot_writeroom(void *tty);
}

#define PAGE_OFFSET 0x02000000u

/* the controller: its I/O BARs, its interrupt, 00:1f.6 */
#define NAMBAR  0xe000
#define NABMBAR 0xe400
#define IRQ     5
#define DEVFN   ((0x1f << 3) | 6)

/*
 *    the modem: the codec, the controller and PCI's configuration
 *    space, the time, the interrupt, the tty
 */

class Sm56 : public HalfModem, public Ac97Codec<Sm56>, public Ich<Sm56>,
	     public PciDevice<Sm56>, public X86Traps<Sm56>,
	     public CodecLine<Sm56> {
public:
	Sm56();

	/* its interrupt handler and its device */
	typedef KAPI int (*kisr)(int irq, void *dev, void *regs);
	kisr isr = nullptr;
	void *isr_dev = nullptr;

	void line_idle(unsigned n);
	u32 io_in(unsigned a, int size);
	void io_out(unsigned a, u32 v, int size);

protected:
	int start() override;
	short line_sample(short in) override;
	void dte_poll() override;

private:
	bool in_isr = false;            /* the ISR or its tasklets running */
};

static Sm56 sm56;
#define LOG(...) do { if (sm56.dbg) sm56.log(__VA_ARGS__); } while (0)

/* the address the module's bus address a stands for */
static void *bus_to_virt(u32 a) { return (void *)(uintptr_t)(a + PAGE_OFFSET); }

/* the Motorola codec (extended modem ID: line 1; "MOTf"), the
   controller's secondary, and the controller's configuration space:
   its IDs, I/O and bus master on, the modem class, the BARs (256 and
   128 octets of I/O), the IRQ; function 0 the ICH's LPC bridge */
Sm56::Sm56() :
	HalfModem("sm56", "SM56"),
	Ac97Codec(0x0001, 0x4d4f, 0x5466),
	Ich(NAMBAR, NABMBAR, true, PAGE_OFFSET)
{
	pci_devfn = DEVFN;
	pci_cfg[0x00 / 4] = 0x8086 | 0x2416u << 16;
	pci_cfg[0x04 / 4] = 0x02800005;         /* I/O, bus master */
	pci_cfg[0x08 / 4] = 0x07030002;         /* modem */
	pci_cfg[0x10 / 4] = NAMBAR | 1;
	pci_cfg[0x14 / 4] = NABMBAR | 1;
	pci_cfg[0x2c / 4] = 0x8086 | 0x2416u << 16;
	pci_cfg[0x3c / 4] = IRQ | 2 << 8;
	pci_bar_size[0] = 0x100;
	pci_bar_size[1] = 0x80;
	pci_fn0_id = 0x24108086;
}

/*
 *    time: the line's samples
 */

extern "C" unsigned long k_jiffies;
unsigned long k_jiffies;

/*
 *    port I/O
 */

/* a port read of size bytes: PCI's configuration ports, the secondary
   codec at NAMBAR + 0x80, the bus masters', GLOB_CNT and GLOB_STA at
   NABMBAR; all ones elsewhere */
u32 Sm56::io_in(unsigned a, int size)
{
	u32 v;
	if (!pci_io_in(a, size, &v) && !ich_io_in(a, size, &v)) {
		LOG("in%d %04x\n", size, a);
		v = size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;
	}
	if (dbg > 1)
		fprintf(stderr, "sm56: in%d %04x = %x\n", size, a, v);
	return v;
}

/* a port write of size bytes, as io_in has them */
void Sm56::io_out(unsigned a, u32 v, int size)
{
	if (dbg > 1)
		fprintf(stderr, "sm56: out%d %04x = %x\n", size, a, v);
	if (!pci_io_out(a, v, size) && !ich_io_out(a, v, size))
		LOG("out%d %04x = %x\n", size, a, v);
}

/* the module's in and out: a general protection fault without I/O
   permission; carried out here and stepped over; cli, sti and its mov
   to and from cr0 likewise */
static void on_segv(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = (ucontext_t *)ctx;
	greg_t *g = uc->uc_mcontext.gregs;
	byte *ip = (byte *)(uintptr_t)g[REG_EIP];

	if (sm56.traps_step(uc))
		return;
	fprintf(stderr, "sm56: fault at %p (%02x %02x %02x), address %p\n",
		(void *)ip, ip[0], ip[1], ip[2], si->si_addr);
	if (sm56.dbg) {
		char buf[4096];
		int fd = open("/proc/self/maps", O_RDONLY), k;
		while (fd >= 0 && (k = (int)read(fd, buf, sizeof(buf))) > 0)
			if (write(2, buf, k) < 0)
				break;
		if (fd >= 0)
			close(fd);
		fprintf(stderr, "eax %x ebx %x ecx %x edx %x esi %x edi %x ebp %x esp %x\n",
			(u32)g[REG_EAX], (u32)g[REG_EBX], (u32)g[REG_ECX], (u32)g[REG_EDX],
			(u32)g[REG_ESI], (u32)g[REG_EDI], (u32)g[REG_EBP], (u32)g[REG_ESP]);
	}
	signal(sig, SIG_DFL);
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

/* SIGTRAP at a patched place: eax our thread_info, on past the nop */
static void on_trap(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = (ucontext_t *)ctx;
	greg_t *g = uc->uc_mcontext.gregs;
	(void)sig; (void)si;
	g[REG_EAX] = (greg_t)(uintptr_t)thread_info;
	g[REG_EIP] += 1;        /* (past the nop) */
}

/* its interrupt and its deferred routine save the FPU (fnsave, 108
   bytes) to .rodata.cst4 + 0x640 and + 0x644: past the end of that
   section (0x648 bytes), into what follows it, and the linker merges and
   reorders those constants, so what they overwrite depends on the link
   (module first: its own constants, which its V.90 interpolation then
   reads wrong).  Their own room for them instead: fnsave and frstor
   pointed there */
static const u32 fpu_at[] = { 0xb4db0, 0xb4dc4, 0xb4e75, 0xb4e85 };
static byte fpu_save[2][128] __attribute__((aligned(16)));

/* fnsave and frstor at fpu_at pointed at fpu_save; -1 if the module is
   not the one expected or its text cannot be written */
static int fpu_patch(void)
{
	uintptr_t text = (uintptr_t)init_module - INIT_MODULE_AT;
	unsigned i;

	for (i = 0; i < sizeof(fpu_at) / sizeof(fpu_at[0]); i++) {
		byte *p = (byte *)(text + fpu_at[i]);
		u32 a = (u32)(uintptr_t)fpu_save[i / 2];
		if (p[0] != 0xdd || (p[1] != 0x35 && p[1] != 0x25)) {
			fprintf(stderr, "sm56: not the module expected (%x)\n",
				fpu_at[i]);
			return -1;
		}
		if (mprotect((void *)((uintptr_t)p & ~4095u), 8192,
			     PROT_READ | PROT_WRITE | PROT_EXEC))
			return -1;
		memcpy(p + 2, &a, 4);
	}
	return 0;
}

/* our thread_info and task; each and %esp,%eax at current_at made
   int3, nop; -1 if the module is not the one expected */
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

extern "C" {

/* its printk: to stderr, SM56_DEBUG */
int k_printk(const char *fmt, ...)
{
	va_list ap;
	int n = 0;
	if (sm56.dbg) {
		va_start(ap, fmt);
		/* (the level's <n> left as it is) */
		n = vfprintf(stderr, fmt, ap);
		va_end(ap);
	}
	return n;
}
/* the kernel's library and the like: the C library's, or nothing */
KAPI void *k_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
KAPI void *k_memset(void *d, int c, size_t n) { return memset(d, c, n); }
KAPI void k_dump_stack(void) { LOG("dump_stack\n"); }
KAPI int k_capable(int cap) { (void)cap; return 1; }
KAPI void k___might_sleep(const char *file, int line) { (void)file; (void)line; }
KAPI int k___cond_resched(void) { return 0; }
u32 k_kernel_flag[8];
unsigned char k__ctype[256];

}

/* the kernel's _ctype (_U _L _D _C _P _S _X _SP) from the C library's */
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
extern "C" struct cache_sizes k_malloc_sizes[32];
struct cache_sizes k_malloc_sizes[32];
/* kmalloc's size classes (32 bytes up), all one cache of 128 kB
   objects */
static void malloc_sizes_init(void)
{
	int i;
	for (i = 0; i < 32; i++) {
		k_malloc_sizes[i].size = 32u << (i < 12 ? i : 12);
		k_malloc_sizes[i].cachep = &kcache;
		k_malloc_sizes[i].dmacachep = &kcache;
	}
}

extern "C" {

/* an object of the cache, zeroed */
KAPI void *k_kmem_cache_alloc(void *cachep, unsigned flags)
{
	(void)flags;
	return calloc(1, ((struct kcache *)cachep)->size);
}
/* and the rest of its memory: calloc (64 bytes to spare), no I/O
   mapped */
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
KAPI void k___udelay(unsigned long us)
{
	sm56.line_idle((unsigned)(us * sm56.line_rate / 1000000) + 1);
}
KAPI void k___const_udelay(unsigned long xloops) { k___udelay(xloops / 0x10c7); }

}

/* semaphores: never contended here (one thread) */
__asm__(".globl k___down_failed\n k___down_failed: ret\n"
	".globl k___up_wakeup\n k___up_wakeup: ret\n");

extern "C" {

KAPI void k___wake_up(void *q, unsigned mode, int nr, void *key)
{
	(void)q; (void)mode; (void)nr; (void)key;
}

}

/* timers (FC3's struct timer_list: expires at 8, function at 0x28, data
   at 0x2c) */
#define NTIMERS 16
static byte *timers[NTIMERS];
typedef KAPI void (*kfn)(unsigned long);
#define T_EXPIRES(t) (*(unsigned long *)((t) + 0x08))
#define T_FN(t) (*(kfn *)((t) + 0x28))
#define T_DATA(t) (*(unsigned long *)((t) + 0x2c))

extern "C" {

/* timer t due at jiffies expires (on the list if not yet); 1 if it was
   pending */
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
/* timer t off; 1 if it was pending */
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

}

/* the timers due, each off and called */
static void run_timers(void)
{
	int i;
	for (i = 0; i < NTIMERS; i++) {
		byte *t = timers[i];
		if (t && (long)(k_jiffies - T_EXPIRES(t)) >= 0) {
			timers[i] = NULL;
			stack_clear();
			T_FN(t)(T_DATA(t));
		}
	}
}

/* tasklets: next, state (bit 0 scheduled), count, func, data */
#define NTASKLETS 8
static byte *tasklets[NTASKLETS];
static int ntasklets;

extern "C" {

/* tasklet t to run after the interrupt (8 queued at most) */
KAPI void k___tasklet_schedule(void *t)
{
	if (ntasklets < NTASKLETS)
		tasklets[ntasklets++] = (byte *)t;
}

}

/* the tasklets queued, in order: their scheduled bit cleared, those
   enabled (count 0) called with their data */
static void run_tasklets(void)
{
	while (ntasklets) {
		byte *t = tasklets[0];
		memmove(tasklets, tasklets + 1, --ntasklets * sizeof(tasklets[0]));
		*(u32 *)(t + 4) &= ~1u;
		if (*(int *)(t + 8) == 0) {
			stack_clear();
			(*(kfn *)(t + 12))(*(unsigned long *)(t + 16));
		}
	}
}

/* PCI: the controller, for pci_find_device; its configuration space at
   0xcf8 (the module reads it there) */
static byte pci_bus[0x100];
static byte pci_dev[0x400];

/* the tty driver; the tty (2.6.9): the flip buffer at 0xec (its work),
   char_buf_ptr 0x160, flag_buf_ptr 0x164, count 0x168, char_buf 0x170 */
static void *tty_driver;
#define TTY_FLIPBUF 512
static byte tty[0x2000];
/* its termios (at 0x80): 115200 bit/s, 8N1, CREAD, CLOCAL, raw */
static u32 termios[16] = { 0, 0, 010000 | 2 | 060 | 0200 | 04000, 0 };

/* the flip buffer empty */
static void flip_reset(void)
{
	*(byte **)(tty + 0x160) = tty + 0x170;
	*(byte **)(tty + 0x164) = tty + 0x170 + 2 * TTY_FLIPBUF;
	*(int *)(tty + 0x168) = 0;
}
/* the tty: zeros, our termios, an empty flip buffer */
static void tty_init(void)
{
	memset(tty, 0, sizeof(tty));
	*(u32 **)(tty + 0x80) = termios;
	flip_reset();
}
/* the flip buffer's characters to the pty (and the result codes'
   scan); empty */
static void flip_push(void)
{
	int n = *(int *)(tty + 0x168);
	if (n > 0) {
		sm56.pty.write(tty + 0x170, n);
		sm56.results.scan(tty + 0x170, n);
	}
	flip_reset();
}

extern "C" {

/* the interrupt: its handler, for line_sample to call */
KAPI int k_request_irq(unsigned irq, void *handler, unsigned long flags,
		       const char *name, void *dev)
{
	(void)flags;
	LOG("irq %u for %s\n", irq, name ? name : "?");
	sm56.isr = (Sm56::kisr)handler;
	sm56.isr_dev = dev;
	return 0;
}
/* and no more */
KAPI void k_free_irq(unsigned irq, void *dev) { (void)irq; (void)dev; sm56.isr = NULL; }

/* the controller, the first time 8086:2416 is asked for; NULL else */
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
/* bus mastering: on already */
KAPI void k_pci_set_master(void *dev) { (void)dev; }

/* its character devices: noted only */
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
/* its tty driver: kept; mot_open and mot_write are called directly */
KAPI int k_tty_register_driver(void *d) { tty_driver = d; return 0; }
KAPI int k_tty_unregister_driver(void *d) { (void)d; return 0; }

/* the tty's flip buffer work at once; other work noted only */
KAPI int k_schedule_delayed_work(void *work, unsigned long delay)
{
	(void)delay;
	if ((byte *)work == tty + 0xec)
		flip_push();
	else
		LOG("schedule_delayed_work %p\n", work);
	return 1;
}

}

/*
 *    the line
 */

/* one line sample each way through the resamplers (a codec sample each
   way through the bus masters); a jiffy each ms and the timers due;
   the ISR and tasklets while an interrupt is up */
short Sm56::line_sample(short in)
{
	short out = codec_line_sample(in, (double)line_rate / codec_rate,
				      [this](short adc) {
		short dac = ich_sample(pcm_out, 0);
		ich_sample(pcm_in, adc);
		return dac;
	});

	line_n++;
	if (line_n * 1000 / line_rate != (line_n - 1) * 1000 / line_rate) {
		k_jiffies++;
		run_timers();
	}
	/* a level, as on the PCI bus: taken while it is up (4 times at
	   most a sample), not from within the ISR or its tasklets (a
	   udelay there runs the line on) */
	if (ich_irq() && isr && !in_isr) {
		int guard = 0;
		in_isr = true;
		while (ich_irq() && guard++ < 4) {
			stack_clear();
			isr(IRQ, isr_dev, NULL);
			run_tasklets();
		}
		in_isr = false;
	}
	return out;
}

/* n silent samples of the line */
void Sm56::line_idle(unsigned n)
{
	while (n--)
		line_sample(0);
}

/* what waits to the DTE, then the DTE's input, as much as the modem
   takes; only commands need the fixed timing, so data (online) goes in
   at once */
void Sm56::dte_poll()
{
	pty.flush();
	if (dte_waits())
		return;
	stack_clear();
	pty.read(mot_writeroom(tty), [](const void *p, int n) {
		stack_clear();
		return mot_write(tty, 0, p, n);
	});
}

/* the traps, the patches, init_module and mot_open */
int Sm56::start()
{
	struct sigaction sa;
	int r;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_segv;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sa.sa_sigaction = on_trap;
	sigaction(SIGTRAP, &sa, NULL);
	if (current_patch() || fpu_patch()) {
		perror("sm56: mprotect");
		return -1;
	}
	ctype_init();
	malloc_sizes_init();

	stack_clear();
	r = init_module();
	if (r) {
		fprintf(stderr, "sm56: init_module: %d\n", r);
		return -1;
	}
	tty_init();
	stack_clear();
	r = mot_open(tty, NULL);
	if (r) {
		fprintf(stderr, "sm56: mot_open: %d\n", r);
		return -1;
	}
	return 0;
}

int main()
{
	return sm56.run();
}
