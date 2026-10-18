/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    codec_probe.cpp - a signal through a mobile network's speech codec
 *                      (codec_sim.h), for measuring what the codec does
 *                      to it: 16 bit samples at 8000 Hz from stdin,
 *                      the codec's out on stdout, one frame (20 ms)
 *                      later
 *
 *    codec_probe <codec>:<kbit/s>[:dtx] [<loss %> [<seed> [<rate>]]]
 *
 *    rate 16000 (AMR-WB only) takes and gives the codec's own 16000 Hz
 *    samples.
 *
 */
#include <stdio.h>
#include <stdlib.h>

#include "codec_sim.h"

/* the samples on stdin through the codec of argv[1] onto stdout, the
   frames and erasures to stderr: 0, 1 on an error, 2 for the usage */
int main(int argc, char *argv[])
{
	dsp::SpeechCodec codec;
	const char *err;
	short buf[1024];
	size_t n;

	if (argc < 2) {
		fprintf(stderr, "usage: codec_probe amr:12.2 | amrwb:23.85 "
			"[<loss %%> [<seed> [<rate>]]]\n");
		return 2;
	}
	if (codec.init(argv[1], argc > 2 ? atof(argv[2]) : 0,
		       argc > 3 ? (unsigned)atoi(argv[3]) : 1, &err,
		       argc > 4 ? atoi(argv[4]) : 8000)) {
		fprintf(stderr, "codec_probe: %s\n", err);
		return 1;
	}
	while ((n = fread(buf, 2, sizeof(buf) / 2, stdin)) > 0) {
		for (size_t i = 0; i < n; i++)
			buf[i] = (short)codec.sample(buf[i]);
		if (fwrite(buf, 2, n, stdout) != n)
			return 1;
	}
	fprintf(stderr, "codec_probe: %s, %ld frames, %ld erased\n",
		codec.name(), codec.frames(), codec.erased());
	return 0;
}
