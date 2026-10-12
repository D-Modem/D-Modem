/*
 *    half_pctel.c - PCTel's HSP soft modem (pctel-0.9.7-9's Intel AMR
 *    data pumps, dsp.a, and its controller, control.a: i386 objects,
 *    V.90 with V.42, V.42bis and V.44) as a test peer, as half_sm56 does
 *    for Motorola's: line audio from fd 3 and to fd 4 (16 bit,
 *    PCTEL_RATE, 8000 by default), AT commands on a pty whose name is
 *    printed ("created pctel: /dev/pts/N"); PCTEL_SYNC as half_sm56's
 *    SM56_SYNC.
 *
 *    The two objects need nothing of a kernel: only their hardware
 *    layer (the HAL, i8xxhal.s in PCTel's driver, here in C) and the
 *    virtual UART's registers and queues (vuart.s there, the same layout
 *    here).  The driver's interrupt handler ran dspMain() and
 *    modem_main() once per block of the codec's samples; the pumps take
 *    the block with RdASIC (into afeRxPtr) and give theirs with WrASIC
 *    (afeTxPtr), at the codec's rate and block size that they set
 *    (SetSampleFreq, SetAsicBufSize).  GlobalTimer is the HAL's: ms, by
 *    the blocks.  A windowed sinc resampler between the codec and the
 *    line, as half_sm56's.
 *
 *    The objects are test/pctel/ as PCTel shipped them (renamed .o);
 *    -DDMODEM_PCTEL_DIR=<dir> for another copy.  PCTEL_DEBUG=1 prints
 *    what the HAL is asked.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

typedef unsigned char byte;
typedef unsigned short word;
typedef unsigned int u32;

static int dbg;
#define LOG(fmt, ...) do { if (dbg) fprintf(stderr, "pctel: " fmt, ##__VA_ARGS__); } while (0)

/*
 *    the objects
 */

extern void dspMain(void);
extern void modem_main(void);
extern void PctelInitCtrlVars(int country);
extern u32 GlobalTimer;

/*
 *    time: the line's samples
 */

static int line_rate = 8000;
static unsigned long long line_n;       /* line samples so far */

/*
 *    the HAL
 */

/* the codec's rates by SetSampleFreq's index (the HAL's FreqTable) */
static const unsigned freq_table[] = { 7200, 8000, 8400, 9000, 9600,
				       10287, 12000, 16000 };
static unsigned codec_rate = 7200;
static unsigned blk = 24;               /* samples a block */
static u32 hal_time;                    /* ms * 64 */

short afeRxPtr[128] __attribute__((aligned(32)));
short afeTxPtr[128] __attribute__((aligned(32)));
short ASICstatus, ASICerrCnt, lineCurrent;
u32 SiRegValueR[5], SiRegValueW[5], SiNumReg;

static short rx_blk[128];               /* the codec's last block in */
static int rx_n;                        /* samples of the next so far */
#define DAC_N 1024
static short dac[DAC_N];                /* the codec's samples out */
static unsigned dac_head, dac_tail;
static u32 gpio;

void RdASIC(void)
{
	memcpy(afeRxPtr, rx_blk, blk * sizeof(short));
}

void WrASIC(void)
{
	unsigned i;
	for (i = 0; i < blk; i++) {
		if (dac_head - dac_tail >= DAC_N) {
			LOG("dac overrun\n");
			break;
		}
		dac[dac_head++ % DAC_N] = afeTxPtr[i];
	}
}

void SetSampleFreq(int i)
{
	unsigned r = i >= 0 && i < 8 ? freq_table[i] : 8000;
	if (r != codec_rate)
		LOG("codec rate %u Hz\n", r);
	codec_rate = r;
}

void SetAsicBufSize(int n)
{
	n &= 0xff;
	if (n > 128 || n <= 0)
		n = 128;
	if ((unsigned)n != blk)
		LOG("block %d\n", n);
	blk = (unsigned)n;
	if (rx_n > n)
		rx_n = 0;
}

/* GPIO: bit 0 the hook relay (1 off hook), 1 caller ID, 3 mute */
void WriteOutport(int v)
{
	if (((u32)v ^ gpio) & 1)
		LOG("%s hook\n", v & 1 ? "off" : "on");
	gpio = (u32)v;
}

/* the ring (GPIO 1 of the codec): never; the tests answer with ATA */
int ReadInport(void)
{
	return 0;
}

void SetCodecGain(int g)
{
	LOG("codec gain %#x\n", g);
}

/* loop current present: valid (0x80), the DAA's reading in 0..15 */
int CheckLineCurrent(void)
{
	lineCurrent = 0x80 | 0x08;
	return 0;
}

/* the DAA's registers (an Si3038 on the codec): no DAA but ours */
static word si_reg[256];
void ReadSiReg(void)
{
	u32 r = (SiRegValueW[0] >> 16) & 0xff;
	SiRegValueR[0] = (SiRegValueW[0] & 0xffff0000u) | 0x80000000u | si_reg[r];
}

void WriteSiReg(void)
{
	u32 i;
	for (i = 0; i < SiNumReg && i < 5; i++) {
		u32 r = (SiRegValueW[i] >> 16) & 0xff;
		si_reg[r] = (word)SiRegValueW[i];
		LOG("si %02x = %04x\n", r, si_reg[r]);
	}
}

void WriteOutSiReg(void) {}
void SiPatch(void) {}
void RestartPt(void) {}

/*
 *    the virtual UART: as vuart.s, the controller's side of it.  Its
 *    queues: tx the DTE's to the modem, rx the modem's to the DTE
 */

