/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    g729_ops.h - the fixed-point arithmetic G.729 is defined in
 *
 *    G.729 and its Annex A are bit exact: a decoder must give the same
 *    samples, an encoder the same bits, as the Recommendation's
 *    fixed-point description.  That description is written in a small
 *    set of saturating 16 and 32 bit operations (ITU-T's and ETSI's
 *    "basic operators", also in G.191's STL), which these are: the same
 *    results, the same saturation, and the same overflow flag where the
 *    codec reads it (overflow(), set on saturation by the 32 bit
 *    additions, the multiplications and the left shifts; set or cleared
 *    by every 16 bit addition and subtraction).
 *
 */

#ifndef DMODEM_LINK_G729_OPS_H
#define DMODEM_LINK_G729_OPS_H


#include <cstdint>

namespace dlink {
namespace g729 {

using i16 = int16_t;
using i32 = int32_t;

constexpr i16 MAX16 = 0x7fff, MIN16 = -0x8000;
constexpr i32 MAX32 = 0x7fffffff, MIN32 = -0x7fffffff - 1;

/* the overflow flag (per thread: each codec runs on its own) */
inline bool &overflow()
{
	static thread_local bool flag = false;
	return flag;
}

/* v to 16 bits, saturated; the flag says whether it was */
inline i16 sat16(i32 v)
{
	if (v > MAX16) {
		overflow() = true;
		return MAX16;
	}
	if (v < MIN16) {
		overflow() = true;
		return MIN16;
	}
	overflow() = false;
	return static_cast<i16>(v);
}

/* a + b, saturated */
inline i16 add(i16 a, i16 b)
{
	return sat16(i32{a} + b);
}

/* a - b, saturated */
inline i16 sub(i16 a, i16 b)
{
	return sat16(i32{a} - b);
}

/* |a|, -32768 to 32767 */
inline i16 abs_s(i16 a)
{
	if (a == MIN16)
		return MAX16;
	return static_cast<i16>(a < 0 ? -a : a);
}

/* -a, -32768 to 32767 */
inline i16 negate(i16 a)
{
	return a == MIN16 ? MAX16 : static_cast<i16>(-a);
}

inline i16 shr(i16 a, i16 n);

/* a << n (n < 0: shr), saturated */
inline i16 shl(i16 a, i16 n)
{
	if (n < 0)
		return shr(a, static_cast<i16>(-n));
	i32 r = static_cast<i32>(static_cast<int64_t>(a) *
				 (int64_t{1} << (n > 31 ? 31 : n)));
	if ((n > 15 && a != 0) || r != static_cast<i16>(r)) {
		overflow() = true;
		return a > 0 ? MAX16 : MIN16;
	}
	return static_cast<i16>(r);
}

/* a >> n, arithmetic (n < 0: shl) */
inline i16 shr(i16 a, i16 n)
{
	if (n < 0)
		return shl(a, static_cast<i16>(-n));
	if (n >= 15)
		return static_cast<i16>(a < 0 ? -1 : 0);
	return static_cast<i16>(a >> n);
}

/* a * b in Q15, truncated, saturated */
inline i16 mult(i16 a, i16 b)
{
	i32 p = (i32{a} * b) >> 15;
	return sat16(p);
}

/* a * b in Q15, rounded, saturated */
inline i16 mult_r(i16 a, i16 b)
{
	i32 p = (i32{a} * b + 0x4000) >> 15;
	return sat16(p);
}

/* 2 * a * b, saturated (only -32768 * -32768) */
inline i32 L_mult(i16 a, i16 b)
{
	i32 p = i32{a} * b;
	if (p == 0x40000000) {
		overflow() = true;
		return MAX32;
	}
	return p * 2;
}

/* a + b, saturated */
inline i32 L_add(i32 a, i32 b)
{
	i32 r = static_cast<i32>(static_cast<uint32_t>(a) +
				 static_cast<uint32_t>(b));
	if (((a ^ b) & MIN32) == 0 && ((r ^ a) & MIN32)) {
		overflow() = true;
		return a < 0 ? MIN32 : MAX32;
	}
	return r;
}

/* a - b, saturated */
inline i32 L_sub(i32 a, i32 b)
{
	i32 r = static_cast<i32>(static_cast<uint32_t>(a) -
				 static_cast<uint32_t>(b));
	if (((a ^ b) & MIN32) != 0 && ((r ^ a) & MIN32)) {
		overflow() = true;
		return a < 0 ? MIN32 : MAX32;
	}
	return r;
}

/* acc + 2 * a * b */
inline i32 L_mac(i32 acc, i16 a, i16 b)
{
	return L_add(acc, L_mult(a, b));
}

/* acc - 2 * a * b */
inline i32 L_msu(i32 acc, i16 a, i16 b)
{
	return L_sub(acc, L_mult(a, b));
}

/* -a, saturated */
inline i32 L_negate(i32 a)
{
	return a == MIN32 ? MAX32 : -a;
}

/* |a|, saturated */
inline i32 L_abs(i32 a)
{
	if (a == MIN32)
		return MAX32;
	return a < 0 ? -a : a;
}

/* the high and the low 16 bits */
inline i16 extract_h(i32 a)
{
	return static_cast<i16>(a >> 16);
}

/* (see extract_h) */
inline i16 extract_l(i32 a)
{
	return static_cast<i16>(a);
}

/* a to its high 16 bits, rounded */
inline i16 round16(i32 a)
{
	return extract_h(L_add(a, 0x8000));
}

/* a << 16, and a as it is */
inline i32 L_deposit_h(i16 a)
{
	return static_cast<i32>(static_cast<uint32_t>(static_cast<uint16_t>(a))
				<< 16);
}

/* (see L_deposit_h) */
inline i32 L_deposit_l(i16 a)
{
	return a;
}

inline i32 L_shr(i32 a, i16 n);

/* a << n (n <= 0: L_shr), saturated */
inline i32 L_shl(i32 a, i16 n)
{
	if (n <= 0)
		return L_shr(a, static_cast<i16>(-n));
	for (; n > 0; n--) {
		if (a > 0x3fffffff) {
			overflow() = true;
			return MAX32;
		}
		if (a < static_cast<i32>(0xc0000000u)) {
			overflow() = true;
			return MIN32;
		}
		a *= 2;
	}
	return a;
}

/* a >> n, arithmetic (n < 0: L_shl) */
inline i32 L_shr(i32 a, i16 n)
{
	if (n < 0)
		return L_shl(a, static_cast<i16>(-n));
	if (n >= 31)
		return a < 0 ? -1 : 0;
	return a >> n;
}

/* a >> n, rounded */
inline i32 L_shr_r(i32 a, i16 n)
{
	if (n > 31)
		return 0;
	i32 r = L_shr(a, n);
	if (n > 0 && (a & (i32{1} << (n - 1))))
		r++;
	return r;
}

/* the left shifts that normalise a (0 for 0, 15 for -1) */
inline i16 norm_s(i16 a)
{
	if (a == 0)
		return 0;
	if (a == -1)
		return 15;
	if (a < 0)
		a = static_cast<i16>(~a);
	i16 n = 0;
	for (; a < 0x4000; n++)
		a = static_cast<i16>(a << 1);
	return n;
}

/* (see norm_s), 32 bits */
inline i16 norm_l(i32 a)
{
	if (a == 0)
		return 0;
	if (a == -1)
		return 31;
	if (a < 0)
		a = ~a;
	i16 n = 0;
	for (; a < 0x40000000; n++)
		a <<= 1;
	return n;
}

/* a / b in Q15, 0 <= a <= b, b > 0 (a == b: 32767) */
inline i16 div_s(i16 a, i16 b)
{
	if (a <= 0 || b <= 0 || a > b)
		return 0;
	if (a == b)
		return MAX16;
	i32 num = a, den = b;
	i16 r = 0;
	for (int i = 0; i < 15; i++) {
		r = static_cast<i16>(r << 1);
		num <<= 1;
		if (num >= den) {
			num -= den;
			r = add(r, 1);
		}
	}
	return r;
}

/* a 32 bit number as two 16 bit halves: hi, and lo the next 15 bits
   (a = hi << 16 + lo << 1) */
inline void L_Extract(i32 a, i16 *hi, i16 *lo)
{
	*hi = extract_h(a);
	*lo = extract_l(L_msu(L_shr(a, 1), *hi, 16384));
}

/* (see L_Extract) put together */
inline i32 L_Comp(i16 hi, i16 lo)
{
	return L_mac(L_deposit_h(hi), lo, 1);
}

/* the product of two numbers in L_Extract's halves */
inline i32 Mpy_32(i16 hi1, i16 lo1, i16 hi2, i16 lo2)
{
	i32 r = L_mult(hi1, hi2);
	r = L_mac(r, mult(hi1, lo2), 1);
	return L_mac(r, mult(lo1, hi2), 1);
}

/* the product of a number in halves and a 16 bit one */
inline i32 Mpy_32_16(i16 hi, i16 lo, i16 n)
{
	return L_mac(L_mult(hi, n), mult(lo, n), 1);
}

/* num / den (den in halves, num < den) in Q31 */
i32 Div_32(i32 num, i16 den_hi, i16 den_lo);
/* 2 ^ (exponent.fraction), fraction in Q15 */
i32 Pow2(i16 exponent, i16 fraction);
/* log2(x): its integer part and its fraction (Q15) */
void Log2(i32 x, i16 *exponent, i16 *fraction);
/* 1 / sqrt(x) in Q30 */
i32 Inv_sqrt(i32 x);

} // namespace g729
} // namespace dlink

#endif
