/*
 *
 *    interop_windows.h - a Windows kernel of our own for the WDM driver
 *                        of a soft modem (half_broadcom): the driver's PE
 *                        image loaded and bound to it, run on one thread
 *                        and the line's clock, its device a PCI one the
 *                        harness emulates, its port opened as modem.sys
 *                        does
 *
 *    The harness describes the device and its driver (struct win_dev):
 *    the driver's files and the INF's model, the PCI configuration space
 *    (half::PciConfig), the registers (BAR 0's memory, port I/O) as
 *    calls, the interrupt line, the line's rate and how to run it on
 *    while the kernel waits.  win_start() loads the driver, starts it and
 *    opens its port; win_tick() after each line sample runs the timers,
 *    the DPCs, the threads and the ISR; win_serial_*() read and write
 *    the port.
 *
 *    The rest the INF says: the registry (its model's AddReg sections),
 *    the driver's service, the device's description, maker and class.
 *
 */

#ifndef INTEROP_WINDOWS_H
#define INTEROP_WINDOWS_H

#include <functional>

#include "modem_half.hpp"

/* the device and its driver, as the harness has them */
struct win_dev {
	/* the driver: its image, its INF and the INF's model (the
	   install section [<model>.NT], its AddReg and its service) */
	const char *sys, *inf, *model;
	const char *port;               /* the port's name (PortName) */
	/* the PCI device on bus 0: its slot and function, and its
	   configuration space as the bus driver reports it (IDs, class,
	   BAR 0, the interrupt line); BAR 0's size is mem_size */
	half::PciConfig *pci;
	/* its registers: BAR 0's memory, mem_size bytes, each access of
	   the driver a call */
	half::u32 mem_size;
	std::function<half::u32(half::u32 off, int size)> mem_read;
	std::function<void(half::u32 off, int size, half::u32 v)> mem_write;
	/* port I/O but PCI's configuration ports (none: all ones) */
	std::function<half::u32(unsigned port, int size)> io_in;
	std::function<void(unsigned port, half::u32 v, int size)> io_out;
	/* its interrupt: up or not */
	std::function<bool()> irq;
	/* the line: its rate, and n samples of it run on (silent, each
	   through win_tick()) while the kernel waits */
	int rate;
	std::function<void(unsigned n)> idle;
};

/* 1: the driver's calls of note, 2: and the device's accesses, 3: and
   every call it makes into the kernel */
extern int win_debug;
/* the line's samples so far (win_tick() counts them) */
extern unsigned long long win_line_n;

/* the driver loaded and started on dev, its port opened and set up;
   0, or -1 with a message */
int win_start(win_dev *dev);

/* a line sample gone: each ms the timers due, then what can run; the
   ISR when the interrupt comes up */
void win_tick();

/* the port's output since the last call: into buf (WIN_SERIAL_READ
   octets of room); how many */
#define WIN_SERIAL_READ 2048
half::u32 win_serial_read(half::byte *buf);

/* no write to the port in flight? */
bool win_serial_writable();

/* n octets at p to the port, as many as one write takes; how many (0
   while a write is in flight) */
int win_serial_write(const void *p, int n);

#endif
