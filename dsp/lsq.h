/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    lsq.h - the normal equations of a least squares fit solved by
 *            Cholesky: the PCM receivers' equalisers (V.90's and
 *            K56flex's analogue modem) and V.34's echo canceller
 *
 */

#ifndef DSP_LSQ_H
#define DSP_LSQ_H

#include <cmath>

#include "dm_types.h"

namespace dsp {

/* R w = p for w, n unknowns: R symmetric, its upper triangle read (row
   major, n by n), with rel * trace(R) / n + abs added to its diagonal;
   L (n by n) is the work, its lower triangle R's Cholesky factor after.
   A pivot below 1e-12 is taken as 1e-12, so there is always an answer */
inline void lsq_solve(const double *R, const double *p, s16 n, double rel,
		      double abs, double *L, double *w)
{
	double tr = 0, lam;
	s16 i, j, m;

	for (i = 0; i < n; i++)
		tr += R[i * n + i];
	lam = rel * tr / n + abs;
	for (i = 0; i < n; i++)
		for (j = 0; j <= i; j++) {
			double s = j < i ? R[j * n + i] : R[i * n + i] + lam;
			for (m = 0; m < j; m++)
				s -= L[i * n + m] * L[j * n + m];
			if (i == j)
				L[i * n + i] = std::sqrt(s > 1e-12 ? s : 1e-12);
			else
				L[i * n + j] = s / L[j * n + j];
		}
	/* L z = p, then L^T w = z (in place) */
	for (i = 0; i < n; i++) {
		double s = p[i];
		for (m = 0; m < i; m++)
			s -= L[i * n + m] * w[m];
		w[i] = s / L[i * n + i];
	}
	for (i = n - 1; i >= 0; i--) {
		double s = w[i];
		for (m = i + 1; m < n; m++)
			s -= L[m * n + i] * w[m];
		w[i] = s / L[i * n + i];
	}
}

/* (see above) the same on fixed size arrays */
template <s16 N>
inline void lsq_solve(const double (&R)[N][N], const double (&p)[N],
		      double rel, double abs, double (&L)[N][N],
		      double (&w)[N])
{
	lsq_solve(&R[0][0], p, N, rel, abs, &L[0][0], w);
}

} // namespace dsp

#endif
