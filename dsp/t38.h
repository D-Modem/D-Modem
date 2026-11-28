/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t38.h - ITU-T T.38 fax over IP: the IFP packets (Annex A) in the
 *            encoding of the 1998 version (T38FaxVersion 0) and of the
 *            2002 one (1 to 3), and their UDPTL transport (sequence
 *            numbers, and the packets before again for redundancy, or
 *            the parity FEC of Annex C)
 *
 *    The octets of HDLC and T.4 data are the line's bits with the first
 *    one in the most significant bit (T.38 7.1.2), not in the lowest as
 *    T.30's frames are written; t38_reverse() turns one into the other.
 *
 */

#ifndef __T38_H__
#define __T38_H__


#include "dm_types.h"

namespace dsp {

/* t30-indicator (T.38 Table 3); from T38_IND_V8_ANSAM on only in the
   2002 version */
enum t38_indicator {
	T38_IND_NO_SIGNAL, T38_IND_CNG, T38_IND_CED, T38_IND_V21_PREAMBLE,
	T38_IND_V27_2400, T38_IND_V27_4800, T38_IND_V29_7200, T38_IND_V29_9600,
	T38_IND_V17_7200_SHORT, T38_IND_V17_7200_LONG,
	T38_IND_V17_9600_SHORT, T38_IND_V17_9600_LONG,
	T38_IND_V17_12000_SHORT, T38_IND_V17_12000_LONG,
	T38_IND_V17_14400_SHORT, T38_IND_V17_14400_LONG,
	T38_IND_V8_ANSAM, T38_IND_V8_SIGNAL, T38_IND_V34_CNTL_1200,
	T38_IND_V34_PRI, T38_IND_V34_CC_RETRAIN, T38_IND_V33_12000,
	T38_IND_V33_14400,
};

/* t30-data, the data's modulation (Table 4); from T38_DATA_V8 on only in
   the 2002 version */
enum t38_data_type {
	T38_DATA_V21, T38_DATA_V27_2400, T38_DATA_V27_4800,
	T38_DATA_V29_7200, T38_DATA_V29_9600, T38_DATA_V17_7200,
	T38_DATA_V17_9600, T38_DATA_V17_12000, T38_DATA_V17_14400,
	T38_DATA_V8, T38_DATA_V34_PRI_RATE, T38_DATA_V34_CC_1200,
	T38_DATA_V34_PRI_CH, T38_DATA_V33_12000, T38_DATA_V33_14400,
};

/* field-type (Table 5); from T38_CM_MESSAGE on only in the 2002
   version */
enum t38_field_type {
	T38_HDLC_DATA, T38_HDLC_SIG_END, T38_HDLC_FCS_OK, T38_HDLC_FCS_BAD,
	T38_HDLC_FCS_OK_SIG_END, T38_HDLC_FCS_BAD_SIG_END,
	T38_T4_DATA, T38_T4_SIG_END,
	T38_CM_MESSAGE, T38_JM_MESSAGE, T38_CI_MESSAGE, T38_V34RATE,
};

#define T38_MAX_FIELDS  8

struct t38_field {
	s16 type;                       /* enum t38_field_type */
	const unsigned char *data;      /* len octets, NULL for none */
	s32 len;
};

/* an IFP packet: an indicator, or data of a modulation in fields */
struct t38_ifp {
	s16 data;                       /* t30-data, else t30-indicator */
	s16 type;                       /* the indicator, or the data's
					   modulation */
	s16 nfields;
	struct t38_field field[T38_MAX_FIELDS];
};

/* p into buf (max octets) as version (0: 1998, else 2002) has it; its
   length, or -1 if it can't be (a field without data goes last only) */
s32 t38_ifp_encode(const struct t38_ifp *p, s16 version, unsigned char *buf,
		   s32 max);
/* len octets of buf into p, its fields' data pointing into buf; 0, or -1
   if they are no IFP packet of version (or have more fields than p
   takes) */
s16 t38_ifp_decode(struct t38_ifp *p, s16 version, const unsigned char *buf,
		   s32 len);

/* n octets of in into out (they may be the same), the bits of each the
   other way round */
void t38_reverse(unsigned char *out, const unsigned char *in, s32 n);

/* UDPTL's error recovery (T38FaxUdpEC): what we send */
enum t38_ec { T38_EC_NONE, T38_EC_REDUNDANCY, T38_EC_FEC };

#define UDPTL_IFP_MAX   512             /* an IFP packet, octets */
#define UDPTL_HISTORY   16              /* the IFP packets kept to repeat,
					   or to make FEC from */
#define UDPTL_RX_WINDOW 32              /* the IFP packets kept to repair
					   with FEC (a power of 2) */

/* UDPTL, sending: each IFP packet numbered, with the ones before it
   again for redundancy, or with the FEC of the ones before (Annex C) */
struct UdptlTx {
	s16 ec;                         /* enum t38_ec */
	s16 depth;                      /* redundancy: the packets before,
					   FEC: the packets in each FEC
					   message (fec-npackets, n) */
	s16 entries;                    /* FEC: the FEC messages in a
					   packet (m), depth * entries at
					   most UDPTL_HISTORY */
	u16 seq;
	unsigned char hist[UDPTL_HISTORY][UDPTL_IFP_MAX];
	s16 hist_len[UDPTL_HISTORY];
	s16 nhist;

