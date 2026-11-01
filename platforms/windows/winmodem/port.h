/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    port.h - the COM port of D-Modem's virtual modem driver: what an app
 *             (or Unimodem's modem.sys above it) asks of a serial port,
 *             with dmodemd's modem on the port's other side (link.h,
 *             wire.h).
 *
 *    The app's side is a serial port's requests as Windows' serial.sys
 *    answers them:
 *      - reads with the serial timeouts (SERIAL_TIMEOUTS: at once, the
 *        first character or a total time, total and interval times);
 *      - writes, held while the transmit buffer is full, with their
 *        total time;
 *      - IOCTL_SERIAL_WAIT_ON_MASK and its events (EV_RXCHAR, EV_RXFLAG,
 *        EV_TXEMPTY, EV_CTS, EV_DSR, EV_RLSD, EV_RING, EV_RX80FULL);
 *      - the modem's lines (CTS, DSR, DCD, RI) with their deltas, and
 *        the app's DTR and RTS (set directly or by the handflow's DTR
 *        and RTS control), IOCTL_SERIAL_LSRMST_INSERT's in-band status;
 *      - the settings a port keeps (baud rate, line control, handflow,
 *        special characters, timeouts), purges, status, properties and
 *        counters.
 *    The modem's side (dmodemd, through the pipe) gives the characters
 *    for the app and its lines, and takes the app's characters.  Without
 *    dmodemd the lines are all off and the app's characters are
 *    dropped, as with a modem switched off.
 *
 *    Portable C++: the UMDF driver (driver.cpp) and the tests share it.
 *    Requests are the caller's (Request): the port holds the ones that
 *    wait and completes each once, with its own lock not held.
 *
 */
#ifndef DMODEM_WINMODEM_PORT_H
#define DMODEM_WINMODEM_PORT_H

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "dm_types.h"

