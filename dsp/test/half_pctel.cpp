/*
 *    half_pctel.cpp - PCTel's HSP soft modem (pctel-0.9.7-9's Intel AMR
 *    data pumps, dsp.a, and its controller, control.a: i386 objects,
 *    V.90 with V.42, V.42bis and V.44) as a test peer (modem_half.hpp):
 *    line audio from fd 3 and to fd 4 (16 bit, PCTEL_RATE, 8000 by
 *    default), AT commands on a pty whose name is printed ("created
 *    pctel: /dev/pts/N"); PCTEL_SYNC as the others'.
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
 *    line (hw/codec_line.hpp).
 *
 *    The objects are test/pctel/ as PCTel shipped them (renamed .o);
 *    -DDMODEM_PCTEL_DIR=<dir> for another copy.  PCTEL_DEBUG=1 prints
 *    what the HAL is asked, and the pump the controller asks for
 *    (sharedRam + 0x1c, + 0x20) against the one running (curModem).
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hw/codec_line.hpp"
#include "modem_half.hpp"

using namespace half;

/*
 *    the objects
 */

extern "C" {
void dspMain(void);
void modem_main(void);
void PctelInitCtrlVars(int country);
extern u32 GlobalTimer;
extern u32 sharedRam[];                 /* the controller's, to the pumps */
extern u32 curModem, curMode;           /* the pumps' */
}

/*
 *    the modem: its HAL's codec, the virtual UART's DTE side
 */

class Pctel : public HalfModem, public CodecLine<Pctel> {
public:
	Pctel() : HalfModem("pctel", "PCTEL") {}

	/* the codec's rate (SetSampleFreq) and the samples a block
	   (SetAsicBufSize) */
	unsigned codec_rate = 7200;
	unsigned blk = 24;
	short rx_blk[128] = {};         /* the codec's last block in */
	int rx_n = 0;                   /* samples of the next so far */
	enum { DAC_N = 1024 };
	short dac[DAC_N] = {};          /* the codec's samples out */
	unsigned dac_head = 0, dac_tail = 0;

protected:
	int start() override;
	short line_sample(short in) override;
	void dte_poll() override;

private:
	u32 hal_time = 0;               /* ms * 64 */

	double codec_hz() const;
	short codec_sample(short adc);
	void block_done();
	void dte_out();
};

static Pctel pctel;
#define LOG(...) do { if (pctel.dbg) pctel.log(__VA_ARGS__); } while (0)

/*
 *    the HAL
 */

extern "C" {

short afeRxPtr[128] __attribute__((aligned(32)));
short afeTxPtr[128] __attribute__((aligned(32)));
short ASICstatus, ASICerrCnt, lineCurrent;
u32 SiRegValueR[5], SiRegValueW[5], SiNumReg;

/* the pumps' block in (afeRxPtr): the codec's last */
void RdASIC(void)
{
	memcpy(afeRxPtr, pctel.rx_blk, pctel.blk * sizeof(short));
}

/* the pumps' block out (afeTxPtr) into the DAC's queue (what does not
   fit is dropped) */
void WrASIC(void)
{
	unsigned i;
	for (i = 0; i < pctel.blk; i++) {
		if (pctel.dac_head - pctel.dac_tail >= Pctel::DAC_N) {
			LOG("dac overrun\n");
			break;
		}
		pctel.dac[pctel.dac_head++ % Pctel::DAC_N] = afeTxPtr[i];
	}
}

/* the codec's rate by FreqTable index i (8000 Hz for another): the
   HAL's FreqTable */
void SetSampleFreq(int i)
{
	static const unsigned freq_table[] = { 7200, 8000, 8400, 9000, 9600,
					       10287, 12000, 16000 };
	unsigned r = i >= 0 && i < 8 ? freq_table[i] : 8000;
	if (r != pctel.codec_rate)
		LOG("codec rate %u Hz\n", r);
	pctel.codec_rate = r;
}

/* samples a block: n (1 to 128, else 128); a block begun longer than
   that is started again */
void SetAsicBufSize(int n)
{
	n &= 0xff;
	if (n > 128 || n <= 0)
		n = 128;
	if ((unsigned)n != pctel.blk)
		LOG("block %d\n", n);
	pctel.blk = (unsigned)n;
	if (pctel.rx_n > n)
		pctel.rx_n = 0;
}

/* GPIO: bit 0 the hook relay (1 off hook), 1 caller ID, 3 mute */
void WriteOutport(int v)
{
	static u32 gpio;
	if (((u32)v ^ gpio) & 1)
		LOG("%s hook\n", v & 1 ? "off" : "on");
	gpio = (u32)v;
}

/* the ring (GPIO 1 of the codec): never; the tests answer with ATA */
int ReadInport(void)
{
	return 0;
}

/* the codec's gain: noted only */
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
/* the DAA register SiRegValueW[0] asks for (bits 16..23) into
   SiRegValueR[0], bit 31 set */
void ReadSiReg(void)
{
	u32 r = (SiRegValueW[0] >> 16) & 0xff;
	SiRegValueR[0] = (SiRegValueW[0] & 0xffff0000u) | 0x80000000u | si_reg[r];
}

/* SiNumReg DAA registers written: each its register in bits 16..23,
   its value in 0..15 */
void WriteSiReg(void)
{
	u32 i;
	for (i = 0; i < SiNumReg && i < 5; i++) {
		u32 r = (SiRegValueW[i] >> 16) & 0xff;
		si_reg[r] = (word)SiRegValueW[i];
		LOG("si %02x = %04x\n", r, si_reg[r]);
	}
}

/* and the rest of the HAL: nothing to do */
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
struct vuart {
	u32 tx_put, tx_get, rx_put, rx_get;
	byte tx[0x1000];
	byte rx[0x1000];
	byte flags, pad[3];
	byte iir, msr, pad2[2];
} vuart_info __attribute__((aligned(32)));

}

