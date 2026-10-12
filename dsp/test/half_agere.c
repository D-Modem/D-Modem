/*
 *    half_agere.c - the Agere Systems soft modem (agrsm 2.1.x,
 *    agrmodemlib.o: its controller and data pumps, i386 object code) as a
 *    test peer, the way modem_half runs the original dsplibs.o: line audio
 *    from fd 3 and to fd 4 (16 bit, 8000 Hz), AT commands on a pty whose
 *    name is printed ("created agere: /dev/pts/N").
 *
 *    The library drives its hardware itself; here it gets an Intel ICH
 *    AC'97 modem controller (MC97, the "AMR Intel MB" of the library) and a
 *    Silicon Labs Si3038 modem codec, emulated on its port I/O:
 *
 *    NAMBAR    the codec's registers (0x7c/0x7e "SIL'", 0x40 line rate,
 *              0x3e ready, 0x54 GPIO)
 *    NABMBAR   the bus master: PCM in at 0x00, PCM out at 0x10 (BDBAR,
 *              CIV, LVI, SR, PICB, CR), GLOB_CNT 0x3c, GLOB_STA 0x40
 *
 *    Each direction walks its buffer descriptor list a sample at a time;
 *    a finished buffer with IOC raises the interrupt: the library's ISR,
 *    then its bottom half.  Time is the samples' (the library's clock,
 *    timer and delays), so a run does not depend on the host's speed.
 *    Its serial side is a virtual 16550 (read_vuart_port and
 *    write_vuart_port) between it and the pty.
 *
 *    The library is test/agere/agrsm_core.o (agrsm-20070804: the object
 *    with LXHardwareInfoCreate weakened, as its Makefile makes it);
 *    -DDMODEM_AGERE_LIB=<path> for another copy of it.
 *    AGERE_DEBUG=1 prints its log and the port accesses we do not know.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#include <ctype.h>

/* the driver's regparm(0): the default here (no -mregparm) */
#define fnatr
typedef unsigned char byte;
typedef unsigned short word;
typedef unsigned int u32;

static int dbg;
#define LOG(fmt, ...) do { if (dbg) fprintf(stderr, "agere: " fmt, ##__VA_ARGS__); } while (0)

/*
 *    the library
 */

extern int linux_modem_init(void);
extern int linux_modem_open(void);
extern int linux_modem_close(void);
extern int lucent_detect_modem(void *res) fnatr;
extern void lucent_init_modem(void) fnatr;
extern int read_vuart_port(int offset) fnatr;
extern void write_vuart_port(int offset, int value) fnatr;
extern byte vxdPortOpen(void) fnatr;
extern void vxdPortClose(void) fnatr;
extern void timertick_function(unsigned long instance) fnatr;
extern void LXHardwareBottomHalf(void *hardware) fnatr;
extern int LX_isr_handler(int irq, void *isr, void *regs) fnatr;

/* filled by us, read by the library */
word BaseAddress, BaseAddress2;
byte Irq;

typedef struct { word vendor, device, subsys, subvendor; } SUPPORTED_DEVICES;
const SUPPORTED_DEVICES agereSupDevices[] = { { 0x8086, 0x24C6, 0, 0 } };
int numSupportedDevice = 1;

/* agr.h of the driver: the hardware instance list LXHardwareInfoCreate
   gives the library */
struct agr_instance_info {
	const char *hierarchy_name;
	const char *name;
	unsigned g_idx, num, node, all_idx;
	unsigned codec_type;
	struct {
		u32 space1[(0x40 - 0x1c) / 4];
		unsigned io;            /* 1 PCI, 2 AMR (AC'97) */
		unsigned res0, res1;    /* NAMBAR, NABMBAR */
		u32 space2[(0x54 - 0x4c) / 4];
		unsigned cd;
		unsigned irq;
	} res;
	u32 space3[(0xa0 - 0x5c) / 4];
	u32 spec2;
};
static struct { int count; struct agr_instance_info slot[0x20]; } infos;

#define NAMBAR  0x1000
#define NABMBAR 0x2000
#define IRQ     11

