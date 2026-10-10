/*
 *    t4_spandsp.cpp - spandsp's T.4 encoder (as in SendFAX) into our line
 *    receiver (t4_rx_bit), page 1 of a TIFF: the rows must be the same.
 *
 *    t4_spandsp <tiff> [encoding: 1 MH, 2 MR] [min bits per row]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../t4.h"

using namespace dsp;

void *sp_t4_tx(const char *file, int page, int encoding, int min_bits);
int sp_t4_bit(void *s);

int main(int argc, char **argv)
{
	TiffIn ti;
	T4Page ref, got;
	T4Rx rx;
	void *s;
	int enc = argc > 2 ? atoi(argv[2]) : 2, b, y, diff = 0;
	long nbits = 0;

	if (argc < 2 || ti.open(argv[1]) || ti.read_page(&ref) != 1)
		return 2;
	s = sp_t4_tx(argv[1], 0, enc, argc > 3 ? atoi(argv[3]) : 0);
	if (!s)
		return 2;
	got.init(ref.width, ref.fine);
	rx.init(&got, enc == 2);
	while ((b = sp_t4_bit(s)) >= 0) {
		nbits++;
		if (rx.bit(b))
			break;
	}
	rx.end();
	for (y = 0; y < ref.rows && y < got.rows; y++)
		if (memcmp(ref.bits + (size_t)y * 216, got.bits + (size_t)y * 216, 216)) {
			if (!diff)
				printf("row %d differs first\n", y);
			diff++;
		}
	printf("%ld bits, rows %d/%d, %d differ: %s\n", nbits, ref.rows, got.rows,
	       diff, diff || ref.rows != got.rows ? "FAIL" : "PASS");
	return diff != 0;
}
