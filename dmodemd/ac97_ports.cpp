/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    ac97_ports.cpp - an ICH modem codec's registers through its I/O
 *                     ports (ac97_ports.h)
 *
 */

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <dirent.h>
#include <unistd.h>

#if defined(__linux__) && (defined(__i386__) || defined(__x86_64__))
#include <sys/io.h>
#define AC97_PORTS 1
#endif

#include "ac97_ports.h"

namespace dmd {

namespace {

const unsigned ICH_GLOB_STA = 0x30, ICH_CAS = 0x34;
const unsigned ICH_RCS = 0x8000;        /* GLOB_STA: read timed out */
const unsigned long IORESOURCE_IO = 0x100;

/* the I/O ranges of PCI device dev (its sysfs directory): BARs 0 and
   1's starts; false if they are not I/O ranges */
bool io_ranges(const std::string &dev, unsigned *bar0, unsigned *bar1)
{
	FILE *f = fopen((dev + "/resource").c_str(), "r");
	unsigned long long start, end, flags;
	unsigned bars[2] = { 0, 0 };
	int i = 0;

	if (!f)
		return false;
	while (i < 2 && fscanf(f, "%llx %llx %llx", &start, &end,
			       &flags) == 3) {
		if (!(flags & IORESOURCE_IO) || start == 0 || start > 0xffff)
			break;
		bars[i++] = static_cast<unsigned>(start);
	}
	fclose(f);
	*bar0 = bars[0];
	*bar1 = bars[1];
	return i == 2;
}

/* the modem codec's number on card card (its mc97#0-<n> in procfs),
   0 if there is none */
unsigned modem_codec(int card)
{
	char path[64];
	unsigned n = 0;

	snprintf(path, sizeof(path), "/proc/asound/card%d/codec97#0", card);
	DIR *d = opendir(path);
	if (!d)
		return 0;
	while (struct dirent *e = readdir(d)) {
		unsigned bus, num;
		if (sscanf(e->d_name, "mc97#%u-%u", &bus, &num) == 2) {
			n = num;
			break;
		}
	}
	closedir(d);
	return n;
}

} // namespace

/* see ac97_ports.h */
bool Ac97Ports::open(int card, std::string *why)
{
	char dev[64], link[PATH_MAX];
	unsigned bar0, bar1;

	snprintf(dev, sizeof(dev), "/sys/class/sound/card%d/device", card);
	ssize_t n = readlink((std::string(dev) + "/driver").c_str(), link,
			     sizeof(link) - 1);
	if (n < 0) {
		*why = std::string(dev) + ": no driver";
		return false;
	}
	link[n] = 0;
	const char *drv = strrchr(link, '/');
	drv = drv ? drv + 1 : link;
	if (strcmp(drv, "snd_intel8x0m") != 0) {
		*why = std::string("card's driver ") + drv +
		       ", not snd_intel8x0m";
		return false;
	}
	if (!io_ranges(dev, &bar0, &bar1)) {
		*why = std::string(dev) + ": no I/O ranges";
		return false;
	}
#ifdef AC97_PORTS
	if (ioperm(bar0, 0x100, 1) < 0 || ioperm(bar1, 0x40, 1) < 0) {
		*why = std::string("its I/O ports: ") + strerror(errno) +
		       " (root only)";
		return false;
	}
	mixer_ = bar0;
	bm_ = bar1;
	codec_ = modem_codec(card);
	return true;
#else
	*why = "I/O ports on x86 Linux only";
	return false;
#endif
}

/* see ac97_ports.h */
std::string Ac97Ports::name() const
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%#x+%#x", mixer_, codec_ * 0x80);
	return buf;
}

/* see ac97_ports.h (private): CAS read 0 is ours; 100 tries 10 us
   apart, as the kernel's driver */
bool Ac97Ports::acquire()
{
#ifdef AC97_PORTS
	for (int i = 0; i < 100; i++) {
		if (!(inb(bm_ + ICH_CAS) & 1))
			return true;
		delay_us(10);
	}
#endif
	return false;
}

/* see ac97_ports.h: a read that timed out (GLOB_STA's RCS) cleared and
   an error */
bool Ac97Ports::read(int reg, uint16_t *v)
{
#ifdef AC97_PORTS
	if (!mixer_ || !acquire())
		return false;
	*v = inw(mixer_ + codec_ * 0x80 + (reg & 0x7e));
	unsigned sta = inl(bm_ + ICH_GLOB_STA);
	if (sta & ICH_RCS) {
		outl(ICH_RCS, bm_ + ICH_GLOB_STA);
		return false;
	}
	return true;
#else
	(void)reg;
	(void)v;
	return false;
#endif
}

/* see ac97_ports.h */
bool Ac97Ports::write(int reg, uint16_t v)
{
#ifdef AC97_PORTS
	if (!mixer_ || !acquire())
		return false;
	outw(v, mixer_ + codec_ * 0x80 + (reg & 0x7e));
	return true;
#else
	(void)reg;
	(void)v;
	return false;
#endif
}

/* see ac97_ports.h: a busy wait for the short waits, a sleep from a
   millisecond on */
void Ac97Ports::delay_us(unsigned us)
{
	struct timespec t0, t;

	if (us >= 1000) {
		t.tv_sec = us / 1000000;
		t.tv_nsec = static_cast<long>(us % 1000000) * 1000;
		while (nanosleep(&t, &t) < 0 && errno == EINTR)
			;
		return;
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	do
		clock_gettime(CLOCK_MONOTONIC, &t);
	while ((t.tv_sec - t0.tv_sec) * 1000000L +
	       (t.tv_nsec - t0.tv_nsec) / 1000 < static_cast<long>(us));
}

} // namespace dmd
