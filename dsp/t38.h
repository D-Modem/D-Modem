/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    t38.h - ITU-T T.38 fax over IP: the IFP packets (Annex A) in the
 *            encoding of the 1998 version (T38FaxVersion 0) and of the
 *            2002 one (1 to 3), and their UDPTL transport (sequence
 *            numbers, and the packets before again for redundancy)
 *
 *    The octets of HDLC and T.4 data are the line's bits with the first
 *    one in the most significant bit (T.38 7.1.2), not in the lowest as
 *    T.30's frames are written; t38_reverse() turns one into the other.
 *
 */

#ifndef __T38_H__
#define __T38_H__

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
	int type;                       /* enum t38_field_type */
	const unsigned char *data;      /* len octets, NULL for none */
	int len;
};

/* an IFP packet: an indicator, or data of a modulation in fields */
struct t38_ifp {
	int data;                       /* t30-data, else t30-indicator */
	int type;                       /* the indicator, or the data's
					   modulation */
	int nfields;
	struct t38_field field[T38_MAX_FIELDS];
};

/* p into buf (max octets) as version (0: 1998, else 2002) has it; its
   length, or -1 if it can't be (a field without data goes last only) */
int t38_ifp_encode(const struct t38_ifp *p, int version, unsigned char *buf,
		   int max);
/* len octets of buf into p, its fields' data pointing into buf; 0, or -1
   if they are no IFP packet of version (or have more fields than p
   takes) */
int t38_ifp_decode(struct t38_ifp *p, int version, const unsigned char *buf,
		   int len);

/* n octets of in into out (they may be the same), the bits of each the
   other way round */
void t38_reverse(unsigned char *out, const unsigned char *in, int n);

/* UDPTL's error recovery (T38FaxUdpEC): what we send */
enum t38_ec { T38_EC_NONE, T38_EC_REDUNDANCY };

#define UDPTL_IFP_MAX   512             /* an IFP packet, octets */
#define UDPTL_HISTORY   8               /* the IFP packets kept to repeat */

/* UDPTL, sending: each IFP packet numbered, with the ones before it
   again for redundancy */
struct UdptlTx {
	int ec;                         /* enum t38_ec */
	int depth;                      /* the packets before, at most
					   UDPTL_HISTORY */
	unsigned seq;
	unsigned char hist[UDPTL_HISTORY][UDPTL_IFP_MAX];
	int hist_len[UDPTL_HISTORY];
	int nhist;

	void init(int ec_, int depth_);
	/* the UDPTL packet for len octets of ifp into buf (max octets, as
	   many of the packets before as fit); its length, -1 if even the
	   IFP packet does not */
	int packet(const unsigned char *ifp, int len, unsigned char *buf,
		   int max);
};

/* UDPTL, receiving: the IFP packets once each, in their order; a lost
   one from the next packet's redundancy where it has it (FEC: the
   primary packets only) */
struct UdptlRx {
	long expected;                  /* the next sequence number, -1:
					   none yet */

	void init();
	/* len octets of a UDPTL packet: each IFP packet not had before to
	   fn, oldest first; -1 if it is no UDPTL packet */
	int packet(const unsigned char *buf, int len,
		   void (*fn)(void *ctx, const unsigned char *ifp, int len,
			      unsigned seq),
		   void *ctx);
};

} // namespace dsp

#endif /* __T38_H__ */
