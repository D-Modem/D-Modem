/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hayes_xid.h - the Hayes private parameter sets of the V.42 XID (the
 *                  V-series Smartmodem 9600, firmware 1.4, bank 3), and
 *                  the selection of the compression that follows
 *
 *    The Hayes XID ends with the user data subfield (GI FFh, no group
 *    length).  It holds two sets, each a parameter FFh with the 3-letter
 *    name, then (PI, PL, PV) parameters (docs/hayes-details.md):
 *
 *        FF 03 41 46 54     "AFT"
 *           01 01 aa        P1: the AFT capabilities (Hayes: S44 AND S45)
 *           02 01 bb        P2: the AFT escape options (Hayes: S33)
 *        FF 03 48 4D 50     "HMP"
 *           01 01 ss        P1: the protocol selection (Hayes: S46)
 *           02 01 cc        P2: 01h with compression, else 00h
 *
 *    The results: AFT P1 is the AND of the two values, AFT P2 the OR,
 *    HMP P1 the AND (bit 1: compression).  With compression agreed, the
 *    modems use V.42bis if the XID agreed it, else Hayes ADC in the two
 *    directions (hayes_adc.h).  Without it, no compression.
 *
 *    This module knows nothing of frames: Lapm puts and parses the sets,
 *    and Link selects the compression.
 *
 */

#ifndef DMODEM_LINK_HAYES_XID_H
#define DMODEM_LINK_HAYES_XID_H


#include "dm_types.h"

namespace dlink {

/* the S46 values of a Hayes modem: LAP-M (with or without compression);
   bit 1 is the compression */
enum : u8 {
	HAYES_S46_LAPM = 136,
	HAYES_S46_LAPM_COMP = 138,
	HAYES_HMP_COMP = 0x02,
};

/* the AFT capability (AFT P1 bit 1); the AFT option bits (AFT P2, the
   OR of the two sides, which a Hayes modem copies to S34 for its AFT
   set-up): escape the control characters (confirmed), the 7-bit mode
   (probable) */
enum : u8 {
	AFT_CAP = 0x02,
	AFT_OPT_CTRL = 0x02,
	AFT_OPT_7BIT = 0x04,
};

/* the compression of a connection with a Hayes modem */
enum class HayesComp { None, V42bis, Adc };

/* the values of the two private sets, and which sets an XID had */
struct HayesSets {
	bool aft = false, hmp = false;
	u8 aft_p1 = 0, aft_p2 = 0;
	u8 hmp_p1 = 0, hmp_p2 = 0;
};

/* what the two modems agreed */
struct HayesAgreed {
	bool hayes = false;     /* both sent the HMP set */
	u8 aft_caps = 0;        /* AFT P1: the AND */
	u8 aft_opts = 0;        /* AFT P2: the OR */
	u8 hmp = 0;             /* HMP P1: the AND */
	bool aft = false;       /* AFT can be used: the AND of AFT P1 is
				   not 0 (bit 1 is the AFT capability) */
	bool comp = false;      /* HMP P1 bit 1 of the AND */
};

/* our sets: HMP P1 the S46 value s46, HMP P2 from its bit 1; the AFT
   capabilities and escape options */
HayesSets hayes_local(u8 s46, u8 aft_caps, u8 aft_opts);

/* the two sets (AFT, then HMP) put at p, in the user data subfield
   after its GI FFh; returns the end */
u8 *hayes_put_sets(u8 *p, const HayesSets &s);

/* the longest that hayes_put_sets() writes */
enum { HAYES_SETS_LEN = 2 * (5 + 3 + 3) };

/* the Hayes sets of a received XID: each (PI, PV, PL) of the user data
   subfield in order to param(); a parameter FFh with a name starts a
   set (one that is not known is ignored), a set identifier of V.44
   (40h) ends it */
class HayesSetParser {
public:
	/* one parameter of the user data subfield */
	void param(u8 pi, const u8 *pv, s16 pl);
	/* what the sets had */
	const HayesSets &sets() const { return sets_; }

private:
	enum class Set { None, Aft, Hmp };
	Set set_ = Set::None;
	HayesSets sets_;
};

/* the results of the exchange (FA 6.9): ours and the remote's sets */
HayesAgreed hayes_agree(const HayesSets &local, const HayesSets &remote);

/* the compression (FA 6.8.1): none without the compression flag;
   V.42bis if the XID agreed it (v42bis); else ADC */
HayesComp hayes_select(const HayesAgreed &a, bool v42bis);

/* the Hayes result code of the compression: 68 ADC, 67 V.42BIS,
   69 NONE */
s16 hayes_comp_result(HayesComp c);

/* the whole XID information field of a Hayes modem (FA 6.9, the
   template at bank 3 765Fh), from the format identifier on: k (S69,
   1 to 7), V.42bis P0, S46, the AFT values; returns its length (at
   most HAYES_XID_LEN) */
s16 hayes_xid_template(u8 *p, u8 k, u8 p0, u8 s46, u8 aft_caps,
		       u8 aft_opts);

/* the length of the template */
enum { HAYES_XID_LEN = 1 + 3 + 19 + 3 + 15 + 1 + HAYES_SETS_LEN };

} // namespace dlink

#endif