static_assert(sizeof(vuart_info) == 0x2018, "vuart_info");

/* the UART after the driver's open: empty queues, 115200 bit/s 8N1,
   DTR, RTS, OUT2 */
static void vuart_init(void)
{
	memset(&vuart_info, 0, sizeof(vuart_info));
	COM_TxQcount = COM_RxQcount = 0;
	vuart_info.flags |= 1;
	COM_Vlcr = 0x03;                /* 8N1 */
	COM_Vmcr = 0x0b;                /* DTR, RTS, OUT2 */
	COM_Vdll = 1;                   /* 115200 */
}

/* room in the DTE to modem queue */
static int vuart_room(void)
{
	return 0x1000 - 2 - COM_TxQcount;
}

/* a DTE character into that queue */
static void vuart_put(byte c)
{
	vuart_info.tx[vuart_info.tx_put] = c;
	vuart_info.tx_put = (vuart_info.tx_put + 1) & 0xfff;
	COM_TxQcount++;
	vuart_info.flags |= 1;
}

/*
 *    the DTE
 */

/* the modem's output to the DTE */
void Pctel::dte_out()
{
	byte buf[0x1000];
	int n = 0;

	while (COM_RxQcount && n < (int)sizeof(buf)) {
		buf[n] = vuart_info.rx[vuart_info.rx_get];
		vuart_info.rx_get = (vuart_info.rx_get + 1) & 0xfff;
		COM_RxQcount--;
		results.scan(buf[n++]);
	}
	if (n > 0)
		pty.write(buf, (size_t)n);
	else
		pty.flush();
}

/* the DTE's input, as much as the modem takes (the modem's output goes
   with each block) */
void Pctel::dte_poll()
{
	byte buf[256];
	int n, i, room;

	if (dte_waits())
		return;
	room = vuart_room();
	if (room <= 0)
		return;
	n = (int)::read(pty.fd(), buf, room < (int)sizeof(buf) ? (size_t)room : sizeof(buf));
	if (n > 0 && !results.online())
		LOG("DTE input at line sample %llu\n", line_n);
	for (i = 0; i < n; i++)
		vuart_put(buf[i]);
}

/*
 *    the codec and the line
 */

/* the codec's sample rate: FreqTable's 10287 is V.34's 3429 baud times
   3, 72000 / 7 Hz (the codec's clock locked to the line's, as V.90's
   analogue modem must be: at 10287 Hz, 125 ppm off, the PCTel modem's
   V.90 receiver finds no usable constellation and retrains to V.34) */
double Pctel::codec_hz() const
{
	return codec_rate == 10287 ? 72000.0 / 7 : codec_rate;
}

/* the interrupt of a block: the driver's pctel_interrupt */
void Pctel::block_done()
{
	hal_time += blk * 64000u / codec_rate;
	GlobalTimer = hal_time >> 6;
	ASICstatus = 0;
	ASICerrCnt = 0;
	/* (the controller's +MS copies the carrier's name to its stack
	   without the 0 after it and compares the copy with "V21" ...
	   "K56FLEX": without the clear, +MS=V90 set V.90 only when the
	   byte after the copy happened to be 0) */
	stack_clear();
	dspMain();
	stack_clear();
	modem_main();
	dte_out();
	if (dbg) {
		static u32 was[4];
		u32 now[4] = { sharedRam[7], sharedRam[8], curModem, curMode };
		if (memcmp(now, was, sizeof(now))) {
			LOG("%u ms: dsp interface %u,%u, modem %u mode %u\n",
			    GlobalTimer, now[0], now[1], now[2], now[3]);
			memcpy(was, now, sizeof(was));
		}
	}
}

/* one codec sample each way: adc into the block in (a full block's
   interrupt), the DAC's next back (0 if none) */
short Pctel::codec_sample(short adc)
{
	short d = 0;

	if (dac_tail != dac_head)
		d = dac[dac_tail++ % DAC_N];
	rx_blk[rx_n++] = adc;
	if ((unsigned)rx_n >= blk) {
		rx_n = 0;
		block_done();
	}
	return d;
}

/* one line sample each way */
short Pctel::line_sample(short in)
{
	short out = codec_line_sample(in, line_rate / codec_hz(),
				      [this](short adc) { return codec_sample(adc); });
	line_n++;
	return out;
}

/* the HAL, UART and controller at startup, as the driver's: the HAL's
   defaults (HAL_Init), the UART's, the controller's (country 0, the
   USA) */
int Pctel::start()
{
	hal_time = 0;
	vuart_init();
	stack_clear();
	PctelInitCtrlVars(0);
	return 0;
}

int main()
{
	return pctel.run();
}
