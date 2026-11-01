/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    g729a.h - ITU-T G.729 Annex A, the 8 kbit/s speech coder of DSVD's
 *              voice channel (V.70)
 *
 *    D-Modem's own G.729 Annex A: a 10 ms frame of 80 samples (8000
 *    samples a second, 16 bit linear) to 80 bits and back.  G.729 is
 *    defined bit exact by its fixed-point description, so this follows
 *    that description's arithmetic step for step (g729_ops.h); it is
 *    checked against the Recommendation's test vectors (ctest's
 *    g729a_vectors, with the vectors where specs/fetch.sh puts them)
 *    and against bcg729, another implementation, where that is found
 *    (g729a_bcg729, a test only).
 *
 *    The frame's 11 parameters (Table 8/G.729) go into its 10 octets as
 *    RFC 3551 4.5.6 and H.245 have them: L0 L1 L2 L3 P1 P0 C1 S1 GA1
 *    GB1 P2 C2 S2 GA2 GB2, each most significant bit first, the first
 *    bit in the first octet's most significant bit.
 *
 */
#ifndef DMODEM_LINK_G729A_H
#define DMODEM_LINK_G729A_H

#include <cstdint>
#include <memory>

namespace dlink {
namespace g729 {

enum {
	FRAME = 80,             /* samples in a frame (10 ms) */
	FRAME_OCTETS = 10,      /* its 80 bits */
	PRMS = 11,              /* its parameters */
	ORDER = 10,             /* LP order (M) */
};

struct EncState;
struct DecState;

/* the frame's parameters to its octets (see above) */
void pack(const int16_t prm[PRMS], uint8_t out[FRAME_OCTETS]);
/* (see pack) the other way */
void unpack(const uint8_t in[FRAME_OCTETS], int16_t prm[PRMS]);

/* the encoder (clause 3/G.729 with Annex A's changes) */
class Encoder {
public:
	Encoder();
	~Encoder();
	/* a new call: as the Recommendation's encoder starts */
	void reset();
	/* a frame of speech to its parameters */
	void encode(const int16_t pcm[FRAME], int16_t prm[PRMS]);
	/* (see encode) to its octets */
	void encode(const int16_t pcm[FRAME], uint8_t out[FRAME_OCTETS]);

private:
	std::unique_ptr<EncState> st_;
};

/* the decoder with its postfilter (clause 4/G.729 with Annex A's) */
class Decoder {
public:
	Decoder();
	~Decoder();
	/* a new call */
	void reset();
	/* a frame's parameters to speech; erased: the frame was lost or is
	   bad (4.4/G.729: it is concealed) */
	void decode(const int16_t prm[PRMS], bool erased, int16_t pcm[FRAME]);
	/* (see decode) from its octets */
	void decode(const uint8_t in[FRAME_OCTETS], int16_t pcm[FRAME]);
	/* a lost frame (see decode) */
	void erase(int16_t pcm[FRAME]);

private:
	std::unique_ptr<DecState> st_;
};

} // namespace g729
} // namespace dlink

#endif
