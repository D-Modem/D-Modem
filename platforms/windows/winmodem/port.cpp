/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    port.cpp - the virtual modem's COM port (port.h)
 *
 *    The serial semantics are those Windows documents for serial.sys
 *    (the Serial Controller Driver Design Guide, SERIAL_TIMEOUTS, the
 *    IOCTL_SERIAL_* pages).
 *
 */

#include "dm_types.h"
#include "port.h"

#include <cstring>

#include "wire.h"

namespace vmodem {

static_assert(sizeof(SerialTimeouts) == 20, "SERIAL_TIMEOUTS");
static_assert(sizeof(SerialHandflow) == 16, "SERIAL_HANDFLOW");
static_assert(sizeof(SerialChars) == 6, "SERIAL_CHARS");
static_assert(sizeof(SerialLineControl) == 3, "SERIAL_LINE_CONTROL");
static_assert(sizeof(SerialStatus) == 20, "SERIAL_STATUS");
static_assert(sizeof(SerialStats) == 24, "SERIALPERF_STATS");
static_assert(sizeof(SerialCommProp) == 64, "SERIAL_COMMPROP");

namespace {

const u32 MAXT = 0xffffffffu;

/* SERIAL_COMMPROP's values: an RS-232 port (SERIAL_SP_RS232) taking any
   baud rate (SERIAL_BAUD_USER), 5 to 8 data bits, every stop bit and
   parity setting, both timeouts and the handshakes */
const u32 SP_SERIALCOMM = 1, SP_RS232 = 1, BAUD_USER = 0x10000000,
	PCF_ALL = 0x1ff, SP_ALL = 0x7f, BAUD_ALL = 0x1007ffff;
const u16 DATABITS_ALL = 0x0f, STOPPARITY_ALL = 0x1f07;

} // namespace

/* see port.h */
Port::Port(PortHost *host, Clock clock) : host_(host), clock_(clock) {}

/* r done: completed once the lock is let go */
void Port::finish(Batch &b, Request *r, u32 status, size_t info)
{
	b.done.push_back({ r, { status, info } });
}

/* the batch's completions and calls, with the lock let go */
void Port::run(Batch &b, std::unique_lock<std::mutex> &lk)
{
	after(b);
	u64 due = next_due();
	bool timer = due != due_;
	due_ = due;
	lk.unlock();
	for (auto &d : b.done)
		d.first->complete(d.second.first, d.second.second);
	if (host_) {
		if (b.tx)
			host_->tx_ready();
		if (b.room)
			host_->rx_room();
		if (b.state)
			host_->state_changed();
		if (timer)
			host_->timer(due);
	}
}

/* what follows every change: the transmit buffer's emptying
   (EV_TXEMPTY), the receive buffer's room for rx_put() again */
void Port::after(Batch &b)
{
	if (tx_busy_ && tx_.empty() && writes_.empty()) {
		tx_busy_ = false;
		event(b, PEV_TXEMPTY);
	}
	if (rx_blocked_ && rx_room_left() >= RX_SIZE / 4) {
		rx_blocked_ = false;
		b.room = true;
	}
	if (rx_.size() < RX_SIZE * 8 / 10)
		rx_full_ = false;
}

/* ev happened: a waiting IOCTL_SERIAL_WAIT_ON_MASK given it if the mask
   has it, else kept for the next */
void Port::event(Batch &b, u32 ev)
{
	ev &= wait_mask_;
	if (!ev)
		return;
	events_ |= ev;
	if (wait_) {
		u32 got = events_;
		events_ = 0;
		memcpy(wait_->data(), &got, 4);
		finish(b, wait_, ST_SUCCESS, 4);
		wait_ = nullptr;
	}
}

/* the earliest of the head read's and the head write's timeouts, 0 if
   neither has one */
u64 Port::next_due() const
{
	u64 due = 0;
	auto at = [&due](u64 t) {
		if (t && (!due || t < due))
			due = t;
	};
	if (!reads_.empty()) {
		at(reads_.front()->total_due_);
		at(reads_.front()->interval_due_);
	}
	if (!writes_.empty())
		at(writes_.front()->total_due_);
	return due;
}

/* the reads, in turn, given what has come; the head read timed from
   when it became the head (SERIAL_TIMEOUTS):
     - interval MAXULONG, multiplier and constant 0: at once, with what
       there is;
     - interval and multiplier MAXULONG, constant c: at once if there is
       something, else at the first character or after c ms;
     - else full, or the total time (multiplier * length + constant)
       gone, or the interval gone between two characters */
void Port::pump_reads(Batch &b, u64 now)
{
	while (!reads_.empty()) {
		Request *r = reads_.front();
		const SerialTimeouts &t = to_;
		bool at_once = t.read_interval == MAXT &&
			t.read_multiplier == 0 && t.read_constant == 0;
		bool first = t.read_interval == MAXT &&
			t.read_multiplier == MAXT && t.read_constant != 0 &&
			t.read_constant != MAXT;
		size_t want = r->size();

		if (!r->started_) {
			r->started_ = true;
			u64 total = 0;
			if (first)
				total = t.read_constant;
			else if (!at_once && (t.read_multiplier ||
					      t.read_constant))
				total = static_cast<u64>(
						t.read_multiplier) *
						want +
					t.read_constant;
			r->total_due_ = total ? now + total : 0;
		}
		size_t got = 0;
		while (r->done_ < want && !rx_.empty()) {
			r->data()[r->done_++] = rx_.front();
			rx_.pop_front();
			got++;
		}
		if (got && !at_once && !first && t.read_interval &&
		    t.read_interval != MAXT)
			r->interval_due_ = now + t.read_interval;

		u32 st = ST_SUCCESS;
		bool end = r->done_ == want || at_once ||
			(first && r->done_ > 0);
		if (!end && ((r->total_due_ && now >= r->total_due_) ||
			     (r->interval_due_ && now >= r->interval_due_))) {
			end = true;
			st = ST_TIMEOUT;
		}
		if (!end)
			break;
		reads_.pop_front();
		finish(b, r, st, r->done_);
	}
}

/* the writes, in turn, into the transmit buffer (all of it at once
   without dmodemd: dropped); the head write timed by the total time
   (multiplier * length + constant) */
void Port::pump_writes(Batch &b, u64 now)
{
	while (!writes_.empty()) {
		Request *r = writes_.front();
		size_t want = r->size();

		if (!r->started_) {
			r->started_ = true;
			u64 total =
				static_cast<u64>(to_.write_multiplier) *
					want +
				to_.write_constant;
			r->total_due_ = total ? now + total : 0;
		}
		if (!connected_) {
			stats_.transmitted +=
				static_cast<u32>(want - r->done_);
			r->done_ = want;
		}
		size_t k = 0;
		while (r->done_ < want && tx_.size() < TX_SIZE) {
			tx_.push_back(r->data()[r->done_++]);
			k++;
		}
		if (k) {
			stats_.transmitted += static_cast<u32>(k);
			b.tx = true;
		}
		if (want)
			tx_busy_ = true;
		u32 st = ST_SUCCESS;
		if (r->done_ < want) {
			if (!r->total_due_ || now < r->total_due_)
				break;
			st = ST_TIMEOUT;
		}
		writes_.pop_front();
		finish(b, r, st, r->done_);
	}
}

/* the modem status register: the lines (none without dmodemd) and the
   deltas since it was read */
u8 Port::msr() const
{
	u8 l = connected_ ? lines_ : 0, m = deltas_;
	if (l & LINE_CTS)
		m |= PMSR_CTS;
	if (l & LINE_DSR)
		m |= PMSR_DSR;
	if (l & LINE_RI)
		m |= PMSR_RI;
	if (l & LINE_DCD)
		m |= PMSR_DCD;
	return m;
}

/* a received character into the buffer, escaped for LSRMST_INSERT:
   false if there is no room */
bool Port::rx_push(u8 c)
{
	if (lsrmst_ && c == lsrmst_) {
		if (rx_room_left() < 2)
			return false;
		rx_.push_back(c);
		rx_.push_back(LSRMST_ESCAPE);
		return true;
	}
	if (rx_room_left() < 1)
		return false;
	rx_.push_back(c);
	return true;
}

/* the modem status as it now is after old, in-band for LSRMST_INSERT
   (escape, LSRMST_MST, the register with its deltas) if it changed */
void Port::put_msr(u8 msr_old)
{
	u8 now = msr();
	if (!lsrmst_ || (now & 0xf0) == (msr_old & 0xf0))
		return;
	if (rx_room_left() >= 3) {
		rx_.push_back(lsrmst_);
		rx_.push_back(LSRMST_MST);
		rx_.push_back(now);
	}
}

/* the app's DTR and RTS: the app's state given to dmodemd when they
   change */
void Port::set_dtr_rts(Batch &b, bool dtr, bool rts)
{
	if (dtr != dtr_ || rts != rts_) {
		dtr_ = dtr;
		rts_ = rts;
		b.state = true;
	}
}

/* DTR and RTS as the handflow sets them: on with its control or
   handshake (a virtual port's buffer has room: handshake's line stays
   up), off with neither */
void Port::apply_handflow(Batch &b)
{
	set_dtr_rts(b, (hf_.control_handshake & HF_DTR_MASK) != 0,
		    (hf_.flow_replace & HF_RTS_MASK) != 0);
}

/* see port.h: DTR and RTS as the handflow has them; the deltas, the
   events and the counters from nothing */
void Port::open()
{
	Batch b;
	std::unique_lock<std::mutex> lk(mu_);
	open_ = true;
	b.state = true;
	deltas_ = 0;
	events_ = 0;
	wait_mask_ = 0;
	lsrmst_ = 0;
	stats_ = {};
	rx_.clear();
	tx_.clear();
	apply_handflow(b);
	run(b, lk);
}

/* see port.h */
void Port::close()
{
	Batch b;
	std::unique_lock<std::mutex> lk(mu_);
	for (Request *r : reads_)
		finish(b, r, ST_CANCELLED, r->done_);
	for (Request *r : writes_)
		finish(b, r, ST_CANCELLED, r->done_);
	reads_.clear();
	writes_.clear();
	if (wait_) {
		finish(b, wait_, ST_CANCELLED, 0);
		wait_ = nullptr;
	}
	rx_.clear();
	tx_.clear();
	tx_busy_ = false;
	wait_mask_ = 0;
	events_ = 0;
	lsrmst_ = 0;
	set_dtr_rts(b, false, false);
	open_ = false;
	b.state = true;
	b.room = rx_blocked_;
	rx_blocked_ = false;
	run(b, lk);
}

/* see port.h */
void Port::read(Request *r)
{
	Batch b;
	std::unique_lock<std::mutex> lk(mu_);
	r->done_ = 0;
	r->started_ = false;
	r->total_due_ = r->interval_due_ = 0;
	if (r->size() == 0) {
		finish(b, r, ST_SUCCESS, 0);
	} else {
		reads_.push_back(r);
		pump_reads(b, clock_());
	}
	run(b, lk);
}

/* see port.h */
void Port::write(Request *r)
{
	Batch b;
	std::unique_lock<std::mutex> lk(mu_);
	r->done_ = 0;
	r->started_ = false;
	r->total_due_ = r->interval_due_ = 0;
	if (r->size() == 0) {
		finish(b, r, ST_SUCCESS, 0);
	} else {
		writes_.push_back(r);
		pump_writes(b, clock_());
	}
	run(b, lk);
}

/* see port.h */
bool Port::cancel(Request *r)
{
	Batch b;
	std::unique_lock<std::mutex> lk(mu_);
	bool found = false;
	for (auto *q : { &reads_, &writes_ }) {
		for (auto it = q->begin(); it != q->end(); ++it) {
			if (*it == r) {
				q->erase(it);
				found = true;
				break;
			}
		}
	}
	if (wait_ == r) {
		wait_ = nullptr;
		found = true;
	}
	/* (the next read or write is the head now: timed from now) */
	u64 now = clock_();
	pump_reads(b, now);
	pump_writes(b, now);
	run(b, lk);
	return found;
}

/* see port.h */
void Port::tick()
{
	Batch b;
	std::unique_lock<std::mutex> lk(mu_);
	u64 now = clock_();
	pump_reads(b, now);
	pump_writes(b, now);
	run(b, lk);
}

/* see port.h: dmodemd gone takes its lines and the characters it had
   yet to take */
void Port::connect(bool up)
{
	Batch b;
	std::unique_lock<std::mutex> lk(mu_);
	u8 old = msr(), was = connected_ ? lines_ : 0;
	connected_ = up;
	lines_ = 0;
	if (!up) {
		tx_.clear();
		pump_writes(b, clock_());
	}
	if (was & LINE_CTS) {
		deltas_ |= PMSR_DCTS;
		event(b, PEV_CTS);
	}
	if (was & LINE_DSR) {
		deltas_ |= PMSR_DDSR;
		event(b, PEV_DSR);
	}
	if (was & LINE_DCD) {
		deltas_ |= PMSR_DDCD;
		event(b, PEV_RLSD);
	}
	if (was & LINE_RI)
		deltas_ |= PMSR_TERI;
	if (open_)
		put_msr(old);
	pump_reads(b, clock_());
	run(b, lk);
}

/* see port.h: each line's change a delta and an event (RI's rising
   edge EV_RING, its falling one TERI, as a UART has it) */
void Port::set_lines(u8 lines)
{
	Batch b;
	std::unique_lock<std::mutex> lk(mu_);
	if (!connected_) {
		run(b, lk);
		return;
	}
	u8 old = msr(), ch = static_cast<u8>(lines ^ lines_);
	lines_ = lines;
	if (ch & LINE_CTS) {
		deltas_ |= PMSR_DCTS;
		event(b, PEV_CTS);
	}
	if (ch & LINE_DSR) {
		deltas_ |= PMSR_DDSR;
		event(b, PEV_DSR);
	}
	if (ch & LINE_DCD) {
		deltas_ |= PMSR_DDCD;
		event(b, PEV_RLSD);
	}
	if (ch & LINE_RI) {
		if (lines & LINE_RI)
			event(b, PEV_RING);
		else
			deltas_ |= PMSR_TERI;
	}
	if (open_)
		put_msr(old);
	pump_reads(b, clock_());
	run(b, lk);
}

/* see port.h: with the app's port closed the characters go nowhere;
   NUL stripped if the handflow says so; the event character EV_RXFLAG;
   the buffer at 80% EV_RX80FULL */
size_t Port::rx_put(const u8 *p, size_t n)
{
	Batch b;
	std::unique_lock<std::mutex> lk(mu_);
	size_t i = 0;
	if (!open_) {
		run(b, lk);
		return n;
	}
	bool any = false, flag = false;
	for (; i < n; i++) {
		u8 c = p[i];
		if (c == 0 && (hf_.flow_replace & HF_NULL_STRIPPING))
			continue;
		if (!rx_push(c)) {
			rx_blocked_ = true;
			break;
		}
		any = true;
		stats_.received++;
		if (c == chars_.event)
			flag = true;
	}
	if (any)
		event(b, PEV_RXCHAR);
	if (flag)
		event(b, PEV_RXFLAG);
	if (!rx_full_ && rx_.size() >= RX_SIZE * 8 / 10) {
		rx_full_ = true;
		event(b, PEV_RX80FULL);
	}
	pump_reads(b, clock_());
	run(b, lk);
	return i;
}

/* see port.h */
size_t Port::tx_take(u8 *p, size_t max)
{
	Batch b;
	std::unique_lock<std::mutex> lk(mu_);
	size_t k = 0;
	while (k < max && !tx_.empty()) {
		p[k++] = tx_.front();
		tx_.pop_front();
	}
	if (k)
		pump_writes(b, clock_());
	run(b, lk);
	return k;
}

/* see port.h */
u8 Port::app_state()
{
	std::lock_guard<std::mutex> lk(mu_);
	return static_cast<u8>((open_ ? STATE_OPEN : 0) |
				    (dtr_ ? STATE_DTR : 0) |
				    (rts_ ? STATE_RTS : 0));
}

/* see port.h */
u32 Port::baud()
{
	std::lock_guard<std::mutex> lk(mu_);
	return baud_;
}

/* see port.h */
u32 Port::ioctl(u32 code, const void *in, size_t inlen, void *out,
		     size_t outlen, size_t *info, Request *r)
{
	Batch b;
	std::unique_lock<std::mutex> lk(mu_);
	*info = 0;
	u32 st = do_ioctl(b, code, in, inlen, out, outlen, info, r);
	run(b, lk);
	return st;
}

/* ioctl() under the lock */
u32 Port::do_ioctl(Batch &b, u32 code, const void *in,
			size_t inlen, void *out, size_t outlen, size_t *info,
			Request *r)
{
	/* in: at least n bytes of input; give: v (n bytes) as the output */
	auto need = [&](size_t n) { return inlen >= n && in; };
	auto give = [&](const void *v, size_t n) -> u32 {
		if (outlen < n || !out)
			return ST_BUFFER_TOO_SMALL;
		memcpy(out, v, n);
		*info = n;
		return ST_SUCCESS;
	};
	u32 u = 0;

	switch (code) {
	case IO_SET_BAUD_RATE:
		if (!need(4))
			return ST_BUFFER_TOO_SMALL;
		memcpy(&u, in, 4);
		if (!u)
			return ST_INVALID_PARAMETER;
		if (u != baud_) {
			baud_ = u;
			b.state = true;
		}
		return ST_SUCCESS;
	case IO_GET_BAUD_RATE:
		return give(&baud_, 4);
	case IO_SET_LINE_CONTROL: {
		SerialLineControl lc;
		if (!need(sizeof(lc)))
			return ST_BUFFER_TOO_SMALL;
		memcpy(&lc, in, sizeof(lc));
		if (lc.word_length < 5 || lc.word_length > 8 ||
		    lc.stop_bits > 2 || lc.parity > 4 ||
		    (lc.stop_bits == 1 && lc.word_length != 5) ||
		    (lc.stop_bits == 2 && lc.word_length == 5))
			return ST_INVALID_PARAMETER;
		lc_ = lc;
		return ST_SUCCESS;
	}
	case IO_GET_LINE_CONTROL:
		return give(&lc_, sizeof(lc_));
	case IO_SET_TIMEOUTS: {
		SerialTimeouts t;
		if (!need(sizeof(t)))
			return ST_BUFFER_TOO_SMALL;
		memcpy(&t, in, sizeof(t));
		if (t.read_interval == MAXT && t.read_multiplier == MAXT &&
		    t.read_constant == MAXT)
			return ST_INVALID_PARAMETER;
		to_ = t;
		return ST_SUCCESS;
	}
	case IO_GET_TIMEOUTS:
		return give(&to_, sizeof(to_));
	case IO_SET_HANDFLOW: {
		SerialHandflow h;
		if (!need(sizeof(h)))
			return ST_BUFFER_TOO_SMALL;
		memcpy(&h, in, sizeof(h));
		if ((h.control_handshake & HF_DTR_MASK) == HF_DTR_MASK ||
		    h.xon_limit < 0 || h.xoff_limit < 0 ||
		    static_cast<size_t>(h.xon_limit) > RX_SIZE ||
		    static_cast<size_t>(h.xoff_limit) > RX_SIZE)
			return ST_INVALID_PARAMETER;
		hf_ = h;
		apply_handflow(b);
		return ST_SUCCESS;
	}
	case IO_GET_HANDFLOW:
		return give(&hf_, sizeof(hf_));
	case IO_SET_CHARS: {
		SerialChars c;
		if (!need(sizeof(c)))
			return ST_BUFFER_TOO_SMALL;
		memcpy(&c, in, sizeof(c));
		if (c.xon == c.xoff && (hf_.flow_replace & 0x03))
			return ST_INVALID_PARAMETER;
		chars_ = c;
		return ST_SUCCESS;
	}
	case IO_GET_CHARS:
		return give(&chars_, sizeof(chars_));
	case IO_SET_DTR:
	case IO_CLR_DTR:
		if ((hf_.control_handshake & HF_DTR_MASK) == HF_DTR_HANDSHAKE)
			return ST_INVALID_PARAMETER;
		set_dtr_rts(b, code == IO_SET_DTR, rts_);
		return ST_SUCCESS;
	case IO_SET_RTS:
	case IO_CLR_RTS: {
		u32 m = hf_.flow_replace & HF_RTS_MASK;
		if (m == HF_RTS_HANDSHAKE || m == HF_TRANSMIT_TOGGLE)
			return ST_INVALID_PARAMETER;
		set_dtr_rts(b, dtr_, code == IO_SET_RTS);
		return ST_SUCCESS;
	}
	case IO_GET_DTRRTS:
		u = (dtr_ ? static_cast<u32>(DTRRTS_DTR) : 0u) |
		    (rts_ ? static_cast<u32>(DTRRTS_RTS) : 0u);
		return give(&u, 4);
	case IO_GET_MODEM_CONTROL:
		/* (MCR: DTR, RTS, and OUT2 as a PC's UART has it) */
		u = (dtr_ ? 1 : 0) | (rts_ ? 2 : 0) | 8;
		return give(&u, 4);
	case IO_SET_MODEM_CONTROL:
		if (!need(4))
			return ST_BUFFER_TOO_SMALL;
		memcpy(&u, in, 4);
		set_dtr_rts(b, (u & 1) != 0, (u & 2) != 0);
		return ST_SUCCESS;
	case IO_GET_MODEMSTATUS:
		u = msr();
		deltas_ = 0;
		return give(&u, 4);
	case IO_SET_WAIT_MASK:
		if (!need(4))
			return ST_BUFFER_TOO_SMALL;
		memcpy(&u, in, 4);
		if (u & ~PEV_ALL)
			return ST_INVALID_PARAMETER;
		/* (a wait waiting ends with no event) */
		if (wait_) {
			u32 none = 0;
			memcpy(wait_->data(), &none, 4);
			finish(b, wait_, ST_SUCCESS, 4);
			wait_ = nullptr;
		}
		wait_mask_ = u;
		events_ = 0;
		return ST_SUCCESS;
	case IO_GET_WAIT_MASK:
		return give(&wait_mask_, 4);
	case IO_WAIT_ON_MASK:
		if (!r || r->size() < 4)
			return ST_BUFFER_TOO_SMALL;
		if (wait_ || !wait_mask_)
			return ST_INVALID_PARAMETER;
		r->done_ = 0;
		if (events_) {
			u32 got = events_;
			events_ = 0;
			memcpy(r->data(), &got, 4);
			finish(b, r, ST_SUCCESS, 4);
		} else {
			wait_ = r;
		}
		return ST_PENDING;
	case IO_PURGE:
		if (!need(4))
			return ST_BUFFER_TOO_SMALL;
		memcpy(&u, in, 4);
		if (!u || (u & ~0x0fu))
			return ST_INVALID_PARAMETER;
		if (u & PRG_TXABORT) {
			for (Request *q : writes_)
				finish(b, q, ST_CANCELLED, q->done_);
			writes_.clear();
		}
		if (u & PRG_RXABORT) {
			for (Request *q : reads_)
				finish(b, q, ST_CANCELLED, q->done_);
			reads_.clear();
		}
		if (u & PRG_TXCLEAR)
			tx_.clear();
		if (u & PRG_RXCLEAR)
			rx_.clear();
		return ST_SUCCESS;
	case IO_GET_COMMSTATUS: {
		SerialStatus s = {};
		s.in_queue = static_cast<u32>(rx_.size());
		s.out_queue = static_cast<u32>(tx_.size());
		for (Request *q : writes_)
			s.out_queue +=
				static_cast<u32>(q->size() - q->done_);
		return give(&s, sizeof(s));
	}
	case IO_GET_PROPERTIES: {
		SerialCommProp p = {};
		p.packet_length = sizeof(p);
		p.packet_version = 2;
		p.service_mask = SP_SERIALCOMM;
		p.max_tx_queue = TX_SIZE;
		p.max_rx_queue = RX_SIZE;
		p.max_baud = BAUD_USER;
		p.prov_sub_type = SP_RS232;
		p.prov_capabilities = PCF_ALL;
		p.settable_params = SP_ALL;
		p.settable_baud = BAUD_ALL;
		p.settable_data = DATABITS_ALL;
		p.settable_stop_parity = STOPPARITY_ALL;
		p.current_tx_queue = TX_SIZE;
		p.current_rx_queue = RX_SIZE;
		/* (kernel32 asks with its COMMPROP, ProvChar[1]: 62 bytes and
		   the padding) */
		if (outlen < 62 || !out)
			return ST_BUFFER_TOO_SMALL;
		return give(&p, outlen < sizeof(p) ? 62 : sizeof(p));
	}
	case IO_GET_STATS:
		return give(&stats_, sizeof(stats_));
	case IO_CLEAR_STATS:
		stats_ = {};
		return ST_SUCCESS;
	case IO_LSRMST_INSERT:
		if (!need(1))
			return ST_BUFFER_TOO_SMALL;
		lsrmst_ = *static_cast<const u8 *>(in);
		return ST_SUCCESS;
	case IO_IMMEDIATE_CHAR:
		if (!need(1))
			return ST_BUFFER_TOO_SMALL;
		if (connected_) {
			if (tx_.size() >= TX_SIZE)
				return ST_INVALID_PARAMETER;
			tx_.push_back(*static_cast<const u8 *>(in));
			b.tx = true;
		}
		tx_busy_ = true;
		stats_.transmitted++;
		return ST_SUCCESS;
	case IO_CONFIG_SIZE:
		/* (none: kernel32's GetCommConfig makes its own) */
		u = 0;
		return give(&u, 4);
	case IO_SET_QUEUE_SIZE:
		return need(8) ? ST_SUCCESS : ST_BUFFER_TOO_SMALL;
	case IO_SET_BREAK_ON:
	case IO_SET_BREAK_OFF:
	case IO_SET_XOFF:
	case IO_SET_XON:
	case IO_RESET_DEVICE:
	case IO_SET_FIFO_CONTROL:
		return ST_SUCCESS;
	case IO_XOFF_COUNTER:
	case IO_GET_COMMCONFIG:
	case IO_SET_COMMCONFIG:
		return ST_NOT_SUPPORTED;
	default:
		return ST_INVALID_PARAMETER;
	}
}

} // namespace vmodem
