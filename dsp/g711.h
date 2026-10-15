/*
 *
 *    g711.h - G.711 mu-law and A-law, the usual reference algorithms
 *             (16 bit linear values; mu-law from 14 bits clipped at
 *             8159, A-law from 13), for libdmodem's G.711 calls, the
 *             V.90/V.92 digital modem and the tests' networks
 *
 *    The reference encoders take a negative value's 14 or 13 bits as
 *    the shift leaves them, rounded down; encode_sm() codes the
 *    magnitude instead, with the sign bit, as V.92's ANSpcm tables
 *    (Tables 7 to 10/V.92) come out and as a codec that is symmetric
 *    about 0 would.
 *
 *    Header only: the tests' programs and libdmodem have it too.
 *
 */
#ifndef DSP_G711_H
#define DSP_G711_H

namespace dsp {
namespace g711 {

/* the segment of v: the first of the 8 ends in t that v does not pass,
   8 beyond the last */
inline int seg_search(int v, const short *t)
{
	for (int i = 0; i < 8; i++)
		if (v <= t[i])
			return i;
	return 8;
}

/* 16 bit linear to mu-law (14 bits, clipped at 8159) */
inline unsigned char lin2ulaw(int x)
{
	static const short seg_end[8] = { 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF,
					  0x7FF, 0xFFF, 0x1FFF };
	int mask = 0xFF, seg;
	x >>= 2;
	if (x < 0) {
		x = -x;
		mask = 0x7F;
	}
	if (x > 8159)
		x = 8159;
	x += 0x84 >> 2;
	seg = seg_search(x, seg_end);
	if (seg >= 8)
		return (unsigned char)(0x7F ^ mask);
	return (unsigned char)(((seg << 4) | ((x >> (seg + 1)) & 15)) ^ mask);
}

/* mu-law to 16 bit linear */
inline int ulaw2lin(unsigned char u)
{
	int t;
	u = (unsigned char)~u;
	t = ((u & 15) << 3) + 0x84;
	t <<= (u & 0x70) >> 4;
	return (u & 0x80) ? 0x84 - t : t - 0x84;
}

/* 16 bit linear to A-law (13 bits, even bits inverted) */
inline unsigned char lin2alaw(int x)
{
	static const short seg_end[8] = { 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF,
					  0x3FF, 0x7FF, 0xFFF };
	int mask = 0xD5, seg, aval;
	x >>= 3;
	if (x < 0) {
		mask = 0x55;
		x = -x - 1;
	}
	seg = seg_search(x, seg_end);
	if (seg >= 8)
		return (unsigned char)(0x7F ^ mask);
	aval = seg << 4;
	aval |= seg < 2 ? (x >> 1) & 15 : (x >> seg) & 15;
	return (unsigned char)(aval ^ mask);
}

/* A-law to 16 bit linear */
inline int alaw2lin(unsigned char a)
{
	int t, seg;
	a ^= 0x55;
	t = (a & 15) << 4;
	seg = (a & 0x70) >> 4;
	if (seg == 0)
		t += 8;
	else if (seg == 1)
		t += 0x108;
	else
		t = (t + 0x108) << (seg - 1);
	return (a & 0x80) ? t : -t;
}

/* an octet of either law (A-law if alaw, else mu-law) to 16 bit
   linear, and 16 bit linear to one */
inline int decode(int alaw, unsigned char c)
{
	return alaw ? alaw2lin(c) : ulaw2lin(c);
}
inline unsigned char encode(int alaw, int x)
{
	return alaw ? lin2alaw(x) : lin2ulaw(x);
}

/* the octet of x's magnitude, its sign bit (the MSB) cleared for a
   negative x */
inline unsigned char encode_sm(int alaw, int x)
{
	return x < 0 ? (unsigned char)(encode(alaw, -x) ^ 0x80) :
		encode(alaw, x);
}

} // namespace g711
} // namespace dsp

#endif
