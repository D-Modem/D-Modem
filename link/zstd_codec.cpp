/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    zstd_codec.cpp - Zstandard compression (see zstd_codec.h), from RFC 8878
 *
 */

#include <algorithm>
#include <cstring>

#include "dm_types.h"
#include "zstd_codec.h"

namespace dlink {

namespace {

constexpr u32 MAGIC_ZSTD = 0xFD2FB528u;
constexpr s32 MAX_BLOCK = 128 * 1024;
constexpr s16 HUF_MAX = 11;

/* the highest set bit of v (> 0) */
s16 hbit(u32 v)
{
	s16 n = -1;
	while (v) {
		v >>= 1;
		n++;
	}
	return n;
}

/* the literals length codes (Table 15/RFC 8878): baseline, bits */
const u32 LL_BASE[36] = { 0,  1,  2,   3,   4,   5,    6,    7,    8,
			  9,  10, 11,  12,  13,  14,   15,   16,   18,
			  20, 22, 24,  28,  32,  40,   48,   64,   128,
			  256, 512, 1024, 2048, 4096, 8192, 16384, 32768,
			  65536 };
const u8 LL_BITS[36] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3,
			 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
/* the match length codes (Table 16) */
const u32 ML_BASE[53] = {
	3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
	17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
	31, 32, 33, 34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83,
	99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539 };
const u8 ML_BITS[53] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4,
			 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
/* the predefined distributions (3.1.1.3.2.2) */
const s16 LL_DEF[36] = { 4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
			 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2,
			 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1 };
const s16 ML_DEF[53] = { 1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1,
			 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
			 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
			 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1 };
const s16 OF_DEF[29] = { 1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1,
			 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1 };

/* a forward bit stream (FSE table descriptions): the lowest bits
   first; reading past the end sets bad */
struct FwdBits {
	const u8 *p;
	s32 n;
	s64 pos = 0;
	bool bad = false;
	/* the next nb bits, not taken */
	u32 peek(s16 nb) const
	{
		u32 v = 0;
		for (s16 k = 0; k < nb; k++) {
			s64 q = pos + k;
			if (q < 8 * static_cast<s64>(n))
				v |= static_cast<u32>((p[q >> 3] >> (q & 7)) & 1)
				     << k;
		}
		return v;
	}
	/* the next nb bits, taken */
	u32 read(s16 nb)
	{
		u32 v = peek(nb);
		pos += nb;
		if (pos > 8 * static_cast<s64>(n))
			bad = true;
		return v;
	}
	/* the octets taken so far */
	s32 bytes() const { return static_cast<s32>((pos + 7) / 8); }
};

/* a backward bit stream (Huffman and FSE coded data): from the bit
   under the last octet's highest 1, toward the start, each value its
   highest bit first; bits before the start read as 0 (over() then) */
struct BackBits {
	const u8 *p;
	s32 n;
	s64 off = 0;
	/* the start (the last octet's marker); false if none */
	bool init()
	{
		if (n <= 0 || !p[n - 1])
			return false;
		off = 8 * static_cast<s64>(n - 1) + hbit(p[n - 1]);
		return true;
	}
	/* the next nb bits, highest first */
	u64 read(s16 nb)
	{
		u64 v = 0;
		off -= nb;
		for (s16 k = nb - 1; k >= 0; k--) {
			s64 q = off + k;
			v = v << 1 |
			    (q >= 0 ? static_cast<u64>((p[q >> 3] >> (q & 7)) & 1)
				    : 0);
		}
		return v;
	}
	/* read past the start */
	bool over() const { return off < 0; }
};

/* a forward bit writer: the lowest bits first */
struct BitW {
	std::vector<u8> b;
	u64 acc = 0;
	s16 n = 0;
	/* the low nb bits of v */
	void put(u64 v, s16 nb)
	{
		for (s16 k = 0; k < nb; k++) {
			acc |= ((v >> k) & 1) << n;
			if (++n == 8) {
				b.push_back(static_cast<u8>(acc));
				acc = 0;
				n = 0;
			}
		}
	}
	/* the end: a 1, then zeros to the octet */
	void close()
	{
		put(1, 1);
		if (n)
			put(0, 8 - n);
	}
};

/* an FSE decoding table from the counts (-1: "less than 1") at
   accuracy log al (4.1.1) */
bool fse_build(Zstd::Fse *t, const s16 *cnt, s16 nsym, s16 al)
{
	s32 size = 1 << al, high = size - 1;
	t->log = al;
	t->sym.assign(size, 0);
	t->nb.assign(size, 0);
	t->base.assign(size, 0);
	std::vector<u16> next(nsym);
	for (s16 s = 0; s < nsym; s++)
		if (cnt[s] == -1) {
			t->sym[high--] = static_cast<u8>(s);
			next[s] = 1;
		} else
			next[s] = static_cast<u16>(cnt[s] > 0 ? cnt[s] : 0);
	s32 step = (size >> 1) + (size >> 3) + 3, mask = size - 1, pos = 0;
	for (s16 s = 0; s < nsym; s++)
		for (s16 i = 0; i < cnt[s]; i++) {
			t->sym[pos] = static_cast<u8>(s);
			do
				pos = (pos + step) & mask;
			while (pos > high);
		}
	if (pos)
		return false;
	for (s32 i = 0; i < size; i++) {
		u16 d = next[t->sym[i]]++;
		s16 nb = static_cast<s16>(al - hbit(d));
		t->nb[i] = static_cast<u8>(nb);
		t->base[i] = static_cast<u16>((d << nb) - size);
	}
	return true;
}

/* an FSE table description (4.1.1) at p (n octets): the counts, at
   most max_sym symbols, accuracy log at most max_al; octets used, or
   -1 */
s32 fse_desc(const u8 *p, s32 n, s16 max_al, s16 max_sym,
	     std::vector<s16> *cnt, s16 *al)
{
	FwdBits in{ p, n };
	*al = static_cast<s16>(5 + in.read(4));
	if (*al > max_al)
		return -1;
	s32 remaining = 1 << *al;
	s16 bits = static_cast<s16>(*al + 1);
	cnt->clear();
	while (remaining > 0 && static_cast<s16>(cnt->size()) < max_sym) {
		u32 lower = (1u << (bits - 1)) - 1;
		u32 thr = (1u << bits) - 1 - static_cast<u32>(remaining + 1);
		u32 val;
		if ((in.peek(bits - 1) & lower) < thr) {
			val = in.read(bits - 1);
		} else {
			val = in.read(bits);
			if (val > lower)
				val -= thr;
		}
		s16 c = static_cast<s16>(static_cast<s32>(val) - 1);
		remaining -= c < 0 ? -c : c;
		cnt->push_back(c);
		if (c == 0) {
			for (;;) {
				u32 rep = in.read(2);
				for (u32 k = 0; k < rep && static_cast<s16>(cnt->size()) <
						     max_sym; k++)
					cnt->push_back(0);
				if (rep != 3)
					break;
			}
		}
		bits = static_cast<s16>(hbit(static_cast<u32>(remaining + 1)) + 1);
		if (in.bad)
			return -1;
	}
	if (remaining != 0 || in.bad)
		return -1;
	return in.bytes();
}

/* a Huffman decoding table from the weights of symbols 0 to n - 1,
   the last symbol's (n) implied (4.2.1) */
bool huf_build(Zstd::Huf *h, const std::vector<u8> &w)
{
	u32 sum = 0;
	for (u8 x : w) {
		if (x > HUF_MAX + 1)
			return false;
		if (x)
			sum += 1u << (x - 1);
	}
	if (!sum || w.size() > 255)
		return false;
	s16 mb = static_cast<s16>(hbit(sum) + 1);
	u32 left = (1u << mb) - sum;
	if (mb > HUF_MAX || left & (left - 1))
		return false;
	std::vector<u8> bits(w.size() + 1);
	for (size_t s = 0; s < w.size(); s++)
		bits[s] = static_cast<u8>(w[s] ? mb + 1 - w[s] : 0);
	bits[w.size()] = static_cast<u8>(mb + 1 - (hbit(left) + 1));
	u32 rank_cnt[HUF_MAX + 2] = {}, rank_idx[HUF_MAX + 2] = {};
	for (u8 b : bits)
		if (b)
			rank_cnt[b]++;
	rank_idx[mb] = 0;
	for (s16 i = mb; i >= 1; i--)
		rank_idx[i - 1] = rank_idx[i] + rank_cnt[i] * (1u << (mb - i));
	h->max_bits = mb;
	h->sym.assign(1u << mb, 0);
	h->nb.assign(1u << mb, 0);
	for (size_t s = 0; s < bits.size(); s++) {
		if (!bits[s])
			continue;
		u32 len = 1u << (mb - bits[s]), at = rank_idx[bits[s]];
		for (u32 k = 0; k < len; k++) {
			h->sym[at + k] = static_cast<u8>(s);
			h->nb[at + k] = bits[s];
		}
		rank_idx[bits[s]] += len;
	}
	return true;
}

/* a Huffman tree description (4.2.1.1) at p: its octets, or -1 */
s32 huf_desc(const u8 *p, s32 n, Zstd::Huf *h)
{
	if (n < 1)
		return -1;
	std::vector<u8> w;
	s32 used;
	if (p[0] >= 128) {
		s32 nw = p[0] - 127;
		used = 1 + (nw + 1) / 2;
		if (used > n)
			return -1;
		for (s32 i = 0; i < nw; i++)
			w.push_back(static_cast<u8>(
				i & 1 ? p[1 + i / 2] & 15 : p[1 + i / 2] >> 4));
	} else {
		s32 cs = p[0];
		used = 1 + cs;
		if (used > n || !cs)
			return -1;
		std::vector<s16> cnt;
		s16 al;
		s32 d = fse_desc(p + 1, cs, 6, 255, &cnt, &al);
		if (d < 0 || d >= cs)
			return -1;
		Zstd::Fse t;
		if (!fse_build(&t, cnt.data(), static_cast<s16>(cnt.size()), al))
			return -1;
		BackBits in{ p + 1 + d, cs - d };
		if (!in.init())
			return -1;
		u32 s1 = static_cast<u32>(in.read(al)),
		    s2 = static_cast<u32>(in.read(al));
		for (;;) {
			if (w.size() > 254)
				return -1;
			w.push_back(t.sym[s1]);
			s1 = t.base[s1] + static_cast<u32>(in.read(t.nb[s1]));
			if (in.over()) {
				w.push_back(t.sym[s2]);
				break;
			}
			w.push_back(t.sym[s2]);
			s2 = t.base[s2] + static_cast<u32>(in.read(t.nb[s2]));
			if (in.over()) {
				w.push_back(t.sym[s1]);
				break;
			}
		}
	}
	return huf_build(h, w) ? used : -1;
}

/* a Huffman coded stream into out: count symbols; false if bad */
bool huf_stream(const Zstd::Huf &h, const u8 *p, s32 n, s32 count,
		std::vector<u8> *out)
{
	BackBits in{ p, n };
	if (!in.init())
		return false;
	u32 mask = (1u << h.max_bits) - 1;
	u32 st = static_cast<u32>(in.read(h.max_bits));
	for (s32 k = 0; k < count; k++) {
		out->push_back(h.sym[st]);
		s16 nb = h.nb[st];
		st = ((st << nb) & mask) | static_cast<u32>(in.read(nb));
	}
	/* all of it, and no more than the table's last look ahead */
	return in.off + h.max_bits == 0;
}

/* the literals length's or match length's code */
s16 len_code(u32 v, const u32 *base, s16 n)
{
	s16 c = 0;
	while (c + 1 < n && base[c + 1] <= v)
		c++;
	return c;
}

/* an FSE encoder from the counts: each symbol's states in order (by
   the decoding table) */
struct FseEnc {
	s16 al = 0;
	std::vector<s16> cnt;
	std::vector<std::vector<u16>> states;
	/* from the counts c of nsym symbols at accuracy log */
	void build(const s16 *c, s16 nsym, s16 log)
	{
		Zstd::Fse t;
		fse_build(&t, c, nsym, log);
		al = log;
		cnt.assign(c, c + nsym);
		states.assign(nsym, {});
		for (s32 i = 0; i < (1 << log); i++)
			states[t.sym[i]].push_back(static_cast<u16>(i));
	}
	/* the state that ends on symbol s (the encoder's first) */
	u32 start(s16 s) const { return states[s][0]; }
	/* from the decoder's next state st, symbol s before it: its bits
	   out, the state that emits s */
	u32 step(u32 st, s16 s, BitW *w) const
	{
		u32 c = static_cast<u32>(cnt[s] < 0 ? 1 : cnt[s]);
		u32 x = st + (1u << al);
		s16 nb = static_cast<s16>(hbit(x) - hbit(c));
		if ((x >> nb) < c)
			nb--;
		u32 d = x >> nb;
		w->put(x & ((1u << nb) - 1), nb);
		return states[s][d - c];
	}
};

/* the repeat offsets after a sequence's offset value (3.2.2.3), the
   same in the encoder and the decoder: its offset; 0 if it is not one */
u32 apply_offset(u32 *rep, u32 ov, u32 ll)
{
	if (ov > 3) {
		rep[2] = rep[1];
		rep[1] = rep[0];
		rep[0] = ov - 3;
		return rep[0];
	}
	u32 idx = ov - 1 + (ll == 0 ? 1 : 0);
	if (idx == 0)
		return rep[0];
	u32 off = idx == 3 ? rep[0] - 1 : rep[idx];
	if (idx > 1)
		rep[2] = rep[1];
	rep[1] = rep[0];
	rep[0] = off;
	return off;
}

/* Huffman code lengths for the counts, none over HUF_MAX (counts
   halved until so); false with fewer than two symbols */
bool huf_lengths(const u32 *freq, u8 *bits)
{
	std::vector<u32> f(freq, freq + 256);
	for (;;) {
		struct Node {
			u64 w;
			s32 l, r;
		};
		std::vector<Node> nodes;
		std::vector<s32> live;
		for (s32 s = 0; s < 256; s++)
			if (f[s]) {
				nodes.push_back({ f[s], -1, s });
				live.push_back(static_cast<s32>(nodes.size()) - 1);
			}
		if (live.size() < 2)
			return false;
		auto less = [&](s32 a, s32 b) {
			return nodes[a].w != nodes[b].w ? nodes[a].w > nodes[b].w :
							  a > b;
		};
		std::make_heap(live.begin(), live.end(), less);
		while (live.size() > 1) {
			std::pop_heap(live.begin(), live.end(), less);
			s32 a = live.back();
			live.pop_back();
			std::pop_heap(live.begin(), live.end(), less);
			s32 b = live.back();
			live.pop_back();
			nodes.push_back({ nodes[a].w + nodes[b].w, a, b });
			live.push_back(static_cast<s32>(nodes.size()) - 1);
			std::push_heap(live.begin(), live.end(), less);
		}
		/* depths: down from the root */
		std::fill(bits, bits + 256, 0);
		std::vector<std::pair<s32, s16>> stack{ { live[0], 0 } };
		s16 mx = 0;
		while (!stack.empty()) {
			auto [i, d] = stack.back();
			stack.pop_back();
			if (nodes[i].l < 0) {
				bits[nodes[i].r] = static_cast<u8>(d);
				mx = std::max(mx, d);
			} else {
				stack.push_back({ nodes[i].l, static_cast<s16>(d + 1) });
				stack.push_back({ nodes[i].r, static_cast<s16>(d + 1) });
			}
		}
		if (mx <= HUF_MAX)
			return true;
		for (auto &x : f)
			if (x)
				x = (x + 1) / 2;
	}
}

/* the codes of the lengths, as the decoder's table has them */
void huf_codes(const u8 *bits, u16 *code)
{
	s16 mb = 0;
	for (s32 s = 0; s < 256; s++)
		mb = std::max<s16>(mb, bits[s]);
	u32 rank_cnt[HUF_MAX + 2] = {}, rank_idx[HUF_MAX + 2] = {};
	for (s32 s = 0; s < 256; s++)
		if (bits[s])
			rank_cnt[bits[s]]++;
	rank_idx[mb] = 0;
	for (s16 i = mb; i >= 1; i--)
		rank_idx[i - 1] = rank_idx[i] + rank_cnt[i] * (1u << (mb - i));
	for (s32 s = 0; s < 256; s++) {
		code[s] = 0;
		if (!bits[s])
			continue;
		code[s] = static_cast<u16>(rank_idx[bits[s]] >> (mb - bits[s]));
		rank_idx[bits[s]] += 1u << (mb - bits[s]);
	}
}

/* a Huffman coded stream of the literals from..to, the last first */
std::vector<u8> huf_encode(const u8 *lit, s32 from, s32 to, const u8 *bits,
			   const u16 *code)
{
	BitW w;
	for (s32 k = to - 1; k >= from; k--)
		w.put(code[lit[k]], bits[lit[k]]);
	w.close();
	return w.b;
}

/* a literals section header (3.1.1.3.1.1) */
void lit_header(std::vector<u8> *o, s16 type, s32 regen, s32 comp,
		bool one_stream)
{
	if (type < 2) {
		if (regen < 32) {
			o->push_back(static_cast<u8>(type | regen << 3));
		} else if (regen < 4096) {
			o->push_back(static_cast<u8>(type | 1 << 2 | (regen & 15) << 4));
			o->push_back(static_cast<u8>(regen >> 4));
		} else {
			o->push_back(static_cast<u8>(type | 3 << 2 | (regen & 15) << 4));
			o->push_back(static_cast<u8>(regen >> 4));
			o->push_back(static_cast<u8>(regen >> 12));
		}
		return;
	}
	s16 sf, nb, hs;
	if (one_stream) {
		sf = 0, nb = 10, hs = 3;
	} else if (regen < 1024 && comp < 1024) {
		sf = 1, nb = 10, hs = 3;
	} else if (regen < 16384 && comp < 16384) {
		sf = 2, nb = 14, hs = 4;
	} else {
		sf = 3, nb = 18, hs = 5;
	}
	u64 h = static_cast<u64>(type) | static_cast<u64>(sf) << 2 |
		static_cast<u64>(regen) << 4 |
		static_cast<u64>(comp) << (4 + nb);
	for (s16 k = 0; k < hs; k++)
		o->push_back(static_cast<u8>(h >> (8 * k)));
}

/* the literals coded with the lengths bits (one stream up to 1023,
   else four with the jump table); empty if too large */
std::vector<u8> huf_section(const std::vector<u8> &lit, const u8 *bits)
{
	u16 code[256];
	huf_codes(bits, code);
	s32 n = static_cast<s32>(lit.size());
	if (n < 1024) {
		return huf_encode(lit.data(), 0, n, bits, code);
	}
	s32 seg = (n + 3) / 4;
	std::vector<u8> s[4], o(6);
	for (s16 k = 0; k < 4; k++)
		s[k] = huf_encode(lit.data(), std::min(n, k * seg),
				  std::min(n, (k + 1) * seg), bits, code);
	for (s16 k = 0; k < 3; k++) {
		if (s[k].size() > 65535)
			return {};
		o[2 * k] = static_cast<u8>(s[k].size());
		o[2 * k + 1] = static_cast<u8>(s[k].size() >> 8);
	}
	for (auto &x : s)
		o.insert(o.end(), x.begin(), x.end());
	return o;
}

/* counts normalized to 2^al, each symbol counted at least 1 */
std::vector<s16> fse_normalize(const u32 *f, s16 nsym, s16 al)
{
	u64 total = 0;
	for (s16 s = 0; s < nsym; s++)
		total += f[s];
	s32 size = 1 << al, sum = 0, big = 0;
	std::vector<s16> c(nsym, 0);
	for (s16 s = 0; s < nsym; s++) {
		if (!f[s])
			continue;
		u64 v = (static_cast<u64>(f[s]) * size + total / 2) / total;
		c[s] = static_cast<s16>(std::max<u64>(1, v));
		sum += c[s];
		if (c[s] > c[big])
			big = s;
	}
	/* the difference off the largest (or on it), then off any above
	   1 */
	c[big] = static_cast<s16>(c[big] + size - sum);
	while (c[big] < 1) {
		s16 need = static_cast<s16>(1 - c[big]);
		c[big] = 1;
		for (s16 s = 0; s < nsym && need; s++)
			while (s != big && c[s] > 1 && need) {
				c[s]--;
				need--;
			}
	}
	return c;
}

/* an FSE table description (4.1.1) of the counts at accuracy log al */
void fse_desc_write(const std::vector<s16> &c, s16 al, std::vector<u8> *o)
{
	BitW w;
	w.put(static_cast<u64>(al - 5), 4);
	s32 remaining = 1 << al;
	s16 n = static_cast<s16>(c.size());
	for (s16 s = 0; s < n && remaining > 0;) {
		s16 bits = static_cast<s16>(hbit(static_cast<u32>(remaining + 1)) + 1);
		u32 lower = (1u << (bits - 1)) - 1;
		u32 thr = (1u << bits) - 1 - static_cast<u32>(remaining + 1);
		u32 val = static_cast<u32>(c[s] + 1);
		if (val < thr)
			w.put(val, static_cast<s16>(bits - 1));
		else if (val <= lower)
			w.put(val, bits);
		else
			w.put(val + thr, bits);
		remaining -= c[s] < 0 ? -c[s] : c[s];
		s++;
		if (c[s - 1] == 0) {
			s16 z = 0;
			while (s + z < n && c[s + z] == 0)
				z++;
			s += z;
			while (z >= 3) {
				w.put(3, 2);
				z = static_cast<s16>(z - 3);
			}
			w.put(static_cast<u64>(z), 2);
		}
	}
	if (w.n)
		w.put(0, static_cast<s16>(8 - w.n));
	o->insert(o->end(), w.b.begin(), w.b.end());
}

/* a Huffman tree description with FSE coded weights (4.2.1.2): the
   nw weights w (the last symbol's left out), two states interleaved
   on one table, as huf_desc() decodes them; false if it does not fit
   (127 octets) or the weights are all the same */
bool huf_desc_fse(const u8 *w, s32 nw, std::vector<u8> *o)
{
	if (nw < 2)
		return false;
	u32 f[16] = {};
	s16 nsym = 0;
	for (s32 i = 0; i < nw; i++) {
		f[w[i]]++;
		nsym = std::max<s16>(nsym, static_cast<s16>(w[i] + 1));
	}
	const s16 al = 6;
	std::vector<s16> cnt = fse_normalize(f, nsym, al);
	Zstd::Fse t;
	if (!fse_build(&t, cnt.data(), nsym, al))
		return false;
	FseEnc enc;
	enc.build(cnt.data(), nsym, al);
	/* the decoder's last two states: the last weight's any, the one
	   before it with bits to read (that read runs off the start and
	   ends it) */
	u32 st[2];
	st[(nw - 1) & 1] = enc.start(w[nw - 1]);
	bool found = false;
	for (u16 s : enc.states[w[nw - 2]])
		if (t.nb[s]) {
			st[(nw - 2) & 1] = s;
			found = true;
			break;
		}
	if (!found)
		return false;
	BitW bw;
	for (s32 i = nw - 3; i >= 0; i--)
		st[i & 1] = enc.step(st[i & 1], w[i], &bw);
	bw.put(st[1], al);
	bw.put(st[0], al);
	bw.close();
	std::vector<u8> d;
	fse_desc_write(cnt, al, &d);
	size_t cs = d.size() + bw.b.size();
	if (cs > 127)
		return false;
	o->push_back(static_cast<u8>(cs));
	o->insert(o->end(), d.begin(), d.end());
	o->insert(o->end(), bw.b.begin(), bw.b.end());
	return true;
}

/* the estimated bits of the symbols on the counts (an accuracy log
   al table); -1 if one of them is not in it */
double fse_cost(const std::vector<s16> &cnt, s16 al, const s16 *sym, s32 n)
{
	double bits = 0;
	for (s32 k = 0; k < n; k++) {
		if (sym[k] >= static_cast<s16>(cnt.size()) || !cnt[sym[k]])
			return -1;
		s16 c = cnt[sym[k]] < 0 ? 1 : cnt[sym[k]];
		bits += al - (hbit(static_cast<u32>(c)) +
			      static_cast<double>(c - (1 << hbit(static_cast<u32>(c)))) /
				      (1 << hbit(static_cast<u32>(c))));
	}
	return bits;
}

} // namespace

/* see zstd_codec.h */
s16 Zstd::init(s16 window_log, const u8 *dict, s32 dict_n, s32 block)
{
	exit();
	if (window_log < 10 || window_log > ZSTD_MAX_WINDOW_LOG ||
	    dict_n < 0 || dict_n > (1 << window_log) || block < 64 ||
	    block > MAX_BLOCK)
		return -1;
	window_ = 1 << window_log;
	block_ = block;
	dict_id_ = 0;
	dict_tables_ = false;
	s32 at = dict_n >= 8 ? parse_dict(dict, dict_n) : 0;
	if (at < 0) {
		window_ = 0;
		return -1;
	}
	dict += at;
	dict_n -= at;
	if (dict_n)
		hist_.assign(dict, dict + dict_n);
	dict_n_ = dict_n;
	coded_ = dict_n;
	chain_base_ = 0;
	chain_.assign(hist_.size(), 0);
	head_.assign(1 << 14, 0);
	ins_ = 0;
	started_ = false;
	frame_start();
	st_ = MAGIC;
	need_ = 4;
	in_.clear();
	obuf_.clear();
	ohead_ = 0;
	raw_bits_ = cmp_bits_ = 0;
	return 0;
}

/* frees it all */
void Zstd::exit()
{
	window_ = 0;
	hist_.clear();
	head_.clear();
	chain_.clear();
	in_.clear();
	obuf_.clear();
	ohead_ = 0;
}

/* up to n octets of the output into buf; how many */
s16 Zstd::out(u8 *buf, s16 n)
{
	size_t k = obuf_.size() - ohead_;
	if (k > static_cast<size_t>(n))
		k = n;
	if (k)
		memcpy(buf, &obuf_[ohead_], k);
	ohead_ += k;
	if (ohead_ == obuf_.size()) {
		obuf_.clear();
		ohead_ = 0;
	}
	return static_cast<s16>(k);
}


/* the frame's start: the repeat offsets and the tables the
   dictionary has (or none) */
void Zstd::frame_start()
{
	for (s16 r = 0; r < 3; r++)
		rep_[r] = dict_rep_[r];
	have_ll_ = have_of_ = have_ml_ = dict_tables_;
	huf_ = dict_tables_ ? dict_huf_ : Huf();
	if (dict_tables_) {
		ll_ = dict_ll_;
		of_ = dict_of_;
		ml_ = dict_ml_;
	}
	enc_bits_ = dict_tables_ ? dict_bits_ : std::vector<u8>();
	for (s16 t = 0; t < 3; t++)
		cur_[t] = dict_tables_ ? dict_cur_[t] : Cur();
}

/* a formatted dictionary (5/RFC 8878) at d: its ID, tables and repeat
   offsets kept; the content's offset in it, 0 if it is raw content, -1
   if bad */
s32 Zstd::parse_dict(const u8 *d, s32 n)
{
	dict_rep_[0] = 1;
	dict_rep_[1] = 4;
	dict_rep_[2] = 8;
	u32 magic = d[0] | d[1] << 8 | d[2] << 16 | static_cast<u32>(d[3]) << 24;
	if (magic != 0xEC30A437u)
		return 0;
	dict_id_ = d[4] | d[5] << 8 | d[6] << 16 | static_cast<u32>(d[7]) << 24;
	s32 at = 8;
	s32 h = huf_desc(d + at, n - at, &dict_huf_);
	if (h < 0)
		return -1;
	at += h;
	dict_bits_.assign(256, 0);
	for (size_t k = 0; k < dict_huf_.sym.size(); k++)
		dict_bits_[dict_huf_.sym[k]] = dict_huf_.nb[k];
	/* offsets, match lengths, literals lengths */
	Fse *tabs[3] = { &dict_of_, &dict_ml_, &dict_ll_ };
	Cur *curs[3] = { &dict_cur_[1], &dict_cur_[2], &dict_cur_[0] };
	const s16 max_al[3] = { 8, 9, 9 }, nsym[3] = { 32, 53, 36 };
	for (s16 t = 0; t < 3; t++) {
		std::vector<s16> cnt;
		s16 al;
		s32 k = fse_desc(d + at, n - at, max_al[t], nsym[t], &cnt, &al);
		if (k < 0 || !fse_build(tabs[t], cnt.data(),
					static_cast<s16>(cnt.size()), al))
			return -1;
		curs[t]->cnt = cnt;
		curs[t]->al = al;
		at += k;
	}
	if (at + 12 > n)
		return -1;
	for (s16 r = 0; r < 3; r++, at += 4) {
		dict_rep_[r] = d[at] | d[at + 1] << 8 | d[at + 2] << 16 |
			       static_cast<u32>(d[at + 3]) << 24;
		if (!dict_rep_[r])
			return -1;
	}
	dict_tables_ = true;
	return at;
}

/* see zstd_codec.h */
std::vector<u8> Zstd::make_dict(const u8 *content, s32 n, u32 id)
{
	Stats st{};
	{
		Zstd e;
		e.init(ZSTD_WINDOW_LOG, nullptr, 0, ZSTD_BLOCK);
		e.set_stats(&st);
		for (s32 k = 0; k < n; k++)
			e.encode(content[k]);
		e.finish();
	}
	/* every literal, length code and offset code of the window (and
	   the content before it) at least once */
	for (auto &x : st.lit)
		x++;
	for (auto &x : st.ll)
		x++;
	for (auto &x : st.ml)
		x++;
	for (s16 s = 0; s <= 16; s++)
		st.of[s]++;
	std::vector<u8> o = { 0x37, 0xA4, 0x30, 0xEC, static_cast<u8>(id),
			      static_cast<u8>(id >> 8), static_cast<u8>(id >> 16),
			      static_cast<u8>(id >> 24) };
	u8 bits[256];
	huf_lengths(st.lit, bits);
	s16 mb = 0;
	for (s32 s = 0; s < 256; s++)
		mb = std::max<s16>(mb, bits[s]);
	/* the weights of symbols 0 to 254 (255's follows from them) */
	u8 w[255];
	for (s32 s = 0; s < 255; s++)
		w[s] = bits[s] ? static_cast<u8>(mb + 1 - bits[s]) : 0;
	if (!huf_desc_fse(w, 255, &o))
		return {};
	fse_desc_write(fse_normalize(st.of, 32, 8), 8, &o);
	fse_desc_write(fse_normalize(st.ml, 53, 9), 9, &o);
	fse_desc_write(fse_normalize(st.ll, 36, 9), 9, &o);
	for (u32 r : { 1u, 4u, 8u })
		for (s16 k = 0; k < 4; k++)
			o.push_back(static_cast<u8>(r >> (8 * k)));
	o.insert(o.end(), content, content + n);
	return o;
}


/*
 *    the encoder
 */

/* the hash of the three characters at hist_[idx] */
static u32 hash3(const u8 *p)
{
	return ((static_cast<u32>(p[0]) << 16 | static_cast<u32>(p[1]) << 8 |
		 p[2]) * 2654435761u) >> 18;
}

/* position i (absolute) into the hash chains */
void Zstd::insert(s32 i)
{
	s32 idx = i - chain_base_;
	if (idx < 0 || idx + 2 >= static_cast<s32>(hist_.size()))
		return;
	u32 h = hash3(&hist_[idx]);
	chain_[idx] = head_[h];
	head_[h] = i + 1;
}

/* the longest match at position i (absolute) up to end: its length
   (0 if none of 3), its offset in *off; the repeat offsets first */
s32 Zstd::best_match(s32 i, s32 end, s32 *off)
{
	s32 idx = i - chain_base_, lim = std::min(end - i, 65536);
	const u8 *p = &hist_[idx];
	s32 best = 0;
	auto len_at = [&](s32 d) {
		s32 l = 0;
		const u8 *q = p - d;
		while (l < lim && q[l] == p[l])
			l++;
		return l;
	};
	for (s16 r = 0; r < 3; r++) {
		s32 d = static_cast<s32>(rep_[r]);
		if (d <= 0 || d > idx || d > window_)
			continue;
		s32 l = len_at(d);
		if (l > best) {
			best = l;
			*off = d;
		}
	}
	s32 rep_best = best;
	if (lim >= 3) {
		u32 h = hash3(p);
		s32 c = head_[h], depth = 48;
		while (c && depth--) {
			s32 cand = c - 1, d = i - cand;
			if (cand < chain_base_ || d > window_ || d <= 0)
				break;
			s32 l = len_at(d);
			/* a new offset over a repeated one: two characters
			   more for its cost */
			if (l > best && (l > rep_best + 1 || best < 3) &&
			    (l > 3 || d <= 1024)) {
				best = l;
				*off = d;
			}
			if (best == lim)
				break;
			c = chain_[cand - chain_base_];
		}
	}
	return best >= 3 ? best : 0;
}

/* the window kept: what is a window behind the characters not coded
   yet dropped */
void Zstd::slide()
{
	s32 drop = coded_ - window_;
	if (drop < window_)
		return;
	hist_.erase(hist_.begin(), hist_.begin() + drop);
	chain_.erase(chain_.begin(), chain_.begin() + drop);
	chain_base_ += drop;
	coded_ -= drop;
}

/* a block's header and body out (and the frame's header first) */
void Zstd::put_block(const std::vector<u8> &body, s16 type, s32 size,
		     bool last)
{
	if (!started_) {
		started_ = true;
		const u8 fh[10] = { 0x28, 0xB5, 0x2F, 0xFD,
				    static_cast<u8>(dict_id_ ? 3 : 0),
				    static_cast<u8>((hbit(window_) - 10) << 3),
				    static_cast<u8>(dict_id_),
				    static_cast<u8>(dict_id_ >> 8),
				    static_cast<u8>(dict_id_ >> 16),
				    static_cast<u8>(dict_id_ >> 24) };
		s16 n = dict_id_ ? 10 : 6;
		obuf_.insert(obuf_.end(), fh, fh + n);
		cmp_bits_ += 8 * static_cast<u32>(n);
	}
	u32 h = (last ? 1u : 0u) | static_cast<u32>(type) << 1 |
		static_cast<u32>(size) << 3;
	obuf_.push_back(static_cast<u8>(h));
	obuf_.push_back(static_cast<u8>(h >> 8));
	obuf_.push_back(static_cast<u8>(h >> 16));
	obuf_.insert(obuf_.end(), body.begin(), body.end());
	cmp_bits_ += 8 * (3 + static_cast<u32>(body.size()));
}

/* the characters not coded yet as a block: sequences by the hash
   chains (a lazy match), the literals and the sequences coded, or the
   characters raw (or RLE) when that is no larger */
void Zstd::enc_block(bool last)
{
	slide();
	s32 n = static_cast<s32>(hist_.size()) - coded_;
	if (n <= 0) {
		if (last)
			put_block({}, 0, 0, true);
		return;
	}
	const s32 base = chain_base_, start = coded_ + base, end = start + n;
	u32 rep_saved[3] = { rep_[0], rep_[1], rep_[2] };
	struct Seq {
		u32 ll, ov, ml;
	};
	std::vector<Seq> seqs;
	std::vector<u8> lit;
	s32 i = start, lit_at = start;
	auto catch_up = [&](s32 upto) {
		while (ins_ < upto)
			insert(ins_++);
	};
	if (ins_ < base)
		ins_ = base;
	while (i + 3 <= end) {
		catch_up(i);
		s32 off = 0, len = best_match(i, end, &off);
		if (len && i + 1 + 3 <= end) {
			catch_up(i + 1);
			s32 off2 = 0, len2 = best_match(i + 1, end, &off2);
			if (len2 > len + 1) {
				i++;
				continue;
			}
		}
		if (!len) {
			i++;
			continue;
		}
		u32 ll = static_cast<u32>(i - lit_at), ov;
		u32 o = static_cast<u32>(off);
		if (ll) {
			ov = o == rep_[0] ? 1 : o == rep_[1] ? 2 :
			     o == rep_[2] ? 3 : o + 3;
		} else {
			ov = o == rep_[1] ? 1 : o == rep_[2] ? 2 :
			     o == rep_[0] - 1 ? 3 : o + 3;
		}
		apply_offset(rep_, ov, ll);
		for (s32 k = lit_at; k < i; k++)
			lit.push_back(hist_[k - base]);
		seqs.push_back({ ll, ov, static_cast<u32>(len) });
		i += len;
		lit_at = i;
	}
	for (s32 k = lit_at; k < end; k++)
		lit.push_back(hist_[k - base]);
	catch_up(std::max(ins_, end - 2));

	/* the literals: raw, RLE, Huffman with a new table or the last */
	std::vector<u8> body;
	s32 nl = static_cast<s32>(lit.size());
	u32 freq[256] = {};
	for (u8 c : lit)
		freq[c]++;
	s32 kinds = 0, last_sym = 0;
	for (s32 s = 0; s < 256; s++)
		if (freq[s]) {
			kinds++;
			last_sym = s;
		}
	std::vector<u8> best_lit, new_bits;
	{
		/* raw */
		lit_header(&best_lit, 0, nl, 0, false);
		best_lit.insert(best_lit.end(), lit.begin(), lit.end());
	}
	if (kinds == 1 && nl > 1) {
		std::vector<u8> r;
		lit_header(&r, 1, nl, 0, false);
		r.push_back(lit[0]);
		if (r.size() < best_lit.size())
			best_lit = r;
	} else if (kinds > 1 && nl >= 16) {
		u8 bits[256];
		/* a new table, its weights direct (symbols up to 128) */
		if (last_sym <= 128 && huf_lengths(freq, bits)) {
			s16 mb = 0;
			for (s32 s = 0; s < 256; s++)
				mb = std::max<s16>(mb, bits[s]);
			std::vector<u8> desc{ static_cast<u8>(127 + last_sym) };
			for (s32 s = 0; s < last_sym; s += 2) {
				u8 w0 = bits[s] ? static_cast<u8>(mb + 1 - bits[s]) : 0;
				u8 w1 = s + 1 < last_sym && bits[s + 1] ?
					static_cast<u8>(mb + 1 - bits[s + 1]) : 0;
				desc.push_back(static_cast<u8>(w0 << 4 | w1));
			}
			std::vector<u8> st = huf_section(lit, bits);
			if (!st.empty()) {
				std::vector<u8> r;
				s32 comp = static_cast<s32>(desc.size() + st.size());
				lit_header(&r, 2, nl, comp, nl < 1024 && comp < 1024);
				if (nl < 1024 && comp >= 1024)
					r.clear();
				if (!r.empty()) {
					r.insert(r.end(), desc.begin(), desc.end());
					r.insert(r.end(), st.begin(), st.end());
					if (r.size() < best_lit.size()) {
						best_lit = r;
						new_bits.assign(bits, bits + 256);
					}
				}
			}
		}
		/* the last table again, if it has every symbol */
		if (!enc_bits_.empty()) {
			bool all = true;
			for (s32 s = 0; s < 256 && all; s++)
				if (freq[s] && !enc_bits_[s])
					all = false;
			if (all) {
				std::vector<u8> st = huf_section(lit, enc_bits_.data());
				s32 comp = static_cast<s32>(st.size());
				if (!st.empty() && !(nl < 1024 && comp >= 1024)) {
					std::vector<u8> r;
					lit_header(&r, 3, nl, comp, nl < 1024);
					r.insert(r.end(), st.begin(), st.end());
					if (r.size() < best_lit.size()) {
						best_lit = r;
						new_bits.clear();
					}
				}
			}
		}
	}
	body = best_lit;

	/* the sequences */
	s32 ns = static_cast<s32>(seqs.size());
	if (ns < 128) {
		body.push_back(static_cast<u8>(ns));
	} else if (ns < 0x7F00) {
		body.push_back(static_cast<u8>((ns >> 8) + 128));
		body.push_back(static_cast<u8>(ns));
	} else {
		body.push_back(255);
		body.push_back(static_cast<u8>(ns - 0x7F00));
		body.push_back(static_cast<u8>((ns - 0x7F00) >> 8));
	}
	Cur cur_saved[3] = { cur_[0], cur_[1], cur_[2] };
	if (ns) {
		std::vector<s16> llc(ns), mlc(ns), ofc(ns);
		for (s32 k = 0; k < ns; k++) {
			llc[k] = len_code(seqs[k].ll, LL_BASE, 36);
			mlc[k] = len_code(seqs[k].ml, ML_BASE, 53);
			ofc[k] = hbit(seqs[k].ov);
		}
		/* each table, whichever costs least: the predefined, the
		   decoder's last again (if it has every code), or a new one
		   of the block's codes with its description */
		const s16 *codes[3] = { llc.data(), ofc.data(), mlc.data() };
		const s16 *defs[3] = { LL_DEF, OF_DEF, ML_DEF };
		const s16 dn[3] = { 36, 29, 53 }, dal[3] = { 6, 5, 6 };
		const s16 max_al[3] = { 9, 8, 9 };
		FseEnc enc[3];
		u8 modes = 0;
		std::vector<u8> descs;
		for (s16 t = 0; t < 3; t++) {
			std::vector<s16> def(defs[t], defs[t] + dn[t]);
			double cp = fse_cost(def, dal[t], codes[t], ns), cr = -1;
			if (!cur_[t].cnt.empty())
				cr = fse_cost(cur_[t].cnt, cur_[t].al, codes[t], ns);
			u32 f[53] = {};
			s16 nsym = 0, kinds = 0;
			for (s32 k = 0; k < ns; k++) {
				if (!f[codes[t][k]]++)
					kinds++;
				nsym = std::max<s16>(nsym, static_cast<s16>(codes[t][k] + 1));
			}
			s16 al = static_cast<s16>(std::min<s32>(max_al[t],
				std::max<s32>({ 5, hbit(static_cast<u32>(ns)) - 1,
						hbit(static_cast<u32>(kinds)) + 1 })));
			std::vector<s16> nc = fse_normalize(f, nsym, al);
			std::vector<u8> d;
			fse_desc_write(nc, al, &d);
			double cn = 8.0 * static_cast<double>(d.size()) +
				    fse_cost(nc, al, codes[t], ns);
			s16 mode = 0;
			if (cr >= 0 && (cp < 0 || cr <= cp) && cr <= cn) {
				mode = 3;
			} else if (cp >= 0 && cp <= cn) {
				cur_[t].cnt = def;
				cur_[t].al = dal[t];
			} else {
				mode = 2;
				cur_[t].cnt = nc;
				cur_[t].al = al;
				descs.insert(descs.end(), d.begin(), d.end());
			}
			enc[t].build(cur_[t].cnt.data(),
				     static_cast<s16>(cur_[t].cnt.size()), cur_[t].al);
			modes = static_cast<u8>(modes | mode << (6 - 2 * t));
		}
		body.push_back(modes);
		body.insert(body.end(), descs.begin(), descs.end());
		const FseEnc &fl = enc[0], &fo = enc[1], &fm = enc[2];
		BitW w;
		auto extras = [&](s32 k) {
			w.put(seqs[k].ll - LL_BASE[llc[k]], LL_BITS[llc[k]]);
			w.put(seqs[k].ml - ML_BASE[mlc[k]], ML_BITS[mlc[k]]);
			w.put(seqs[k].ov - (1u << ofc[k]), ofc[k]);
		};
		u32 sl = fl.start(llc[ns - 1]), sm = fm.start(mlc[ns - 1]),
		    so = fo.start(ofc[ns - 1]);
		extras(ns - 1);
		for (s32 k = ns - 2; k >= 0; k--) {
			so = fo.step(so, ofc[k], &w);
			sm = fm.step(sm, mlc[k], &w);
			sl = fl.step(sl, llc[k], &w);
			extras(k);
		}
		w.put(sm, fm.al);
		w.put(so, fo.al);
		w.put(sl, fl.al);
		w.close();
		body.insert(body.end(), w.b.begin(), w.b.end());
	}

	bool same = std::all_of(hist_.begin() + coded_, hist_.end(),
				[&](u8 c) { return c == hist_[coded_]; });
	bool coded = false;
	if (same && n > 1) {
		put_block({ hist_[coded_] }, 1, n, last);
	} else if (static_cast<s32>(body.size()) < n) {
		put_block(body, 2, static_cast<s32>(body.size()), last);
		if (!new_bits.empty())
			enc_bits_ = new_bits;
		coded = true;
	} else {
		put_block(std::vector<u8>(hist_.begin() + coded_, hist_.end()),
			  0, n, last);
	}
	if (!coded) {
		/* (the decoder saw none of it: as before) */
		for (s16 r = 0; r < 3; r++) {
			rep_[r] = rep_saved[r];
			cur_[r] = cur_saved[r];
		}
	} else if (stats_) {
		for (u8 c : lit)
			stats_->lit[c]++;
		for (const auto &q : seqs) {
			stats_->ll[len_code(q.ll, LL_BASE, 36)]++;
			stats_->ml[len_code(q.ml, ML_BASE, 53)]++;
			stats_->of[hbit(q.ov)]++;
		}
	}
	coded_ += n;
}

/* see zstd_codec.h */
s16 Zstd::encode(u8 ch)
{
	if (!ready())
		return -1;
	hist_.push_back(ch);
	chain_.push_back(0);
	raw_bits_ += 8;
	if (static_cast<s32>(hist_.size()) - coded_ >= block_)
		enc_block(false);
	return 0;
}

/* see zstd_codec.h */
s16 Zstd::flush_encoder()
{
	if (!ready())
		return 0;
	if (static_cast<s32>(hist_.size()) > coded_)
		enc_block(false);
	return 0;
}

/* see zstd_codec.h */
s16 Zstd::finish()
{
	if (!ready())
		return -1;
	enc_block(true);
	return 0;
}


/*
 *    the decoder
 */

/* a character out, into the window */
void Zstd::emit(u8 c)
{
	hist_.push_back(c);
	obuf_.push_back(c);
}

/* the window kept: the characters before it dropped */
void Zstd::trim()
{
	size_t keep = static_cast<size_t>(window_);
	if (hist_.size() > 2 * keep + MAX_BLOCK)
		hist_.erase(hist_.begin(), hist_.end() - keep);
}

/* the literals section at p (a block's n octets) into lit: its
   octets, or -1 */
s32 Zstd::literals(const u8 *p, s32 n, std::vector<u8> *lit)
{
	if (n < 1)
		return -1;
	s16 type = p[0] & 3, sf = (p[0] >> 2) & 3;
	lit->clear();
	if (type < 2) {
		s32 regen, hs;
		if (!(sf & 1)) {
			regen = p[0] >> 3;
			hs = 1;
		} else if (sf == 1) {
			if (n < 2)
				return -1;
			regen = (p[0] >> 4) | p[1] << 4;
			hs = 2;
		} else {
			if (n < 3)
				return -1;
			regen = (p[0] >> 4) | p[1] << 4 | p[2] << 12;
			hs = 3;
		}
		if (regen > MAX_BLOCK)
			return -1;
		if (type == 0) {
			if (hs + regen > n)
				return -1;
			lit->assign(p + hs, p + hs + regen);
			return hs + regen;
		}
		if (hs + 1 > n)
			return -1;
		lit->assign(regen, p[hs]);
		return hs + 1;
	}
	s16 streams = sf == 0 ? 1 : 4, hs = sf < 2 ? 3 : sf == 2 ? 4 : 5,
	    nb = sf < 2 ? 10 : sf == 2 ? 14 : 18;
	if (n < hs)
		return -1;
	u64 h = 0;
	for (s16 k = 0; k < hs; k++)
		h |= static_cast<u64>(p[k]) << (8 * k);
	s32 regen = static_cast<s32>((h >> 4) & ((1u << nb) - 1));
	s32 comp = static_cast<s32>((h >> (4 + nb)) & ((1u << nb) - 1));
	if (hs + comp > n || regen > MAX_BLOCK)
		return -1;
	const u8 *q = p + hs;
	s32 left = comp;
	if (type == 2) {
		s32 d = huf_desc(q, left, &huf_);
		if (d < 0)
			return -1;
		q += d;
		left -= d;
	} else if (huf_.sym.empty()) {
		return -1;
	}
	if (streams == 1) {
		if (!huf_stream(huf_, q, left, regen, lit))
			return -1;
	} else {
		if (left < 6)
			return -1;
		s32 sz[4];
		sz[0] = q[0] | q[1] << 8;
		sz[1] = q[2] | q[3] << 8;
		sz[2] = q[4] | q[5] << 8;
		sz[3] = left - 6 - sz[0] - sz[1] - sz[2];
		if (sz[3] < 0)
			return -1;
		s32 seg = (regen + 3) / 4;
		q += 6;
		for (s16 k = 0; k < 4; k++) {
			s32 cnt = k < 3 ? seg : regen - 3 * seg;
			if (cnt < 0 || !huf_stream(huf_, q, sz[k], cnt, lit))
				return -1;
			q += sz[k];
		}
	}
	return hs + comp;
}

/* the sequences section at p (the rest of the block) carried out with
   the literals; 0, or -1 */
s16 Zstd::sequences(const u8 *p, s32 n, const std::vector<u8> &lit)
{
	if (n < 1)
		return -1;
	s32 ns, at;
	if (p[0] < 128) {
		ns = p[0];
		at = 1;
	} else if (p[0] < 255) {
		if (n < 2)
			return -1;
		ns = ((p[0] - 128) << 8) + p[1];
		at = 2;
	} else {
		if (n < 3)
			return -1;
		ns = p[1] + (p[2] << 8) + 0x7F00;
		at = 3;
	}
	size_t li = 0;
	if (ns) {
		if (at >= n)
			return -1;
		u8 modes = p[at++];
		if (modes & 3)
			return -1;
		struct T {
			Fse *t;
			bool *have;
			const s16 *def;
			s16 nsym, def_al, max_al;
			s16 mode;
		} tabs[3] = {
			{ &ll_, &have_ll_, LL_DEF, 36, 6, 9,
			  static_cast<s16>(modes >> 6) },
			{ &of_, &have_of_, OF_DEF, 32, 5, 8,
			  static_cast<s16>((modes >> 4) & 3) },
			{ &ml_, &have_ml_, ML_DEF, 53, 6, 9,
			  static_cast<s16>((modes >> 2) & 3) },
		};
		for (auto &t : tabs) {
			switch (t.mode) {
			case 0:
				fse_build(t.t, t.def, t.def == OF_DEF ? 29 : t.nsym,
					  t.def_al);
				break;
			case 1: {
				if (at >= n || p[at] >= t.nsym)
					return -1;
				t.t->log = 0;
				t.t->sym.assign(1, p[at++]);
				t.t->nb.assign(1, 0);
				t.t->base.assign(1, 0);
				break;
			}
			case 2: {
				std::vector<s16> cnt;
				s16 al;
				s32 d = fse_desc(p + at, n - at, t.max_al, t.nsym, &cnt,
						 &al);
				if (d < 0 ||
				    !fse_build(t.t, cnt.data(),
					       static_cast<s16>(cnt.size()), al))
					return -1;
				at += d;
				break;
			}
			case 3:
				if (!*t.have)
					return -1;
				break;
			}
			*t.have = true;
		}
		BackBits in{ p + at, n - at };
		if (!in.init())
			return -1;
		u32 sl = static_cast<u32>(in.read(ll_.log)),
		    so = static_cast<u32>(in.read(of_.log)),
		    sm = static_cast<u32>(in.read(ml_.log));
		for (s32 k = 0; k < ns; k++) {
			u8 llc = ll_.sym[sl], ofc = of_.sym[so], mlc = ml_.sym[sm];
			if (llc > 35 || mlc > 52 || ofc > 31)
				return -1;
			u32 ov = (1u << ofc) + static_cast<u32>(in.read(ofc));
			u32 ml = ML_BASE[mlc] + static_cast<u32>(in.read(ML_BITS[mlc]));
			u32 ll = LL_BASE[llc] + static_cast<u32>(in.read(LL_BITS[llc]));
			if (k + 1 < ns) {
				sl = ll_.base[sl] + static_cast<u32>(in.read(ll_.nb[sl]));
				sm = ml_.base[sm] + static_cast<u32>(in.read(ml_.nb[sm]));
				so = of_.base[so] + static_cast<u32>(in.read(of_.nb[so]));
			}
			if (in.over() || li + ll > lit.size())
				return -1;
			for (u32 j = 0; j < ll; j++)
				emit(lit[li++]);
			u32 off = apply_offset(rep_, ov, ll);
			if (!off || off > hist_.size() ||
			    off > static_cast<u32>(window_) + dict_n_)
				return -1;
			size_t from = hist_.size() - off;
			for (u32 j = 0; j < ml; j++)
				emit(hist_[from + j]);
		}
	}
	while (li < lit.size())
		emit(lit[li++]);
	return 0;
}

/* a frame header at p (n octets, all of it): 0, or -1 */
s16 Zstd::header(const u8 *p, s32 n)
{
	(void)n;
	u8 fhd = p[0];
	bool single = fhd & 0x20;
	if (fhd & 0x08)
		return -1;
	checksum_ = fhd & 0x04;
	s16 did = (fhd & 3) == 3 ? 4 : (fhd & 3);
	s16 fcs_flag = fhd >> 6;
	s16 fcs = fcs_flag == 0 ? (single ? 1 : 0) : 1 << fcs_flag;
	s32 at = 1;
	u64 win = 0;
	if (!single) {
		u8 wd = p[at++];
		s16 exp = wd >> 3, man = wd & 7;
		u64 wb = 1ull << (10 + exp);
		win = wb + wb / 8 * man;
	}
	at += did;
	if (fcs) {
		u64 v = 0;
		for (s16 k = 0; k < fcs; k++)
			v |= static_cast<u64>(p[at + k]) << (8 * k);
		if (fcs == 2)
			v += 256;
		if (single)
			win = v;
	}
	if (win > (1ull << ZSTD_MAX_WINDOW_LOG))
		return -1;
	window_ = static_cast<s32>(std::max<u64>(win, 1));
	return 0;
}

/* a block's n octets (of type block_type_): 0, or -1 */
s16 Zstd::block(const u8 *p, s32 n)
{
	switch (block_type_) {
	case 0:
		for (s32 k = 0; k < n; k++)
			emit(p[k]);
		break;
	case 1:
		for (s32 k = 0; k < block_size_; k++)
			emit(p[0]);
		break;
	case 2: {
		std::vector<u8> lit;
		s32 l = literals(p, n, &lit);
		if (l < 0 || sequences(p + l, n - l, lit))
			return -1;
		break;
	}
	default:
		return -1;
	}
	trim();
	return 0;
}

/* see zstd_codec.h: the octets kept until a step has them all */
s16 Zstd::decode(u8 octet)
{
	if (!ready() || st_ == ERROR)
		return -1;
	if (st_ == DONE) {
		/* another frame: the history anew (the dictionary kept) */
		hist_.resize(static_cast<size_t>(dict_n_));
		st_ = MAGIC;
		need_ = 4;
	}
	in_.push_back(octet);
	while (static_cast<s32>(in_.size()) >= need_ && st_ != DONE) {
		const u8 *p = in_.data();
		s32 used = need_;
		switch (st_) {
		case MAGIC: {
			u32 m = p[0] | p[1] << 8 | p[2] << 16 |
				static_cast<u32>(p[3]) << 24;
			if (m == MAGIC_ZSTD) {
				st_ = HEADER;
				need_ = 1;
				frame_start();
			} else if ((m & 0xFFFFFFF0u) == 0x184D2A50u) {
				st_ = SKIP;
				need_ = 4;
				skip_ = -1;
			} else {
				st_ = ERROR;
				return -1;
			}
			break;
		}
		case HEADER: {
			u8 fhd = p[0];
			bool single = fhd & 0x20;
			s16 did = (fhd & 3) == 3 ? 4 : (fhd & 3);
			s16 f = fhd >> 6;
			s32 hs = 1 + (single ? 0 : 1) + did +
				 (f == 0 ? (single ? 1 : 0) : 1 << f);
			if (static_cast<s32>(in_.size()) < hs) {
				need_ = hs;
				return 0;
			}
			if (header(p, hs)) {
				st_ = ERROR;
				return -1;
			}
			used = hs;
			st_ = BLOCK_HEADER;
			need_ = 3;
			break;
		}
		case BLOCK_HEADER: {
			u32 h = p[0] | p[1] << 8 | p[2] << 16;
			last_block_ = h & 1;
			block_type_ = static_cast<s16>((h >> 1) & 3);
			block_size_ = static_cast<s32>(h >> 3);
			if (block_type_ == 3 || block_size_ > MAX_BLOCK) {
				st_ = ERROR;
				return -1;
			}
			st_ = BLOCK;
			need_ = block_type_ == 1 ? 1 : block_size_;
			if (!need_) {
				/* an empty raw block */
				in_.erase(in_.begin(), in_.begin() + used);
				used = 0;
				st_ = last_block_ ? (checksum_ ? SKIP : DONE) :
						    BLOCK_HEADER;
				need_ = last_block_ ? (checksum_ ? 4 : 0) : 3;
				skip_ = 4;
			}
			break;
		}
		case BLOCK:
			if (block(p, need_)) {
				st_ = ERROR;
				return -1;
			}
			if (last_block_) {
				st_ = checksum_ ? SKIP : DONE;
				need_ = checksum_ ? 4 : 0;
				skip_ = 4;
			} else {
				st_ = BLOCK_HEADER;
				need_ = 3;
			}
			break;
		case SKIP:
			if (skip_ < 0) {
				/* a skippable frame's size */
				skip_ = static_cast<s32>(
					p[0] | p[1] << 8 | p[2] << 16 |
					static_cast<u32>(p[3]) << 24);
				need_ = std::max<s32>(skip_, 0);
				skip_ = 0;
				if (need_)
					break;
			}
			st_ = DONE;
			need_ = 0;
			break;
		default:
			break;
		}
		if (used)
			in_.erase(in_.begin(), in_.begin() + used);
		if (st_ == DONE)
			break;
	}
	return 0;
}

} // namespace dlink
