/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    hw/pci.hpp - PCI's configuration space of one device on bus 0, and
 *                 the ports of configuration mechanism 1 (0xcf8, 0xcfc):
 *                 a mixin of a harness
 *
 *    The harness (Host, a half::HalfModem) sets the device's space
 *    (pci_cfg: its IDs, class, BARs, interrupt line), its slot and
 *    function (pci_devfn) and its BARs' sizes (pci_bar_size), and has
 *    its port I/O go to pci_io_in() and pci_io_out(); a driver that has
 *    its OS read the space calls pci_read() and pci_write().
 *
 */

#ifndef HW_PCI_HPP
#define HW_PCI_HPP

#include "../modem_half.hpp"

namespace half {

template <class Host>
class PciDevice {
public:
	u32 pci_cfg[64] = {};
	unsigned pci_devfn = 0;
	/* the BARs' sizes (powers of 2, 0 none): what a write of all ones
	   reads back */
	u32 pci_bar_size[6] = {};
	/* not 0: function 0 of the slot is a multi-function bridge with
	   this ID (an ICH's LPC bridge; the SM56 module looks before it
	   reads function 6) */
	u32 pci_fn0_id = 0;

	/* size bytes of register r */
	u32 pci_read(unsigned r, int size) const
	{
		u32 v = pci_cfg[(r >> 2) & 63] >> (8 * (r & 3));
		return size == 4 ? v : size == 2 ? v & 0xffff : v & 0xff;
	}

	/* size bytes of register r = v; a BAR's size in the bits that stay
	   0 (bit 0 kept: I/O space) */
	void pci_write(unsigned r, u32 v, int size)
	{
		u32 mask = size == 4 ? 0xffffffff : size == 2 ? 0xffff : 0xff;
		if (host().dbg)
			host().log("pci config %02x = %x\n", r, v);
		if (r >= 0x10 && r < 0x28 && !(r & 3) && pci_bar_size[(r - 0x10) / 4]) {
			u32 size_mask = pci_bar_size[(r - 0x10) / 4] - 1;
			u32 io = pci_cfg[r / 4] & 1;
			v = v == 0xffffffff ? ~size_mask | io : (v & ~size_mask) | io;
		}
		pci_cfg[(r >> 2) & 63] = (pci_cfg[(r >> 2) & 63] & ~(mask << 8 * (r & 3))) |
			(v & mask) << 8 * (r & 3);
	}

	/* a read of the configuration ports (size bytes at port a): the
	   device's space, function 0's ID (pci_fn0_id), all ones for any
	   other device; false for another port */
	bool pci_io_in(unsigned a, int size, u32 *v)
	{
		u32 r;
		if (a == 0xcf8) {
			r = cf8_;
		} else if (a >= 0xcfc && a < 0xd00) {
			unsigned reg = (cf8_ & 0xfc) + (a - 0xcfc);
			bool on = (cf8_ & 0x80000000) && ((cf8_ >> 16) & 0xff) == 0;
			unsigned df = (cf8_ >> 8) & 0xff;
			if (on && pci_fn0_id && df == (pci_devfn & ~7u)) {
				r = reg < 4 ? pci_fn0_id : (reg & ~3u) == 0x0c ? 0x00800000 : 0;
				r >>= 8 * (reg & 3);
			} else if (on && df == pci_devfn)
				r = pci_read(reg, 4);
			else
				r = 0xffffffff;
		} else
			return false;
		*v = size == 1 ? r & 0xff : size == 2 ? r & 0xffff : r;
		return true;
	}

	/* a write of them */
	bool pci_io_out(unsigned a, u32 v, int size)
	{
		if (a == 0xcf8 && size == 4) {
			cf8_ = v;
			return true;
		}
		if (a < 0xcfc || a >= 0xd00)
			return false;
		if ((cf8_ & 0x80000000) && ((cf8_ >> 16) & 0xff) == 0 &&
		    ((cf8_ >> 8) & 0xff) == pci_devfn)
			pci_write((cf8_ & 0xfc) + (a - 0xcfc), v, size);
		return true;
	}

private:
	u32 cf8_ = 0;

	Host &host() { return static_cast<Host &>(*this); }
};

}

#endif
