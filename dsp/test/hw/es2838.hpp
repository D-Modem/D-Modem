/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    hw/es2838.hpp - ESS's ES2838 PCI modem chip (125d:2838, the ES56H-PI
 *                    host signal processing modem): a mixin of a harness
 *
 *    Its registers are 256 ports (BAR 0), 16 bit (no datasheet: what the
 *    driver, es56hpi.sys, does with them):
 *
 *      0x00, 0x02     an index and its register's data: the chip's own
 *                     registers below 0x100, the codec's (AC'97 like)
 *                     above: 0x14c GPIO configuration, 0x154 GPIO status
 *                     (bit 9 the loop current: without it off hook the
 *                     driver says OVERLOAD), 0x15a the rate (bits 12..15)
 *      0x04           the FIFO: four DAC samples written, then four ADC
 *                     samples read, and again
 *      0x06           the ADC's samples in the FIFO (above 0x1ff an
 *                     overrun to the driver)
 *      0x08, 0x0a     a DAA's registers: 0x08 a command (the register,
 *                     bit 7 a read; bit 0 busy, never here), 0x0a its data
 *                     (the codec type here leaves them alone)
 *      0x0c           control: bit 0 the FIFO runs, bit 1 its interrupt
 *      0x0e           status: bit 1 the FIFO's interrupt (ES_IRQ_LEVEL
 *                     samples or more in), bit 0 the line's
 *
 *    (The rest as memory.)  The FIFO runs at the codec's rate (es_rate()).
 *
 *    The harness (Host, a half::HalfModem) maps the ports' accesses to
 *    es_in() and es_out(), and runs each codec sample through
 *    es_sample().
 *
 */

#ifndef HW_ES2838_HPP
#define HW_ES2838_HPP

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../modem_half.hpp"

namespace half {

template <class Host>
class Es2838 {
public:
	enum { ES_SIZE = 0x100 };       /* the BAR's ports */
	enum { ES_FIFO = 512, ES_IRQ_LEVEL = 16 };
	byte es_ports[ES_SIZE] = {};
	word es_regs[0x200] = {};       /* behind 0x00/0x02 */
	word es_daa[0x80] = {};         /* behind 0x08/0x0a */

	/* a port read of size bytes at off */
	u32 es_in(u32 off, int size)
	{
		u32 v = 0;
		off &= ES_SIZE - 1;
		switch (off) {
		case 0x02:
			v = es_reg_read(idx());
			break;
		case 0x04:
			v = (word)rx_pop();
			break;
		case 0x06:
			v = rx_n_;
			break;
		case 0x08:
			v = port16(0x08) & ~1u;
			break;
		case 0x0e:
			v = status();
			break;
		default:
			memcpy(&v, &es_ports[off], size);
			break;
		}
		if (host().dbg >= 2 && off != 0x04 && off != 0x06 && off != 0x0c)
			host().log("es in %02x = %x\n", off, v);
		return v;
	}

	/* a port write */
	void es_out(u32 off, u32 v, int size)
	{
		off &= ES_SIZE - 1;
		if (host().dbg >= 2 && off != 0x04)
			host().log("es out %02x = %x\n", off, v);
		switch (off) {
		case 0x02:
			es_reg_write(idx(), (word)v);
			return;
		case 0x04:
			tx_push((short)v);
			return;
		case 0x08:
			memcpy(&es_ports[off], &v, size);
			if (v & 0x80)
				port16(0x0a) = es_daa[v & 0x7f];
			else
				es_daa[v & 0x7f] = port16(0x0a);
			return;
		case 0x0c:
			if ((v & 1) && !(port16(0x0c) & 1))
				rx_n_ = tx_n_ = rx_r_ = tx_r_ = 0;
			break;
		}
		memcpy(&es_ports[off], &v, size);
	}

	/* its interrupt: the FIFO's, enabled */
	bool es_irq() const { return (status() & 2) && (port16c(0x0c) & 2); }

	/* the codec's rate (Hz): the codec's register 0x5a (0x15a), bits
	   12..15 (the driver's table for its rates 0 to 6: 6, 5, 4, 3, 2,
	   1, 11; 11, 8000 Hz, for call progress and V.90; the others the
	   V.34 symbol rates' three samples (four at 2400)) */
	double es_rate() const
	{
		switch (es_regs[0x15a] >> 12) {
		case 6: return 7200;
		case 5: return 57600.0 / 7;
		case 4: return 8400;
		case 3: return 9000;
		case 2: return 9600;
		case 1: return 72000.0 / 7;
		case 11: return 8000;
		default: return 8000;
		}
	}

