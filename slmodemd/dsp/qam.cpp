/*
 *
 *    qam.cpp - generic QAM transmitter and receiver, V.17/V.32bis trellis
 *            coding with a Viterbi decoder, line echo canceller
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "qam.h"


/*
 *    transmitter
 */

void qam_tx_init(struct qam_tx *q, int srate, float carrier, float baud,
		 float alpha, int span, float rms_symbol, float amplitude)
{
	int i, n;

	memset(q, 0, sizeof(*q));
	q->srate = srate;
	q->sps = srate / baud;
	q->span = span > QAM_MAX_SPAN ? QAM_MAX_SPAN : span;
	n = 2 * q->span * QAM_PULSE_RES;
	for (i = 0; i <= n + 1; i++)
		q->pulse[i] = rrc_pulse((float)i / QAM_PULSE_RES - q->span, alpha);
	/* unit energy pulses: the pass band rms is amplitude / sqrt(2) */
	q->scale = amplitude / rms_symbol;
	osc_set_freq(&q->carrier, carrier, srate);
}

void qam_tx_flush(struct qam_tx *q)
{
	memset(q->sym, 0, sizeof(q->sym));
}

static float qam_pulse(struct qam_tx *q, float t)
{
	float p = (t + q->span) * QAM_PULSE_RES;
	int i;
	if (p < 0 || p >= 2 * q->span * QAM_PULSE_RES)
		return 0;
	i = (int)p;
	p -= i;
	return q->pulse[i] + p * (q->pulse[i + 1] - q->pulse[i]);
}

float qam_tx_sample(struct qam_tx *q)
{
	cplx s = { 0, 0 }, c;
	int k;

	q->t += 1.0f / q->sps;
	if (q->t >= 1) {
		q->t -= 1;
		memmove(q->sym + 1, q->sym,
			(2 * q->span - 1) * sizeof(q->sym[0]));
		q->sym[0] = q->get_symbol(q->ctx);
	}
	for (k = 0; k < 2 * q->span; k++) {
		float g = qam_pulse(q, q->t + k - q->span);
		s.re += g * q->sym[k].re;
		s.im += g * q->sym[k].im;
	}
	c = osc_next_cplx(&q->carrier);
	return q->scale * (s.re * c.re - s.im * c.im);
}


/*
 *    receiver
 */

int qam_rx_init(struct qam_rx *q, int srate, float carrier, float baud,
		float alpha, int span, int eq_len, float target_power)
{
	int i;
	float sum;

	memset(q, 0, sizeof(*q));
	q->srate = srate;
	q->baud = baud;
	q->sps = srate / baud;
	osc_set_freq(&q->lo, -carrier, srate);
	q->mf_len = (int)(2 * span * q->sps) | 1;
	/* the matched filter is evaluated at the T/2 instants themselves,
	   from a table of QAM_MF_RES points per sample */
	q->mf = static_cast<float*>(calloc((q->mf_len + 2) * QAM_MF_RES + 2, sizeof(float)));
	/* the last mf_len + 1 samples, twice */
	q->mf_buf = static_cast<cplx*>(calloc(2 * (q->mf_len + 1), sizeof(cplx)));
	if (!q->mf || !q->mf_buf) {
		qam_rx_free(q);
		return -1;
	}
	for (i = 0, sum = 0; i < q->mf_len; i++) {
		float h = rrc_pulse((i - q->mf_len / 2) / q->sps, alpha);
		sum += h * h;
	}
	for (i = 0; i < (q->mf_len + 2) * QAM_MF_RES + 2; i++)
		q->mf[i] = rrc_pulse(((float)i / QAM_MF_RES - 1 -
				      q->mf_len / 2) / q->sps, alpha) / sqrtf(sum);
	q->power_alpha = 1.0f / (0.02f * srate);
	q->agc_alpha = 1.0f / (0.03f * 2 * baud);
	/* the mean power of the matched filter output is lower than at the
	   symbol instants by the roll off */
	q->agc_target = target_power * (1 - alpha / 4);
	q->kt_p = 0.03f * q->sps;
	q->kt_i = 0.001f * q->sps;
	q->kt_gi = 0.03f;
	q->eq_len = eq_len > QAM_MAX_EQ ? QAM_MAX_EQ : eq_len;
	q->t_next = q->sps / 2;
	qam_rx_reset(q);
	return 0;
}

void qam_rx_free(struct qam_rx *q)
{
	free(q->mf);
	free(q->mf_buf);
	q->mf = NULL;
	q->mf_buf = NULL;
}

void qam_rx_reset(struct qam_rx *q)
{
	memset(q->eq, 0, sizeof(q->eq));
	q->eq[q->eq_len / 2 & ~1].re = 1;
	q->theta = q->dtheta = 0;
	q->t_adj = 0;
}

