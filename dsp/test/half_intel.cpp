/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    half_intel.cpp - Intel's 537EP soft modem (537core.lib of the
 *    Intel537EP 2.37.50.1 driver for Fedora Core 1: its controller, AT
 *    commands, V.42, V.42bis, V.44, fax, and its data pumps, Intel's own on
 *    IPP, to V.92) as a test peer (modem_half.hpp): line audio from fd 3
 *    and to fd 4 (16 bit, INTEL_RATE, 8000 by default), AT commands on a
 *    pty whose name is printed ("created intel: /dev/pts/N"); INTEL_SYNC
 *    as the others'.
 *
 *    The core needs nothing of a kernel: only what the driver's open
 *    glue gave it (coredrv.c, afedsp_int.c, clmmain.c, rts.c, task.c,
 *    uart.c, locks.c, BSD licensed), here again: the virtual UART's
 *    queues and lines, the 10 ms real-time service that runs the
 *    controller's background (clm_bg) as a task of its own, the tasker's
 *    yield, the locks (none: one thread), the DMA rings' memory, and the
 *    chip (hw/i537ep.hpp).  Its country tables are the driver's own
 *    (wwh_dflt.c, data).
 *
 *    The driver's contexts, one thread here, the line's samples its
 *    time:
 *      - the chip's interrupt at each period of its rings: AfeIRQ()
 *        (the chip's handler, then afe_irq() asks for the DSP tasklet),
 *        dspdrv_CommRamISR(), then DspTask() (the pumps; the kernel
 *        gave them a clean FPU, fnsave, and so does this);
 *      - the kernel's timer every 10 ms: the controller's timers, and
 *        when its time comes the RTS task, clm_bg(), on a stack of its
 *        own: the tasker's RSATaskYield() sleeps it until the next time
 *        (task.c's once its time slice was over: no time passes in the
 *        task here, so at each yield);
 *      - the precise dialer (pulse dialling, hook flash), a task that
 *        waits with linux_schedule(): resumed every ms.
 *    Delays (kdelay, udelay) are none: the core waits on its own
 *    timers.
 *
 *    IPP picks its pumps' code by the CPU (cpuid: px, m6, a6, w7): here
 *    always a Pentium 4's (SSE2, w7), whatever the host, and its cycle
 *    counter (rdtsc) runs on the line's clock at 1 GHz, as do the
 *    driver's gettimeofday and rdtscll.  The object's exit, index and
 *    pow (its own, and an empty exit) are renamed (CMakeLists.txt).
 *
 *    The core is test/intel/537core.lib as Intel shipped it;
 *    -DDMODEM_INTEL_DIR=<dir> for another copy.  INTEL_DEBUG=1 prints
 *    the chip's DMA and the codec's rate, 2 the core's own messages too.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include "hw/codec_line.hpp"
#include "hw/i537ep.hpp"
#include "modem_half.hpp"

using namespace half;

typedef unsigned long ulong;
typedef unsigned short uword;

/*
 *    the core
 */

extern "C" {
/* the driver's device (afedsp_int.h) */
struct sysaudio_t {
	ulong SoundProfile;
	unsigned bIsAudio;
	ulong dwSysAudioOpenRefCount, dwSysAudioPinCount;
	unsigned bIsRenderProceed, bRenderVolumeExist;
	ulong dwSysAudioRenderPinId;
	char RenderCurDataBuf[2048];
	char *RenderCurDataBuf1, *RenderCurDataBuf2;
	ulong RenderNumChannels, RenderBitsPerSample, RenderSamplesPerSec,
		RenderSamplesBlockSize;
	unsigned bIsCaptureProceed, bCaptureVolumeExist;
	ulong dwSysAudioCapturePinId;
	char CaptureCurDataBuf[2048];
	char *CaptureCurDataBuf1, *CaptureCurDataBuf2;
	ulong CaptureNumChannels, CaptureBitsPerSample, CaptureSamplesPerSec,
		CaptureSamplesBlockSize;
	unsigned CaptureSoftControl;
	ulong RenderVolumeNodeId;
	char RenderVolumeLevel;
	ulong CaptureVolumeNodeId;
	char CaptureVolumeLevel;
	volatile char *SoundBuffer, *SoundBuffer2;
	ulong wakeUpSound;
};
struct afe_device_t {
	void *pcidev;
	void *modem;
	void *afeInternal;
	unsigned iobase, membase;
	sysaudio_t SysAudio;
	unsigned RdtscRes;
	int afe_type;
};
struct dmachnl_t {
	unsigned *bdbar;
	u32 bdbar_addr, physbuf;
	int dma_size;
};

extern volatile afe_device_t *devAfe;
extern dmachnl_t *michnl, *mochnl;
extern void *pDspResources;
extern void (*CRAM_CallBack)(void);
extern volatile byte dce_tx_enabled;
extern byte acu_loop, acu_enabled, fax_dte_enabled, voice_dte_enabled;
extern byte acu_state, fax_class, fax1_state, AFE_type, mt_v90_configured;
extern byte at_z_command, slG, slQ;
extern byte dte_to_modem_xon_char, dte_to_modem_xoff_char;
extern char mdm_mfg_id[64], mdm_model_name[64];

int afe_init(unsigned *cfg);
void ModemCardStart(void);
int AfeIRQ(void);
void dspdrv_CommRamISR(void);
void dspdrv_clear_dsp_interrupt(void);
void dspdrv_SetCramISRCallBack(void (*cb)(void));
void line_int(void);
void DspTask(void *);
void ModemTask(void *);
void Precdialer_Task(void *);
void linux_sys_isr_timer_ticker(void);
void acu_timer(void);
void init_eeprom_data(void);
void master_init(void);
void ACU_Main(void);
void Init_WWH(int);
int Read_ProductStrings(void);
int Read_EEPROM(void);
int Read_CountryList(void);
int Read_CurrentCountry(void);
void dte_to_clear(void);
void dte_from_clear(void);
void acu_to_clear(void);
void acu_from_clear(void);
void acu_io_enable(void);
uword acu_from_free(void);
uword dte_from_free(void);
byte acu_rx_char(byte);
byte dte_rx_char(byte);
byte fax_rx_char(byte);
byte voice_dte_rx_char(byte);
void acu_tx_int(void);
void dte_tx_int(void);
void acu_external_int(void);
void dte_external_int(void);
void dte_check_rx_resume(const void *);
/* (the glue's, below) */
void clm_bg(void);
}

/*
 *    the modem: the chip, its codec and the line, the driver's contexts
 */

class Intel : public HalfModem, public CodecLine<Intel>, public I537ep<Intel> {
public:
	/* the modem, its environment's prefix INTEL */
	Intel() : HalfModem("intel", "INTEL") {}

	/* a task of its own stack: clm_bg (RTS_Task), the precise
	   dialer */
	struct Task {
		ucontext_t uc;
		char *stack = nullptr;
		bool running = false;   /* started, not returned */
		bool yielded = false;   /* waits to be resumed */
		void (*fn)() = nullptr;
	};
	Task rts_task, prec_task;
	Task *cur_task = nullptr;       /* the one running, if any */
	ucontext_t main_uc;

	bool dsp_pending = false;       /* afe_irq(): the DSP tasklet */
	/* (debug: interrupts, those the chip's handler took, the DSP's
	   runs, the peaks each way, this second) */
	unsigned st_irq = 0, st_ok = 0, st_dsp = 0, st_mt = 0;
	int st_in = 0, st_out = 0;
	bool prec_pending = false;      /* schedule_precdialer() */

	/* the RTS (rts.c): ticks (ms), the task's timeout (ms) and what is
	   left of it */
	ulong rts_ticks = 0, rts_timeout = 10, rts_timer = 10;
	bool rts_enabled = false;
	unsigned long long next_tick = 0;       /* line sample */

	/* the tasker (task.c): the time slice the core asks for (ms; none
	   here, see RSATaskYield), the minimum loops */
	ulong slice = 0;
	uword min_loops = 0, cur_loops = 0;

	void task_start(Task &t, void (*fn)());
	void task_run(Task &t);
	void yield();
	void rts_schedule();

	/* time on the line's clock: us, and the cycle counter at 1 GHz */
	unsigned long long line_us() const
	{
		return 1000000000ull + line_n * 1000000ull / line_rate;
	}

protected:
	int start() override;
	short line_sample(short in) override;
	void dte_poll() override;

private:
	unsigned long long ms_done = 0;

	short codec_sample(short adc);
	void irq();
	void tick();
	void dte_out();
};

static Intel intel;
#define LOG(...) do { if (intel.dbg) intel.log(__VA_ARGS__); } while (0)

/* a call into the pumps as the kernel made it (afedsp_int.c's
   dsp_function): the FPU saved and clean (fnsave), SSE's control at its
   default; then the caller's again */
template <class F> static void fpu_clean(F &&f)
{
	unsigned char env[108] __attribute__((aligned(16)));
	unsigned mxcsr, def = 0x1f80;
	__asm__ __volatile__("fnsave %0" : "=m"(env));
	__asm__ __volatile__("stmxcsr %0" : "=m"(mxcsr));
	__asm__ __volatile__("ldmxcsr %0" : : "m"(def));
	f();
	__asm__ __volatile__("ldmxcsr %0" : : "m"(mxcsr));
	__asm__ __volatile__("frstor %0" : : "m"(env));
}

/*
 *    the tasks
 */

/* a task's start: its function run, then it is done (back to who
   ran it) */
static void task_main(void)
{
	Intel::Task *t = intel.cur_task;
	stack_clear();
	t->fn();
	t->running = false;
	t->yielded = false;
	intel.cur_task = nullptr;
	/* (back to who ran it: uc_link) */
}

/* t started on fn, run until it yields or returns */
void Intel::task_start(Task &t, void (*fn)())
{
	enum { STACK = 256 * 1024 };
	if (!t.stack)
		t.stack = (char *)malloc(STACK);
	getcontext(&t.uc);
	t.uc.uc_stack.ss_sp = t.stack;
	t.uc.uc_stack.ss_size = STACK;
	t.uc.uc_link = &main_uc;
	makecontext(&t.uc, task_main, 0);
	t.fn = fn;
	t.running = true;
	t.yielded = false;
	task_run(t);
}

/* t on until it yields or returns */
void Intel::task_run(Task &t)
{
	t.yielded = false;
	cur_task = &t;
	swapcontext(&main_uc, &t.uc);
	cur_task = nullptr;
}

/* the running task waits (task.c's interruptible_sleep_on): back to the
   line, resumed later */
void Intel::yield()
{
	Task *t = cur_task;
	if (!t)
		return;
	t->yielded = true;
	cur_task = nullptr;
	swapcontext(&t->uc, &main_uc);
}

/* rts.c's RTS_Task_Schedule: clm_bg run, or resumed where it yielded */
static void rts_task_fn() { clm_bg(); }

/* rts.c's RTS_Task_Schedule: clm_bg started, or resumed where it
   yielded, its time again the timeout's (while the RTS is enabled) */
void Intel::rts_schedule()
{
	if (!rts_enabled)
		return;
	if (!rts_task.running) {
		rts_timer = rts_timeout;
		task_start(rts_task, rts_task_fn);
	} else if (rts_task.yielded) {
		rts_timer = rts_timeout;
		task_run(rts_task);
	}
}

/*
 *    the driver's glue
 */

extern "C" {

/* (coredrv.c and afedsp_int.c) */
unsigned *dsp_irq;
static unsigned the_irq = 5;
volatile unsigned char line_io_enabled = 0;
int acu_init_ok = 0;

/* the core's messages: with INTEL_DEBUG=2; the characters printed */
static int debug_printf(const char *fmt, ...)
{
	va_list ap;
	if (intel.dbg < 2)
		return 0;
	fprintf(stderr, "intel: core: ");
	va_start(ap, fmt);
	int n = vfprintf(stderr, fmt, ap);
	va_end(ap);
	return n;
}
int (*Debug_Printf)(const char *, ...) = debug_printf;

/* the kernel debug messages: with INTEL_DEBUG=2 */
void KdPrintf(const char *fmt, ...)
{
	va_list ap;
	if (intel.dbg < 2)
		return;
	fprintf(stderr, "intel: kd: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/* n bytes zeroed (one at least) */
void *linux_kmalloc(unsigned n) { return calloc(1, n ? n : 1); }
/* p freed */
void linux_kfree(void *p) { free(p); }
/* memset */
void *linux_memset(void *s, int c, unsigned long n) { return memset(s, c, n); }
/* memcpy */
void *linux_memcpy(void *s, const void *c, unsigned long n)
{
	return memcpy(s, c, n);
}
/* udelay: none (the core waits on its own timers) */
void linux_udelay(unsigned long) {}
/* kdelay: none */
void kdelay(short) {}
/* kdisable_irq: nothing (one thread) */
void kdisable_irq(unsigned) {}
/* kenable_irq: nothing */
void kenable_irq(unsigned) {}
/* up_exec_reg_sem: nothing (one thread) */
void up_exec_reg_sem(void) {}
/* down_exec_reg_sem: nothing */
void down_exec_reg_sem(void) {}
/* the current process's name */
char *kCurrentComm(void) { static char c[] = "half_intel"; return c; }
/* wake_up_sound: nothing; 0 */
int wake_up_sound(void) { return 0; }
/* release_resources: nothing */
void release_resources(void *, int) {}

/* the cycle counter: the line's time at 1 GHz */
long long linux_rdtscll(void) { return (long long)intel.line_us() * 1000; }
/* the time of day: the line's */
void linux_gettimeofday(struct timeval *tv)
{
	unsigned long long us = intel.line_us();
	tv->tv_sec = (long)(us / 1000000);
	tv->tv_usec = (long)(us % 1000000);
}

/* the precise dialer's wait for its time: the line on */
void linux_schedule(void)
{
	intel.yield();
}

/* the persistent settings (hamregistry): none, each request times out */
void wake_up_interruptible_persistWriteQ(void) {}
/* wake_up_interruptible_persistReadQ: nothing */
void wake_up_interruptible_persistReadQ(void) {}
/* interruptible_sleep_on_persistReadQ: back at once */
void interruptible_sleep_on_persistReadQ(void) {}
/* interruptible_sleep_on_timeout_persistWriteQ: times out at once */
void interruptible_sleep_on_timeout_persistWriteQ(int *, unsigned long) {}
/* interruptible_sleep_on_timeout_persistReadQ: times out at once */
void interruptible_sleep_on_timeout_persistReadQ(int *, unsigned long) {}
/* ham_proc_shutdown_wait: back at once */
void ham_proc_shutdown_wait(void) {}

/* the locks (locks.c): one thread */
#define LOCK(n) void linux_MT_AcquireLock_##n(void) {} \
	void linux_MT_ReleaseLock_##n(void) {}
/* each lock's acquire and release: nothing */
LOCK(LAPMemptied_que) LOCK(LAPMrx_que) LOCK(LAPMtx_que)
LOCK(acu_rx_buffer) LOCK(acu_tx_buffer) LOCK(dce_rx_buffer)
LOCK(dce_tx_buffer) LOCK(dte_rx_buffer) LOCK(dte_tx_buffer)
LOCK(emptied_que) LOCK(received_que)
void linux_MT_ReleaseLock_transmit_que(void) {}
/* the core's resources (locks.c's semaphores, taken with down_trylock):
   *got 1 if it was free; the RTS task can yield holding one */
static bool held[64];
/* resource r taken: *got 1, or 0 if it was held already */
void linux_RSAGetExclusive(int r, byte *got)
{
	*got = 0;
	if (r < 0 || r >= 64 || held[r])
		return;
	held[r] = true;
	*got = 1;
}
/* resource r free again */
void linux_RSAFreeExclusive(int r)
{
	if (r >= 0 && r < 64)
		held[r] = false;
}

/* the DMA rings' memory: the bus address is the pointer */
int AllocateDmaResources(int size)
{
	dmachnl_t *c[2] = { michnl, mochnl };
	for (int i = 0; i < 2; i++) {
		c[i]->dma_size = size;
		c[i]->bdbar = (unsigned *)aligned_alloc(4096,
			((unsigned)size + 4095) & ~4095u);
		memset(c[i]->bdbar, 0, (unsigned)size);
		c[i]->bdbar_addr = (u32)(uintptr_t)c[i]->bdbar;
	}
	LOG("dma rings of %d bytes: in %p, out %p\n", size,
	    (void *)michnl->bdbar, (void *)mochnl->bdbar);
	return 0;
}

/* the chip's handler asks for the DSP tasklet; the controller's for its
   DPC (here at once, as the driver for this chip had it), the dialer */
int afe_irq(void)
{
	intel.dsp_pending = true;
	return 0;
}
/* the controller's DPC, at once: the DCE's transmit on, the comm
   RAM's callback */
void kScheduleDPC(void)
{
	dce_tx_enabled = 1;
	if (CRAM_CallBack)
		CRAM_CallBack();
}
/* the DSP's interrupt: the modem task run */
void InterruptDSP(void *)
{
	intel.st_mt++;
	ModemTask(pDspResources);
}
/* the precise dialer, started at the line's next ms */
void schedule_precdialer(void)
{
	intel.prec_pending = true;
}

/*
 *    the RTS and the tasker (rts.c, task.c)
 */

/* the RTS's ticks (ms) */
ulong RTS_Get_Ticks(void) { return intel.rts_ticks; }
/* the RTS task's timeout t ms */
void RTS_Set_Timeout(ulong t) { intel.rts_timeout = t; }
/* the RTS on: no ticks yet, the task due in 10 ms */
static void RTS_Enable(void)
{
	intel.rts_enabled = true;
	intel.rts_ticks = 0;
	intel.rts_timeout = intel.rts_timer = 10;
}

/* the task's time slice msecs; the one before */
ulong RSATaskSetMinRunTime(byte msecs, byte)
{
	ulong old = intel.slice;
	intel.slice = msecs;
	return old;
}

/* the task sleeps until the RTS's next time (task.c: once its time
   slice is over; no time passes in the task here, the line's waits for
   it, so a wait of its own for a time, fax1_execute_command's, takes a
   slice of none) */
void RSATaskYield(void)
{
	intel.cur_loops++;
	if (intel.cur_loops < intel.min_loops)
		return;
	if (intel.cur_task != &intel.rts_task)
		return;
	intel.yield();
	intel.cur_loops = 0;
}

/*
 *    the virtual UART (uart.c): to the modem (tx, the DTE's), from it
 *    (rx, to the DTE)
 */

enum { L_DSR, L_DTR, L_RTS, L_CTS, L_RLSD, L_RING, L_PC_BREAK, L_MODEM_BREAK };
static struct {
	byte data_bits = 8, stop_bits = 0, parity = 0;
	ulong baud = 115200;
	byte line[8] = {};
} uart;
enum { FIFO = 8192 };
static byte tx_fifo[FIFO], rx_fifo[FIFO];
static unsigned tx_put, tx_get, tx_count, rx_put, rx_get, rx_count;
static bool ext_int_pending;

/* the controller's external interrupt (a line changed), for
   clm_rx_int */
void clm_external_int(void) { ext_int_pending = true; }

/* line l of the UART v (logged when it changes, CTS aside) */
static void line_set(int l, byte v)
{
	if (uart.line[l] != v && intel.dbg && l != L_CTS)
		intel.log("uart: %s %s\n", (const char *[]){ "DSR", "DTR",
			  "RTS", "CTS", "DCD", "RING", "break", "break" }[l],
			  v ? "on" : "off");
	uart.line[l] = v;
}

/* DSR on */
void UART_dsr_on(void) { line_set(L_DSR, 1); }
/* DSR off */
void UART_dsr_off(void) { line_set(L_DSR, 0); }
/* CTS on */
void UART_cts_on(void) { line_set(L_CTS, 1); }
/* CTS off */
void UART_cts_off(void) { line_set(L_CTS, 0); }
/* DCD on */
void UART_rlsd_on(void) { line_set(L_RLSD, 1); }
/* DCD off */
void UART_rlsd_off(void) { line_set(L_RLSD, 0); }
/* RING on */
void UART_ring_on(void) { line_set(L_RING, 1); }
/* RING off */
void UART_ring_off(void) { line_set(L_RING, 0); }
/* the DTE's DTR on */
static void UART_dtr_on(void) { line_set(L_DTR, 1); }
/* the DTE's RTS on (an external interrupt if it was off) */
static void UART_rts_on(void)
{
	if (!uart.line[L_RTS])
		clm_external_int();
	line_set(L_RTS, 1);
}
/* line l's state, 0 if there is no such line */
byte UART_line_status(byte l) { return l < 8 ? uart.line[l] : 0; }
/* the modem's break to the DTE on */
void UART_start_dce_break(void) { uart.line[L_MODEM_BREAK] = 1; }
/* the room in the queue to the DTE */
unsigned UART_dte_to_free(void) { return FIFO - rx_count; }
/* c on the queue to the DTE; 1 if it is full, else 0 */
int UART_dte_to_char(byte c)
{
	if (rx_count >= FIFO)
		return 1;
	rx_fifo[rx_put] = c;
	rx_put = (rx_put + 1) % FIFO;
	rx_count++;
	return 0;
}
/* the modem's break off, a NUL to the DTE for it (room permitting) */
void UART_stop_dce_break(void)
{
	uart.line[L_MODEM_BREAK] = 0;
	if (UART_dte_to_free())
		UART_dte_to_char(0);
}
/* the DTE's rate */
ulong UART_get_baud(void) { return uart.baud; }
/* the DTE's data bits */
unsigned UART_get_data_bits(void) { return uart.data_bits; }
/* the DTE's parity */
byte UART_get_parity(void) { return uart.parity; }
/* the DTE's characters queued to the modem */
unsigned UART_dce_to_num(void) { return tx_count; }
/* the queue to the modem emptied */
void UART_dce_flush(void) { tx_put = tx_get = tx_count = 0; }
/* the queue to the DTE emptied */
void UART_dte_flush(void) { rx_put = rx_get = rx_count = 0; }
/* the UART's timer: nothing */
void UART_timer(uword) {}

/* the next character to the modem, 0 if none */
static byte UART_dce_from_char(void)
{
	byte c = 0;
	if (tx_count) {
		c = tx_fifo[tx_get];
		tx_get = (tx_get + 1) % FIFO;
		tx_count--;
	}
	return c;
}

/* the UART at 115200 8N1, its queues empty, its lines off */
static void UART_init(void)
{
	uart.data_bits = 8;
	uart.stop_bits = 0;
	uart.parity = 0;
	uart.baud = 115200;
	UART_dce_flush();
	UART_dte_flush();
	memset(uart.line, 0, sizeof(uart.line));
}

/*
 *    the controller's background (clmmain.c)
 */

/* the DTE's characters to the controller (its rx "interrupt") */
void clm_rx_int(void)
{
	static bool in;
	if (in)
		return;
	in = true;
	if (acu_loop) {
		dte_check_rx_resume("clm_rx_int");
		while (UART_dce_to_num() > 0) {
			uword room = acu_enabled ? acu_from_free() : dte_from_free();
			if (!room)
				break;
			byte c = UART_dce_from_char();
			if (acu_enabled)
				acu_rx_char(c);
			else if (fax_dte_enabled)
				fax_rx_char(c);
			else if (voice_dte_enabled)
				voice_dte_rx_char(c);
			else
				dte_rx_char(c);
		}
	}
	if (ext_int_pending) {
		ext_int_pending = false;
		if (acu_enabled)
			acu_external_int();
		else
			dte_external_int();
	}
	in = false;
}

/* the controller's output to the DTE (its tx "interrupt"): the
   AT parser's or the data's */
static void clm_tx_int(void)
{
	if (acu_enabled)
		acu_tx_int();
	else
		dte_tx_int();
}

/* the RTS's timeout and the task's slice by what the controller does
   (clmmain.c's UpdateMinRunTime) */
static void update_min_run_time(void)
{
	static byte last_acu = 99, last_fax1 = 99;
	switch (fax_class) {
	case 0:
		if (last_acu == acu_state)
			break;
		last_acu = acu_state;
		if (acu_state == 1) {           /* S_PARSE */
			RTS_Set_Timeout(10);
			RSATaskSetMinRunTime(2, 1);
		} else if (acu_state == 4) {    /* S_ONLINE */
			RTS_Set_Timeout(10);
			RSATaskSetMinRunTime(0, 3);
		} else {
			RTS_Set_Timeout(10);
			RSATaskSetMinRunTime(0, 2);
		}
		break;
	case 1:
		if (last_fax1 == fax1_state)
			break;
		last_fax1 = fax1_state;
		RTS_Set_Timeout(10);
		if (fax1_state == 0)
			RSATaskSetMinRunTime(2, 8);
		else if (fax1_state % 3 == 1)   /* CONFIGURE_ */
			RSATaskSetMinRunTime(10, 11);
		else
			RSATaskSetMinRunTime(0, 9);
		break;
	case 8:
	case 80:
		RTS_Set_Timeout(15);
		RSATaskSetMinRunTime(0, 16);
		break;
	}
}

/* the controller's background (RTS_Task): its init the first time,
   then the AT parser, the DTE's input and output, the task's slice */
void clm_bg(void)
{
	if (!acu_loop) {
		RSATaskSetMinRunTime(2, 0);
		master_init();
		RSATaskSetMinRunTime(0, 2);
	}
	ACU_Main();
	clm_rx_int();
	clm_tx_int();
	update_min_run_time();
}

/* the EEPROM's defaults, the maker and model strings */
static void clm_configure(void)
{
	init_eeprom_data();
	strcpy(mdm_mfg_id, "Intel Corporation");
	strcpy(mdm_model_name, "Intel(R) 537");
}

/* clmmain.c's init: the country (USA) and the EEPROM's settings, the
   RTS, the UART and the queues, DCE flow control, DTR and RTS on */
static void clm_initialize(void)
{
	Init_WWH(0xB5);                 /* USA */
	Read_ProductStrings();
	Read_EEPROM();
	Read_CountryList();
	Read_CurrentCountry();
	RTS_Set_Timeout(10);
	RTS_Enable();
	AFE_type = 0;
	mt_v90_configured = 1;
	acu_loop = 0;
	at_z_command = 0;
	UART_init();
	dte_to_clear();
	dte_from_clear();
	acu_to_clear();
	acu_from_clear();
	dte_to_modem_xon_char = 0x11;
	dte_to_modem_xoff_char = 0x13;
	slG = 1;                        /* DCE flow control */
	slQ = 1;                        /* no XON/XOFF to the DTE */
	acu_io_enable();
	UART_dtr_on();
	UART_rts_on();
	acu_init_ok = 1;
}

/*
 *    IPP: its CPU a Pentium 4's, its cycles on the line's clock
 */

/* IPP's CPU type: 6 (INTEL_CPU for another) */
int ippCoreGetCpuType(void)
{
	static int t = -1;
	if (t < 0) {
		const char *e = intel.env("CPU");
		t = e ? atoi(e) : 6;
	}
	return t;
}
/* the cycle counter: the line's time at 1 GHz */
unsigned long long ipp_get_pentium_counter(void)
{
	return intel.line_us() * 1000;
}

}

/*
 *    the DTE
 */

/* the modem's output to the DTE */
void Intel::dte_out()
{
	byte buf[FIFO];
	int n = 0;

	while (rx_count && n < (int)sizeof(buf)) {
		buf[n] = rx_fifo[rx_get];
		rx_get = (rx_get + 1) % FIFO;
		rx_count--;
		results.scan(buf[n++]);
	}
	if (n > 0)
		pty.write(buf, (size_t)n);
	else
		pty.flush();
}

/* the DTE's input, as much as the UART takes */
void Intel::dte_poll()
{
	byte buf[256];
	int n, i, room;

	dte_out();
	if (dte_waits())
		return;
	room = FIFO - (int)tx_count;
	if (room <= 0)
		return;
	n = (int)::read(pty.fd(), buf, room < (int)sizeof(buf) ? (size_t)room : sizeof(buf));
	if (n > 0 && !results.online())
		LOG("DTE input at line sample %llu\n", line_n);
	for (i = 0; i < n; i++) {
		tx_fifo[tx_put] = buf[i];
		tx_put = (tx_put + 1) % FIFO;
		tx_count++;
	}
}

/*
 *    the chip, the codec and the line
 */

/* the chip's interrupt: its handler, the comm RAM's, the DSP tasklet
   after them */
void Intel::irq()
{
	stack_clear();
	int r = AfeIRQ();
	i537_acked();
	st_irq++;
	if (r == 0) {
		st_ok++;
		stack_clear();
		dspdrv_CommRamISR();
	}
	if (dsp_pending) {
		dsp_pending = false;
		st_dsp++;
		stack_clear();
		fpu_clean([] { DspTask(nullptr); });
	}
	i537_fix();
}

/* the kernel's timer, 10 ms (rts.c's RTS_Timer) */
void Intel::tick()
{
	stack_clear();
	linux_sys_isr_timer_ticker();
	rts_ticks += 10;
	acu_timer();
	rts_timer = rts_timer >= 10 ? rts_timer - 10 : 0;
	if (dbg && rts_ticks % 1000 == 0) {
		log("%lu ms: irq %u (%u), dsp %u, mt %u, peaks in %d out %d, "
		    "hook %d, acu %d, rate %.0f\n", rts_ticks, st_irq, st_ok,
		    st_dsp, st_mt,
		    st_in, st_out, i537_regs()[0x35] & 1, acu_state,
		    i537_rate());
		st_irq = st_ok = st_dsp = st_mt = 0;
		st_in = st_out = 0;
	}
	if (rts_timer == 0)
		rts_schedule();
	i537_fix();
}

/* one codec sample each way, the interrupt at a period's end */
short Intel::codec_sample(short adc)
{
	short dac = i537_sample(adc);
	if (i537_irq())
		irq();
	if (dbg) {
		st_in = abs(adc) > st_in ? abs(adc) : st_in;
		st_out = abs(dac) > st_out ? abs(dac) : st_out;
	}
	return dac;
}

/* one line sample each way; the timer, the dialer by the line's ms */
short Intel::line_sample(short in)
{
	short out = codec_line_sample(in, line_rate / i537_rate(),
				      [this](short adc) { return codec_sample(adc); });
	line_n++;
	if (line_n >= next_tick) {
		next_tick += (unsigned long long)line_rate / 100;
		tick();
	}
	if (ms() != ms_done) {
		ms_done = ms();
		if (prec_pending) {
			prec_pending = false;
			task_start(prec_task, [] { Precdialer_Task(nullptr); });
		} else if (prec_task.running && prec_task.yielded)
			task_run(prec_task);
	}
	return out;
}

/* the driver's start (coredrv.c's open with modem_init): the device and
   the chip (afe_init: its layer and the comm RAM), the controller, the
   UART, the RTS, then the card started */
int Intel::start()
{
	static unsigned char pcidev[4096];
	unsigned ids[2] = { 0x1080, 0x8086 };

	devAfe = (afe_device_t *)calloc(1, sizeof(afe_device_t));
	devAfe->pcidev = pcidev;
	devAfe->afe_type = 0x8086 + 2;
	devAfe->iobase = (unsigned)(uintptr_t)i537_regs();
	i537_fix();
	dsp_irq = &the_irq;
	stack_clear();
	if (afe_init(ids)) {
		log("afe_init failed\n");
		return -1;
	}
	i537_fix();
	clm_configure();
	UART_init();
	dspdrv_clear_dsp_interrupt();
	dspdrv_SetCramISRCallBack(line_int);
	stack_clear();
	clm_initialize();
	stack_clear();
	ModemCardStart();
	i537_fix();
	next_tick = (unsigned long long)line_rate / 100;
	return 0;
}

/* the 537EP as a test peer (HalfModem::run) */
int main()
{
	return intel.run();
}
