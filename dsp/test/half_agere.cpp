/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    half_agere.cpp - the Agere Systems soft modem (agrsm 2.1.x,
 *    agrmodemlib.o: its controller and data pumps, i386 object code) as a
 *    test peer (modem_half.hpp), the way half_smartlink runs the original
 *    dsplibs.o: line audio from fd 3 and to fd 4 (16 bit, 8000 Hz), AT
 *    commands on a pty whose name is printed ("created agere:
 *    /dev/pts/N"); AGERE_SYNC and AGERE_SYNC_PHASE as the others'.
 *
 *    The library drives its hardware itself; here it gets an Intel ICH
 *    AC'97 modem controller (MC97, the "AMR Intel MB" of the library;
 *    hw/ich.hpp) and a Silicon Labs Si3038 modem codec
 *    (hw/ac97_codec.hpp), emulated on its port I/O:
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
 *    The library is test/agere/agrsm_core.o (Agere SoftModem 2.1.40 of
 *    agrsm-20070804: the object with LXHardwareInfoCreate and
 *    LXHardwareInfoDestroy weakened, as its Makefile makes it);
 *    -DDMODEM_AGERE_LIB=<path> for another copy of it.
 *    AGERE_DEBUG=1 prints its log and the port accesses we do not know.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "hw/ac97_codec.hpp"
#include "hw/ich.hpp"
#include "modem_half.hpp"

using namespace half;

/* the driver's regparm(0): the default here (no -mregparm) */
#define fnatr

/*
 *    the library
 */

extern "C" {
int linux_modem_init(void);
int linux_modem_open(void);
int linux_modem_close(void);
int lucent_detect_modem(void *res) fnatr;
void lucent_init_modem(void) fnatr;
int read_vuart_port(int offset) fnatr;
void write_vuart_port(int offset, int value) fnatr;
byte vxdPortOpen(void) fnatr;
void vxdPortClose(void) fnatr;
void timertick_function(unsigned long instance) fnatr;
void LXHardwareBottomHalf(void *hardware) fnatr;
int LX_isr_handler(int irq, void *isr, void *regs) fnatr;
}

#define NAMBAR  0x1000
#define NABMBAR 0x2000
#define IRQ     11

/*
 *    the modem: the codec, the controller, the time, the interrupt and
 *    the UART's DTE side
 */

class Agere : public HalfModem, public Ac97Codec<Agere>, public Ich<Agere> {
public:
	Agere();

	/* time: 125 us a sample; clock reads since a sample */
	unsigned long long now_us = 0;
	int idle_reads = 0;
	/* its timer */
	void (*timer_fn)(unsigned long) = nullptr;
	int timer_armed = 0;
	unsigned long long timer_at = 0;
	/* its interrupt; the ISR (or its bottom halves) running */
	void *isr_dev = nullptr;
	int isr_on = 0;
	bool in_isr = false;

	unsigned long long clock_read();
	void delay_samples(unsigned n);
	bool codec_write_hook(int r, word &v);
	u32 io_in(unsigned a, int size);
	void io_out(unsigned a, u32 v, int size);
	void run_tasks();

protected:
	int start() override;
	short line_sample(short in) override;
	void dte_poll() override;

private:
	long creeps = 0;

	short sample(short in);
	void idle_sample();
	void raise_irq();
};

static Agere agere;
#define LOG(...) do { if (agere.dbg) agere.log(__VA_ARGS__); } while (0)

/* the Si3038 (extended modem ID: line 1; "SIL'"), the primary codec of
   the controller */
Agere::Agere() :
	HalfModem("agere", "AGERE"),
	Ac97Codec(0x0001, 0x5349, 0x4c27),
	Ich(NAMBAR, NABMBAR, false, 0)
{
	codec_regs[0x62] = 0x7000;      /* line side device: 7 */
}

/*
 *    the library's hardware
 */

extern "C" {

/* filled by us, read by the library */
word BaseAddress, BaseAddress2;
byte Irq;

typedef struct { word vendor, device, subsys, subvendor; } SUPPORTED_DEVICES;
extern const SUPPORTED_DEVICES agereSupDevices[];
const SUPPORTED_DEVICES agereSupDevices[] = { { 0x8086, 0x24C6, 0, 0 } };
int numSupportedDevice = 1;

}

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

