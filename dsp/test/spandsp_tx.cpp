/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 */

/* spandsp's side of spandsp_test.cpp (its headers clash with ours) */
#include <stdlib.h>
#include <spandsp.h>

/* spandsp's V.17 transmitter at bps (14400 to 7200 bit/s), its bits from
   get_bit(ctx); short_train: restarted for the short train (a page
   after the TCF's long one); NULL if spandsp has none */
void *sp_v17_tx(int bps, int short_train, int (*get_bit)(void *), void *ctx)
{
	v17_tx_state_t *s = v17_tx_init(NULL, bps, 0, get_bit, ctx);
	if (s && short_train)
		v17_tx_restart(s, bps, 0, 1);
	return s;
}

/* up to n samples of its signal into amp; how many (fewer once its
   data has ended) */
int sp_v17_samples(void *s, short *amp, int n)
{
	return v17_tx(static_cast<v17_tx_state_t*>(s), amp, n);
}

/* the transmitter freed */
void sp_v17_free(void *s)
{
	v17_tx_free(static_cast<v17_tx_state_t*>(s));
}

/* x through spandsp's mu-law, there and back */
int sp_ulaw(int x)
{
	return ulaw_to_linear(linear_to_ulaw(x));
}

/* what get_bit returns at the end of the data (SIG_STATUS_END_OF_DATA) */
int sp_end_of_data(void)
{
	return SIG_STATUS_END_OF_DATA;
}

/* spandsp's T.4 encoder: page <page> of <file> as a bit stream */
void *sp_t4_tx(const char *file, int page, int encoding, int min_bits)
{
	t4_tx_state_t *s = t4_tx_init(NULL, file, page, page);
	if (!s)
		return NULL;
	t4_tx_set_tx_encoding(s, encoding);
	t4_tx_set_min_bits_per_row(s, min_bits);
	if (t4_tx_start_page(s)) {
		t4_tx_free(s);
		return NULL;
	}
	return s;
}

/* the encoder freed */
void sp_t4_free(void *s)
{
	t4_tx_free(static_cast<t4_tx_state_t*>(s));
}

/* the next bit, or < 0 at the end of the page */
int sp_t4_bit(void *s)
{
	int b = t4_tx_get_bit(static_cast<t4_tx_state_t*>(s));
	return b < 0 || b > 1 ? -1 : b;
}

/* bits coded in encoding (2 MR, 3 MMR), as on the line, through
   spandsp's T.4 receiver into a TIFF file; the rows it decoded, -1 if
   it failed */
int sp_t4_rx(const char *file, int encoding, int width, int fine, const unsigned char *bits, size_t n)
{
	t4_rx_state_t *s = t4_rx_init(NULL, file, T4_COMPRESSION_ITU_T6);
	if (!s)
		return -1;
	t4_rx_set_rx_encoding(s, encoding);
	t4_rx_set_image_width(s, width);
	t4_rx_set_x_resolution(s, T4_X_RESOLUTION_R8);
	t4_rx_set_y_resolution(s, fine ? T4_Y_RESOLUTION_FINE : T4_Y_RESOLUTION_STANDARD);
	t4_rx_start_page(s);
	for (size_t i = 0; i < n; i++)
		if (t4_rx_put_bit(s, (bits[i / 8] >> (7 - i % 8)) & 1))
			break;
	t4_rx_end_page(s);
	t4_stats_t st;
	t4_rx_get_transfer_statistics(s, &st);
	t4_rx_free(s);
	return st.length;
}

/* spandsp's V.17 receiver at bps, the bits to put(ctx, bit) (status
   codes too: negative) */
void *sp_v17_rx(int bps, void (*put)(void *, int), void *ctx)
{
	return v17_rx_init(NULL, bps, put, ctx);
}

/* the receiver again for the next signal: short_train after a long
   train (its equalizer kept) */
void sp_v17_rx_restart(void *s, int bps, int short_train)
{
	v17_rx_restart(static_cast<v17_rx_state_t*>(s), bps, short_train);
}

/* n samples of the signal into the receiver */
void sp_v17_rx_samples(void *s, const short *amp, int n)
{
	v17_rx(static_cast<v17_rx_state_t*>(s), amp, n);
}

/* the receiver freed */
void sp_v17_rx_free(void *s)
{
	v17_rx_free(static_cast<v17_rx_state_t*>(s));
}
