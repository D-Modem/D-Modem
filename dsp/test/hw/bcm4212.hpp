/*
 *
 *    hw/bcm4212.hpp - Broadcom's BCM4212 PCI modem chip (14e4:4212): a
 *                     mixin of a harness
 *
 *    Its registers are a memory BAR of 16 KB.  Its DMA is Broadcom's
 *    (the BCM4401 Ethernet's, as Linux's b44 knows it): two channels at
 *    0x200 (TX) and 0x210 (RX), each a descriptor ring (enable, the
 *    ring's bus address, its PTR, status: state in bits 12..15, the
 *    position below), running while the control register (0) has 0x60;
 *    the samples 16 bit at the line side's rate, an interrupt on a
 *    descriptor's IOC: the interrupt status at 0x20 (written back to
 *    clear), its enables at 0x24.  Its line side is a DAA behind a
 *    serial port: commands to 0x38, a read's result at 0x3c (bit 15:
 *    busy, never here).
 *
 *    The harness (Host, a half::HalfModem) has the DAA, an Si3034's
 *    (hw/si3034.hpp: si3034_cmd(), si3034_data, si3034_rate), maps the
 *    BAR's accesses to bcm_read() and bcm_write(), and runs each codec
 *    sample through bcm_sample().
 *
 */

#ifndef HW_BCM4212_HPP
#define HW_BCM4212_HPP

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../modem_half.hpp"

namespace half {

template <class Host>
class Bcm4212 {
public:
	enum { BCM_SIZE = 0x4000 };     /* the BAR's */
	u32 bcm_regs[BCM_SIZE / 4] = {};

	/* a register read of size bytes at off: what the chip has there */
	u32 bcm_read(u32 off, int size)
	{
		u32 v;
		if ((off & ~3u) == 0x3c)
			bcm_regs[0x3c / 4] = host().si3034_data;
		if ((off & ~3u) == 0x20c)
			bcm_regs[0x20c / 4] = dma_status(0);
		if ((off & ~3u) == 0x21c)
			bcm_regs[0x21c / 4] = dma_status(1);
		v = bcm_regs[(off & (BCM_SIZE - 1)) / 4] >> (8 * (off & 3));
		if (size == 1)
			v &= 0xff;
		else if (size == 2)
			v &= 0xffff;
		return v;
	}

	/* a register write */
	void bcm_write(u32 off, int size, u32 v);

	/* its interrupt: a status bit it enables */
	bool bcm_irq() const
	{
		return (bcm_regs[0x20 / 4] & bcm_regs[0x24 / 4]) != 0;
	}

	/* a codec sample each way through the DMA: the ADC's in, the
	   DAC's out */
	short bcm_sample(short adc);

private:
	enum { DESC_EOT = 0x10000000, DESC_IOC = 0x20000000 };
	/* each channel's current descriptor (its offset), the bytes of it
	   done */
	u32 dma_cur_[2] = {}, dma_pos_[2] = {};
	/* (debug: the peaks each way, each second) */
	int lv_n_ = 0, lv_in_ = 0, lv_out_ = 0;

	Host &host() { return static_cast<Host &>(*this); }

	bool dma_run(int c) const
	{
		return (bcm_regs[0] & 0x60) && (bcm_regs[(0x200 + 0x10 * c) / 4] & 1);
	}
	/* channel c's descriptors posted: those before its PTR (0x208,
	   0x218) */
	bool dma_idle(int c) const
	{
		return dma_cur_[c] == (bcm_regs[(0x208 + 0x10 * c) / 4] & 0xfff);
	}
	u32 dma_status(int c) const
	{
		return dma_run(c) ? (dma_idle(c) ? 0x2000 : 0x1000) |
			(dma_cur_[c] & 0xfff) : 0;
	}
	short dma_sample(int c, short in);
};

template <class Host>
void Bcm4212<Host>::bcm_write(u32 off, int size, u32 v)
{
	u32 *r = &bcm_regs[(off & (BCM_SIZE - 1)) / 4];
	u32 mask = size == 4 ? 0xffffffff : size == 2 ? 0xffff : 0xff;
	int sh = 8 * (off & 3);
	u32 old = *r;
	if ((off & ~3u) == 0x20) {             /* (written back: cleared) */
		*r &= ~((v & mask) << sh);
		return;
	}
	*r = (*r & ~(mask << sh)) | (v & mask) << sh;
	if (off == 0x38)
		host().si3034_cmd(*r);
	if ((off == 0x200 || off == 0x210) && !(*r & 1)) {
		dma_cur_[(off - 0x200) / 0x10] = 0;
		dma_pos_[(off - 0x200) / 0x10] = 0;
	}
	if (off == 0 && (old & 0x60) != (*r & 0x60)) {
		dma_cur_[0] = dma_cur_[1] = dma_pos_[0] = dma_pos_[1] = 0;
		if (host().dbg)
			host().log("dma %s\n", *r & 0x60 ? "on" : "off");
		if (host().dbg && (*r & 0x60)) {
			for (int c = 0; c < 2; c++) {
				u32 *d = (u32 *)(uintptr_t)bcm_regs[(0x204 + 0x10 * c) / 4];
				int i;
				host().log("ring %d at %p:", c, (void *)d);
				for (i = 0; i < 10; i++)
					fprintf(stderr, " %08x/%08x", d[2 * i], d[2 * i + 1]);
				for (i = 0; i < 512 && !(d[2 * i] & DESC_EOT); i++)
					;
				fprintf(stderr, " ... EOT at %d\n", i);
			}
		}
	}
}

/* channel c's next sample: TX (0) the one it reads, RX (1) the one it
   writes; its descriptors' interrupts, the ring's end */
template <class Host>
short Bcm4212<Host>::dma_sample(int c, short in)
{
	u32 *ring = (u32 *)(uintptr_t)bcm_regs[(0x204 + 0x10 * c) / 4], *d;
	short *buf, out = 0;
	u32 len;
	if (!dma_run(c) || !ring || dma_idle(c))
		return 0;
	d = ring + dma_cur_[c] / 4;
	len = d[0] & 0x1fff;
	buf = (short *)(uintptr_t)d[1];
	if (dma_pos_[c] + 2 <= len) {
		if (c)
			buf[dma_pos_[c] / 2] = in;
		else
			out = buf[dma_pos_[c] / 2];
	}
	dma_pos_[c] += 2;
	if (dma_pos_[c] >= len) {
		dma_pos_[c] = 0;
		if (d[0] & DESC_IOC)
			bcm_regs[0x20 / 4] |= c ? 0x10000 : 0x1000000;
		dma_cur_[c] = d[0] & DESC_EOT ? 0 : (dma_cur_[c] + 8) & 0xfff;
	}
	return out;
}

template <class Host>
short Bcm4212<Host>::bcm_sample(short adc)
{
	short dac = dma_sample(0, 0);
	dma_sample(1, adc);
	if (host().dbg) {
		if (abs(adc) > lv_in_)
			lv_in_ = abs(adc);
		if (abs(dac) > lv_out_)
			lv_out_ = abs(dac);
		if (++lv_n_ == (int)host().si3034_rate) {
			host().log("levels: in %d out %d (tx %03x/%03x rx %03x)\n",
				   lv_in_, lv_out_, dma_cur_[0],
				   bcm_regs[0x208 / 4] & 0xfff, dma_cur_[1]);
			lv_n_ = lv_in_ = lv_out_ = 0;
		}
	}
	return dac;
}

}

#endif