byte COM_Vier, COM_Viir, COM_Vlcr, COM_Vmcr, COM_Vlsr, COM_Vmsr;
word COM_Vscr, COM_Vdll;
word COM_RxQcount, COM_TxQcount;
struct {
	u32 tx_put, tx_get, rx_put, rx_get;
	byte tx[0x1000];
	byte rx[0x1000];
	byte flags, pad[3];
	byte iir, msr, pad2[2];
} vuart_info __attribute__((aligned(32)));
_Static_assert(sizeof(vuart_info) == 0x2018, "vuart_info");

static void vuart_init(void)
{
	memset(&vuart_info, 0, sizeof(vuart_info));
	COM_TxQcount = COM_RxQcount = 0;
	vuart_info.flags |= 1;
	COM_Vlcr = 0x03;                /* 8N1 */
	COM_Vmcr = 0x0b;                /* DTR, RTS, OUT2 */
	COM_Vdll = 1;                   /* 115200 */
}

static int vuart_room(void)
{
	return 0x1000 - 2 - COM_TxQcount;
}

static void vuart_put(byte c)
{
	vuart_info.tx[vuart_info.tx_put] = c;
	vuart_info.tx_put = (vuart_info.tx_put + 1) & 0xfff;
	COM_TxQcount++;
	vuart_info.flags |= 1;
}

/*
 *    the DTE: a pty
 */

static int pty;

/* PCTEL_SYNC=<samples>: the DTE's input only every that many samples of
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

/* the modem's output to the DTE */
static void dte_out(void)
{
	byte buf[0x1000];
	int n = 0;

	while (COM_RxQcount && n < (int)sizeof(buf)) {
		buf[n] = vuart_info.rx[vuart_info.rx_get];
		vuart_info.rx_get = (vuart_info.rx_get + 1) & 0xfff;
		COM_RxQcount--;
		result_scan(buf[n++]);
	}
	if (n > 0 && write(pty, buf, (size_t)n) < 0)
		LOG("pty write: %s\n", strerror(errno));
}

/* the DTE's input, as much as the modem takes; only commands need the
   fixed timing, so data (online) goes in at once */
static void dte_poll(void)
{
	byte buf[256];
	int n, i, room;

	if (sync_n && line_n % sync_n && !online)
		return;
	room = vuart_room();
	if (room <= 0)
		return;
	n = (int)read(pty, buf, room < (int)sizeof(buf) ? (size_t)room : sizeof(buf));
	for (i = 0; i < n; i++)
		vuart_put(buf[i]);
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
static void rs_push(struct rs *r, float x)
{
	memmove(r->hist, r->hist + 1, (RS_TAPS - 1) * sizeof(float));
	r->hist[RS_TAPS - 1] = x;
}

static struct rs up, down;      /* line to codec, codec to line */

/* the codec's input noise (PCTEL_NOISE, rms; 5 by default): a line is
   never digitally silent */
static float noise_rms = 5;
static float noise(void)
{
	static u32 x = 1;
	x = x * 1664525u + 1013904223u;         /* (the same every run) */
	return noise_rms * 3.4641f * ((float)(x >> 8) / (1 << 24) - 0.5f);
}
static double up_ph;            /* the next codec sample's time after the
				   line's sample before the newest, in line
				   samples */

static short sat(double x)
{
	return (short)(x > 32767 ? 32767 : x < -32768 ? -32768 : lrint(x));
}

/* the interrupt of a block: the driver's pctel_interrupt */
static void block_done(void)
{
	hal_time += blk * 64000u / codec_rate;
	GlobalTimer = hal_time >> 6;
	ASICstatus = 0;
	ASICerrCnt = 0;
	dspMain();
	modem_main();
	dte_out();
}

static void codec_sample(short adc)
{
	short d = 0;

	if (dac_tail != dac_head)
		d = dac[dac_tail++ % DAC_N];
	rs_push(&down, d);
	rx_blk[rx_n++] = adc;
	if ((unsigned)rx_n >= blk) {
		rx_n = 0;
		block_done();
	}
}

/* one line sample each way */
static short line_sample(short in)
{
	double r = (double)line_rate / codec_rate;
	double out;

	/* the codec's samples due within this line sample */
	rs_push(&up, in + noise());
	while (up_ph < 1) {
		codec_sample(sat(rs_at(up.hist, up_ph, r > 1 ? 1 / r : 1)));
		up_ph += r;
	}
	up_ph -= 1;
	/* (the newest codec sample was at up_ph - r) */
	out = rs_at(down.hist, 1 - up_ph / r, r < 1 ? r : 1);
	line_n++;
	return sat(out);
}

/*
 *    main loop
 */

int main(int argc, char **argv)
{
	short in[480], out[480];
	char *name;
	int i, n;

	(void)argc; (void)argv;
	dbg = getenv("PCTEL_DEBUG") ? atoi(getenv("PCTEL_DEBUG")) : 0;
	/* PCTEL_RATE: the line's sample rate (8000) */
	if (getenv("PCTEL_RATE"))
		line_rate = atoi(getenv("PCTEL_RATE"));
	if (getenv("PCTEL_NOISE"))
		noise_rms = (float)atof(getenv("PCTEL_NOISE"));
	sync_n = getenv("PCTEL_SYNC") ? (atol(getenv("PCTEL_SYNC")) + 47) / 48 * 48 : 0;
	setvbuf(stdout, NULL, _IONBF, 0);

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

	/* as the driver's startup: the HAL's defaults (HAL_Init), the
	   UART's, the controller's (country 0, the USA) */
	hal_time = 0;
	vuart_init();
	PctelInitCtrlVars(0);
	printf("created pctel: %s\n", name);

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
