/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
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
 *    and FEC (fec-info): fec-npackets (n) as an unconstrained INTEGER (a
 *    length, then the octets), then the count of FEC messages (m) and
 *    each one with its length.
 *
 *    FEC (Annex C): FEC message I of m (1 to m) in the packet of
 *    sequence number S is the exclusive OR of the IFP packets S - I,
 *    S - I - m, ... S - I - (n - 1) m, each one padded with zeros to the
 *    longest.  It is as long as the longest (as spandsp and Asterisk
 *    have it: C.2.1's "plus 2 octets" says nothing of what they hold).
 *    spandsp and Asterisk put the FEC messages the other way round
 *    (their first one is I = m); with one message (m = 1) the two are
 *    the same, so we send one only.  The receiver finds the order the
 *    far end uses from the FEC messages of packets it has all of.
 *
 */

#include <cstring>
#include <iterator>

#include "dm_types.h"
#include "reinit.h"
#include "t38.h"

namespace dsp {

/* the 2002 extensions' first values */
#define IND_EXT         T38_IND_V8_ANSAM
#define DATA_EXT        T38_DATA_V8
#define FIELD_EXT       T38_CM_MESSAGE

/* see t38.h */
s32 t38_ifp_encode(const struct t38_ifp *p, s16 version, unsigned char *buf,
		   s32 max)
{
	s32 at = 0;
	s16 k, present;

	if (max < 2)
		return -1;
	if (!p->data) {
		if (p->type >= 0 && p->type < IND_EXT) {
			buf[0] = static_cast<unsigned char>(p->type << 1);
			return 1;
		}
		if (version && p->type <= T38_IND_V33_14400) {
			s16 v = p->type - IND_EXT;
			buf[0] = static_cast<unsigned char>(0x20 | v >> 2);
			buf[1] = static_cast<unsigned char>((v & 3) << 6);
			return 2;
		}
		return -1;
	}
	present = p->nfields > 0 ? 0x80 : 0;
	if (p->type >= 0 && p->type < DATA_EXT) {
		buf[at++] = static_cast<unsigned char>(present | 0x40 |
						       p->type << 1);
	} else if (version && p->type <= T38_DATA_V33_14400) {
		s16 v = p->type - DATA_EXT;
		buf[at++] = static_cast<unsigned char>(present | 0x60 | v >> 2);
		buf[at++] = static_cast<unsigned char>((v & 3) << 6);
	} else {
		return -1;
	}
	if (!present)
		return at;
	if (p->nfields > T38_MAX_FIELDS)
		return -1;
	buf[at++] = static_cast<unsigned char>(p->nfields);
	for (k = 0; k < p->nfields; k++) {
		const struct t38_field *f = &p->field[k];
		s16 fp = f->len > 0 ? 0x80 : 0;
		/* (so the 1998 encoding never shares an octet) */
		if (!fp && k != p->nfields - 1)
			return -1;
		if (at + 2 > max || f->type < 0)
			return -1;
		if (f->type < FIELD_EXT) {
			buf[at++] = static_cast<unsigned char>(
				fp | f->type << (version ? 3 : 4));
		} else if (version && f->type <= T38_V34RATE) {
			s16 v = f->type - FIELD_EXT;
			buf[at++] =
				static_cast<unsigned char>(fp | 0x40 | v >> 2);
			buf[at++] = static_cast<unsigned char>((v & 3) << 6);
		} else {
			return -1;
		}
		if (fp) {
			if (f->len > 65535 || at + 2 + f->len > max)
				return -1;
			buf[at++] =
				static_cast<unsigned char>((f->len - 1) >> 8);
			buf[at++] = static_cast<unsigned char>(f->len - 1);
			memcpy(buf + at, f->data, f->len);
			at += f->len;
		}
	}
	return at;
}

/* see t38.h */
s16 t38_ifp_decode(struct t38_ifp *p, s16 version, const unsigned char *buf,
		   s32 len)
{
	s32 at;
	s16 k, count, present, half = 0;

	zero(*p);
	if (len < 1)
		return -1;
	present = buf[0] & 0x80;
	p->data = (buf[0] >> 6) & 1;
	if (buf[0] & 0x20) {
		/* an extension: six bits over the octet's end */
		if (!version || len < 2)
			return -1;
		p->type = (p->data ? static_cast<s16>(DATA_EXT) :
				     static_cast<s16>(IND_EXT)) +
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
		s16 fp;
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
			f->len = (static_cast<s32>(buf[at]) << 8 |
				  buf[at + 1]) + 1;
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
void t38_reverse(unsigned char *out, const unsigned char *in, s32 n)
{
	for (s32 k = 0; k < n; k++) {
		u16 v = in[k];
		v = (v & 0xf0) >> 4 | (v & 0x0f) << 4;
		v = (v & 0xcc) >> 2 | (v & 0x33) << 2;
		v = (v & 0xaa) >> 1 | (v & 0x55) << 1;
		out[k] = static_cast<unsigned char>(v);
	}
}


/*
 *    UDPTL
 */

/* a length below 16384 at buf[at]: the octets it took, 0 if max leaves
   no room */
static s16 put_len(unsigned char *buf, s32 at, s32 max, u16 v)
{
	if (v < 0x80) {
		if (at + 1 > max)
			return 0;
		buf[at] = static_cast<unsigned char>(v);
		return 1;
	}
	if (at + 2 > max)
		return 0;
	buf[at] = static_cast<unsigned char>(0x80 | v >> 8);
	buf[at + 1] = static_cast<unsigned char>(v);
	return 2;
}

/* the octets a length takes */
static s16 len_len(s32 v)
{
	return v < 0x80 ? 1 : 2;
}

/* a length from buf[*at] (len octets) into *v; -1 if there is none, or
   it is a fragment's (16 K or more) */
static s16 get_len(const unsigned char *buf, s32 len, s32 *at,
		   u16 *v)
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

/* see t38.h */
void UdptlTx::init(s16 ec_, s16 depth_, s16 entries_)
{
	reinit(*this);
	ec = ec_;
	depth = depth_ < 0 ? 0 : depth_ > UDPTL_HISTORY ? UDPTL_HISTORY :
		depth_;
	entries = entries_ < 1 ? 1 : entries_;
	if (ec == T38_EC_FEC) {
		if (depth < 1)
			depth = 1;
		if (depth * entries > UDPTL_HISTORY)
			entries = UDPTL_HISTORY / depth;
	}
}

/* the octets FEC (n packets each, m messages) of the packets before in
   tx takes after the primary; *lens the length of each message */
static s32 fec_size(const UdptlTx *tx, s16 n, s16 m, s16 *lens)
{
	s32 size = 1 + 2 + len_len(m);
	for (s16 i = 0; i < m; i++) {
		s16 l = 0;
		for (s16 j = 0; j < n; j++) {
			s16 k = static_cast<s16>(i + 1 + j * m);
			s16 h = (tx->nhist - k) % UDPTL_HISTORY;
			if (tx->hist_len[h] > l)
				l = tx->hist_len[h];
		}
		lens[i] = l;
		size += len_len(l) + l;
	}
	return size;
}

/* see t38.h */
s32 UdptlTx::packet(const unsigned char *ifp, s32 len, unsigned char *buf,
		    s32 max)
{
	s32 at, room;
	s16 n, m, k, j, i;

	if (len <= 0 || len > UDPTL_IFP_MAX ||
	    2 + len_len(len) + len + (ec == T38_EC_FEC ? 4 : 2) > max)
		return -1;
	buf[0] = static_cast<unsigned char>(seq >> 8);
	buf[1] = static_cast<unsigned char>(seq);
	at = 2;
	at += put_len(buf, at, max, len);
	memcpy(buf + at, ifp, len);
	at += len;
	if (ec == T38_EC_FEC) {
		s16 lens[UDPTL_HISTORY];
		unsigned char fec[UDPTL_IFP_MAX];
		/* n packets in each of m messages, fewer while there are not
		   n * m packets before (and fewer messages if they don't
		   fit) */
		n = depth;
		m = entries;
		if (nhist < n * m) {
			m = static_cast<s16>(nhist / n);
			if (m == 0) {
				n = nhist;
				m = nhist ? 1 : 0;
			}
		}
		while (m > 0 && at + fec_size(this, n, m, lens) > max)
			m--;
		if (m == 0)
			n = 0;
		buf[at++] = 0x80;                       /* fec-info */
		buf[at++] = 0x01;                       /* fec-npackets */
		buf[at++] = static_cast<unsigned char>(n);
		at += put_len(buf, at, max, m);
		/* message I = i + 1: the packets I, I + m, ... before */
		for (i = 0; i < m; i++) {
			memset(fec, 0, lens[i]);
			for (j = 0; j < n; j++) {
				k = static_cast<s16>(i + 1 + j * m);
				s16 h = (nhist - k) % UDPTL_HISTORY;
				for (s16 o = 0; o < hist_len[h]; o++)
					fec[o] ^= hist[h][o];
			}
			at += put_len(buf, at, max, lens[i]);
			memcpy(buf + at, fec, lens[i]);
			at += lens[i];
		}
	} else {
		buf[at++] = 0x00;               /* secondary-ifp-packets */
		/* as many of the ones before as fit, newest first */
		n = ec == T38_EC_REDUNDANCY ? (depth < nhist ? depth : nhist) :
			0;
		for (;;) {
			room = len_len(n);
			for (k = 0; k < n; k++) {
				s16 l = hist_len[(nhist - 1 - k) % UDPTL_HISTORY];
				room += len_len(l) + l;
			}
			if (at + room <= max || n == 0)
				break;
			n--;
		}
		at += put_len(buf, at, max, n);
		for (k = 0; k < n; k++) {
			s16 h = (nhist - 1 - k) % UDPTL_HISTORY;
			at += put_len(buf, at, max, hist_len[h]);
			memcpy(buf + at, hist[h], hist_len[h]);
			at += hist_len[h];
		}
	}
	/* this one for the next packets */
	memcpy(hist[nhist % UDPTL_HISTORY], ifp, len);
	hist_len[nhist % UDPTL_HISTORY] = static_cast<s16>(len);
	nhist++;
	if (nhist == 2 * UDPTL_HISTORY)
		nhist = UDPTL_HISTORY;          /* (the same slots, no wrap) */
	seq = (seq + 1) & 0xffff;
	return at;
}

/* see t38.h */
void UdptlRx::init()
{
	expected = -1;
	for (s32 &w : win_seq)
		w = -1;
	fec_order = 0;
	recovered = 0;
}

/* the FEC messages of a packet, as they came */
struct Fec {
	s16 n;                          /* fec-npackets */
	s16 m;                          /* the messages */
	const unsigned char *data[UDPTL_HISTORY];
	s16 len[UDPTL_HISTORY];
};

/* the sequence number of packet j (0 to n - 1) of FEC message i (0 to
   m - 1) of the packet of sequence number seq, in order (1: Annex C,
   2: the other way round) */
static u16 fec_covers(u16 seq, const Fec &f, s16 i, s16 j, s16 order)
{
	s16 first = order == 2 ? f.m - i : i + 1;
	return static_cast<u16>(seq - first - j * f.m);
}

/* rx has the IFP packet of sequence number s */
static bool have(const UdptlRx *rx, u16 s)
{
	return rx->win_seq[s & (UDPTL_RX_WINDOW - 1)] == s;
}

/* FEC message i of f (from the packet of sequence number seq) in order
   is the exclusive OR of the packets it covers, which rx has all of */
static bool fec_matches(const UdptlRx *rx, u16 seq, const Fec &f, s16 i,
			s16 order)
{
	unsigned char x[UDPTL_IFP_MAX] = {};
	s16 longest = 0;

	for (s16 j = 0; j < f.n; j++) {
		u16 s = fec_covers(seq, f, i, j, order);
		s16 w = s & (UDPTL_RX_WINDOW - 1);
		if (!have(rx, s))
			return false;
		for (s16 o = 0; o < rx->win_len[w]; o++)
			x[o] ^= rx->win[w][o];
		if (rx->win_len[w] > longest)
			longest = rx->win_len[w];
	}
	if (f.len[i] < longest)
		return false;
	return !memcmp(x, f.data[i], f.len[i]);
}

/* the packets the FEC f of the packet of sequence number seq makes
   again, those of the gap of gap packets before it that rx does not
   have; their order found first where f can show it */
static void fec_repair(UdptlRx *rx, u16 seq, const Fec &f, u16 gap)
{
	s16 i, j, order = f.m == 1 ? 1 : rx->fec_order;
	bool more = true;

	if (f.n <= 0 || f.m <= 0 || f.n * f.m > UDPTL_RX_WINDOW)
		return;
	if (!order) {
		bool spec = false, rev = false;
		for (i = 0; i < f.m; i++) {
			spec = spec || fec_matches(rx, seq, f, i, 1);
			rev = rev || fec_matches(rx, seq, f, i, 2);
		}
		if (spec != rev)
			rx->fec_order = order = spec ? 1 : 2;
		else
			return;
	}
	/* (one packet made again can let another message make one) */
	while (more) {
		more = false;
		for (i = 0; i < f.m; i++) {
			s16 missing = 0;
			u16 lost = 0;
			for (j = 0; j < f.n; j++) {
				u16 s = fec_covers(seq, f, i, j, order);
				if (!have(rx, s)) {
					missing++;
					lost = s;
				}
			}
			/* one missing, of the gap (not given up before) */
			if (missing != 1 ||
			    static_cast<u16>(seq - lost) > gap ||
			    f.len[i] > UDPTL_IFP_MAX)
				continue;
			s16 w = lost & (UDPTL_RX_WINDOW - 1);
			memcpy(rx->win[w], f.data[i], f.len[i]);
			for (j = 0; j < f.n; j++) {
				u16 s = fec_covers(seq, f, i, j, order);
				s16 v = s & (UDPTL_RX_WINDOW - 1);
				if (s == lost)
					continue;
				for (s16 o = 0; o < rx->win_len[v]; o++)
					rx->win[w][o] ^= rx->win[v][o];
			}
			rx->win_len[w] = f.len[i];
			rx->win_seq[w] = lost;
			rx->recovered++;
			more = true;
		}
	}
}

/* the IFP packet of sequence number s (len octets of ifp) kept in rx;
   ifp NULL: rx has none of s */
static void keep(UdptlRx *rx, u16 s, const unsigned char *ifp, s16 len)
{
	s16 w = s & (UDPTL_RX_WINDOW - 1);
	if (!ifp || len <= 0 || len > UDPTL_IFP_MAX) {
		rx->win_seq[w] = -1;
		return;
	}
	if (rx->win[w] != ifp)
		memcpy(rx->win[w], ifp, len);
	rx->win_len[w] = len;
	rx->win_seq[w] = s;
}

/* see t38.h */
s16 UdptlRx::packet(const unsigned char *buf, s32 len,
		    void (*fn)(void *ctx, const unsigned char *ifp, s16 len,
			       u16 seq),
		    void *ctx)
{
	const unsigned char *prim, *sec[UDPTL_HISTORY * 2];
	s16 sec_len[UDPTL_HISTORY * 2], nsec = 0, k, prim_len;
	s32 at = 2;
	u16 seq, n, count, d;
	bool fec = false;
	Fec f = {};

	if (len < 4)
		return -1;
	seq = static_cast<u16>(buf[0]) << 8 | buf[1];
	if (get_len(buf, len, &at, &n) || n == 0 ||
	    at + static_cast<s32>(n) > len || n > UDPTL_IFP_MAX)
		return -1;
	prim = buf + at;
	prim_len = static_cast<s16>(n);
	at += n;
	if (at >= len)
		return -1;
	if (!(buf[at++] & 0x80)) {
		/* the packets before, newest first */
		if (get_len(buf, len, &at, &count))
			return -1;
		for (k = 0; k < static_cast<s16>(count); k++) {
			if (get_len(buf, len, &at, &n) ||
			    at + static_cast<s32>(n) > len)
				return -1;
			if (nsec < static_cast<s16>(std::size(sec))) {
				sec[nsec] = buf + at;
				sec_len[nsec++] = n;
			}
			at += n;
		}
	} else {
		/* fec-npackets (an INTEGER: its octets, then the value),
		   then the FEC messages */
		u16 octets;
		s32 v = 0;
		if (get_len(buf, len, &at, &octets) || octets < 1 ||
		    octets > 2 || at + octets > len)
			return -1;
		while (octets--)
			v = v << 8 | buf[at++];
		if (get_len(buf, len, &at, &count))
			return -1;
		fec = true;
		f.n = static_cast<s16>(v);
		for (k = 0; k < static_cast<s16>(count); k++) {
			if (get_len(buf, len, &at, &n) ||
			    at + static_cast<s32>(n) > len)
				return -1;
			if (f.m < UDPTL_HISTORY) {
				f.data[f.m] = buf + at;
				f.len[f.m++] = static_cast<s16>(n);
			} else {
				fec = false;    /* (more than we take) */
			}
			at += n;
		}
	}
	if (expected < 0) {
		keep(this, seq, prim, prim_len);
		fn(ctx, prim, prim_len, seq);
		expected = (seq + 1) & 0xffff;
		return 0;
	}
	d = (seq - static_cast<u16>(expected)) & 0xffff;
	if (d >= 0x8000)
		return 0;                       /* had it, or too late */
	keep(this, seq, prim, prim_len);
	if (fec) {
		/* (none of the gap kept yet) */
		for (k = static_cast<s16>(d < UDPTL_RX_WINDOW ? d :
					  UDPTL_RX_WINDOW - 1); k >= 1; k--)
			keep(this, static_cast<u16>(seq - k), nullptr, 0);
		fec_repair(this, seq, f, d);
	}
	/* the lost ones from the packets before or from FEC, oldest first
	   (an empty packet before stands for none); those it doesn't have
	   to fn as lost */
	for (k = static_cast<s16>(d); k >= 1; k--) {
		u16 s = (seq - k) & 0xffff;
		s16 w = s & (UDPTL_RX_WINDOW - 1);
		if (fec && k < UDPTL_RX_WINDOW && have(this, s)) {
			fn(ctx, win[w], win_len[w], s);
		} else if (!fec && k <= nsec && sec_len[k - 1] > 0) {
			keep(this, s, sec[k - 1], sec_len[k - 1]);
			fn(ctx, sec[k - 1], sec_len[k - 1], s);
		} else if (fec || k > nsec) {
			keep(this, s, nullptr, 0);
			fn(ctx, nullptr, 0, s);         /* lost */
		}
	}
	fn(ctx, prim, prim_len, seq);
	expected = (seq + 1) & 0xffff;
	return 0;
}


} // namespace dsp
