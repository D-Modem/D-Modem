/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    ac97_ports.h - an AC'97 modem codec's registers from user space on
 *                   an Intel ICH modem controller (snd-intel8x0m's: ICH,
 *                   and the SiS, nVidia and AMD ones like it), through
 *                   its I/O ports (Linux, x86, root: ioperm())
 *
 *    The controller's first I/O range (BAR 0) holds the codecs'
 *    registers, the secondary's 0x80 on; its second (BAR 1, the bus
 *    master's) the codec access semaphore (CAS, 0x34: read 0, it is
 *    ours until the access is over) and the global status (0x30, bit 15:
 *    a codec read timed out).  The kernel's driver keeps to the same
 *    semaphore, so the two can share the codec.  The ALSA card's PCI
 *    device and its ranges are in sysfs, which codec is the modem's in
 *    /proc/asound/card<N>/codec97#0/mc97#0-<codec>.
 *
 */
#ifndef DMODEMD_AC97_PORTS_H
#define DMODEMD_AC97_PORTS_H

#include <string>

#include "cx_daa.h"

namespace dmd {

class Ac97Ports final : public Ac97Regs {
public:
	/* the modem codec of ALSA card card, its ports ours; false (why
	   in *why) if the card is not snd-intel8x0m's, or no root */
	bool open(int card, std::string *why);
	/* whether open() succeeded */
	bool is_open() const { return mixer_ != 0; }
	/* where its registers are, for the log ("0xe100+0x80") */
	std::string name() const;

	/* see Ac97Regs: each under the controller's semaphore */
	bool read(int reg, uint16_t *v) override;
	bool write(int reg, uint16_t v) override;
	void delay_us(uint32_t us) override;

private:
	/* the codec access semaphore ours (about 1 ms at most) */
	bool acquire();

	unsigned mixer_ = 0, bm_ = 0;   /* the two I/O ranges */
	unsigned codec_ = 0;            /* the modem codec's 0x80s */
};

} // namespace dmd

#endif
