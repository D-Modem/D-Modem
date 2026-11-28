/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    jpeg.h - ITU-T T.81 sequential JPEG as T.4 Annex E uses it for
 *             colour and grey-scale fax (8-bit baseline, 12-bit extended,
 *             the G3FAX APP1 segments), and the conversions between sRGB
 *             and T.42's CIELAB (any gamut and illuminant of T.4 E.6.6
 *             and E.6.7) and ITU-YCC, in 8 or 12 bits
 *
 */

#ifndef __JPEG_H__
#define __JPEG_H__

#include <cstddef>
#include <vector>

#include "dm_types.h"

namespace dsp {

/* T.42's colour spaces */
#define T42_CIELAB      0               /* 6.2.1, for hard copy */
#define T42_YCC         1               /* 6.2.2, ITU-YCC (sYCC), for soft copy */

/* T.4 E.6.7's illuminant codes (G3FAX2), their four octets as a number */
#define T42_D50         0x00443530u
#define T42_D65         0x00443635u
#define T42_D75         0x00443735u
#define T42_SA          0x00005341u
#define T42_SC          0x00005343u
#define T42_F2          0x00004632u
#define T42_F7          0x00004637u
#define T42_F11         0x00463131u
#define T42_CT(kelvin)  (0x43540000u | ((kelvin) & 0xffffu))  /* "CT", a colour temperature */

/* a CIELAB gamut as T.4 E.6.6 (G3FAX1) gives it: for L*, a* and b* the
   offset of the zero point (in the 8 most significant bits of a
   sample) and the span of the range.  The default is T.42 6.2.1.3's */
struct T42Gamut {
	s16 offset[3] = {0, 128, 96};
	s16 range[3] = {100, 170, 200};

	/* true when both give the same gamut */
	bool operator==(const T42Gamut &o) const
	{
		for (int i = 0; i < 3; i++)
			if (offset[i] != o.offset[i] || range[i] != o.range[i])
				return false;
		return true;
	}

	/* true when they give different gamuts */
	bool operator!=(const T42Gamut &o) const
	{
		return !(*this == o);
	}
};

/* what the samples of an image mean: the colour space, the bits of a
   sample, and (CIELAB only) the gamut and the illuminant */
struct T42Colour {
	s16 space = T42_CIELAB;
	s16 bits = 8;                   /* 8 or 12 in JPEG; 1 to 16 in the
					   conversions */
	T42Gamut gamut;
	u32 illuminant = T42_D50;
};

/* an image, comps (1 or 3) samples a pel, a row after another: in pix
   for 8-bit samples, in pix16 for more bits (colour.bits) */
struct JpegImage {
	u32 width = 0, height = 0;
	s16 comps = 0;
	std::vector<unsigned char> pix;
	std::vector<u16> pix16;
	s16 dpi = 0;                    /* G3FAX APP1's resolution (0: none) */
	T42Colour colour;               /* the decoder sets bits, and the gamut
					   and illuminant of G3FAX1 and G3FAX2
					   (else the defaults); it cannot know
					   YCC (T.30 gives it), the caller sets
					   it */
};

/* how an image is coded */
struct JpegOptions {
	s16 quality = 75;               /* 1 to 100, scaling T.81 K.1/K.2 */
	s16 subsample = 1;              /* 3 components: a* and b* at half
					   the pels each way (T.4's 4:1:1) */
	s16 dpi = 200;                  /* in the G3FAX APP1 (0: none) */
	u16 restart = 0;                /* MCUs between RSTn (0: none) */
	s16 dnl = 0;                    /* lines given by DNL after the scan,
					   Y 0 in the frame header */
	s16 colour_info = 0;            /* CIELAB: G3FAX1 and G3FAX2 also for
					   the default gamut and D50 (without
					   it, only for others) */
};

/* a JPEG of img.colour.bits samples: SOI, the G3FAX APP1 (and G3FAX1,
   G3FAX2 for a CIELAB gamut or illuminant that is not the default),
   DQT, DHT, SOF0 (8 bits, T.81's K.3 to K.6 tables) or SOF1 (12 bits,
   Huffman tables made for the image, T.81 K.2), one interleaved scan,
   EOI.  false (and nothing) when bits is not 8 or 12, or the samples
   are too few */
bool jpeg_encode(const JpegImage &img, const JpegOptions &opt, std::vector<unsigned char> *out);

/* a baseline (SOF0) or extended Huffman (SOF1, 8 or 12-bit) JPEG's
   first frame, its components brought to the full size, in pix (8
   bits) or pix16 (12 bits); 0 when whole, 1 when the data ended early
   (the lines so far, the rest at mid level), -1 when it is none; at
   most max_rows lines.  *used (if given): its octets, up to its EOI */
s16 jpeg_decode(const unsigned char *data, size_t len, u32 max_rows, JpegImage *img,
		size_t *used = nullptr);

/* the CIE XYZ white point (Y 1) of a T.4 E.6.7 illuminant code (D50 and
   D65 as T.42 gives them, the others from CIE 15; a colour temperature
   on the CIE daylight locus from 4000 K, on the Planckian locus from
   1667 K to 4000 K); false when it is not one of these */
bool t42_white(u32 illuminant, double xyz[3]);

/* n pels of sRGB (3 octets each) to T.42 samples of colour c, and back;
   false (and nothing) when c is not one that we convert (bits not 1 to
   16, a range not above 0, an illuminant that t42_white does not know) */
bool t42_from_rgb(const T42Colour &c, const unsigned char *rgb, u16 *out, size_t n);
bool t42_to_rgb(const T42Colour &c, const u16 *in, unsigned char *rgb, size_t n);
/* the same for grey-scale: L* (or Y) from an sRGB grey level, and back */
bool t42_from_grey(const T42Colour &c, const unsigned char *grey, u16 *out, size_t n);
bool t42_to_grey(const T42Colour &c, const u16 *in, unsigned char *grey, size_t n);

/* img's samples (pix or pix16, for its width, height, comps and colour)
   from sRGB (comps 3) or grey levels (comps 1); false as t42_from_rgb */
bool t42_image_from_rgb(JpegImage *img, const unsigned char *pix);
/* img's samples to sRGB or grey levels, img.comps octets a pel; false
   as t42_to_rgb, or when its samples are too few */
bool t42_image_to_rgb(const JpegImage &img, std::vector<unsigned char> *pix);

/* T.42's default CIELAB (D50, L* 0 to 100, a* -85 to 85, b* -75 to 125
   in 8 bits, T.4 E.6.4) from sRGB, and back; n pels, 3 octets each */
void t42_rgb_to_lab(const unsigned char *rgb, unsigned char *lab, size_t n);
void t42_lab_to_rgb(const unsigned char *lab, unsigned char *rgb, size_t n);
/* the same for grey-scale: L* from an sRGB grey level, and back */
void t42_grey_to_l(const unsigned char *grey, unsigned char *l, size_t n);
void t42_l_to_grey(const unsigned char *l, unsigned char *grey, size_t n);

} // namespace dsp

#endif /* __JPEG_H__ */
