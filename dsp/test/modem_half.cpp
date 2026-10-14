/*
 *
 *    modem_half.cpp - what the test peers' harnesses share (see
 *                     modem_half.hpp)
 *
 */

#include "modem_half.hpp"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

namespace half {

/*
 *    the stack before a call into a closed module
 */

void __attribute__((noinline)) stack_clear()
{
	static int fill = -1;
	char z[8192];

	if (fill < 0)
		fill = getenv("HALF_STACK_FILL") ?
			atoi(getenv("HALF_STACK_FILL")) & 0xff : 0;
	memset(z, fill, sizeof(z));
	__asm__ volatile("" : : "r"(z) : "memory");
}

/*
 *    the pty
 */

Pty::~Pty()
{
	free(q_);
}

int Pty::open()
{
	struct termios t;

	fd_ = posix_openpt(O_RDWR | O_NOCTTY);
	if (fd_ < 0 || grantpt(fd_) || unlockpt(fd_)) {
		perror("pty");
		return -1;
	}
	tcgetattr(fd_, &t);
	cfmakeraw(&t);
	tcsetattr(fd_, TCSANOW, &t);
	fcntl(fd_, F_SETFL, O_NONBLOCK);
	name_ = ptsname(fd_);
	return 0;
}

void Pty::flush()
{
	while (n_) {
		ssize_t w = ::write(fd_, q_ + head_, n_);
		if (w <= 0) {
			if (w < 0 && errno != EAGAIN && errno != EINTR)
				n_ = 0;         /* (no DTE: the pty gone) */
			break;
		}
		head_ += (size_t)w;
		n_ -= (size_t)w;
	}
	if (!n_)
		head_ = 0;
}

void Pty::write(const void *p, size_t n)
{
	if (!n)
		return;
	flush();
	if (!n_) {
		ssize_t w = ::write(fd_, p, n);
		if (w > 0) {
			p = (const byte *)p + w;
			n -= (size_t)w;
		}
		if (!n)
			return;
	}
	if (head_ + n_ + n > size_) {
		memmove(q_, q_ + head_, n_);
		head_ = 0;
		if (n_ + n > size_) {
			size_t size = size_ ? size_ : 65536;
			while (size < n_ + n)
				size *= 2;
			q_ = (byte *)realloc(q_, size);
			if (!q_) {
				perror("pty");
				exit(1);
			}
			size_ = size;
		}
	}
	memcpy(q_ + head_ + n_, p, n);
	n_ += n;
}

void Pty::read(int room, const std::function<int(const void *p, int n)> &put)
{
	int k;

	while (room > 0) {
		if (!in_n_) {
			ssize_t r = ::read(fd_, in_, room < (int)sizeof(in_) ?
					   (size_t)room : sizeof(in_));
			if (r <= 0)
				return;
			in_head_ = 0;
			in_n_ = (int)r;
		}
		k = put(in_ + in_head_, in_n_ < room ? in_n_ : room);
		if (k <= 0)
			return;
		in_head_ += k;
		in_n_ -= k;
		room -= k;
	}
}

/*
 *    the result codes
 */

void Results::scan(byte c)
{
	static const char *const pat[] = { "CONNECT", "NO CARRIER",
					   "\r\nOK\r\n" };
	int i;

	for (i = 0; i < 3; i++) {
		if (c != (byte)pat[i][at_[i]])
			at_[i] = c == (byte)pat[i][0];
		else
			at_[i]++;
		if (!pat[i][at_[i]]) {
			at_[i] = 0;
			online_ = i == 0;
		}
	}
}

void Results::scan(const void *p, size_t n)
{
	for (size_t k = 0; k < n; k++)
		scan(((const byte *)p)[k]);
}

/*
 *    the line and a codec
 */

short CodecLine::sat(double x)
{
	return (short)(x > 32767 ? 32767 : x < -32768 ? -32768 : lrint(x));
}

float CodecLine::noise()
{
	seed_ = seed_ * 1664525u + 1013904223u;         /* (the same every run) */
	return noise_rms * 3.4641f * ((float)(seed_ >> 8) / (1 << 24) - 0.5f);
}

float CodecLine::at(const float *h, double f, double cut)
{
	double y = 0;
	int k;
	for (k = 0; k < TAPS; k++) {
		double t = k - (TAPS / 2 - 1) - f, x = M_PI * t * cut;
		double w = fabs(t) < TAPS / 2 ?
			0.5 + 0.5 * cos(M_PI * t / (TAPS / 2)) : 0;
		y += h[k] * cut * (fabs(x) < 1e-9 ? 1 : sin(x) / x) * w;
	}
	return (float)y;
}

void CodecLine::push(float *h, float x)
{
	memmove(h, h + 1, (TAPS - 1) * sizeof(float));
	h[TAPS - 1] = x;
}

/*
 *    the program
 */

HalfModem::HalfModem(const char *name, const char *prefix) :
	name_(name), prefix_(prefix)
{
}

const char *HalfModem::env(const char *what) const
{
	char v[64];
	snprintf(v, sizeof(v), "%s_%s", prefix_, what);
	return getenv(v);
}

void HalfModem::log(const char *fmt, ...) const
{
	va_list ap;
	fprintf(stderr, "%s: ", name_);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

short HalfModem::line_sample(short in)
{
	(void)in;
	line_n++;
	return 0;
}

void HalfModem::line_block(const short *in, short *out, int n)
{
	for (int i = 0; i < n; i++)
		out[i] = line_sample(in[i]);
}

void HalfModem::after_read()
{
	if (!sync_n)
		dte_poll();
}

int HalfModem::run()
{
	short in[480], out[480];
	int odd = 0;
	const char *s;

	dbg = (s = env("DEBUG")) ? atoi(s) : 0;
	if ((s = env("RATE")))
		line_rate = atoi(s);
	if ((s = env("NOISE")))
		codec.noise_rms = (float)atof(s);
	sync_n = (s = env("SYNC")) ? (atol(s) + 47) / 48 * 48 : 0;
	sync_phase = (s = env("SYNC_PHASE")) ?
		atol(s) / 48 * 48 % (sync_n ? sync_n : 1) : 0;
	setvbuf(stdout, NULL, _IONBF, 0);

	if (pty.open() || start())
		return 1;
	printf("created %s: %s\n", name_, pty.name());

	/* the other side waits for us first */
	memset(out, 0, sizeof(out));
	if (::write(4, out, 48 * 2 * 4) < 0) {
		perror("the line (fd 4)");
		return 1;
	}
	for (;;) {
		int n = (int)::read(3, (char *)in + odd, sizeof(in) - odd);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		n += odd;
		odd = n & 1;
		n /= 2;
		/* in blocks to each 48th sample of the line, the DTE
		   between them */
		for (int i = 0; i < n; ) {
			int k = 48 - (int)(line_n % 48);
			if (k > n - i)
				k = n - i;
			line_block(in + i, out + i, k);
			i += k;
			if (line_n % 48 == 0)
				dte_poll();
		}
		if (::write(4, out, n * 2) != n * 2)
			break;
		if (odd)
			((char *)in)[0] = ((char *)in)[n * 2];
		after_read();
	}
	return 0;
}

/*
 *    the ICH AC'97 modem controller
 */

Ich::Ich(const HalfModem &m, unsigned nambar, unsigned nabmbar,
	 bool secondary, uintptr_t bus, bool resume) :
	glob_sta(secondary ? 0x300 : 0x100), nambar(nambar), nabmbar(nabmbar), m_(m),
	codec_at_(secondary ? 0x80 : 0), bus_(bus), resume_(resume)
{
}

/* buffer descriptor i of b's list: the buffer's bus address, its
   length in samples | IOC (bit 31) */
u32 *Ich::bd(const BusMaster &b, int i) const
{
	return (u32 *)(uintptr_t)(b.bdbar + 8 * (i & 31) + bus_);
}

/* the current descriptor's length into PICB, from its start */
void Ich::load(BusMaster &b)
{
	b.picb = (word)(bd(b, b.civ)[1] & 0xffff);
	b.pos = 0;
}

/* a write of the control register: reset (RR), run (RPBM: from the
   current descriptor; with resume_, on where it was after a pause),
   stop (DCH set) */
void Ich::control(BusMaster &b, byte v)
{
	if (m_.dbg > 1)
		m_.log("%s cr %02x\n", &b == &pi ? "pi" : "po", v);
	if (v & CR_RR) {
		b.civ = b.lvi = 0;
		b.picb = 0;
		b.pos = 0;
		b.sr = SR_DCH;
		b.cr = 0;
		b.fresh = true;
		return;
	}
	if (resume_ && (v & CR_RPBM) && !(b.cr & CR_RPBM) && !b.fresh) {
		b.sr &= ~SR_DCH;
		if (b.picb == 0 && b.civ != b.lvi) {
			b.civ = (b.civ + 1) & 31;
			b.sr &= ~SR_CELV;
			load(b);
		}
		b.cr = v & 0x1f;
		return;
	}
	if ((v & CR_RPBM) && !(b.cr & CR_RPBM)) {
		b.fresh = false;
		if (resume_ && m_.dbg) {
			m_.log("%s runs: lvi %u:", name(b), b.lvi);
			for (int i = 0; i < 32 && b.bdbar; i++)
				if (bd(b, i)[1] & 0xffff)
					fprintf(stderr, " %x", bd(b, i)[1]);
			fprintf(stderr, "\n");
		}
		b.sr &= ~(SR_DCH | SR_CELV);
		if (b.bdbar)
			load(b);
	}
	if (!(v & CR_RPBM))
		b.sr |= SR_DCH;
	b.cr = v & 0x1f;
}

bool Ich::irq(const BusMaster &b) const
{
	return ((b.sr & SR_BCIS) && (b.cr & CR_IOCE)) ||
		((b.sr & SR_LVBCI) && (b.cr & CR_LVBIE)) ||
		((b.sr & SR_FIFOE) && (b.cr & CR_FEIE));
}

u32 Ich::status() const
{
	return (glob_sta & ~6u) | (irq(pi) ? 2 : 0) | (irq(po) ? 4 : 0);
}

bool Ich::at_start(const BusMaster &b) const
{
	return (b.cr & CR_RPBM) && !(b.sr & SR_DCH) && b.bdbar &&
		b.picb == (bd(b, b.civ)[1] & 0xffff);
}

short Ich::sample(BusMaster &b, short in)
{
	short out = 0;
	u32 *d;
	short *buf;

	if (resume_ && (b.cr & CR_RPBM) && (b.sr & SR_CELV) && b.civ != b.lvi) {
		/* halted at the last valid buffer, and there is another */
		b.civ = (b.civ + 1) & 31;
		b.sr &= ~(SR_CELV | SR_DCH);
		load(b);
	}
	if (!(b.cr & CR_RPBM) || (b.sr & SR_DCH) || !b.bdbar)
		return 0;
	d = bd(b, b.civ);
	buf = (short *)(uintptr_t)(d[0] + bus_);
	if (b.pos < (int)(d[1] & 0xffff) && d[0]) {
		if (&b == &pi)
			buf[b.pos] = in;
		else
			out = buf[b.pos];
	}
	b.pos++;
	if (b.picb)
		b.picb--;
	if (b.picb == 0) {
		if (d[1] & 0x80000000)
			b.sr |= SR_BCIS;
		if (b.civ == b.lvi) {
			if (resume_ && m_.dbg)
				m_.log("%s halted at %llu ms (civ %u)\n", name(b),
				       m_.ms(), b.civ);
			b.sr |= SR_LVBCI | SR_CELV | SR_DCH;
		} else {
			b.civ = (b.civ + 1) & 31;
			load(b);
		}
	}
	return out;
}

bool Ich::io_in(unsigned a, int size, u32 *v)
{
	u32 r = 0xffffffff;

	if (a >= nambar && a < nambar + 0x100) {
		unsigned o = a - nambar;
		if (!codec_at_)
			r = codec_read(o);
		else
			r = o >= codec_at_ ? codec_read(o - codec_at_) : 0xffff;
	} else if (a >= nabmbar && a < nabmbar + 0x80) {
		unsigned o = a - nabmbar;
		BusMaster *b = o < 0x10 ? &pi : o < 0x20 ? &po : nullptr;
		if (b) {
			switch (o & 0xf) {
			case 0x0: r = b->bdbar; break;
			case 0x4: r = b->civ | b->lvi << 8 | (u32)b->sr << 16; break;
			case 0x5: r = b->lvi; break;
			case 0x6: r = b->sr; break;
			case 0x8: r = b->picb; break;
			case 0xa: r = (b->civ + 1) & 31; break;
			case 0xb: r = b->cr; break;
			default:
				if (m_.dbg)
					m_.log("in%d bm %02x\n", size, o);
			}
		} else {
			switch (o) {
			case 0x3c: r = glob_cnt; break;
			case 0x40: r = status(); break;
			case 0x44: r = 0; break;        /* the codec is ours */
			default:
				if (m_.dbg)
					m_.log("in%d nabm %02x\n", size, o);
			}
		}
	} else
		return false;
	*v = size == 1 ? r & 0xff : size == 2 ? r & 0xffff : r;
	return true;
}

bool Ich::io_out(unsigned a, u32 v, int size)
{
	if (a >= nambar && a < nambar + 0x100) {
		unsigned o = a - nambar;
		if (!codec_at_)
			codec_write(o, (word)v);
		else if (o >= codec_at_)
			codec_write(o - codec_at_, (word)v);
		return true;
	}
	if (a < nabmbar || a >= nabmbar + 0x80)
		return false;
	unsigned o = a - nabmbar;
	BusMaster *b = o < 0x10 ? &pi : o < 0x20 ? &po : nullptr;
	if (b) {
		switch (o & 0xf) {
		case 0x0: b->bdbar = v & ~7u; return true;
		case 0x5: b->lvi = v & 31; return true;
		case 0x6: b->sr &= ~(v & (SR_LVBCI | SR_BCIS | SR_FIFOE)); return true;
		case 0xb: control(*b, (byte)v); return true;
		}
		if (m_.dbg)
			m_.log("out%d bm %02x = %x\n", size, o, v);
		return true;
	}
	switch (o) {
	case 0x3c: glob_cnt = v; return true;
	case 0x40: glob_sta &= ~(v & 0xc01); return true;
	case 0x44: return true;
	}
	if (m_.dbg)
		m_.log("out%d nabm %02x = %x\n", size, o, v);
	return true;
}

/*
 *    PCI's configuration space
 */

u32 PciConfig::read(unsigned r, int size) const
{
	u32 v = cfg[(r >> 2) & 63] >> (8 * (r & 3));
	return size == 4 ? v : size == 2 ? v & 0xffff : v & 0xff;
}

void PciConfig::write(unsigned r, u32 v, int size)
{
	u32 mask = size == 4 ? 0xffffffff : size == 2 ? 0xffff : 0xff;
	if (m_.dbg)
		m_.log("pci config %02x = %x\n", r, v);
	if (r >= 0x10 && r < 0x28 && !(r & 3) && bar_size[(r - 0x10) / 4]) {
		/* a BAR: its size in the bits that stay 0 (bit 0 kept: I/O
		   space) */
		u32 size_mask = bar_size[(r - 0x10) / 4] - 1;
		u32 io = cfg[r / 4] & 1;
		v = v == 0xffffffff ? ~size_mask | io : (v & ~size_mask) | io;
	}
	cfg[(r >> 2) & 63] = (cfg[(r >> 2) & 63] & ~(mask << 8 * (r & 3))) |
		(v & mask) << 8 * (r & 3);
}

bool PciConfig::io_in(unsigned a, int size, u32 *v)
{
	u32 r;
	if (a == 0xcf8) {
		r = cf8_;
	} else if (a >= 0xcfc && a < 0xd00) {
		unsigned reg = (cf8_ & 0xfc) + (a - 0xcfc);
		bool on = (cf8_ & 0x80000000) && ((cf8_ >> 16) & 0xff) == 0;
		unsigned df = (cf8_ >> 8) & 0xff;
		if (on && fn0_id && df == (devfn & ~7u)) {
			r = reg < 4 ? fn0_id : (reg & ~3u) == 0x0c ? 0x00800000 : 0;
			r >>= 8 * (reg & 3);
		} else if (on && df == devfn)
			r = read(reg, 4);
		else
			r = 0xffffffff;
	} else
		return false;
	*v = size == 1 ? r & 0xff : size == 2 ? r & 0xffff : r;
	return true;
}

bool PciConfig::io_out(unsigned a, u32 v, int size)
{
	if (a == 0xcf8 && size == 4) {
		cf8_ = v;
		return true;
	}
	if (a < 0xcfc || a >= 0xd00)
		return false;
	if ((cf8_ & 0x80000000) && ((cf8_ >> 16) & 0xff) == 0 &&
	    ((cf8_ >> 8) & 0xff) == devfn)
		write((cf8_ & 0xfc) + (a - 0xcfc), v, size);
	return true;
}

#if defined(__i386__)
/*
 *    the instructions a driver traps on
 */

bool X86Traps::step(ucontext_t *uc)
{
	greg_t *g = uc->uc_mcontext.gregs;
	byte *ip = (byte *)(uintptr_t)g[REG_EIP];
	int size = 4, n = 1;
	unsigned port = (unsigned)g[REG_EDX] & 0xffff;

	if (ip[0] == 0x0f && (ip[1] == 0x20 || ip[1] == 0x22) &&
	    (ip[2] & 0xc0) == 0xc0) {
		/* mov from and to a control register */
		static const int greg[8] = { REG_EAX, REG_ECX, REG_EDX, REG_EBX,
					     REG_ESP, REG_EBP, REG_ESI, REG_EDI };
		int c = (ip[2] >> 3) & 7, r = greg[ip[2] & 7];
		if (ip[1] == 0x20)
			g[r] = (greg_t)cr_[c];
		else
			cr_[c] = (u32)g[r];
		g[REG_EIP] += 3;
		return true;
	}
	if (ip[0] == 0x0f && ip[1] == 0x31 && tsc) {
		unsigned long long t = tsc();
		g[REG_EAX] = (greg_t)(u32)t;
		g[REG_EDX] = (greg_t)(u32)(t >> 32);
		g[REG_EIP] += 2;
		return true;
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
	case 0xe4:      /* in al, imm8; in eax, imm8; out imm8, al/eax */
		g[REG_EAX] = (g[REG_EAX] & ~0xff) | in(ip[1], 1);
		n++;
		break;
	case 0xe5:
		g[REG_EAX] = size == 2 ? (g[REG_EAX] & ~0xffff) | in(ip[1], 2) :
			in(ip[1], 4);
		n++;
		break;
	case 0xe6:
		out(ip[1], g[REG_EAX] & 0xff, 1);
		n++;
		break;
	case 0xe7:
		out(ip[1], size == 2 ? g[REG_EAX] & 0xffff : (u32)g[REG_EAX], size);
		n++;
		break;
	case 0xec:
		g[REG_EAX] = (g[REG_EAX] & ~0xff) | in(port, 1);
		break;
	case 0xed:
		if (size == 2)
			g[REG_EAX] = (g[REG_EAX] & ~0xffff) | in(port, 2);
		else
			g[REG_EAX] = in(port, 4);
		break;
	case 0xee:
		out(port, g[REG_EAX] & 0xff, 1);
		break;
	case 0xef:
		out(port, size == 2 ? g[REG_EAX] & 0xffff : (u32)g[REG_EAX], size);
		break;
	default:
		return false;
	}
	g[REG_EIP] += n;
	return true;
}
#endif

}
