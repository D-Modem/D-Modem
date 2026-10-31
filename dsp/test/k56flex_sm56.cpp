/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    k56flex_sm56.cpp - K56flex's data mode encoder (dsp/k56flex.h) into
 *                       the data mode decoder of Motorola's SM56 client
 *                       (sm56.ko's pstn_init_rx and putBIOS, called as
 *                       they are, regparm(3) as the kernel was built): at
 *                       every rate, pad and robbed-bit pattern, the
 *                       client's octets must be the data sent.  And our
 *                       training (K56Train) against the digital modem's
 *                       transmitter the module carries unused (isdn_tx):
 *                       the same codes.  Exits 1 when one is not.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "k56flex.h"

using namespace dsp;

#define RP __attribute__((regparm(3)))
extern "C" {
RP void pstn_reset_rx(int mode, void *st, int *aux);
RP void pstn_init_rx(void *st, int rate);
RP int putBIOS(void *st, int *code, void *ctx);
RP void isdn_reset_tx(int mode, void *st, int *aux);
RP void isdn_tx(void *st, int unused, unsigned char *out, int *count,
		int *done, void *ctx);
}

namespace {

std::vector<unsigned char> got;

/* SM56's decoder's octets out */
RP void put_octet(int b, void *)
{
	got.push_back(static_cast<unsigned char>(b));
}

/* a field of SM56's receiver state */
int &field(char *st, int off)
{
	return *reinterpret_cast<int *>(st + off);
}

/* nframes frames of random octets at rate (kbit/s), pad and robbed
   through our encoder and SM56's decoder; true when they come out */
bool check(s16 kbps, s16 pad, unsigned char robbed, s32 nframes)
{
	static char st[0x20000];
	int aux[4];
	memset(st, 0, sizeof st);
	pstn_reset_rx(0, st, aux);
	*reinterpret_cast<void **>(st + 0x20) = reinterpret_cast<void *>(put_octet);
	field(st, 0x1110) = pad;
	for (int k = 0; k < 6; k++)
		field(st, 0x10f8 + 4 * k) = (robbed >> k) & 1 ? 0 : 2;
	pstn_init_rx(st, kbps);
	K56Enc e;
	if (e.init({ kbps * 1000, pad, 0, robbed }))
		return false;
	srand(static_cast<unsigned>(kbps * 64 + pad * 8 + robbed));
	std::vector<unsigned char> sent;
	std::vector<unsigned char> bits;
	got.clear();
	for (s32 k = 0; k < nframes; k++) {
		unsigned char code[8];
		s16 nb = e.frame_bits();
		while (static_cast<s32>(bits.size()) < nb) {
			unsigned char o = static_cast<unsigned char>(rand());
			sent.push_back(o);
			for (int i = 0; i < 8; i++)
				bits.push_back((o >> i) & 1);
		}
		e.frame(bits.data(), code);
		bits.erase(bits.begin(), bits.begin() + nb);
		for (int i = 0; i < 8; i++) {
			int c = code[i];
			putBIOS(st, &c, nullptr);
		}
	}
	/* SM56 decodes a frame when the next one starts */
	size_t want = sent.size() - (kbps + 7) / 8 - 1;
	bool ok = got.size() >= want &&
		  !memcmp(got.data(), sent.data(), want);
	size_t first = 0;
	while (first < want && first < got.size() && got[first] == sent[first])
		first++;
	printf("%d bit/s pad %d robbed %#04x: %zu octets sent, %zu decoded: %s",
	       kbps * 1000, pad, robbed, sent.size(), got.size(),
	       ok ? "ok\n" : "WRONG");
	if (!ok)
		printf(" from octet %zu\n", first);
	return ok;
}

/* isdn_tx's code (its own: positive values 0 to 127, the smallest 0,
   negative ones their complements) as ours (bit 7 positive) */
unsigned char from_isdn(unsigned char c)
{
	return c & 0x80 ? static_cast<unsigned char>(0xff - c) :
		static_cast<unsigned char>(0x80 | c);
}

/* the training from silence to E, with J's bits js and MP's octets
   random: isdn_tx's codes and ours (without TRN1B, which isdn_tx does
   not send; SM56's receiver wants it, see sm56_k56flex) */
bool check_train(u16 js)
{
	static char st[0x20000];
	int aux[4];
	memset(st, 0, sizeof st);
	isdn_reset_tx(0, st, aux);
	K56Train t;
	t.init(0);
	t.js = js;
	t.no_trn1b = 1;
	field(st, 0x44) = js;
	srand(js);
	for (int i = 0; i < 11; i++) {
		st[0x48 + i] = static_cast<char>(t.mp[i] = static_cast<unsigned char>(rand()));
		st[0x60 + i] = static_cast<char>(t.mpp[i] = static_cast<unsigned char>(rand()));
	}
	s32 n = 0;
	for (; t.seg != K56_DATA; n++) {
		unsigned char o;
		int c = 1, done = 0, seg = field(st, 0);
		/* isdn_tx's segments are ours from 1 on, without TRN1B */
		s16 ours = t.seg < K56_TRN1B ? t.seg + 1 : t.seg;
		isdn_tx(st, 0, &o, &c, &done, nullptr);
		unsigned char want = from_isdn(o), got_c = t.next();
		if (seg != ours || want != got_c) {
			printf("training, J %#06x: symbol %d (segment %d, %d): "
			       "%#04x, ours %#04x: WRONG\n", js, n, seg, ours,
			       want, got_c);
			return false;
		}
	}
	printf("training, J %#06x: %d codes: ok\n", js, n);
	return true;
}

} // namespace

/* every rate, pad and a set of robbed-bit patterns */
int main()
{
	static const unsigned char robbed[] = { 0, 1, 3, 7, 0x15, 0x0f, 0x1f,
						0x3f, 0x21 };
	int bad = 0;
	for (s16 kbps = 32; kbps <= 56; kbps += 2)
		for (s16 pad = 0; pad < 3; pad++)
			for (unsigned char rb : robbed)
				bad += !check(kbps, pad, rb, 1000);
	bad += !check_train(0x8990);
	bad += !check_train(0xb0e1);
	printf("%d wrong\n", bad);
	return bad ? 1 : 0;
}