namespace vmodem {

/* the NTSTATUS values requests complete with */
enum : u32 {
	ST_SUCCESS = 0x00000000, ST_TIMEOUT = 0x00000102,
	ST_PENDING = 0x00000103, ST_CANCELLED = 0xC0000120,
	ST_INVALID_PARAMETER = 0xC000000D, ST_BUFFER_TOO_SMALL = 0xC0000023,
	ST_NOT_SUPPORTED = 0xC00000BB,
};

/* the serial IOCTLs: CTL_CODE(FILE_DEVICE_SERIAL_PORT, fn,
   METHOD_BUFFERED, FILE_ANY_ACCESS) (ntddser.h's IOCTL_SERIAL_*) */
constexpr u32 serial_ioctl(u32 fn) { return 0x1b0000u | fn << 2; }
enum : u32 {
	IO_SET_BAUD_RATE = serial_ioctl(1), IO_SET_QUEUE_SIZE = serial_ioctl(2),
	IO_SET_LINE_CONTROL = serial_ioctl(3), IO_SET_BREAK_ON = serial_ioctl(4),
	IO_SET_BREAK_OFF = serial_ioctl(5), IO_IMMEDIATE_CHAR = serial_ioctl(6),
	IO_SET_TIMEOUTS = serial_ioctl(7), IO_GET_TIMEOUTS = serial_ioctl(8),
	IO_SET_DTR = serial_ioctl(9), IO_CLR_DTR = serial_ioctl(10),
	IO_RESET_DEVICE = serial_ioctl(11), IO_SET_RTS = serial_ioctl(12),
	IO_CLR_RTS = serial_ioctl(13), IO_SET_XOFF = serial_ioctl(14),
	IO_SET_XON = serial_ioctl(15), IO_GET_WAIT_MASK = serial_ioctl(16),
	IO_SET_WAIT_MASK = serial_ioctl(17), IO_WAIT_ON_MASK = serial_ioctl(18),
	IO_PURGE = serial_ioctl(19), IO_GET_BAUD_RATE = serial_ioctl(20),
	IO_GET_LINE_CONTROL = serial_ioctl(21), IO_GET_CHARS = serial_ioctl(22),
	IO_SET_CHARS = serial_ioctl(23), IO_GET_HANDFLOW = serial_ioctl(24),
	IO_SET_HANDFLOW = serial_ioctl(25),
	IO_GET_MODEMSTATUS = serial_ioctl(26),
	IO_GET_COMMSTATUS = serial_ioctl(27),
	IO_XOFF_COUNTER = serial_ioctl(28), IO_GET_PROPERTIES = serial_ioctl(29),
	IO_GET_DTRRTS = serial_ioctl(30), IO_LSRMST_INSERT = serial_ioctl(31),
	IO_CONFIG_SIZE = serial_ioctl(32), IO_GET_COMMCONFIG = serial_ioctl(33),
	IO_SET_COMMCONFIG = serial_ioctl(34), IO_GET_STATS = serial_ioctl(35),
	IO_CLEAR_STATS = serial_ioctl(36),
	IO_GET_MODEM_CONTROL = serial_ioctl(37),
	IO_SET_MODEM_CONTROL = serial_ioctl(38),
	IO_SET_FIFO_CONTROL = serial_ioctl(39),
};

/* the wait mask's events (winbase.h's EV_*) */
enum : u32 {
	PEV_RXCHAR = 0x0001, PEV_RXFLAG = 0x0002, PEV_TXEMPTY = 0x0004,
	PEV_CTS = 0x0008, PEV_DSR = 0x0010, PEV_RLSD = 0x0020,
	PEV_BREAK = 0x0040, PEV_ERR = 0x0080, PEV_RING = 0x0100,
	PEV_PERR = 0x0200, PEV_RX80FULL = 0x0400, PEV_EVENT1 = 0x0800,
	PEV_EVENT2 = 0x1000, PEV_ALL = 0x1fff,
};

/* the modem status register (SERIAL_MSR_*): deltas, then the lines */
enum : u8 {
	PMSR_DCTS = 0x01, PMSR_DDSR = 0x02, PMSR_TERI = 0x04, PMSR_DDCD = 0x08,
	PMSR_CTS = 0x10, PMSR_DSR = 0x20, PMSR_RI = 0x40, PMSR_DCD = 0x80,
};

/* the requests' structures, as ntddser.h has them */
struct SerialTimeouts {
	u32 read_interval, read_multiplier, read_constant;
	u32 write_multiplier, write_constant;
};
struct SerialHandflow {
	u32 control_handshake, flow_replace;
	s32 xon_limit, xoff_limit;
};
struct SerialChars {
	u8 eof, error, brk, event, xon, xoff;
};
struct SerialLineControl {
	u8 stop_bits, parity, word_length;
};
struct SerialStatus {
	u32 errors, hold_reasons, in_queue, out_queue;
	u8 eof_received, wait_for_immediate;
};
struct SerialStats {
	u32 received, transmitted, frame_errors, overrun_errors,
		buffer_overruns, parity_errors;
};
/* SERIAL_COMMPROP (its ProvChar[1] and the padding after it) */
struct SerialCommProp {
	u16 packet_length, packet_version;
	u32 service_mask, reserved1, max_tx_queue, max_rx_queue, max_baud;
	u32 prov_sub_type, prov_capabilities, settable_params;
	u32 settable_baud;
	u16 settable_data, settable_stop_parity;
	u32 current_tx_queue, current_rx_queue, prov_spec1, prov_spec2;
	u16 prov_char[2];
};

/* the handflow's bits (SERIAL_DTR_*, SERIAL_RTS_*) */
enum : u32 {
	HF_DTR_MASK = 0x03, HF_DTR_CONTROL = 0x01, HF_DTR_HANDSHAKE = 0x02,
	HF_NULL_STRIPPING = 0x08,
	HF_RTS_MASK = 0xc0, HF_RTS_CONTROL = 0x40, HF_RTS_HANDSHAKE = 0x80,
	HF_TRANSMIT_TOGGLE = 0xc0,
};
/* IOCTL_SERIAL_PURGE's (SERIAL_PURGE_*) */
enum : u32 {
	PRG_TXABORT = 1, PRG_RXABORT = 2, PRG_TXCLEAR = 4,
	PRG_RXCLEAR = 8,
};
/* IOCTL_SERIAL_GET_DTRRTS's, and the modem control register's DTR and
   RTS (SERIAL_DTR_STATE, SERIAL_MCR_DTR...) */
enum : u32 { DTRRTS_DTR = 1, DTRRTS_RTS = 2 };
/* LSRMST_INSERT's in-band status: escape, then what follows */
enum : u8 { LSRMST_ESCAPE = 0, LSRMST_LSR_DATA = 1,
		 LSRMST_LSR_NODATA = 2, LSRMST_MST = 3 };

/* an app's read, write or IOCTL_SERIAL_WAIT_ON_MASK while the port holds
   it.  data() is a read's room, a write's characters, a wait's output
   (4 bytes) */
class Request {
public:
	virtual ~Request() = default;
	virtual u8 *data() = 0;
	virtual size_t size() const = 0;
	/* done: the status, and the bytes read or written (a wait's 4);
	   called once, with the port's lock not held */
	virtual void complete(u32 status, size_t info) = 0;

private:
	friend class Port;
	size_t done_ = 0;               /* bytes so far */
	bool started_ = false;          /* the head of its queue: timed */
	u64 total_due_ = 0, interval_due_ = 0;       /* ms; 0: none */
};

/* the port's callbacks to its driver (called with the lock not held) */
class PortHost {
public:
	virtual ~PortHost() = default;
	/* characters for the modem (tx_take()) */
	virtual void tx_ready() {}
	/* room for the modem's characters again (rx_put()) */
	virtual void rx_room() {}
	/* the app's side (app_state(), baud()) has changed */
	virtual void state_changed() {}
	/* the next timeout is at due (ms by the clock), 0: none; tick()
	   then */
	virtual void timer(u64 /*due*/) {}
};

/* see above */
class Port {
public:
	enum : size_t { RX_SIZE = 16384, TX_SIZE = 4096 };
	typedef u64 (*Clock)();

