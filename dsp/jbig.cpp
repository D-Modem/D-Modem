/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    jbig.cpp - ITU-T T.82 (JBIG1) bi-level image coding as T.85
 *               profiles it for fax
 *
 * A T.85 page is one bi-image entity (BIE): a 20 byte header (BIH) and
 * the image in stripes of L0 lines.  Each stripe is coded with the QM
 * arithmetic coder, the pixel's context taken from a template of ten
 * pixels coded before it (three lines, or with LRLTWO two), one of
 * them (the AT pixel) movable.  With TPBON a line the same as the one
 * above is sent as one "typical" pseudo-pixel.  The coded bytes have
 * each 0xff escaped as ff 00 and end with ff 02 (or ff 03, which also
 * resets the probabilities).  Marker segments between stripes move the
 * AT pixel (ATMOVE), shorten the page (NEWLEN), carry comments, or end
 * it (ABORT).
 *
 * T.43 codes an image of more bit-planes in one BIE: each plane is
 * coded as above, with its own probabilities, and their stripes come a
 * plane after another, or a stripe of each plane in turn.
 *
 */

#include <string.h>

#include <algorithm>

#include "jbig.h"

namespace dsp {

namespace {

/* T.82 Table 24: the probability estimation state machine */
struct QmEntry {
	u16 lsz;
	unsigned char nlps, nmps, swtch;
};

const QmEntry qm_table[113] = {
	{0x5a1d,   1,   1, 1}, {0x2586,  14,   2, 0}, {0x1114,  16,   3, 0}, {0x080b,  18,   4, 0},
	{0x03d8,  20,   5, 0}, {0x01da,  23,   6, 0}, {0x00e5,  25,   7, 0}, {0x006f,  28,   8, 0},
	{0x0036,  30,   9, 0}, {0x001a,  33,  10, 0}, {0x000d,  35,  11, 0}, {0x0006,   9,  12, 0},
	{0x0003,  10,  13, 0}, {0x0001,  12,  13, 0}, {0x5a7f,  15,  15, 1}, {0x3f25,  36,  16, 0},
	{0x2cf2,  38,  17, 0}, {0x207c,  39,  18, 0}, {0x17b9,  40,  19, 0}, {0x1182,  42,  20, 0},
	{0x0cef,  43,  21, 0}, {0x09a1,  45,  22, 0}, {0x072f,  46,  23, 0}, {0x055c,  48,  24, 0},
	{0x0406,  49,  25, 0}, {0x0303,  51,  26, 0}, {0x0240,  52,  27, 0}, {0x01b1,  54,  28, 0},
	{0x0144,  56,  29, 0}, {0x00f5,  57,  30, 0}, {0x00b7,  59,  31, 0}, {0x008a,  60,  32, 0},
	{0x0068,  62,  33, 0}, {0x004e,  63,  34, 0}, {0x003b,  32,  35, 0}, {0x002c,  33,   9, 0},
	{0x5ae1,  37,  37, 1}, {0x484c,  64,  38, 0}, {0x3a0d,  65,  39, 0}, {0x2ef1,  67,  40, 0},
	{0x261f,  68,  41, 0}, {0x1f33,  69,  42, 0}, {0x19a8,  70,  43, 0}, {0x1518,  72,  44, 0},
	{0x1177,  73,  45, 0}, {0x0e74,  74,  46, 0}, {0x0bfb,  75,  47, 0}, {0x09f8,  77,  48, 0},
	{0x0861,  78,  49, 0}, {0x0706,  79,  50, 0}, {0x05cd,  48,  51, 0}, {0x04de,  50,  52, 0},
	{0x040f,  50,  53, 0}, {0x0363,  51,  54, 0}, {0x02d4,  52,  55, 0}, {0x025c,  53,  56, 0},
	{0x01f8,  54,  57, 0}, {0x01a4,  55,  58, 0}, {0x0160,  56,  59, 0}, {0x0125,  57,  60, 0},
	{0x00f6,  58,  61, 0}, {0x00cb,  59,  62, 0}, {0x00ab,  61,  63, 0}, {0x008f,  61,  32, 0},
	{0x5b12,  65,  65, 1}, {0x4d04,  80,  66, 0}, {0x412c,  81,  67, 0}, {0x37d8,  82,  68, 0},
	{0x2fe8,  83,  69, 0}, {0x293c,  84,  70, 0}, {0x2379,  86,  71, 0}, {0x1edf,  87,  72, 0},
	{0x1aa9,  87,  73, 0}, {0x174e,  72,  74, 0}, {0x1424,  72,  75, 0}, {0x119c,  74,  76, 0},
	{0x0f6b,  74,  77, 0}, {0x0d51,  75,  78, 0}, {0x0bb6,  77,  79, 0}, {0x0a40,  77,  48, 0},
	{0x5832,  80,  81, 1}, {0x4d1c,  88,  82, 0}, {0x438e,  89,  83, 0}, {0x3bdd,  90,  84, 0},
	{0x34ee,  91,  85, 0}, {0x2eae,  92,  86, 0}, {0x299a,  93,  87, 0}, {0x2516,  86,  71, 0},
	{0x5570,  88,  89, 1}, {0x4ca9,  95,  90, 0}, {0x44d9,  96,  91, 0}, {0x3e22,  97,  92, 0},
	{0x3824,  99,  93, 0}, {0x32b4,  99,  94, 0}, {0x2e17,  93,  86, 0}, {0x56a8,  95,  96, 1},
	{0x4f46, 101,  97, 0}, {0x47e5, 102,  98, 0}, {0x41cf, 103,  99, 0}, {0x3c3d, 104, 100, 0},
	{0x375e,  99,  93, 0}, {0x5231, 105, 102, 0}, {0x4c0f, 106, 103, 0}, {0x4639, 107, 104, 0},
	{0x415e, 103,  99, 0}, {0x5627, 105, 106, 1}, {0x50e7, 108, 107, 0}, {0x4b85, 109, 103, 0},
	{0x5597, 110, 109, 0}, {0x504f, 111, 107, 0}, {0x5a10, 110, 111, 1}, {0x5522, 112, 109, 0},
	{0x59eb, 112, 111, 1},
};

/* marker codes (T.82 Table 2) */
#define J_ESC           0xff
#define J_STUFF         0x00
#define J_SDNORM        0x02
#define J_SDRST         0x03
#define J_ABORT         0x04
#define J_NEWLEN        0x05
#define J_ATMOVE        0x06
#define J_COMMENT       0x07

#define J_CONTEXTS      1024
#define J_BIH           20

/* the contexts of the typical prediction pseudo-pixel (T.82 Figures 11
   and 12) in the bit order of tmpl() */
#define J_TP3           0x0e5
#define J_TP2           0x195

/* room left of a line for the AT pixel's largest lag, and right of it
   for the template */
#define J_LPAD          132
#define J_RPAD          4

/* a big-endian 32 bit number */
u32 get32(const unsigned char *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

/* a big-endian 32 bit number appended */
void put32(std::vector<unsigned char> *out, u32 v)
{
	out->push_back((unsigned char)(v >> 24));
	out->push_back((unsigned char)(v >> 16));
	out->push_back((unsigned char)(v >> 8));
	out->push_back((unsigned char)v);
}

/* three lines (current, one above, two above), one byte a pixel, with
   white borders */
struct Lines {
	std::vector<unsigned char> buf[3];
	unsigned char *r[3];

	/* lines of width pixels, all white */
	void init(u32 width)
	{
		for (int i = 0; i < 3; i++) {
			buf[i].assign(J_LPAD + width + J_RPAD, 0);
			r[i] = buf[i].data() + J_LPAD;
		}
	}

	/* the current line becomes the one above; the new one white */
	void next(u32 width)
	{
		unsigned char *t = r[2];
		r[2] = r[1];
		r[1] = r[0];
		r[0] = t;
		memset(r[0], 0, width);
	}
};

/* pixel x's context; tx: the AT pixel's lag (0: its default place) */
inline u32 tmpl(const Lines &l, u32 x, s16 two, s16 tx)
{
	const unsigned char *r0 = l.r[0] + x, *r1 = l.r[1] + x, *r2 = l.r[2] + x;
	u32 a = tx ? r0[-tx] : r1[2];
	if (two)
		return (u32)r1[-3] << 9 | (u32)r1[-2] << 8 | (u32)r1[-1] << 7 | (u32)r1[0] << 6 |
		       (u32)r1[1] << 5 | a << 4 | (u32)r0[-4] << 3 | (u32)r0[-3] << 2 |
		       (u32)r0[-2] << 1 | r0[-1];
	return (u32)r2[-1] << 9 | (u32)r2[0] << 8 | (u32)r2[1] << 7 | (u32)r1[-2] << 6 |
	       (u32)r1[-1] << 5 | (u32)r1[0] << 4 | (u32)r1[1] << 3 | a << 2 |
	       (u32)r0[-2] << 1 | r0[-1];
}

/* a packed row unpacked into a line */
void unpack(const unsigned char *src, u32 width, unsigned char *line)
{
	for (u32 x = 0; x < width; x++)
		line[x] = (src[x >> 3] >> (7 - (x & 7))) & 1;
}

/* a line packed into a row */
void pack(const unsigned char *line, u32 width, unsigned char *row)
{
	memset(row, 0, (width + 7) / 8);
	for (u32 x = 0; x < width; x++)
		if (line[x])
			row[x >> 3] |= (unsigned char)(0x80 >> (x & 7));
}

} // namespace

/* T.82 INITENC */
void QmEnc::init(std::vector<unsigned char> *out_)
{
	out = out_;
	c = 0;
	a = 0x10000;
	ct = 11;
	sc = 0;
	buffer = -1;
}

/* T.82 ENCODE: CODELPS or CODEMPS, then RENORME with BYTEOUT */
void QmEnc::code(QmState *st, s16 pix)
{
	s16 idx = *st & 0x7f, mps = *st >> 7;
	const QmEntry &e = qm_table[idx];
	a -= e.lsz;
	if (pix != mps) {
		if (a >= e.lsz) {
			c += a;
			a = e.lsz;
		}
		if (e.swtch)
			mps ^= 1;
		idx = e.nlps;
	} else {
		if (a >= 0x8000)
			return;
		if (a < e.lsz) {
			c += a;
			a = e.lsz;
		}
		idx = e.nmps;
	}
	*st = (QmState)(idx | mps << 7);
	do {
		a <<= 1;
		c <<= 1;
		if (--ct == 0) {
			u32 temp = c >> 19;
			if (temp > 0xff) {
				if (buffer >= 0)
					out->push_back((unsigned char)(buffer + 1));
				for (; sc; sc--)
					out->push_back(0x00);
				buffer = (s16)(temp & 0xff);
			} else if (temp == 0xff) {
				sc++;
			} else {
				if (buffer >= 0)
					out->push_back((unsigned char)buffer);
				for (; sc; sc--)
					out->push_back(0xff);
				buffer = (s16)temp;
			}
			c &= 0x7ffff;
			ct = 8;
		}
	} while (a < 0x8000);
}

/* T.82 FLUSH: CLEARBITS, then FINALWRITES (where the figure's carry
   free branch writes BUFFER, not BUFFER + 1) */
void QmEnc::flush()
{
	u32 temp = (a - 1 + c) & 0xffff0000;
	c = temp < c ? temp + 0x8000 : temp;
	c <<= ct;
	if (c > 0x7ffffff) {
		if (buffer >= 0)
			out->push_back((unsigned char)(buffer + 1));
		for (; sc; sc--)
			out->push_back(0x00);
	} else {
		if (buffer >= 0)
			out->push_back((unsigned char)buffer);
		for (; sc; sc--)
			out->push_back(0xff);
	}
	out->push_back((unsigned char)(c >> 19));
	out->push_back((unsigned char)(c >> 11));
}

/* T.82 INITDEC: three bytes into C */
void QmDec::init(const unsigned char *data_, size_t len_)
{
	data = data_;
	len = len_;
	pos = 0;
	c = 0;
	for (int i = 0; i < 3; i++) {
		c <<= 8;
		c += (u32)(pos < len ? data[pos++] : 0) << 8;
	}
	ct = 8;
	a = 0x10000;
}

/* T.82 DECODE, with LPS_EXCHANGE, MPS_EXCHANGE, RENORMD and BYTEIN */
s16 QmDec::decode(QmState *st)
{
	s16 idx = *st & 0x7f, mps = *st >> 7, pix;
	const QmEntry &e = qm_table[idx];
	a -= e.lsz;
	if ((c >> 16) < a) {
		if (a >= 0x8000)
			return mps;
		if (a < e.lsz) {
			pix = !mps;
			if (e.swtch)
				mps ^= 1;
			idx = e.nlps;
		} else {
			pix = mps;
			idx = e.nmps;
		}
	} else {
		c -= a << 16;
		if (a < e.lsz) {
			pix = mps;
			idx = e.nmps;
		} else {
			pix = !mps;
			if (e.swtch)
				mps ^= 1;
			idx = e.nlps;
		}
		a = e.lsz;
	}
	*st = (QmState)(idx | mps << 7);
	do {
		if (ct == 0) {
			c += (u32)(pos < len ? data[pos++] : 0) << 8;
			ct = 8;
		}
		a <<= 1;
		c <<= 1;
		ct--;
	} while (a < 0x8000);
	if (ct == 0) {
		c += (u32)(pos < len ? data[pos++] : 0) << 8;
		ct = 8;
	}
	return pix;
}

/* the order of the SDEs, T.82 Table 11 for one layer: true when the
   stripe is the outer loop (each stripe in every plane, then the next),
   false when the plane is; -1 for the two combinations not allowed */
static s16 stripe_outer(s16 order)
{
	s16 seq = (order & JBG_SEQ) != 0, il = (order & JBG_ILEAVE) != 0,
	    smid = (order & JBG_SMID) != 0;
	if (smid && seq == il)
		return -1;
	return seq ? !smid : il && smid;
}

/* the image's planes in stripes of opt.l0 lines, each stripe of each
   plane its own SCD */
void jbig_encode_image(const JbigImage &img, const JbigOptions &opt, std::vector<unsigned char> *bie)
{
	u32 width = img.width, rows = img.height;
	u32 l0 = opt.l0 ? opt.l0 : 128;
	s16 planes = std::max<s16>(img.planes, 1);
	s16 two = (opt.options & JBG_LRLTWO) != 0, tp = (opt.options & JBG_TPBON) != 0;
	s16 order = opt.order & (JBG_SEQ | JBG_ILEAVE | JBG_SMID);
	if (stripe_outer(order) < 0)
		order = 0;
	size_t stride = img.stride();
	bie->clear();
	bie->push_back(0);                      /* DL */
	bie->push_back(0);                      /* D */
	bie->push_back((unsigned char)planes);  /* P */
	bie->push_back(0);
	put32(bie, width);
	put32(bie, rows);
	put32(bie, l0);
	bie->push_back((unsigned char)opt.mx);
	bie->push_back(0);                      /* MY */
	bie->push_back((unsigned char)order);   /* HITOLO 0 */
	bie->push_back((unsigned char)(opt.options & (JBG_LRLTWO | JBG_TPBON | JBG_VLENGTH)));

	struct Plane {
		std::vector<QmState> st;
		Lines l;
		s16 lntp_prev = 1, tx = 0;
	};
	std::vector<Plane> pl(planes);
	for (Plane &p : pl) {
		p.st.assign(J_CONTEXTS, 0);
		p.l.init(width);
	}
	std::vector<unsigned char> scd;
	QmEnc enc;
	u32 stripes = (rows + l0 - 1) / l0;
	s16 outer = stripe_outer(order);
	for (u32 k = 0; k < stripes * (u32)planes; k++) {
		u32 s = outer ? k / planes : k % stripes;
		s16 i = (s16)(outer ? k % planes : k / stripes);
		Plane &p = pl[i];
		const unsigned char *bits = i < (s16)img.bits.size() ? img.bits[i].data() : nullptr;
		u32 y0 = s * l0, y1 = std::min(rows, y0 + l0);
		for (size_t m = 0; m < opt.n_moves; m++) {
			const JbigAtMove &mv = opt.moves[m];
			if (mv.y < y0 || mv.y >= y1)
				continue;
			bie->push_back(J_ESC);
			bie->push_back(J_ATMOVE);
			put32(bie, mv.y - y0);  /* (from the stripe's top) */
			bie->push_back((unsigned char)mv.tx);
			bie->push_back(0);
		}
		scd.clear();
		enc.init(&scd);
		for (u32 y = y0; y < y1; y++) {
			for (size_t m = 0; m < opt.n_moves; m++)
				if (opt.moves[m].y == y)
					p.tx = opt.moves[m].tx;
			p.l.next(width);
			if (bits)
				unpack(bits + y * stride, width, p.l.r[0]);
			if (tp) {
				s16 lntp = memcmp(p.l.r[0], p.l.r[1], width) != 0;
				enc.code(&p.st[two ? J_TP2 : J_TP3], !(lntp ^ p.lntp_prev));
				p.lntp_prev = lntp;
				if (!lntp)
					continue;
			}
			for (u32 x = 0; x < width; x++)
				enc.code(&p.st[tmpl(p.l, x, two, p.tx)], p.l.r[0][x]);
		}
		enc.flush();
		while (!scd.empty() && scd.back() == 0x00)
			scd.pop_back();
		for (unsigned char b : scd) {
			bie->push_back(b);
			if (b == J_ESC)
				bie->push_back(J_STUFF);
		}
		bie->push_back(J_ESC);
		bie->push_back(J_SDNORM);
	}
}

/* the header checked, then marker segments and stripes in the order
   the header gives */
s16 jbig_decode_image(const unsigned char *data, size_t len, u32 max_rows, s16 max_planes,
		      JbigImage *img, size_t *used)
{
	img->width = img->height = 0;
	img->planes = 0;
	img->bits.clear();
	if (used)
		*used = 0;
	if (len < J_BIH || data[0] != 0 || data[1] != 0 || data[2] == 0 || data[2] > max_planes)
		return -1;
	s16 planes = data[2];
	u32 xd = get32(data + 4), yd = get32(data + 8), l0 = get32(data + 12);
	s16 mx = data[16], options = data[19], outer = stripe_outer(data[18] & 7);
	if (xd == 0 || xd > 8192 || l0 == 0 || data[17] != 0 || (options & JBG_DPON) || outer < 0)
		return -1;
	s16 two = (options & JBG_LRLTWO) != 0, tp = (options & JBG_TPBON) != 0;
	u32 cap = max_rows;
	size_t stride = (xd + 7) / 8;
	img->width = xd;
	img->planes = planes;
	img->bits.resize(planes);

	struct Move {
		u32 y;
		s16 tx;
	};
	struct Plane {
		std::vector<QmState> st;
		Lines l;
		std::vector<Move> moves;
		s16 lntp_prev = 1, tx = 0;
		u32 rows = 0;
	};
	std::vector<Plane> pl(planes);
	for (Plane &p : pl) {
		p.st.assign(J_CONTEXTS, 0);
		p.l.init(xd);
	}
	std::vector<Move> pending;              /* ATMOVEs for the next SDE */
	std::vector<unsigned char> scd;
	QmDec dec;
	size_t pos = J_BIH;

	/* marker segments up to an SDE; 0 at an SDE, 2 when none may follow
	   (the page's lines all there: from NEWLEN, or done), 1 at the data's
	   end or ABORT, -1 when bad; trail: after the last stripe, up to
	   anything else (0) */
	auto markers = [&](u32 y0, bool trail) -> s16 {
		for (;;) {
			if (!trail && y0 >= yd)
				return 2;
			if (pos >= len)
				return 1;
			if (data[pos] != J_ESC || pos + 1 >= len || data[pos + 1] == J_STUFF ||
			    data[pos + 1] == J_SDNORM || data[pos + 1] == J_SDRST)
				return 0;
			switch (data[pos + 1]) {
			case J_ATMOVE:
				if (pos + 8 > len)
					return 1;
				if (data[pos + 7] != 0 || data[pos + 6] > mx || data[pos + 6] > 127 ||
				    (data[pos + 6] && data[pos + 6] < 3))
					return -1;
				/* yAT counts from the top of the stripe that
				   follows */
				pending.push_back({get32(data + pos + 2), data[pos + 6]});
				pos += 8;
				break;
			case J_NEWLEN: {
				if (pos + 6 > len)
					return 1;
				u32 n = get32(data + pos + 2);
				if (n < yd)
					yd = n;
				pos += 6;
				break;
			}
			case J_COMMENT: {
				if (pos + 6 > len)
					return 1;
				u32 n = get32(data + pos + 2);
				if (n > len - pos - 6)
					return 1;
				pos += 6 + n;
				break;
			}
			case J_ABORT:
				return 1;
			default:
				return trail ? 0 : -1;
			}
		}
	};

	/* plane i's stripe s; as markers() */
	auto stripe = [&](s16 i, u32 s) -> s16 {
		u32 y0 = s * l0;
		s16 r = markers(y0, false);
		if (r)
			return r;
		Plane &p = pl[i];
		for (const Move &m : pending)
			p.moves.push_back({y0 + m.y, m.tx});
		pending.clear();

		/* an SDE: unescape up to its end */
		scd.clear();
		s16 end = 0;
		while (pos < len) {
			unsigned char b = data[pos++];
			if (b != J_ESC) {
				scd.push_back(b);
				continue;
			}
			if (pos >= len)
				break;
			b = data[pos++];
			if (b == J_STUFF) {
				scd.push_back(J_ESC);
				continue;
			}
			if (b != J_SDNORM && b != J_SDRST)
				return -1;
			end = b;
			break;
		}

		dec.init(scd.data(), scd.size());
		u32 y1 = std::min(std::min(yd, cap), y0 + l0);
		if (y1 > y0 && y0 == p.rows) {
			img->bits[i].resize((size_t)y1 * stride);
			for (u32 y = y0; y < y1; y++) {
				for (const Move &m : p.moves)
					if (m.y == y)
						p.tx = m.tx;
				p.l.next(xd);
				s16 copy = 0;
				if (tp) {
					s16 lntp = !(dec.decode(&p.st[two ? J_TP2 : J_TP3]) ^ p.lntp_prev);
					p.lntp_prev = lntp;
					copy = !lntp;
				}
				if (copy)
					memcpy(p.l.r[0], p.l.r[1], xd);
				else
					for (u32 x = 0; x < xd; x++)
						p.l.r[0][x] = (unsigned char)dec.decode(&p.st[tmpl(p.l, x, two, p.tx)]);
				pack(p.l.r[0], xd, img->bits[i].data() + (size_t)y * stride);
				p.rows = y + 1;
			}
		}
		if (!end)
			return 1;
		if (end == J_SDRST) {
			std::fill(p.st.begin(), p.st.end(), 0);
			p.tx = 0;
			p.lntp_prev = 1;
		}
		return 0;
	};

	s16 r = 0;
	if (outer) {
		for (u32 s = 0; !r && s * l0 < std::min(yd, cap); s++)
			for (s16 i = 0; !r && i < planes; i++)
				r = stripe(i, s);
	} else {
		/* (the last plane's stripes past the cap not read) */
		for (s16 i = 0; !r && i < planes; i++) {
			for (u32 s = 0; !r && s * l0 < (i == planes - 1 ? std::min(yd, cap) : yd); s++)
				r = stripe(i, s);
			if (r == 2)
				r = 0;
		}
	}
	if (r == 2)
		r = 0;
	if (!r && markers(0, true) == 1 && pos < len)
		r = 1;          /* (ABORT after the last stripe) */
	u32 h = std::min(yd, cap);
	for (const Plane &p : pl)
		h = std::min(h, p.rows);
	img->height = h;
	for (auto &b : img->bits)
		b.resize((size_t)h * stride);
	if (used)
		*used = pos;
	return r;
}

/* the page as one plane */
void jbig_encode(const T4Page *page, const JbigOptions &opt, std::vector<unsigned char> *bie)
{
	JbigImage img;
	img.width = (u32)std::max<s16>(page->width, 0);
	img.height = (u32)std::max<s32>(page->rows, 0);
	img.planes = 1;
	img.bits.emplace_back(page->bits.begin(),
			      page->bits.begin() + (long)img.height * T4_STRIDE(page->width));
	JbigOptions o = opt;
	o.order = 0;
	jbig_encode_image(img, o, bie);
}

/* one plane's image into the page's rows */
s16 jbig_decode(const unsigned char *data, size_t len, s32 max_rows, T4Page *page)
{
	JbigImage img;
	s16 r = jbig_decode_image(data, len, max_rows > 0 ? (u32)max_rows : 0, 1, &img, nullptr);
	if (r < 0)
		return r;
	s16 fine = page->fine;
	page->release();
	page->init((s16)img.width, fine);
	for (u32 y = 0; y < img.height; y++)
		if (!page->add_row(img.bits[0].data() + (size_t)y * img.stride()))
			return 1;
	return r;
}

} // namespace dsp
