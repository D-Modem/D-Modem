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
 *    Annex B (silence compression) is here too, for Annex A as
 *    G.729 Annex B's "A+B" reference has it: with DTX on, the encoder's
 *    voice activity detector finds the frames with no speech, and for
 *    those sends a 15 bit SID frame (in 2 octets, RFC 3551's, its last
 *    bit 0) when the background noise changes, and nothing otherwise.
 *    The decoder makes comfort noise from the SID frames.
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
	SID_PRMS = 4,           /* a SID frame's parameters (Annex B) */
	SID_OCTETS = 2,         /* its 15 bits and a 0 */
};

/* a frame's type (Annex B) */
enum Type {
	NONE = 0,               /* nothing sent: silence goes on */
	SPEECH = 1,
	SID = 2,                /* silence: the noise's description */
};

struct EncState;
struct DecState;

/* the frame's parameters to its octets (see above) */
void pack(const int16_t prm[PRMS], uint8_t out[FRAME_OCTETS]);
/* (see pack) the other way */
void unpack(const uint8_t in[FRAME_OCTETS], int16_t prm[PRMS]);
/* a SID frame's parameters (Table B.2/G.729: the LSF predictor 1 bit,
   the stages 5 and 4, the gain 5) to its octets, and back */
void pack_sid(const int16_t prm[SID_PRMS], uint8_t out[SID_OCTETS]);
/* (see pack_sid) */
void unpack_sid(const uint8_t in[SID_OCTETS], int16_t prm[SID_PRMS]);

/* the encoder (clause 3/G.729 with Annex A's changes) */
class Encoder {
public:
	Encoder();
	~Encoder();
	/* a new call: as the Recommendation's encoder starts (DTX as it
	   was) */
	void reset();
	/* Annex B's VAD and DTX on (off at first); from a reset, as the
	   reference's encoder with its VAD flag 1 */
	void set_dtx(bool on);
	/* a frame of speech to its parameters; its type: SPEECH (always
	   without DTX), SID (SID_PRMS parameters) or NONE */
	int encode(const int16_t pcm[FRAME], int16_t prm[PRMS]);
	/* (see encode) to its octets; how many: FRAME_OCTETS, SID_OCTETS
	   or 0 */
	int encode(const int16_t pcm[FRAME], uint8_t out[FRAME_OCTETS]);

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
	/* (see decode) a frame of a type (Annex B): SPEECH, SID (prm its
	   SID_PRMS) or NONE (prm not read): comfort noise */
	void decode(int type, const int16_t *prm, bool erased,
		    int16_t pcm[FRAME]);
	/* (see decode) from its octets */
	void decode(const uint8_t in[FRAME_OCTETS], int16_t pcm[FRAME]);
	/* (see decode) from n octets: FRAME_OCTETS a speech frame,
	   SID_OCTETS a SID frame, 0 none sent */
	void decode(const uint8_t *in, int n, int16_t pcm[FRAME]);
	/* a lost frame (see decode): concealed speech after speech, comfort
	   noise after silence */
	void erase(int16_t pcm[FRAME]);

private:
	std::unique_ptr<DecState> st_;
};

} // namespace g729
} // namespace dlink

#endif