extern "C" {

/* the library's hardware probe: one ICH4 AC'97 modem controller at our
   NAMBAR, NABMBAR and IRQ; 0 */
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
/* nothing to free; 0 */
int LXHardwareInfoDestroy(void **pp) fnatr;
int LXHardwareInfoDestroy(void **pp) { (void)pp; return 0; }

}

/*
 *    time: the samples'
 */

/* the library also waits by reading the clock in a loop (CodecDrvWait):
   between samples, time creeps on with the reads */
unsigned long long Agere::clock_read()
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

#define TICK_US 4000                    /* a jiffy (HZ 250) */

extern "C" {

/* its timer off */
fnatr void lt_init_timer(void) { agere.timer_armed = 0; }
/* its timer: due a jiffy (TICK_US of sample time) from now, once */
fnatr void lt_add_timer(void (*fn)(unsigned long))
{
	if (!agere.timer_fn)
		LOG("timer\n");
	agere.timer_fn = fn;
	agere.timer_armed = 1;
	agere.timer_at = agere.now_us + TICK_US;
}
/* ms of sample time */
fnatr u32 VMODEM_Get_System_Time(void) { return (u32)(agere.clock_read() / 1000); }
/* sample time as a timeval */
fnatr void *x_do_gettimeofday(void *tv)
{
	struct timeval *t = (struct timeval *)tv;
	unsigned long long us = agere.clock_read();
	t->tv_sec = (long)(us / 1000000);
	t->tv_usec = (long)(us % 1000000);
	return tv;
}
/* a busy wait, interrupts on (LXHardwareStart waits for the first DMA
   interrupt so): the line runs on, silent */
fnatr void x_ms_delay(unsigned ms) { agere.delay_samples(ms * 8); }
/* no I/O delay */
fnatr void x__SLOW_DOWN_IO(void) {}

}

/*
 *    the Si3038 codec
 */

/* its registers but the IDs and the ready bits (hw/ac97_codec.hpp): the
   line side device kept 7, a rate other than 8000 Hz said to be
   missing */
bool Agere::codec_write_hook(int r, word &v)
{
	if (r == 0x62)
		v |= 0x7000;
	if (r == 0x40 && v != 8000)
		fprintf(stderr, "agere: line rate %u Hz, only 8000 is there\n", v);
	return false;
}

/*
 *    port I/O: the controller's
 */

/* a port read of size bytes (logged with AGERE_DEBUG=2): the codec's
   registers at NAMBAR; the bus masters', GLOB_CNT and GLOB_STA at
   NABMBAR; all ones elsewhere */
u32 Agere::io_in(unsigned a, int size)
{
	u32 v;
	if (!ich_io_in(a, size, &v)) {
		LOG("in%d %04x\n", size, a);
		v = size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;
	}
	if (dbg > 1)
		fprintf(stderr, "agere: in%d %04x = %x\n", size, a, v);
	return v;
}

/* a port write of size bytes, as io_in has them */
void Agere::io_out(unsigned a, u32 v, int size)
{
	if (dbg > 1)
		fprintf(stderr, "agere: out%d %04x = %x\n", size, a, v);
	if (!ich_io_out(a, v, size))
		LOG("out%d %04x = %x\n", size, a, v);
}

