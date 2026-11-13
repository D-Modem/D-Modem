/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hcf_probe.cpp - dmodem-hcf-probe: brings up a Conexant HCF USB
 *                    modem's DSP (the Phoebe Pocket 56K, USB 1690:0103)
 *                    and says what it found, the first step of the open
 *                    driver for its own data pump (hcf_usb.h)
 *
 *    dmodem-hcf-probe [-v[v]] [--device VID:PID] [--driver WINACUSB.sys
 *                     | --images CTL SPX] [--seconds N]
 *
 *    It lists the modem's endpoints, resets its modem interface and its
 *    DSP, and reports the DSP's ID block (its signature and the chip).
 *    Given Conexant's driver (--driver, the user's own copy: the code is
 *    not ours to ship) or two images (--images), it downloads the
 *    modules the driver loads at the start and checks the DSP's
 *    checksum of each.  Then it reads the status for N seconds (2) and
 *    reports the registers.  -v logs the register writes, -vv the
 *    status records too.
 *
 *    Its exit status: 0 if all it tried went well.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "hcf_image.h"
#include "hcf_usb.h"

namespace {

void usage()
{
	fprintf(stderr,
		"usage: dmodem-hcf-probe [-v[v]] [--device VID:PID]\n"
		"         [--driver WINACUSB.sys | --images CTL SPX] "
		"[--seconds N]\n");
	exit(2);
}

const char *type_name(uint8_t t)
{
	switch (t) {
	case 0: return "control";
	case 1: return "isochronous";
	case 2: return "bulk";
	default: return "interrupt";
	}
}

} // namespace

int main(int argc, char **argv)
{
	int log = 0, seconds = 2;
	unsigned vid = 0, pid = 0;
	std::string driver, ctl, spx;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "-v")
			log = 1;
		else if (a == "-vv")
			log = 2;
		else if (a == "--device" && i + 1 < argc) {
			if (sscanf(argv[++i], "%x:%x", &vid, &pid) != 2)
				usage();
		} else if (a == "--driver" && i + 1 < argc)
			driver = argv[++i];
		else if (a == "--images" && i + 2 < argc) {
			ctl = argv[++i];
			spx = argv[++i];
		} else if (a == "--seconds" && i + 1 < argc)
			seconds = atoi(argv[++i]);
		else
			usage();
	}

	hcf::Usb u(log);
	std::string err;
	if (!u.open((uint16_t)vid, (uint16_t)pid, err)) {
		fprintf(stderr, "dmodem-hcf-probe: %s\n", err.c_str());
		return 1;
	}
	printf("modem: USB %04x:%04x\n", u.vid(), u.pid());
	static const char *use[] = { "registers", "line side", "DSP code",
				     "T-buffer", "data", "status", "GPIO",
				     "LEDs" };
	for (size_t i = 0; i < u.pipes().size(); i++) {
		const hcf::Usb::Pipe &p = u.pipes()[i];
		printf("pipe %zu: endpoint %02x %s %s, %u octets (%s)\n", i,
		       p.ep, p.ep & 0x80 ? "IN" : "OUT", type_name(p.type),
		       p.max, i < 8 ? use[i] : "?");
	}

	hcf::Id id;
	if (!u.reset(id, err)) {
		printf("reset: %s\n", err.c_str());
		return 1;
	}
	printf("DSP signature %04x %04x, chip %04x (%s), ID %04x %04x %04x "
	       "%04x\n", id.sig1, id.sig2, id.chip, hcf::chip_name(id.chip),
	       id.w48, id.w46, id.w44, id.w41);

	int status = 0;
	if (!driver.empty() || !ctl.empty()) {
		hcf::Images imgs;
		bool ok = driver.empty() ?
			hcf::from_files(ctl, spx, imgs, err) :
			hcf::from_driver(driver, id.chip, imgs, err);
		if (!ok) {
			printf("DSP code: %s\n", err.c_str());
			return 1;
		}
		unsigned done = 0, total = 0;
		for (int set : { 1, 2 }) {
			for (const hcf::Load &l : hcf::download_set(id.chip, set)) {
				const hcf::Image &img = l.spx ? imgs.spx : imgs.ctl;
				const hcf::Module *m = img.find(l.id);
				total++;
				if (!m) {
					printf("set %d: %s module %02x: not in "
					       "the images\n", set,
					       l.spx ? "SPX" : "CTL", l.id);
					status = 1;
					continue;
				}
				unsigned sum = 0;
				if (u.download(img, *m, l.spx, err, &sum)) {
					printf("set %d: %s %s, %u octets at %04x: "
					       "checksum %02x\n", set,
					       l.spx ? "SPX" : "CTL",
					       m->name.c_str(), m->octets,
					       m->address, sum);
					done++;
				} else {
					printf("set %d: %s\n", set, err.c_str());
					status = 1;
				}
			}
		}
		printf("DSP code: %u of %u modules loaded\n", done, total);
	}

	if (!u.run(seconds * 1000)) {
		printf("the modem is gone\n");
		return 1;
	}
	printf("status: %u records; registers", u.records());
	for (unsigned r = 0; r < 0x20; r++)
		printf("%s%02x", r % 8 ? " " : "  ", u.get(hcf::reg_byte(r)));
	printf("\n");
	return status;
}
