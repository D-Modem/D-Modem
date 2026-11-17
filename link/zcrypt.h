/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    zcrypt.h - ZyXEL's data encryption (the Elite 2864I's S102 and
 *               ATCK): the key from ATCK's text, PKDS (a 128-bit
 *               Diffie-Hellman exchange), the DSP's key check, the IV,
 *               and DES or two-key triple DES in 64-bit CFB
 *
 *    From the Elite 2864I 1.11 firmware (firmware-trace.md, section 8 of
 *    the ZyXEL notes, and docs/zyxel-des.md).  A key here is the
 *    firmware's 8 bytes: 56 key bits packed in bytes 0 to 6 (byte 7 is
 *    not used), each 7 bits one byte of a standard DES key without its
 *    parity bit.
 *
 */

#ifndef DMODEM_LINK_ZCRYPT_H
#define DMODEM_LINK_ZCRYPT_H


#include <cstddef>

#include "dm_types.h"

namespace dlink {

enum {
	/* S102's bits */
	ZDES_ON = 0x01,         /* encryption offered */
	ZDES_TRIPLE = 0x02,     /* triple DES */
	ZDES_NO_PKDS = 0x04,    /* the ATCK key, no key exchange */
	ZDES_REQUIRED = 0x08,   /* no agreement: disconnect */
	ZDES_KEY = 16,          /* ATCK's two registers, A and B */
	ZDES_PKDS = 16,         /* a PKDS value: 128 bits, big-endian */
};

/* ATCK's key from its text (the characters between '<' and '>'):
   key[0..7] register A, key[8..15] register B */
void zdes_atck(const char *text, size_t n, u8 key[ZDES_KEY]);
/* the key a modem has at power-on (A; B the same) */
void zdes_default_key(u8 key[ZDES_KEY]);

/* PKDS: our public value y = g^x mod p from the secret x (any 16
   bytes: taken modulo p, 0 made 1) */
void zdes_public(const u8 x[ZDES_PKDS], u8 y[ZDES_PKDS]);
/* the shared value: y_remote^x mod p */
void zdes_shared(const u8 y_remote[ZDES_PKDS], const u8 x[ZDES_PKDS],
		 u8 k[ZDES_PKDS]);

/* the DSP's key check: the weak keys of its table changed (by its LFSR)
   until neither key is one, and byte 7 of each key 0.  two: both keys
   (triple DES, or a PKDS key), else A alone (B as it was) */
void zdes_key_check(u8 key[ZDES_KEY], bool two);

/* the IV from A and B: A's first 32 bits less B's, and A's last 32 bits
   plus B's (big-endian, modulo 2^32) */
void zdes_iv(const u8 key[ZDES_KEY], u8 iv[8]);

/* DES (FIPS 46-3) on one block, with the firmware's packed key */
class Des {
public:
	void set_key(const u8 key[8]);
	void encrypt(const u8 in[8], u8 out[8]) const;
	void decrypt(const u8 in[8], u8 out[8]) const;

private:
	void crypt(const u8 in[8], u8 out[8], bool dec) const;
	u64 sub_[16] = {};
};

/* one direction of the cipher: 64-bit CFB over DES (one key) or triple
   DES (E-D-E with A, B, A); its register goes on from frame to frame.
   A frame's last n < 8 octets take the first n of the keystream, and
   the register moves n octets to the left with the n cipher octets
   after it */
class ZCfb {
public:
	void init(const u8 key[ZDES_KEY], bool triple, const u8 iv[8]);
	void encrypt(u8 *p, size_t n);
	void decrypt(u8 *p, size_t n);

private:
	void run(u8 *p, size_t n, bool dec);
	void block(const u8 in[8], u8 out[8]) const;
	Des k1_, k2_;
	bool triple_ = false;
	u8 reg_[8] = {};
};

} // namespace dlink

#endif