extern "C" {

/* the library's port I/O: absolute ports, and ModemReg* from
   BaseAddress */
fnatr byte agr_inp(unsigned a) { return (byte)agere.io_in(a, 1); }
fnatr word agr_inpw(unsigned a) { return (word)agere.io_in(a, 2); }
fnatr unsigned agr_inpl(unsigned a) { return agere.io_in(a, 4); }
fnatr void agr_outp(unsigned a, byte v) { agere.io_out(a, v, 1); }
fnatr void agr_outpw(unsigned a, word v) { agere.io_out(a, v, 2); }
fnatr void agr_outpl(unsigned a, unsigned v) { agere.io_out(a, v, 4); }
fnatr unsigned long inpd(unsigned long a) { return agere.io_in(a, 4); }
fnatr void outpd(unsigned long a, unsigned long v) { agere.io_out(a, v, 4); }
fnatr void ModemRegOut8(unsigned p, byte v) { agere.io_out(p + BaseAddress, v, 1); }
fnatr void ModemRegOut16(unsigned p, unsigned v) { agere.io_out(p + BaseAddress, v, 2); }
fnatr void ModemRegOut32(unsigned p, unsigned long v) { agere.io_out(p + BaseAddress, v, 4); }
fnatr byte ModemRegIn8(unsigned p) { return (byte)agere.io_in(p + BaseAddress, 1); }
fnatr word ModemRegIn16(unsigned p) { return (word)agere.io_in(p + BaseAddress, 2); }
fnatr unsigned long ModemRegIn32(unsigned p) { return agere.io_in(p + BaseAddress, 4); }

/*
 *    the operating system
 */

/* memory: calloc'd; a bus address the pointer (a 32 bit process) */
fnatr void *x_vmalloc(unsigned n) { return calloc(1, n); }
fnatr void x_vfree(void *p) { free(p); }
fnatr void *x_kmalloc(unsigned n) { return calloc(1, n); }
fnatr void x_kfree(void *p) { free(p); }
fnatr unsigned x_virt_to_bus(void *p) { return (unsigned)(unsigned long)p; }
/* no memory mapped I/O: NULL */
fnatr void *x_ioremap_nocache(unsigned a, unsigned n)
{
	LOG("ioremap %x %u\n", a, n);
	return NULL;
}
/* unmapping, the interrupt flag and I/O regions: nothing to do (one
   thread, every port ours) */
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

/* its interrupt: dev noted for LX_isr_handler, which raise_irq calls */
int x_request_irq(unsigned irq, void *handler, unsigned long flags,
		  const char *name, void *dev) fnatr;
int x_request_irq(unsigned irq, void *handler, unsigned long flags,
		  const char *name, void *dev)
{
	(void)handler; (void)flags;
	LOG("irq %u for %s\n", irq, name ? name : "?");
	agere.isr_dev = dev;
	agere.isr_on = 1;
	return 0;
}
/* and no more */
fnatr void x_free_irq(unsigned irq, void *dev) { (void)irq; (void)dev; agere.isr_on = 0; }

}

/* the bottom half: run after the ISR that queues it */
typedef void (*x_func_ptr)(void *);
struct task { void *data; int queued; };
static struct task *tasks[8];
static int ntasks;

extern "C" {

/* a bottom half: task t for data (its function is LXHardwareBottomHalf) */
void x_task_queue_init(void *t, x_func_ptr f, void *data) fnatr;
void x_task_queue_init(void *t, x_func_ptr f, void *data)
{
	struct task *k = (struct task *)t;
	(void)f;
	k->data = data;
	k->queued = 0;
}
/* t to run after the ISR, once (8 queued at most) */
fnatr void x_queue_task(void *t)
{
	struct task *k = (struct task *)t;
	if (!k->queued && ntasks < 8) {
		k->queued = 1;
		tasks[ntasks++] = k;
	}
}

}

/* the bottom halves queued, in order, each with its data */
void Agere::run_tasks()
{
	while (ntasks) {
		struct task *k = tasks[0];
		memmove(tasks, tasks + 1, --ntasks * sizeof(tasks[0]));
		k->queued = 0;
		stack_clear();
		LXHardwareBottomHalf(k->data);
	}
}

