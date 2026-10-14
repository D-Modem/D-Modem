/*
 *
 *    hw/ac97_codec.hpp - an AC'97 modem codec's registers (MC97: the
 *                        Silicon Labs Si3038 of the Agere modem, the
 *                        Motorola one of the SM56, the Conexant one of the
 *                        HSF): a mixin of a harness
 *
 *    Its registers as the controller reads and writes them (an
 *    Ich<Host>'s codec_read() and codec_write()): its IDs read only
 *    (extended modem ID 0x3c, vendor ID 0x7c and 0x7e), its sample rate
 *    at 0x40, line 1's ADC and DAC ready at 0x3e as soon as they are
 *    asked for, the rest kept as written.
 *
 *    A vendor's registers (Host, a half::HalfModem): Host's own
 *    codec_write_hook(r, v), which sees each write first and can change
 *    it, or do it all (true: done, nothing more is stored or logged).
 *
 */

#ifndef HW_AC97_CODEC_HPP
#define HW_AC97_CODEC_HPP

#include "../modem_half.hpp"

namespace half {

template <class Host>
class Ac97Codec {
public:
	word codec_regs[0x80] = {};
	/* its sample rate, as register 0x40 has it */
	unsigned codec_rate = 8000;

	/* its registers at reset: the IDs (extended modem ID: its lines
	   and codec ID; the vendor's, "CX" "T)" for instance), 8000 Hz */
	Ac97Codec(word modem_id, word vendor_id1, word vendor_id2)
	{
		codec_regs[0x3c] = modem_id;
		codec_regs[0x40] = 8000;
		codec_regs[0x7c] = vendor_id1;
		codec_regs[0x7e] = vendor_id2;
	}

	/* register r */
	word codec_read(int r)
	{
		r &= 0x7e;
		if (r == 0x3e)
			/* ADC and DAC of line 1, MREF, GPIO ready (bits 0..3),
			   as written above */
			return (codec_regs[r] & 0xff00) | 0x000f;
		return codec_regs[r];
	}

	/* register r = v (the IDs read only); 0x40 is the sample rate */
	void codec_write(int r, word v)
	{
		r &= 0x7e;
		if (r == 0x3c || r == 0x7c || r == 0x7e)
			return;
		if (host().codec_write_hook(r, v))
			return;
		if (codec_regs[r] != v && host().dbg)
			host().log("codec %02x = %04x\n", r, v);
		if (r == 0x40 && v && v != codec_rate) {
			codec_rate = v;
			if (host().dbg)
				host().log("codec rate %u Hz\n", v);
		}
		codec_regs[r] = v;
	}

	/* a write of register r = v, before it is done: v as it is to be,
	   or true if done (none: as it is) */
	bool codec_write_hook(int r, word &v)
	{
		(void)r; (void)v;
		return false;
	}

private:
	Host &host() { return static_cast<Host &>(*this); }
};

}

#endif
