/*
 *    etc.h: ETC's transmit filter, as 3Com's Megahertz PC Cards have it
 *
 *    ETC (Enhanced Throughput Cellular, AT&T Paradyne's) on the 3Com
 *    3CXM556 and its family (the TC110 firmware, a TMS320C5x) is V.32bis
 *    and below with error control, and a 6-tap FIR on every transmitted
 *    sample while bit 5 of its ETC flags (our S114) is set.  The taps are
 *    Q14 (0x4000 a gain of 1), from the table AT+EF=n loads (the
 *    firmware's handler at 4:8ee5; set 5 its default, 4:878a).  Sets 1 to
 *    6 are symmetric low-passes about the third sample: they take back
 *    the high frequencies a cellular voice channel's pre-emphasis adds.
 *
 *    The firmware scales the filter's output by its transmit gain (data
 *    0x0392, from its attenuation in dB), not by the taps' own gain; we
 *    scale each set to a gain of 1 at 1800 Hz (V.32bis's carrier), so
 *    that the level of the carrier holds.  The taps run at the pump's
 *    rate (8000 or 9600 Hz) as the firmware's do at its V.32bis pump's
 *    7200 Hz.
 *
 *    The trace: the project's etc-cellular/firmware-trace.md.
 */
#ifndef DMODEM_CORE_ETC_H
#define DMODEM_CORE_ETC_H

#include <cmath>
#include <cstdint>

namespace dcore {

/* AT+EF's sets */
constexpr int ETC_FILTERS = 11, ETC_TAPS = 6;
constexpr int16_t etc_taps[ETC_FILTERS][ETC_TAPS] = {
	{ 0, 0, 0, 0, 0, 0x4000 },
	{ 0, -0x0076, 0x0443, 0x3FFE, 0x0443, -0x0076 },
	{ 0, -0x0091, 0x08A9, 0x4367, 0x08A9, -0x0091 },
	{ 0, -0x0056, 0x0D19, 0x44F0, 0x0D19, -0x0056 },
	{ 0, 0x0032, 0x1179, 0x4659, 0x1179, 0x0032 },
	{ 0, 0x00FB, 0x15B6, 0x47A2, 0x15B6, 0x00FB },
	{ 0, -0x0155, 0x0830, 0x31E5, 0x0830, -0x0155 },
	{ 0, 0x0001, 0x0031, 0x021E, 0x0BDE, 0x20C4 },
	{ 0x000B, 0x0083, 0x02F2, 0x0AB0, 0x15C3, 0x18B4 },
	{ 0, 0x0002, 0x0042, 0x0323, 0x12A0, 0x328F },
	{ 0x0010, 0x0106, 0x0615, 0x15E3, 0x2C9C, 0x328F },
};

/* set's gain (a ratio) at f Hz with the samples at rate Hz */
inline double etc_gain(int set, double f, double rate)
{
	double re = 0, im = 0;
	for (int k = 0; k < ETC_TAPS; k++) {
		const double w = 2 * M_PI * f / rate * k;
		re += etc_taps[set][k] / 16384.0 * cos(w);
		im -= etc_taps[set][k] / 16384.0 * sin(w);
	}
	return sqrt(re * re + im * im);
}

/* the filter on the transmitted samples */
class EtcFilter {
public:
	/* set (AT+EF) at rate Hz; a set out of range is set 5 */
	void start(int set, double rate)
	{
		if (set < 0 || set >= ETC_FILTERS)
			set = 5;
		const double g = etc_gain(set, 1800, rate);
		for (int k = 0; k < ETC_TAPS; k++) {
			c_[k] = static_cast<float>(etc_taps[set][k] / 16384.0 /
						   (g > 0 ? g : 1));
			x_[k] = 0;
		}
	}
	/* n samples, in place (clipped to 16 bits) */
	void process(int16_t *s, int n)
	{
		for (int i = 0; i < n; i++) {
			for (int k = 0; k < ETC_TAPS - 1; k++)
				x_[k] = x_[k + 1];
			x_[ETC_TAPS - 1] = s[i];
			float y = 0;
			for (int k = 0; k < ETC_TAPS; k++)
				y += c_[k] * x_[ETC_TAPS - 1 - k];
			s[i] = static_cast<int16_t>(
				y > 32767 ? 32767 : y < -32768 ? -32768 :
				lroundf(y));
		}
	}

private:
	float c_[ETC_TAPS] = {}, x_[ETC_TAPS] = {};
};

/* the landline side's detector of an ETC caller (the firmware's tone
   bank at 7200 Hz: a 1500 Hz band-pass, Q14 [-15735, 8311, -325, 0,
   325], about 50 Hz wide; its energy over a measurement both above a
   floor and above half of the band's sets the flags' bits 0, 2 and 5).
   We measure in blocks of 50 ms and need two in a row: an ETC caller
   sends 1500 Hz for 0.7 s every 2.2 s */
class EtcToneDetector {
public:
	/* the detector at rate Hz, nothing found */
	void start(double rate)
	{
		const double r = exp(-M_PI * 50 / rate);
		const double w = 2 * M_PI * 1500 / rate;
		a1_ = static_cast<float>(2 * r * cos(w));
		a2_ = static_cast<float>(-r * r);
		g_ = static_cast<float>(1 - r);
		block_ = static_cast<int>(rate / 20);
		x1_ = x2_ = y1_ = y2_ = 0;
		e_tone_ = e_all_ = 0;
		n_ = hits_ = 0;
		found_ = false;
	}
	/* n received samples; true once the tone is found */
	bool process(const int16_t *s, int n)
	{
		for (int i = 0; i < n && !found_; i++) {
			const float x = s[i];
			/* (b0 1, b2 -1: the firmware's band-pass) */
			const float y = g_ * (x - x2_) + a1_ * y1_ +
				a2_ * y2_;
			x2_ = x1_;
			x1_ = x;
			y2_ = y1_;
			y1_ = y;
			e_tone_ += y * y;
			e_all_ += x * x;
			if (++n_ < block_)
				continue;
			/* (the band-pass's gain is 1 at 1500 Hz) */
			const bool on = e_tone_ > 0.5f * e_all_ &&
				e_all_ / n_ > FLOOR;
			hits_ = on ? hits_ + 1 : 0;
			found_ = hits_ >= 2;
			e_tone_ = e_all_ = 0;
			n_ = 0;
		}
		return found_;
	}
	/* the tone was found */
	bool found() const { return found_; }

private:
	/* the least mean square (DP_DBM0_POWER: -43 dBm0) */
	static constexpr float FLOOR = 12500;
	float a1_ = 0, a2_ = 0, g_ = 0;
	float x1_ = 0, x2_ = 0, y1_ = 0, y2_ = 0;
	float e_tone_ = 0, e_all_ = 0;
	int block_ = 400, n_ = 0, hits_ = 0;
	bool found_ = false;
};

} // namespace dcore

#endif
