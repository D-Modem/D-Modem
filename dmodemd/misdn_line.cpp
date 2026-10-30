/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    misdn_line.cpp - an ISDN basic rate interface through mISDN for
 *                     the modem's line (misdn_line.h)
 *
 *    mISDN's sockets (PF_ISDN): each message a struct mISDNhead (the
 *    primitive, an id) and its data.  Its interface is the kernel's
 *    include/linux/mISDNif.h, which Linux does not export to user
 *    space: what is used of it is here.
 *
 */

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <log.h>

#include "dmodem_g711.h"
#include "misdn_line.h"
#include "portable.h"

#define ERR(fmt, ...) dcore::log_printf(0, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "misdn: " fmt, ##__VA_ARGS__)
#define DBG2(fmt, ...) dcore::log_printf(2, "misdn: " fmt, ##__VA_ARGS__)

namespace dmd {

namespace {

/*
 *    mISDN's interface (include/linux/mISDNif.h)
 */

const int PF_MISDN = 34;                /* PF_ISDN, AF_ISDN */
enum : int {
	ISDN_P_BASE = 0x00, ISDN_P_TE_S0 = 0x01, ISDN_P_LAPD_TE = 0x10,
	ISDN_P_B_RAW = 0x21,
};
enum : u32 {
	PH_ACTIVATE_REQ = 0x0101, PH_DEACTIVATE_REQ = 0x0201,
	PH_DATA_REQ = 0x2001, PH_ACTIVATE_IND = 0x0102,
	PH_ACTIVATE_CNF = 0x4102, PH_DEACTIVATE_IND = 0x0202,
	PH_DEACTIVATE_CNF = 0x4202, PH_DATA_IND = 0x2002,
	DL_ESTABLISH_REQ = 0x1004, DL_DATA_REQ = 0x3004,
	DL_ESTABLISH_IND = 0x1008, DL_ESTABLISH_CNF = 0x5008,
	DL_RELEASE_IND = 0x1108, DL_RELEASE_CNF = 0x5108,
	DL_DATA_IND = 0x3008, DL_UNITDATA_IND = 0x3108,
};
const u32 MISDN_ID_ANY = 0xFFFF;
const u8 GROUP_TEI = 127;
const size_t HEADER = 8;                /* struct mISDNhead */
const size_t MAX_DATA = 2048;

struct SockaddrMisdn {
	sa_family_t family;
	unsigned char dev;
	unsigned char channel;
	unsigned char sapi;
	unsigned char tei;
};

struct DevInfo {
	unsigned int id;
	unsigned int Dprotocols;
	unsigned int Bprotocols;
	unsigned int protocol;
	unsigned char channelmap[16];
	unsigned int nrbchan;
	char name[20];
};

/* (_IOR('I', 67, int), _IOR('I', 68, int)) */
const unsigned long IMGETCOUNT = _IOR('I', 67, int);
const unsigned long IMGETDEVINFO = _IOR('I', 68, int);

/* the ring signal's period, ms */
const u64 RING_MS = 5000;

/* an octet's bits reversed */
u8 flipped(u8 v)
{
	v = static_cast<u8>((v & 0xF0) >> 4 | (v & 0x0F) << 4);
	v = static_cast<u8>((v & 0xCC) >> 2 | (v & 0x33) << 2);
	return static_cast<u8>((v & 0xAA) >> 1 | (v & 0x55) << 1);
}

/* a message's octets in hex, for the log */
std::string hex(const u8 *p, s32 n)
{
	std::string s;
	char b[4];
	for (s32 i = 0; i < n; i++) {
		snprintf(b, sizeof(b), " %02x", p[i]);
		s += b;
	}
	return s;
}

/* a bound socket of protocol on dev's channel (sapi 0, tei), made
   non-blocking; -1 (errno) if it can't be */
int misdn_socket(int type, int protocol, u8 dev, u8 channel, u8 tei)
{
	int fd = socket(PF_MISDN, type, protocol);
	if (fd < 0)
		return -1;
	SockaddrMisdn a;
	memset(&a, 0, sizeof(a));
	a.family = PF_MISDN;
	a.dev = dev;
	a.channel = channel;
	a.tei = tei;
	if (bind(fd, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) < 0 ||
	    set_nonblocking(fd) < 0) {
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	return fd;
}

/* the device dev names (its number, or its name) into info; false with
   the reason on stderr */
bool find_device(const char *dev, DevInfo &info)
{
	int base = socket(PF_MISDN, SOCK_RAW, ISDN_P_BASE);
	if (base < 0) {
		fprintf(stderr, "error: --misdn: no mISDN (%s): the kernel's "
			"mISDN_core and a card's driver loaded?\n",
			strerror(errno));
		return false;
	}
	int count = 0;
	if (ioctl(base, IMGETCOUNT, &count) < 0 || count <= 0) {
		fprintf(stderr, "error: --misdn: no mISDN devices\n");
		close(base);
		return false;
	}
	bool numeric = *dev != '\0';
	for (const char *p = dev; *p; p++)
		if (!isdigit(static_cast<unsigned char>(*p)))
			numeric = false;
	/* (the ids are the devices' numbers, with gaps where one has
	   gone) */
	bool found = false;
	for (int id = 0, seen = 0; id < 256 && seen < count && !found; id++) {
		memset(&info, 0, sizeof(info));
		info.id = static_cast<unsigned int>(id);
		if (ioctl(base, IMGETDEVINFO, &info) < 0)
			continue;
		seen++;
		info.name[sizeof(info.name) - 1] = '\0';
		found = numeric ? id == atoi(dev) : !strcmp(info.name, dev);
	}
	close(base);
	if (!found) {
		fprintf(stderr, "error: --misdn: no mISDN device %s\n", dev);
		return false;
	}
	if (!(info.Dprotocols & (1u << ISDN_P_TE_S0))) {
		fprintf(stderr, "error: --misdn: %s (%u) is no S0 terminal "
			"interface\n", info.name, info.id);
		return false;
	}
	return true;
}

} // namespace

/* see misdn_line.h */
MisdnLine::~MisdnLine()
{
	b_close();
	if (d_ >= 0)
		close(d_);
}

/* see misdn_line.h */
bool MisdnLine::open(const char *dev, const char *msn, bool flip)
{
	DevInfo info;
	if (!find_device(dev, info))
		return false;
	dev_ = static_cast<u8>(info.id);
	flip_ = flip;
	q931_.set_msn(msn ? msn : "");
	d_ = misdn_socket(SOCK_DGRAM, ISDN_P_LAPD_TE, dev_, 0, GROUP_TEI);
	if (d_ < 0) {
		fprintf(stderr, "error: --misdn: %s's D-channel: %s\n",
			info.name, strerror(errno));
		return false;
	}
	DBG("%s (%u): %u B-channels, MSN %s\n", info.name, info.id,
	    info.nrbchan, msn && *msn ? msn : "any");
	return true;
}

/* see misdn_line.h */
void MisdnLine::watch(RunLoop &loop)
{
	loop.watch(d_);
	if (b_ >= 0)
		loop.watch(b_);
}

/* see misdn_line.h */
s16 MisdnLine::pump(dcore::Modem &m)
{
	m_ = &m;
	if (!d_receive())
		return -1;
	if (b_ >= 0)
		b_receive();
	const u64 now = now_ms();
	q931_.poll(now);
	if (ringing_ && now >= next_ring_) {
		next_ring_ = now + RING_MS;
		m.ring_signal(true);
	}
	return 0;
}

/* see misdn_line.h */
s16 MisdnLine::start(dcore::Modem &m)
{
	m_ = &m;
	const u64 now = now_ms();
	if (m.caller()) {
		udi_ = m.sreg(SREG_DP) == DP_CLEAR;
		connected_ = false;
		DBG("dialling %s (%s)\n", m.dial_string(),
		    udi_ ? "unrestricted digital" : "3.1 kHz audio");
		if (q931_.dial(m.dial_string(), udi_ ? Q931::Bearer::Udi :
			       Q931::Bearer::Audio, now) < 0) {
			ERR("misdn: cannot dial %s\n", m.dial_string());
			return -1;
		}
		return 0;
	}
	if (q931_.state() == Q931::U7) {
		DBG("answering\n");
		ringing_ = false;
		q931_.answer(now);
	} else {
		DBG("answering: no call\n");
	}
	return 0;
}

/* see misdn_line.h */
s16 MisdnLine::stop(dcore::Modem &m)
{
	m_ = &m;
	DBG("on hook\n");
	q931_.hangup(now_ms());
	b_close();
	ringing_ = false;
	udi_ = false;
	return 0;
}

/* see misdn_line.h */
dcore::Result MisdnLine::result_of(s16 cause)
{
	switch (cause) {
	case Q931::USER_BUSY:
		return dcore::Result::Busy;
	case Q931::NO_USER_RESPONDING:
	case Q931::NO_ANSWER:
		return dcore::Result::NoAnswer;
	default:
		return dcore::Result::NoCarrier;
	}
}

/* see misdn_line.h: an I frame, once layer 2 is up (it is asked to
   come up for the first) */
void MisdnLine::q931_send(const u8 *msg, s16 n)
{
	DBG2("D >%s\n", hex(msg, n).c_str());
	if (l2_up_) {
		send(d_, DL_DATA_REQ, msg, n);
		return;
	}
	queue_.emplace_back(msg, msg + n);
	if (!l2_asked_) {
		l2_asked_ = true;
		send(d_, DL_ESTABLISH_REQ, nullptr, 0);
	}
}

/* see misdn_line.h: its socket, activated */
void MisdnLine::q931_channel(s16 b)
{
	if (b_ >= 0 && channel_ == b)
		return;
	b_close();
	b_ = misdn_socket(SOCK_DGRAM, ISDN_P_B_RAW, dev_,
			  static_cast<u8>(b), 0);
	if (b_ < 0) {
		ERR("misdn: B%d: %s\n", b, strerror(errno));
		return;
	}
	channel_ = b;
	DBG("B%d\n", b);
	send(b_, PH_ACTIVATE_REQ, nullptr, 0);
}

/* see misdn_line.h */
void MisdnLine::q931_offered(const Q931::Call &c)
{
	DBG("call from %s to %s on B%d%s\n",
	    c.withheld ? "(withheld)" : c.calling.empty() ? "?" :
	    c.calling.c_str(), c.called.c_str(), c.channel,
	    c.bearer == Q931::Bearer::Udi ? ", unrestricted digital" : "");
	if (!m_)
		return;
	udi_ = c.bearer == Q931::Bearer::Udi;
	connected_ = false;
	m_->caller_id("", c.withheld ? nullptr : c.calling.c_str(), "");
	ringing_ = true;
	next_ring_ = now_ms() + RING_MS;
	m_->ring_signal(true);
}

/* see misdn_line.h */
void MisdnLine::q931_connected()
{
	DBG("connected\n");
	connected_ = true;
	if (m_ && m_->caller())
		m_->line_answered();
}

/* see misdn_line.h: a call that rang stops ringing; one we dialled
   that was never answered fails with the cause's result */
void MisdnLine::q931_ended(s16 cause)
{
	DBG("call ended: cause %d\n", cause);
	b_close();
	if (ringing_) {
		ringing_ = false;
		udi_ = false;
		if (m_)
			m_->ring_signal(false);
		return;
	}
	if (!m_ || !m_->started())
		return;
	if (m_->caller() && !connected_)
		m_->line_failed(result_of(cause));
	m_->line_dropped();
}

/* see misdn_line.h */
bool MisdnLine::send(int fd, u32 prim, const u8 *data, s32 n)
{
	u8 buf[HEADER + MAX_DATA];
	if (n < 0 || static_cast<size_t>(n) > MAX_DATA)
		return false;
	const u32 id = MISDN_ID_ANY;
	memcpy(buf, &prim, 4);
	memcpy(buf + 4, &id, 4);
	if (n)
		memcpy(buf + HEADER, data, static_cast<size_t>(n));
	if (::send(fd, buf, HEADER + static_cast<size_t>(n), 0) < 0) {
		ERR("misdn: send %#x: %s\n", prim, strerror(errno));
		return false;
	}
	return true;
}

/* see misdn_line.h */
bool MisdnLine::d_receive()
{
	u8 buf[HEADER + MAX_DATA];
	for (;;) {
		ssize_t n = recv(d_, buf, sizeof(buf), 0);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK ||
			    errno == EINTR)
				return true;
			ERR("misdn: D-channel: %s\n", strerror(errno));
			return false;
		}
		if (n == 0) {
			ERR("misdn: D-channel closed\n");
			return false;
		}
		if (static_cast<size_t>(n) < HEADER)
			continue;
		u32 prim;
		memcpy(&prim, buf, 4);
		const u8 *data = buf + HEADER;
		const s16 len = static_cast<s16>(static_cast<size_t>(n) - HEADER);
		switch (prim) {
		case DL_ESTABLISH_IND:
		case DL_ESTABLISH_CNF:
			DBG2("layer 2 up\n");
			l2_up_ = true;
			l2_asked_ = false;
			for (const auto &msg : queue_)
				send(d_, DL_DATA_REQ, msg.data(),
				     static_cast<s32>(msg.size()));
			queue_.clear();
			break;
		case DL_RELEASE_IND:
		case DL_RELEASE_CNF:
			DBG2("layer 2 down\n");
			l2_up_ = false;
			/* (a request that failed: what waited for it
			   lost, Q.931's timers to tell) */
			if (l2_asked_)
				queue_.clear();
			l2_asked_ = false;
			break;
		case DL_DATA_IND:
		case DL_UNITDATA_IND:
			DBG2("D <%s\n", hex(data, len).c_str());
			q931_.receive(data, len, now_ms());
			break;
		default:
			DBG2("D: primitive %#x\n", prim);
			break;
		}
	}
}

