/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    hda_codec.cpp - an HD-audio modem codec's registers through the
 *                    kernel's hwdep device (hda_codec.h)
 *
 */

#include "hda_codec.h"

#include <cstdio>

#include <sys/ioctl.h>

namespace dmd {

namespace {

/* the kernel's include/sound/hda_hwdep.h */
struct hda_verb_ioctl {
	uint32_t verb;          /* node << 24 | verb << 8 | parameter */
	uint32_t res;
};
const unsigned long HDA_IOCTL_VERB_WRITE =
	_IOWR('H', 0x11, struct hda_verb_ioctl);

/* the verbs and parameters (the HD Audio specification's) */
const unsigned GET_PARAMETER = 0xf00;
const unsigned PAR_NODE_COUNT = 0x04, PAR_FUNCTION_TYPE = 0x05;
const unsigned GRP_MODEM = 0x02;
const unsigned SI3054_READ = 0x900, SI3054_WRITE = 0x1;

} // namespace

/* see hda_codec.h: each hwdep device of the card's, an HD-audio
   codec's (its interface, before any of its commands), until one is a
   modem codec */
bool HdaCodec::open(snd_ctl_t *ctl, std::string *why)
{
	snd_ctl_card_info_t *ci;
	snd_hwdep_info_t *hi;
	int card, dev = -1;

	close();
	snd_ctl_card_info_alloca(&ci);
	snd_hwdep_info_alloca(&hi);
	if (snd_ctl_card_info(ctl, ci) < 0 ||
	    (card = snd_ctl_card_info_get_card(ci)) < 0) {
		*why = "not a card";
		return false;
	}
	*why = "no HD-audio codec device (not HD audio, or a kernel "
		"without CONFIG_SND_HDA_HWDEP)";
	while (snd_ctl_hwdep_next_device(ctl, &dev) >= 0 && dev >= 0) {
		char name[32];
		snprintf(name, sizeof(name), "hw:%d,%d", card, dev);
		if (snd_hwdep_open(&h_, name, SND_HWDEP_OPEN_DUPLEX) < 0) {
			h_ = nullptr;
			*why = std::string("cannot open ") + name;
			continue;
		}
		if (snd_hwdep_info(h_, hi) >= 0 &&
		    snd_hwdep_info_get_iface(hi) == SND_HWDEP_IFACE_HDA &&
		    is_modem()) {
			name_ = name;
			return true;
		}
		*why = "no modem codec among the card's HD-audio codecs";
		close();
	}
	return false;
}

/* see hda_codec.h */
void HdaCodec::close()
{
	if (h_)
		snd_hwdep_close(h_);
	h_ = nullptr;
	name_.clear();
}

/* see hda_codec.h */
bool HdaCodec::verb(int nid, unsigned verb, unsigned param, uint32_t *res)
{
	hda_verb_ioctl v;

	v.verb = (uint32_t)nid << 24 | verb << 8 | param;
	v.res = 0;
	if (snd_hwdep_ioctl(h_, HDA_IOCTL_VERB_WRITE, &v) < 0)
		return false;
	*res = v.res;
	return true;
}

/* see hda_codec.h: the root's function groups (its node count) */
bool HdaCodec::is_modem()
{
	uint32_t r;

	if (!verb(0, GET_PARAMETER, PAR_NODE_COUNT, &r))
		return false;
	unsigned start = r >> 16 & 0xff, n = r & 0xff;
	for (unsigned nid = start; nid < start + n; nid++)
		if (verb((int)nid, GET_PARAMETER, PAR_FUNCTION_TYPE, &r) &&
		    (r & 0xff) == GRP_MODEM)
			return true;
	return false;
}

/* see hda_codec.h */
bool HdaCodec::read(int nid, uint16_t *v)
{
	uint32_t r;

	if (!h_ || !verb(nid, SI3054_READ, 0, &r))
		return false;
	*v = (uint16_t)r;
	return true;
}

/* see hda_codec.h: the 4 bit verb and its 16 bit payload, as the
   kernel takes them (its verb 0x1vv, parameter the low octet) */
bool HdaCodec::write(int nid, uint16_t v)
{
	uint32_t r;

	return h_ && verb(nid, SI3054_WRITE << 8 | v >> 8, v & 0xff, &r);
}

} // namespace dmd