int LXHardwareInfoCreate(void **pp) fnatr;
int LXHardwareInfoCreate(void **pp)
{
	struct agr_instance_info *i = &infos.slot[0];
	memset(&infos, 0, sizeof(infos));
	/* as the library's own probe makes it for an ICH4 (8086:24c6) */
	i->hierarchy_name = "AMR Intel MB";
	i->name = "AC97";
	i->num = 1;
	i->codec_type = 0x10;
	i->spec2 = 3;
	i->res.io = 2;
	i->res.res0 = NAMBAR;
	i->res.res1 = NABMBAR;
	i->res.cd = 1;
	i->res.irq = IRQ;
	infos.count = 1;
	BaseAddress = NAMBAR;
	BaseAddress2 = NABMBAR;
	Irq = IRQ;
	*pp = &infos;
	return 0;
}
int LXHardwareInfoDestroy(void **pp) fnatr;
int LXHardwareInfoDestroy(void **pp) { (void)pp; return 0; }

/*
 *    time: the samples'
 */

static unsigned long long now_us;       /* 125 us a sample */
static int idle_reads;                  /* clock reads since a sample */

/* the library also waits by reading the clock in a loop (CodecDrvWait):
   between samples, time creeps on with the reads */
static void idle_sample(void);
static unsigned long long clock_read(void)
{
	if (++idle_reads > 32) {
		/* a sample's time, and the bus masters go on with it
		   (silence in; the interrupt waits for the next real
		   sample) */
		now_us += 125;
		idle_sample();
	}
	return now_us;
}
static void (*timer_fn)(unsigned long);
static int timer_armed;
static unsigned long long timer_at;
#define TICK_US 4000                    /* a jiffy (HZ 250) */

fnatr void lt_init_timer(void) { timer_armed = 0; }
fnatr void lt_add_timer(void (*fn)(unsigned long))
{
	if (!timer_fn)
		LOG("timer\n");
	timer_fn = fn;
	timer_armed = 1;
	timer_at = now_us + TICK_US;
}
fnatr u32 VMODEM_Get_System_Time(void) { return (u32)(clock_read() / 1000); }
fnatr void *x_do_gettimeofday(void *tv)
{
	struct timeval *t = (struct timeval *)tv;
	unsigned long long us = clock_read();
	t->tv_sec = (long)(us / 1000000);
	t->tv_usec = (long)(us % 1000000);
	return tv;
}
static void delay_samples(unsigned n);
/* a busy wait, interrupts on (LXHardwareStart waits for the first DMA
   interrupt so): the line runs on, silent */
fnatr void x_ms_delay(unsigned ms) { delay_samples(ms * 8); }
fnatr void x__SLOW_DOWN_IO(void) {}

/*
 *    the Si3038 codec
 */

static word codec[0x80];

static void codec_init(void)
{
	memset(codec, 0, sizeof(codec));
	codec[0x3c] = 0x0001;           /* extended modem ID: line 1 */
	codec[0x40] = 8000;
	codec[0x7c] = 0x5349;           /* "SI" */
	codec[0x7e] = 0x4c27;           /* "L'": Si3038 */
	codec[0x62] = 0x7000;           /* line side device: 7 */
}