extern "C" {

/* the C library's, by the kernel layer's names */
fnatr char *x_strcpy(char *d, const char *s) { return strcpy(d, s); }
fnatr char *x_strncpy(char *d, const char *s, int n) { return strncpy(d, s, n); }
fnatr char *x_strcat(char *d, const char *s) { return strcat(d, s); }
fnatr int x_strlen(const char *s) { return (int)strlen(s); }
fnatr char *x_strchr(const char *s, char c) { return (char *)strchr(s, c); }
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

/* its log: with AGERE_DEBUG; x_linux_dbg_print_crit's always */
fnatr void linux_debug_message(byte *msg) { LOG("%s\n", msg); }
void x_linux_dbg_print(const char *fmt, ...) fnatr;
void x_linux_dbg_print(const char *fmt, ...)
{
	va_list ap;
	if (!agere.dbg)
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

/* PCI: there, but no device found and its configuration space zeros
   (LXHardwareInfoCreate gives the hardware) */
fnatr int x_pcibios_present(void) { return 1; }
fnatr unsigned agr_pci_find_device(unsigned v, unsigned d, unsigned *irq)
{
	LOG("pci_find_device %04x:%04x\n", v, d);
	(void)irq;
	return 0;
}
/* a configuration space read: 0 */
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

/* one CPU, no module counts, atomics plain */
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

}

/*
 *    the virtual UART and the DTE
 */

#define UART_RX  0
#define UART_IER 1
#define UART_LCR 3
#define UART_MCR 4
#define UART_LSR 5

/* the UART's output to the pty, then the pty's input into the UART
   while it has room (in command mode at AGERE_SYNC's phase only) */
void Agere::dte_poll()
{
	unsigned char buf[256];
	int n = 0, k;

	/* the modem's output to the DTE */
	stack_clear();
	while (n < (int)sizeof(buf) && (read_vuart_port(UART_LSR) & 1))
		buf[n++] = (unsigned char)read_vuart_port(UART_RX);
	if (n)
		pty.write(buf, n);
	else
		pty.flush();
	results.scan(buf, n);
	/* the DTE's input, while the modem takes it; only commands need
	   the fixed timing, so data (online) goes in at once */
	if (dte_waits())
		return;
	for (k = 0; k < 64; k++) {
		unsigned char c;
		stack_clear();
		if (!(read_vuart_port(UART_LSR) & 0x20) ||
		    ::read(pty.fd(), &c, 1) != 1)
			break;
		stack_clear();
		write_vuart_port(UART_RX, c);
	}
}

/* the serial driver's: polled instead */
static int rs_interrupt(void)
{
	return 0;
}

/*
 *    the line: a sample at a time through both bus masters
 */

/* a sample's time without the line: the bus masters move on, silent */
void Agere::idle_sample()
{
	if (++creeps % 1000 == 1 && dbg > 1)
		LOG("clock creep %ld at %llu ms\n", creeps, now_us / 1000);
	ich_sample(pcm_out, 0);
	ich_sample(pcm_in, 0);
}

/* the interrupt: the ISR and the bottom halves while GLOB_STA asks for
   it (4 times at most; still up, it is taken again at the next sample),
   not from within them (a delay there runs the line on) */
void Agere::raise_irq()
{
	int guard = 0;
	static long nirq;
	if (in_isr)
		return;
	if (++nirq % 1000 == 1 && dbg > 1)
		LOG("irq %ld, sta %x, pi civ %d lvi %d, po civ %d lvi %d\n", nirq,
		    ich_status(), pcm_in.civ, pcm_in.lvi, pcm_out.civ, pcm_out.lvi);
	in_isr = true;
	while (isr_on && (ich_status() & 0x7) && guard++ < 4) {
		stack_clear();
		LX_isr_handler(IRQ, isr_dev, NULL);
		run_tasks();
	}
	in_isr = false;
}

/* n silent samples of the line, in the library's delay: its time goes
   on, not the line's samples (line_n, which AGERE_SYNC counts) */
void Agere::delay_samples(unsigned n)
{
	while (n--)
		sample(0);
}

/* one sample each way through the bus masters, the interrupt while
   one asks for it (a level, as on the PCI bus: one that came up in
   idle_sample() is taken here), the timer when due; the sample to the
   line */
short Agere::sample(short in)
{
	short out;

	out = ich_sample(pcm_out, 0);
	ich_sample(pcm_in, in);
	now_us += 125;
	idle_reads = 0;
	if (ich_irq())
		raise_irq();
	if (timer_armed && now_us >= timer_at) {
		timer_armed = 0;
		if (timer_fn) {
			stack_clear();
			timertick_function(0);
		}
	}
	return out;
}

/* a sample of the line */
short Agere::line_sample(short in)
{
	short out = sample(in);
	line_n++;
	return out;
}

/* the library's init and open, the UART as the serial driver sets it */
int Agere::start()
{
	unsigned char res[8] = { 0 };

	stack_clear();
	if (linux_modem_init()) {
		fprintf(stderr, "agere: linux_modem_init failed\n");
		return -1;
	}
	stack_clear();
	if (lucent_detect_modem(res)) {
		fprintf(stderr, "agere: no modem detected\n");
		return -1;
	}
	lucent_init_modem();
	agr_rs_interrupt = rs_interrupt;
	stack_clear();
	linux_modem_open();
	if (!vxdPortOpen())
		LOG("vxdPortOpen: 0\n");
	/* as the serial driver opens it: 115200 bit/s, 8N1, FIFOs */
	stack_clear();
	write_vuart_port(UART_LCR, 0x83);
	write_vuart_port(0, 1);
	write_vuart_port(1, 0);
	write_vuart_port(UART_LCR, 0x03);
	write_vuart_port(2, 0xc7);
	write_vuart_port(UART_MCR, 0x0b);       /* DTR, RTS, OUT2 */
	write_vuart_port(UART_IER, 0x0f);
	return 0;
}

int main()
{
	return agere.run();
}
