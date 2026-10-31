/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    k56flex_tables.cpp - K56flex's data mode constants: per rate and digital
 *                         pad, the shell mapper's parameters, the rings'
 *                         order and the constellation (docs/dsp/k56flex-details.md)
 *
 */

#include "k56flex.h"

namespace dsp {

/* per rate (32000 + 2000 i bit/s): the shell mapper's K, the shaping
   period r, u (u - 1 bits a symbol within its ring) and the rings M per
   pad (0, 3, 6 dB) */
const K56Rate k56_rates[K56_NRATES] = {
	{ 28,  2, 1, { 13, 13, 13 } },   /* 32000 bit/s */
	{ 30,  2, 1, { 15, 15, 15 } },   /* 34000 bit/s */
	{ 24,  2, 2, {  9,  9,  9 } },   /* 36000 bit/s */
	{ 26,  2, 2, { 11, 11, 11 } },   /* 38000 bit/s */
	{ 26,  4, 2, { 11, 11, 11 } },   /* 40000 bit/s */
	{ 28,  4, 2, { 13, 13, 13 } },   /* 42000 bit/s */
	{ 30,  4, 2, { 15, 15, 15 } },   /* 44000 bit/s */
	{ 24,  4, 3, {  9,  9,  9 } },   /* 46000 bit/s */
	{ 25,  8, 3, { 10, 10, 10 } },   /* 48000 bit/s */
	{ 27,  8, 3, { 11, 11, 11 } },   /* 50000 bit/s */
	{ 29,  8, 3, { 13, 14, 13 } },   /* 52000 bit/s */
	{ 31,  8, 3, { 16, 16, 16 } },   /* 54000 bit/s */
	{ 25,  8, 4, {  9, 10, 10 } },   /* 56000 bit/s */
};

/* robbed bits a frame for 0 to 6 robbed symbols in 6 */
const unsigned char k56_rbs_loss[7] = { 0, 2, 4, 4, 6, 8, 8 };

/* the ring (a shell mapper output, cheapest first) -> its group of
   the constellation, per pad (0, 3, 6 dB) and rate */
static const unsigned char ring_0_32[] = {
	2, 3, 4, 5, 6, 7, 1, 8, 0, 10, 9, 11, 12 };
static const unsigned char ring_0_34[] = {
	0, 1, 3, 4, 5, 7, 2, 6, 8, 9, 10, 11, 13, 12, 14 };
static const unsigned char ring_0_36[] = {
	0, 5, 4, 3, 2, 1, 6, 7, 8 };
static const unsigned char ring_0_38[] = {
	10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
static const unsigned char ring_0_40[] = {
	10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
static const unsigned char ring_0_42[] = {
	9, 8, 7, 6, 5, 10, 4, 3, 2, 1, 0, 11, 12 };
static const unsigned char ring_0_44[] = {
	6, 5, 4, 3, 2, 1, 7, 0, 9, 8, 10, 11, 12, 13, 14 };
static const unsigned char ring_0_46[] = {
	3, 2, 4, 1, 5, 0, 6, 7, 8 };
static const unsigned char ring_0_48[] = {
	9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
static const unsigned char ring_0_50[] = {
	9, 8, 7, 6, 5, 4, 3, 2, 1, 10, 0 };
static const unsigned char ring_0_52[] = {
	3, 5, 4, 2, 1, 0, 7, 6, 8, 9, 10, 11, 12 };
static const unsigned char ring_0_54[] = {
	10, 9, 8, 12, 11, 13, 7, 6, 5, 14, 4, 3, 2, 1, 0, 15 };
static const unsigned char ring_0_56[] = {
	1, 2, 3, 4, 0, 6, 5, 7, 8 };
static const unsigned char ring_1_32[] = {
	2, 3, 4, 5, 6, 7, 1, 8, 0, 10, 9, 11, 12 };
static const unsigned char ring_1_34[] = {
	0, 1, 2, 5, 6, 7, 9, 8, 4, 3, 10, 11, 12, 13, 14 };
static const unsigned char ring_1_36[] = {
	2, 3, 4, 5, 6, 1, 0, 7, 8 };
static const unsigned char ring_1_38[] = {
	8, 7, 6, 5, 4, 3, 2, 9, 1, 0, 10 };
static const unsigned char ring_1_40[] = {
	8, 7, 6, 5, 4, 3, 2, 9, 1, 0, 10 };
static const unsigned char ring_1_42[] = {
	5, 4, 3, 2, 1, 0, 6, 7, 8, 9, 11, 10, 12 };
static const unsigned char ring_1_44[] = {
	1, 5, 6, 7, 8, 4, 3, 2, 9, 0, 10, 12, 11, 13, 14 };
static const unsigned char ring_1_46[] = {
	7, 6, 5, 4, 3, 2, 1, 0, 8 };
static const unsigned char ring_1_48[] = {
	5, 4, 3, 6, 2, 1, 0, 7, 8, 9 };
static const unsigned char ring_1_50[] = {
	5, 4, 3, 6, 2, 1, 0, 7, 8, 9, 10 };
static const unsigned char ring_1_52[] = {
	7, 8, 6, 5, 4, 3, 2, 1, 0, 9, 10, 11, 12, 13 };
static const unsigned char ring_1_54[] = {
	6, 7, 8, 9, 12, 11, 10, 14, 13, 5, 4, 3, 2, 1, 0, 15 };
static const unsigned char ring_1_56[] = {
	3, 4, 6, 5, 7, 8, 2, 1, 9, 0 };
static const unsigned char ring_2_32[] = {
	2, 3, 4, 5, 6, 7, 1, 8, 0, 10, 9, 11, 12 };
static const unsigned char ring_2_34[] = {
	0, 1, 3, 4, 5, 6, 7, 2, 8, 9, 11, 10, 12, 13, 14 };
static const unsigned char ring_2_36[] = {
	0, 5, 4, 3, 2, 1, 6, 7, 8 };
static const unsigned char ring_2_38[] = {
	10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
static const unsigned char ring_2_40[] = {
	10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
static const unsigned char ring_2_42[] = {
	9, 8, 7, 6, 5, 10, 4, 3, 2, 1, 0, 11, 12 };
static const unsigned char ring_2_44[] = {
	6, 5, 4, 3, 2, 1, 7, 0, 8, 9, 10, 11, 12, 13, 14 };
static const unsigned char ring_2_46[] = {
	2, 3, 4, 1, 0, 6, 5, 7, 8 };
static const unsigned char ring_2_48[] = {
	9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
static const unsigned char ring_2_50[] = {
	9, 8, 7, 6, 5, 4, 3, 2, 1, 10, 0 };
static const unsigned char ring_2_52[] = {
	3, 5, 6, 4, 7, 2, 1, 0, 8, 9, 10, 11, 12 };
static const unsigned char ring_2_54[] = {
	8, 10, 9, 11, 12, 13, 7, 6, 14, 5, 4, 3, 2, 1, 0, 15 };
static const unsigned char ring_2_56[] = {
	6, 5, 4, 8, 7, 3, 9, 2, 1, 0 };
const unsigned char *const k56_ring[3][K56_NRATES] = {
	{ ring_0_32, ring_0_34, ring_0_36, ring_0_38, ring_0_40, ring_0_42, ring_0_44, ring_0_46, ring_0_48, ring_0_50, ring_0_52, ring_0_54, ring_0_56 },
	{ ring_1_32, ring_1_34, ring_1_36, ring_1_38, ring_1_40, ring_1_42, ring_1_44, ring_1_46, ring_1_48, ring_1_50, ring_1_52, ring_1_54, ring_1_56 },
	{ ring_2_32, ring_2_34, ring_2_36, ring_2_38, ring_2_40, ring_2_42, ring_2_44, ring_2_46, ring_2_48, ring_2_50, ring_2_52, ring_2_54, ring_2_56 },
};

/* the constellation: per point (group << (u - 1) | bits) its
   magnitude, a mu-law code index (0 the smallest), per pad (0, 3, 6 dB) and rate */
static const unsigned char bios_0_32[] = {
	24, 44, 54, 63, 68, 73, 78, 81, 84, 87, 90, 93, 96 };
static const unsigned char bios_0_34[] = {
	24, 44, 54, 62, 67, 71, 75, 79, 82, 84, 86, 88, 90, 92, 94 };
static const unsigned char bios_0_36[] = {
	20, 38, 49, 55, 61, 66, 69, 72, 75, 78, 81, 83, 85, 87, 89, 91, 93, 95 };
static const unsigned char bios_0_38[] = {
	16, 32, 40, 48, 52, 56, 60, 64, 66, 68, 70, 72, 74, 76, 78, 80, 81, 82, 83, 84, 85, 86 };
static const unsigned char bios_0_40[] = {
	16, 32, 40, 48, 52, 56, 60, 64, 66, 68, 70, 72, 74, 76, 78, 80, 81, 82, 83, 84, 85, 86 };
static const unsigned char bios_0_42[] = {
	16, 32, 40, 48, 52, 56, 60, 64, 66, 68, 70, 72, 74, 76, 78, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90 };
static const unsigned char bios_0_44[] = {
	16, 32, 40, 48, 52, 56, 60, 64, 66, 68, 70, 72, 74, 76, 78, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94 };
static const unsigned char bios_0_46[] = {
	10, 23, 33, 38, 43, 48, 51, 54, 57, 60, 63, 65, 67, 69, 71, 73, 75, 77, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96 };
static const unsigned char bios_0_48[] = {
	8, 20, 28, 34, 38, 42, 46, 49, 51, 53, 55, 57, 59, 61, 63, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89 };
static const unsigned char bios_0_50[] = {
	8, 20, 28, 34, 38, 42, 46, 49, 51, 53, 55, 57, 59, 61, 63, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93 };
static const unsigned char bios_0_52[] = {
	6, 17, 23, 29, 34, 37, 40, 43, 46, 49, 51, 53, 55, 57, 59, 61, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98 };
static const unsigned char bios_0_54[] = {
	4, 12, 18, 22, 26, 30, 33, 35, 37, 39, 41, 43, 45, 47, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98 };
static const unsigned char bios_0_56[] = {
	3, 8, 12, 16, 18, 20, 22, 25, 28, 31, 33, 35, 37, 39, 41, 43, 45, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101 };
static const unsigned char bios_1_32[] = {
	30, 51, 62, 69, 76, 81, 85, 88, 92, 96, 98, 100, 103 };
static const unsigned char bios_1_34[] = {
	30, 50, 61, 68, 73, 79, 82, 85, 88, 91, 93, 96, 98, 100, 103 };
static const unsigned char bios_1_36[] = {
	24, 44, 54, 62, 67, 72, 76, 80, 82, 84, 87, 89, 92, 95, 97, 98, 100, 103 };
static const unsigned char bios_1_38[] = {
	20, 37, 48, 54, 59, 65, 68, 72, 74, 77, 80, 81, 84, 87, 88, 89, 92, 93, 96, 97, 98, 99 };
static const unsigned char bios_1_40[] = {
	20, 37, 48, 54, 59, 65, 68, 72, 74, 77, 80, 81, 84, 87, 88, 89, 92, 93, 96, 97, 98, 99 };
static const unsigned char bios_1_42[] = {
	20, 37, 48, 54, 59, 65, 68, 72, 74, 77, 80, 81, 84, 87, 88, 89, 92, 93, 96, 97, 98, 99, 100, 101, 103, 104 };
static const unsigned char bios_1_44[] = {
	16, 33, 43, 50, 55, 59, 64, 66, 68, 70, 73, 76, 79, 81, 83, 84, 87, 88, 89, 91, 92, 93, 96, 97, 98, 99, 100, 101, 103, 104 };
static const unsigned char bios_1_46[] = {
	12, 25, 34, 40, 45, 50, 53, 57, 59, 62, 65, 67, 69, 72, 73, 74, 76, 77, 80, 81, 82, 83, 84, 85, 87, 88, 89, 91, 92, 93, 96, 97, 98, 99, 100, 101 };
static const unsigned char bios_1_48[] = {
	12, 25, 34, 40, 45, 50, 53, 57, 59, 62, 65, 67, 69, 72, 73, 74, 76, 77, 80, 81, 82, 83, 84, 85, 87, 88, 89, 91, 92, 93, 96, 97, 98, 99, 100, 101, 103, 104, 105, 107 };
static const unsigned char bios_1_50[] = {
	8, 21, 29, 35, 40, 44, 48, 50, 52, 55, 58, 61, 64, 65, 68, 69, 70, 72, 73, 74, 76, 77, 80, 81, 82, 83, 84, 85, 87, 88, 89, 91, 92, 93, 96, 97, 98, 99, 100, 101, 103, 104, 105, 107 };
static const unsigned char bios_1_52[] = {
	6, 16, 22, 28, 33, 36, 40, 43, 45, 48, 50, 52, 54, 55, 58, 59, 62, 64, 65, 66, 67, 68, 69, 70, 72, 73, 74, 76, 77, 79, 80, 81, 82, 83, 84, 85, 87, 88, 89, 91, 92, 93, 96, 97, 98, 99, 100, 101, 103, 104, 105, 107, 108, 110, 111, 112 };
static const unsigned char bios_1_54[] = {
	4, 10, 15, 18, 22, 25, 28, 30, 33, 35, 37, 38, 40, 41, 44, 45, 48, 49, 50, 51, 52, 53, 54, 55, 58, 59, 62, 64, 65, 66, 67, 68, 69, 70, 72, 73, 74, 76, 77, 79, 80, 81, 82, 83, 84, 85, 87, 88, 89, 91, 92, 93, 96, 97, 98, 99, 100, 101, 103, 104, 105, 107, 108, 110 };
static const unsigned char bios_1_56[] = {
	2, 4, 7, 10, 13, 15, 17, 18, 21, 22, 23, 25, 26, 28, 29, 30, 32, 33, 34, 35, 36, 37, 38, 40, 41, 43, 44, 45, 48, 49, 50, 51, 52, 53, 54, 55, 58, 59, 62, 64, 65, 66, 67, 68, 69, 70, 72, 73, 74, 76, 77, 79, 80, 81, 82, 83, 84, 85, 87, 88, 89, 91, 92, 93, 96, 97, 98, 99, 100, 101, 103, 104, 105, 107, 108, 110, 111, 112, 113, 114 };
static const unsigned char bios_2_32[] = {
	36, 58, 69, 78, 83, 88, 93, 97, 100, 103, 106, 109, 112 };
static const unsigned char bios_2_34[] = {
	36, 58, 69, 77, 82, 86, 90, 94, 98, 100, 102, 104, 106, 108, 110 };
static const unsigned char bios_2_36[] = {
	32, 52, 64, 70, 76, 81, 84, 87, 90, 93, 97, 99, 101, 103, 105, 107, 109, 111 };
static const unsigned char bios_2_38[] = {
	24, 44, 54, 62, 67, 71, 75, 79, 81, 83, 85, 87, 89, 91, 93, 96, 97, 98, 99, 100, 101, 102 };
static const unsigned char bios_2_40[] = {
	24, 44, 54, 62, 67, 71, 75, 79, 81, 83, 85, 87, 89, 91, 93, 96, 97, 98, 99, 100, 101, 102 };
static const unsigned char bios_2_42[] = {
	24, 44, 54, 62, 67, 71, 75, 79, 81, 83, 85, 87, 89, 91, 93, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106 };
static const unsigned char bios_2_44[] = {
	24, 44, 54, 62, 67, 71, 75, 79, 81, 83, 85, 87, 89, 91, 93, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110 };
static const unsigned char bios_2_46[] = {
	18, 35, 46, 52, 57, 62, 66, 69, 72, 75, 78, 80, 82, 84, 86, 88, 90, 92, 94, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112 };
static const unsigned char bios_2_48[] = {
	16, 32, 40, 48, 52, 56, 60, 64, 66, 68, 70, 72, 74, 76, 78, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105 };
static const unsigned char bios_2_50[] = {
	16, 32, 40, 48, 52, 56, 60, 64, 66, 68, 70, 72, 74, 76, 78, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109 };
static const unsigned char bios_2_52[] = {
	12, 26, 35, 41, 48, 51, 54, 57, 60, 64, 66, 68, 70, 72, 74, 76, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114 };
static const unsigned char bios_2_54[] = {
	8, 20, 28, 34, 38, 42, 46, 49, 51, 53, 55, 57, 59, 61, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114 };
static const unsigned char bios_2_56[] = {
	4, 12, 18, 22, 26, 30, 33, 35, 37, 39, 41, 43, 46, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116 };
const unsigned char *const k56_bios[3][K56_NRATES] = {
	{ bios_0_32, bios_0_34, bios_0_36, bios_0_38, bios_0_40, bios_0_42, bios_0_44, bios_0_46, bios_0_48, bios_0_50, bios_0_52, bios_0_54, bios_0_56 },
	{ bios_1_32, bios_1_34, bios_1_36, bios_1_38, bios_1_40, bios_1_42, bios_1_44, bios_1_46, bios_1_48, bios_1_50, bios_1_52, bios_1_54, bios_1_56 },
	{ bios_2_32, bios_2_34, bios_2_36, bios_2_38, bios_2_40, bios_2_42, bios_2_44, bios_2_46, bios_2_48, bios_2_50, bios_2_52, bios_2_54, bios_2_56 },
};

} // namespace dsp
