/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    fake_hda_hwdep.cpp - an HD-audio Si3054 modem codec's hwdep device
 *                         for alsa_test.py: alsa-lib's hwdep calls,
 *                         put in front of the real ones (LD_PRELOAD)
 *
 *    The card has one hwdep device, 0, an HD-audio codec's
 *    (SND_HWDEP_IFACE_HDA) with one function group, node 1, a modem
 *    group.  HDA_IOCTL_VERB_WRITE takes its commands as the kernel does
 *    (the node in bits 31:24, the verb and parameter below): the
 *    parameters' (0xf00), and the Si3054's registers, nodes 2 to 21,
 *    read (0x900) and written (0x1 and the 16 bit value).  Their values
 *    at first are the datasheet's after reset, node 13 as the kernel's
 *    snd-hda-codec-si3054 leaves it (0x200), the chip ID a Si3054's with
 *    a Si3018 line side, powered (0x1155: line side and system side
 *    revision 5; FAKE_HDA_CHIPID=<hex> another).
 *
 *    FAKE_HDA_LOG=1: each register written on stderr, "fake hda: node
 *    <n> <hex>".
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include <alsa/asoundlib.h>

namespace {

struct hda_verb_ioctl {
	uint32_t verb;
	uint32_t res;
};
const unsigned long HDA_IOCTL_VERB_WRITE =
	_IOWR('H', 0x11, struct hda_verb_ioctl);

struct Codec {
	uint16_t regs[22] = {};
	bool init = false;
	bool open = false;
} codec;

/* the registers' first values (once) */
void setup()
{
	if (codec.init)
		return;
	codec.init = true;
	codec.regs[12] = getenv("FAKE_HDA_CHIPID") ?
		(uint16_t)strtoul(getenv("FAKE_HDA_CHIPID"), nullptr, 16) :
		0x1155;
	codec.regs[13] = 0x0200;
	codec.regs[15] = 0x6000;
	codec.regs[16] = 0x0400;
	codec.regs[18] = 0x0000;
}

/* a command's response (the kernel's 32 bit command: node 31:24 here,
   then 20 bits of verb and parameter) */
uint32_t command(uint32_t v)
{
	unsigned nid = v >> 24, low = v & 0xfffff;

	if ((low >> 8) == 0xf00) {              /* GET_PARAMETER */
		unsigned par = low & 0xff;
		if (nid == 0 && par == 0x04)    /* node count: node 1 */
			return 1 << 16 | 1;
		if (nid == 1 && par == 0x05)    /* a modem group */
			return 0x02;
		return 0;
	}
	if (nid < 2 || nid > 21)
		return 0;
	if ((low >> 8) == 0x900)                /* a register read */
		return codec.regs[nid];
	if ((low >> 16) == 0x1) {               /* a register written */
		codec.regs[nid] = (uint16_t)low;
		if (getenv("FAKE_HDA_LOG"))
			fprintf(stderr, "fake hda: node %u %04x\n", nid,
				codec.regs[nid]);
		return 0;
	}
	return 0;
}

int fake_hwdep;                 /* the one device's handle */

} // namespace

extern "C" {

/* the card's hwdep devices: 0 alone */
int snd_ctl_hwdep_next_device(snd_ctl_t *, int *device)
{
	*device = *device < 0 ? 0 : -1;
	return 0;
}

/* device 0 of any card */
int snd_hwdep_open(snd_hwdep_t **hwdep, const char *name, int)
{
	size_t n = strlen(name);
	if (n < 2 || strcmp(name + n - 2, ",0"))
		return -ENOENT;
	setup();
	codec.open = true;
	*hwdep = reinterpret_cast<snd_hwdep_t *>(&fake_hwdep);
	return 0;
}

/* (it closed) */
int snd_hwdep_close(snd_hwdep_t *)
{
	codec.open = false;
	return 0;
}

/* (its interface, from snd_hwdep_info_get_iface()) */
int snd_hwdep_info(snd_hwdep_t *, snd_hwdep_info_t *)
{
	return 0;
}

/* an HD-audio codec's */
snd_hwdep_iface_t snd_hwdep_info_get_iface(const snd_hwdep_info_t *)
{
	return SND_HWDEP_IFACE_HDA;
}

/* HDA_IOCTL_VERB_WRITE: the command's response */
int snd_hwdep_ioctl(snd_hwdep_t *, unsigned int request, void *arg)
{
	if (request != HDA_IOCTL_VERB_WRITE || !codec.open)
		return -EINVAL;
	hda_verb_ioctl *v = static_cast<hda_verb_ioctl *>(arg);
	v->res = command(v->verb);
	return 0;
}

} // extern "C"
