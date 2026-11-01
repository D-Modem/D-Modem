/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v8bis.h - ITU-T V.8 bis (11/2000): the identification and selection
 *              of a common mode of operation, before V.8 or during a
 *              telephone call; the start-up of V.70's simultaneous voice
 *              and data, and of plain data
 *
 *    Msg, Tree   the messages (MS, CL, CLR, ACK, NAK; clause 8): the
 *                identification field (I) and the standard information
 *                field (S), each a tree of parameters (8.2) with its
 *                delimiting bits
 *    Caps        what a station has, or an MS selects, of the trees:
 *                the I field's V.8 start-ups and ACK(1), the S field's
 *                modes (data, SVD ...) and the data and SVD modes'
 *                parameters
 *    SigTx/SigRx the signals (7.1): segment 1 a dual tone (one pair for
 *                the initiating station, one for the responding),
 *                segment 2 a single tone naming the signal
 *    MsgTx/MsgRx the messages on the line (7.2): V.21 (the initiating
 *                station on the low channel, the responding one on the
 *                high), 100 ms of mark, flags, the HDLC frame with its
 *                FCS and stuffed zeros, flags
 *
 *    The transactions (clause 9) are the pump's (dp_v8bis.cpp).
 *
 */

#ifndef DSP_V8BIS_H
#define DSP_V8BIS_H

#include <cstddef>
#include <vector>

#include "dm_types.h"
#include "dsp.h"
#include "fax_hdlc.h"

namespace dsp {
namespace v8bis {

/* the message types (Table 3) */
enum Type : u8 {
	MS = 1, CL = 2, CLR = 3, ACK1 = 4, ACK2 = 5,
	NAK1 = 8, NAK2 = 9, NAK3 = 10, NAK4 = 11,
};
/* our revision (Table 4) */
constexpr u8 REVISION = 2;
/* the most octets an information field has (9.10) */
constexpr size_t MAX_INFO = 64;

/* a tree of parameters (8.2, Figure 7): the level 1 blocks' octets (7
   bits each, the delimiter taken off), one level 2 block for each SPar(1)
   set, in the order of their bits; each of those its NPar(2) and SPar(2)
   octets (6 bits each) and one NPar(3) block for each SPar(2) set */
struct Tree {
	struct Par2 {
		std::vector<u8> npar2, spar2;
		std::vector<std::vector<u8>> npar3;
	};
	std::vector<u8> npar1, spar1;
	std::vector<Par2> par2;
};
/* t onto out */
void tree_encode(const Tree &t, std::vector<u8> *out);
/* a tree from p's n octets at *pos (moved past it): false when they end
   before it does */
bool tree_decode(const u8 *p, size_t n, size_t *pos, Tree *t);

/* a message: its type and revision, the I and S fields' trees (an ACK's
   and a NAK's are empty) */
struct Msg {
	u8 type = 0;
	u8 rev = REVISION;
	Tree id, s;
};
/* m as an information field (no FCS) */
std::vector<u8> msg_encode(const Msg &m);
/* an information field p of n octets into *m (the non-standard field
   left out): false when it is short or its type is not one of Table 3's */
bool msg_decode(const u8 *p, size_t n, Msg *m);
/* a type's name, for the log */
const char *type_name(u8 type);

/* the I field's NPar(1) (Table 5-1) */
enum IdPar : u8 {
	ID_V8 = 0x01, ID_SHORT_V8 = 0x02, ID_MORE = 0x04, ID_ACK1 = 0x08,
	ID_NS = 0x40,
};
/* the S field's SPar(1), octet 1 (Table 6-2a) */
enum Mode : u8 {
	MODE_DATA = 0x01, MODE_SVD = 0x02, MODE_H324 = 0x04, MODE_V18 = 0x08,
	MODE_T30 = 0x10, MODE_TEL = 0x20, MODE_T101 = 0x40,
};
/* the data mode's NPar(2) (Tables 6-3a to 6-3d), six bits an octet, the
   first octet's lowest */
enum DataPar : u32 {
	DATA_TRANSPARENT = 1u << 0, DATA_V42 = 1u << 1, DATA_V42BIS = 1u << 2,
	DATA_V14 = 1u << 3, DATA_T120 = 1u << 4, DATA_NS = 1u << 5,
	DATA_T84 = 1u << 6, DATA_T434 = 1u << 7, DATA_V80 = 1u << 8,
	DATA_V34 = 1u << 10, DATA_V32BIS = 1u << 11,
	DATA_V32 = 1u << 12, DATA_V22BIS = 1u << 13, DATA_V22 = 1u << 14,
	DATA_V21 = 1u << 15, DATA_V90A = 1u << 16, DATA_V90D = 1u << 17,
	DATA_V91 = 1u << 18, DATA_V92A = 1u << 19, DATA_V92D = 1u << 20,
};
/* the data mode's modulations, the best first */
constexpr u32 DATA_MODS = DATA_V34 | DATA_V32BIS | DATA_V32 | DATA_V22BIS |
			  DATA_V22 | DATA_V21 | DATA_V90A | DATA_V90D |
			  DATA_V91 | DATA_V92A | DATA_V92D;
/* simultaneous voice and data's NPar(2) (Tables 6-4a to 6-4c), as
   above */
enum SvdPar : u32 {
	SVD_V70 = 1u << 0, SVD_V61 = 1u << 1, SVD_V34 = 1u << 3,
	SVD_V32BIS = 1u << 4, SVD_NS = 1u << 5,
	SVD_TRANSPARENT = 1u << 6, SVD_V42 = 1u << 7, SVD_V42BIS = 1u << 8,
	SVD_V14 = 1u << 9, SVD_T120 = 1u << 10, SVD_HDLC = 1u << 11,
	SVD_T84 = 1u << 12, SVD_T434 = 1u << 13, SVD_V76 = 1u << 14,
};

/* what a CL or CLR says a station has, or an MS selects: the I field's
   NPar(1) (IdPar), the S field's first SPar(1) octet (Mode), and the data
   and SVD modes' NPar(2) */
struct Caps {
	u8 id = 0;
	u8 modes = 0;
	u32 data = 0;
	u32 svd = 0;
};
/* a message of type with caps c */
Msg msg_of(u8 type, const Caps &c);
/* the caps of m (what it has of them) */
Caps caps_of(const Msg &m);

/* the signals (Tables 1 and 2) */
enum Sig : u8 {
	SIG_NONE, SIG_MRE, SIG_MRD, SIG_CRE, SIG_CRD, SIG_ESI, SIG_ESR,
};
/* a signal's name, for the log */
const char *sig_name(u8 s);

/* a signal out: segment 1 (400 ms) on the initiating pair or the
   responding one, segment 2 (100 ms) its tone; ESi and ESr without their
   segment 2, which is the message's preamble that follows (7.2.4) */
struct SigTx {
	Osc a, b, c;
	s32 n, seg1, seg2;
	float amp;

