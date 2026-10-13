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
	if (t4_tx_start_page(s))
		return NULL;
	return s;
}

/* the next bit, or < 0 at the end of the page */
int sp_t4_bit(void *s)
{
	int b = t4_tx_get_bit(static_cast<t4_tx_state_t*>(s));
	return b < 0 || b > 1 ? -1 : b;
}