	/* a port reporting to host (may be null), its time by clock (ms) */
	Port(PortHost *host, Clock clock);

	/* the app's side: its handle opened, closed (every request it has
	   waiting cancelled, DTR and RTS dropped, the buffers emptied) */
	void open();
	void close();
	/* a read or a write: completed now or held */
	void read(Request *r);
	void write(Request *r);
	/* an IOCTL: its status, and in *info the bytes of out it filled;
	   IO_WAIT_ON_MASK takes r and gives ST_PENDING (r completes then,
	   now or later), unless it is refused */
	u32 ioctl(u32 code, const void *in, size_t inlen, void *out,
		       size_t outlen, size_t *info, Request *r);
	/* r no longer held (true) to be completed as cancelled by the
	   caller, with done(r) bytes; false: not held (completed or about
	   to be) */
	bool cancel(Request *r);
	/* bytes a request has moved so far */
	static size_t done(const Request *r) { return r->done_; }
	/* the timeouts that are due */
	void tick();

	/* the modem's side: dmodemd there or not (not: its lines off, the
	   app's characters dropped) */
	void connect(bool up);
	/* the modem's lines (LINE_*) */
	void set_lines(u8 lines);
	/* the modem's characters for the app: how many were taken (the
	   rest when there is room: PortHost::rx_room) */
	size_t rx_put(const u8 *p, size_t n);
	/* up to max of the app's characters for the modem */
	size_t tx_take(u8 *p, size_t max);
	/* the app's side (STATE_*), its baud rate */
	u8 app_state();
	u32 baud();

private:
	/* what the public calls collect under the lock and do after */
	struct Batch {
		std::vector<std::pair<Request *, std::pair<u32, size_t>>>
			done;
		bool tx = false, room = false, state = false;
	};
	void finish(Batch &b, Request *r, u32 status, size_t info);
	void run(Batch &b, std::unique_lock<std::mutex> &lk);
	void event(Batch &b, u32 ev);
	void pump_reads(Batch &b, u64 now);
	void pump_writes(Batch &b, u64 now);
	void after(Batch &b);
	void set_dtr_rts(Batch &b, bool dtr, bool rts);
	void apply_handflow(Batch &b);
	void put_msr(u8 msr_old);
	u8 msr() const;
	/* the receive buffer's room */
	size_t rx_room_left() const { return RX_SIZE - rx_.size(); }
	bool rx_push(u8 c);
	u32 do_ioctl(Batch &b, u32 code, const void *in,
			  size_t inlen, void *out, size_t outlen,
			  size_t *info, Request *r);
	u64 next_due() const;

	PortHost *host_;
	Clock clock_;
	std::mutex mu_;

	bool open_ = false, connected_ = false;
	bool dtr_ = false, rts_ = false;
	u8 lines_ = 0;                  /* the modem's (LINE_*) */
	u8 deltas_ = 0;                 /* PMSR_D*, TERI since read */
	u32 baud_ = 115200;
	SerialLineControl lc_ = { 0, 0, 8 };
	SerialHandflow hf_ = { HF_DTR_CONTROL, HF_RTS_CONTROL, 0, 0 };
	SerialChars chars_ = { 0, 0, 0, 0, 0x11, 0x13 };
	SerialTimeouts to_ = {};
	SerialStats stats_ = {};
	u32 wait_mask_ = 0, events_ = 0;
	u8 lsrmst_ = 0;                 /* the escape, 0: none */
	bool tx_busy_ = false;          /* written since TXEMPTY */
	bool rx_full_ = false;          /* RX80FULL given */
	bool rx_blocked_ = false;       /* rx_put() turned some away */

	std::deque<u8> rx_, tx_;
	std::deque<Request *> reads_, writes_;
	Request *wait_ = nullptr;
	u64 due_ = 0;                   /* the timer as last given */
};

} // namespace vmodem

#endif