static word codec_read(int r)
{
	r &= 0x7e;
	switch (r) {
	case 0x3e:
		/* powered up (8..11 clear): ADC/DAC of both lines ready */
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
	if (r == 0x62)
		v = (v & ~0x7000) | 0x7000;
	if (r == 0x40 && v != 8000)
		fprintf(stderr, "agere: line rate %u Hz, only 8000 is there\n", v);
	codec[r] = v;
}

/*
 *    the ICH AC'97 modem controller
 */

struct bm {
	u32 bdbar;
	byte civ, lvi, cr;
	word sr, picb;
	int pos;                /* samples done in the current buffer */
};
static struct bm pi, po;        /* PCM in (to the library), PCM out */
static u32 glob_cnt, glob_sta = 0x100;  /* the primary codec is ready */

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
	return (u32 *)(unsigned long)(b->bdbar + 8 * (i & 31));
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

/* a sample through a running bus master: in to memory (PCM in) or out of
   it (PCM out) */
static short bm_sample(struct bm *b, short in)
{
	short out = 0;
	u32 *d;
	short *buf;

	if (!(b->cr & CR_RPBM) || (b->sr & SR_DCH) || !b->bdbar)
		return 0;
	d = bd(b, b->civ);
	buf = (short *)(unsigned long)d[0];
	if (b->pos < (int)(d[1] & 0xffff) && buf) {
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

static u32 io_in2(unsigned a, int size);
static u32 io_in(unsigned a, int size)
{
	u32 v = io_in2(a, size);
	if (dbg > 1)
		fprintf(stderr, "agere: in%d %04x = %x\n", size, a, v);
	return v;
}

static u32 io_in2(unsigned a, int size)
{
	if (a >= NAMBAR && a < NAMBAR + 0x100)
		return codec_read(a - NAMBAR);
	if (a >= NABMBAR && a < NABMBAR + 0x80) {
		int o = a - NABMBAR;
		struct bm *b = o < 0x10 ? &pi : o < 0x20 ? &po : NULL;
		if (b) {
			switch (o & 0xf) {
			case 0x0: return b->bdbar;
			case 0x4: return size == 1 ? b->civ :
				b->civ | b->lvi << 8 | (u32)b->sr << 16;
			case 0x5: return b->lvi;
			case 0x6: return b->sr;
			case 0x8: return b->picb;
			case 0xa: return (b->civ + 1) & 31;
			case 0xb: return b->cr;
			}
		}
		switch (o) {
		case 0x3c: return glob_cnt;
		case 0x40: return glob_status();
		case 0x44: return 0;            /* the codec is ours */
		}
	}
	LOG("in%d %04x\n", size, a);
	return size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;
}

static void io_out(unsigned a, u32 v, int size)
{
	if (dbg > 1)
		fprintf(stderr, "agere: out%d %04x = %x\n", size, a, v);
	if (a >= NAMBAR && a < NAMBAR + 0x100) {
		codec_write(a - NAMBAR, (word)v);
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
		}
		switch (o) {
		case 0x3c: glob_cnt = v; return;
		case 0x40: glob_sta &= ~(v & 0xc01); return;
		case 0x44: return;
		}
	}
	LOG("out%d %04x = %x\n", size, a, v);
}

fnatr byte agr_inp(unsigned a) { return (byte)io_in(a, 1); }
fnatr word agr_inpw(unsigned a) { return (word)io_in(a, 2); }
fnatr unsigned agr_inpl(unsigned a) { return io_in(a, 4); }
fnatr void agr_outp(unsigned a, byte v) { io_out(a, v, 1); }
fnatr void agr_outpw(unsigned a, word v) { io_out(a, v, 2); }
fnatr void agr_outpl(unsigned a, unsigned v) { io_out(a, v, 4); }
fnatr unsigned long inpd(unsigned long a) { return io_in(a, 4); }
fnatr void outpd(unsigned long a, unsigned long v) { io_out(a, v, 4); }
fnatr void ModemRegOut8(unsigned p, byte v) { io_out(p + BaseAddress, v, 1); }
fnatr void ModemRegOut16(unsigned p, unsigned v) { io_out(p + BaseAddress, v, 2); }
fnatr void ModemRegOut32(unsigned p, unsigned long v) { io_out(p + BaseAddress, v, 4); }
fnatr byte ModemRegIn8(unsigned p) { return (byte)io_in(p + BaseAddress, 1); }
fnatr word ModemRegIn16(unsigned p) { return (word)io_in(p + BaseAddress, 2); }
fnatr unsigned long ModemRegIn32(unsigned p) { return io_in(p + BaseAddress, 4); }

/*
 *    the operating system
 */

fnatr void *x_vmalloc(unsigned n) { return calloc(1, n); }
fnatr void x_vfree(void *p) { free(p); }
fnatr void *x_kmalloc(unsigned n) { return calloc(1, n); }
fnatr void x_kfree(void *p) { free(p); }
fnatr unsigned x_virt_to_bus(void *p) { return (unsigned)(unsigned long)p; }
fnatr void *x_ioremap_nocache(unsigned a, unsigned n)
{
	LOG("ioremap %x %u\n", a, n);
	return NULL;
}
fnatr void x_iounmap(void *p) { (void)p; }
fnatr void x_save_flags(unsigned long *f) { *f = 0; }
fnatr void x_restore_flags(unsigned long *f) { (void)f; }
fnatr void x_sti(void) {}
fnatr void x_cli(void) {}
fnatr void *x_request_region(unsigned long s, unsigned long n, const char *name)
{
	(void)s; (void)n;
	return (void *)name;
}
fnatr int x_check_region(unsigned long s, unsigned long n) { (void)s; (void)n; return 0; }
fnatr void x_release_region(unsigned long s, unsigned long n) { (void)s; (void)n; }

static void *isr_dev;
static int isr_on;
int x_request_irq(unsigned irq, void *handler, unsigned long flags,
		  const char *name, void *dev) fnatr;
int x_request_irq(unsigned irq, void *handler, unsigned long flags,
		  const char *name, void *dev)
{
	(void)handler; (void)flags;
	LOG("irq %u for %s\n", irq, name ? name : "?");
	isr_dev = dev;
	isr_on = 1;
	return 0;
}
fnatr void x_free_irq(unsigned irq, void *dev) { (void)irq; (void)dev; isr_on = 0; }

/* the bottom half: run after the ISR that queues it */
typedef void (*x_func_ptr)(void *);
struct task { void *data; int queued; };
static struct task *tasks[8];
static int ntasks;
void x_task_queue_init(void *t, x_func_ptr f, void *data) fnatr;
void x_task_queue_init(void *t, x_func_ptr f, void *data)
{
	struct task *k = (struct task *)t;
	(void)f;
	k->data = data;
	k->queued = 0;
}
fnatr void x_queue_task(void *t)
{
	struct task *k = (struct task *)t;
	if (!k->queued && ntasks < 8) {
		k->queued = 1;
		tasks[ntasks++] = k;
	}
}

static void run_tasks(void)
{
	while (ntasks) {
		struct task *k = tasks[0];
		memmove(tasks, tasks + 1, --ntasks * sizeof(tasks[0]));
		k->queued = 0;
		LXHardwareBottomHalf(k->data);
	}
}

fnatr char *x_strcpy(char *d, const char *s) { return strcpy(d, s); }
fnatr char *x_strncpy(char *d, const char *s, int n) { return strncpy(d, s, n); }
fnatr char *x_strcat(char *d, const char *s) { return strcat(d, s); }
fnatr int x_strlen(const char *s) { return (int)strlen(s); }
fnatr char *x_strchr(const char *s, char c) { return strchr(s, c); }
fnatr void *x_memset(void *d, char c, int n) { return memset(d, c, n); }
fnatr void *x_memcpy(void *d, const void *s, int n) { return memcpy(d, s, n); }
fnatr int x_memcmp(const void *a, const void *b, int n) { return memcmp(a, b, n); }
fnatr byte x_isprint(char c) { return isprint((unsigned char)c) != 0; }
fnatr byte x_toupper(char c) { return (byte)toupper((unsigned char)c); }
int x_sprintf(char *buf, const char *fmt, ...) fnatr;
int x_sprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsprintf(buf, fmt, ap);
	va_end(ap);
	return n;
}

fnatr void linux_debug_message(byte *msg) { LOG("%s\n", msg); }
void x_linux_dbg_print(const char *fmt, ...) fnatr;
void x_linux_dbg_print(const char *fmt, ...)
{
	va_list ap;
	if (!dbg)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
void x_linux_dbg_print_crit(const char *fmt, ...) fnatr;
void x_linux_dbg_print_crit(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

fnatr int x_pcibios_present(void) { return 1; }
fnatr unsigned agr_pci_find_device(unsigned v, unsigned d, unsigned *irq)
{
	LOG("pci_find_device %04x:%04x\n", v, d);
	(void)irq;
	return 0;
}
#define PCICFG(name, type) \
	fnatr int name(byte bus, byte devfn, byte where, type *val) \
	{ LOG(#name " %02x\n", where); (void)bus; (void)devfn; *val = 0; return 0; }
PCICFG(x_pcibios_read_config_byte, byte)
PCICFG(x_pcibios_read_config_word, word)
PCICFG(x_pcibios_read_config_dword, unsigned)
fnatr int x_pcibios_write_config_word(byte b, byte df, byte w, word v)
{ (void)b; (void)df; (void)w; (void)v; return 0; }
fnatr int x_pcibios_write_config_dword(byte b, byte df, byte w, unsigned v)
{ (void)b; (void)df; (void)w; (void)v; return 0; }

fnatr int x_smp_processor_id(void) { return 0; }
fnatr int x_smp_num_cpus(void) { return 1; }
fnatr void x_MOD_INC_USE_COUNT(void) {}
fnatr void x_MOD_DEC_USE_COUNT(void) {}
fnatr int x_atomic_read(int *a) { return *a; }
fnatr void x_atomic_set(int *a, int v) { *a = v; }
fnatr void x_atomic_inc(int *a) { ++*a; }
fnatr void x_atomic_dec(int *a) { --*a; }
fnatr int x_atomic_dec_and_test(int *a) { return --*a == 0; }

/* the serial driver's: the library calls it when its UART has news */
int (*agr_rs_interrupt)(void);

/*
 *    the virtual UART and the pty
 */

#define UART_RX  0
#define UART_IER 1
#define UART_LCR 3
#define UART_MCR 4
#define UART_LSR 5

static int pty;
/* AGERE_SYNC=<samples>: the DTE's input only every that many samples of
   the line, so that a test's commands meet the line at the same samples
   every time */
static long sync_n, sync_phase, line_n; /* AGERE_SYNC_PHASE: at that
						   sample of each period */

/* online between its CONNECT and NO CARRIER or OK (the UART has no
   DCD to tell) */
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

static void uart_poll(void)
{
	unsigned char buf[256];
	int n = 0, k;

	/* the modem's output to the DTE */
	while (n < (int)sizeof(buf) && (read_vuart_port(UART_LSR) & 1))
		buf[n++] = (unsigned char)read_vuart_port(UART_RX);
	if (n && write(pty, buf, n) < 0)
		LOG("pty write: %s\n", strerror(errno));
	if (sync_n)
		for (k = 0; k < n; k++)
			result_scan(buf[k]);
	/* the DTE's input, while the modem takes it; only commands need
	   the fixed timing, so data (online) goes in at once */
	if (sync_n && line_n % sync_n != sync_phase && !online)
		return;
	for (k = 0; k < 64 && (read_vuart_port(UART_LSR) & 0x20); k++) {
		unsigned char c;
		if (read(pty, &c, 1) != 1)
			break;
		write_vuart_port(UART_RX, c);

	}
}

static int rs_interrupt(void)
{
	return 0;
}

/*
 *    main loop: a sample at a time through both bus masters
 */

static long creeps;
static void idle_sample(void)
{
	if (++creeps % 1000 == 1 && dbg > 1)
		LOG("clock creep %ld at %llu ms\n", creeps, now_us / 1000);
	bm_sample(&po, 0);
	bm_sample(&pi, 0);
}

static void raise_irq(void)
{
	int guard = 0;
	static long nirq;
	if (++nirq % 1000 == 1 && dbg > 1)
		LOG("irq %ld, sta %x, pi civ %d lvi %d, po civ %d lvi %d\n", nirq,
		    glob_status(), pi.civ, pi.lvi, po.civ, po.lvi);
	while (isr_on && (glob_status() & 0x7) && guard++ < 4) {
		LX_isr_handler(IRQ, isr_dev, NULL);
		run_tasks();
	}
}

static short line_sample(short in);
static void delay_samples(unsigned n)
{
	while (n--)
		line_sample(0);
}

static short line_sample(short in)
{
	short out;
	int before = bm_irq(&pi) || bm_irq(&po);

	out = bm_sample(&po, 0);
	bm_sample(&pi, in);
	now_us += 125;
	idle_reads = 0;
	if (!before && (bm_irq(&pi) || bm_irq(&po)))
		raise_irq();
	if (timer_armed && now_us >= timer_at) {
		timer_armed = 0;
		if (timer_fn)
			timertick_function(0);
	}
	return out;
}

int main(int argc, char **argv)
{
	unsigned char res[8] = { 0 };
	short in[480], out[480];
	char *name;
	int i, n;

	(void)argc; (void)argv;
	dbg = getenv("AGERE_DEBUG") ? atoi(getenv("AGERE_DEBUG")) : 0;
	sync_n = getenv("AGERE_SYNC") ? (atol(getenv("AGERE_SYNC")) + 47) / 48 * 48 : 0;
	sync_phase = getenv("AGERE_SYNC_PHASE") ?
		atol(getenv("AGERE_SYNC_PHASE")) / 48 * 48 % (sync_n ? sync_n : 1) : 0;
	setvbuf(stdout, NULL, _IONBF, 0);
	codec_init();
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

	if (linux_modem_init()) {
		fprintf(stderr, "agere: linux_modem_init failed\n");
		return 1;
	}
	if (lucent_detect_modem(res)) {
		fprintf(stderr, "agere: no modem detected\n");
		return 1;
	}
	lucent_init_modem();
	agr_rs_interrupt = rs_interrupt;
	linux_modem_open();
	if (!vxdPortOpen())
		LOG("vxdPortOpen: 0\n");
	/* as the serial driver opens it: 115200 bit/s, 8N1, FIFOs */
	write_vuart_port(UART_LCR, 0x83);
	write_vuart_port(0, 1);
	write_vuart_port(1, 0);
	write_vuart_port(UART_LCR, 0x03);
	write_vuart_port(2, 0xc7);
	write_vuart_port(UART_MCR, 0x0b);       /* DTR, RTS, OUT2 */
	write_vuart_port(UART_IER, 0x0f);
	printf("created agere: %s\n", name);

	/* the other side waits for us first */
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
			if (++line_n % 48 == 0)
				uart_poll();
		}
		if (write(4, out, n * 2) != n * 2)
			break;
		if (odd)
			((char *)in)[0] = ((char *)in)[n * 2];
		if (!sync_n)
			uart_poll();
	}
	return 0;
}
