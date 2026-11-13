/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hcf_image.cpp - an HCF modem's DSP code: its "DRAM" images, read
 *                    from Conexant's Windows driver or from files
 *                    (hcf_image.h)
 */
#include "hcf_image.h"

#include <cstdio>
#include <cstring>
#include <map>

namespace hcf {

namespace {

/* the little-endian 16 bits at p */
uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
/* the little-endian 32 bits at p */
uint32_t u32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
		(uint32_t)p[3] << 24;
}

/* the whole of the file at path into out: true, or false with why */
bool read_file(const std::string &path, std::vector<uint8_t> &out,
	       std::string &err)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) {
		err = path + ": cannot be read";
		return false;
	}
	out.clear();
	uint8_t b[65536];
	size_t n;
	while ((n = fread(b, 1, sizeof b, f)) > 0)
		out.insert(out.end(), b, b + n);
	fclose(f);
	return true;
}

/* a PE's sections and COFF symbols, as far as the images need them */
struct Pe {
	struct Section {
		uint32_t va, size, raw, raw_size;
	};
	std::vector<Section> sections;
	std::map<std::string, uint32_t> symbols;   /* name: its RVA */

	/* the file offset of rva, or -1 */
	long offset(uint32_t rva) const
	{
		for (const Section &s : sections)
			if (rva >= s.va && rva - s.va < s.size &&
			    rva - s.va < s.raw_size)
				return (long)(s.raw + rva - s.va);
		return -1;
	}
};

/* the PE in d into pe: true, or false with why */
bool read_pe(const std::vector<uint8_t> &d, Pe &pe, std::string &err)
{
	if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z') {
		err = "not a Windows driver (no MZ header)";
		return false;
	}
	uint32_t h = u32(&d[0x3c]);
	if (h + 24 > d.size() || memcmp(&d[h], "PE\0\0", 4) != 0) {
		err = "not a Windows driver (no PE header)";
		return false;
	}
	unsigned nsec = u16(&d[h + 6]);
	uint32_t symtab = u32(&d[h + 12]), nsym = u32(&d[h + 16]);
	unsigned optsize = u16(&d[h + 20]);
	size_t s0 = h + 24 + optsize;
	if (s0 + 40 * (size_t)nsec > d.size()) {
		err = "its sections are cut short";
		return false;
	}
	for (unsigned i = 0; i < nsec; i++) {
		const uint8_t *s = &d[s0 + 40 * i];
		pe.sections.push_back({ u32(s + 12), u32(s + 8),
					u32(s + 20), u32(s + 16) });
	}
	if (!symtab || !nsym || symtab + 18 * (size_t)nsym > d.size()) {
		err = "it has no symbols (the images are found by their "
			"names: the driver must be one that keeps them)";
		return false;
	}
	size_t strtab = symtab + 18 * (size_t)nsym;
	for (uint32_t i = 0; i < nsym; i++) {
		const uint8_t *s = &d[symtab + 18 * (size_t)i];
		std::string name;
		if (u32(s) == 0) {
			size_t o = strtab + u32(s + 4);
			while (o < d.size() && d[o])
				name += (char)d[o++];
		} else {
			name.assign((const char *)s,
				    strnlen((const char *)s, 8));
		}
		/* (its linker's values are the RVAs themselves, not
		   offsets in their sections) */
		int16_t sec = (int16_t)u16(s + 12);
		if (sec > 0 && (unsigned)sec <= pe.sections.size())
			pe.symbols[name] = u32(s + 8);
		i += s[17];
	}
	return true;
}

/* the image named name (and name_SIZE) in the driver: true, or false
   with why */
bool image_at(const std::vector<uint8_t> &d, const Pe &pe,
	      const std::string &name, unsigned unit, Image &img,
	      std::string &err)
{
	auto a = pe.symbols.find("_" + name);
	auto n = pe.symbols.find("_" + name + "_SIZE");
	if (a == pe.symbols.end() || n == pe.symbols.end()) {
		err = "it has no " + name;
		return false;
	}
	long ao = pe.offset(a->second), no = pe.offset(n->second);
	if (ao < 0 || no < 0 || (size_t)no + 4 > d.size()) {
		err = name + " is not in the file";
		return false;
	}
	uint32_t size = u32(&d[no]);
	/* (a module's code may run a few octets past the size the driver
	   gives, into the padding before the size itself: up to it) */
	if (no > ao && (size_t)(no - ao) > size && (size_t)(no - ao) <= size + 16)
		size = (uint32_t)(no - ao);
	if ((size_t)ao + size > d.size()) {
		err = name + " is cut short";
		return false;
	}
	std::vector<uint8_t> v(d.begin() + ao, d.begin() + ao + size);
	if (!parse(std::move(v), unit, img, err)) {
		err = name + ": " + err;
		return false;
	}
	return true;
}

} // namespace

