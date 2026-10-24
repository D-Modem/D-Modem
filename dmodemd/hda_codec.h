/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    hda_codec.h - an HD-audio modem codec's registers from user space,
 *                  through the kernel's HD-audio hwdep device
 *                  (/dev/snd/hwC<card>D<codec>, CONFIG_SND_HDA_HWDEP)
 *
 *    The device takes a codec command (HDA_IOCTL_VERB_WRITE: the node in
 *    bits 31:24, the verb and its parameter below, as the kernel's
 *    snd_hda_codec_read() puts them together) and gives its response.
 *    A card's codecs each have one; the modem codec is the one with a
 *    modem function group (type 2).  A Si3054's registers are nodes of
 *    their own: read with verb 0x900, written with 0x1 and the 16 bit
 *    value (the kernel's snd-hda-codec-si3054 does the same).
 *
 */
#ifndef DMODEMD_HDA_CODEC_H
#define DMODEMD_HDA_CODEC_H

#include <string>

#include <alsa/asoundlib.h>

#include "dm_types.h"

namespace dmd {

class HdaCodec {
public:
	HdaCodec() = default;
	/* (closed) */
	~HdaCodec() { close(); }
	HdaCodec(const HdaCodec &) = delete;
	HdaCodec &operator=(const HdaCodec &) = delete;

	/* the modem codec of ctl's card, its hwdep device opened; false if
	   it has none (not HD audio, no hwdep in the kernel, no modem
	   codec), why in *why */
	bool open(snd_ctl_t *ctl, std::string *why);
	void close();
	/* the hwdep device is open */
	bool is_open() const { return h_ != nullptr; }
	/* its name ("hw:0,1") */
	const std::string &name() const { return name_; }
	/* register (node) nid read into *v, or written with v; false on
	   an error */
	bool read(s16 nid, u16 *v);
	bool write(s16 nid, u16 v);

private:
	/* a command (node, verb, parameter) and its response */
	bool verb(s16 nid, u32 verb, u32 param, u32 *res);
	/* whether the codec has a modem function group */
	bool is_modem();

	snd_hwdep_t *h_ = nullptr;
	std::string name_;
};

} // namespace dmd

#endif
