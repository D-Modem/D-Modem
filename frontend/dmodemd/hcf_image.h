/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hcf_image.h - the code a Conexant (Rockwell) HCF modem's DSP is
 *                  given by the host: its "DRAM" images, and where they
 *                  are in Conexant's Windows driver (WINACUSB.sys)
 *
 *    An HCF modem's data pump runs in its DSP, from mask ROM and from
 *    RAM code the host downloads at start-up: modules, each loaded at
 *    its own address, in the CTL memory (octets) or the SPX one (words
 *    of four octets).  The code is Conexant's and not ours to ship: it
 *    is read from the user's own copy of their driver.
 *
 *    An image ("DRAM" format, as the driver's DP_ReadDownloadFile reads
 *    it): 8 octets ("DRAM", a version, the module count, little-endian
 *    16 bits at 6); then for each module a record of 0x1c octets and
 *    its name (NUL-ended); then the code.  The record: 16 bits its id,
 *    two 16-bit words, 16 bits whose low octet is its checksum, 32 bits
 *    its load address, 32 bits its end, 32 bits its length (in units
 *    of the memory), 32 bits its code's offset from the image's start,
 *    and 32 bits more.
 *
 *    The driver has an image pair (CTL, SPX) for each DSP: L8371, L8373
 *    and L8374 (the chip's ID word "17", "37", "47").  WINACUSB.sys keeps
 *    its COFF symbols (_CTL_DATA_L8371DP, _CTL_DATA_L8371DP_SIZE, ...):
 *    the images are found by name.
 */
#ifndef DMODEMD_HCF_IMAGE_H
#define DMODEMD_HCF_IMAGE_H

#include <cstdint>
#include <string>
#include <vector>

namespace hcf {

/* one module of an image */
struct Module {
	uint16_t id = 0;
	uint8_t checksum = 0;           /* what the DSP reports once loaded */
	uint32_t address = 0;           /* its load address */
	uint32_t octets = 0;            /* its length in octets */
	uint32_t offset = 0;            /* its code's, from the image's start */
	std::string name;
};

/* an image: its octets and its modules */
struct Image {
	std::vector<uint8_t> data;
	std::vector<Module> modules;
	unsigned unit = 1;              /* octets in a unit: 1 CTL, 4 SPX */

	/* the module with id, nullptr if none */
	const Module *find(uint16_t id) const;
	/* its code */
	const uint8_t *code(const Module &m) const
	{
		return data.data() + m.offset;
	}
};

/* the image in data (unit octets in a unit of its memory): true, or
   false with why in err */
bool parse(std::vector<uint8_t> data, unsigned unit, Image &img,
	   std::string &err);

/* the DSP's two images */
struct Images {
	Image ctl, spx;
};

/* the images for chip (its ID word: 0x3731 L8371, 0x3733 L8373, 0x3734
   L8374) from Conexant's driver at path (a PE with its COFF symbols):
   true, or false with why in err */
bool from_driver(const std::string &path, uint16_t chip, Images &imgs,
		 std::string &err);

/* the images from two files that hold one image each (the tests',
   and the images saved out of a driver): true, or false with why */
bool from_files(const std::string &ctl, const std::string &spx,
		Images &imgs, std::string &err);

/* the chip's name ("L8371"), "?" if unknown */
const char *chip_name(uint16_t chip);

/* one module to load: its id, and whether it is the SPX's */
struct Load {
	uint16_t id;
	bool spx;
};

/* the modules the driver loads (DP_DnldNoNEWCNoDelay) for set (1 at
   the start, then 2 or 3) on chip, in order; empty if none (or an
   unknown chip) */
std::vector<Load> download_set(uint16_t chip, int set);

} // namespace hcf

#endif
