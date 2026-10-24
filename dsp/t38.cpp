/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    t38.cpp - ITU-T T.38's IFP packets and their UDPTL transport
 *
 *    IFP packets in the basic aligned PER of Annex A, as the common
 *    implementations have them:
 *
 *        type    bit 7: a data field follows, bit 6: t30-data (else
 *                t30-indicator), bit 5: an extension (2002: 6 bits over
 *                the octet's end), bits 4-1: the indicator or modulation
 *        count   the fields (a length: one octet below 128)
 *        field   bit 7: data follows; 1998: bits 6-4 the field type,
 *                2002: bit 6 an extension, bits 5-3 the type; a field
 *                without data in the 1998 encoding leaves the octet's
 *                low half to the next field; data: its length less one
 *                in two octets, then the octets
 *
 *    UDPTL (Annex A's UDPTLPacket): the sequence number in two octets,
 *    the IFP packet with its length, then the error recovery: 0x00 and
 *    the packets before (secondary-ifp-packets), newest first, or 0x80
 *    and FEC (fec-info).
 *
 */

#include <cstring>
#include <iterator>

#include "reinit.h"
#include "t38.h"

namespace dsp {

/* the 2002 extensions' first values */
#define IND_EXT         T38_IND_V8_ANSAM
#define DATA_EXT        T38_DATA_V8
#define FIELD_EXT       T38_CM_MESSAGE

/* see t38.h */
int t38_ifp_encode(const struct t38_ifp *p, int version, unsigned char *buf,
		   int max)
{
	int at = 0, k, present;

	if (max < 2)
		return -1;
	if (!p->data) {
		if (p->type >= 0 && p->type < IND_EXT) {
			buf[0] = (unsigned char)(p->type << 1);
			return 1;
		}
		if (version && p->type <= T38_IND_V33_14400) {
			int v = p->type - IND_EXT;
			buf[0] = (unsigned char)(0x20 | v >> 2);
			buf[1] = (unsigned char)((v & 3) << 6);
			return 2;
		}
		return -1;
	}
	present = p->nfields > 0 ? 0x80 : 0;
	if (p->type >= 0 && p->type < DATA_EXT) {
		buf[at++] = (unsigned char)(present | 0x40 | p->type << 1);
	} else if (version && p->type <= T38_DATA_V33_14400) {
		int v = p->type - DATA_EXT;
		buf[at++] = (unsigned char)(present | 0x60 | v >> 2);
		buf[at++] = (unsigned char)((v & 3) << 6);
	} else {
		return -1;
	}
	if (!present)
		return at;
	if (p->nfields > T38_MAX_FIELDS)
		return -1;
	buf[at++] = (unsigned char)p->nfields;
	for (k = 0; k < p->nfields; k++) {
		const struct t38_field *f = &p->field[k];
		int fp = f->len > 0 ? 0x80 : 0;
		/* (so the 1998 encoding never shares an octet) */
		if (!fp && k != p->nfields - 1)
			return -1;
		if (at + 2 > max || f->type < 0)
			return -1;
		if (f->type < FIELD_EXT) {
			buf[at++] = (unsigned char)(fp | f->type <<
						    (version ? 3 : 4));
		} else if (version && f->type <= T38_V34RATE) {
			int v = f->type - FIELD_EXT;
			buf[at++] = (unsigned char)(fp | 0x40 | v >> 2);
			buf[at++] = (unsigned char)((v & 3) << 6);
		} else {
			return -1;
		}
		if (fp) {
			if (f->len > 65535 || at + 2 + f->len > max)
				return -1;
			buf[at++] = (unsigned char)((f->len - 1) >> 8);
			buf[at++] = (unsigned char)(f->len - 1);
			memcpy(buf + at, f->data, f->len);
			at += f->len;
		}
	}
	return at;
}

/* see t38.h */
int t38_ifp_decode(struct t38_ifp *p, int version, const unsigned char *buf,
		   int len)
{
	int at, k, count, present, half = 0;

	memset(p, 0, sizeof(*p));
	if (len < 1)
		return -1;
	present = buf[0] & 0x80;
	p->data = (buf[0] >> 6) & 1;
	if (buf[0] & 0x20) {
		/* an extension: six bits over the octet's end */
		if (!version || len < 2)
			return -1;
		p->type = (p->data ? (int)DATA_EXT : (int)IND_EXT) +
			((buf[0] << 2 & 0x3c) | buf[1] >> 6);
		at = 2;
	} else {
		p->type = (buf[0] >> 1) & 0xf;
		at = 1;
	}
	if (!p->data)
		return present || p->type > T38_IND_V33_14400 ? -1 : 0;
	if (p->type > T38_DATA_V33_14400)
		return -1;
	if (!present)
		return 0;
	if (at >= len)
		return -1;
	if (buf[at] & 0x80) {
		if ((buf[at] & 0x40) || at + 2 > len)
			return -1;              /* (fragmented: never this long) */
		count = (buf[at] & 0x3f) << 8 | buf[at + 1];
		at += 2;
	} else {
		count = buf[at++];
	}
	if (count > T38_MAX_FIELDS)
		return -1;
	for (k = 0; k < count; k++) {
		struct t38_field *f = &p->field[k];
		int fp;
		if (at >= len)
			return -1;
		if (!version) {
			if (half) {
				/* the low half of the octet the field before
				   left */
				fp = (buf[at] >> 3) & 1;
				f->type = buf[at] & 7;
				at++;
				half = 0;
			} else {
				fp = buf[at] >> 7;
				f->type = (buf[at] >> 4) & 7;
				if (fp)
					at++;
				else
					half = 1;
			}
		} else {
			fp = buf[at] >> 7;
			if (buf[at] & 0x40) {
				if (at + 2 > len)
					return -1;
				f->type = FIELD_EXT +
					((buf[at] << 2 & 0x3c) | buf[at + 1] >> 6);
				at += 2;
			} else {
				f->type = (buf[at] >> 3) & 7;
				at++;
			}
		}
		if (fp) {
			if (at + 2 > len)
				return -1;
			f->len = (buf[at] << 8 | buf[at + 1]) + 1;
			at += 2;
			if (at + f->len > len)
				return -1;
			f->data = buf + at;
			at += f->len;
		}
	}
	p->nfields = count;
	return 0;
}

/* see t38.h */
void t38_reverse(unsigned char *out, const unsigned char *in, int n)
{
	for (int k = 0; k < n; k++) {
		unsigned v = in[k];
		v = (v & 0xf0) >> 4 | (v & 0x0f) << 4;
		v = (v & 0xcc) >> 2 | (v & 0x33) << 2;
		v = (v & 0xaa) >> 1 | (v & 0x55) << 1;
		out[k] = (unsigned char)v;
	}
}


/*
 *    UDPTL
 */

/* a length below 16384 at buf[at]: the octets it took, 0 if max leaves
   no room */
static int put_len(unsigned char *buf, int at, int max, unsigned v)
{
	if (v < 0x80) {
		if (at + 1 > max)
			return 0;
		buf[at] = (unsigned char)v;
		return 1;
	}
	if (at + 2 > max)
		return 0;
	buf[at] = (unsigned char)(0x80 | v >> 8);
	buf[at + 1] = (unsigned char)v;
	return 2;
}

/* the octets a length takes */
static int len_len(unsigned v)
{
	return v < 0x80 ? 1 : 2;
}

/* a length from buf[*at] (len octets) into *v; -1 if there is none, or
   it is a fragment's (16 K or more) */
static int get_len(const unsigned char *buf, int len, int *at, unsigned *v)
{
	if (*at >= len)
		return -1;
	if (!(buf[*at] & 0x80)) {
		*v = buf[(*at)++];
		return 0;
	}
	if ((buf[*at] & 0x40) || *at + 2 > len)
		return -1;
	*v = (buf[*at] & 0x3f) << 8 | buf[*at + 1];
	*at += 2;
	return 0;
}

/* ec (enum t38_ec) with depth packets before (at most
   UDPTL_HISTORY), sequence number 0 */
void UdptlTx::init(int ec_, int depth_)
{
	reinit(*this);
	ec = ec_;
	depth = depth_ < 0 ? 0 : depth_ > UDPTL_HISTORY ? UDPTL_HISTORY :
		depth_;
}

/* see t38.h */
int UdptlTx::packet(const unsigned char *ifp, int len, unsigned char *buf,
		    int max)
{
	int at, n, k, room;

	if (len <= 0 || len > UDPTL_IFP_MAX ||
	    2 + len_len(len) + len + 2 > max)
		return -1;
	buf[0] = (unsigned char)(seq >> 8);
	buf[1] = (unsigned char)seq;
	at = 2;
	at += put_len(buf, at, max, len);
	memcpy(buf + at, ifp, len);
	at += len;
	buf[at++] = 0x00;                       /* secondary-ifp-packets */
	/* as many of the ones before as fit, newest first */
	n = ec == T38_EC_REDUNDANCY ? (depth < nhist ? depth : nhist) : 0;
	for (;;) {
		room = len_len(n);
		for (k = 0; k < n; k++) {
			int l = hist_len[(nhist - 1 - k) % UDPTL_HISTORY];
			room += len_len(l) + l;
		}
		if (at + room <= max || n == 0)
			break;
		n--;
	}
	at += put_len(buf, at, max, n);
	for (k = 0; k < n; k++) {
		int h = (nhist - 1 - k) % UDPTL_HISTORY;
		at += put_len(buf, at, max, hist_len[h]);
		memcpy(buf + at, hist[h], hist_len[h]);
		at += hist_len[h];
	}
	/* this one for the next packets */
	memcpy(hist[nhist % UDPTL_HISTORY], ifp, len);
	hist_len[nhist % UDPTL_HISTORY] = len;
	nhist++;
	if (nhist == 2 * UDPTL_HISTORY)
		nhist = UDPTL_HISTORY;          /* (the same slots, no wrap) */
	seq = (seq + 1) & 0xffff;
	return at;
}

/* no packet yet */
void UdptlRx::init()
{
	expected = -1;
}

/* see t38.h */
int UdptlRx::packet(const unsigned char *buf, int len,
		    void (*fn)(void *ctx, const unsigned char *ifp, int len,
			       unsigned seq),
		    void *ctx)
{
	const unsigned char *prim, *sec[UDPTL_HISTORY * 2];
	int sec_len[UDPTL_HISTORY * 2], nsec = 0, at = 2, k, prim_len;
	unsigned seq, n, count, d;

	if (len < 4)
		return -1;
	seq = (unsigned)(buf[0] << 8 | buf[1]);
	if (get_len(buf, len, &at, &n) || n == 0 || at + (int)n > len)
		return -1;
	prim = buf + at;
	prim_len = (int)n;
	at += n;
	if (at >= len)
		return -1;
	if (!(buf[at++] & 0x80)) {
		/* the packets before, newest first */
		if (get_len(buf, len, &at, &count))
			return -1;
		for (k = 0; k < (int)count; k++) {
			if (get_len(buf, len, &at, &n) || at + (int)n > len)
				return -1;
			if (nsec < (int)std::size(sec)) {
				sec[nsec] = buf + at;
				sec_len[nsec++] = n;
			}
			at += n;
		}
	}
	/* (FEC: the primary packet only) */
	if (expected < 0) {
		fn(ctx, prim, prim_len, seq);
		expected = (seq + 1) & 0xffff;
		return 0;
	}
	d = (seq - (unsigned)expected) & 0xffff;
	if (d >= 0x8000)
		return 0;                       /* had it, or too late */
	/* the lost ones from the packets before, oldest first (an empty
	   one stands for none) */
	for (k = (int)d; k >= 1; k--)
		if (k <= nsec && sec_len[k - 1] > 0)
			fn(ctx, sec[k - 1], sec_len[k - 1], (seq - k) & 0xffff);
	fn(ctx, prim, prim_len, seq);
	expected = (seq + 1) & 0xffff;
	return 0;
}

} // namespace dsp
