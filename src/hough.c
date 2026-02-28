#include <meteor/hough.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Fixed-point sin/cos lookup table, initialised once at startup.
 * Values are scaled by 1024 so that integer arithmetic can be used
 * in the vote inner loop instead of floating-point calls.
 */
static int16_t s_cos_tab[HOUGH_THETA_STEPS];
static int16_t s_sin_tab[HOUGH_THETA_STEPS];
static int     s_trig_ready;

static void trig_init(void)
{
	int t;

	if (s_trig_ready)
		return;

	for (t = 0; t < HOUGH_THETA_STEPS; t++) {
		double theta = (double)t * M_PI / (double)HOUGH_THETA_STEPS;

		s_cos_tab[t] = (int16_t)(cos(theta) * 1024.0);
		s_sin_tab[t] = (int16_t)(sin(theta) * 1024.0);
	}
	s_trig_ready = 1;
}

HoughAccum *hough_create(void)
{
	HoughAccum *h;

	trig_init();

	h = malloc(sizeof(*h));
	if (!h)
		return NULL;

	(void)memset(h->accum, 0, sizeof(h->accum));
	return h;
}

void hough_destroy(HoughAccum *h)
{
	free(h);
}

void hough_reset(HoughAccum *h)
{
	(void)memset(h->accum, 0, sizeof(h->accum));
}

void hough_vote(HoughAccum *h, int x, int y)
{
	int t;

	for (t = 0; t < HOUGH_THETA_STEPS; t++) {
		int32_t rho_f = (int32_t)x * (int32_t)s_cos_tab[t]
			      + (int32_t)y * (int32_t)s_sin_tab[t];
		int rho = (int)(rho_f / 1024);
		int idx = rho + HOUGH_RHO_MAX;

		if (idx >= 0 && idx < 2 * HOUGH_RHO_MAX) {
			if (h->accum[idx][t] < UINT16_MAX)
				h->accum[idx][t]++;
		}
	}
}

int hough_find_peaks(const HoughAccum *h, int threshold,
		     MeteorLine *out, int max_lines)
{
	int found = 0;
	int r, t;

	for (r = 1; r < 2 * HOUGH_RHO_MAX - 1 && found < max_lines; r++) {
		for (t = 1; t < HOUGH_THETA_STEPS - 1 && found < max_lines; t++) {
			uint16_t v = h->accum[r][t];

			if ((int)v < threshold)
				continue;

			/* 3×3 local maximum suppression */
			if (v < h->accum[r - 1][t - 1] ||
			    v < h->accum[r - 1][t    ] ||
			    v < h->accum[r - 1][t + 1] ||
			    v < h->accum[r    ][t - 1] ||
			    v < h->accum[r    ][t + 1] ||
			    v < h->accum[r + 1][t - 1] ||
			    v < h->accum[r + 1][t    ] ||
			    v < h->accum[r + 1][t + 1])
				continue;

			out[found].rho       = r - HOUGH_RHO_MAX;
			out[found].theta     = t;
			out[found].votes     = (int)v;
			out[found].length_px = (int)v; /* proxy; refined in detector */
			found++;
		}
	}
	return found;
}