	/* a codec sample each way through the FIFO: the ADC's in, the
	   DAC's out */
	short es_sample(short adc)
	{
		short dac;
		if (!(port16(0x0c) & 1))
			return 0;
		if (rx_n_ < ES_FIFO) {
			rx_[(rx_r_ + rx_n_) % ES_FIFO] = adc;
			rx_n_++;
		} else
			lv_over_++;
		if (tx_n_)
			dac = tx_pop();
		else {
			dac = 0;
			lv_under_++;
		}
		if (dump_) {
			fwrite(&dac, 2, 1, dump_);
			fwrite(&adc, 2, 1, dump_);
		}
		if (host().dbg) {
			lv_in_ = abs(adc) > lv_in_ ? abs(adc) : lv_in_;
			lv_out_ = abs(dac) > lv_out_ ? abs(dac) : lv_out_;
			if (++lv_n_ >= es_rate()) {
				host().log("es: %llu ms: in %d out %d, fifo rx %u tx %u, "
					   "over %u, under %u, gpio %x\n", host().ms(), lv_in_,
					   lv_out_, rx_n_, tx_n_, lv_over_, lv_under_,
					   es_regs[0x154]);
				lv_n_ = lv_in_ = lv_out_ = 0;
			}
		}
		return dac;
	}

	/* the indexed registers: the line's in the codec's GPIO status
	   (0x154 bit 9: the loop current) */
	word es_reg_read(unsigned i)
	{
		word v = es_regs[i & 0x1ff];
		if (i == 0x154)
			v |= 0x200;
		if (host().dbg >= 2)
			host().log("es reg %03x = %x\n", i, v);
		return v;
	}
	/* indexed register i written with v */
	void es_reg_write(unsigned i, word v)
	{
		if (host().dbg >= 2)
			host().log("es reg %03x <- %x\n", i, v);
		if (i == 0x15a && host().dbg && (v ^ es_regs[0x15a]) >> 12) {
			es_regs[0x15a] = v;
			host().log("es: %llu ms: rate %.1f\n", host().ms(), es_rate());
		}
		es_regs[i & 0x1ff] = v;
	}

private:
	short rx_[ES_FIFO] = {}, tx_[ES_FIFO] = {};
	unsigned rx_r_ = 0, rx_n_ = 0, tx_r_ = 0, tx_n_ = 0;
	/* (debug: the peaks each way, each second; the ADC's samples lost) */
	int lv_n_ = 0, lv_in_ = 0, lv_out_ = 0;
	unsigned lv_over_ = 0, lv_under_ = 0;
	/* (debug: the DAC's and the ADC's samples, in pairs, to <PREFIX>_DUMP) */
	FILE *dump_ = host().env("DUMP") ? fopen(host().env("DUMP"), "wb") : NULL;

	/* the harness */
	Host &host() { return static_cast<Host &>(*this); }
	/* the 16 bit port at offset o */
	word &port16(unsigned o) { return *(word *)&es_ports[o]; }
	/* its value */
	word port16c(unsigned o) const { return *(const word *)&es_ports[o]; }
	/* the index (0x00) of the indexed registers */
	unsigned idx() const { return port16c(0x00); }

	/* the status (0x0e): bit 1 if the FIFO runs with ES_IRQ_LEVEL
	   samples in */
	u32 status() const
	{
		u32 s = port16c(0x0e) & ~3u;
		if ((port16c(0x0c) & 1) && rx_n_ >= ES_IRQ_LEVEL)
			s |= 2;
		return s;
	}
	/* the ADC's oldest sample out of the FIFO; 0 if none */
	short rx_pop()
	{
		short v;
		if (!rx_n_)
			return 0;
		v = rx_[rx_r_];
		rx_r_ = (rx_r_ + 1) % ES_FIFO;
		rx_n_--;
		return v;
	}
	/* v to the DAC's FIFO, unless it is full */
	void tx_push(short v)
	{
		if (tx_n_ < ES_FIFO) {
			tx_[(tx_r_ + tx_n_) % ES_FIFO] = v;
			tx_n_++;
		}
	}
	/* the DAC's next sample out of its FIFO (there is one) */
	short tx_pop()
	{
		short v = tx_[tx_r_];
		tx_r_ = (tx_r_ + 1) % ES_FIFO;
		tx_n_--;
		return v;
	}
};

}

#endif