/* see hcf_image.h */
const Module *Image::find(uint16_t id) const
{
	for (const Module &m : modules)
		if (m.id == id)
			return &m;
	return nullptr;
}

/* see hcf_image.h */
bool parse(std::vector<uint8_t> data, unsigned unit, Image &img,
	   std::string &err)
{
	if (data.size() < 8 || memcmp(data.data(), "DRAM", 4) != 0) {
		err = "not a DSP image (no DRAM header)";
		return false;
	}
	img.modules.clear();
	img.unit = unit;
	unsigned n = u16(&data[6]);
	size_t p = 8;
	for (unsigned i = 0; i < n; i++) {
		if (p + 0x1c > data.size()) {
			err = "its module list is cut short";
			return false;
		}
		const uint8_t *r = &data[p];
		Module m;
		m.id = u16(r);
		m.checksum = r[6];
		m.address = u32(r + 8);
		m.octets = u32(r + 0x10) * unit;
		m.offset = u32(r + 0x14);
		p += 0x1c;
		while (p < data.size() && data[p])
			m.name += (char)data[p++];
		p++;
		/* (the driver's images end with empty slots: no code) */
		if (m.octets == 0)
			continue;
		if (m.offset > data.size() || m.octets > data.size() - m.offset) {
			err = "module " + m.name + " is past its end";
			return false;
		}
		img.modules.push_back(m);
	}
	img.data = std::move(data);
	return true;
}

/* see hcf_image.h */
const char *chip_name(uint16_t chip)
{
	switch (chip) {
	case 0x3731: return "L8371";
	case 0x3733: return "L8373";
	case 0x3734: return "L8374";
	default: return "?";
	}
}

/* see hcf_image.h */
bool from_driver(const std::string &path, uint16_t chip, Images &imgs,
		 std::string &err)
{
	const char *c = chip_name(chip);
	if (c[0] == '?') {
		char b[64];
		snprintf(b, sizeof b, "the DSP %04x is not one the driver has "
			 "code for", chip);
		err = b;
		return false;
	}
	std::vector<uint8_t> d;
	Pe pe;
	if (!read_file(path, d, err))
		return false;
	if (!read_pe(d, pe, err)) {
		err = path + ": " + err;
		return false;
	}
	std::string s = std::string(c) + "DP";
	if (!image_at(d, pe, "CTL_DATA_" + s, 1, imgs.ctl, err) ||
	    !image_at(d, pe, "SPX_DATA_" + s, 4, imgs.spx, err)) {
		err = path + ": " + err;
		return false;
	}
	return true;
}

/* see hcf_image.h */
bool from_files(const std::string &ctl, const std::string &spx,
		Images &imgs, std::string &err)
{
	std::vector<uint8_t> d;
	if (!read_file(ctl, d, err))
		return false;
	if (!parse(std::move(d), 1, imgs.ctl, err)) {
		err = ctl + ": " + err;
		return false;
	}
	if (!read_file(spx, d, err))
		return false;
	if (!parse(std::move(d), 4, imgs.spx, err)) {
		err = spx + ": " + err;
		return false;
	}
	return true;
}

/* see hcf_image.h (the sets as DP_DnldNoNEWCNoDelay has them) */
std::vector<Load> download_set(uint16_t chip, int set)
{
	const bool C = false, S = true;
	switch (set) {
	case 1:
		switch (chip) {
		case 0x3731:
			return { { 0xec, C }, { 0xeb, C }, { 0xed, C },
				 { 0xee, C }, { 0xea, C }, { 0xee, S },
				 { 0xf0, S }, { 0xf6, S }, { 0xf7, S } };
		case 0x3733:
			return { { 0xec, C }, { 0xeb, C }, { 0xea, C },
				 { 0xf1, S }, { 0xf0, S }, { 0xf7, S },
				 { 0xf6, S } };
		case 0x3734:
			return { { 0xec, C }, { 0xeb, C }, { 0x41, C },
				 { 0xea, C }, { 0xee, S }, { 0xf7, S },
				 { 0xf0, S } };
		}
		break;
	case 2:
	case 3:
		switch (chip) {
		case 0x3731:
			return { { 0x49, C }, { 0x41, C } };
		case 0x3733:
			return { { 0x4a, C } };
		}
		break;
	}
	return {};
}

} // namespace hcf