int qam_rx_sample(struct qam_rx *q, float x)
{
	cplx b;
	int i, ret = 0;

	q->power += q->power_alpha * (x * x - q->power);

	b = cplx_scale(osc_next_cplx(&q->lo), x);
	/* newest sample at mf_buf[mf_pos], kept twice for a contiguous run */
	if (--q->mf_pos < 0)
		q->mf_pos = q->mf_len;
	q->mf_buf[q->mf_pos] = q->mf_buf[q->mf_pos + q->mf_len + 1] = b;

	q->t_next -= 1;
	while (q->t_next <= 0) {
		/* the matched filter output at t_next (<= 0) samples before
		   the previous sample (the latency the handshake timings
		   were tuned with) */
		const cplx *buf = q->mf_buf + q->mf_pos + 1;
		float p = (1 + q->t_next) * QAM_MF_RES, f;
		int k;
		cplx s = { 0, 0 };
		if (p < 0)
			p = 0;
		k = (int)p;
		f = p - k;
		for (i = 0; i < q->mf_len; i++, k += QAM_MF_RES) {
			float h = q->mf[k] + f * (q->mf[k + 1] - q->mf[k]);
			s.re += h * buf[i].re;
			s.im += h * buf[i].im;
		}
		if (!q->agc_hold) {
			q->agc_pwr += q->agc_alpha * (cplx_pow(s) - q->agc_pwr);
			q->agc_gain = q->agc_pwr > 1e-3f ?
				sqrtf(q->agc_target / q->agc_pwr) : 0;
		}
		s = cplx_scale(s, q->agc_gain);

		memmove(q->eq_buf + 1, q->eq_buf, (q->eq_len - 1) * sizeof(cplx));
		q->eq_buf[0] = s;
		q->t_next += q->sps / 2;

		if (q->half) {
			cplx out = { 0, 0 };
			if (!q->timing_hold && !q->timing_freeze) {
				/* Gardner timing error detector */
				float e = (q->mid.re * (q->prev_sym.re - s.re) +
					   q->mid.im * (q->prev_sym.im - s.im)) /
					(q->agc_target + 1e-3f);
				if (e > 1)
					e = 1;
				else if (e < -1)
					e = -1;
				q->t_adj += q->kt_gi * q->kt_i * e;
				if (q->t_adj > q->kt_i)
					q->t_adj = q->kt_i;
				else if (q->t_adj < -q->kt_i)
					q->t_adj = -q->kt_i;
				q->t_next += q->kt_p * e;
			}
			if (!q->timing_freeze)
				q->t_next += q->t_adj;
			q->prev_sym = s;
			q->y = s;
			for (i = 0; i < q->eq_len; i++)
				out = cplx_add(out, cplx_mul(q->eq[i], q->eq_buf[i]));
			q->eq_out = out;
			q->rot = cplx_make(cosf(q->theta), -sinf(q->theta));
			q->z = cplx_mul(out, q->rot);
			q->theta += q->dtheta;
			q->theta = remainderf(q->theta, 2 * (float)M_PI);
			q->symbols++;
			ret = 1;
		} else
			q->mid = s;
		q->half ^= 1;
	}
	return ret;
}

void qam_rx_adapt(struct qam_rx *q, cplx d, float mu, float k1, float k2)
{
	float p = cplx_pow(d), perr;
	int i;

	if (p > 0 && (k1 > 0 || k2 > 0)) {
		perr = (q->z.im * d.re - q->z.re * d.im) / p;
		if (perr > 0.5f)
			perr = 0.5f;
		else if (perr < -0.5f)
			perr = -0.5f;
		q->theta += k1 * perr;
		q->dtheta += k2 * perr;
		if (q->dtheta > 0.05f)
			q->dtheta = 0.05f;
		else if (q->dtheta < -0.05f)
			q->dtheta = -0.05f;
	}
	if (mu > 0) {
		float norm = 1e-3f;
		cplx e = cplx_mulc(cplx_sub(d, q->z), q->rot);
		for (i = 0; i < q->eq_len; i++)
			norm += cplx_pow(q->eq_buf[i]);
		mu /= norm;
		for (i = 0; i < q->eq_len; i++) {
			cplx g = cplx_mulc(e, q->eq_buf[i]);
			q->eq[i].re += mu * g.re;
			q->eq[i].im += mu * g.im;
		}
	}
}

void qam_rx_align_phase(struct qam_rx *q, cplx d)
{
	float a = atan2f(q->z.im, q->z.re) - atan2f(d.im, d.re);
	q->theta = remainderf(q->theta + a, 2 * (float)M_PI);
}


/*
 *    trellis code: differential encoding of Y1Y2 (addition mod 4) and
 *    the 8 state convolutional encoder producing Y0
 */

static const unsigned char conv_next[8][4] = {
	{ 0, 2, 3, 1 }, { 4, 7, 5, 6 }, { 1, 3, 2, 0 }, { 7, 4, 6, 5 },
	{ 2, 0, 1, 3 }, { 6, 5, 7, 4 }, { 3, 1, 0, 2 }, { 5, 6, 4, 7 },
};

