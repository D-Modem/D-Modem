/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    fax_hdlc.h - HDLC framing of fax frames on the FaxTx / FaxRx bit
 *                 callbacks, as mixins for the machines that use it:
 *                 class 1 (+FTH / +FRH) and T.30 (V.21 frames and the
 *                 ECM frames on the fast modulation)
 *
 *    The transmitter sends flags, then the frames its host gives it with
 *    their frame check sequence and the stuffed zeros, a flag between
 *    them, then closing flags.  The receiver takes out the flags, stuffed
 *    zeros and aborts and hands the host what came before each flag.
 *
 *    A host H derives from HdlcTx<H, MAX, Tag> and / or HdlcRx<H, MAX,
 *    Tag> (Tag an empty type naming the use, so that a host may have
 *    two) and has
 *
 *        int hdlc_tx_next(Tag, unsigned char *buf, int max);
 *        void hdlc_rx_frame(Tag, const unsigned char *d, int len,
 *                           bool whole);
 *
 */

#ifndef DSP_FAX_HDLC_H
#define DSP_FAX_HDLC_H

#include "bits.h"
#include "fax.h"

namespace dsp {

/* the uses: control frames on V.21 channel 2, ECM frames (T.4 Annex A)
   on the fast modulation */
struct HdlcV21 {};
struct HdlcEcm {};

/* frames of up to MAX octets (the FCS not counted) out.  The host's
   hdlc_tx_next(Tag(), buf, MAX) puts the next frame in buf and returns
   its length, 0 when there is none yet (a flag goes out and it is asked
   again after it), -1 when there are no more */
template <class Host, int MAX, class Tag>
class HdlcTx {
public:
	/* FaxTx bit callback, ctx the host */
	static int hdlc_tx_cb(void *ctx)
	{
		return static_cast<HdlcTx*>(static_cast<Host*>(ctx))->hdlc_tx_bit();
	}

	/* the next bit to send, FAX_BIT_END after the closing flags */
	int hdlc_tx_bit()
	{
		int bit;

		/* a stuffed zero, also after the FCS's last bit */
		if (htx_ones == 5) {
			htx_ones = 0;
			return 0;
		}
		if (htx_pos < htx_len) {
			bit = (htx_buf[htx_pos] >> htx_bit) & 1;
			if (++htx_bit == 8) {
				htx_bit = 0;
				if (++htx_pos == htx_len) {
					/* one flag at least to the next */
					htx_flags = 0;
					htx_flag_bit = 0;
					htx_min = 8;
				}
			}
			htx_ones = bit ? htx_ones + 1 : 0;
			return bit;
		}

		/* at a flag boundary: the next frame, the end, or a flag */
		if (htx_flag_bit == 0) {
			if (!htx_closing && htx_flags >= htx_min) {
				int n = host().hdlc_tx_next(Tag(), htx_buf, MAX);
				if (n > 0) {
					unsigned crc = hdlc_fcs(htx_buf, n) ^ 0xffff;
					htx_buf[n] = crc & 0xff;
					htx_buf[n + 1] = crc >> 8;
					htx_len = n + 2;
					htx_pos = htx_bit = htx_ones = 0;
					return hdlc_tx_bit();
				}
				if (n < 0)
					htx_closing = 1;
			}
			if (htx_closing && htx_flags >= htx_close)
				return FAX_BIT_END;
		}
		bit = (0x7e >> htx_flag_bit) & 1;
		htx_flag_bit = (htx_flag_bit + 1) & 7;
		htx_flags++;
		return bit;
	}

protected:
	/* lead flag bits before the first frame (whole flags, so up to 7
	   more), close after the last one */
	void hdlc_tx_start(long lead, long close)
	{
		htx_len = htx_pos = htx_bit = htx_ones = 0;
		htx_flag_bit = 0;
		htx_flags = 0;
		htx_min = lead;
		htx_close = close;
		htx_closing = 0;
	}

	/* no more frames: the closing flags (counted from the end of the
	   last frame) once the frame going out is through */
	void hdlc_tx_close() { htx_closing = 1; }
	/* (see hdlc_tx_close()) */
	bool hdlc_tx_closing() const { return htx_closing; }

private:
	/* the modem this is part of */
	Host &host() { return static_cast<Host&>(*this); }

	unsigned char htx_buf[MAX + 2];         /* the frame and its FCS */
	int htx_len, htx_pos, htx_bit;          /* going out: octet, bit */
	int htx_ones;                           /* ones in a row in it */
	int htx_flag_bit;                       /* of the flag going out */
	long htx_flags;                         /* flag bits since a frame */
	long htx_min, htx_close;                /* before a frame, the end */
	int htx_closing;
};

/* frames of up to MAX octets (with their FCS) in.  At each flag the
   host's hdlc_rx_frame(Tag(), d, len, whole) gets the len octets before
   it: whole when they were a whole number of octets, MAX at most */
template <class Host, int MAX, class Tag>
class HdlcRx {
public:
	/* FaxRx bit callback, ctx the host */
	static void hdlc_rx_cb(void *ctx, int bit)
	{
		static_cast<HdlcRx*>(static_cast<Host*>(ctx))->hdlc_rx_bit(bit);
	}

	/* a received bit: flags, stuffed zeros and aborts (7 ones) */
	void hdlc_rx_bit(int bit)
	{
		if (bit) {
			if (++hrx_ones > 6) {           /* idle or abort */
				hrx_clear();
				return;
			}
		} else {
			if (hrx_ones == 6) {            /* a flag */
				/* its 0111111 went in already: a whole
				   number of octets before it */
				host().hdlc_rx_frame(Tag(), hrx_buf, hrx_len,
						     hrx_nbits == 7 && !hrx_over);
				hrx_ones = 0;
				hrx_clear();
				return;
			}
			if (hrx_ones == 5) {            /* stuffed */
				hrx_ones = 0;
				return;
			}
			hrx_ones = 0;
		}
		hrx_byte |= static_cast<unsigned>(bit) << hrx_nbits;
		if (++hrx_nbits == 8) {
			if (hrx_len < MAX)
				hrx_buf[hrx_len++] =
					static_cast<unsigned char>(hrx_byte);
			else
				hrx_over = 1;
			hrx_nbits = 0;
			hrx_byte = 0;
		}
	}

protected:
	/* nothing received yet */
	void hdlc_rx_reset()
	{
		hrx_ones = 0;
		hrx_clear();
	}

	/* no bits since the last flag (or abort, or the reset) */
	bool hdlc_rx_idle() const
	{
		return !hrx_len && !hrx_nbits && !hrx_over;
	}

private:
	/* the modem this is part of */
	Host &host() { return static_cast<Host&>(*this); }

	/* no frame in, its bits dropped */
	void hrx_clear()
	{
		hrx_len = hrx_nbits = hrx_over = 0;
		hrx_byte = 0;
	}

	unsigned char hrx_buf[MAX];
	int hrx_len, hrx_nbits;         /* octets in, bits of the next */
	int hrx_ones;                   /* ones in a row */
	int hrx_over;                   /* longer than MAX */
	unsigned hrx_byte;
};

} // namespace dsp

#endif