	/* ec (enum t38_ec) with depth packets before (redundancy, at most
	   UDPTL_HISTORY), or FEC messages of depth packets each, entries
	   of them in a packet; sequence number 0 */
	void init(s16 ec_, s16 depth_, s16 entries_ = 1);
	/* the UDPTL packet for len octets of ifp into buf (max octets, as
	   many of the packets before, or of the FEC messages, as fit); its
	   length, -1 if even the IFP packet does not */
	s32 packet(const unsigned char *ifp, s32 len, unsigned char *buf,
		   s32 max);
};

/* UDPTL, receiving: the IFP packets once each, in their order; a lost
   one from the next packet's redundancy or FEC where it has it, else
   to fn as lost: len 0, ifp NULL */
struct UdptlRx {
	s32 expected;                   /* the next sequence number, -1:
					   none yet */
	/* the IFP packets had (and made again), by sequence number modulo
	   UDPTL_RX_WINDOW: -1 for none */
	s32 win_seq[UDPTL_RX_WINDOW];
	s16 win_len[UDPTL_RX_WINDOW];
	unsigned char win[UDPTL_RX_WINDOW][UDPTL_IFP_MAX];
	/* the order of the FEC messages of a packet, when it has more than
	   one: 0 not known yet, 1 as Annex C.2.2 and Figure C.3 give it
	   (the first message from the packet just before), 2 the other
	   way round (spandsp's, Asterisk's) */
	s16 fec_order;
	s32 recovered;                  /* the IFP packets made again from
					   FEC */

	/* no packet yet */
	void init();
	/* len octets of a UDPTL packet: each IFP packet not had before to
	   fn, oldest first; -1 if it is no UDPTL packet */
	s16 packet(const unsigned char *buf, s32 len,
		   void (*fn)(void *ctx, const unsigned char *ifp, s16 len,
			      u16 seq),
		   void *ctx);
};

/* a and b the same, A to Z as a to z (SDP's values) */
inline bool t38_same_nocase(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char x = *a >= 'A' && *a <= 'Z' ? *a - 'A' + 'a' : *a;
		char y = *b >= 'A' && *b <= 'Z' ? *b - 'A' + 'a' : *b;
		if (x != y)
			return false;
	}
	return *a == *b;
}

/* T38FaxUdpEC in SDP (Annex D.2.3, Table D.2): the value an answer
   gives for the value offered (any case, NULL or "" for none): the same
   for t38UDPNoEC, t38UDPRedundancy and t38UDPFEC, which we have, else
   t38UDPRedundancy */
inline const char *t38_udp_ec_answer(const char *offered)
{
	if (offered && t38_same_nocase(offered, "t38UDPNoEC"))
		return "t38UDPNoEC";
	if (offered && t38_same_nocase(offered, "t38UDPFEC"))
		return "t38UDPFEC";
	return "t38UDPRedundancy";
}

/* the far end's T38FaxUdpEC (any case) lets us send the packets before
   again: t38UDPRedundancy, or t38UDPFEC (either scheme); not
   t38UDPNoEC or none */
inline bool t38_udp_ec_redundancy(const char *value)
{
	return value && (t38_same_nocase(value, "t38UDPRedundancy") ||
			 t38_same_nocase(value, "t38UDPFEC"));
}

/* the far end's T38FaxUdpEC (any case) is t38UDPFEC: we send FEC */
inline bool t38_udp_ec_fec(const char *value)
{
	return value && t38_same_nocase(value, "t38UDPFEC");
}

/* the error recovery we send for the far end's T38FaxUdpEC (any case,
   NULL or "" for none): enum t38_ec */
inline s16 t38_udp_ec_tx(const char *value)
{
	return t38_udp_ec_fec(value) ? T38_EC_FEC :
	       t38_udp_ec_redundancy(value) ? T38_EC_REDUNDANCY : T38_EC_NONE;
}

} // namespace dsp

#endif /* __T38_H__ */