/* see misdn_line.h */
void MisdnLine::b_receive()
{
	u8 buf[HEADER + MAX_DATA];
	while (b_ >= 0) {
		ssize_t n = recv(b_, buf, sizeof(buf), 0);
		if (n < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK &&
			    errno != EINTR) {
				ERR("misdn: B%d: %s\n", channel_,
				    strerror(errno));
				b_close();
			}
			return;
		}
		if (n == 0) {
			b_close();
			return;
		}
		if (static_cast<size_t>(n) < HEADER)
			continue;
		u32 prim;
		memcpy(&prim, buf, 4);
		switch (prim) {
		case PH_ACTIVATE_IND:
		case PH_ACTIVATE_CNF:
			if (!b_active_) {
				DBG("B%d active\n", channel_);
				b_active_ = true;
				/* (20 ms ahead: what the card takes before
				   our answers to its octets come) */
				u8 idle[160];
				memset(idle, udi_ ? 0xFF :
				       flip_ ? flipped(0xD5) : 0xD5,
				       sizeof(idle));
				send(b_, PH_DATA_REQ, idle, sizeof(idle));
			}
			break;
		case PH_DEACTIVATE_IND:
		case PH_DEACTIVATE_CNF:
			DBG("B%d inactive\n", channel_);
			b_active_ = false;
			break;
		case PH_DATA_IND:
			b_data(buf + HEADER, static_cast<s32>(
				static_cast<size_t>(n) - HEADER));
			break;
		default:
			break;
		}
	}
}

/* see misdn_line.h: the octets as A-law samples, while the modem has
   the line */
void MisdnLine::b_data(const u8 *data, s32 n)
{
	if (!m_ || !m_->started() || n <= 0)
		return;
	u8 in[MAX_DATA], out[MAX_DATA];
	for (s32 i = 0; i < n; i++)
		in[i] = flip_ ? flipped(data[i]) : data[i];
	m_->set_line_law(2);
	dcore::Modem &m = *m_;
	dmodem_g711_blocks(1, in, out, n,
			   [&m](const s16 *li, s16 *lo, s16 k) {
				   m.process(li, lo, k);
			   });
	if (flip_)
		for (s32 i = 0; i < n; i++)
			out[i] = flipped(out[i]);
	if (b_ >= 0)
		send(b_, PH_DATA_REQ, out, n);
}

/* see misdn_line.h */
void MisdnLine::b_close()
{
	if (b_ < 0)
		return;
	if (b_active_)
		send(b_, PH_DEACTIVATE_REQ, nullptr, 0);
	close(b_);
	DBG("B%d closed\n", channel_);
	b_ = -1;
	channel_ = 0;
	b_active_ = false;
}

} // namespace dmd
