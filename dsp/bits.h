/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    bits.h - frames kept as bits, one a byte in bit 0, in time order:
 *             their fields, the CRC-16 of V.34's INFO and MP style
 *             frames, and the HDLC frame check sequence; the scrambler
 *             1 + x^-tap + x^-23
 *
 *    MP style frames (V.34's MP, V.90's Ja and CP, V.92's CPt, CPu, CPus
 *    and SUVu) begin with a sync of 17 ones, then their 16 bit fields
 *    each after a start bit: 17 bit blocks from bit 17 on, a field at
 *    bit 18 + 17 k.  Their CRC covers the fields and follows them, most
 *    significant bit first.
 *
 */

#ifndef DSP_BITS_H
#define DSP_BITS_H

namespace dsp {

/* nbits of val at b[*n] on, least significant first; *n past them */
inline void put_bits(unsigned char *b, int *n, unsigned val, int nbits)
{
	for (int i = 0; i < nbits; i++)
		b[(*n)++] = (val >> i) & 1;
}

/* nbits of val at b[*n] on, most significant first (a CRC); *n past
   them */
inline void put_bits_msb(unsigned char *b, int *n, unsigned val, int nbits)
{
	for (int i = nbits - 1; i >= 0; i--)
		b[(*n)++] = (val >> i) & 1;
}

/* the nbits wide field at bit first, least significant bit first */
inline int field(const unsigned char *b, int first, int nbits)
{
	int v = 0;
	for (int i = nbits - 1; i >= 0; i--)
		v = (v << 1) | b[first + i];
	return v;
}

/* 1 if the nbits at b[at] on are val, most significant first */
inline bool bits_msb_are(const unsigned char *b, int at, unsigned val,
			 int nbits)
{
	for (int i = 0; i < nbits; i++)
		if (b[at + i] != ((val >> (nbits - 1 - i)) & 1))
			return false;
	return true;
}

/* CRC-16 (x^16 + x^12 + x^5 + 1, preset to ones) over n bits in time
   order, from register crc (0xFFFF to start); the new register.  As
   V.34's INFO and MP sequences have it (10.1.2.3.2/V.34) */
inline unsigned v34_crc_bits(const unsigned char *bits, int n, unsigned crc)
{
	for (int i = 0; i < n; i++) {
		unsigned top = (crc >> 15) & 1;
		crc = (crc << 1) & 0xFFFF;
		if (top ^ (bits[i] & 1))
			crc ^= 0x1021;
	}
	return crc;
}

/* the CRC of an MP style frame's fields before bit end (where the CRC
   goes): the 16 bits after each start bit */
inline unsigned frame_crc(const unsigned char *b, int end)
{
	unsigned crc = 0xFFFF;
	for (int i = 18; i < end - 1; i += 17)
		crc = v34_crc_bits(b + i, 16, crc);
	return crc;
}

/* the frame's CRC at b[*n] (its fields before); *n past it */
inline void put_frame_crc(unsigned char *b, int *n)
{
	put_bits_msb(b, n, frame_crc(b, *n), 16);
}

/* 1 if the frame's CRC at bit at is right */
inline bool frame_crc_ok(const unsigned char *b, int at)
{
	return bits_msb_are(b, at, frame_crc(b, at), 16);
}

/* the HDLC frame check sequence (ISO 3309, x^16 + x^12 + x^5 + 1 on
   octets least significant bit first, preset to ones) of n octets: sent
   inverted after them, low octet first; over a frame and its FCS it
   is HDLC_FCS_GOOD */
#define HDLC_FCS_GOOD   0xF0B8
/* the FCS of p's n octets (see above) */
inline unsigned hdlc_fcs(const unsigned char *p, int n)
{
	unsigned crc = 0xFFFF;
	for (int i = 0; i < n; i++) {
		crc ^= p[i];
		for (int k = 0; k < 8; k++)
			crc = crc & 1 ? (crc >> 1) ^ 0x8408 : crc >> 1;
	}
	return crc;
}

/* the self-synchronising scrambler 1 + x^-tap + x^-23 (7.2/V.34: tap 18
   is the GPC, 5 the GPA), V.32's, V.17's, V.29's and the PCM modems'
   too, over the register of its last line bits, newest in bit 0: a
   data bit in, its line bit out */
enum { SCR_GPC = 18, SCR_GPA = 5 };
/* a data bit through the scrambler (see above) */
inline int scramble(unsigned *reg, int tap, int bit)
{
	int out = (bit ^ (*reg >> (tap - 1)) ^ (*reg >> 22)) & 1;
	*reg = (*reg << 1) | out;
	return out;
}

/* its descrambler: a line bit in (into the register), the data bit
   out */
inline int descramble(unsigned *reg, int tap, int bit)
{
	int out = (bit ^ (*reg >> (tap - 1)) ^ (*reg >> 22)) & 1;
	*reg = (*reg << 1) | (bit & 1);
	return out;
}

} // namespace dsp

#endif