int trellis_encode(struct trellis_enc *t, int q, int bits)
{
	t->diff = (t->diff + (q & 3)) & 3;
	t->conv = conv_next[t->conv][t->diff];
	return ((q << 1) & 0x78 & ((1 << (bits + 1)) - 1)) |
		(t->diff << 1) | ((t->conv >> 2) & 1);
}

void viterbi_init(struct viterbi *v, const cplx *points, int nbits)
{
	memset(v, 0, sizeof(*v));
	v->points = points;
	v->nbits = nbits;
	v->npoints = 1 << (nbits + 1);
}

int viterbi_decode(struct viterbi *v, cplx z, cplx *decided)
{
	float best_d[8], m[8];
	int best_i[8], i, s, k, pos, ret = -1;
	float bm;

	/* nearest point of each of the 8 subsets (index bits 0-2) */
	for (k = 0; k < 8; k++)
		best_d[k] = 1e30f;
	for (i = 0; i < v->npoints; i++) {
		float d = cplx_pow(cplx_sub(z, v->points[i]));
		k = i & 7;
		if (d < best_d[k]) {
			best_d[k] = d;
			best_i[k] = i;
		}
	}

	pos = v->pos;
	for (s = 0; s < 8; s++)
		m[s] = 1e30f;
	for (s = 0; s < 8; s++) {
		for (k = 0; k < 4; k++) {
			int ns = conv_next[s][k];
			int sub = (k << 1) | ((ns >> 2) & 1);
			float metric = v->metric[s] + best_d[sub];
			if (metric < m[ns]) {
				m[ns] = metric;
				v->prev[pos][ns] = s;
				v->path[pos][ns] = best_i[sub];
			}
		}
	}
	bm = m[0];
	for (s = 1; s < 8; s++)
		if (m[s] < bm)
			bm = m[s];
	for (s = 0; s < 8; s++)
		v->metric[s] = m[s] - bm;

	if (++v->filled >= VITERBI_DEPTH) {
		int idx, y12, q;
		/* trace back from the best state */
		for (s = 0, k = 0; k < 8; k++)
			if (v->metric[k] == 0)
				s = k;
		for (i = 0, k = pos; i < VITERBI_DEPTH - 1; i++) {
			s = v->prev[k][s];
			k = k ? k - 1 : VITERBI_DEPTH - 1;
		}
		idx = v->path[k][s];
		if (decided)
			*decided = v->points[idx];
		y12 = (idx >> 1) & 3;
		q = (y12 - v->last_y12) & 3;
		v->last_y12 = y12;
		ret = q | ((idx >> 3) << 2);
		v->filled = VITERBI_DEPTH;
	}
	v->pos = (pos + 1) % VITERBI_DEPTH;
	return ret;
}


/*
 *    echo canceller
 */

int echo_init(struct echo_canceller *e, int len)
{
	memset(e, 0, sizeof(*e));
	e->len = len;
	e->w = static_cast<float*>(calloc(len, sizeof(float)));
	e->hist = static_cast<float*>(calloc(2 * len, sizeof(float)));
	if (!e->w || !e->hist) {
		echo_free(e);
		return -1;
	}
	return 0;
}

void echo_free(struct echo_canceller *e)
{
	free(e->w);
	free(e->hist);
	e->w = e->hist = NULL;
}

float echo_run(struct echo_canceller *e, float tx, float rx, float mu)
{
	float est = 0, err, *h;
	int i;

	if (mu > 0 && !e->training) {
		e->training = 1;
		e->off = 0;
		e->pw_rx = e->pw_err = 0;
	}
	if (e->off)
		return rx;
	/* history kept twice so that it can be read linearly */
	if (--e->pos < 0)
		e->pos = e->len - 1;
	e->hist[e->pos] = e->hist[e->pos + e->len] = tx;
	e->tx_power += 0.001f * (tx * tx - e->tx_power);
	h = e->hist + e->pos;
	for (i = 0; i < e->len; i++)
		est += e->w[i] * h[i];
	err = rx - est;
	if (mu > 0) {
		e->pw_rx += 0.001f * (rx * rx - e->pw_rx);
		e->pw_err += 0.001f * (err * err - e->pw_err);
	}
	if (mu > 0 && e->tx_power > 1) {
		float g = mu * err / (e->len * e->tx_power);
		for (i = 0; i < e->len; i++)
			e->w[i] += g * h[i];
	}
	return err;
}

float echo_judge(struct echo_canceller *e)
{
	float erle = 10 * log10f((e->pw_rx + 1) / (e->pw_err + 1));
	e->training = 0;
	if (erle < 6) {
		e->off = 1;
		memset(e->w, 0, e->len * sizeof(*e->w));
	}
	return erle;
}

void qam_rx_recompute(struct qam_rx *q)
{
	cplx out = { 0, 0 };
	int i;
	for (i = 0; i < q->eq_len; i++)
		out = cplx_add(out, cplx_mul(q->eq[i], q->eq_buf[i]));
	q->eq_out = out;
	q->z = cplx_mul(out, q->rot);
}
