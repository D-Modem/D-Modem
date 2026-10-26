/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    agere_patch.h - the Agere USB modem's ROM patches (docs/agere-usb.md),
 *                    known by their length and hashes only: the patches
 *                    themselves are Agere's, in Agere's drivers, and are
 *                    taken from the user's own copy (usb_agere.cpp)
 *
 *    They are byte for byte the same in every driver of the package
 *    (agrsm 2.1.73's agrmodemlib.o and agrmodem.ko, AGRSM.sys and
 *    AGRSM64.sys from 2.1.70 to 2.2.98), with no relocations: found by a
 *    hash of their first PATCH_HEAD octets, then checked whole.
 *
 *    The tests' patches (DMODEM_AGERE_TEST_PATCHES=1, fake_agere.cpp)
 *    stand for them: the same lengths, octets of test_patch().
 *
 */
#ifndef DMODEMD_AGERE_PATCH_H
#define DMODEMD_AGERE_PATCH_H

#include <stddef.h>
#include <stdint.h>

namespace agere {

/* the patches: the bulk endpoints' for ROM 1.0.6, for 1.2.1 in flash
   and in ROM, and the clock change command's for 1.0.6 */
enum { V106, V121_FLASH, V121_ROM, ASYNC_CLOCKCHANGE, PATCHES };

/* a patch: its name, its length, the hashes of its first PATCH_HEAD
   octets and of the whole of it */
struct Patch {
	const char *name;
	uint32_t len;
	uint64_t head, all;
};

const uint32_t PATCH_HEAD = 16;

/* Agere's (agrmodemlib.o's bulk_patch_v106, bulk_patch_v121_flash,
   bulk_patch_v121_rom, async_clockchange) */
inline const Patch &known(int i)
{
	static const Patch p[PATCHES] = {
		{ "bulk_patch_v106", 748, 0xc2d225a28ef5d54dull,
		  0xfa9c07ba99a056d6ull },
		{ "bulk_patch_v121_flash", 748, 0x791c45d505eb3989ull,
		  0xa32b91c355aa4585ull },
		{ "bulk_patch_v121_rom", 1156, 0xbb5e44065300f78full,
		  0x20fb59b2283b5167ull },
		{ "async_clockchange", 508, 0x0d0be8aef4cdc205ull,
		  0x418fb8640f1e4b23ull },
	};
	return p[i];
}

/* FNV-1a, 64 bits, of n octets */
inline uint64_t fnv(const uint8_t *b, size_t n)
{
	uint64_t h = 0xcbf29ce484222325ull;
	for (size_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 0x100000001b3ull;
	return h;
}

/* octet k of the tests' patch i (patch i's length) */
inline uint8_t test_patch(int i, uint32_t k)
{
	return static_cast<uint8_t>(k * 31u + static_cast<uint32_t>(i) * 77u + 5u +
				    (k >> 8));
}

} // namespace agere

#endif