	/* s from the initiating station or not, at a peak of amp (the two
	   tones of segment 1 half its power each) at srate */
	void init(u8 s, bool initiating, float amp, s32 srate);
	/* the next sample, 0 once done */
	float next();
	/* all of it went out */
	bool done() const { return n >= seg1 + seg2; }
};

/* the signals in: segment 1 of the pairs asked for, 180 ms of it at
   least, then within 80 ms segment 2's tone for 40 ms */
struct SigRx {
	enum { PAIR_INIT = 1, PAIR_RESP = 2 };
	enum { NG = 10 };
	Goertzel g[NG];
	s16 len, pairs;
	s16 pair;               /* the pair whose segment 1 is on (0 none) */
	s16 seg1, after;        /* blocks of it, blocks since it ended */
	s16 tone, seg2;         /* segment 2's tone (index in g), blocks */
	float energy;           /* the block's samples squared so far */

	/* 10 ms blocks at srate, for the pairs (PAIR_INIT | PAIR_RESP) */
	void init(s32 srate, s16 pairs);
	/* a sample in: a signal when it has been told, else SIG_NONE */
	u8 run(float x);
	/* segment 1 of a pair is on (180 ms of it) */
	bool seg1_on() const { return seg1 >= 18; }
private:
	u8 block(float bp);
};

/* tags for the HDLC mixins */
struct HdlcV8bis {};

/* messages out on V.21: each with its 100 ms of mark, three flags, the
   frame, two flags; one after another (CL then MS) with no gap */
struct MsgTx : HdlcTx<MsgTx, MAX_INFO, HdlcV8bis> {
	FskBitTx fsk;
	std::vector<std::vector<u8>> q;   /* the messages to go */
	size_t at;                        /* the one going out */
	s16 preamble;                     /* bits of mark left */
	bool framed;                      /* its frame handed over */
	float amp;
	s32 srate;
	bool high;

	/* on V.21's high channel (the responding station) or its low one,
	   at a peak of amp, at srate */
	void init(bool high, float amp, s32 srate);
	/* m goes out after those before it */
	void send(const std::vector<u8> &m);
	/* the next sample (0 when there is nothing to send) */
	float next();
	/* nothing (more) to send */
	bool idle() const { return at >= q.size(); }
	/* the HDLC transmitter's frame: the message's once */
	s16 hdlc_tx_next(HdlcV8bis, unsigned char *buf, s16 max);
private:
	/* the next bit on the line, -1 at the end of a message */
	s16 bit();
	void start();
};

/* messages in on V.21's other channel: a frame with its FCS right is a
   message (msgs), one with a wrong FCS an invalid frame (bad) */
struct MsgRx : HdlcRx<MsgRx, MAX_INFO + 2, HdlcV8bis> {
	FskDemod demod;
	std::vector<std::vector<u8>> msgs;
	s16 bad;

	/* the high channel (from the responding station) or the low one, at
	   srate */
	void init(bool high, s32 srate);
	/* a sample in */
	void run(float x);
	/* the HDLC receiver's frame */
	void hdlc_rx_frame(HdlcV8bis, const unsigned char *d, s16 len,
			   bool whole);
};

} // namespace v8bis
} // namespace dsp

#endif
